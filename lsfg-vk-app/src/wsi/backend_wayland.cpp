/* SPDX-License-Identifier: GPL-3.0-or-later */

// Wayland SurfaceBackend implementation for lsfg-vk-app (task 10).
//
// Implements the SurfaceBackend interface using libwayland-client with
// xdg-shell (stable) and xdg-output (unstable v4) protocols. Creates a
// borderless, no-focus toplevel window on the requested output and builds
// a VkSurfaceKHR for it through vkCreateWaylandSurfaceKHR.
//
// Protocol versions pinned:
// - xdg_wm_base: stable (version 1)
// - zxdg_output_manager_v1: unstable v4 (interface version 4)

#define _POSIX_C_SOURCE 200809L

#include "lsfg-vk-app/wsi/surface_backend.hpp"

#include "lsfg-vk-common/helpers/errors.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <poll.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

// Generated protocol headers (from wayland-scanner in CMake)
#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

namespace ls::wsi {
namespace {

/// TEMP DEBUG: elapsed-ms probe (app start) for stall localization. gated
/// on LSFGVK_APP_DBG so the default stream stays clean.
const std::chrono::steady_clock::time_point g_dbgT0 = std::chrono::steady_clock::now();
bool dbgEnabled() {
    return std::getenv("LSFGVK_APP_DBG") != nullptr;
}
void dbg(const char* fmt, ...) {
    if (!dbgEnabled())
        return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_dbgT0).count();
    std::fprintf(stderr, "lsfg-vk-app: [dbg] wl: %s (t+%lld ms)\n", buf, ms);
}

/// TEMP DEBUG: wl_surface enter/leave an OUTPUT's scanout region (leave means
/// the compositor stopped displaying this surface anywhere)
void surfaceEnter(void* data, wl_surface* /*s*/, wl_output* output) {
    dbg("wl_surface ENTERED output %p", (void*)output);
    (void)data;
}

void surfaceLeave(void* data, wl_surface* /*s*/, wl_output* output) {
    dbg("wl_surface LEFT output %p (compositor stopped scanning it out)", (void*)output);
    (void)data;
}

const wl_surface_listener surfaceListener = {
    .enter = surfaceEnter,
    .leave = surfaceLeave
};

/// TEMP DEBUG: frame callback listener (one wl_callback at a time; each
/// fires once when the compositor displays the buffer committed after the
/// request - zero callbacks means the compositor stopped showing the surface)
uint32_t g_dbgFrameCount = 0;
bool g_dbgFrameCbPending = false;

void surfaceFrameEvent(void* /*data*/, wl_callback* cb, uint32_t /*time*/) {
    g_dbgFrameCbPending = false;
    ++g_dbgFrameCount;
    dbg("wl_surface frame callback #%u (compositor displayed a buffer)", g_dbgFrameCount);
    wl_callback_destroy(cb);
}

const wl_callback_listener surfaceFrameListener = {
    .done = surfaceFrameEvent
};

/// Wayland registry globals we bind
struct Globals {
    wl_compositor* compositor{nullptr};
    xdg_wm_base* xdgWmBase{nullptr};
    zxdg_output_manager_v1* xdgOutputManager{nullptr};
    wl_seat* seat{nullptr};
};

/// one Wayland output plus its xdg_output wrapper
struct OutputEntry {
    OutputGeometry geom;
    bool connected{false};
    wl_output* wlOutput{nullptr};
    zxdg_output_v1* xdgOutput{nullptr};
    bool xdgOutputDone{false};
};

/// the loader's vkGetInstanceProcAddr
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

/// wl_registry listener: bind globals
void registryGlobal(void* data, wl_registry* registry, uint32_t name,
                    const char* interface, uint32_t version) {
    auto* g = static_cast<Globals*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        g->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        g->xdgWmBase = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    } else if (std::strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        // Bind at the protocol's true interface version (3). The xdg-output
        // unstable v4 protocol declares zxdg_output_manager_v1 and
        // zxdg_output_v1 at version 3; requesting 4 trips compositors (KWin
        // errors with "invalid version ... expected at most 3"). Version 3
        // still exposes the `name`/`description` events (v2+) we rely on.
        g->xdgOutputManager = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 3));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        g->seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, 1));
    }
}

