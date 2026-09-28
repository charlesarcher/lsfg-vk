/* SPDX-License-Identifier: GPL-2.0-or-later */
/* udmabuf bounce probe: a system-memory ring between two GPUs.
 *
 *   render GPU --DMA--> udmabuf (system memory) --DMA--> doubler GPU
 *
 * Shared/default backing memory, no point-to-point, no CPU copy. This is
 * the transport lsfg-vk needs when the two cards have no usable cross-device
 * page-table mapping (the p2p path faults with GCVM_L2_PROTECTION_FAULT).
 * Proven here before any of it is wired into the app.
 *
 * Configurable so the same binary covers the whole size range:
 *   -DW=2560 -DH=1440     capture size (default 512x256)
 *   -DSLOTS=4             ring depth
 *   -DITERS=200           frames to push through
 *
 * What it asserts, in order:
 *   1. the two Navi parts are identified by NAME (never by index - index 0
 *      is the Intel iGPU on this box, and picking it silently produced wrong
 *      answers during development)
 *   2. copies are recorded on a NON-GRAPHICS queue family. On a graphics
 *      family RADV lowers vkCmdCopyImage to an internal draw that writes
 *      through the color backend (UTCL2 client CB) instead of issuing a DMA.
 *      RADV exposes its DMA family as COMPUTE|TRANSFER, not transfer-only.
 *   3. the image memory requirement fits the udmabuf exactly
 *   4. every byte survives render -> sysmem -> doubler
 */
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#define DRM_FORMAT_MOD_LINEAR 0x301

#ifndef W
#define W 2560
#endif
#ifndef H
#define H 1440
#endif
#ifndef SLOTS
#define SLOTS 4
#endif
#ifndef ITERS
#define ITERS 200
#endif
/* SAFETY: this probe has hard-faulted the render GPU on this box. An
 * over-read past the end of the shared mapping wedges gfx_v12_0, and the
 * card then survives rocm-smi --gpureset, PCI hotplug AND a driver rebind
 * - only a power cycle recovers it. Default to the small size that is known
 * good; opt in to full scale with -DFULLSCALE=1. */
#ifndef FULLSCALE
#define FULLSCALE 0
#endif
/* Slack past the end of the image, page-aligned.
 *
 * The faulting accesses are SDMA READS (RW=0x1, client TCP 0x8) on the RENDER
 * card, at page-aligned addresses past the end of the allocation. A read that
 * faults just beyond the last row is the signature of a linear copy that
 * prefetches or rounds up past the final row, not of a buffer that failed to
 * map (that would fault INSIDE the buffer). So the cheap test is to pad, not
 * to shrink: if 64 KiB of slack makes the full-size read clean, every slot
 * just needs padding and nothing else changes. */
#ifndef PAD_BYTES
#define PAD_BYTES (64 * 1024)
#endif
/* Doubler-only mode: exercise the shared-memory import on the 9060 ALONE.
 * No render GPU is touched, so nothing here can wedge the card that actually
 * killed gfx_v12_0. The buffer is filled by the CPU through an mmap of the
 * same memfd, which is legitimate for this test: we are probing whether the
 * doubler can map and read the whole allocation, not benchmarking DMA. */
#ifndef DOUBLER_ONLY
#define DOUBLER_ONLY 0
#endif
/* BUFFER_ONLY: import the udmabuf as a plain VkBuffer and do a pure
 * buffer-to-buffer copy. No VkImage, no tiled layout, nothing to disagree
 * with about row pitch. Isolates "the import maps the wrong pages" from
 * "the linear image layout does not match what the CPU wrote". Implies
 * DOUBLER_ONLY behaviour: only the 9060 is initialised. */
#ifndef BUFFER_ONLY
#define BUFFER_ONLY 0
#endif
#if BUFFER_ONLY && !DOUBLER_ONLY
#undef DOUBLER_ONLY
#define DOUBLER_ONLY 1
#endif
#if !FULLSCALE && (W != 512 || H != 256)
#undef W
#undef H
#define W 512
#define H 256
#endif

static const size_t BYTES = (size_t)W * H * 4;

static const char* vkResultName(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        default: return "VkResult";
    }
}

