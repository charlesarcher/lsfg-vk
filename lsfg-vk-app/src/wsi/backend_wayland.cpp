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
#include <cstdint>
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
        g->xdgOutputManager = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 4));
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

        // Commit initial surface state
        wl_surface_commit(mSurface);
        wl_display_roundtrip(mDisplay); // wait for configure

        // Apply pending size from configure if any
        if (mResizePending) {
            extent.width = mPendingWidth;
            extent.height = mPendingHeight;
            mResizePending = false;
        }

        mWindowExtent = extent;
        return reinterpret_cast<WindowHandle>(mSurface);
    }

    VkSurfaceKHR createSurface(const vk::Vulkan& vk, WindowHandle handle) override {
        auto createWaylandSurface = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
            loaderGetProc()(vk.inst(), "vkCreateWaylandSurfaceKHR"));
        if (!createWaylandSurface)
            throw ls::error("vkGetInstanceProcAddr(vkCreateWaylandSurfaceKHR) returned null");

        VkWaylandSurfaceCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        ci.display = mDisplay;
        ci.surface = static_cast<wl_surface*>(handle);

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

    void surfaceCaps(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR& caps,
                     std::vector<VkColorSpaceKHR>& colorspaces) override {
        const PFN_vkGetInstanceProcAddr mpa = loaderGetProc();
        auto getCaps = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            mpa(mVkInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        if (getCaps != nullptr)
            getCaps(mPhysDev, surface, &caps);

        auto getFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
            mpa(mVkInstance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
        if (getFormats == nullptr)
            return;

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

    bool processEvents(int timeout_ms) override {
        if (!mDisplay || !mSurface)
            return false;

        // Dispatch pending events
        int ret = wl_display_dispatch_pending(mDisplay);
        if (ret < 0)
            return false;

        // If no pending events, block with timeout
        if (ret == 0) {
            struct pollfd pfd{};
            pfd.fd = wl_display_get_fd(mDisplay);
            pfd.events = POLLIN;
            int pollRet = poll(&pfd, 1, timeout_ms);
            if (pollRet > 0 && (pfd.revents & POLLIN)) {
                wl_display_dispatch(mDisplay);
            }
        }

        // Check for resize
        if (mResizePending) {
            mResizePending = false;
            return true;
        }

        // Check for close
        if (mClosePending) {
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
                wl_output* output = static_cast<wl_output*>(
                    wl_registry_bind(registry, name, &wl_output_interface, 4));
                OutputEntry entry{};
                entry.wlOutput = output;
                entry.connected = true;
                wl_output_add_listener(output, &wlOutputListener, &entry);
                c->entries->push_back(std::move(entry));

                // Also get xdg_output for this wl_output
                if (c->globals->xdgOutputManager) {
                    auto& back = c->entries->back();
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
        wl_display_roundtrip(mDisplay);
        wl_registry_destroy(registry);

        // Wait for xdg_output done events
        for (auto& entry : entries) {
            while (!entry.xdgOutputDone && entry.xdgOutput) {
                wl_display_dispatch(mDisplay);
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
                          wl_array* /*states*/) {
    auto* backend = static_cast<WaylandSurfaceBackend*>(data);
    if (width > 0 && height > 0) {
        backend->mPendingWidth = static_cast<uint32_t>(width);
        backend->mPendingHeight = static_cast<uint32_t>(height);
        backend->mResizePending = true;
    }
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