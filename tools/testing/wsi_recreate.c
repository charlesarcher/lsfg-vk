/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * wsi_recreate.c - Standalone Vulkan headless dynamic swapchain recreation test.
 *
 * Simulates dynamic resolution switches (1080p -> 1080p new -> 1440p)
 * while previous swapchains are active or transitioning, verifying
 * that lsfg-vk isolated swapchain and external IPC presentation handle
 * swapchain destruction and recreation cleanly without crashing.
 */

#define VK_USE_PLATFORM_HEADLESS_EXT
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(expr) do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
        fprintf(stderr, "FAIL %s:%d %s -> %d\n", __FILE__, __LINE__, #expr, _r); \
        return 1; \
    } \
} while (0)

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue q;
static uint32_t qfam;
static VkSurfaceKHR surf;
static PFN_vkCreateHeadlessSurfaceEXT createHeadless;

static int pick_render_gpu(void) {
    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, NULL));
    VkPhysicalDevice pd[8];
    if (n > 8) n = 8;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, pd));

    // Prefer dedicated AMD discrete GPU if available, else first device
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.vendorID == 0x1002 && (p.deviceID == 0x7550 || p.deviceID == 0x7590)) {
            phys = pd[i];
            fprintf(stderr, "selected render device: %s (%04x:%04x)\n", p.deviceName, p.vendorID, p.deviceID);
            return 0;
        }
    }
    if (n > 0) {
        phys = pd[0];
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(phys, &p);
        fprintf(stderr, "fallback render device: %s (%04x:%04x)\n", p.deviceName, p.vendorID, p.deviceID);
        return 0;
    }
    fprintf(stderr, "FAIL: no Vulkan physical device found\n");
    return 1;
}

static VkSwapchainKHR make_sc(uint32_t w, uint32_t h) {
    VkSwapchainCreateInfoKHR ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surf;
    ci.minImageCount = 3;
    ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent.width = w;
    ci.imageExtent.height = h;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    ci.clipped = VK_TRUE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(dev, &ci, NULL, &sc);
    fprintf(stderr, "CreateSwapchain %ux%u -> %d sc=%p\n", w, h, r, (void*)sc);
    if (r != VK_SUCCESS) return VK_NULL_HANDLE;
    return sc;
}

static int present_n(VkSwapchainKHR sc, int n) {
    uint32_t imgCount = 0;
    CHECK(vkGetSwapchainImagesKHR(dev, sc, &imgCount, NULL));
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore sem;
    CHECK(vkCreateSemaphore(dev, &si, NULL, &sem));
    for (int i = 0; i < n; i++) {
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, sc, 1000000000ull, sem, VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "FAIL acquire i=%d r=%d\n", i, ar);
            vkDestroySemaphore(dev, sem, NULL);
            return 1;
        }
        VkPresentInfoKHR pi = {0};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &sem;
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc;
        pi.pImageIndices = &idx;
        VkResult pr = vkQueuePresentKHR(q, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "FAIL present i=%d r=%d\n", i, pr);
            vkDestroySemaphore(dev, sem, NULL);
            return 1;
        }
    }
    vkDestroySemaphore(dev, sem, NULL);
    fprintf(stderr, "presented %d ok sc=%p\n", n, (void*)sc);
    return 0;
}

int main(void) {
    const char* instExt[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME,
    };
    const char* layers[] = { "VK_LAYER_LSFGVK_frame_generation" };
    VkApplicationInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "lsfg-wsi-recreate";
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ii = {0};
    ii.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ii.pApplicationInfo = &ai;
    ii.enabledExtensionCount = 2;
    ii.ppEnabledExtensionNames = instExt;
    ii.enabledLayerCount = 1;
    ii.ppEnabledLayerNames = layers;
    CHECK(vkCreateInstance(&ii, NULL, &inst));
    if (pick_render_gpu()) return 1;

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, NULL);
    VkQueueFamilyProperties qps[16];
    if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qps);
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        if (qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    }
    if (qfam == UINT32_MAX) { fprintf(stderr, "FAIL no gfx queue\n"); return 1; }
    float prio = 1.f;
    VkDeviceQueueCreateInfo dq = {0};
    dq.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dq.queueFamilyIndex = qfam;
    dq.queueCount = 1;
    dq.pQueuePriorities = &prio;
    const char* devExt[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo di = {0};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &dq;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = devExt;
    CHECK(vkCreateDevice(phys, &di, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &q);

    createHeadless = (PFN_vkCreateHeadlessSurfaceEXT)
        vkGetInstanceProcAddr(inst, "vkCreateHeadlessSurfaceEXT");
    if (!createHeadless) { fprintf(stderr, "FAIL no headless surface extension proc\n"); return 1; }
    VkHeadlessSurfaceCreateInfoEXT hi = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    CHECK(createHeadless(inst, &hi, NULL, &surf));

    fprintf(stderr, "--- phase 1080a ---\n");
    VkSwapchainKHR sc1080 = make_sc(1920, 1080);
    if (!sc1080) return 1;
    if (present_n(sc1080, 8)) return 1;

    fprintf(stderr, "--- phase 1080b (1080a still live) ---\n");
    VkSwapchainKHR sc1080b = make_sc(1920, 1080);
    if (!sc1080b) {
        fprintf(stderr, "FAIL second 1080 create while first live\n");
        vkDestroySwapchainKHR(dev, sc1080, NULL);
        return 2;
    }
    if (present_n(sc1080b, 16)) {
        fprintf(stderr, "FAIL second 1080 present\n");
        return 3;
    }

    fprintf(stderr, "--- phase 1440 (1080b still live) ---\n");
    VkSwapchainKHR sc1440 = make_sc(2560, 1440);
    if (!sc1440) {
        fprintf(stderr, "FAIL 1440 create while 1080 live\n");
        vkDestroySwapchainKHR(dev, sc1080b, NULL);
        vkDestroySwapchainKHR(dev, sc1080, NULL);
        return 2;
    }
    if (present_n(sc1440, 16)) {
        fprintf(stderr, "FAIL 1440 present\n");
        vkDestroySwapchainKHR(dev, sc1440, NULL);
        vkDestroySwapchainKHR(dev, sc1080b, NULL);
        vkDestroySwapchainKHR(dev, sc1080, NULL);
        return 3;
    }
    vkDestroySwapchainKHR(dev, sc1080, NULL);
    vkDestroySwapchainKHR(dev, sc1080b, NULL);
    vkDestroySwapchainKHR(dev, sc1440, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    fprintf(stderr, "RECREATE_OK\n");
    return 0;
}