/// Abort on ANY non-success. Never ignore a VkResult here: a failed
/// vkAllocateMemory followed by a vkBindBufferMemory on the uninitialised
/// VkDeviceMemory is a segfault in the driver, which tells us nothing about
/// why the import failed.
#define CHK(x)                                                            \
    do {                                                                  \
        VkResult r_ = (x);                                                \
        if (r_ != VK_SUCCESS) {                                           \
            std::fprintf(stderr, "%s:%d %s -> %d (%s)\n", __FILE__,      \
                         __LINE__, #x, (int)r_, vkResultName(r_));        \
            std::exit(1);                                                 \
        }                                                                 \
    } while (0)

/// one logical GPU: device, a NON-GRAPHICS transfer queue, a command pool
struct Gpu {
    VkPhysicalDevice pd{};
    VkDevice dev{};
    VkQueue queue{};          // transfer-only (or compute+transfer) family
    uint32_t qfi{};
    VkCommandPool pool{};
    std::string name;

    void init(VkInstance inst, VkPhysicalDevice physical) {
        pd = physical;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        name = props.deviceName;

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());

        // Prefer TRANSFER-only; RADV actually offers COMPUTE|TRANSFER, so the
        // second pass is what matches there. A family with GRAPHICS is never
        // acceptable for a copy (see file header).
        bool found = false;
        for (uint32_t i = 0; i < qn; ++i) {
            const VkQueueFlags f = qs[i].queueFlags;
            if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)
                    && !(f & VK_QUEUE_COMPUTE_BIT)) { qfi = i; found = true; break; }
        }
        if (!found) {
            for (uint32_t i = 0; i < qn; ++i) {
                const VkQueueFlags f = qs[i].queueFlags;
                if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)) {
                    qfi = i; found = true; break;
                }
            }
        }
        if (!found) {
            std::fprintf(stderr,
                "%s: no non-graphics transfer queue family. A copy on a "
                "graphics family is lowered to a draw, not a DMA.\n",
                name.c_str());
            std::exit(4);
        }

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
        // Query what the device actually supports. Requesting an extension
        // that is absent makes vkCreateDevice fail, but assuming a fixed set
        // and never printing it means a later import runs on a device that
        // never got the extension it needs.
        uint32_t en = 0;
        CHK(vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr));
        std::vector<VkExtensionProperties> avail(en);
        CHK(vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, avail.data()));
        // VK_KHR_external_memory is a DEPENDENCY of the other two and must be
        // enabled alongside them, otherwise vkCreateDevice silently returns a
        // device with no external-memory support at all.
        // Enable the full dependency closure. Enabling an extension without
        // the extensions it depends on makes vkCreateDevice return a device
        // that silently lacks the feature - which is exactly how this probe
        // ended up with a working extension list and a broken import.
        const char* want[] = {
            "VK_KHR_external_memory",                 // root: memory handles
            "VK_KHR_external_memory_fd",              // root: fd imports
            "VK_EXT_external_memory_dma_buf",          // root: dma-buf
            "VK_EXT_image_drm_format_modifier",       // root: explicit modifier
            // dependencies of the above, per VUID-vkCreateDevice-...-01387
            "VK_KHR_bind_memory2",
            "VK_KHR_sampler_ycbcr_conversion",
            "VK_KHR_image_format_list",
            "VK_KHR_get_memory_requirements2",
            "VK_KHR_dedicated_allocation",
            "VK_KHR_maintenance1",
            "VK_KHR_maintenance3",
            "VK_KHR_sampler",
        };
        std::vector<const char*> exts;
        for (const char* w : want) {
            bool have = false;
            for (const auto& a : avail)
                if (std::strcmp(a.extensionName, w) == 0) { have = true; break; }
            std::printf("    ext %-38s %s\n", w, have ? "enabled" : "MISSING");
            if (have) exts.push_back(w);
        }
        for (const char* need : { "VK_KHR_external_memory_fd",
                                  "VK_EXT_external_memory_dma_buf",
                                  "VK_EXT_image_drm_format_modifier" }) {
            bool ok = false;
            for (const char* e : exts)
                if (std::strcmp(e, need) == 0) ok = true;
            if (!ok) {
                std::fprintf(stderr, "%s: %s unavailable; cannot import\n",
                    name.c_str(), need);
                std::exit(5);
            }
        }
        VkDeviceCreateInfo dci{ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
            .enabledExtensionCount = (uint32_t)exts.size(),
            .ppEnabledExtensionNames = exts.data() };
        CHK(vkCreateDevice(pd, &dci, nullptr, &dev));
        vkGetDeviceQueue(dev, qfi, 0, &queue);
        VkCommandPoolCreateInfo pci{ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = qfi };
        CHK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
        std::printf("  gpu: %-38s transfer family %u\n", name.c_str(), qfi);
    }
};


