/* SPDX-License-Identifier: GPL-3.0-or-later */

// X11/xcb SurfaceBackend implementation for lsfg-vk-app (task 7a).
//
// The app is a headless receiver, so its own display is the X server running
// under Xwayland. We connect to $DISPLAY with xcb, enumerate RandR outputs,
// create a borderless, no-focus window on the requested output and build a
// VkSurfaceKHR for it through the loader-provided vkCreateXcbSurfaceKHR. The
// surface-format query is likewise fetched from the loader because
// VulkanInstanceFuncs only carries the capabilities query.
//
// This xcb build is intentionally minimal: the setup/screen iterator helpers
// (xcb_next_screen / xcb_roots_iterator) and xcb_get_fd are absent, so the
// root screen is reached through xcb_setup_roots_iterator and the event pump
// uses xcb_poll_for_event on a deadline instead of poll() on the connection
// file descriptor.

// glibc exposes dlopen/dlsym behind a POSIX feature-test macro; define it
// before any include so <dlfcn.h> declares the loader entry points.
#define _POSIX_C_SOURCE 200809L

#include "lsfg-vk-app/wsi/surface_backend.hpp"

#include "lsfg-vk-common/helpers/errors.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>

namespace ls::wsi {
namespace {

    /// RandR exposes no XCB_RANDR_CRTC_INVALID constant in this build; it is
    /// simply "any non-zero XID is a live crtc".
    constexpr xcb_randr_crtc_t kInvalidCrtc = 0xFFFFFFFFu;

    /// one RandR output plus the CRTC it is attached to. The CRTC id and the
    /// connected flag let selectWindow pick the primary output again.
    struct OutputEntry {
        OutputGeometry geom;
        bool connected{false};
        xcb_randr_crtc_t crtc{kInvalidCrtc};
    };

    /// xcb_randr_get_output_info_name returns a pointer to the raw (null
    /// terminated) name bytes; decode them into std::string.
    std::string outputName(xcb_randr_get_output_info_reply_t* info) {
        const std::size_t len = xcb_randr_get_output_info_name_length(info);
        const uint8_t* bytes = xcb_randr_get_output_info_name(info);
        return std::string(reinterpret_cast<const char*>(bytes), len);
    }

    /// refresh rate (mHz) of a RandR mode, or 0 when its timings are absent.
    /// dot_clock is in kHz; refresh = pixel_clock / (htotal * vtotal).
    uint32_t modeRefreshMhz(const xcb_randr_mode_info_t* mode) {
        if (mode == nullptr || mode->htotal == 0 || mode->vtotal == 0)
            return 0;
        const uint64_t period =
            static_cast<uint64_t>(mode->htotal) * static_cast<uint64_t>(mode->vtotal);
        return static_cast<uint32_t>(
            (static_cast<uint64_t>(mode->dot_clock) * 1000000ULL) / period);
    }

    /// find the mode of a screen-resources reply matching a crtc's current mode.
    xcb_randr_mode_info_t* findMode(
            xcb_randr_get_screen_resources_current_reply_t* resources,
            xcb_randr_mode_t id) {
        xcb_randr_mode_info_t* modes =
            xcb_randr_get_screen_resources_current_modes(resources);
        const int modeCount =
            xcb_randr_get_screen_resources_current_modes_length(resources);
        for (int32_t i = 0; i < modeCount; ++i)
            if (modes[i].id == id)
                return &modes[i];
        return nullptr;
    }

    /// the loader's vkGetInstanceProcAddr. The app never links libvulkan; it
    /// dlopens it to control instance creation, so the same dlsym dance is used
    /// to reach the surface-creation/format functions that are not carried by
    /// VulkanInstanceFuncs. cached across calls.
    PFN_vkGetInstanceProcAddr loaderGetProc() {
        static PFN_vkGetInstanceProcAddr mpa{nullptr};
        if (mpa == nullptr) {
            void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
            if (h == nullptr)
                h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
            if (h == nullptr)
                throw ls::error("dlopen(libvulkan) failed");
            mpa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                dlsym(h, "vkGetInstanceProcAddr"));
            if (mpa == nullptr)
                throw ls::error("dlsym(vkGetInstanceProcAddr) failed");
        }
        return mpa;
    }
} // namespace

