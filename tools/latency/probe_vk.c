/* probe_vk.c — click→photon THROUGH a Vulkan swapchain, with optional lsfg-vk
 * layer injection for the DOUBLED leg (Session 40).
 *
 * How it measures:
 *   - renders a trivial Vulkan swapchain (2 images, immediate/mailbox) at ~240 Hz
 *     on the Wayland surface of this client (VK_KHR_wayland_surface)
 *   - a uinput click (uclick binary, timestamped to /tmp/latency_probe_inputs.log)
 *     sets the NEXT committed frame's color to magenta (one frame only)
 *   - each commit arms a wp_presentation_feedback; the 'presented' latch is the
 *     photon anchor (KWin CLOCK_MONOTONIC, same clock as evdev stamps)
 *   - we pair a click t0 with the latch of the FIRST magenta commit after t0
 *     => click→photon. baseline leg: layer OFF; doubled leg: layer ON
 *     (LSFGVK_PROFILE=furmark-oneway forced).
 * build:
 *   gcc -O2 -o /tmp/probe_vk tools/latency/probe_vk.c  \
 *       $(pkg-config --cflags --libs wayland-client vulkan) -lpthread
 */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "presentation-time-client-protocol.h"
#undef nullptr  /* protocol headers may define it; keep C style explicit */
#include <linux/input.h>
#include <fcntl.h>
#include <poll.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdatomic.h>

static struct wl_display* dpy;
static struct wl_compositor* comp;
static struct wl_shm* shm;
static struct xdg_wm_base* xdgwm;
static struct wp_presentation* present;
static uint32_t presentClock = 0;

static void pres_clock(void* d, struct wp_presentation* p, uint32_t c) {
    (void)d; (void)p; presentClock = c;
}
static const struct wp_presentation_listener pres_listener = { .clock_id = pres_clock };

static int g_surfConfigured = 0;
static void xdg_surf_conf(void* d, struct xdg_surface* s, uint32_t serial) {
    (void)d; xdg_surface_ack_configure(s, serial); g_surfConfigured = 1;
}
static const struct xdg_surface_listener xdg_surf_listener = { .configure = xdg_surf_conf };
static void xdg_tl_conf(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* s) { (void)d;(void)t;(void)w;(void)h;(void)s; }
static const struct xdg_toplevel_listener tl_listener = { .configure = xdg_tl_conf };