/// Ask the driver which (format, usage) combinations it accepts for a LINEAR
/// image that will be backed by a dma-buf. Guessing VK_FORMAT_R8G8B8A8_UNORM
/// with TRANSFER_DST is NOT supported here
/// (VUID-VkImageCreateInfo-pNext-00990), so we query rather than assume.
/// Query dma-buf-capable linear image formats.
///
/// RADV only answers external-memory image queries for
/// VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT with a
/// VkPhysicalDeviceImageDrmFormatModifierInfoEXT naming DRM_FORMAT_MOD_LINEAR.
/// Querying with plain VK_IMAGE_TILING_LINEAR is refused and comes back with
/// compatibleHandleTypes == 0 for every format, which looks exactly like
/// "no format is supported" and is not that at all. Ask the right way.
static VkFormat pickLinearDmaBufFormat(Gpu& g, VkImageUsageFlags usage,
                                       const char* forWhat) {
    const VkFormat cands[] = {
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
    };
    for (VkFormat f : cands) {
        VkPhysicalDeviceExternalImageFormatInfo efi{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };  // 0x200
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT dmi{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = DRM_FORMAT_MOD_LINEAR };
        VkPhysicalDeviceImageFormatInfo2 ici{};
        ici.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        ici.pNext = &dmi;                     // then efi, below
        dmi.pNext = &efi;
        ici.format = f;
        ici.type = VK_IMAGE_TYPE_2D;
        ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ici.usage = usage;
        ici.flags = 0;

        VkExternalImageFormatProperties efp{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
        VkImageFormatProperties2 p2{ .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
            .pNext = &efp };
        const VkResult r = vkGetPhysicalDeviceImageFormatProperties2(g.pd, &ici, &p2);
        const bool dmabuf = (r == VK_SUCCESS) && (efp.externalMemoryProperties.compatibleHandleTypes &
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
        std::printf("    %s %s fmt %-4d usage 0x%x: rc=%-16s handles=0x%x%s\n",
            g.name.c_str(), forWhat, (int)f, usage,
            r == VK_SUCCESS ? "OK" : vkResultName(r),
            r == VK_SUCCESS ? (unsigned)efp.externalMemoryProperties.compatibleHandleTypes : 0u,
            dmabuf ? "  <- dmabuf OK" : "");
        if (dmabuf) {
            return f;
        }
    }
    std::fprintf(stderr, "%s: no linear+dma-buf format for %s (queried with "
            "DRM_FORMAT_MODIFIER_EXT + DRM_FORMAT_MOD_LINEAR)\n",
            g.name.c_str(), forWhat);
    std::exit(6);
}

/// import a dma-buf as a LINEAR 2D image on one device, reporting whether the
/// image's memory requirement fits the backing allocation
static VkImage importDmabuf(Gpu& g, int fd, VkFormat fmt, VkImageUsageFlags usage,
                            VkDeviceSize backing) {
    // Create the image exactly as the capability query described it:
    // DRM_FORMAT_MODIFIER_EXT tiling with an explicit DRM_FORMAT_MOD_LINEAR
    // modifier. Asking about VK_IMAGE_TILING_LINEAR and then creating with it
    // is not the same query, and the driver rejects the mismatch.
    VkImageDrmFormatModifierExplicitCreateInfoEXT drm{
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = DRM_FORMAT_MOD_LINEAR };
    VkSubresourceLayout lay{};
    drm.pPlaneLayouts = &lay;
    drm.pNext = nullptr;
    VkImageCreateInfo ii{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &drm, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    // vkCreateImage pNext: declare the external handle type the image will be
    // bound to. VkMemoryDedicatedAllocateInfo is an ALLOCATE-time struct and is
    // not legal in this chain (VUID-VkImageCreateInfo-pNext-pNext).
    VkExternalMemoryImageCreateInfo em{ .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    ii.pNext = &em;

    VkImage img{};
    CHK(vkCreateImage(g.dev, &ii, nullptr, &img));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g.dev, img, &req);
    if (req.size > backing) {
        std::printf("    %s: image needs %llu but udmabuf backs %llu"
                    " -> OVERRUN by %lld\n",
                    g.name.c_str(), (unsigned long long)req.size,
                    (unsigned long long)backing,
                    (long long)(req.size - (VkDeviceSize)backing));
        VkImageSubresource sub{ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .arrayLayer = 0 };
        VkSubresourceLayout sl{};
        vkGetImageSubresourceLayout(g.dev, img, &sub, &sl);
        std::printf("    %s: SUBRESOURCE offset=%llu rowPitch=%llu size=%llu"
                    " (bufferRowPitch would be %d)\n",
            g.name.c_str(), (unsigned long long)sl.offset,
            (unsigned long long)sl.rowPitch, (unsigned long long)sl.size, W * 4);
    } else {
        std::printf("    %s: image %llu fits udmabuf %llu (slack %lld)\n",
            g.name.c_str(), (unsigned long long)req.size,
            (unsigned long long)backing,
            (long long)((VkDeviceSize)backing - req.size));
    }

    // vkGetMemoryFdPropertiesKHR is not exported by the loader; it must come
    // from vkGetDeviceProcAddr (lsfg-vk-common does the same).
    using FdPropsFn = VkResult(VKAPI_PTR*)(VkDevice,
        VkExternalMemoryHandleTypeFlagBits, int, VkMemoryFdPropertiesKHR*);
    auto getFdProps = (FdPropsFn)
        vkGetDeviceProcAddr(g.dev, "vkGetMemoryFdPropertiesKHR");
    if (!getFdProps) { std::fprintf(stderr, "no vkGetMemoryFdPropertiesKHR\n"); std::exit(3); }
    VkMemoryFdPropertiesKHR fp{ .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    CHK(getFdProps(g.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp));

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g.pd, &mp);
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(req.memoryTypeBits & (1u << i))) continue;
        if (!(fp.memoryTypeBits & (1u << i))) continue;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            mti = i; break;
        }
    }
    if (mti == UINT32_MAX) { std::fprintf(stderr, "no host-visible type\n"); std::exit(2); }

    VkImportMemoryFdInfoKHR imp2{ .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd };
    VkMemoryDedicatedAllocateInfo ded2{ .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &imp2, .image = img };   // allocate-time chain: dedicated -> import
    VkMemoryAllocateInfo mai{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &ded2, .allocationSize = req.size, .memoryTypeIndex = mti };
    VkDeviceMemory mem{};
    CHK(vkAllocateMemory(g.dev, &mai, nullptr, &mem));
    CHK(vkBindImageMemory(g.dev, img, mem, 0));
    return img;
}

