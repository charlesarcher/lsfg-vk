// Does RADV accept a HOST-POINTER import of a udmabuf mmap?
// If yes, Udmabuf can follow the decoupled design: memfd + UDMABUF_CREATE,
// both processes mmap it and wrap their own mapping in VkDeviceMemory with
// VK_EXTERNAL_MEMORY_HOST_BIT, with no vkImportMemoryFdKHR anywhere.
// Run: udmabuf_hostptr_probe
#include <vulkan/vulkan.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <unistd.h>
#include <linux/udmabuf.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main() {
    const size_t bytes = 16u << 20;
    const int memfd = static_cast<int>(::syscall(SYS_memfd_create, "lsfg-probe",
        static_cast<long>(MFD_CLOEXEC | MFD_ALLOW_SEALING)));
    if (memfd < 0 || ::ftruncate(memfd, bytes) != 0) { std::perror("memfd"); return 1; }
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (p == MAP_FAILED) { std::perror("mmap"); return 3; }
    void* pMemfd = p;   // the shared memory itself; udmabuf only wraps these pages
    if (::fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) { std::perror("F_SEAL_SHRINK"); return 2; }
    const int udev = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udev < 0) { std::perror("open /dev/udmabuf"); return 2; }
    struct udmabuf_create uc {};
    uc.memfd = static_cast<uint32_t>(memfd);
    uc.flags = UDMABUF_FLAGS_CLOEXEC;
    uc.offset = 0;
    uc.size = bytes;
    const int udfd = ::ioctl(udev, UDMABUF_CREATE, &uc);
    ::close(udev);
    if (udfd < 0) { std::perror("UDMABUF_CREATE"); return 2; }
    std::printf("udmabuf created: udfd=%d bytes=%zu\n", udfd, bytes);

    VkApplicationInfo ai{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    VkInstance inst{};
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { std::puts("no instance"); return 4; }
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
    VkPhysicalDevice* pds = (VkPhysicalDevice*)std::calloc(n, sizeof *pds);
    vkEnumeratePhysicalDevices(inst, &n, pds);
    std::printf("physical devices: %u\n", n); std::fflush(stdout);
    // Vulkan promoted this to a Device+handleType entry point:
    // vkGetMemoryHostPointerPropertiesEXT(VkDevice, handleType, pHostPointer, out)
    using GetHostPtrPropsFn = VkResult(VKAPI_PTR*)(VkDevice,
        VkExternalMemoryHandleTypeFlagBits, const void*,
        VkMemoryHostPointerPropertiesEXT*);
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(pds[i], &mp);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
            if (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) { type = t; break; }
        std::printf("dev%u: enter loop\n", i); std::fflush(stdout);
        if (type == UINT32_MAX) { std::printf("dev%u: no HOST_VISIBLE type\n", i); continue; }
        uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, nullptr);
        VkQueueFamilyProperties* qs = (VkQueueFamilyProperties*)std::calloc(qn, sizeof *qs);
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, qs);
        uint32_t qf = 0; for (; qf < qn; ++qf) if (qs[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
        std::printf("dev%u: qn=%u qf=%u type=%u\n", i, qn, qf, type); std::fflush(stdout);
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qci.queueFamilyIndex = qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        VkPhysicalDeviceFeatures2 feat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(pds[i], &props);
        const char* exts[] = { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME };
        VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        dci.pNext = &feat; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = exts;
        VkDevice dev{};
        const VkResult dr = vkCreateDevice(pds[i], &dci, nullptr, &dev);
        std::printf("dev%u %s: vkCreateDevice=%d\n", i, props.deviceName, (int)dr); std::fflush(stdout);
        if (dr != VK_SUCCESS) continue;
        // DESIGN VARIANT A (what the design means): the MEMFD is the shared
        // memory. Both processes mmap the memfd directly and import THAT
        // pointer as host memory. The udmabuf only wraps the same pages.
        auto tryImport = [&](const char* what, void* hostPtr) {
            auto GetProps = reinterpret_cast<GetHostPtrPropsFn>(
                vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT"));
            if (!GetProps) { std::printf("  %-22s procaddr=NULL\n", what); return; }
            VkMemoryHostPointerPropertiesEXT hpp{ VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
            const VkResult hpr = GetProps(dev,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, hostPtr, &hpp);
            std::printf("  %-22s GetMemoryHostPointerProperties VkResult=%d typeBits=0x%x\n",
                what, (int)hpr, hpp.memoryTypeBits);
            if (hpr != VK_SUCCESS || hpp.memoryTypeBits == 0) return;
            VkImportMemoryHostPointerInfoEXT imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
            imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            imp.pHostPointer = hostPtr;
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.pNext = &imp; mai.allocationSize = bytes; mai.memoryTypeIndex = type;
            VkDeviceMemory mem{};
            const VkResult r = vkAllocateMemory(dev, &mai, nullptr, &mem);
            if (r != VK_SUCCESS) {
                std::printf("  %-22s vkAllocateMemory FAILED VkResult=%d (%s)\n", what,
                    (int)r, r == VK_ERROR_INVALID_EXTERNAL_HANDLE ? "INVALID_EXTERNAL_HANDLE" : "other");
                return;
            }
            void* mapped = nullptr;
            const VkResult mr = vkMapMemory(dev, mem, 0, bytes, 0, &mapped);
            std::printf("  %-22s vkAllocateMemory OK  map=%d ptrsMatch=%d\n", what,
                (int)mr, (int)(mapped == hostPtr));
            if (mr == VK_SUCCESS) vkUnmapMemory(dev, mem);
            vkFreeMemory(dev, mem, nullptr);
        };
        tryImport("memfd-mapping(before)", pMemfd);
        if (udfd >= 0) {
            void* pUdma = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, udfd, 0);
            std::printf("  udmabuf mmap=%s\n", pUdma == MAP_FAILED ? "FAILED" : "ok");
            if (pUdma != MAP_FAILED) {
                tryImport("udmabuf-mapping", pUdma);
                tryImport("memfd-mapping(after)", pMemfd);
                ::munmap(pUdma, bytes);
            }
        }
        vkDestroyDevice(dev, nullptr);
    }
    ::munmap(p, bytes); ::close(udfd); ::close(memfd);
    std::fflush(stdout);
    return 0;
}