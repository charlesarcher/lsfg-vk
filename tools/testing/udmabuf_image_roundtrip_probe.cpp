/* SPDX-License-Identifier: GPL-2.0-or-later */
/* image -> buffer -> image round trip over a udmabuf, on ONE GPU.
 *
 * The production transport needs the doubler leg to be a buffer hop, not an
 * image hop: import the shared slot as a VkBuffer, then convert between the
 * capture image and that buffer with vkCmdCopyImageToBuffer and
 * vkCmdCopyBufferToImage. This probe proves that conversion pattern in
 * isolation, with no second GPU and no layer in the way.
 *
 * Why the middle hop is a buffer:
 *   - the buffer import is the one that is proven clean at 14 MiB on this
 *     driver, while a dma-buf-backed linear image import queries back with
 *     compatibleHandleTypes == 0 for every format
 *   - a buffer has no modifier and no pitch to agree with the other GPU, so
 *     the two ends cannot disagree about layout
 *   - both copy directions are the ordinary AMD transfer (SDMA) path
 *
 * Both ends are explicit: bufferRowLength is in TEXELS (W, not W*4) and
 * bufferImageHeight is the full height. Getting either wrong silently
 * produces plausible-looking garbage, which is what this probe checks for.
 *
 * Default 512x256. FULLSCALE=1 for the real capture size. Doubler-only by
 * construction: the render GPU is never initialised, so this cannot wedge
 * the card that hard-faulted gfx_v12_0.
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
#define DRM_FORMAT_MOD_LINEAR 0x301

static const size_t BYTES = (size_t)W * H * 4;
static const VkDeviceSize ROW_TEXELS = W;      // bufferRowLength: TEXELS
static const uint32_t   BUF_IMAGE_H = H;       // bufferImageHeight: rows

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
#define CHK(x)                                                            \
    do {                                                                  \
        VkResult r_ = (x);                                                \
        if (r_ != VK_SUCCESS) {                                           \
            std::fprintf(stderr, "%s:%d %s -> %d (%s)\n", __FILE__,      \
                         __LINE__, #x, (int)r_, vkResultName(r_));        \
            std::exit(1);                                                 \
        }                                                                 \
    } while (0)

static VkBuffer makeBuffer(VkDevice dev, VkPhysicalDevice pd, VkDeviceSize bytes,
                           VkBufferUsageFlags usage, VkMemoryPropertyFlags need,
                           VkDeviceMemory* memOut) {
    VkBufferCreateInfo bi{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf{};
    CHK(vkCreateBuffer(dev, &bi, nullptr, &buf));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & need) == need) { mti = i; break; }
    if (mti == UINT32_MAX) { std::fprintf(stderr, "no memory type\n"); std::exit(2); }
    VkMemoryAllocateInfo mai{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = mti };
    CHK(vkAllocateMemory(dev, &mai, nullptr, memOut));
    CHK(vkBindBufferMemory(dev, buf, *memOut, 0));
    return buf;
}

static VkImage makeImage(VkDevice dev, VkPhysicalDevice pd, VkImageUsageFlags usage,
                         VkMemoryPropertyFlags need, VkDeviceMemory* memOut,
                         VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL) {
    VkImageCreateInfo ii{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = tiling,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img{};
    CHK(vkCreateImage(dev, &ii, nullptr, &img));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, img, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & need) == need) { mti = i; break; }
    if (mti == UINT32_MAX) { std::fprintf(stderr, "no image memory type\n"); std::exit(2); }
    VkMemoryDedicatedAllocateInfo ded{ .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = img };
    VkMemoryAllocateInfo mai{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &ded, .allocationSize = req.size, .memoryTypeIndex = mti };
    CHK(vkAllocateMemory(dev, &mai, nullptr, memOut));
    CHK(vkBindImageMemory(dev, img, *memOut, 0));
    return img;
}

static void bar(VkDevice dev, VkCommandBuffer cb, VkImage img, VkImageLayout from,
                VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    // A HOST access needs a host stage mask, not TRANSFER
    // (VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02819).
    const VkPipelineStageFlags srcStage = (src & VK_ACCESS_HOST_WRITE_BIT)
        ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkPipelineStageFlags dstStage = (dst & VK_ACCESS_HOST_READ_BIT)
        ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkImageMemoryBarrier b{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src, .dstAccessMask = dst,
        .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("image->buffer->image round trip over a udmabuf: %dx%d"
                " (%.2f MiB), %d iters\n", W, H, BYTES / 1048576.0, ITERS);
#if !FULLSCALE
    std::printf("NOTE: 512x256 default; -DFULLSCALE=1 for the real capture size\n");
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
    int di = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pds[i], &p);
        std::printf("  device %u = %s\n", i, p.deviceName);
        if (std::string(p.deviceName).find("9060") != std::string::npos) di = (int)i;
    }
    if (di < 0) { std::fprintf(stderr, "need the 9060\n"); return 1; }
    const VkPhysicalDevice pd = pds[di];

    // transfer-only (or COMPUTE|TRANSFER) queue: never a graphics family
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < qn; ++i) {
        const VkQueueFlags f = qs[i].queueFlags;
        if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)
                && !(f & VK_QUEUE_COMPUTE_BIT)) { qfi = i; break; }
    }
    if (qfi == UINT32_MAX)
        for (uint32_t i = 0; i < qn; ++i) {
            const VkQueueFlags f = qs[i].queueFlags;
            if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)) { qfi = i; break; }
        }
    if (qfi == UINT32_MAX) { std::fprintf(stderr, "no non-graphics transfer family\n"); return 4; }

    uint32_t en = 0;
    CHK(vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr));
    std::vector<VkExtensionProperties> avail(en);
    CHK(vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, avail.data()));
    const char* want[] = { "VK_KHR_external_memory", "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf", "VK_KHR_bind_memory2",
        "VK_KHR_sampler_ycbcr_conversion", "VK_KHR_image_format_list",
        "VK_KHR_get_memory_requirements2", "VK_KHR_dedicated_allocation",
        "VK_KHR_maintenance1", "VK_KHR_maintenance3" };
    std::vector<const char*> dexts;
    for (const char* w : want) {
        for (const auto& a : avail)
            if (std::strcmp(a.extensionName, w) == 0) { dexts.push_back(w); break; }
    }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci{ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = (uint32_t)dexts.size(),
        .ppEnabledExtensionNames = dexts.data() };
    VkDevice dev{};
    CHK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue{};
    vkGetDeviceQueue(dev, qfi, 0, &queue);
    std::printf("  device created, transfer family %u\n", qfi);
    VkCommandPoolCreateInfo pci{ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        // RESET_COMMAND_BUFFER_BIT is required: we reset and re-record the
        // same buffer every iteration
        // (VUID-vkResetCommandBuffer-commandBuffer-00046).
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
               | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = qfi };
    VkCommandPool pool{};
    CHK(vkCreateCommandPool(dev, &pci, nullptr, &pool));

    // --- the shared slot, as a BUFFER ---
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const VkDeviceSize imgBytes = (BYTES + page - 1) & ~(page - 1);
    const int memfd = (int)syscall(SYS_memfd_create, "roundtrip", MFD_CLOEXEC | MFD_ALLOW_SEALING);
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
    std::printf("  udmabuf: %llu bytes\n", (unsigned long long)imgBytes);

    VkExternalMemoryBufferCreateInfo emb{ .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkBufferCreateInfo sbi{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &emb,
        .size = imgBytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer shared{};
    CHK(vkCreateBuffer(dev, &sbi, nullptr, &shared));
    VkMemoryRequirements sreq{};
    vkGetBufferMemoryRequirements(dev, shared, &sreq);
    using FdPropsFn = VkResult(VKAPI_PTR*)(VkDevice, VkExternalMemoryHandleTypeFlagBits,
                                           int, VkMemoryFdPropertiesKHR*);
    auto getFdProps = (FdPropsFn)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fp{ .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    CHK(getFdProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dmafd, &fp));
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t smti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((sreq.memoryTypeBits & (1u << i)) && (fp.memoryTypeBits & (1u << i))
            && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { smti = i; break; }
    if (smti == UINT32_MAX) { std::fprintf(stderr, "no host-visible type for the import\n"); return 2; }
    VkImportMemoryFdInfoKHR imp{ .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dmafd };
    VkMemoryAllocateInfo smai{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &imp, .allocationSize = sreq.size, .memoryTypeIndex = smti };
    VkDeviceMemory sharedMem{};
    CHK(vkAllocateMemory(dev, &smai, nullptr, &sharedMem));
    CHK(vkBindBufferMemory(dev, shared, sharedMem, 0));
    std::printf("  imported as VkBuffer: req=%llu (ok)\n", (unsigned long long)sreq.size);

    const VkMemoryPropertyFlags hostv =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkImageUsageFlags iu = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkDeviceMemory srcMem{}, dstMem{}, outMem{};
    // SOURCE is LINEAR: this probe CPU-fills it through a linear mmap, and an
    // OPTIMAL image is stored swizzled, so a linear fill would read back in a
    // different order and the byte comparison would be meaningless.
    VkImage srcImg = makeImage(dev, pd, iu, hostv, &srcMem, VK_IMAGE_TILING_LINEAR);
    VkImage dstImg = makeImage(dev, pd, iu, hostv, &dstMem);
    VkBuffer outBuf = makeBuffer(dev, pd, imgBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, hostv, &outMem);
    std::printf("  bufferRowLength=%llu TEXELS  bufferImageHeight=%u  rowPitch=%d bytes\n",
        (unsigned long long)ROW_TEXELS, BUF_IMAGE_H, W * 4);

    // reusable one-shot submit
    VkCommandBufferAllocateInfo cbi{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb{};
    CHK(vkAllocateCommandBuffers(dev, &cbi, &cb));
    VkFenceCreateInfo fci{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence{};
    CHK(vkCreateFence(dev, &fci, nullptr, &fence));

    VkImageLayout srcLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout dstLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    size_t fails = 0;
    for (int it = 0; it < ITERS; ++it) {
        const uint32_t f = (uint32_t)it;
        {   // CPU lays a known pattern in the source image
            void* lp{}; CHK(vkMapMemory(dev, srcMem, 0, VK_WHOLE_SIZE, 0, &lp));
            auto* px = (uint8_t*)lp;
            for (size_t i = 0; i < BYTES; ++i) px[i] = (uint8_t)((i + f) & 0xFF);
            char tag[16]; std::snprintf(tag, sizeof tag, "F%07u", f);
            std::memcpy(px, tag, 8);
            vkUnmapMemory(dev, srcMem);
        }
        CHK(vkResetFences(dev, 1, &fence));
        CHK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo beg{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        CHK(vkBeginCommandBuffer(cb, &beg));

        bar(dev, cb, srcImg, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        srcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkBufferImageCopy i2b{};
        i2b.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        i2b.imageOffset = { 0, 0, 0 };
        i2b.imageExtent = { W, H, 1 };
        i2b.bufferOffset = 0;
        i2b.bufferRowLength = ROW_TEXELS;    // TEXELS, not bytes
        i2b.bufferImageHeight = BUF_IMAGE_H;
        vkCmdCopyImageToBuffer(cb, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               shared, 1, &i2b);

        bar(dev, cb, dstImg, dstLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT);
        dstLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkBufferImageCopy b2i{};
        b2i.bufferOffset = 0;
        b2i.bufferRowLength = ROW_TEXELS;
        b2i.bufferImageHeight = BUF_IMAGE_H;
        b2i.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        b2i.imageOffset = { 0, 0, 0 };
        b2i.imageExtent = { W, H, 1 };
        vkCmdCopyBufferToImage(cb, shared, dstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &b2i);

        bar(dev, cb, dstImg, dstLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        dstLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        // read the RESULT image back out, not the shared buffer we copied
        // through - otherwise this verifies the middle hop and never the
        // buffer->image leg.
        VkBufferImageCopy i2bOut{};
        i2bOut.bufferOffset = 0;
        i2bOut.bufferRowLength = ROW_TEXELS;
        i2bOut.bufferImageHeight = BUF_IMAGE_H;
        i2bOut.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        i2bOut.imageOffset = { 0, 0, 0 };
        i2bOut.imageExtent = { W, H, 1 };
        vkCmdCopyImageToBuffer(cb, dstImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               outBuf, 1, &i2bOut);
        bar(dev, cb, dstImg, dstLayout, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_HOST_READ_BIT);
        dstLayout = VK_IMAGE_LAYOUT_GENERAL;

        CHK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cb };
        CHK(vkQueueSubmit(queue, 1, &si, fence));
        CHK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

        {   // verify byte for byte out of the readback buffer
            void* lp{}; CHK(vkMapMemory(dev, outMem, 0, VK_WHOLE_SIZE, 0, &lp));
            const auto* got = (const uint8_t*)lp;
            char tag[16]; std::snprintf(tag, sizeof tag, "F%07u", f);
            const bool tagOk = std::memcmp(got, tag, 8) == 0;
            size_t bad = 0, firstBad = (size_t)-1;
            for (size_t i = 8; i < imgBytes; ++i)
                if (got[i] != (uint8_t)((i + f) & 0xFF)) {
                    if (firstBad == (size_t)-1) firstBad = i; ++bad;
                }
            vkUnmapMemory(dev, outMem);
            if (bad || !tagOk) {
                ++fails;
                std::printf("iter %d: MISMATCH tag=%s bad=%zu first=%zd\n",
                    it, tagOk ? "ok" : "BAD", bad, (ssize_t)firstBad);
                if (fails >= 3) { std::printf("stopping after 3 failures\n"); break; }
            } else if (it % 50 == 0 || it == ITERS - 1) {
                std::printf("iter %d: ok\n", it);
            }
        }
    }

    std::printf("\n%d iterations, %zu failures\n", ITERS, fails);
    if (fails) { std::printf("RESULT: FAIL\n"); return 4; }
    std::printf("RESULT: PASS - image->buffer->image via a udmabuf VkBuffer,"
                " %dx%d, bufferRowLength=%llu texels\n", W, H,
                (unsigned long long)ROW_TEXELS);
    return 0;
}