/// device-local staging image. hostVisible selects a mappable type, which is
/// how we get a known pattern in (CPU fills it, GPU copies it out) and how we
/// read the result back.
static VkImage allocLocal(Gpu& g, VkImageUsageFlags usage, VkDeviceMemory* memOut,
                          bool hostVisible) {
    VkImageCreateInfo ii{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img{};
    CHK(vkCreateImage(g.dev, &ii, nullptr, &img));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g.dev, img, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g.pd, &mp);
    const VkMemoryPropertyFlags need = hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & need) == need) { mti = i; break; }
    if (mti == UINT32_MAX) { std::fprintf(stderr, "no suitable memory type\n"); std::exit(2); }
    VkMemoryDedicatedAllocateInfo ded{ .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = img };
    VkMemoryAllocateInfo mai{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &ded, .allocationSize = req.size, .memoryTypeIndex = mti };
    CHK(vkAllocateMemory(g.dev, &mai, nullptr, memOut));
    CHK(vkBindImageMemory(g.dev, img, *memOut, 0));
    return img;
}

static VkCommandBuffer beginOneShot(Gpu& g) {
    VkCommandBufferAllocateInfo ai{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb{};
    CHK(vkAllocateCommandBuffers(g.dev, &ai, &cb));
    VkCommandBufferBeginInfo bi{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

static void submitWait(Gpu& g, VkCommandBuffer cb) {
    CHK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence{};
    CHK(vkCreateFence(g.dev, &fci, nullptr, &fence));
    VkSubmitInfo si{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHK(vkQueueSubmit(g.queue, 1, &si, fence));
    CHK(vkWaitForFences(g.dev, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(g.dev, fence, nullptr);
}


/* ---------------------------------------------------------------------------
 * BUFFER-ONLY mode: no image anywhere in the path.
 *
 * The image path reports a mismatch from byte 512 of row 0, which means the
 * GPU is not reading the pages the CPU wrote. Two causes fit that: the linear
 * image layout does not match what the CPU laid down, or the udmabuf import
 * lands on entirely different pages. A pure buffer-to-buffer copy has no
 * tiled layout to disagree about, so it separates the two.
 */
#if BUFFER_ONLY
static VkBuffer makeBuffer(Gpu& g, VkDeviceSize bytes, VkDeviceMemory* memOut,
                           bool hostVisible, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bi{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf{};
    CHK(vkCreateBuffer(g.dev, &bi, nullptr, &buf));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g.dev, buf, &req);
    std::printf("    buffer req: size=%llu (offered %llu, %s)\n",
        (unsigned long long)req.size, (unsigned long long)bytes,
        req.size > bytes ? "SHORT" : "ok");
    if (req.size > bytes)
        std::printf("    >>> buffer needs %llu > %llu: OVERRUN\n",
            (unsigned long long)req.size, (unsigned long long)bytes);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g.pd, &mp);
    const VkMemoryPropertyFlags need = hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & need) == need) { mti = i; break; }
    if (mti == UINT32_MAX) { std::fprintf(stderr, "no suitable memory type\n"); std::exit(2); }
    VkMemoryAllocateInfo mai{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = mti };
    CHK(vkAllocateMemory(g.dev, &mai, nullptr, memOut));
    CHK(vkBindBufferMemory(g.dev, buf, *memOut, 0));
    return buf;
}

/// import the udmabuf as a plain VkBuffer on one device
static VkBuffer importDmabufBuffer(Gpu& g, int fd, VkDeviceSize bytes,
                                   VkDeviceMemory* memOut) {
    // The buffer must declare the external handle types it will be bound to,
    // or the import is rejected at bind time
    // (VUID-vkBindBufferMemory-memory-02985).
    VkExternalMemoryBufferCreateInfo em{ .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkBufferCreateInfo bi{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &em, .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf{};
    CHK(vkCreateBuffer(g.dev, &bi, nullptr, &buf));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g.dev, buf, &req);
    std::printf("    %s buffer import: req=%llu backing=%llu (%s)\n",
        g.name.c_str(), (unsigned long long)req.size, (unsigned long long)bytes,
        req.size > bytes ? "SHORT" : "ok");
    if (req.size > bytes)
        std::printf("    >>> import needs %llu > %llu: OVERRUN\n",
            (unsigned long long)req.size, (unsigned long long)bytes);

    using FdPropsFn = VkResult(VKAPI_PTR*)(VkDevice,
        VkExternalMemoryHandleTypeFlagBits, int, VkMemoryFdPropertiesKHR*);
    auto getFdProps = (FdPropsFn)
        vkGetDeviceProcAddr(g.dev, "vkGetMemoryFdPropertiesKHR");
    if (!getFdProps) { std::fprintf(stderr, "no vkGetMemoryFdPropertiesKHR\n"); std::exit(3); }
    VkMemoryFdPropertiesKHR fp{ .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    CHK(getFdProps(g.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp));

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g.pd, &mp);
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(req.memoryTypeBits & (1u << i))) continue;
        if (!(fp.memoryTypeBits & (1u << i))) continue;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) { mti = i; break; }
    }
    if (mti == UINT32_MAX) { std::fprintf(stderr, "no host-visible type\n"); std::exit(2); }

    VkImportMemoryFdInfoKHR imp{ .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd };
    // NOTE: no VkMemoryDedicatedAllocateInfo here. Dedicated imports are an
    // IMAGE feature; chaining it onto a buffer import segfaults RADV inside
    // vkBindBufferMemory. The import chain is just VkMemoryAllocateInfo.
    VkMemoryAllocateInfo mai{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &imp, .allocationSize = req.size, .memoryTypeIndex = mti };
    // Allocate directly into *memOut and bind *that. Declaring a separate
    // `mem` here and binding it left the buffer bound to VK_NULL_HANDLE, which
    // vkBindBufferMemory accepted and the driver later segfaulted on.
    CHK(vkAllocateMemory(g.dev, &mai, nullptr, memOut));
    CHK(vkBindBufferMemory(g.dev, buf, *memOut, 0));
    return buf;
}
#endif

/// one ring slot: a udmabuf plus per-GPU images over it
struct Slot {
    int memfd{-1};
    int dmafd{-1};
    VkImage imgRender{VK_NULL_HANDLE};   // render writes here
    VkImage imgDoubler{VK_NULL_HANDLE};  // doubler reads from here
    VkImage renderLocal{VK_NULL_HANDLE};
    VkDeviceMemory renderLocalMem{VK_NULL_HANDLE};
    VkImage doublerLocal{VK_NULL_HANDLE};
    VkDeviceMemory doublerLocalMem{VK_NULL_HANDLE};
#if BUFFER_ONLY
    VkBuffer sharedBuf{VK_NULL_HANDLE};      // udmabuf imported as a buffer
    VkDeviceMemory sharedBufMem{VK_NULL_HANDLE};
    VkBuffer dstBuf{VK_NULL_HANDLE};         // device-local destination
    VkDeviceMemory dstBufMem{VK_NULL_HANDLE};
#endif
};

static void makeSlot(Slot& sl, Gpu& render, Gpu& doubler, size_t size) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t imgSize = (BYTES + page - 1) & ~(page - 1);   // image only
#if !BUFFER_ONLY
    // Query a supported linear+dma-buf format per role rather than assuming
    // R8G8B8A8_UNORM works for both.
    const VkFormat fmtDst = DOUBLER_ONLY ? VK_FORMAT_R8G8B8A8_UNORM
        : pickLinearDmaBufFormat(render, VK_IMAGE_USAGE_TRANSFER_DST_BIT, "render dst");
    const VkFormat fmtSrc = pickLinearDmaBufFormat(doubler, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "doubler src");
#endif
    // udmabuf requires the backing memfd to be sealable and sealed
    // F_SEAL_SHRINK, and it must NOT carry F_SEAL_WRITE.
    sl.memfd = (int)syscall(SYS_memfd_create, "udma-ring",
                            MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (sl.memfd < 0 || ::ftruncate(sl.memfd, (off_t)size) != 0) {
        std::perror("memfd_create/ftruncate"); std::exit(1);
    }
    if (::fcntl(sl.memfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
        std::perror("F_ADD_SEALS(F_SEAL_SHRINK)"); std::exit(1);
    }
    int dev = ::open("/dev/udmabuf", O_RDWR);
    if (dev < 0) { std::perror("open /dev/udmabuf"); std::exit(1); }
    udmabuf_create c{};
    c.memfd = (uint32_t)sl.memfd;
    c.flags = UDMABUF_FLAGS_CLOEXEC;
    c.offset = 0;                 // must be page-aligned
    c.size = size;                // must be a whole number of pages
    sl.dmafd = ::ioctl(dev, UDMABUF_CREATE, &c);   // RETURN VALUE is the fd
    ::close(dev);
    if (sl.dmafd < 0) { std::perror("UDMABUF_CREATE"); std::exit(1); }

#if BUFFER_ONLY
    sl.sharedBuf = importDmabufBuffer(doubler, ::dup(sl.dmafd), size, &sl.sharedBufMem);
    sl.dstBuf = makeBuffer(doubler, imgSize, &sl.dstBufMem, true,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
#elif DOUBLER_ONLY
    sl.imgDoubler = importDmabuf(doubler, ::dup(sl.dmafd), fmtSrc,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT, size);
    sl.doublerLocal = allocLocal(doubler,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        &sl.doublerLocalMem, true);
#else
    sl.imgRender = importDmabuf(render, ::dup(sl.dmafd), fmtDst,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT, size);
    sl.imgDoubler = importDmabuf(doubler, ::dup(sl.dmafd), fmtSrc,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT, size);
    sl.renderLocal = allocLocal(render,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        &sl.renderLocalMem, true);
#endif
    sl.doublerLocal = allocLocal(doubler,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        &sl.doublerLocalMem, true);
}


#if DOUBLER_ONLY
/// Fill the shared buffer through an mmap of the same memfd the udmabuf
/// backs. This is CPU setup, not the transport under test: we want the
/// doubler's READ path exercised over a known pattern, and we do not want
/// the render GPU anywhere near this run.
static void cpuFillShared(Slot& sl, size_t imgSize, uint32_t frame) {
    void* m = ::mmap(nullptr, imgSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                     sl.memfd, 0);
    if (m == MAP_FAILED) { std::perror("mmap shared"); std::exit(1); }
    auto* px = (uint8_t*)m;
    for (size_t i = 0; i < imgSize; ++i) px[i] = (uint8_t)((i + frame) & 0xFF);
    char tag[16];
    std::snprintf(tag, sizeof tag, "F%07u", frame);
    std::memcpy(px, tag, 8);
    ::munmap(m, imgSize);
}
#endif

/// render GPU: CPU fills a pattern, then DMA it into the shared slot
static void renderFill(Gpu& render, Slot& sl, uint32_t frame) {
    {
        void* lp{};
        CHK(vkMapMemory(render.dev, sl.renderLocalMem, 0, VK_WHOLE_SIZE, 0, &lp));
        auto* px = (uint8_t*)lp;
        for (size_t i = 0; i < BYTES; ++i) px[i] = (uint8_t)((i + frame) & 0xFF);
        char tag[16];
        std::snprintf(tag, sizeof tag, "F%07u", frame);
        std::memcpy(px, tag, 8);            // header, excluded from the compare
        vkUnmapMemory(render.dev, sl.renderLocalMem);
    }
    VkCommandBuffer cb = beginOneShot(render);
    VkImageMemoryBarrier pre[2]{};
    pre[0] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = sl.renderLocal, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    pre[1] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = sl.imgRender, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, pre);
    VkImageCopy r{};
    r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.extent = { W, H, 1 };
    vkCmdCopyImage(cb, sl.renderLocal, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   sl.imgRender, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    // leave the shared image in TRANSFER_SRC so the doubler can import it that
    // way; this is the only ordering between the two GPUs and it is the
    // caller's job to have waited on the render card's completion signal
    VkImageMemoryBarrier post[1]{};
    post[0] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = sl.imgRender, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, post);
    submitWait(render, cb);
}

#if BUFFER_ONLY
/// pure buffer-to-buffer copy out of the shared udmabuf
static void bufferDrain(Gpu& doubler, Slot& sl, VkDeviceSize imgSize) {
    VkCommandBuffer cb = beginOneShot(doubler);
    VkBufferMemoryBarrier pre{ .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = sl.sharedBuf, .offset = 0, .size = imgSize };
    const VkMemoryBarrier noMB[1]{};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, noMB, 1, &pre, 0, nullptr);
    const VkBufferCopy region{ 0, 0, imgSize };
    vkCmdCopyBuffer(cb, sl.sharedBuf, sl.dstBuf, 1, &region);
    VkBufferMemoryBarrier post{ .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = sl.dstBuf, .offset = 0, .size = imgSize };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, noMB, 1, &post, 0, nullptr);
    submitWait(doubler, cb);
}

