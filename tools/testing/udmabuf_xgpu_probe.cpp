/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Step 2: THE GATE. One udmabuf, imported on BOTH GPUs.
 *   render (9070) DMAs a known pattern into the shared buffer
 *   doubler (9060) DMAs the buffer into its own device-local image
 *   read back on the CPU and verify the pattern survived
 *
 * No p2p is requested or used. Both sides are ordinary DMA to/from
 * system memory, which is the design: render -> sysmem -> doubler.
 *
 * Throwaway probe; nothing in the lsfg-vk tree is touched.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

static const uint32_t W = 512, H = 256;
static const size_t BYTES = (size_t)W * H * 4;

#define CHK(x)                                                            \
    do {                                                                  \
        VkResult r_ = (x);                                                \
        if (r_ != VK_SUCCESS) {                                           \
            std::fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__,   \
                         #x, (int)r_);                                   \
            std::exit(1);                                                 \
        }                                                                 \
    } while (0)

/// one logical GPU: instance already made, device + queue + cmd pool
struct Gpu {
    VkPhysicalDevice pd{};
    VkDevice dev{};
    VkQueue queue{};
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
        bool found = false;
        for (uint32_t i = 0; i < qn; ++i) {
            if (qs[i].queueFlags & VK_QUEUE_TRANSFER_BIT) { qfi = i; found = true; break; }
        }
        if (!found) { qfi = 0; }
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
        const char* exts[] = { "VK_KHR_external_memory_fd",
                               "VK_EXT_external_memory_dma_buf" };
        VkDeviceCreateInfo dci{ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
            .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts };
        CHK(vkCreateDevice(pd, &dci, nullptr, &dev));
        vkGetDeviceQueue(dev, qfi, 0, &queue);
        VkCommandPoolCreateInfo pci{ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = qfi };
        CHK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
        std::printf("  gpu: %s (queue family %u)\n", name.c_str(), qfi);
    }
};

/// import dma-buf as a LINEAR 2D image on one device
static VkImage importDmabuf(Gpu& g, int fd, VkFormat fmt, VkImageUsageFlags usage,
                            VkDeviceSize* sizeOut) {
    VkImageCreateInfo ii{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImportMemoryFdInfoKHR imp{ .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd };
    VkMemoryDedicatedAllocateInfo ded{ .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &imp, .image = VK_NULL_HANDLE };
    ii.pNext = &ded;

    VkImage img{};
    CHK(vkCreateImage(g.dev, &ii, nullptr, &img));
    ded.image = img;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g.dev, img, &req);
    *sizeOut = req.size;

    auto getFdProps = (PFN_vkGetMemoryFdPropertiesKHR)
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
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) { mti = i; break; }
    }
    if (mti == UINT32_MAX) { std::fprintf(stderr, "no host-visible type\n"); std::exit(2); }

    VkImportMemoryFdInfoKHR imp2{ .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd };
    VkMemoryDedicatedAllocateInfo ded2{ .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &imp2, .image = img };
    VkMemoryAllocateInfo mai{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &ded2, .allocationSize = req.size, .memoryTypeIndex = mti };
    VkDeviceMemory mem{};
    CHK(vkAllocateMemory(g.dev, &mai, nullptr, &mem));
    CHK(vkBindImageMemory(g.dev, img, mem, 0));
    std::printf("  imported on %s: memtype=%u size=%llu\n",
                g.name.c_str(), mti, (unsigned long long)req.size);
    return img;
}

