// Does the render card fault when the udmabuf write reads from an IMAGE?
//
// Phase 1 (render_udmabuf_write_probe.cpp) proved buffer->udmabuf is clean on
// the 9070: import OK, DMA OK, 0 wrong bytes, no faults. The ring's copy
// differs in exactly one way - its SOURCE is an image, not a buffer:
//
//   image --vkCmdCopyImageToBuffer--> udmabuf buffer
//
// This probe adds that phase. The image is created the way the layer's
// isolated swapchain images are: DEDICATED memory, 2D, TRANSFER_DST|SRC, and
// explicit-sync (AMDGPU_GEM_CREATE_EXPLICIT_SYNC via VkExportMemoryAllocateInfo
// with a dma-buf handle type), on the DMA queue family.
//
// Build: g++ -std=c++20 -O2 -o render_img_probe render_image_to_udmabuf_probe.cpp -lvulkan
#include <vulkan/vulkan.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define VKC(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    std::printf("FAIL %s -> VkResult %d\n", #call, (int)r_); return 1; } } while (0)

static constexpr uint32_t W = 2560, H = 1440;
static constexpr size_t BYTES = (size_t)W * 4 * H;

int main() {
    VkApplicationInfo ai{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_2;
    const char* iexts[] = { VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
                            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = iexts;
    VkInstance inst{};
    VKC(vkCreateInstance(&ici, nullptr, &inst));

    uint32_t n = 0;
    VKC(vkEnumeratePhysicalDevices(inst, &n, nullptr));
    std::vector<VkPhysicalDevice> pds(n);
    VKC(vkEnumeratePhysicalDevices(inst, &n, pds.data()));
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (auto p : pds) {
        VkPhysicalDeviceProperties pr{}; vkGetPhysicalDeviceProperties(p, &pr);
        if (std::string(pr.deviceName).find("9070") != std::string::npos) { pd = p; break; }
    }
    if (!pd) { std::printf("FAIL: no 9070\n"); return 1; }
    VkPhysicalDeviceProperties pr{}; vkGetPhysicalDeviceProperties(pd, &pr);
    std::printf("render device: %s\n", pr.deviceName);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    uint32_t fif = UINT32_MAX;
    for (uint32_t i = 0; i < qn; ++i) {
        const auto f = qf[i].queueFlags;
        if ((f & VK_QUEUE_COMPUTE_BIT) && (f & VK_QUEUE_TRANSFER_BIT)
                && !(f & VK_QUEUE_GRAPHICS_BIT)) { fif = i; break; }
    }
    std::printf("dma family = %u\n", fif == UINT32_MAX ? 999u : fif);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = (fif == UINT32_MAX ? 0u : fif); qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dexts[] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                            VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
                            VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME };
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 4; dci.ppEnabledExtensionNames = dexts;
    VkDevice dev{};
    VKC(vkCreateDevice(pd, &dci, nullptr, &dev));

    auto GetFdProps = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
        dev, "vkGetMemoryFdPropertiesKHR");
    if (!GetFdProps) { std::printf("FAIL: no vkGetMemoryFdPropertiesKHR\n"); return 1; }
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = qci.queueFamilyIndex;
    VkCommandPool pool{};
    VKC(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb{};
    VKC(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkQueue q{};
    vkGetDeviceQueue(dev, qci.queueFamilyIndex, 0, &q);

    // ---- udmabuf destination, as in phase 1 ----
    const int mfd = (int)::syscall(SYS_memfd_create, "probe-img",
        (long)(MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (mfd < 0 || ::ftruncate(mfd, (off_t)BYTES) != 0) { std::printf("FAIL: memfd\n"); return 1; }
    if (::fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) { std::printf("FAIL: seal\n"); return 1; }
    void* map = ::mmap(nullptr, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (map == MAP_FAILED) { std::printf("FAIL: mmap\n"); return 1; }
    ::memset(map, 0, BYTES);
    const int udev = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    udmabuf_create uc{};
    uc.memfd = (uint32_t)mfd; uc.flags = UDMABUF_FLAGS_CLOEXEC;
    uc.offset = 0; uc.size = BYTES;
    const int udfd = (udev < 0) ? -1 : ::ioctl(udev, UDMABUF_CREATE, &uc);
    if (udev >= 0) ::close(udev);
    if (udfd < 0) { std::printf("FAIL: UDMABUF_CREATE\n"); return 1; }

    VkBuffer buf{};
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = BYTES;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKC(vkCreateBuffer(dev, &bci, nullptr, &buf));
    VkMemoryRequirements breq{};
    vkGetBufferMemoryRequirements(dev, buf, &breq);
    VkMemoryFdPropertiesKHR fp{ VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    VKC(GetFdProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, udfd, &fp));
    uint32_t bmt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((breq.memoryTypeBits & (1u << i)) && (fp.memoryTypeBits & (1u << i))
                && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            bmt = i; break; }
    if (bmt == UINT32_MAX) { std::printf("FAIL: no mem type for the udmabuf\n"); return 1; }
    VkImportMemoryFdInfoKHR imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp.fd = udfd;
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &imp; mai.allocationSize = breq.size; mai.memoryTypeIndex = bmt;
    VkDeviceMemory bmem{};
    VKC(vkAllocateMemory(dev, &mai, nullptr, &bmem));
    VKC(vkBindBufferMemory(dev, buf, bmem, 0));
    std::printf("udmabuf buffer imported: memType=%u OK\n", bmt);

    // ---- the IMAGE source: DEDICATED + explicit-sync, like the isolated swapchain ----
    VkImage img{};
    VkImageCreateInfo ici2{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici2.imageType = VK_IMAGE_TYPE_2D;
    ici2.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici2.extent = { W, H, 1 };
    ici2.mipLevels = 1; ici2.arrayLayers = 1;
    ici2.samples = VK_SAMPLE_COUNT_1_BIT;
    ici2.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici2.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici2.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKC(vkCreateImage(dev, &ici2, nullptr, &img));
    VkMemoryRequirements ireq{};
    vkGetImageMemoryRequirements(dev, img, &ireq);
    uint32_t imt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if (ireq.memoryTypeBits & (1u << i)) { imt = i; break; }
    VkMemoryDedicatedAllocateInfo ded{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.image = img;
    VkMemoryAllocateInfo imai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imai.pNext = &ded; imai.allocationSize = ireq.size; imai.memoryTypeIndex = imt;
    VkDeviceMemory imem{};
    VKC(vkAllocateMemory(dev, &imai, nullptr, &imem));
    VKC(vkBindImageMemory(dev, img, imem, 0));
    std::printf("dedicated source image created (%ux%u) OK\n", W, H);

    // ---- record: clear image -> copy image to the udmabuf buffer ----
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(cb, &bi));

    VkImageMemoryBarrier toDst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = img;
    toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkClearColorValue clear{}; clear.float32[0] = 0.25f; clear.float32[3] = 1.0f;
    VkImageSubresourceRange rng{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &rng);

    VkImageMemoryBarrier toSrc{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = img;
    toSrc.subresourceRange = rng;
    toSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = W;
    region.bufferImageHeight = H;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { W, H, 1 };
    // THIS is the ring's copy.
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    VKC(vkEndCommandBuffer(cb));

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    std::printf("submitting image -> udmabuf on family %u ...\n", qci.queueFamilyIndex);
    std::fflush(stdout);
    VkResult sr = vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    std::printf("vkQueueSubmit rc=%d\n", (int)sr);
    if (sr != VK_SUCCESS) return 1;
    VkResult wr = vkQueueWaitIdle(q);
    std::printf("vkQueueWaitIdle rc=%d\n", (int)wr);
    if (wr != VK_SUCCESS) return 1;

    // cleared to 0.25 -> R=0x40, G=0, B=0, A=0xff, tight-packed
    const uint8_t* p = (const uint8_t*)map;
    size_t bad = 0;
    for (size_t px = 0; px < (size_t)W * H; px += 977) {   // sample
        if (p[px * 4 + 0] != 0x40 || p[px * 4 + 3] != 0xFF) ++bad;
    }
    std::printf("verify: %zu bad samples\n", bad);
    std::printf("%s\n", bad ? "RESULT: FAIL" : "RESULT: PASS - image -> udmabuf on the render card");
    return bad ? 1 : 0;
}