static bool bufferVerify(Gpu& doubler, Slot& sl, VkDeviceSize imgSize,
                         uint32_t frame, size_t* badOut) {
    void* lp{};
    CHK(vkMapMemory(doubler.dev, sl.dstBufMem, 0, VK_WHOLE_SIZE, 0, &lp));
    const auto* got = (const uint8_t*)lp;
    char tag[16];
    std::snprintf(tag, sizeof tag, "F%07u", frame);
    const bool tagOk = std::memcmp(got, tag, 8) == 0;
    size_t bad = 0, firstBad = (size_t)-1;
    for (size_t i = 8; i < imgSize; ++i) {
        if (got[i] != (uint8_t)((i + frame) & 0xFF)) {
            if (firstBad == (size_t)-1) firstBad = i;
            ++bad;
        }
    }
    vkUnmapMemory(doubler.dev, sl.dstBufMem);
    if (firstBad != (size_t)-1)
        std::printf("    first mismatch at byte %zu (%.3f MiB in)\n",
            firstBad, firstBad / 1048576.0);
    *badOut = bad;
    return tagOk && bad == 0;
}
#endif

/// doubler GPU: DMA the shared slot into its own device-local image
static void doublerDrain(Gpu& doubler, Slot& sl) {
    VkCommandBuffer cb = beginOneShot(doubler);
    VkImageMemoryBarrier pre[1]{};
    pre[0] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = sl.imgDoubler, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, pre);
    VkImageCopy r{};
    r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.extent = { W, H, 1 };
    vkCmdCopyImage(cb, sl.imgDoubler, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   sl.doublerLocal, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    VkImageMemoryBarrier post[1]{};
    post[0] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = sl.doublerLocal, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, nullptr, 0, nullptr, 1, post);
    submitWait(doubler, cb);
}