void registryGlobalRemove(void* /*data*/, wl_registry* /*registry*/,
                          uint32_t /*name*/) {}

const wl_registry_listener registryListener = {
    .global = registryGlobal,
    .global_remove = registryGlobalRemove
};

/// xdg_wm_base listener: handle ping
void xdgWmBasePing(void* data, xdg_wm_base* wmBase, uint32_t serial) {
    xdg_wm_base_pong(wmBase, serial);
    (void)data;
}

const xdg_wm_base_listener xdgWmBaseListener = {
    .ping = xdgWmBasePing
};

/// xdg_output listener: collect output name and logical geometry
void xdgOutputName(void* data, zxdg_output_v1* /*output*/, const char* name) {
    auto* entry = static_cast<OutputEntry*>(data);
    entry->geom.name = name;
    // KWin sends every xdg_output property (logical_position/size, name,
    // description) but never the trailing `done` event, so we cannot wait on
    // `done` to know an output is ready - that would busy-spin forever in the
    // enumeration roundtrip loop. `name` is always delivered and is exactly
    // what enumerateOutputs()/selectOutput() need, so treat it as the ready
    // signal (the `done` handler sets it too, as a safety for compositors
    // that do emit done).
    entry->xdgOutputDone = true;
}

void xdgOutputDescription(void* /*data*/, zxdg_output_v1* /*output*/,
                          const char* /*description*/) {}

void xdgOutputLogicalPosition(void* data, zxdg_output_v1* /*output*/,
                              int32_t x, int32_t y) {
    auto* entry = static_cast<OutputEntry*>(data);
    entry->geom.x = static_cast<uint32_t>(x);
    entry->geom.y = static_cast<uint32_t>(y);
}

void xdgOutputLogicalSize(void* data, zxdg_output_v1* /*output*/,
                          int32_t width, int32_t height) {
    auto* entry = static_cast<OutputEntry*>(data);
    entry->geom.width = static_cast<uint32_t>(width);
    entry->geom.height = static_cast<uint32_t>(height);
}

void xdgOutputDone(void* data, zxdg_output_v1* /*output*/) {
    auto* entry = static_cast<OutputEntry*>(data);
    entry->xdgOutputDone = true;
}

void xdgOutputScale(void* /*data*/, zxdg_output_v1* /*output*/,
                    int32_t /*scale*/) {}

const zxdg_output_v1_listener xdgOutputListener = {
    .logical_position = xdgOutputLogicalPosition,
    .logical_size = xdgOutputLogicalSize,
    .done = xdgOutputDone,
    .name = xdgOutputName,
    .description = xdgOutputDescription
};

/// wl_output listener: fallback for compositors without xdg-output
void wlOutputGeometry(void* data, wl_output* /*output*/,
                      int32_t x, int32_t y,
                      int32_t physical_width, int32_t physical_height,
                      int32_t subpixel, const char* make,
                      const char* model, int32_t transform) {
    auto* entry = static_cast<OutputEntry*>(data);
    if (entry->geom.name.empty()) {
        entry->geom.name = std::string(make) + " " + model;
    }
    entry->geom.x = static_cast<uint32_t>(x);
    entry->geom.y = static_cast<uint32_t>(y);
    (void)physical_width; (void)physical_height; (void)subpixel; (void)transform;
}

void wlOutputMode(void* data, wl_output* /*output*/,
                  uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    auto* entry = static_cast<OutputEntry*>(data);
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        entry->geom.width = static_cast<uint32_t>(width);
        entry->geom.height = static_cast<uint32_t>(height);
        entry->geom.refresh = static_cast<uint32_t>(refresh) * 1000; // mHz
    }
}

