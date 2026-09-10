/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "swapchain.hpp"

#include "lsfg-vk-layer/capture_context.hpp"
#include "lsfg-vk-layer/isolated_swapchain.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/memfd.h>
#include <linux/udmabuf.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>
#include <sys/ioctl.h>
#include <vulkan/vulkan_core.h>

using namespace lsfgvk::layer;
using namespace ls::ipc;

namespace {
bool exportIsolatedOn() {
    const char* e = std::getenv("LSFGVK_EXPORT_ISOLATED");
    return e && e[0] == '1';
}
bool rawDmaBufOn() {
    const char* e = std::getenv("LSFGVK_RAW_DMABUF");
    return e && e[0] == '1';
}
int createRawDmaBuf(size_t bytes, void** mapOut) {
    *mapOut = nullptr;
    const int mfd = static_cast<int>(::syscall(SYS_memfd_create, "lsfg-raw",
        static_cast<long>(MFD_CLOEXEC | MFD_ALLOW_SEALING)));
    if (mfd < 0)
        return -1;
    if (::ftruncate(mfd, static_cast<off_t>(bytes)) != 0) {
        ::close(mfd);
        return -1;
    }
    if (::fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
        ::close(mfd);
        return -1;
    }
    const int ufd = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (ufd < 0) {
        ::close(mfd);
        return -1;
    }
    udmabuf_create cr{};
    cr.memfd = static_cast<__u32>(mfd);
    cr.flags = UDMABUF_FLAGS_CLOEXEC;
    cr.offset = 0;
    cr.size = bytes;
    const int dfd = ::ioctl(ufd, UDMABUF_CREATE, &cr);
    const int err = errno;
    ::close(ufd);
    ::close(mfd);
    if (dfd < 0) {
        errno = err;
        return -1;
    }
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
    if (p == MAP_FAILED) {
        ::close(dfd);
        return -1;
    }
    *mapOut = p;
    return dfd;
}
int existingRenderFd() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (!dir)
        return -1;
    int found = -1;
    while (dirent* e = ::readdir(dir)) {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
        char link[128]{};
        const ssize_t n = ::readlink(path, link, sizeof(link) - 1);
        if (n > 0 && std::strstr(link, "renderD") != nullptr) {
            found = std::atoi(e->d_name);
            if (found > 2)
                break;
        }
    }
    ::closedir(dir);
    return found;
}

int openRenderFdForDevice(const vk::Vulkan& vk) {
    VkPhysicalDeviceDrmPropertiesEXT drmProp{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT
    };
    VkPhysicalDeviceProperties2 props{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &drmProp
    };
    vk.fi().GetPhysicalDeviceProperties2(vk.physdev(), &props);
    if (!drmProp.hasRender)
        throw ls::error("VkPhysicalDeviceDrmPropertiesEXT hasRender=0");
    DIR* dir = ::opendir("/dev/dri");
    if (!dir)
        throw ls::error("opendir /dev/dri failed");
    int out = -1;
    std::string path;
    while (dirent* e = ::readdir(dir)) {
        if (std::strncmp(e->d_name, "renderD", 7) != 0)
            continue;
        path = std::string("/dev/dri/") + e->d_name;
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0)
            continue;
        if (static_cast<int>(gnu_dev_major(st.st_rdev)) == drmProp.renderMajor
                && static_cast<int>(gnu_dev_minor(st.st_rdev)) == drmProp.renderMinor) {
            out = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
            break;
        }
    }
    ::closedir(dir);
    if (out < 0)
        throw ls::error("open matching render node failed maj="
            + std::to_string(drmProp.renderMajor) + " min="
            + std::to_string(drmProp.renderMinor));
    std::cerr << "lsfg-vk: explicit-sync dest node=" << path
        << " maj=" << drmProp.renderMajor
        << " min=" << drmProp.renderMinor << "\n";
    return out;
}

int allocExplicitGttDmaBuf(const vk::Vulkan& vk, uint64_t size) {
    static amdgpu_device_handle adev = nullptr;
    if (!adev) {
        const int dfd = openRenderFdForDevice(vk);
        uint32_t maj = 0, min = 0;
        const int ir = amdgpu_device_initialize(dfd, &maj, &min, &adev);
        if (ir != 0 || !adev) {
            const int e = errno;
            ::close(dfd);
            throw ls::error(std::string("amdgpu_device_initialize ir=")
                + std::to_string(ir) + " errno=" + std::to_string(e));
        }
    }
    amdgpu_bo_alloc_request req{};
    req.alloc_size = size;
    req.phys_alignment = 4096;
    req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
    req.flags = AMDGPU_GEM_CREATE_EXPLICIT_SYNC
        | AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
    amdgpu_bo_handle bo{};
    int r = amdgpu_bo_alloc(adev, &req, &bo);
    if (r != 0)
        throw ls::error("amdgpu_bo_alloc EXPLICIT_SYNC GTT failed");
    struct amdgpu_bo_info binfo{};
    if (amdgpu_bo_query_info(bo, &binfo) == 0) {
        std::cerr << "lsfg-vk: dest alloc_flags=0x" << std::hex
            << binfo.alloc_flags << std::dec
            << " explicit="
            << !!(binfo.alloc_flags & AMDGPU_GEM_CREATE_EXPLICIT_SYNC)
            << "\n";
    }
    uint32_t rawFd = 0;
    r = amdgpu_bo_export(bo, amdgpu_bo_handle_type_dma_buf_fd, &rawFd);
    if (r != 0)
        throw ls::error("amdgpu_bo_export dma-buf failed");
    return static_cast<int>(rawFd);
}

bool gemIsExplicitSync(int dmaFd) {
    const int drm = existingRenderFd();
    if (drm < 0 || dmaFd < 0)
        return false;
    drm_prime_handle ph{};
    ph.fd = dmaFd;
    if (::drmIoctl(drm, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) != 0) {
        std::cerr << "lsfg-vk: dest GEM FD_TO_HANDLE errno=" << errno << "\n";
        return false;
    }
    drm_amdgpu_gem_create_in info{};
    drm_amdgpu_gem_op op{};
    op.handle = ph.handle;
    op.op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO;
    op.value = reinterpret_cast<uint64_t>(&info);
    const int ir = ::drmIoctl(drm, DRM_IOCTL_AMDGPU_GEM_OP, &op);
    const bool expl = (ir == 0)
        && (info.domain_flags & AMDGPU_GEM_CREATE_EXPLICIT_SYNC);
    std::cerr << "lsfg-vk: dest GEM get=" << ir
        << " domains=0x" << std::hex << info.domains
        << " flags=0x" << info.domain_flags << std::dec
        << " explicit=" << expl << "\n";
    return expl;
}
}

struct lsfgvk::layer::CopyHop {
    struct Job {
        void* src{nullptr};
        void* dst{nullptr};
        size_t n{0};
        int fd{-1};
        int destFd{-1};
        int readyFd{-1};
        uint32_t* seq{nullptr};
    };
    std::mutex mx;
    std::condition_variable cv;
    std::queue<Job> q;
    std::atomic<bool> stop{false};
    std::thread th;

