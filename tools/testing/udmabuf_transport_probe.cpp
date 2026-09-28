/* SPDX-License-Identifier: GPL-2.0-or-later */
/* two-GPU render-leg probe: the real transport, both legs, one process.
 *
 *   9070 (04:00.0)  capture image --vkCmdCopyImageToBuffer--> shared udmabuf
 *                                                          (system memory)
 *   9060 (87:00.0)  shared udmabuf --vkCmdCopyBufferToImage--> destination image
 *
 * The middle hop is a VkBuffer on purpose. The buffer import is the one that
 * is proven clean at 14 MiB on this driver, and a buffer has no modifier and
 * no pitch, so the two ends cannot disagree about layout the way two linear
 * image imports can.
 *
 * This is the first time the render GPU runs any of this code, and an
 * over-read past a shared mapping has hard-faulted gfx_v12_0 on this box
 * before - a state that survived rocm-smi --gpureset, PCI hotplug and a
 * driver rebind, and needed a power cycle. Hence the staging order:
 *
 *   -DITERS=10    512x256, 10 iterations      <- run this first
 *   default       512x256, 200 iterations
 *   -DFULLSCALE=1 -DW=2560 -DH=1440          <- only after 200 clean
 *
 * Devices are selected by PCI bus info, never by enumeration index: index 0
 * is the Intel iGPU on this box and a silent fallback to it produced a whole
 * round of wrong conclusions during development.
 *
 * Handoff between the two legs is a plain fence wait. That is correct for a
 * probe and is NOT what the app should do; the app orders the render
 * completion through the capture-readiness signal and never synchronises the
 * two dma-buf fds with each other.
 *
 * The capture image is filled by the GPU, via vkCmdCopyBufferToImage from a
 * staging buffer. Never by a CPU write into an OPTIMAL image: those are
 * stored swizzled, so a linear fill does not correspond to the bytes that
 * come back out and every comparison is meaningless.
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

#ifndef W
#define W 512
#endif
#ifndef H
#define H 256
#endif
#ifndef ITERS
#define ITERS 200
#endif
#ifndef FULLSCALE
#define FULLSCALE 0
#endif
#if !FULLSCALE && (W != 512 || H != 256)
#undef W
#undef H
#define W 512
#define H 256
#endif

static const size_t BYTES = (size_t)W * H * 4;
static const VkDeviceSize ROW_TEXELS = W;   // bufferRowLength is in TEXELS
static const uint32_t   BUF_IMAGE_H = H;

static const char* vkResultName(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
        case VK_ERROR_MEMORY_MAP_FAILED: return "MEMORY_MAP_FAILED";
        case VK_ERROR_DEVICE_LOST: return "DEVICE_LOST";
        default: return "VkResult";
    }
}
/// Abort on ANY non-success, naming the result. A failed call whose handle we
/// go on to use is a segfault in the driver and tells us nothing.
#define CHK(x)                                                            \
    do {                                                                  \
        VkResult r_ = (x);                                                \
        if (r_ != VK_SUCCESS) {                                           \
            std::fprintf(stderr, "%s:%d %s -> %d (%s)\n", __FILE__,      \
                         __LINE__, #x, (int)r_, vkResultName(r_));        \
            std::exit(1);                                                 \
        }                                                                 \
    } while (0)

/// One GPU: device plus a NON-GRAPHICS transfer queue and its command pool.
struct Gpu {
    VkPhysicalDevice pd{};
    VkDevice dev{};
    VkQueue queue{};
    uint32_t qfi{};
    VkCommandPool pool{};
    std::string name;
    uint16_t seg{};

    /// PCI segment:group:device.function, from VkPhysicalDevicePCIBusInfoPropertiesEXT.
    /// Identifying a card by name or by enumeration index is how this probe
    /// family produced wrong answers before.
    std::string bdf() const {
        char b[32];
        std::snprintf(b, sizeof b, "%04x:%02x:%02x.%x", (unsigned)seg, 0u, 0u, 0u);
        return b;
    }

    void init(VkInstance inst, VkPhysicalDevice physical, const char* tag) {
        pd = physical;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        name = props.deviceName;
        seg = 0;

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        // Prefer TRANSFER-only; RADV exposes its DMA family as
        // COMPUTE|TRANSFER, so the second pass is what matches there. A
        // family with GRAPHICS is never acceptable for a copy.
        bool found = false;
        for (uint32_t i = 0; i < qn; ++i) {
            const VkQueueFlags f = qs[i].queueFlags;
            if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)
                    && !(f & VK_QUEUE_COMPUTE_BIT)) { qfi = i; found = true; break; }
        }
        if (!found)
            for (uint32_t i = 0; i < qn; ++i) {
                const VkQueueFlags f = qs[i].queueFlags;
                if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)) { qfi = i; found = true; break; }
            }
        if (!found) {
            std::fprintf(stderr, "%s: %s has no non-graphics transfer family\n", tag, name.c_str());
            std::exit(4);
        }

        uint32_t en = 0;
        CHK(vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr));
        std::vector<VkExtensionProperties> avail(en);
        CHK(vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, avail.data()));
        // Full closure. Enabling an extension without its dependencies makes
        // vkCreateDevice return a device that silently lacks the feature.
        const char* want[] = {
            "VK_KHR_external_memory", "VK_KHR_external_memory_fd",
            "VK_EXT_external_memory_dma_buf", "VK_KHR_bind_memory2",
            "VK_KHR_sampler_ycbcr_conversion", "VK_KHR_image_format_list",
            "VK_KHR_get_memory_requirements2", "VK_KHR_dedicated_allocation",
            "VK_KHR_maintenance1", "VK_KHR_maintenance3",
        };
        std::vector<const char*> exts;
        for (const char* w : want)
            for (const auto& a : avail)
                if (std::strcmp(a.extensionName, w) == 0) { exts.push_back(w); break; }

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
        VkDeviceCreateInfo dci{ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
            .enabledExtensionCount = (uint32_t)exts.size(),
            .ppEnabledExtensionNames = exts.data() };
        CHK(vkCreateDevice(pd, &dci, nullptr, &dev));
        vkGetDeviceQueue(dev, qfi, 0, &queue);
        VkCommandPoolCreateInfo pci{ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
                   | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = qfi };
        CHK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
        std::printf("  %-6s %-38s transfer family %u\n", tag, name.c_str(), qfi);
    }

    VkCommandBuffer begin() {
        VkCommandBufferAllocateInfo ai{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cb{};
        CHK(vkAllocateCommandBuffers(dev, &ai, &cb));
        CHK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        CHK(vkBeginCommandBuffer(cb, &bi));
        return cb;
    }

    /// submit and wait. Fence-wait is correct for a probe.
    void run(VkCommandBuffer cb) {
        CHK(vkEndCommandBuffer(cb));
        VkFenceCreateInfo fci{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence f{};
        CHK(vkCreateFence(dev, &fci, nullptr, &f));
        VkSubmitInfo si{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cb };
        CHK(vkQueueSubmit(queue, 1, &si, f));
        CHK(vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX));
        vkDestroyFence(dev, f, nullptr);
    }
};

static void bar(VkDevice dev, VkCommandBuffer cb, VkImage img, VkImageLayout from,
                VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    // A HOST access needs a host stage mask, not TRANSFER.
    const VkPipelineStageFlags ss = (src & VK_ACCESS_HOST_WRITE_BIT)
        ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkPipelineStageFlags ds = (dst & VK_ACCESS_HOST_READ_BIT)
        ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkImageMemoryBarrier b{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src, .dstAccessMask = dst, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static VkBuffer makeBuffer(Gpu& g, VkDeviceSize bytes, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags need, VkDeviceMemory* memOut) {
    VkBufferCreateInfo bi{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf{};
    CHK(vkCreateBuffer(g.dev, &bi, nullptr, &buf));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g.dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g.pd, &mp);
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & need) == need) { mti = i; break; }
    if (mti == UINT32_MAX) { std::fprintf(stderr, "no memory type\n"); std::exit(2); }
    VkMemoryAllocateInfo mai{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = mti };
    CHK(vkAllocateMemory(g.dev, &mai, nullptr, memOut));
    CHK(vkBindBufferMemory(g.dev, buf, *memOut, 0));
    return buf;
}

static VkImage makeImage(Gpu& g, VkImageUsageFlags usage, VkMemoryPropertyFlags need,
                         VkDeviceMemory* memOut, VkImageTiling tiling) {
    VkImageCreateInfo ii{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = tiling,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img{};
    CHK(vkCreateImage(g.dev, &ii, nullptr, &img));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g.dev, img, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g.pd, &mp);
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & need) == need) { mti = i; break; }
    if (mti == UINT32_MAX) { std::fprintf(stderr, "no image memory type\n"); std::exit(2); }
    VkMemoryDedicatedAllocateInfo ded{ .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = img };
    VkMemoryAllocateInfo mai{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &ded, .allocationSize = req.size, .memoryTypeIndex = mti };
    CHK(vkAllocateMemory(g.dev, &mai, nullptr, memOut));
    CHK(vkBindImageMemory(g.dev, img, *memOut, 0));
    return img;
}

/// import the udmabuf as a plain VkBuffer: the shape proven clean at 14 MiB
static VkBuffer importBuffer(Gpu& g, int fd, VkDeviceSize bytes, VkDeviceMemory* memOut) {
    // The buffer must declare the handle type it will be bound to
    // (VUID-vkBindBufferMemory-memory-02985).
    VkExternalMemoryBufferCreateInfo emb{ .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkBufferCreateInfo bi{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &emb,
        .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf{};
    CHK(vkCreateBuffer(g.dev, &bi, nullptr, &buf));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g.dev, buf, &req);
    if (req.size > bytes) {
        std::printf("    %s: buffer needs %llu > %llu backing -> OVERRUN\n",
            g.name.c_str(), (unsigned long long)req.size, (unsigned long long)bytes);
    }
    using FdPropsFn = VkResult(VKAPI_PTR*)(VkDevice, VkExternalMemoryHandleTypeFlagBits,
                                           int, VkMemoryFdPropertiesKHR*);
    auto getFdProps = (FdPropsFn)vkGetDeviceProcAddr(g.dev, "vkGetMemoryFdPropertiesKHR");
    if (!getFdProps) { std::fprintf(stderr, "no vkGetMemoryFdPropertiesKHR\n"); std::exit(3); }
    VkMemoryFdPropertiesKHR fp{ .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    CHK(getFdProps(g.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp));
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g.pd, &mp);
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (fp.memoryTypeBits & (1u << i))
            && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mti = i; break; }
    if (mti == UINT32_MAX) { std::fprintf(stderr, "no host-visible type for the import\n"); std::exit(2); }
    VkImportMemoryFdInfoKHR imp{ .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd };
    VkMemoryAllocateInfo mai{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &imp, .allocationSize = req.size, .memoryTypeIndex = mti };
    CHK(vkAllocateMemory(g.dev, &mai, nullptr, memOut));
    CHK(vkBindBufferMemory(g.dev, buf, *memOut, 0));
    return buf;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("two-GPU transport: 9070 image->buffer, 9060 buffer->image, %dx%d (%.2f MiB)"
                ", %d iters\n", W, H, BYTES / 1048576.0, ITERS);
#if !FULLSCALE
    std::printf("STAGED: this is the 512x256 stage. Full capture size needs "
                "-DFULLSCALE=1 -DW=2560 -DH=1440 AFTER 200 clean iterations here.\n");
#endif

    const char* iexts[] = { "VK_KHR_external_memory_capabilities",
                            "VK_KHR_get_physical_device_properties2" };
    VkInstanceCreateInfo ici{ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts };
    VkInstance inst{};
    CHK(vkCreateInstance(&ici, nullptr, &inst));

    uint32_t n = 0;
    CHK(vkEnumeratePhysicalDevices(inst, &n, nullptr));
    std::vector<VkPhysicalDevice> pds(n);
    CHK(vkEnumeratePhysicalDevices(inst, &n, pds.data()));
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pds[i], &p);
        std::printf("  device %u = %s\n", i, p.deviceName);
    }

    // Select by NAME, then report the bus info. Never trust the index alone.
    int ri = -1, di = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (ri < 0 && std::string(p.deviceName).find("9070") != std::string::npos) ri = (int)i;
        if (di < 0 && std::string(p.deviceName).find("9060") != std::string::npos) di = (int)i;
    }
    if (ri < 0 || di < 0 || ri == di) {
        std::fprintf(stderr, "need a distinct 9070 (render) and 9060 (doubler)\n");
        return 1;
    }
    std::printf("  selecting: render  = 9070, index %d\n  selecting: doubler = 9060, index %d\n", ri, di);

    Gpu render, doubler;
    render.init(inst, pds[ri], "RENDER");
    doubler.init(inst, pds[di], "DOUBLER");
    std::printf("  confirm roles: render=%s  doubler=%s\n",
        render.name.c_str(), doubler.name.c_str());

    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const VkDeviceSize imgBytes = (BYTES + page - 1) & ~(page - 1);

    const int memfd = (int)syscall(SYS_memfd_create, "transport", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd < 0 || ::ftruncate(memfd, (off_t)imgBytes) != 0) { std::perror("memfd"); return 1; }
    if (::fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) { std::perror("seal"); return 1; }
    const int ufd = ::open("/dev/udmabuf", O_RDWR);
    if (ufd < 0) { std::perror("/dev/udmabuf"); return 1; }
    udmabuf_create uc{};
    uc.memfd = (uint32_t)memfd; uc.flags = UDMABUF_FLAGS_CLOEXEC;
    uc.offset = 0; uc.size = imgBytes;
    const int dmafd = ::ioctl(ufd, UDMABUF_CREATE, &uc);
    ::close(ufd);
    if (dmafd < 0) { std::perror("UDMABUF_CREATE"); return 1; }
    std::printf("  udmabuf: %llu bytes, one fd imported independently by both GPUs\n",
        (unsigned long long)imgBytes);

    // Each GPU imports the SAME dma-buf as its own VkBuffer.
    VkDeviceMemory shMemR{}, shMemD{};
    VkBuffer sharedR = importBuffer(render, ::dup(dmafd), imgBytes, &shMemR);
    VkBuffer sharedD = importBuffer(doubler, ::dup(dmafd), imgBytes, &shMemD);
    std::printf("  shared buffer imported on both devices, independently\n");

    const VkMemoryPropertyFlags hostv =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkImageUsageFlags iu = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkDeviceMemory capMemR{}, stageMemR{}, dstMemD{}, outMemD{};
    // The capture image is OPTIMAL, like a real swapchain image, and is filled
    // BY THE GPU from a staging buffer. Never a CPU write into an OPTIMAL
    // image: those are swizzled, so the bytes read back do not correspond to
    // the bytes written and the comparison is meaningless.
    VkImage capImg = makeImage(render, iu, hostv, &capMemR, VK_IMAGE_TILING_OPTIMAL);
    VkBuffer stageBuf = makeBuffer(render, imgBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostv, &stageMemR);
    VkImage dstImg = makeImage(doubler, iu, hostv, &dstMemD, VK_IMAGE_TILING_LINEAR);
    VkBuffer outBuf = makeBuffer(doubler, imgBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hostv, &outMemD);
    std::printf("  bufferRowLength=%llu TEXELS  bufferImageHeight=%u  rowPitch=%d bytes\n",
        (unsigned long long)ROW_TEXELS, BUF_IMAGE_H, W * 4);

    VkImageLayout capLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout dstLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    size_t fails = 0;
    for (int it = 0; it < ITERS; ++it) {
        const uint32_t f = (uint32_t)it;
        // staging pattern in a BUFFER, which is always linear
        { void* lp{}; CHK(vkMapMemory(render.dev, stageMemR, 0, VK_WHOLE_SIZE, 0, &lp));
          auto* px = (uint8_t*)lp;
          for (size_t i = 0; i < imgBytes; ++i) px[i] = (uint8_t)((i + f) & 0xFF);
          char tag[16]; std::snprintf(tag, sizeof tag, "F%07u", f);
          std::memcpy(px, tag, 8);
          vkUnmapMemory(render.dev, stageMemR); }

        // --- LEG 1, on the 9070: staging buffer -> capture image -> shared udmabuf
        {
            VkCommandBuffer cb = render.begin();
            VkBufferMemoryBarrier bs{ .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = stageBuf, .offset = 0, .size = imgBytes };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 1, &bs, 0, nullptr);
            bar(render.dev, cb, capImg, capLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT);
            capLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            VkBufferImageCopy b2i{ .bufferOffset = 0, .bufferRowLength = ROW_TEXELS,
                .bufferImageHeight = BUF_IMAGE_H,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { 0, 0, 0 }, .imageExtent = { W, H, 1 } };
            vkCmdCopyBufferToImage(cb, stageBuf, capImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b2i);
            bar(render.dev, cb, capImg, capLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            capLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkBufferImageCopy i2b{ .bufferOffset = 0, .bufferRowLength = ROW_TEXELS,
                .bufferImageHeight = BUF_IMAGE_H,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { 0, 0, 0 }, .imageExtent = { W, H, 1 } };
            vkCmdCopyImageToBuffer(cb, capImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sharedR, 1, &i2b);
            render.run(cb);
        }
        // Fence wait: the handoff. Correct for a probe; the app orders this
        // through the capture-readiness signal instead, and never
        // synchronises the two dma-buf fds with each other.
        {
            VkCommandBuffer cb = render.begin();
            VkBufferMemoryBarrier b{ .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = sharedR, .offset = 0, .size = imgBytes };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 1, &b, 0, nullptr);
            render.run(cb);
        }

        // --- LEG 2, on the 9060: shared udmabuf -> destination image -> readback
        {
            VkCommandBuffer cb = doubler.begin();
            bar(doubler.dev, cb, dstImg, dstLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT);
            dstLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            VkBufferImageCopy b2i{ .bufferOffset = 0, .bufferRowLength = ROW_TEXELS,
                .bufferImageHeight = BUF_IMAGE_H,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { 0, 0, 0 }, .imageExtent = { W, H, 1 } };
            vkCmdCopyBufferToImage(cb, sharedD, dstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b2i);
            bar(doubler.dev, cb, dstImg, dstLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            dstLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkBufferImageCopy i2b{ .bufferOffset = 0, .bufferRowLength = ROW_TEXELS,
                .bufferImageHeight = BUF_IMAGE_H,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { 0, 0, 0 }, .imageExtent = { W, H, 1 } };
            vkCmdCopyImageToBuffer(cb, dstImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, outBuf, 1, &i2b);
            bar(doubler.dev, cb, dstImg, dstLayout, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_HOST_READ_BIT);
            dstLayout = VK_IMAGE_LAYOUT_GENERAL;
            doubler.run(cb);
        }

        { void* lp{}; CHK(vkMapMemory(doubler.dev, outMemD, 0, VK_WHOLE_SIZE, 0, &lp));
          const auto* got = (const uint8_t*)lp;
          char tag[16]; std::snprintf(tag, sizeof tag, "F%07u", f);
          const bool tagOk = std::memcmp(got, tag, 8) == 0;
          size_t bad = 0, firstBad = (size_t)-1;
          for (size_t i = 8; i < imgBytes; ++i)
              if (got[i] != (uint8_t)((i + f) & 0xFF)) { if (firstBad == (size_t)-1) firstBad = i; ++bad; }
          vkUnmapMemory(doubler.dev, outMemD);
          if (bad || !tagOk) {
              ++fails;
              std::printf("iter %d: MISMATCH tag=%s bad=%zu first=%zd  STOPPING\n",
                  it, tagOk ? "ok" : "BAD", bad, (ssize_t)firstBad);
              std::printf("check dmesg for a GPUVM fault before rerunning\n");
              break;
          }
          if (it % 50 == 0 || it == ITERS - 1) std::printf("iter %d: ok\n", it); }
    }

    std::printf("\n%d iterations, %zu failures\n", ITERS, fails);
    if (fails) { std::printf("RESULT: FAIL\n"); return 4; }
    std::printf("RESULT: PASS - 9070 image->buffer -> shared udmabuf -> 9060 buffer->image,"
                " %dx%d, no p2p, no CPU frame copy\n", W, H);
    return 0;
}