void wlOutputDone(void* /*data*/, wl_output* /*output*/) {}
void wlOutputScale(void* /*data*/, wl_output* /*output*/, int32_t /*factor*/) {}

const wl_output_listener wlOutputListener = {
    .geometry = wlOutputGeometry,
    .mode = wlOutputMode,
    .done = wlOutputDone,
    .scale = wlOutputScale
};

/// xdg_surface listener: ack configure
void xdgSurfaceConfigure(void* data, xdg_surface* surface, uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
    (void)data;
}

const xdg_surface_listener xdgSurfaceListener = {
    .configure = xdgSurfaceConfigure
};

} // namespace

// Forward declarations for xdg_toplevel listeners (implemented after class)
void xdgToplevelConfigure(void* data, xdg_toplevel* /*toplevel*/,
                          int32_t width, int32_t height,
                          wl_array* /*states*/);
void xdgToplevelClose(void* data, xdg_toplevel* /*toplevel*/);
void xdgToplevelConfigureBounds(void* /*data*/, xdg_toplevel* /*toplevel*/,
                                int32_t /*width*/, int32_t /*height*/);
void xdgToplevelWmCapabilities(void* /*data*/, xdg_toplevel* /*toplevel*/,
                               wl_array* /*capabilities*/);

const xdg_toplevel_listener xdgToplevelListener = {
    .configure = xdgToplevelConfigure,
    .close = xdgToplevelClose,
    .configure_bounds = xdgToplevelConfigureBounds,
    .wm_capabilities = xdgToplevelWmCapabilities
};

class WaylandSurfaceBackend : public SurfaceBackend {
public:
    ~WaylandSurfaceBackend() { destroy(); }

    // Friend declarations for xdg_toplevel listeners to access private members
    friend void xdgToplevelConfigure(void* data, xdg_toplevel* /*toplevel*/,
                                     int32_t width, int32_t height,
                                     wl_array* /*states*/);
    friend void xdgToplevelClose(void* data, xdg_toplevel* /*toplevel*/);