    CopyHop() { th = std::thread([this] { this->run(); }); }
    CopyHop(const CopyHop&) = delete;
    CopyHop& operator=(const CopyHop&) = delete;
    ~CopyHop() {
        stop.store(true);
        cv.notify_all();
        if (th.joinable())
            th.join();
        while (!q.empty()) {
            if (q.front().fd >= 0)
                ::close(q.front().fd);
            q.pop();
        }
    }
    void push(Job j) {
        {
            std::lock_guard<std::mutex> lk(mx);
            q.push(j);
        }
        cv.notify_one();
    }
    void run() {
        while (!stop.load()) {
            Job j{};
            {
                std::unique_lock<std::mutex> lk(mx);
                cv.wait(lk, [&] { return stop.load() || !q.empty(); });
                if (stop.load() && q.empty())
                    return;
                if (q.empty())
                    continue;
                j = q.front();
                q.pop();
            }
            if (j.fd >= 0) {
                pollfd pfd{};
                pfd.fd = j.fd;
                pfd.events = POLLIN;
                ::poll(&pfd, 1, 2);
                ::close(j.fd);
                j.fd = -1;
            }
            if (j.destFd >= 0) {
                dma_buf_sync st{};
                st.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
                (void)::ioctl(j.destFd, DMA_BUF_IOCTL_SYNC, &st);
            }
            if (j.src && j.dst && j.n) {
                // Game swapchain is B8G8R8A8_UNORM; FG/overlay want RGBA.
                // copyImage is bit-preserving (R/B swap). Capture queue is
                // compute — cannot vkCmdBlitImage. Swizzle on the hop thread.
                auto* d = static_cast<std::uint32_t*>(j.dst);
                const auto* s = static_cast<const std::uint32_t*>(j.src);
                const size_t px = j.n / 4;
                for (size_t i = 0; i < px; ++i) {
                    const std::uint32_t v = s[i];
                    d[i] = (v & 0xFF00FF00u)
                        | ((v & 0xFFu) << 16)
                        | ((v >> 16) & 0xFFu);
                }
            }
            if (j.destFd >= 0) {
                dma_buf_sync en{};
                en.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                (void)::ioctl(j.destFd, DMA_BUF_IOCTL_SYNC, &en);
            }
            if (j.readyFd >= 0) {
                const uint64_t one = 1;
                (void)::write(j.readyFd, &one, sizeof(one));
            }
            if (j.seq)
                __atomic_add_fetch(j.seq, 1u, __ATOMIC_RELEASE);
        }
    }
};

namespace {
    // TEMP DEBUG (Session 13.29d): per-phase absolute wall-clock timestamps
    // to identify exactly which step in present() blocks the game thread.
    [[maybe_unused]] const bool layerDbg{ std::getenv("LSFGVK_LAYER_DBG") != nullptr };
    using SteadyClock = std::chrono::steady_clock;
    [[maybe_unused]] auto g_layerDbgT0 = SteadyClock::now();
    [[maybe_unused]] long long layerDbgMs(SteadyClock::time_point from) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            SteadyClock::now() - from).count();
    }
    std::string phaseAbsMs() {
        if (!layerDbg) return "";
        using namespace std::chrono;
        auto now = system_clock::now();
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
        char buf[32];
        std::snprintf(buf, sizeof(buf), " @%.3fs", ms / 1000.0);
        return buf;
    }

    VkImageMemoryBarrier barrierHelper(VkImage handle,
            VkAccessFlags srcAccessMask,
            VkAccessFlags dstAccessMask,
            VkImageLayout oldLayout,
            VkImageLayout newLayout) {
        return VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = srcAccessMask,
            .dstAccessMask = dstAccessMask,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = handle,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
    }
}

