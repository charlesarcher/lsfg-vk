// Can the RENDER card (9070) import a udmabuf as a VkBuffer and DMA into it?
//
// This is the operation the ring's producer performs and the one the existing
// ring probe never covered: BUFFER_ONLY forces DOUBLER_ONLY, so that probe only
// ever touched the 9060. The game-side faults we are chasing are on the RENDER
// card, so they were never exercised by it.
//
// No game, no layer, no compositor involvement. Raw Vulkan only.
//
//   render GPU --vkCmdCopyBuffer--> udmabuf (system memory)
//
// Build: g++ -std=c++20 -O2 -o render_buf_probe render_udmabuf_write_probe.cpp -lvulkan
// Run:   ./render_buf_probe
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

#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    std::printf("FAIL %s -> VkResult %d\n", #x, (int)r_); return 1; } } while (0)

static constexpr size_t BYTES = 4u << 20;   // 4 MiB

int main() {
    // ---- instance ----
    VkApplicationInfo ai{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_2;
    const char* iexts[] = { VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
                            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iexts;
    VkInstance inst{};
    VKC(vkCreateInstance(&ici, nullptr, &inst));

    // ---- pick the 9070 by NAME (index 0 is the Intel iGPU on this box) ----
    uint32_t n = 0;
    VKC(vkEnumeratePhysicalDevices(inst, &n, nullptr));
    std::vector<VkPhysicalDevice> pds(n);
    VKC(vkEnumeratePhysicalDevices(inst, &n, pds.data()));
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (auto p : pds) {
        VkPhysicalDeviceProperties pr{};
        vkGetPhysicalDeviceProperties(p, &pr);
        if (std::string(pr.deviceName).find("9070") != std::string::npos) { pd = p; break; }
    }
    if (!pd) { std::printf("FAIL: no 9070 in the enumeration (%u devices)\n", n); return 1; }
    VkPhysicalDeviceProperties pr{};
    vkGetPhysicalDeviceProperties(pd, &pr);
    std::printf("render device: %s\n", pr.deviceName);

    // ---- find the DMA family: probe says it is COMPUTE|TRANSFER, not xfer-only ----
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
    std::printf("dma family (COMPUTE|TRANSFER, no graph) = %u\n",
        fif == UINT32_MAX ? 999u : fif);

    // ---- device: transfer + external memory fd/dma-buf ----
    float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qcis;
    qcis.push_back({ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0,
        fif == UINT32_MAX ? 0u : fif, 1, &prio });
    const char* dexts[] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME };
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = (uint32_t)qcis.size();
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = dexts;
    VkDevice dev{};
    VKC(vkCreateDevice(pd, &dci, nullptr, &dev));

    auto GetFdProps = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
        dev, "vkGetMemoryFdPropertiesKHR");
    if (!GetFdProps) { std::printf("FAIL: no vkGetMemoryFdPropertiesKHR\n"); return 1; }

    // ---- memfd + seal + UDMABUF_CREATE ----
    const int mfd = (int)::syscall(SYS_memfd_create, "probe-slot",
        (long)(MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (mfd < 0) { std::printf("FAIL: memfd_create\n"); return 1; }
    if (::ftruncate(mfd, (off_t)BYTES) != 0) { std::printf("FAIL: ftruncate\n"); return 1; }
    if (::fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
        std::printf("FAIL: F_SEAL_SHRINK\n"); return 1; }
    void* map = ::mmap(nullptr, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (map == MAP_FAILED) { std::printf("FAIL: mmap\n"); return 1; }
    ::memset(map, 0, BYTES);
    const int udev = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udev < 0) { std::printf("FAIL: /dev/udmabuf\n"); return 1; }
    udmabuf_create uc{};
    uc.memfd = (uint32_t)mfd; uc.flags = UDMABUF_FLAGS_CLOEXEC;
    uc.offset = 0; uc.size = BYTES;
    const int udfd = ::ioctl(udev, UDMABUF_CREATE, &uc);
    ::close(udev);
    if (udfd < 0) { std::printf("FAIL: UDMABUF_CREATE\n"); return 1; }
    std::printf("udmabuf: fd=%d bytes=%zu\n", udfd, BYTES);

    // ---- import the udmabuf as a BUFFER on the render card ----
    VkBuffer buf{};
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = BYTES;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKC(vkCreateBuffer(dev, &bci, nullptr, &buf));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryFdPropertiesKHR fp{ VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    VKC(GetFdProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, udfd, &fp));
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) && (fp.memoryTypeBits & (1u << i))
                && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mt = i; break; }
    }
    if (mt == UINT32_MAX) { std::printf("FAIL: no shared memory type for the import\n"); return 1; }
    VkImportMemoryFdInfoKHR imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    imp.fd = udfd;
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &imp; mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    VkDeviceMemory mem{};
    VKC(vkAllocateMemory(dev, &mai, nullptr, &mem));
    VKC(vkBindBufferMemory(dev, buf, mem, 0));
    std::printf("imported udmabuf as VkBuffer: memType=%u OK\n", mt);

    // ---- a device-local source buffer filled from the host ----
    VkBuffer src{}; VkDeviceMemory srcMem{};
    VkBufferCreateInfo sci = bci;
    sci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VKC(vkCreateBuffer(dev, &sci, nullptr, &src));
    VkMemoryRequirements sreq{};
    vkGetBufferMemoryRequirements(dev, src, &sreq);
    // must be HOST_VISIBLE: we fill it from the CPU
    uint32_t smt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((sreq.memoryTypeBits & (1u << i))
                && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            smt = i; break;
        }
    }
    if (smt == UINT32_MAX) { std::printf("FAIL: no host-visible type for the source\n"); return 1; }
    VkMemoryAllocateInfo smai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    smai.allocationSize = sreq.size; smai.memoryTypeIndex = smt;
    VKC(vkAllocateMemory(dev, &smai, nullptr, &srcMem));
    VKC(vkBindBufferMemory(dev, src, srcMem, 0));
    uint8_t* sp = nullptr;
    VKC(vkMapMemory(dev, srcMem, 0, BYTES, 0, (void**)&sp));
    for (size_t i = 0; i < BYTES; ++i) sp[i] = (uint8_t)(i * 7 + 3);
    vkUnmapMemory(dev, srcMem);

    // ---- command pool + queue on the DMA family ----
    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = (fif == UINT32_MAX ? 0u : fif);
    VkCommandPool p{};
    VKC(vkCreateCommandPool(dev, &pci, nullptr, &p));
    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = p; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb{};
    VKC(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkQueue q{};
    vkGetDeviceQueue(dev, pci.queueFamilyIndex, 0, &q);
    std::printf("queue family %u acquired (%p)\n", pci.queueFamilyIndex, (void*)q);

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(cb, &bi));
    VkBufferCopy region{ 0, 0, BYTES };
    // THIS is the operation the ring producer performs.
    vkCmdCopyBuffer(cb, src, buf, 1, &region);
    VKC(vkEndCommandBuffer(cb));

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    std::printf("submitting copy -> udmabuf on family %u ...\n", pci.queueFamilyIndex);
    std::fflush(stdout);
    VkResult sr = vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    std::printf("vkQueueSubmit rc=%d\n", (int)sr);
    if (sr != VK_SUCCESS) return 1;
    VkResult wr = vkQueueWaitIdle(q);
    std::printf("vkQueueWaitIdle rc=%d\n", (int)wr);
    if (wr != VK_SUCCESS) return 1;

    // ---- verify the bytes actually landed in the udmabuf ----
    size_t bad = 0;
    for (size_t i = 0; i < BYTES; ++i)
        if (((uint8_t*)map)[i] != (uint8_t)(i * 7 + 3)) ++bad;
    std::printf("verify: %zu wrong bytes of %zu\n", bad, BYTES);
    std::printf("%s\n", bad ? "RESULT: FAIL" : "RESULT: PASS - render card wrote the udmabuf");
    return bad ? 1 : 0;
}