    bool connect(std::string_view session) override {
        // Only connect if explicitly "wayland" or "auto" with WAYLAND_DISPLAY set
        if (session == "x11")
            return false;
        if (session == "auto") {
            const char* wlDisplay = std::getenv("WAYLAND_DISPLAY");
            if (!wlDisplay || *wlDisplay == '\0')
                return false;
        }

        mDisplay = wl_display_connect(nullptr);
        if (!mDisplay)
            throw ls::error("cannot connect to Wayland display ($WAYLAND_DISPLAY)");

        wl_registry* registry = wl_display_get_registry(mDisplay);
        wl_registry_add_listener(registry, &registryListener, &mGlobals);
        wl_display_roundtrip(mDisplay); // block until globals bound

        if (!mGlobals.compositor || !mGlobals.xdgWmBase)
            throw ls::error("Wayland compositor missing required globals (compositor, xdg_wm_base)");

        xdg_wm_base_add_listener(mGlobals.xdgWmBase, &xdgWmBaseListener, this);

        // Enumerate outputs
        mOutputs = enumerateOutputs();

        wl_registry_destroy(registry);
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
                              uint32_t /*colorspace*/) override {
        const OutputEntry* target = selectOutput(output_name);

        // Create wl_surface
        mSurface = wl_compositor_create_surface(mGlobals.compositor);
        if (!mSurface)
            throw ls::error("wl_compositor_create_surface failed");
        wl_surface_add_listener(mSurface, &surfaceListener, this); // TEMP DEBUG

        // Create xdg_surface
        mXdgSurface = xdg_wm_base_get_xdg_surface(mGlobals.xdgWmBase, mSurface);
        if (!mXdgSurface)
            throw ls::error("xdg_wm_base_get_xdg_surface failed");
        xdg_surface_add_listener(mXdgSurface, &xdgSurfaceListener, this);

        // Create xdg_toplevel
        mXdgToplevel = xdg_surface_get_toplevel(mXdgSurface);
        if (!mXdgToplevel)
            throw ls::error("xdg_surface_get_toplevel failed");
        xdg_toplevel_add_listener(mXdgToplevel, &xdgToplevelListener, this);

        // Set app ID for WM identification
        xdg_toplevel_set_app_id(mXdgToplevel, "lsfg-vk-app");

        // Fullscreen on specified output or primary
        // TEMP DEBUG: LSFGVK_APP_NO_FS=1 skips the fullscreen request (plain
        // toplevel) to isolate KWin fullscreen handling as the stall cause.
        const bool noFullscreen = std::getenv("LSFGVK_APP_NO_FS") != nullptr;
        if (!noFullscreen) {
            if (!output_name.empty()) {
                for (const auto& o : mOutputs) {
                    if (o.geom.name == output_name && o.wlOutput) {
                        xdg_toplevel_set_fullscreen(mXdgToplevel, o.wlOutput);
                        break;
                    }
                }
            } else {
                // Primary output: first connected output with xdg_output done
                for (const auto& o : mOutputs) {
                    if (o.connected && o.xdgOutputDone && o.wlOutput) {
                        xdg_toplevel_set_fullscreen(mXdgToplevel, o.wlOutput);
                        break;
                    }
                }
            }
            dbg("fullscreen requested (noFullscreen=%d)", noFullscreen);
        } else {
            dbg("fullscreen SKIPPED (LSFGVK_APP_NO_FS set)");
        }

        // Commit initial surface state
        wl_surface_commit(mSurface);
        // wait for the initial configure: KWin may deliver it only after the
        // sync callback of a single roundtrip, so loop (bounded) until a size
        // was reported; the caller falls back to the requested extent if not.
        for (int i = 0; i < 50; ++i) {
            if (mWindowExtent.width > 0 && mWindowExtent.height > 0)
                break;
            wl_display_roundtrip(mDisplay);
        }

        // Apply pending size from configure if any
        if (mResizePending) {
            extent.width = mPendingWidth;
            extent.height = mPendingHeight;
            mResizePending = false;
        }

        // the configure dispatched during the roundtrip above already recorded
        // the compositor's actual size; only fall back to the requested extent
        // when no size was reported (lazy compositor).
        if (mWindowExtent.width == 0 && mWindowExtent.height == 0)
            mWindowExtent = extent;
        return reinterpret_cast<WindowHandle>(mSurface);
    }

    VkSurfaceKHR createSurface(const vk::Vulkan& vk, WindowHandle handle) override {
        auto createWaylandSurface = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
            loaderGetProc()(vk.inst(), "vkCreateWaylandSurfaceKHR"));
        if (!createWaylandSurface)
            throw ls::error("vkGetInstanceProcAddr(vkCreateWaylandSurfaceKHR) returned null");

        auto* surf = static_cast<wl_surface*>(handle);
        VkWaylandSurfaceCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        ci.display = mDisplay;
        ci.surface = surf;

        VkSurfaceKHR surface{VK_NULL_HANDLE};
        VkResult result = createWaylandSurface(vk.inst(), &ci, nullptr, &surface);
        if (result != VK_SUCCESS)
            throw ls::error("vkCreateWaylandSurfaceKHR failed",
                    ls::vulkan_error(result, "vkCreateWaylandSurfaceKHR"));