static bool verify(Gpu& doubler, Slot& sl, uint32_t frame, size_t* badOut) {
    void* lp{};
    CHK(vkMapMemory(doubler.dev, sl.doublerLocalMem, 0, VK_WHOLE_SIZE, 0, &lp));
    const auto* got = (const uint8_t*)lp;
    char tag[16];
    std::snprintf(tag, sizeof tag, "F%07u", frame);
    const bool tagOk = std::memcmp(got, tag, 8) == 0;
    size_t bad = 0, firstBad = (size_t)-1;
    for (size_t i = 8; i < BYTES; ++i) {   // image region only; padding is slack
        if (got[i] != (uint8_t)((i + frame) & 0xFF)) {
            if (firstBad == (size_t)-1) firstBad = i;
            ++bad;
        }
    }
    vkUnmapMemory(doubler.dev, sl.doublerLocalMem);
    if (firstBad != (size_t)-1) {
        std::printf("    first mismatch at byte %zu (row %zu of %u, row stride %d)\n",
            firstBad, firstBad / (size_t)(W * 4), H, W * 4);
    }
    *badOut = bad;
    return tagOk && bad == 0;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("udmabuf ring: %dx%d (%.2f MiB/slot), %d slots, %d iters\n",
        W, H, BYTES / 1048576.0, SLOTS, ITERS);
#if BUFFER_ONLY
    std::printf("BUFFER-ONLY: udmabuf as VkBuffer, pure buffer-to-buffer copy, "
                "no VkImage in the path\n");
#endif
#if !FULLSCALE && !DOUBLER_ONLY
    std::printf("NOTE: reduced to 512x256 because full scale has hard-faulted "
                "the render GPU. Rebuild with -DFULLSCALE=1 to try it.\n");
#endif

    // VK_KHR_external_memory_capabilities is an INSTANCE extension and is
    // required to enable VK_KHR_external_memory on the device. Without it in
    // ppEnabledExtensionNames here, vkCreateDevice returns a device with no
    // external-memory support and every later import fails.
    uint32_t ien = 0;
    CHK(vkEnumerateInstanceExtensionProperties(nullptr, &ien, nullptr));
    std::vector<VkExtensionProperties> iavail(ien);
    CHK(vkEnumerateInstanceExtensionProperties(nullptr, &ien, iavail.data()));
    // Full dependency closure. Each of these has required extensions of its
    // own, and enabling one without its dependency makes vkCreateInstance /
    // vkCreateDevice return a device with no external-memory support.
    const char* iwantAll[] = { "VK_KHR_external_memory_capabilities",
                               "VK_KHR_get_physical_device_properties2" };
    std::vector<const char*> iexts;
    for (const char* w : iwantAll) {
        bool have = false;
        for (const auto& a : iavail)
            if (std::strcmp(a.extensionName, w) == 0) { have = true; break; }
        std::printf("  inst ext %-42s %s\n", w, have ? "available" : "MISSING");
        if (have) iexts.push_back(w);
    }
    if (iexts.empty()) { std::fprintf(stderr, "no instance ext available\n"); return 5; }
    VkInstanceCreateInfo ici{ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = (uint32_t)iexts.size(),
        .ppEnabledExtensionNames = iexts.data() };
    VkInstance inst{};
    CHK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 0;
    CHK(vkEnumeratePhysicalDevices(inst, &n, nullptr));
    std::vector<VkPhysicalDevice> pds(n);
    CHK(vkEnumeratePhysicalDevices(inst, &n, pds.data()));

    // Identify the two Navi parts by NAME. Never by index: index 0 is the
    // Intel iGPU on this box and a silent fallback to it produced a whole
    // round of wrong conclusions.
    int ri = -1, di = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (ri < 0 && std::string(p.deviceName).find("9070") != std::string::npos) ri = (int)i;
        if (di < 0 && std::string(p.deviceName).find("9060") != std::string::npos) di = (int)i;
    }
    if (ri < 0 || di < 0) {
        for (uint32_t i = 0; i < n; ++i) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(pds[i], &p);
            std::fprintf(stderr, "  device %u = %s\n", i, p.deviceName);
        }
        std::fprintf(stderr, "need both a 9070 (render) and a 9060 (doubler)\n");
        return 1;
    }
    std::printf("  render = 9070 (index %d), doubler = 9060 (index %d)\n", ri, di);

    Gpu render, doubler;