static void global(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) xdgwm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wp_presentation_interface.name)) {
        present = wl_registry_bind(r, name, &wp_presentation_interface, 2);
        wp_presentation_add_listener(present, &pres_listener, nullptr);
    }
}
static void global_remove(void* d, struct wl_registry* r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_listener = { .global = global, .global_remove = global_remove };

/* --- presented feedback (newest latch) --- */
static _Atomic uint64_t g_tag[4] = {0,0,0,0};  /* per-slot latch value  */
static _Atomic uint64_t g_latch[4] = {0,0,0,0};
static void fb_presented(void* data, struct wp_presentation_feedback* f,
        uint32_t s_hi, uint32_t s_lo, uint32_t nsec, uint32_t refresh,
        uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
    (void)refresh; (void)flags;
    uint64_t ns = ((uint64_t)s_hi << 32 | s_lo) * 1000000000ULL + nsec;
    unsigned slot = (unsigned)(uintptr_t)data & 3u;
    atomic_store(&g_latch[slot], ns);
    atomic_store(&g_tag[slot], ((uint64_t)seq_hi << 32) | seq_lo);
    wp_presentation_feedback_destroy(f);
}
static void fb_disc(void* data, struct wp_presentation_feedback* f) {
    (void)data; wp_presentation_feedback_destroy(f);
}
static void fb_sync(void* data, struct wp_presentation_feedback* f, struct wl_output* o) { (void)data;(void)f;(void)o; }
static const struct wp_presentation_feedback_listener fb_listener =
    { .sync_output = fb_sync, .presented = fb_presented, .discarded = fb_disc };

static _Atomic unsigned g_plantSlot = 2;
static _Atomic uint64_t  g_plantNs  = 0;     /* click injection moment  */
static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const unsigned wantSamples = argc > 1 ? (unsigned)atoi(argv[1]) : 20;

    dpy = wl_display_connect(nullptr);
    if (!dpy) { fprintf(stderr, "wl_display_connect failed\n"); return 1; }
    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, nullptr);
    wl_display_roundtrip(dpy);
    if (!comp || !xdgwm || !present) { fprintf(stderr, "missing globals\n"); return 2; }

    /* surface, mapped per the proven contract */
    struct wl_surface* surf = wl_compositor_create_surface(comp);
    struct xdg_surface* xs = xdg_wm_base_get_xdg_surface(xdgwm, surf);
    xdg_surface_add_listener(xs, &xdg_surf_listener, nullptr);
    struct xdg_toplevel* tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_listener, nullptr);
    xdg_toplevel_set_app_id(tl, "probe-vk");
    xdg_toplevel_set_title(tl, "probe-vk");
    /* NOTE: do NOT fullscreen — occluding the WORK SCREEN's own output keeps
       KWin from feeding presented events in some focus states; a small idle
       toplevel composed on the active screen works reliably (probe_vk10). */
    wl_surface_commit(surf);
    for (int i = 0; i < 10 && !g_surfConfigured; ++i) wl_display_roundtrip(dpy);
    if (!g_surfConfigured) { fprintf(stderr, "xdg configure never arrived\n"); return 3; }

    /* --- Vulkan init --- */
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "probe-vk"; ai.apiVersion = VK_API_VERSION_1_1;
    const char* instExts[] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = instExts;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { fprintf(stderr, "vkCreateInstance failed\n"); return 4; }

    uint32_t ndev = 0; vkEnumeratePhysicalDevices(inst, &ndev, nullptr);
    if (!ndev) { fprintf(stderr, "no devices\n"); return 5; }
    VkPhysicalDevice devs[8]; vkEnumeratePhysicalDevices(inst, &ndev, devs);
    /* pick the RENDER GPU explicitly (MESA_VK_DEVICE_SELECT is honored too) */
    VkPhysicalDevice chosen = devs[0];
    for (uint32_t i = 0; i < ndev; ++i) {
        VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
        vkGetPhysicalDeviceProperties2(devs[i], &p2);
        if (p2.properties.vendorID == 0x1002 && p2.properties.deviceID == 0x7550) chosen = devs[i];
    }
    {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(chosen, &p);
        printf("render device: %s\n", p.deviceName);
    }
    uint32_t qfamN = 0; vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qfamN, nullptr);
    VkQueueFamilyProperties qfams[8]; vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qfamN, qfams);
    uint32_t gfxFam = UINT32_MAX;
    for (uint32_t i = 0; i < qfamN; ++i)
        if (qfams[i].queueCount && (qfams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { gfxFam = i; break; }
    if (gfxFam == UINT32_MAX) { fprintf(stderr, "no gfx queue\n"); return 6; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = gfxFam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* devExts[] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = devExts;
    VkDevice dev;
    if (vkCreateDevice(chosen, &dci, nullptr, &dev) != VK_SUCCESS) { fprintf(stderr, "vkCreateDevice failed\n"); return 7; }
    VkQueue queue; vkGetDeviceQueue(dev, gfxFam, 0, &queue);

    /* --- Wayland surface via WSI + swapchain --- */
    VkWaylandSurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
    sci.display = dpy; sci.surface = surf;
    VkSurfaceKHR vsurf;
    if (vkCreateWaylandSurfaceKHR(inst, &sci, nullptr, &vsurf) != VK_SUCCESS) { fprintf(stderr, "vkCreateWaylandSurface failed\n"); return 8; }

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(chosen, gfxFam, vsurf, &supported);
    if (!supported) { fprintf(stderr, "surface not supported on gfx family\n"); return 9; }

    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(chosen, vsurf, &caps);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) { ext.width = 320; ext.height = 180; }
    printf("swapchain extent %ux%u minImages=%u\n", ext.width, ext.height, caps.minImageCount);

    VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swci.surface = vsurf;
    swci.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    swci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swci.imageExtent = ext;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;  /* un-limited, rule out FIFO pacing */
    VkSwapchainKHR swap;
    if (vkCreateSwapchainKHR(dev, &swci, nullptr, &swap) != VK_SUCCESS) { fprintf(stderr, "CreateSwapchain failed\n"); return 10; }
    uint32_t nimg = 0; vkGetSwapchainImagesKHR(dev, swap, &nimg, nullptr);
    VkImage imgs[8]; vkGetSwapchainImagesKHR(dev, swap, &nimg, imgs);
    printf("swapchain images %u, present clock %u\n", nimg, presentClock);

    /* memory-free fill: use vkCmdClearColorImage (no vertex pipeline needed) */
    VkCommandPool cpool;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = gfxFam;
    vkCreateCommandPool(dev, &pci, nullptr, &cpool);
    VkCommandBuffer cbs[8];
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = nimg;
    vkAllocateCommandBuffers(dev, &cai, cbs);

    /* swapchain image layout → GENERAL once for clearImage */
    {
        vkResetCommandBuffer(cbs[0], 0);
        VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(cbs[0], &bbi);
        for (uint32_t i = 0; i < nimg; ++i) {
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = imgs[i]; b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 };
            vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,0,0,0,1,&b);
            VkClearColorValue clear = { .float32 = { 0.06f, 0.05f, 0.12f, 1.0f } };
            vkCmdClearColorImage(cbs[0], imgs[i], VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                &(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 });
        }
        vkEndCommandBuffer(cbs[0]);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cbs[0];
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkSemaphore acqSems[3]; VkSemaphore presSems[3];
    VkSemaphoreCreateInfo semci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < 3; ++i) {
        vkCreateSemaphore(dev, &semci, nullptr, &acqSems[i]);
        vkCreateSemaphore(dev, &semci, nullptr, &presSems[i]);
    }

    /* --- input thread: SAME discovery as probe_latency (all mice + virtual clicker) --- */
    int fds[64]; int nfd = 0;
    glob_t gg;
    if (glob("/dev/input/by-id/*event-mouse*", 0, nullptr, &gg) == 0) {
        char names[64][128];
        size_t n = gg.gl_pathc < 32 ? gg.gl_pathc : 32;
        for (size_t i = 0; i < n; ++i) snprintf(names[i], 128, "%s", gg.gl_pathv[i]);
        globfree(&gg);
        for (size_t i = 0; i < n; ++i) {
            int fd = open(names[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            fds[nfd++] = fd;
        }
    }
    glob_t ge;
    if (glob("/dev/input/event*", 0, nullptr, &ge) == 0) {
        char enames[64][128];
        size_t n = ge.gl_pathc < 32 ? ge.gl_pathc : 32;
        for (size_t i = 0; i < n; ++i) snprintf(enames[i], 128, "%s", ge.gl_pathv[i]);
        globfree(&ge);
        for (size_t i = 0; i < n; ++i) {
            char nm[256] = "";
            int fd = open(enames[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            if (ioctl(fd, EVIOCGNAME(255), nm) < 0 || !strstr(nm, "latency-probe-clicker")) { close(fd); continue; }
            printf("virtual clicker: %s\n", enames[i]);
            fds[nfd++] = fd;
        }
    }
    printf("input devices armed: %d\n", nfd);
    /* fd health probe: poll each armed fd for 150 ms — the clicker is seen as
       readable within one click period (400 ms), real mice stay silent. */
    {
        struct pollfd hp[64];
        for (int i = 0; i < nfd; ++i) { hp[i].fd = fds[i]; hp[i].events = POLLIN; }
        const int hpres = poll(hp, (nfds_t)nfd, 150);
        printf("health poll: %d readable of %d fds (t=0s)\n", hpres, nfd);
        for (int i = 0; i < nfd; ++i)
            if (hp[i].revents & POLLIN) printf("  readable fd idx %d\n", i);
    }

    /* --- main loop: render + feedback + click pairing --- */
    printf("click the mouse (or run uclick) — %u samples wanted\n", wantSamples);
    uint64_t samples[512]; unsigned collected = 0;
    uint64_t thisClickT2 = 0;            /* pending pairing (declared outer scope) */
    uint64_t clickT[64]; unsigned nClickT = 0;
    for (uint32_t i = 0; i < nfd; ++i) {
        unsigned clk = CLOCK_MONOTONIC;
        ioctl(fds[i], EVIOCSCLOCKID, &clk);
    }

    uint32_t imgIdx = 0; (void)imgIdx;
    struct pollfd pfd[64];
    for (int i = 0; i < nfd; ++i) { pfd[i].fd = fds[i]; pfd[i].events = POLLIN; }
    struct input_event ev[64];

    uint64_t frameSeq = 0;
    for (;;) {
        /* pump input: collect clicks — poll() first, only read armed fds */
        {
            struct pollfd qfds[64];
            for (int i = 0; i < nfd; ++i) { qfds[i].fd = fds[i]; qfds[i].events = POLLIN; }
            const int qres = poll(qfds, (nfds_t)nfd, 0);
            for (int i = 0; qres > 0 && i < nfd; ++i) {
                if (!(qfds[i].revents & POLLIN)) continue;
                ssize_t n;
                while ((n = read(fds[i], ev, sizeof(ev))) > 0) {
                    static unsigned rawEvents = 0;
                    if (++rawEvents <= 5)
                        printf("raw ev fd#%d n=%zd type=%u code=%u val=%d\n",
                            i, n, ev[0].type, ev[0].code, ev[0].value);
                    for (size_t j = 0; j < (size_t)n / sizeof(ev[0]); ++j) {
                        if (ev[j].type == EV_KEY
                                && ev[j].code == BTN_LEFT && ev[j].value == 1) {
                            static unsigned totalClicks = 0;
                            if (++totalClicks == 1)
                                printf("first click seen\n");
                            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                            const uint64_t t0 = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
                            if (nClickT < 64) clickT[nClickT++] = t0;
                        }
                    }
                }
            }
        } /* pump scope */

        /* acquire + draw + commit */
        uint32_t idx = 0;
        VkResult ac = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, acqSems[frameSeq % 3], VK_NULL_HANDLE, &idx);
        if (ac != VK_SUCCESS && ac != VK_SUBOPTIMAL_KHR) continue;

        int magenta = 0;
        uint64_t thisClickT = 0;
        if (nClickT > 0) {
            /* newest click becomes THIS frame's paint instant */
            thisClickT = clickT[--nClickT];
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
            (void)now;
            /* paint magenta for the frame from THIS swapchain image */
            vkResetCommandBuffer(cbs[idx], 0);
            VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            vkBeginCommandBuffer(cbs[idx], &bbi);
            VkClearColorValue clear = { .float32 = { 1.0f, 0.02f, 1.0f, 1.0f } };
            vkCmdClearColorImage(cbs[idx], imgs[idx], VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                &(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 });
            vkEndCommandBuffer(cbs[idx]);
            VkSemaphore ws[1] = { acqSems[frameSeq % 3] };
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = 1; si.pWaitSemaphores = ws;
            VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            si.pWaitDstStageMask = &st;
            si.commandBufferCount = 1; si.pCommandBuffers = &cbs[idx];
            VkSemaphore sg[1] = { presSems[frameSeq % 3] };
            si.signalSemaphoreCount = 1; si.pSignalSemaphores = sg;
            vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
            magenta = 1;
        } else {
            /* background frame: reset the slot back to dim (cheap clear command) */
            vkResetCommandBuffer(cbs[idx], 0);
            VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            vkBeginCommandBuffer(cbs[idx], &bbi);
            VkClearColorValue clear = { .float32 = { 0.06f, 0.05f, 0.12f, 1.0f } };
            vkCmdClearColorImage(cbs[idx], imgs[idx], VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                &(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 });
            vkEndCommandBuffer(cbs[idx]);
            VkSemaphore ws[1] = { acqSems[frameSeq % 3] };
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = 1; si.pWaitSemaphores = ws;
            VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            si.pWaitDstStageMask = &st;
            si.commandBufferCount = 1; si.pCommandBuffers = &cbs[idx];
            VkSemaphore sg[1] = { presSems[frameSeq % 3] };
            si.signalSemaphoreCount = 1; si.pSignalSemaphores = sg;
            vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        }

        /* restore-dim for the NEXT use of this slot is handled by the bg repaint */

        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        VkSemaphore sg[1] = { presSems[frameSeq % 3] };
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = sg;
        pi.swapchainCount = 1; pi.pSwapchains = &swap; pi.pImageIndices = &idx;
        unsigned slot = idx & 3u;
        if (magenta) printf("magenta commit (slot %u)\n", slot);
        atomic_store(&g_latch[slot], 0);
        struct wp_presentation_feedback* fb = wp_presentation_feedback(present, surf);
        wp_presentation_feedback_add_listener(fb, &fb_listener, (void*)(uintptr_t)slot);
        if (magenta) atomic_store(&g_plantSlot, slot);
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        (void)pr;
        wl_display_flush(dpy);
        wl_display_roundtrip(dpy);  /* drain presented feedback for this commit */
        frameSeq++;
        if (frameSeq == 1) printf("render loop ALIVE (first frame presented, slot %u)\n", slot);
        if (frameSeq % 240 == 0) printf("loop tick %u\n", frameSeq);

        if (magenta) {
            const uint64_t latch = atomic_load(&g_latch[slot]);
            printf("magenta latch=%llu\n", (unsigned long long)latch);
            if (latch && thisClickT && collected < wantSamples && collected < 512) {
                const double ms = (double)(latch - thisClickT) / 1e6;
                if (ms > 0.0 && ms < 500.0) {
                    samples[collected++] = ms;
                    printf("sample %u: click->latch %.2f ms\n", collected, ms);
                }
                thisClickT = 0;
            }
        }
        if (collected >= wantSamples) break;
        if (wantSamples && frameSeq > wantSamples * 2 + 2000) break; /* runaway guard */
    }

    printf("RESULT n=%u", collected);
    if (collected > 1) {
        for (unsigned i = 1; i < collected; ++i)
            for (unsigned j = i; j && samples[j] < samples[j - 1]; --j) {
                uint64_t t = samples[j]; samples[j] = samples[j - 1]; samples[j - 1] = t;
            }
        printf(" p50=%.2f ms p99=%.2f ms min=%.2f ms max=%.2f ms",
            samples[collected / 2],
            samples[(unsigned)((double)collected * 0.99)],
            samples[0], samples[collected - 1]);
    }
    printf("\n");
    return collected ? 0 : 1;
}