        // Cache for later queries
        mFuncs = vk.fi();
        mPhysDev = vk.physdev();
        mVkInstance = vk.inst();
        mSurfaceKHR = surface;
        return surface;
    }

    VkExtent2D windowExtent() const override {
        // last size reported by the compositor (0x0 until the first configure)
        return mWindowExtent;
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
        // Wayland surfaces report their formats; pick the first 8-bit UNORM
        // RGBA the compositor accepts. B8G8R8A8 (GBM XRGB8888) is the
        // near-universal native layout of wlroots/KWin/Mutter.
        const PFN_vkGetInstanceProcAddr mpa = loaderGetProc();
        auto getFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
            mpa(mVkInstance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
        uint32_t count{};
        if (getFormats != nullptr
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
        return VK_FORMAT_B8G8R8A8_UNORM;
    }

    bool processEvents(int timeout_ms) override {
        if (!mDisplay || !mSurface)
            return false;

        // TEMP DEBUG: keep exactly one frame callback outstanding; it fires
        // once per displayed buffer, so its rate shows whether the compositor
        // is still presenting this surface. gated: issuing a frame request on
        // every loop iteration would add a protocol round-trip per frame.
        if (dbgEnabled() && !g_dbgFrameCbPending) {
            wl_callback* cb = wl_surface_frame(mSurface);
            if (cb != nullptr) {
                wl_callback_add_listener(cb, &surfaceFrameListener, nullptr);
                g_dbgFrameCbPending = true;
            }
        }

        // Dispatch everything already buffered (non-blocking).
        int ret = wl_display_dispatch_pending(mDisplay);
        if (ret < 0)
            return false;

        // Nothing buffered: wait for data up to timeout, then drain. The
        // drain must stay NON-blocking: the blocking wl_display_dispatch
        // (default queue) deadlocks when the bytes poll signalled carry only
        // events for another queue - RADV owns the wl_buffer release
        // listeners on its own queue. It consumes those bytes, dispatches
        // nothing on the default queue, and then waits forever on the
        // already-empty socket (observed: main thread stuck in
        // wl_display_dispatch -> ppoll(NULL) with Recv-Q 0).
        // dispatch_pending routes every read byte to its own queue and
        // returns without blocking.
        if (ret == 0) {
            struct pollfd pfd{};
            pfd.fd = wl_display_get_fd(mDisplay);
            pfd.events = POLLIN;
            int pollRet = poll(&pfd, 1, timeout_ms);
            if (pollRet > 0 && (pfd.revents & POLLIN)) {
                int r;
                do {
                    r = wl_display_dispatch_pending(mDisplay);
                } while (r > 0);
            }
        }

        // Check for resize
        if (mResizePending) {
            mResizePending = false;
            if (dbgEnabled())
                std::fprintf(stderr, "lsfg-vk-app: [dbg] processEvents exit: resize\n");
            return true;
        }

        // Check for close
        if (mClosePending) {
            if (dbgEnabled())
                std::fprintf(stderr, "lsfg-vk-app: [dbg] processEvents exit: close\n");
            return true;
        }

        return false;
    }

    void destroy() override {
        if (mXdgToplevel) {
            xdg_toplevel_destroy(mXdgToplevel);
            mXdgToplevel = nullptr;
        }
        if (mXdgSurface) {
            xdg_surface_destroy(mXdgSurface);
            mXdgSurface = nullptr;
        }
        if (mSurface) {
            wl_surface_destroy(mSurface);
            mSurface = nullptr;
        }
        if (mSurfaceKHR != VK_NULL_HANDLE && mVkInstance != VK_NULL_HANDLE) {
            const PFN_vkGetInstanceProcAddr mpa = loaderGetProc();
            auto destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
                mpa(mVkInstance, "vkDestroySurfaceKHR"));
            if (destroySurface != nullptr)
                destroySurface(mVkInstance, mSurfaceKHR, nullptr);
            mSurfaceKHR = VK_NULL_HANDLE;
        }
        if (mDisplay) {
            wl_display_disconnect(mDisplay);
            mDisplay = nullptr;
        }
        mOutputs.clear();
        mGlobals = {};
    }

private:
    std::vector<OutputEntry> enumerateOutputs() {
        std::vector<OutputEntry> entries;

        if (!mGlobals.xdgOutputManager) {
            // Fallback: enumerate via wl_output directly
            // This requires a roundtrip to get wl_output globals
            // For simplicity, we rely on xdg-output being available on KWin
            return entries;
        }

        // We need to get wl_output globals first. Since we already did a roundtrip
        // during connect, we need to iterate the registry again or cache them.
        // Simpler approach: create a temporary registry to get wl_output objects.
        // But we already have the registry from connect. Let's use a different approach:
        // The xdg_output_manager gives us xdg_output for each wl_output.
        // We need to get wl_output objects. Since we can't easily re-iterate,
        // we'll use a trick: the xdg_output_manager creates xdg_output for each
        // wl_output. We'll just wait for the xdg_output events.

        // Actually, we need to get wl_output objects. Let's do a roundtrip with
        // a temporary registry listener that captures wl_output.
        // But we already destroyed the registry. Let's create a new one.

        wl_registry* registry = wl_display_get_registry(mDisplay);
        struct OutputCollector {
            std::vector<OutputEntry>* entries;
            Globals* globals;
        } collector{&entries, &mGlobals};

        auto outputGlobal = [](void* data, wl_registry* registry, uint32_t name,
                               const char* interface, uint32_t version) {
            auto* c = static_cast<OutputCollector*>(data);
            if (std::strcmp(interface, wl_output_interface.name) == 0) {
                // Bind wl_output at v2: versions 3+ add the `name`/`description`
                // events (opcodes 4/5) that wlOutputListener does not implement,
                // and KWin sends them, crashing the client ("listener function
                // for opcode 4 of wl_output is NULL"). Names come from xdg-output,
                // so v2 (geometry/mode/done/scale only) is all we need.
                wl_output* output = static_cast<wl_output*>(
                    wl_registry_bind(registry, name, &wl_output_interface, 2));
                // push the entry into the vector FIRST, then attach the wl_output
                // listener to the stable heap element (&vector.back()). Binding to a
                // stack-local `entry` here and moving it into the vector afterwards
                // leaves the listener pointing at a dead stack frame: KWin's later
                // wl_output mode/geometry/done events would write through it (ASan:
                // stack-use-after-return in wlOutputMode). Guaranteeing vector
                // element stability requires reserve() up front (no reallocation
                // moves the element the listener points at).
                c->entries->push_back(OutputEntry{});
                auto& back = c->entries->back();
                back.wlOutput = output;
                back.connected = true;
                wl_output_add_listener(output, &wlOutputListener, &back);

                // Also get xdg_output for this wl_output
                if (c->globals->xdgOutputManager) {
                    back.xdgOutput = zxdg_output_manager_v1_get_xdg_output(
                        c->globals->xdgOutputManager, output);
                    zxdg_output_v1_add_listener(back.xdgOutput, &xdgOutputListener, &back);
                }
            }
        };

        wl_registry_listener collectorListener = {
            .global = outputGlobal,
            .global_remove = registryGlobalRemove
        };
        wl_registry_add_listener(registry, &collectorListener, &collector);
        // reserve before the roundtrip: once outputGlobal binds a wl_output and
        // registers its listener against &entries->back(), any later push_back
        // that reallocates would invalidate that pointer too; sizing the vector
        // up front keeps every element at a stable address for the listener.
        collector.entries->reserve(4);
        wl_display_roundtrip(mDisplay);
        wl_registry_destroy(registry);

        // Wait for xdg_output done events. wl_display_roundtrip (not the
        // blocking wl_display_dispatch) is mandatory here: dispatch blocks on
        // idle waiting for any event and hangs forever when the done event was
        // already delivered during the roundtrip above. A roundtrip forces the
        // compositor to reply to our sync request and dispatches whatever
        // xdg_output events are queued, so the done handler reliably fires.
        for (auto& entry : entries) {
            while (!entry.xdgOutputDone && entry.xdgOutput) {
                wl_display_roundtrip(mDisplay);
            }
        }

        return entries;
    }

    const OutputEntry* selectOutput(std::string_view name) {
        const std::string want = std::string(name);
        if (want.empty()) {
            // Primary: first connected output with xdg_output done
            for (const auto& e : mOutputs)
                if (e.connected && e.xdgOutputDone)
                    return &e;
            // Fallback: first connected
            for (const auto& e : mOutputs)
                if (e.connected)
                    return &e;
            if (!mOutputs.empty())
                return &mOutputs.front();
            throw ls::error("no Wayland outputs available");
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

    wl_display* mDisplay{nullptr};
    Globals mGlobals{};
    std::vector<OutputEntry> mOutputs;
    wl_surface* mSurface{nullptr};
    xdg_surface* mXdgSurface{nullptr};
    xdg_toplevel* mXdgToplevel{nullptr};

    // Cached from createSurface for queries
    vk::VulkanInstanceFuncs mFuncs{};
    VkPhysicalDevice mPhysDev{VK_NULL_HANDLE};
    VkInstance mVkInstance{VK_NULL_HANDLE};
    VkSurfaceKHR mSurfaceKHR{VK_NULL_HANDLE};

    // Resize/close tracking
    bool mResizePending{false};
    bool mClosePending{false};
    uint32_t mPendingWidth{0};
    uint32_t mPendingHeight{0};
    VkExtent2D mWindowExtent{0, 0};
};

std::unique_ptr<SurfaceBackend> createWaylandSurfaceBackend() {
    return std::make_unique<WaylandSurfaceBackend>();
}

/// xdg_toplevel listener implementations (in ls::wsi namespace for access to WaylandSurfaceBackend)
void xdgToplevelConfigure(void* data, xdg_toplevel* /*toplevel*/,
                          int32_t width, int32_t height,
                          wl_array* states) {
    auto* backend = static_cast<WaylandSurfaceBackend*>(data);
    // TEMP DEBUG: log every configure with its states (KWin signals
    // fullscreen/activation changes here)
    std::string stateStr;
    if (states != nullptr) {
        const auto* statePtr = static_cast<const uint32_t*>(states->data);
        size_t count = states->size / sizeof(uint32_t);
        for (size_t i = 0; i < count; ++i) {
            if (!stateStr.empty())
                stateStr += ",";
            stateStr += std::to_string(statePtr[i]);
        }
    }
    dbg("toplevel configure %dx%d states=[%s]", width, height, stateStr.c_str());
    if (width <= 0 || height <= 0)
        return;
    // xdg-shell emits a configure after every buffer commit, not only on real
    // resizes (KWin acks each presented frame this way). Flag a resize only
    // when the size actually changed; mWindowExtent is the last observed size.
    const uint32_t w = static_cast<uint32_t>(width);
    const uint32_t h = static_cast<uint32_t>(height);
    if (w == backend->mWindowExtent.width && h == backend->mWindowExtent.height)
        return;
if (dbgEnabled())
        std::fprintf(stderr, "lsfg-vk-app: [dbg] configure %ux%u (was %ux%u) -> FLAG\n",
                     w, h, backend->mWindowExtent.width, backend->mWindowExtent.height);
    backend->mWindowExtent = VkExtent2D{ w, h };
    backend->mPendingWidth = w;
    backend->mPendingHeight = h;
    backend->mResizePending = true;
}

void xdgToplevelClose(void* data, xdg_toplevel* /*toplevel*/) {
    auto* backend = static_cast<WaylandSurfaceBackend*>(data);
    backend->mClosePending = true;
}

void xdgToplevelConfigureBounds(void* /*data*/, xdg_toplevel* /*toplevel*/,
                                int32_t /*width*/, int32_t /*height*/) {}

void xdgToplevelWmCapabilities(void* /*data*/, xdg_toplevel* /*toplevel*/,
                                wl_array* /*capabilities*/) {}

} // namespace ls::wsi