#if DOUBLER_ONLY
    std::printf("DOUBLER-ONLY: the render GPU is not initialised and not touched\n");
#else
    render.init(inst, pds[ri]);
#endif
    doubler.init(inst, pds[di]);

    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t imgSize = (BYTES + page - 1) & ~(page - 1);
    // The image is bound at offset 0 and occupies imgSize bytes; the rest is
    // slack so an SDMA read that runs past the last row lands in padding
    // rather than off the end of the mapping.
    const size_t size = imgSize + ((size_t)PAD_BYTES + page - 1) & ~(page - 1);
    std::printf("slot allocation: %zu bytes = image %zu + pad %zu (%.2f MiB, %zu pages)\n",
        size, imgSize, size - imgSize, size / 1048576.0, size / page);

    std::vector<Slot> slots(SLOTS);
    for (int s = 0; s < SLOTS; ++s) {
        std::printf("slot %d:\n", s);
        makeSlot(slots[s], render, doubler, size);
    }

    size_t fails = 0;
    for (int it = 0; it < ITERS; ++it) {
        Slot& sl = slots[it % SLOTS];
        const uint32_t f = (uint32_t)it;
#if DOUBLER_ONLY
        cpuFillShared(sl, imgSize, f);
#else
        renderFill(render, sl, f);
#endif
        size_t bad = 0;
#if BUFFER_ONLY
        bufferDrain(doubler, sl, imgSize);
        const bool ok = bufferVerify(doubler, sl, imgSize, f, &bad);
#else
        doublerDrain(doubler, sl);
        const bool ok = verify(doubler, sl, f, &bad);
#endif
        if (!ok) {
            ++fails;
            std::printf("iter %d slot %d: MISMATCH (%zu bytes)\n", it, it % SLOTS, bad);
            if (fails >= 3) { std::printf("stopping after 3 failures\n"); break; }
        } else if (it % 50 == 0 || it == ITERS - 1) {
            std::printf("iter %d slot %d: ok\n", it, it % SLOTS);
        }
    }

    std::printf("\n%d iterations, %zu failures\n", ITERS, fails);
    if (fails) { std::printf("RESULT: FAIL\n"); return 4; }
    std::printf("RESULT: PASS - %d x %d render->sysmem->doubler, no p2p, no CPU copy\n",
        W, H);
    return 0;
}