CaptureContext::CaptureContext(const vk::Vulkan& vk, ls::GameConf profile,
        SwapchainInfo info, const std::string& gameDeviceName)
        : profile(std::move(profile)), info(std::move(info)),
          gameDeviceName(gameDeviceName), fake(this->info.fake), vkPtr(&vk),
          timingRing(vk, "layer-capture") {
    // only constructed for External presentation; caller guards this
    if (this->profile.presentation != ls::Presentation::External)
        throw ls::error("CaptureContext created for non-external presentation");

    // all ring slots start free (the member declaration is value-initialized
    // to all-false; the app has not captured anything yet, so every slot is
    // reusable from frame 1)
    this->slotFree.fill(true);
    this->localExportFds.fill(-1);
    this->rawExportFds.fill(-1);
    this->rawMemFds.fill(-1);
    this->rawReadyFds.fill(-1);
    this->bExportFds.fill(-1);

    // --- IPC handshake (2 s deadline on the NEGOTIATED reply) --------------
    std::filesystem::path sockPath;
    try {
        sockPath = ls::ipc::Listener::defaultPath();
    } catch (const std::exception& e) {
        throw ls::error("lsfg-vk: external presentation active but XDG_RUNTIME_DIR is not set", e);
    }

    try {
        this->ipcConn.emplace(ls::ipc::Connection::connect(sockPath));
    } catch (const std::exception& e) {
        throw ls::error("lsfg-vk: failed to connect to app socket '" + sockPath.string() + "'", e);
    }

    // send HELLO with true game caps
    const auto gameUuid = vk.deviceUUID();
    const auto hello = ls::ipc::makeHello(ls::ipc::PROTO_VERSION, gameUuid,
        gameDeviceName, static_cast<uint32_t>(this->info.format),
        this->info.extent.width, this->info.extent.height);
    try {
        this->ipcConn->send(hello);
    } catch (const std::exception& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error("lsfg-vk: external stream error: failed to send HELLO", e);
    }

    // receive NEGOTIATED with 2 s deadline (poll-based, never hangs)
    ls::ipc::Negotiated negotiated{};
    try {
        auto msg = this->ipcConn->receive(std::chrono::milliseconds(10000));
        if (auto* err = std::get_if<ls::ipc::ErrorMsg>(&msg)) {
            std::cerr << "lsfg-vk: external stream error: peer refused: " << err->message << "\n";
            throw ls::error("lsfg-vk: external stream error: peer refused handshake: " + err->message);
        }
        if (!std::holds_alternative<ls::ipc::Negotiated>(msg)) {
            const auto got = ls::ipc::typeOf(msg);
            throw ls::error(std::string("lsfg-vk: external stream error: expected NEGOTIATED, got ")
                + ls::ipc::nameOf(got));
        }
        negotiated = std::get<ls::ipc::Negotiated>(std::move(msg));
    } catch (const ls::ipc::socket_error& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error("lsfg-vk: external stream error: handshake deadline or socket failure", e);
    } catch (const ls::error& e) {
        // includes our explicit type-mismatch throws above
        if (std::string(e.what()).find("external stream error") != std::string::npos)
            throw;
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error(std::string("lsfg-vk: external stream error: ") + e.what(), e);
    }

    // --- layer-side validation of the negotiated modifier on the GAME device ---
    constexpr VkFormatFeatureFlags2 usageNeeds =
        VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT
        | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT
        | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT
        | VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;

    try {
        const auto gameCaps = vk.exchangeCaps(this->info.format);
        auto it = gameCaps.find(this->info.format);
        if (it == gameCaps.end())
            throw ls::error("game device has no caps for format " + std::to_string(static_cast<int>(this->info.format)));

        const ls::ipc::Negotiated& neg = negotiated;
        const uint64_t mod = neg.modifier;
        bool found = false;
        VkFormatFeatureFlags2 foundBits = 0;
        for (const auto& entry : it->second) {
            if (entry.modifier == mod) {
                found = true;
                foundBits = entry.requiredUsageBits;
                break;
            }
        }
        if (!found)
            throw ls::error("negotiated modifier 0x" + std::to_string(mod) + " not advertised by game device");
        if ((foundBits & usageNeeds) != usageNeeds) {
            const auto missing = usageNeeds & ~foundBits;
            throw ls::error("negotiated modifier 0x" + std::to_string(mod)
                + " lacks required usage bits 0x" + std::to_string(missing));
        }
    } catch (const ls::error& e) {
        if (std::string(e.what()).find("negotiated modifier") != std::string::npos) {
            std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
            throw ls::error(std::string("lsfg-vk: external stream error: ") + e.what(), e);
        }
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw;
    }

    // --- receive 2 staging-image handoffs from the app ----------------------
    // the app owns the staging images (created in its OWN local VRAM on the
    // processing device, exported as dma-buf). importing them TRANSFER_DST-only
    // flips the PCIe traffic direction: the capture blit now WRITES each frame
    // A→B as a sequential DMA transfer (~0.5-1.5 ms at 1440p) instead of the
    // app re-reading A's VRAM as latency-bound texture samples (~13 ms).
    // the layout must match the app's creation layout (NEGOTIATED carries it).
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkExtent2D extent = this->info.extent;

    vk::ImageLayout layout{};
    if (negotiated.modifier == vk::EXCHANGE_MODIFIER_LINEAR) {
        layout.mode = vk::ImageMode::Linear;
        layout.rowPitch = negotiated.rowPitch;
        layout.drmModifier = 0;
    } else {
        layout.mode = vk::ImageMode::DrmModifier;
        layout.drmModifier = negotiated.modifier;
        layout.rowPitch = negotiated.rowPitch;
    }
    this->exchangeLayout = layout;

    try {
        this->stagingImages.reserve(STAGING_RING_DEPTH);
        for (size_t i = 0; i < STAGING_RING_DEPTH; ++i) {
            auto msg = this->ipcConn->receive(std::chrono::milliseconds(10000));
            if (auto* err = std::get_if<ls::ipc::ErrorMsg>(&msg)) {
                std::cerr << "lsfg-vk: external stream error: peer refused: " << err->message << "\n";
                throw ls::error("lsfg-vk: external stream error: peer refused at STAGING: " + err->message);
            }
            if (!std::holds_alternative<ls::ipc::Staging>(msg)) {
                const auto got = ls::ipc::typeOf(msg);
                throw ls::error(std::string("lsfg-vk: external stream error: expected STAGING, got ")
                    + ls::ipc::nameOf(got));
            }
            const int fd = this->ipcConn->takeReceivedFd();
            if (fd < 0)
                throw ls::error("lsfg-vk: external stream error: STAGING arrived without its fd");
            const bool importStaging = (std::getenv("LSFGVK_IMPORT_STAGING")
                    && std::getenv("LSFGVK_IMPORT_STAGING")[0] == '1')
                || (!this->fake && !(std::getenv("LSFGVK_NO_IMPORT")
                    && std::getenv("LSFGVK_NO_IMPORT")[0] == '1'));
            if (!importStaging) {
                static const bool posixShm = std::getenv("LSFGVK_POSIX_SHM") == nullptr
                    || std::getenv("LSFGVK_POSIX_SHM")[0] != '0';
                if (!posixShm) {
                    ::close(fd);
                    continue;
                }
                const VkDeviceSize hostSize = std::max<VkDeviceSize>(
                    negotiated.allocationSize,
                    static_cast<VkDeviceSize>(layout.rowPitch) * extent.height);
                const size_t mapBytes = static_cast<size_t>(hostSize) + 4096;
                void* map = ::mmap(nullptr, mapBytes,
                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                ::close(fd);
                if (map == MAP_FAILED) {
                    std::cerr << "lsfg-vk: posix-shm mmap failed slot " << i
                        << " errno=" << errno << "\n";
                    continue;
                }
                this->shmMaps.at(i) = map;
                this->shmSeq.at(i) = reinterpret_cast<uint32_t*>(
                    static_cast<char*>(map) + static_cast<size_t>(hostSize));
                this->hostAllocSize = hostSize;
                std::cerr << "lsfg-vk: posix-shm mmap slot " << i
                    << " size=" << hostSize << "\n";
                continue;
            }
            // import consumes the fd on success; on failure the image closes it
            this->stagingImages.emplace_back(vk, extent, format,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                fd /*importFd*/, std::nullopt /*exportFd*/, layout);
        }
    } catch (const ls::ipc::socket_error& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error("lsfg-vk: external stream error: STAGING deadline or socket failure", e);
    } catch (const ls::error& e) {
        if (std::string(e.what()).find("external stream error") != std::string::npos)
            throw;
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error(std::string("lsfg-vk: external stream error: ") + e.what(), e);
    }

    // wait for READY (2 s deadline as well)
    try {
        auto msg = this->ipcConn->receive(std::chrono::milliseconds(10000));
        if (auto* err = std::get_if<ls::ipc::ErrorMsg>(&msg)) {
            std::cerr << "lsfg-vk: external stream error: peer refused: " << err->message << "\n";
            throw ls::error("lsfg-vk: external stream error: peer refused at READY: " + err->message);
        }
        if (!std::holds_alternative<ls::ipc::Ready>(msg)) {
            const auto got = ls::ipc::typeOf(msg);
            throw ls::error(std::string("lsfg-vk: external stream error: expected READY, got ")
                + ls::ipc::nameOf(got));
        }
    } catch (const ls::ipc::socket_error& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error("lsfg-vk: external stream error: READY deadline or socket failure", e);
    } catch (const ls::error& e) {
        if (std::string(e.what()).find("external stream error") != std::string::npos)
            throw;
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error(std::string("lsfg-vk: external stream error: ") + e.what(), e);
    }

    // --- capture command buffer + per-slot sync-fd semaphores ---------------
    try {
        // Session 13.17: capture ring - render thread never blocks on the
        // previous blit (fences created signaled so first uses pass freely)
        uint32_t extraFam = 0, extraIdx = 0;
        static const bool noExtraQ = std::getenv("LSFGVK_NO_EXTRA_Q")
            && std::getenv("LSFGVK_NO_EXTRA_Q")[0] == '1';
        if (!noExtraQ && this->fake && getIsolatedSignalQueue(extraFam, extraIdx)) {
            vk.df().GetDeviceQueue(vk.dev(), extraFam, extraIdx, &this->captureQ);
            const VkCommandPoolCreateInfo poolInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex = extraFam
            };
            auto res = vk.df().CreateCommandPool(vk.dev(), &poolInfo, VK_NULL_HANDLE, &this->capturePool);
            if (res != VK_SUCCESS)
                throw ls::vulkan_error(res, "capture command pool failed");
            std::cerr << "lsfg-vk: capture copy on extra queue fam=" << extraFam
                      << " idx=" << extraIdx << "\n";
        }
        const bool importStaging = (std::getenv("LSFGVK_IMPORT_STAGING")
            && std::getenv("LSFGVK_IMPORT_STAGING")[0] == '1');
        this->localCopyOnly = (this->fake && !importStaging)
            || (std::getenv("LSFGVK_LOCAL_COPY")
                && std::getenv("LSFGVK_LOCAL_COPY")[0] == '1')
            || (std::getenv("LSFGVK_NO_IMPORT")
                && std::getenv("LSFGVK_NO_IMPORT")[0] == '1');
        if (this->fake && !exportIsolatedOn()) {
            this->localImages.reserve(STAGING_RING_DEPTH);
            const VkImageUsageFlags localUsage =
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                | VK_IMAGE_USAGE_SAMPLED_BIT;
            this->exchangeLayout.hostVisible = true;
            this->dmaBufSent.fill(false);
            if (this->exchangeLayout.rowPitch == 0)
                this->exchangeLayout.rowPitch =
                    (this->info.extent.width * 4u + 255u) / 256u * 256u;
            const uint64_t gemBytes =
                (static_cast<uint64_t>(this->exchangeLayout.rowPitch)
                    * this->info.extent.height + 4095ull) & ~4095ull;
            for (size_t i = 0; i < STAGING_RING_DEPTH; ++i) {
                int gemFd = -1;
                try {
                    gemFd = allocExplicitGttDmaBuf(vk, gemBytes);
                } catch (const std::exception& e) {
                    std::cerr << "lsfg-vk: explicit-sync dest alloc failed: "
                        << e.what() << "\n";
                }
                if (gemFd >= 0) {
                    (void)gemIsExplicitSync(gemFd);
                    const int imp = ::dup(gemFd);
                    ::close(gemFd);
                    gemFd = -1;
                    if (imp < 0)
                        throw ls::error("dup() failed before dest import");
                    this->localImages.emplace_back(vk, this->info.extent,
                        VK_FORMAT_R8G8B8A8_UNORM, localUsage,
                        imp, std::nullopt, this->exchangeLayout);
                } else {
                    this->localImages.emplace_back(vk, this->info.extent,
                        VK_FORMAT_R8G8B8A8_UNORM, localUsage,
                        std::nullopt, std::nullopt, this->exchangeLayout);
                }
                auto exp = this->localImages.back().exportDmaBuf(vk);
                this->localExportFds.at(i) = exp.fd;
                VkMemoryFdPropertiesKHR fp{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR
                };
                auto pr = vk.df().GetMemoryFdPropertiesKHR(vk.dev(),
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                    exp.fd, &fp);
                std::cerr << "lsfg-vk: render dma-buf slot " << i
                    << " fd=" << exp.fd << " props=" << pr
                    << " types=0x" << std::hex << fp.memoryTypeBits << std::dec
                    << " pitch=" << exp.rowPitch
                    << " size=" << exp.allocationSize << "\n";
            }
            if (this->localCopyOnly)
                std::cerr << "lsfg-vk: capture dst=render-owned dma-buf\n";
            if (rawDmaBufOn() && !this->shmMaps.at(0) && this->rawExportFds.at(0) < 0) {
                this->rawBytes = gemBytes;
                for (size_t i = 0; i < STAGING_RING_DEPTH; ++i) {
                    const int dfd = this->localExportFds.at(i);
                    if (dfd < 0)
                        continue;
                    void* dm = ::mmap(nullptr, static_cast<size_t>(gemBytes),
                        PROT_READ, MAP_SHARED, dfd, 0);
                    if (dm == MAP_FAILED) {
                        std::cerr << "lsfg-vk: dest mmap failed slot " << i
                            << " errno=" << errno << "\n";
                        continue;
                    }
                    this->destMaps.at(i) = dm;
                    void* rm = nullptr;
                    const int rfd = createRawDmaBuf(static_cast<size_t>(gemBytes), &rm);
                    if (rfd < 0) {
                        std::cerr << "lsfg-vk: raw dma-buf create failed slot " << i
                            << " errno=" << errno << "\n";
                        ::munmap(dm, static_cast<size_t>(gemBytes));
                        this->destMaps.at(i) = nullptr;
                        continue;
                    }
                    this->rawExportFds.at(i) = rfd;
                    this->rawMaps.at(i) = rm;
                    this->rawReadyFds.at(i) = ::eventfd(0,
                        EFD_CLOEXEC | EFD_SEMAPHORE | EFD_NONBLOCK);
                    if (this->rawReadyFds.at(i) < 0) {
                        std::cerr << "lsfg-vk: raw ready eventfd failed slot " << i
                            << " errno=" << errno << "\n";
                    }
                }
                if (!this->copyHop)
                    this->copyHop = std::make_unique<CopyHop>();
                std::cerr << "lsfg-vk: raw dma-buf re-export size=" << gemBytes << "\n";
            }
        }
        if (this->fake && exportIsolatedOn()) {
            this->dmaBufSent.fill(false);
            this->localCopyOnly = true;
            std::cerr << "lsfg-vk: capture dst=isolated-image dma-buf (no dest)\n";
        }
        if (this->fake && this->hostImages.empty() && this->shmMaps.at(0)) {
            const VkDeviceSize hostSize = this->hostAllocSize
                ? this->hostAllocSize
                : ((static_cast<VkDeviceSize>(this->info.extent.width) * 4
                    * this->info.extent.height + 4095) & ~4095ull);
            const VkImageUsageFlags hostUsage =
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            this->hostAllocSize = hostSize;
            for (size_t i = 0; i < STAGING_RING_DEPTH; ++i) {
                void* p = nullptr;
                if (::posix_memalign(&p, 4096, static_cast<size_t>(hostSize)) != 0)
                    throw ls::error("posix_memalign 9070 host capture failed");
                std::memset(p, 0, static_cast<size_t>(hostSize));
                this->hostPtrsA.at(i) = p;
                this->hostImages.emplace_back(vk, this->info.extent,
                    VK_FORMAT_R8G8B8A8_UNORM, hostUsage, p, hostSize);
            }
            std::cerr << "lsfg-vk: capture dst=9070-host-malloc size=" << hostSize << "\n";
            this->copyHop = std::make_unique<CopyHop>();
        }
        const bool dualHost = false; // Never create a second VkDevice in game process (crashes vkd3d-proton)
        if (dualHost && this->fake && !this->shmMaps.at(0)) {
            try {
                auto selectB = [](const vk::VulkanInstanceFuncs& fi,
                        const std::vector<VkPhysicalDevice>& devs) -> VkPhysicalDevice {
                    for (auto pd : devs) {
                        VkPhysicalDeviceProperties2 p{
                            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
                        };
                        fi.GetPhysicalDeviceProperties2(pd, &p);
                        if (p.properties.deviceID == 0x7590)
                            return pd;
                    }
                    throw ls::error("dual-host: RX 9060 XT not found");
                };
                this->bVk = std::make_unique<vk::Vulkan>(
                    "lsfg-dual-host", vk::version{2, 0, 0},
                    "lsfg-dual-host", vk::version{2, 0, 0},
                    selectB, true, std::nullopt, std::nullopt, true, false);
                const VkDeviceSize hostSize =
                    (static_cast<VkDeviceSize>(this->info.extent.width) * 4
                        * this->info.extent.height + 4095) & ~4095ull;
                const VkImageUsageFlags hostUsage =
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                this->hostImages.clear();
                this->bHostImages.clear();
                this->bVramImages.clear();
                this->hostAllocSize = hostSize;
                const vk::ImageLayout bLayout{ .mode = vk::ImageMode::Linear };
                const VkImageUsageFlags vramUsage =
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    | VK_IMAGE_USAGE_SAMPLED_BIT;
                for (size_t i = 0; i < STAGING_RING_DEPTH; ++i) {
                    void* p = nullptr;
                    if (::posix_memalign(&p, 4096, static_cast<size_t>(hostSize)) != 0)
                        throw ls::error("posix_memalign dual-host failed");
                    std::memset(p, 0, static_cast<size_t>(hostSize));
                    this->hostPtrsA.at(i) = p;
                    this->hostImages.emplace_back(vk, this->info.extent,
                        VK_FORMAT_R8G8B8A8_UNORM, hostUsage, p, hostSize);
                    this->bHostImages.emplace_back(*this->bVk, this->info.extent,
                        VK_FORMAT_R8G8B8A8_UNORM, hostUsage, p, hostSize);
                    this->bVramImages.emplace_back(*this->bVk, this->info.extent,
                        VK_FORMAT_R8G8B8A8_UNORM, vramUsage,
                        std::nullopt, std::nullopt, bLayout);
                    auto exp = this->bVramImages.back().exportDmaBuf(*this->bVk);
                    this->bExportFds.at(i) = exp.fd;
                }
                for (size_t i = 0; i < STAGING_RING_DEPTH; ++i) {
                    this->bCbs.emplace_back(*this->bVk);
                    this->bFences.emplace_back(*this->bVk, true);
                }
                std::cerr << "lsfg-vk: zero-copy dual-host shared-malloc + 9060 dma-buf size="
                    << hostSize << "\n";
            } catch (const std::exception& e) {
                std::cerr << "lsfg-vk: dual-host failed: " << e.what() << "\n";
                this->hostImages.clear();
                this->bHostImages.clear();
                this->bVramImages.clear();
                this->bVk.reset();
            }
        }
        for (size_t i = 0; i < CAPTURE_RING_DEPTH; ++i) {
            this->captureCommandBuffers.emplace_back(vk, this->capturePool);
            this->captureFences.emplace_back(vk, true);
        }
        this->captureCommandBuffer.emplace(vk);
        this->captureFence.emplace(vk);
        this->captureSemaphores.reserve(STAGING_RING_DEPTH);
        this->presentSemaphores.reserve(STAGING_RING_DEPTH);
        for (size_t i = 0; i < STAGING_RING_DEPTH; ++i) {
            this->captureSemaphores.emplace_back(vk, std::nullopt,
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
            this->presentSemaphores.emplace_back(vk, std::nullopt,
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
        }
        // FRAME sends should not block forever if app stops reading
        this->ipcConn->setSendTimeout(std::chrono::milliseconds(250));
        std::cerr << "lsfg-vk: ipc sockFd=" << this->ipcConn->fd() << "\n";
    } catch (const std::exception& e) {
        throw ls::error("lsfg-vk: failed to create capture semaphores", e);
    }

    std::cerr << "lsfg-vk: external presentation active (game on '" << gameDeviceName << "', app on socket)\n";
}

CaptureContext::CaptureContext(CaptureContext&&) noexcept = default;
CaptureContext& CaptureContext::operator=(CaptureContext&&) noexcept = default;

CaptureContext::~CaptureContext() {
    this->copyHop.reset();
    if (this->vkPtr) {
        try {
            if (this->fenceSubmitted && this->captureFence.has_value())
                (void)this->captureFence->wait(*this->vkPtr, 150ULL * 1000 * 1000);
            for (auto& f : this->captureFences)
                (void)f.wait(*this->vkPtr, 150ULL * 1000 * 1000);
        } catch (...) {
            // teardown must not throw
        }
    }
    for (int& fd : this->localExportFds) {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    for (size_t i = 0; i < this->destMaps.size(); ++i) {
        if (this->destMaps.at(i)) {
            ::munmap(this->destMaps.at(i), static_cast<size_t>(this->rawBytes));
            this->destMaps.at(i) = nullptr;
        }
        if (this->rawMaps.at(i)) {
            ::munmap(this->rawMaps.at(i), static_cast<size_t>(this->rawBytes));
            this->rawMaps.at(i) = nullptr;
        }
    }
    for (int& fd : this->rawExportFds) {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    for (int& fd : this->rawMemFds) {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    for (int& fd : this->rawReadyFds) {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    for (int& fd : this->bExportFds) {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    static auto* leakBCbs = new std::vector<vk::CommandBuffer>;
    static auto* leakBFences = new std::vector<vk::Fence>;
    leakBCbs->insert(leakBCbs->end(),
        std::make_move_iterator(this->bCbs.begin()),
        std::make_move_iterator(this->bCbs.end()));
    this->bCbs.clear();
    leakBFences->insert(leakBFences->end(),
        std::make_move_iterator(this->bFences.begin()),
        std::make_move_iterator(this->bFences.end()));
    this->bFences.clear();
    // RADV FreeMemory on HOST_ALLOCATION_BIT + dual VkDevice teardown
    // aborts FurMark (free(): invalid size). Intentionally never destroy.
    static auto* leakHost = new std::vector<vk::Image>;
    static auto* leakBHost = new std::vector<vk::Image>;
    static auto* leakBVram = new std::vector<vk::Image>;
    static auto* leakBvks = new std::vector<std::unique_ptr<vk::Vulkan>>;
    leakHost->insert(leakHost->end(),
        std::make_move_iterator(this->hostImages.begin()),
        std::make_move_iterator(this->hostImages.end()));
    this->hostImages.clear();
    leakBHost->insert(leakBHost->end(),
        std::make_move_iterator(this->bHostImages.begin()),
        std::make_move_iterator(this->bHostImages.end()));
    this->bHostImages.clear();
    leakBVram->insert(leakBVram->end(),
        std::make_move_iterator(this->bVramImages.begin()),
        std::make_move_iterator(this->bVramImages.end()));
    this->bVramImages.clear();
    if (this->bVk)
        leakBvks->push_back(std::move(this->bVk));
}

int CaptureContext::drainReleases() {
    if (!this->ipcConn.has_value()) return 0;
    // non-blocking drain: poll until no readable data
    int applied = 0;
    while (true) {
        // Inlined ::poll on purpose (not Connection::drained()): GCC 16.2
        // -O2/-O3 dropped the `if (drained) break;` guard when `drained` was a
        // function-call result assigned inside a try/catch (its catch edge
        // leaves the `false` initializer, wrongly generalized to "always
        // false"), so this drain became a blocking receive(nullopt) on an
        // empty socket -> first-present deadlock. A direct poll result feeding
        // a plain branch is immune, and HUP/ERR is separated from "empty"
        // (the old `res == 0` conflated them, misreading a closed peer as data).
        pollfd pfd{};
        pfd.fd = this->ipcConn->fd();
        pfd.events = POLLIN;
        const int res = ::poll(&pfd, 1, 0);
        if (res < 0) {
            if (errno == EINTR) continue;
            break; // poll error: stop draining conservatively
        }
        if (res == 0) break;
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;

        ls::ipc::Message msg;
        try {
            // data is ready; receive without deadline (already poll-proven readable)
            msg = this->ipcConn->receive(std::nullopt);
        } catch (const std::exception& e) {
            std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
            throw ls::error(std::string("lsfg-vk: external stream error: ") + e.what(), e);
        }

        if (auto* rel = std::get_if<ls::ipc::Release>(&msg)) {
            if (rel->stagingIdx < this->slotFree.size())
                this->slotFree.at(rel->stagingIdx) = true;
            applied++;
        } else if (auto* err = std::get_if<ls::ipc::ErrorMsg>(&msg)) {
            std::cerr << "lsfg-vk: external stream error: peer error: " << err->message << "\n";
            throw ls::error("lsfg-vk: external stream error: peer error: " + err->message);
        } else {
            // unexpected message in steady state - treat as stream error
            const auto got = ls::ipc::typeOf(msg);
            std::cerr << "lsfg-vk: external stream error: unexpected " << ls::ipc::nameOf(got) << " in steady state\n";
            throw ls::error(std::string("lsfg-vk: external stream error: unexpected ")
                + ls::ipc::nameOf(got));
        }
    }
    return applied;
}

std::optional<size_t> CaptureContext::trySelectFreeSlot() {
    // never waits on GPU B. one probe; caller skips capture on nullopt.
    for (size_t tries = 0; tries < this->slotFree.size(); ++tries) {
        const size_t idx = (this->nextSlot + tries) % this->slotFree.size();
        if (this->slotFree.at(idx)) {
            this->nextSlot = (idx + 1) % this->slotFree.size();
            return idx;
        }
    }
    return std::nullopt;
}

VkResult CaptureContext::present(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores) {
// RELEASE handling frees slots draining non-blocking
    int entryDrained = 0;
    try {
        entryDrained = this->drainReleases();
    } catch (const ls::error& e) {
        // already logged inside drainReleases; surface as stream error
        throw;
    }

    // TEMP DEBUG (E7): wall-clock entry
    const SteadyClock::time_point dbgEnter = layerDbg ? SteadyClock::now()
                                                       : std::chrono::steady_clock::time_point{};
    auto phaseLog = [this](const char* phase) {
        if (layerDbg)
            std::cerr << "lsfg-vk-layer: [dbg] present: " << phase
                      << " (fidx " << this->fidx << ")" << phaseAbsMs() << "\n";
    };
    phaseLog("enter (post-drain)");
    static const bool emptyCb = std::getenv("LSFGVK_EMPTY_CB")
        && std::getenv("LSFGVK_EMPTY_CB")[0] == '1';
    static const bool copyNoSig = std::getenv("LSFGVK_COPY_NOSIG")
        && std::getenv("LSFGVK_COPY_NOSIG")[0] == '1';
    if ((emptyCb || copyNoSig) && this->fake && !this->captureCommandBuffers.empty()) {
        const auto& cmdbuf = this->captureCommandBuffers.at(0);
        cmdbuf.begin(vk);
        if (copyNoSig && imageIdx < this->info.images.size()
                && !this->stagingImages.empty()) {
            const VkImage srcImage = this->info.images.at(imageIdx);
            const vk::Image& dstImage = this->stagingImages.at(0);
            cmdbuf.copyImage(vk,
                {
                    barrierHelper(srcImage,
                        VK_ACCESS_NONE,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                    barrierHelper(dstImage.handle(),
                        VK_ACCESS_NONE,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                },
                { srcImage, dstImage.handle() },
                dstImage.getExtent(),
                {
                    barrierHelper(srcImage,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_ACCESS_MEMORY_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                });
        }
        cmdbuf.end(vk);
        VkCommandBuffer rawBuf = cmdbuf.raw();
        std::vector<VkPipelineStageFlags> stages(semaphores.size(),
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = static_cast<uint32_t>(semaphores.size()),
            .pWaitSemaphores = semaphores.empty() ? nullptr : semaphores.data(),
            .pWaitDstStageMask = stages.empty() ? nullptr : stages.data(),
            .commandBufferCount = 1,
            .pCommandBuffers = &rawBuf,
        };
        VkQueue q = this->captureQ != VK_NULL_HANDLE ? this->captureQ : queue;
        const auto res = vk.df().QueueSubmit(q, 1, &submit, VK_NULL_HANDLE);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkQueueSubmit() failed");
        phaseLog("empty CB wait-only (no fence)");
        return VK_SUCCESS;
    }
    if (layerDbg && entryDrained > 0)
        std::cerr << "lsfg-vk-layer: [dbg] present: enter drain +" << entryDrained
                  << " free=" << this->freeMask() << " next=" << this->nextSlot
                  << " (fidx " << this->fidx << ")" << phaseAbsMs() << "\n";

    // read back capture-blit timing for frame fidx-4 (GPU has finished it)
    if (this->timingRing.enabled() && this->fidx >= 4) {
        if (auto timing = this->timingRing.readFrame(this->fidx - 4))
            this->timingRing.writeCsvRow(*timing);
    }

    size_t slot = 0;
    static const bool skipAll = std::getenv("LSFGVK_SKIP_ALL")
        && std::getenv("LSFGVK_SKIP_ALL")[0] == '1';
    if (const auto picked = skipAll ? std::nullopt : this->trySelectFreeSlot()) {
        slot = *picked;
    } else {
        // GPU B is behind: skip this capture. do NOT wait. blit never ran so
        // the game's wait semaphores are still pending — present must wait
        // on them (the capture path consumes them in QueueSubmit).
        this->droppedCaptures++;
        if (layerDbg && (this->droppedCaptures == 1 || (this->droppedCaptures % 64) == 0))
            std::cerr << "lsfg-vk-layer: [dbg] present: capture skipped (no slot)"
                      << " dropped=" << this->droppedCaptures
                      << " free=" << this->freeMask()
                      << " (fidx " << this->fidx << ")" << phaseAbsMs() << "\n";
        phaseLog("capture skipped (no slot)");
        if (this->fake) {
            if (!semaphores.empty()) {
                std::vector<VkPipelineStageFlags> stages(semaphores.size(),
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
                const VkSubmitInfo submit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .waitSemaphoreCount = static_cast<uint32_t>(semaphores.size()),
                    .pWaitSemaphores = semaphores.data(),
                    .pWaitDstStageMask = stages.data(),
                };
                VkQueue sig = this->captureQ != VK_NULL_HANDLE ? this->captureQ : queue;
                const auto res = vk.df().QueueSubmit(sig, 1, &submit, VK_NULL_HANDLE);
                if (res != VK_SUCCESS)
                    throw ls::vulkan_error(res, "vkQueueSubmit() failed");
            }
            phaseLog("isolated present (skipped capture)");
            return VK_SUCCESS;
        }
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = next_chain,
            .waitSemaphoreCount = static_cast<uint32_t>(semaphores.size()),
            .pWaitSemaphores = semaphores.empty() ? nullptr : semaphores.data(),
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &imageIdx,
        };
        auto res = vk.df().QueuePresentKHR(queue, &presentInfo);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");
        phaseLog("forwarded present returned (TOTAL)");
        return res;
    }
    phaseLog("slot selected");
    if (exportIsolatedOn()) {
        if (imageIdx >= this->slotFree.size() || !this->slotFree.at(imageIdx)) {
            this->droppedCaptures++;
            phaseLog("isolated-export skip (image slot busy)");
            if (!semaphores.empty()) {
                std::vector<VkPipelineStageFlags> stages(semaphores.size(),
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
                const VkSubmitInfo submit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .waitSemaphoreCount = static_cast<uint32_t>(semaphores.size()),
                    .pWaitSemaphores = semaphores.data(),
                    .pWaitDstStageMask = stages.data(),
                };
                VkQueue sig = this->captureQ != VK_NULL_HANDLE ? this->captureQ : queue;
                const auto res = vk.df().QueueSubmit(sig, 1, &submit, VK_NULL_HANDLE);
                if (res != VK_SUCCESS)
                    throw ls::vulkan_error(res, "vkQueueSubmit() failed");
            }
            return VK_SUCCESS;
        }
        slot = imageIdx;
    }

    if (imageIdx >= this->info.images.size())
        throw ls::error("swapchain image index out of range");

    const bool dummySrc = std::getenv("LSFGVK_DUMMY_SRC")
        && std::getenv("LSFGVK_DUMMY_SRC")[0] == '1'
        && !this->localImages.empty();
    const VkImage srcImage = dummySrc
        ? this->localImages.front().handle() : this->info.images.at(imageIdx);
    const vk::Semaphore& presentSem = this->presentSemaphores.at(slot);

    // Empty-CB + no fence holds 141 fps. A fence on that same submit is 53.
    // Copy is 343 µs; 6 CBs at 140 fps reuse after ~43 ms. Do not host-wait
    // or signal a fence on the capture submit.
    const size_t ringIdx = this->captureRingIdx % CAPTURE_RING_DEPTH;
    this->captureRingIdx = (ringIdx + 1) % CAPTURE_RING_DEPTH;
    phaseLog("ring slot acquired");

    // FRESH capture semaphore per cycle, replacing the previous one now that
    // the fence gate proves its signal completed. re-signaling an already
    // signaled binary semaphore is undefined, and its exported sync fd would
    // read as signaled immediately - letting the app's pre-pass sample the
    // staging image before this cycle's blit completes (ghosting under load).
    // first present uses the ctor-created, never-signaled semaphores.
    static const bool leakSem = std::getenv("LSFGVK_LEAK_SEM")
        && std::getenv("LSFGVK_LEAK_SEM")[0] == '1';
    const vk::Semaphore* sigSemPtr = nullptr;
    if (leakSem) {
        this->leakCaptureSems.emplace_back(vk, std::nullopt,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
        sigSemPtr = &this->leakCaptureSems.back();
    } else {
        this->captureSemaphores.at(slot) = vk::Semaphore(vk, std::nullopt,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
        sigSemPtr = &this->captureSemaphores.at(slot);
    }
    const vk::Semaphore& sigSem = *sigSemPtr;

    // record blit info.images[imageIdx] -> staging[slot] waiting on game's
    // present wait-semaphores, signal slot's capture semaphore
    const auto& cmdbuf = this->captureCommandBuffers.at(ringIdx);
    cmdbuf.begin(vk);
    static const bool emptyFrame = std::getenv("LSFGVK_EMPTY_FRAME")
        && std::getenv("LSFGVK_EMPTY_FRAME")[0] == '1';
    if (!emptyFrame && !exportIsolatedOn()) {
    const vk::Image& dstImage = !this->hostImages.empty()
        ? this->hostImages.at(slot)
        : ((this->localCopyOnly && !this->localImages.empty())
            ? this->localImages.at(slot) : this->stagingImages.at(slot));
    cmdbuf.copyImage(vk,
        {
            barrierHelper(srcImage,
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_READ_BIT,
                dummySrc ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            ),
            barrierHelper(dstImage.handle(),
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            ),
        },
        { srcImage, dstImage.handle() },
        dstImage.getExtent(),
        {
            barrierHelper(srcImage,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                dummySrc ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                         : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            ),
        }
    );
    }
    cmdbuf.end(vk);

    // submit the capture blit waiting on the game's present wait-semaphores
    // and signaling the slot's capture semaphore. use the game's present
    // queue (the queue param) for the submit so the wait/signal is on the
    // same queue that the subsequent QueuePresent will wait on; using
    // vk.queue() (first-graphics) can be a different queue handle and cause
    // the present wait to block forever on some drivers.
    try {
        std::vector<VkSemaphore> waitSems = semaphores;
        std::vector<VkSemaphore> signalSems = this->fake
            ? std::vector<VkSemaphore>{ sigSem.handle() }
            : std::vector<VkSemaphore>{ sigSem.handle(), presentSem.handle() };
        std::vector<VkPipelineStageFlags> stages(waitSems.size(),
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        VkCommandBuffer rawBuf = cmdbuf.raw();
        {
            FILE* sf = std::fopen("/tmp/lsfg_capture_slot", "w");
            if (sf) {
                std::fprintf(sf, "%zu\n", slot);
                std::fclose(sf);
            }
        }
        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = static_cast<uint32_t>(waitSems.size()),
            .pWaitSemaphores = waitSems.empty() ? nullptr : waitSems.data(),
            .pWaitDstStageMask = stages.empty() ? nullptr : stages.data(),
            .commandBufferCount = 1,
            .pCommandBuffers = &rawBuf,
            .signalSemaphoreCount = static_cast<uint32_t>(signalSems.size()),
            .pSignalSemaphores = signalSems.data()
        };
        VkFence sigFence = VK_NULL_HANDLE;
        if (isIsolated(swapchain)) {
            auto& iso = isolatedAt(swapchain);
            if (imageIdx < iso.recycleFences.size())
                sigFence = iso.recycleFences.at(imageIdx).handle();
        }
        auto res = vk.df().QueueSubmit(
            this->captureQ != VK_NULL_HANDLE ? this->captureQ : queue,
            1, &submitInfo, sigFence);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkQueueSubmit() failed");
        this->fenceSubmitted = true;
    } catch (const std::exception& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error(std::string("lsfg-vk: external stream error: capture submit failed: ") + e.what(), e);
    }
    phaseLog("blit submitted");
    static int nIsoResv = 0;
    if (nIsoResv < 16 && isIsolated(swapchain)) {
        auto& iso = isolatedAt(swapchain);
        if (imageIdx < iso.exportFds.size() && iso.exportFds.at(imageIdx) >= 0) {
            dma_buf_export_sync_file exp{};
            exp.flags = DMA_BUF_SYNC_WRITE;
            exp.fd = -1;
            if (::ioctl(iso.exportFds.at(imageIdx), DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exp) == 0) {
                pollfd p{};
                p.fd = exp.fd;
                p.events = POLLIN;
                const int pr = ::poll(&p, 1, 0);
                std::cerr << "lsfg-vk: isolated img " << imageIdx
                    << " WRITE poll0=" << pr << " revents=0x" << std::hex
                    << p.revents << std::dec << "\n";
                ::close(exp.fd);
                ++nIsoResv;
            } else {
                std::cerr << "lsfg-vk: isolated img " << imageIdx
                    << " WRITE export errno=" << errno << "\n";
                ++nIsoResv;
            }
        }
    }
    if (false && this->bVk && slot < this->bCbs.size() && slot < this->bFences.size()) {
        auto& bvk = *this->bVk;
        if (this->bFences.at(slot).wait(bvk, 0)
                && slot < this->bHostImages.size()
                && slot < this->bVramImages.size()) {
            this->bFences.at(slot).reset(bvk);
            auto& bcb = this->bCbs.at(slot);
            bcb.begin(bvk);
            const auto& srcB = this->bHostImages.at(slot);
            const auto& dstB = this->bVramImages.at(slot);
            bcb.copyImage(bvk,
                {
                    barrierHelper(srcB.handle(),
                        VK_ACCESS_NONE, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
                    barrierHelper(dstB.handle(),
                        VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                },
                { srcB.handle(), dstB.handle() },
                dstB.getExtent(),
                {
                    barrierHelper(dstB.handle(),
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
                });
            bcb.end(bvk);
            VkCommandBuffer rawB = bcb.raw();
            const VkSubmitInfo bsi{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &rawB,
            };
            auto bres = bvk.df().QueueSubmit(bvk.queue(), 1, &bsi,
                this->bFences.at(slot).handle());
            if (bres != VK_SUCCESS)
                std::cerr << "lsfg-vk: dual-host 9060 QueueSubmit " << bres << "\n";
        }
    }

    // export sync-fd immediately after enqueue (copy transference)
    int syncFd = -1;
    try {
        syncFd = sigSem.exportFd(vk);
    } catch (const std::exception& e) {
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        throw ls::error(std::string("lsfg-vk: external stream error: export sync fd failed: ") + e.what(), e);
    }
    if (this->copyHop && syncFd >= 0) {
        if (slot < this->shmMaps.size() && this->shmMaps.at(slot)
                && this->hostPtrsA.at(slot) && this->hostAllocSize > 0) {
            const int hopFd = ::dup(syncFd);
            if (hopFd >= 0) {
                this->copyHop->push(CopyHop::Job{
                    this->hostPtrsA.at(slot),
                    this->shmMaps.at(slot),
                    static_cast<size_t>(this->hostAllocSize),
                    hopFd,
                    -1,
                    -1,
                    this->shmSeq.at(slot)
                });
            }
        } else if (slot < this->rawMaps.size() && this->rawMaps.at(slot)
                && this->destMaps.at(slot) && this->rawBytes > 0) {
            const int hopFd = ::dup(syncFd);
            if (hopFd >= 0) {
                this->copyHop->push(CopyHop::Job{
                    this->destMaps.at(slot),
                    this->rawMaps.at(slot),
                    static_cast<size_t>(this->rawBytes),
                    hopFd,
                    this->localExportFds.at(slot),
                    this->rawReadyFds.at(slot),
                    nullptr
                });
            }
        }
    }

    static const bool noFrame = std::getenv("LSFGVK_NO_FRAME")
        && std::getenv("LSFGVK_NO_FRAME")[0] == '1';
    if (noFrame) {
        if (syncFd >= 0) { ::close(syncFd); syncFd = -1; }
        phaseLog("no FRAME (export closed)");
        if (this->fake)
            return VK_SUCCESS;
    }

    // send FRAME (owns fd on success, closes on failure path via Connection)
    try {
        int sendFd = syncFd;
        if (exportIsolatedOn() && this->fake && isIsolated(swapchain)) {
            auto& iso = isolatedAt(swapchain);
            if (imageIdx < iso.exportFds.size() && iso.exportFds.at(imageIdx) >= 0) {
                if (!this->dmaBufSent.at(slot)) {
                    sendFd = ::dup(iso.exportFds.at(imageIdx));
                    if (sendFd < 0)
                        sendFd = syncFd;
                    else if (syncFd >= 0) { ::close(syncFd); syncFd = -1; }
                    this->dmaBufSent.at(slot) = true;
                    std::cerr << "lsfg-vk: FRAME carries isolated-image dma-buf slot="
                        << slot << " img=" << imageIdx << " fd=" << sendFd << "\n";
                } else {
                    dma_buf_export_sync_file exp{};
                    exp.flags = DMA_BUF_SYNC_WRITE;
                    exp.fd = -1;
                    if (::ioctl(iso.exportFds.at(imageIdx),
                            DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exp) == 0) {
                        sendFd = exp.fd;
                        if (syncFd >= 0) { ::close(syncFd); syncFd = -1; }
                    }
                    static bool loggedW = false;
                    if (!loggedW) {
                        loggedW = true;
                        std::cerr << "lsfg-vk: FRAME carries isolated WRITE fence\n";
                    }
                }
            }
        } else if (this->shmMaps.at(0)) {
            // POSIX shm already has the pixels; keep the 9070 capture sync-fd
        } else if (slot < this->rawExportFds.size()
                && this->rawExportFds.at(slot) >= 0) {
            if (!this->dmaBufSent.at(slot)) {
                sendFd = ::dup(this->rawExportFds.at(slot));
                if (sendFd < 0)
                    sendFd = syncFd;
                else if (syncFd >= 0) { ::close(syncFd); syncFd = -1; }
                this->dmaBufSent.at(slot) = true;
                std::cerr << "lsfg-vk: FRAME carries raw dma-buf slot="
                    << slot << " fd=" << sendFd << "\n";
            } else if (this->rawReadyFds.at(slot) >= 0) {
                sendFd = ::dup(this->rawReadyFds.at(slot));
                if (sendFd < 0)
                    sendFd = syncFd;
                else if (syncFd >= 0) { ::close(syncFd); syncFd = -1; }
            }
        } else if (this->fake && slot < this->bExportFds.size()
                && this->bExportFds.at(slot) >= 0) {
            sendFd = ::dup(this->bExportFds.at(slot));
            if (sendFd < 0)
                sendFd = syncFd;
            else if (syncFd >= 0) { ::close(syncFd); syncFd = -1; }
            static bool loggedBfd = false;
            if (!loggedBfd) {
                loggedBfd = true;
                std::cerr << "lsfg-vk: FRAME carries 9060 dma-buf fd=" << sendFd
                    << " slot=" << slot << "\n";
            }
        } else if (this->fake && this->localCopyOnly
                && slot < this->localImages.size()) {
            if (!this->dmaBufSent.at(slot)) {
                auto exp = this->localImages.at(slot).exportDmaBuf(vk);
                sendFd = exp.fd;
                if (sendFd < 0)
                    sendFd = syncFd;
                else if (syncFd >= 0) { ::close(syncFd); syncFd = -1; }
                this->dmaBufSent.at(slot) = true;
                {
                    char link[80]{};
                    (void)::readlink((std::string("/proc/self/fd/") + std::to_string(sendFd)).c_str(),
                        link, sizeof(link) - 1);
                    std::cerr << "lsfg-vk: FRAME carries render-dmabuf slot="
                        << slot << " fd=" << sendFd << " link=" << link << "\n";
                }
            } else {
                static bool loggedS = false;
                if (!loggedS) {
                    loggedS = true;
                    std::cerr << "lsfg-vk: FRAME carries render-sync_fd\n";
                }
            }
        }
        const uint64_t capTs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        this->ipcConn->attachFd(sendFd);
        this->ipcConn->send(ls::ipc::Frame{ static_cast<uint32_t>(slot), capTs });
        sendFd = -1;
        syncFd = -1;
    } catch (const std::exception& e) {
        if (syncFd >= 0) ::close(syncFd);
        std::cerr << "lsfg-vk: external stream error: " << e.what() << "\n";
        // send()/recv() EPIPE handling → context marked dead → present hook
        // throws named error; entrypoint's existing catch converts to VK_ERROR_*
        throw ls::error(std::string("lsfg-vk: external stream error: send FRAME failed: ") + e.what(), e);
    }

    phaseLog("FRAME sent");
    static const bool noBusy = std::getenv("LSFGVK_NO_BUSY")
        && std::getenv("LSFGVK_NO_BUSY")[0] == '1';
    if (!noBusy)
        this->slotFree.at(slot) = false;
    this->fidx++;

    if (this->fake) {
        phaseLog("isolated present (no WSI)");
        return VK_SUCCESS;
    }

    // forward original present down-chain WITHOUT waiting on capture semaphore.
    // The game thread must not block on display presentation; that is the
    // frame-doubler's job on the output thread. Reusing the game's original
    // wait semaphores here is a no-op for correctness because those semaphores
    // were already waited on by the blit submit above and the GPU has consumed
    // their signal; re-waiting them is a no-op wait, not a double-wait.
    VkSemaphore waitSem = presentSem.handle();
    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = next_chain,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIdx,
    };
    auto res = vk.df().QueuePresentKHR(queue, &presentInfo);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");
    phaseLog("forwarded present returned (TOTAL)");

    return res;
}