class X11SurfaceBackend : public SurfaceBackend {
public:
    ~X11SurfaceBackend() { destroy(); }

    bool connect(std::string_view session) override {
        // The app is X11-only today; a wayland-only request is the later
        // backend. Anything that is not an explicit "wayland" is x11 here.
        if (session == "wayland")
            return false;

        mConn = xcb_connect(nullptr, nullptr);
        if (xcb_connection_has_error(mConn) != 0)
            throw ls::error("cannot connect to X display ($DISPLAY)");

        const xcb_setup_t* setup = xcb_get_setup(mConn);
        if (setup == nullptr)
            throw ls::error("no X setup after connect");

        // minimal xcb: reach the first screen through the setup roots iterator.
        xcb_screen_iterator_t screenIt = xcb_setup_roots_iterator(setup);
        mScreen = screenIt.data;
        if (mScreen == nullptr)
            throw ls::error("X server has no screen");

        // RandR must be present for output enumeration.
        xcb_query_extension_cookie_t q = xcb_query_extension(mConn, 5, "RANDR");
        xcb_query_extension_reply_t* qReply = xcb_query_extension_reply(mConn, q, nullptr);
        if (qReply == nullptr || qReply->present == 0)
            throw ls::error("X server does not support the RandR extension");

        mOutputs = enumerateOutputs();

        // The app window inherits the root visual, whose channel masks define
        // the X pixel byte layout. On an LSBFirst screen the channel whose
        // mask holds the low byte sits at the lowest memory address and is
        // the first-listed component of the matching Vulkan 8888 format: the
        // standard X TrueColor visual (red mask 0xff0000) stores B,G,R,X and
        // maps to B8G8R8A8. Presenting a swapchain in the non-native 8888
        // format on such a surface shows red and blue swapped.
        mNativeSwapFormat = VK_FORMAT_B8G8R8A8_UNORM;
        const xcb_visualid_t visId = mScreen->root_visual;
        bool visualFound = false;
        for (xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(mScreen);
                di.rem > 0 && !visualFound; xcb_depth_next(&di)) {
            xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);
            while (vi.rem > 0 && !visualFound) {
                const xcb_visualtype_t* vt = vi.data;
                xcb_visualtype_next(&vi);
                if (vt->visual_id != visId)
                    continue;
                visualFound = true;
                const bool lowByteBlue = (vt->blue_mask & 0x000000ffu) != 0;
                const bool lowByteRed = (vt->red_mask & 0x000000ffu) != 0;
                if (lowByteBlue == lowByteRed)
                    continue;
                const bool lsbFirst =
                    setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;
                const bool blueFirstMemory = lsbFirst ? lowByteBlue : lowByteRed;
                mNativeSwapFormat = blueFirstMemory ? VK_FORMAT_B8G8R8A8_UNORM
                                                    : VK_FORMAT_R8G8B8A8_UNORM;
            }
        }
        return true;
    }

    std::vector<OutputGeometry> outputs() const override {
        std::vector<OutputGeometry> result;
        result.reserve(mOutputs.size());
        for (const auto& o : mOutputs)
            result.push_back(o.geom);
        return result;
    }

    WindowHandle createWindow(std::string_view output_name, VkExtent2D extent,
            uint32_t colorspace) override {
        (void)colorspace; // colorSpace is chosen by Vulkan at surface creation

        const OutputEntry* target = selectWindow(output_name);

        // the overlay covers the whole chosen output; game frames are blit-
        // scaled into the (larger) swapchain. the requested extent is the
        // fallback for outputs whose geometry is unknown (0x0).
        uint32_t winW = extent.width, winH = extent.height;
        if (target->geom.width > 0 && target->geom.height > 0) {
            winW = target->geom.width;
            winH = target->geom.height;
        }

        const uint32_t backPixel = mScreen->black_pixel;
        mWindow = xcb_generate_id(mConn);
        xcb_create_window(mConn,
            XCB_COPY_FROM_PARENT,      // depth
            mWindow,                     // new window id
            mScreen->root,               // parent = root, covering the output
            static_cast<int16_t>(target->geom.x),
            static_cast<int16_t>(target->geom.y),
            static_cast<uint16_t>(winW),
            static_cast<uint16_t>(winH),
            0,                           // border width
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            mScreen->root_visual,        // inherit the root visual
            XCB_CW_BACK_PIXEL,           // value mask
            &backPixel);                 // value list

        if (xcb_connection_has_error(mConn) != 0)
            throw ls::error("xcb_create_window failed for output '" +
                    std::string(output_name) + "'");

        // no event mask is selected on this window (xcb_create_window has
        // none), so no ConfigureNotify ever arrives: seed the last-observed
        // size with the created size so windowExtent() is valid from the
        // first caller, and a later attribute change is still detectable.
        mLastWidth = static_cast<uint16_t>(winW);
        mLastHeight = static_cast<uint16_t>(winH);

        applyWindowChrome(mWindow);

        xcb_map_window(mConn, mWindow);
        // the state request must follow the map: the WM only starts managing
        // the window after seeing the map, and EWMH mandates a post-map
        // ClientMessage. a pre-map property set is ignored by KWin, which
        // then placed the window at (0,28) 2560x1382 instead of (0,0) 2560x1440.
        requestFullscreenState(mWindow);
        xcb_flush(mConn);

        if (xcb_connection_has_error(mConn) != 0)
            throw ls::error("xcb_map_window/flush failed");

        return reinterpret_cast<WindowHandle>(static_cast<uintptr_t>(mWindow));
    }

    VkSurfaceKHR createSurface(const vk::Vulkan& vk, WindowHandle handle) override {
        auto createXcbSurface = reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(
            loaderGetProc()(vk.inst(), "vkCreateXcbSurfaceKHR"));
        if (createXcbSurface == nullptr)
            throw ls::error("vkGetInstanceProcAddr(vkCreateXcbSurfaceKHR) returned null");

        VkXcbSurfaceCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        ci.connection = mConn;
        ci.window = static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(handle));

        VkSurfaceKHR surface{VK_NULL_HANDLE};
        VkResult result = createXcbSurface(vk.inst(), &ci, nullptr, &surface);
        if (result != VK_SUCCESS)
            throw ls::error("vkCreateXcbSurfaceKHR failed",
                    ls::vulkan_error(result, "vkCreateXcbSurfaceKHR"));

        // cache the instance + device + instance funcs for the later queries;
        // surfaceCaps() has no vk parameter, so it must reuse what was captured.
        mFuncs = vk.fi();
        mPhysDev = vk.physdev();
        mVkInstance = vk.inst();
        mSurface = surface;
        return surface;
    }

    void surfaceCaps(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR& caps,
            std::vector<VkColorSpaceKHR>& colorspaces) override {
        // the instance enables VK_KHR_surface (createInstance in vulkan.cpp),
        // so the surface queries must resolve; a null pointer here means the
        // instance was built without the surface extensions, in which case the
        // swapchain would be created on zeroed caps, so fail loudly.
        const PFN_vkGetInstanceProcAddr mpa = loaderGetProc();
        auto getCaps = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            mpa(mVkInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        if (getCaps == nullptr)
            throw ls::error("vkGetPhysicalDeviceSurfaceCapabilitiesKHR unavailable "
                "(instance missing VK_KHR_surface)");
        const VkResult res = getCaps(mPhysDev, surface, &caps);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

        // GetPhysicalDeviceSurfaceFormatsKHR is NOT in VulkanInstanceFuncs, so
        // fetch it from the loader like the surface-creation function. Reuse the
        // loader proc addr already fetched above (no second declaration).
        auto getFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
            mpa(mVkInstance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
        if (getFormats == nullptr)
            throw ls::error("vkGetPhysicalDeviceSurfaceFormatsKHR unavailable "
                "(instance missing VK_KHR_surface)");

        // the color-space list is optional
        uint32_t count{};
        if (getFormats(mPhysDev, surface, &count, nullptr) != VK_SUCCESS || count == 0)
            return;
        std::vector<VkSurfaceFormatKHR> formats(count);
        if (getFormats(mPhysDev, surface, &count, formats.data()) != VK_SUCCESS)
            return;
        colorspaces.reserve(colorspaces.size() + count);
        for (const auto& f : formats)
            colorspaces.push_back(f.colorSpace);
    }

    VkFormat swapchainFormat(VkSurfaceKHR surface) override {
        // Prefer the driver's own answer when it reports a usable 8-bit UNORM
        // RGBA format. Under XWayland the surface format query is degenerate
        // (zero formats), in which case the visual-derived native format is
        // the only correct choice.
        const PFN_vkGetInstanceProcAddr mpa = loaderGetProc();
        auto getFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
            mpa(mVkInstance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
        uint32_t count{};
        if (getFormats != nullptr && mPhysDev != VK_NULL_HANDLE
                && getFormats(mPhysDev, surface, &count, nullptr) == VK_SUCCESS
                && count != 0) {
            std::vector<VkSurfaceFormatKHR> formats(count);
            if (getFormats(mPhysDev, surface, &count, formats.data()) == VK_SUCCESS) {
                for (const auto& f : formats)
                    if (f.format == VK_FORMAT_R8G8B8A8_UNORM
                            || f.format == VK_FORMAT_B8G8R8A8_UNORM)
                        return f.format;
            }
        }
        return mNativeSwapFormat;
    }

    VkExtent2D windowExtent() const override {
        // the last ConfigureNotify size; 0x0 before the first one (the window
        // was created at the requested extent, so the caller falls back to it)
        return VkExtent2D{ mLastWidth, mLastHeight };
    }

    bool processEvents(int timeout_ms) override {
        if (mConn == nullptr || mWindow == 0)
            return false;

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        for (;;) {
            xcb_generic_event_t* ev = xcb_poll_for_event(mConn);
            if (ev != nullptr) {
                const bool consumed = handleEvent(ev);
                free(ev);
                if (consumed)
                    return true;
                continue; // keep draining events within the timeout window
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void destroy() override {
        if (mWindow != 0 && mConn != nullptr) {
            xcb_destroy_window(mConn, mWindow);
            mWindow = 0;
        }
        if (mSurface != VK_NULL_HANDLE && mVkInstance != VK_NULL_HANDLE) {
            const PFN_vkGetInstanceProcAddr mpa = loaderGetProc();
            auto destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
                mpa(mVkInstance, "vkDestroySurfaceKHR"));
            if (destroySurface != nullptr)
                destroySurface(mVkInstance, mSurface, nullptr);
            mSurface = VK_NULL_HANDLE;
        }
        if (mConn != nullptr) {
            xcb_disconnect(mConn);
            mConn = nullptr;
        }
    }

private:
    /// enumerate the RandR outputs attached to the root window, keeping the
    /// list stable across calls (stored in the mOutputs member).
    std::vector<OutputEntry> enumerateOutputs() {
        std::vector<OutputEntry> entries;

        xcb_randr_get_screen_resources_current_cookie_t currentCookie =
            xcb_randr_get_screen_resources_current(mConn, mScreen->root);
        xcb_randr_get_screen_resources_current_reply_t* resources =
            xcb_randr_get_screen_resources_current_reply(mConn, currentCookie, nullptr);
        if (resources == nullptr)
            return entries;

        xcb_randr_output_t* outputs =
            xcb_randr_get_screen_resources_current_outputs(resources);
        const int outputCount =
            xcb_randr_get_screen_resources_current_outputs_length(resources);
        for (int32_t i = 0; i < outputCount; ++i) {
            const xcb_randr_output_t output = outputs[i];
            xcb_randr_get_output_info_cookie_t oc =
                xcb_randr_get_output_info(mConn, output, 0);
            xcb_randr_get_output_info_reply_t* info =
                xcb_randr_get_output_info_reply(mConn, oc, nullptr);
            if (info == nullptr)
                continue;

            OutputEntry e{};
            e.geom.name = outputName(info);
            e.connected = info->status == 0; // status 0 == connected
            e.crtc = info->crtc;

            if (info->crtc != kInvalidCrtc) {
                xcb_randr_get_crtc_info_cookie_t cc =
                    xcb_randr_get_crtc_info(mConn, info->crtc, info->timestamp);
                xcb_randr_get_crtc_info_reply_t* crtcInfo =
                    xcb_randr_get_crtc_info_reply(mConn, cc, nullptr);
                if (crtcInfo != nullptr) {
                    e.geom.x = static_cast<uint32_t>(crtcInfo->x);
                    e.geom.y = static_cast<uint32_t>(crtcInfo->y);
                    e.geom.width = static_cast<uint32_t>(crtcInfo->width);
                    e.geom.height = static_cast<uint32_t>(crtcInfo->height);
                    e.geom.refresh = modeRefreshMhz(findMode(resources, crtcInfo->mode));
                }
            }

            entries.push_back(std::move(e));
        }

        return entries;
    }

    /// resolve the target output for createWindow: an explicit name must match
    /// a connector exactly (error otherwise); an empty name picks the primary
    /// output — here, the first connected output with a live CRTC.
    const OutputEntry* selectWindow(std::string_view name) {
        const std::string want = std::string(name);
        if (want.empty()) {
            for (const auto& e : mOutputs)
                if (e.connected && e.crtc != kInvalidCrtc)
                    return &e;
            for (const auto& e : mOutputs)
                if (e.crtc != kInvalidCrtc)
                    return &e;
            if (!mOutputs.empty())
                return &mOutputs.front();
            throw ls::error("no X outputs available");
        }

        for (const auto& e : mOutputs)
            if (e.geom.name == want)
                return &e;

        std::string available;
        for (std::size_t i = 0; i < mOutputs.size(); ++i) {
            if (i) available += ", ";
            available += mOutputs[i].geom.name;
        }
        throw ls::error("output '" + want + "' not found; available: " + available);
    }

    /// never-focus + utility type, per the presentation spec. The FULLSCREEN +
    /// ABOVE state is NOT set here: _NET_WM_STATE is owned by the window
    /// manager and clients must request it with a post-map ClientMessage
    /// (requestFullscreenState). KWin ignores a client-set property and
    /// places the window below its top panel otherwise (observed: created at
    /// (0,0) 2560x1440, placed at (0,28) 2560x1382).
    void applyWindowChrome(xcb_window_t window) {
        const xcb_atom_t netWmWindowType = internAtom("_NET_WM_WINDOW_TYPE");
        const xcb_atom_t typeUtility = internAtom("_NET_WM_WINDOW_TYPE_UTILITY");
        mDeleteWindowAtom = internAtom("_NET_WM_DELETE_WINDOW");

        // WM_HINTS with input=False so the window NEVER grabs keyboard focus
        // (spec: the window must never take focus). InputHint flag = 0x2, input=0.
        uint32_t hints[2] = {2u /*InputHint*/, 0u /*input=False*/};
        const xcb_atom_t hintsType = internAtom("HINTS");
        xcb_change_property(mConn, XCB_PROP_MODE_REPLACE, window,
            XCB_ATOM_WM_HINTS, hintsType, 32, 2, hints);

        // _NET_WM_WINDOW_TYPE = UTILITY (damps activation + focus).
        xcb_change_property(mConn, XCB_PROP_MODE_REPLACE, window,
            netWmWindowType, netWmWindowType, 32, 1, &typeUtility);

        if (xcb_connection_has_error(mConn) != 0)
            throw ls::error("failed to set EWMH properties on window");
    }

    /// EWMH: a client may only change _NET_WM_STATE by sending a ClientMessage
    /// to the root window; the 32-bit data fields carry action (2 = add state),
    /// the state atoms, and the source indicator (0 = client). KWin ignores a
    /// client-set property, so this must be a post-map message.
    void requestFullscreenState(xcb_window_t window) {
        const xcb_atom_t netWmState = internAtom("_NET_WM_STATE");
        const xcb_atom_t stateFullscreen = internAtom("_NET_WM_STATE_FULLSCREEN");
        const xcb_atom_t stateAbove = internAtom("_NET_WM_STATE_ABOVE");
        const xcb_screen_t* root = mScreen;

        xcb_client_message_event_t cm;
        std::memset(&cm, 0, sizeof(cm));
        cm.response_type = XCB_CLIENT_MESSAGE;
        cm.format = 32;
        cm.window = window;
        cm.type = netWmState;
        cm.data.data32[0] = 2;
        cm.data.data32[1] = static_cast<uint32_t>(stateFullscreen);
        cm.data.data32[2] = static_cast<uint32_t>(stateAbove);
        cm.data.data32[3] = 0;

        xcb_send_event(mConn, 0, root->root,
            XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
            XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
            reinterpret_cast<const char*>(&cm));
    }

    xcb_atom_t internAtom(const char* name) {
        xcb_intern_atom_cookie_t cookie =
            xcb_intern_atom(mConn, 0, static_cast<uint16_t>(std::strlen(name)), name);
        xcb_intern_atom_reply_t* reply =
            xcb_intern_atom_reply(mConn, cookie, nullptr);
        if (reply == nullptr)
            return XCB_ATOM_NONE;
        const xcb_atom_t atom = reply->atom;
        free(reply);
        return atom;
    }

    /// return true when the event asks task 8 to resize or the stream to stop.
    bool handleEvent(xcb_generic_event_t* ev) {
        const uint8_t code = ev->response_type & ~0x80;

        if (code == XCB_CONFIGURE_NOTIFY) {
            auto* e = reinterpret_cast<xcb_configure_notify_event_t*>(ev);
            // ConfigureNotify also arrives on the initial map; only report a
            // resize when the size actually changed (mLastWidth/Height are the
            // last observed size and start at 0).
            if (e->width == mLastWidth && e->height == mLastHeight)
                return false;
            mLastWidth = e->width;
            mLastHeight = e->height;
            return true; // resize to the new extent

        } else if (code == XCB_DESTROY_NOTIFY) {
            return true; // WM destroyed the window

        } else if (code == XCB_CLIENT_MESSAGE) {
            auto* e = reinterpret_cast<xcb_client_message_event_t*>(ev);
            if (mDeleteWindowAtom != XCB_ATOM_NONE && e->type == mDeleteWindowAtom)
                return true; // WM_DELETE_WINDOW requested
        }
        return false;
    }

    xcb_connection_t* mConn{nullptr};
    xcb_screen_t* mScreen{nullptr};
    std::vector<OutputEntry> mOutputs;
    xcb_window_t mWindow{0};
    xcb_atom_t mDeleteWindowAtom{XCB_ATOM_NONE};
    VkFormat mNativeSwapFormat{VK_FORMAT_B8G8R8A8_UNORM};

    // captured from vk in createSurface() for the queries surfaceCaps()/destroy()
    // run without a vk parameter.
    vk::VulkanInstanceFuncs mFuncs{};
    VkPhysicalDevice mPhysDev{VK_NULL_HANDLE};
    VkInstance mVkInstance{VK_NULL_HANDLE};
    VkSurfaceKHR mSurface{VK_NULL_HANDLE};
    uint16_t mLastWidth{0};
    uint16_t mLastHeight{0};
};

std::unique_ptr<SurfaceBackend> createX11SurfaceBackend() {
    return std::make_unique<X11SurfaceBackend>();
}

} // namespace ls::wsi