/// allocate a plain device-local image (the doubler-side destination)
static VkImage allocLocal(Gpu& g, VkImageUsageFlags usage, VkDeviceMemory* memOut,
                         bool hostVisible = false) {
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
    uint32_t mti = UINT32_MAX;
    const VkMemoryPropertyFlags need = hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & need) == need) { mti = i; break; }
    if (mti == UINT32_MAX) { std::fprintf(stderr, "no suitable type\n"); std::exit(2); }
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
    VkSubmitInfo si{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
        .pCommandBuffers = &cb };
    CHK(vkQueueSubmit(g.queue, 1, &si, fence));
    CHK(vkWaitForFences(g.dev, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(g.dev, fence, nullptr);
}

int main() {
    std::printf("STEP2: one udmabuf, two GPUs\n");

    // ---- 1. shared buffer -------------------------------------------------
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t size = (BYTES + page - 1) & ~(page - 1);
    int memfd = (int)syscall(SYS_memfd_create, "udma-xgpu",
                             MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd < 0) { std::perror("memfd_create"); return 1; }
    if (::ftruncate(memfd, (off_t)size) != 0) { std::perror("ftruncate"); return 1; }
    if (::fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) { std::perror("seal"); return 1; }
    int dev = ::open("/dev/udmabuf", O_RDWR);
    if (dev < 0) { std::perror("open /dev/udmabuf"); return 1; }
    udmabuf_create c{};
    c.memfd = (uint32_t)memfd; c.flags = UDMABUF_FLAGS_CLOEXEC;
    c.offset = 0; c.size = size;
    int dmafd = ::ioctl(dev, UDMABUF_CREATE, &c);
    if (dmafd < 0) { std::perror("UDMABUF_CREATE"); return 1; }
    ::close(dev);
    std::printf("udmabuf fd=%d size=%zu (%zu x %zu x 4)\n", dmafd, size, W, H);

    // ---- 2. two devices ---------------------------------------------------
    VkInstanceCreateInfo ici{ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst{};
    CHK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 0;
    CHK(vkEnumeratePhysicalDevices(inst, &n, nullptr));
    std::vector<VkPhysicalDevice> pds(n);
    CHK(vkEnumeratePhysicalDevices(inst, &n, pds.data()));
    if (n < 2) { std::fprintf(stderr, "need 2 GPUs, found %u\n", n); return 1; }

    // pick the two AMD parts
    Gpu render, doubler;
    int ri = -1, di = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pds[i], &p);
        const std::string nm = p.deviceName;
        if (ri < 0 && nm.find("9070") != std::string::npos) ri = (int)i;
        if (di < 0 && nm.find("9060") != std::string::npos) di = (int)i;
    }
    if (ri < 0) ri = 0;
    if (di < 0 || di == ri) di = (ri == 0) ? 1 : 0;
    std::printf("render = idx %d, doubler = idx %d\n", ri, di);
    render.init(inst, pds[ri]);
    doubler.init(inst, pds[di]);
    if (render.name == doubler.name)
        { std::fprintf(stderr, "both are '%s' - need distinct GPUs\n", render.name.c_str()); return 1; }

    // ---- 3. import the SAME dma-buf on both -------------------------------
    VkDeviceSize dummy{};
    int fdr = ::dup(dmafd);
    int fdd = ::dup(dmafd);
    VkImage imgRender = importDmabuf(render, fdr, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT, &dummy);
    VkImage imgDoubler = importDmabuf(doubler, fdd, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &dummy);

    // ---- 4. render GPU: fill the shared buffer with a pattern ------------
    //      (via an intermediate device-local image on the render card, then
    //       copy into the shared buffer)
    VkDeviceMemory renderLocalMem{};
    VkImage renderLocal = allocLocal(render,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &renderLocalMem,
        /*hostVisible=*/true);

    // fill renderLocal on the CPU, then copy renderLocal -> shared
    {
        void* lp{};
        CHK(vkMapMemory(render.dev, renderLocalMem, 0, VK_WHOLE_SIZE, 0, &lp));
        auto* px = (uint8_t*)lp;
        for (size_t i = 0; i < BYTES; ++i) px[i] = (uint8_t)(i & 0xFF);
        // a recognisable header so a partial copy is obvious
        const char* tag = "LSFGUDMA";
        std::memcpy(px, tag, 8);
        vkUnmapMemory(render.dev, renderLocalMem);
    }
    {
        VkCommandBuffer cb = beginOneShot(render);
        VkImageMemoryBarrier pre[2]{};
        pre[0] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = renderLocal, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        pre[1] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = imgRender, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, pre);
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.extent = { W, H, 1 };
        vkCmdCopyImage(cb, renderLocal, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       imgRender, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier post[1]{};
        post[0] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = imgRender, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 0, nullptr, 1, post);
        submitWait(render, cb);
    }
    std::printf("RENDER: wrote pattern into shared udmabuf\n");

    // ---- 6. doubler GPU: copy the shared buffer into a local image -------
    VkDeviceMemory doublerLocalMem{};
    VkImage doublerLocal = allocLocal(doubler,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &doublerLocalMem,
        /*hostVisible=*/true);
    {
        VkCommandBuffer cb = beginOneShot(doubler);
        VkImageMemoryBarrier pre[2]{};
        pre[0] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = imgDoubler, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        pre[1] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = doublerLocal, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, pre);
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.extent = { W, H, 1 };
        vkCmdCopyImage(cb, imgDoubler, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       doublerLocal, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier post[1]{};
        post[0] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = doublerLocal, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 0, nullptr, 1, post);
        submitWait(doubler, cb);
    }
    std::printf("DOUBLER: copied shared -> device-local\n");

    // ---- 7. verify the doubler-side copy matches ------------------------
    {
        void* lp{};
        CHK(vkMapMemory(doubler.dev, doublerLocalMem, 0, VK_WHOLE_SIZE, 0, &lp));
        const auto* got = (const uint8_t*)lp;
        const char* tag = "LSFGUDMA";
        bool tagOk = std::memcmp(got, tag, 8) == 0;
        size_t mismatches = 0;
        for (size_t i = 0; i < BYTES; ++i) {
            if (i < 8) continue;                 // tag bytes are deliberate
            if (got[i] != (uint8_t)(i & 0xFF)) ++mismatches;
        }
        std::printf("VERIFY: tag=%s  mismatched bytes=%zu / %zu\n",
                    tagOk ? "OK" : "BAD", mismatches, BYTES);
        vkUnmapMemory(doubler.dev, doublerLocalMem);
        if (!tagOk || mismatches != 0) {
            std::printf("RESULT: FAIL - data did not survive render->sysmem->doubler\n");
            return 4;
        }
    }

    std::printf("RESULT: PASS - one udmabuf, two GPUs, no p2p, pixels intact\n");
    return 0;
}
