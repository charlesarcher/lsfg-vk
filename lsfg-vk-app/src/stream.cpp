/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-app/stream.hpp"
#include "lsfg-vk-common/frame_dbg.hpp"
#include "lsfg-vk-app/presentation.hpp"

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/helpers/env_flag.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/ipc/protocol.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"

#include <vulkan/vulkan_core.h>

#include <chrono>
#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <atomic>
#include <vector>
#include <span>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/memfd.h>

using namespace ls::ipc;

StreamState::~StreamState() {
    // RADV FreeMemory on HOST_ALLOCATION_BIT frees the posix_memalign
    // pointer and aborts the overlay on FurMark swapchain recreate.
    if (this->shmBytes == 0)
        return;
    /* S42e OOM fix: the old dtor moved EVERY dead stream's staging
       into a process-lifetime static vector — a FurMark-recreate
       workaround that becomes an unbounded leak when a game
       reconnects repeatedly (RE2 2026-09-20: six kernel OOM kills,
       swap exhaustion; watchdog + journal evidence). Policy:
       retain the FIRST stream's images (the RADV FreeMemory aliasing
       hazard needs at most one retention), free every stream's
       memfd maps + host buffers beyond that, and let later streams'
       images destruct normally. */
    static unsigned retainedStreams = 0;
    if (retainedStreams == 0) {
        static auto* leak = new std::vector<ls::lazy<vk::Image>>;
        for (auto& im : this->sourceImages)
            if (im.has_value())
                leak->push_back(std::move(im));
        retainedStreams = 1;
        return;
    }
    /* S42m (alt-F4 SIGABRT fix): free ONLY the memfd maps here.
       The host buffers (hostPtrs) are singly owned by the
       host-imported DeviceMemory of hostImages: RADV's
       vkFreeMemory on HOST_ALLOCATION_BIT frees the caller's
       malloc block (see image.cpp importMemoryHostPointer).
       The member dtors run AFTER this body, so freeing hostPtrs
       here — and letting hostImages' vkFreeMemory free the same
       block again — is a guaranteed glibc double free:
       'double free or corruption (!prev)' + SIGABRT repro'd on
       every dxvk/ReFramework/RE2 swapchain teardown (alt-F4).
       hostImages destruct normally and release everything. */
    for (auto& m : this->shmMaps)
        if (m) { ::munmap(m, this->shmBytes + 4096); m = nullptr; }
}

namespace {

    /// bound the blocking recv() so a SIGINT (EINTR) or a silent peer can never
    /// hang the accept loop: SO_RCVTIMEO makes recvmsg return within this span
    /// when no data is ready.
    constexpr std::chrono::milliseconds RECV_TIMEOUT{500};
    /// deadline handed to Connection::receive so a single message cannot block
    /// longer than this; checked against the shutdown flag after any failure.
    constexpr std::chrono::milliseconds RECEIVE_DEADLINE{2000};
    /// poll backstop between messages (also wakes the loop to re-check stop)
    constexpr int IDLE_POLL_MS{500};

    /// format a VkFormat as a readable name; unknown formats fall back to the
    /// integer value rather than guessing (matches the "tiny switch" contract)
    std::string formatName(uint32_t format) {
        switch (static_cast<VkFormat>(format)) {
            case VK_FORMAT_R8G8B8A8_UNORM:   return "R8G8B8A8_UNORM";
            case VK_FORMAT_R8G8B8A8_SRGB:    return "R8G8B8A8_SRGB";
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "B10G11R11_UFLOAT_PACK32";
            default:
                return "VkFormat(" + std::to_string(format) + ")";
        }
    }

    /// receive the next message, breaking out cleanly when the shutdown flag is
    /// set or the peer has gone away. poll() first so an idle loop re-checks the
    /// flag (and detects POLLHUP); a blocking receive is bounded by SO_RCVTIMEO
    /// (set on the socket) and the receive deadline.
    /// @return the message, or std::nullopt to end the stream (SIGINT / peer close)
    std::optional<Message> recvStop(Connection& conn, const std::atomic<bool>& stop) {
        while (true) {
            if (stop.load(std::memory_order_relaxed))
                return std::nullopt;

            pollfd pfd{};
            pfd.fd = conn.fd();
            pfd.events = POLLIN;
            const int r = ::poll(&pfd, 1, IDLE_POLL_MS);
            if (r < 0) {
                if (errno == EINTR) continue;
                throw socket_error("poll() on stream", errno);
            }
            if (r == 0) continue;                                   // backstop timeout
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
                return std::nullopt;                                // peer gone

            try {
                return conn.receive(RECEIVE_DEADLINE);
            } catch (const std::exception&) {
                if (stop.load(std::memory_order_relaxed))
                    return std::nullopt;                            // SIGINT / timeout
                throw;
            }
        }
    }

    /// set SO_RCVTIMEO on a socket fd
    void setRecvTimeout(int fd, std::chrono::milliseconds timeout) {
        timeval tv{};
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
        tv.tv_sec = static_cast<time_t>(micros / 1'000'000);
        tv.tv_usec = static_cast<suseconds_t>(micros % 1'000'000);
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
            throw socket_error("setsockopt(SO_RCVTIMEO) on stream", errno);
    }
}

namespace ls::ipc {
void runStream(Connection& conn, StreamState& state, const std::atomic<bool>& stop,
    const vk::Vulkan& vk, lsfgvk::backend::Instance& backend, const ls::GameConf& conf,
    std::string_view session) {
    // own the backend for this stream's lifetime (used by the context janitor
    // below on erase); points to the process-level instance in main.cpp, so it
    // outlives this stream.
    state.backend = &backend;
    // bound blocking recv so SIGINT/EINTR cannot hang the stream forever
    setRecvTimeout(conn.fd(), RECV_TIMEOUT);

    // 1. HELLO: game identity + swapchain size/format --------------------
    LSFG_FRAME_DBG("runStream: waiting for HELLO");
    auto helloMsg = recvStop(conn, stop);
    LSFG_FRAME_DBG("runStream: HELLO received");
    if (!helloMsg)
        return;
    const auto* hello = std::get_if<Hello>(&*helloMsg);
    if (!hello)
        throw ls::error(std::string("expected HELLO, got ") + nameOf(typeOf(*helloMsg)));
    if (hello->protoVersion != PROTO_VERSION)
        throw ls::error("unsupported protocol version " + std::to_string(hello->protoVersion)
            + " (this app speaks " + std::to_string(PROTO_VERSION) + ")");

    std::cerr << "lsfg-vk-app: stream from '" << helloDeviceName(*hello) << "' "
              << hello->width << "x" << hello->height << " "
              << formatName(hello->vkFormat) << "\n";

    // 2. NEGOTIATED: the staging images (created below, step 3) are ALWAYS
    //    R8G8B8A8_UNORM, 4 Bpp. negotiate the exchange
    //    layout from the app device's TRUE caps and reply with the negotiated
    //    modifier + 256-byte-aligned pitch + allocation size.
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const uint32_t w = hello->width, h = hello->height;
    const uint32_t rowPitch = ((w * 4u) + 255u) / 256u * 256u;
    const auto appCaps = vk.exchangeCaps(fmt);
    const VkFormatFeatureFlags2 usageNeeds =
        VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
    // cross-device exchange shares no proper DRM modifier; negotiate against a
    // LINEAR-only proxy (swapchain.cpp pattern) to force modifier == LINEAR.
    const vk::DeviceExchangeCaps gameProxyCaps{
        { fmt, {{ vk::EXCHANGE_MODIFIER_LINEAR, usageNeeds }} }
    };
    const auto neg = vk::negotiateExchangeLayout(appCaps, gameProxyCaps, fmt, usageNeeds);
    state.negotiatedModifier = neg.modifier;
    state.width = w; state.height = h; state.gameUuid = hello->gameUuid;
    state.sourceFormat = fmt;
    state.captureFormat = static_cast<VkFormat>(hello->vkFormat);
    if (state.captureFormat != VK_FORMAT_B8G8R8A8_UNORM
            && state.captureFormat != VK_FORMAT_R8G8B8A8_UNORM)
        state.captureFormat = VK_FORMAT_R8G8B8A8_UNORM;
    state.rowPitch = rowPitch;
    conn.send(ls::ipc::Negotiated{
        .modifier = neg.modifier,
        .rowPitch = rowPitch,
        /* page-rounded so both sides agree on the shm seq-counter offset
         * (the staging ring allocates exactly this size; an un-rounded
         * allocationSize desyncs the layer's seq map at non-4K sizes) */
        .allocationSize = ((static_cast<uint64_t>(rowPitch) * h + 4095ull) & ~4095ull)
    });

    const vk::ImageLayout layout{
        .mode = (neg.modifier == vk::EXCHANGE_MODIFIER_LINEAR) ? vk::ImageMode::Linear
                                                               : vk::ImageMode::DrmModifier,
        .drmModifier = neg.modifier,
        .rowPitch = rowPitch
    };
    const VkImageUsageFlags imgUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
        | VK_IMAGE_USAGE_SAMPLED_BIT;

    // 3. STAGING_RING_DEPTH STAGING messages: create the staging images LOCALLY
    //    on B at the negotiated layout, self-export each as dma-buf, and hand
    //    the fds to the layer (one SCM_RIGHTS fd per STAGING message). the
    //    layer imports them TRANSFER_DST-only, so its capture blit writes each
    //    frame A→B over PCIe as a sequential DMA transfer while the LSSC chain
    //    runs on B-local sources. the app keeps the images as sourceImages
    //    (real frame blits) and hands the backend a dup of the same exports as
    //    the source descriptor fds (the backend imports the dup; the kernel
    //    duplicated the fd on send, no re-export roundtrip).
    std::vector<vk::ExchangeDescriptor> sourceDescs;
    std::vector<int> handedSourceFds;
    const std::vector<uint32_t> concurrentFamilies{
        vk.queueFamilyIndex(), vk.transferQueueFamilyIndex()
    };
    const VkSharingMode sourceSharing = vk.hasTransferQueue()
        ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    {
        const size_t probeSz = 4ull * 1024 * 1024;
        void* mall = nullptr;
        if (::posix_memalign(&mall, 4096, probeSz) == 0) {
            try {
                vk::Image img(vk, VkExtent2D{ 256, 256 }, fmt,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    mall, probeSz);
                LSFG_FRAME_DBG("host-import malloc OK");
            } catch (const std::exception& e) {
                LSFG_FRAME_DBG("host-import malloc FAIL %s", e.what());
            }
            ::free(mall);
        }
        {
            const size_t probeSz = 4ull * 1024 * 1024;
            const int sfd = ::shm_open("/lsfg-vk-host-probe", O_CREAT | O_RDWR, 0600);
            if (sfd >= 0) {
                (void)::ftruncate(sfd, static_cast<off_t>(probeSz));
                void* p = ::mmap(nullptr, probeSz, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
                ::shm_unlink("/lsfg-vk-host-probe");
                if (p != MAP_FAILED) {
                    ::memset(p, 0, probeSz);
                    try {
                        vk::Image img(vk, VkExtent2D{ 256, 256 }, fmt,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            p, probeSz);
                        LSFG_FRAME_DBG("host-import shm_open OK");
                    } catch (const std::exception& e) {
                        LSFG_FRAME_DBG("host-import shm_open FAIL %s", e.what());
                    }
                    ::munmap(p, probeSz);
                }
                ::close(sfd);
            }
        }
    }
    for (size_t i = 0; i < STAGING_RING_DEPTH; ++i) {
        const uint64_t bytes = (static_cast<uint64_t>(rowPitch) * h + 4095ull) & ~4095ull;
        // Host-buffer bounce: same memfd staging the shm path builds, but the
        // app's GPU DMAs out of the shared mapping instead of the CPU copying
        // out of it. The render card's leg is identical either way - it DMAs
        // into this buffer - so the only difference is who does the second
        // hop. No p2p, no CPU frame copy.
        const bool hostBounce =
            conf.presentation == ls::Presentation::External
            && !envFlagOn("LSFGVK_POSIX_SHM")
            // DecoupledDma is the historical name for the old dual-host
            // bounce; Udmabuf is the buffer-based one that is proven to work
            // on this platform. Both are the same staging shape here, the
            // difference is only the import on the app side.
            && (conf.transport == ls::Transport::Udmabuf
                || conf.transport == ls::Transport::DecoupledDma);
        const bool posixShm = hostBounce
            || (conf.presentation == ls::Presentation::External
                && (envFlagOn("LSFGVK_POSIX_SHM")
                    || conf.transport == ls::Transport::PosixShm));
        if (posixShm) {
        const uint64_t mapBytes = bytes + 4096ull;
        // udmabuf REQUIRES the backing memfd to be sealable and sealed
        // F_SEAL_SHRINK (and must NOT carry F_SEAL_WRITE), so the flag set
        // differs between the bounce and the plain POSIX-shm staging.
        const int mfdFlags = hostBounce
            ? (MFD_CLOEXEC | MFD_ALLOW_SEALING)
            : MFD_CLOEXEC;
        const int memfd = static_cast<int>(::syscall(SYS_memfd_create, "lsfg-host", mfdFlags));
        if (memfd < 0 || ::ftruncate(memfd, static_cast<off_t>(mapBytes)) != 0)
            throw ls::error("memfd_create/ftruncate failed for POSIX staging");
        void* map = ::mmap(nullptr, static_cast<size_t>(mapBytes),
            PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        if (map == MAP_FAILED) {
            ::close(memfd);
            throw ls::error("mmap memfd failed for POSIX staging");
        }
        ::memset(map, 0, static_cast<size_t>(mapBytes));
        (void)::mlock(map, static_cast<size_t>(mapBytes));

        // Host-buffer bounce: wrap these exact pages in a udmabuf so OUR gpu
        // can import them and DMA out with no CPU copy. The layer keeps
        // receiving the plain memfd below and keeps DMAing into it, so the
        // two processes share one physical allocation:
        //
        //     render GPU --DMA--> system memory --DMA--> doubler GPU
        //
        // No p2p (the two cards have no usable cross-device page-table
        // mapping here) and no host-pointer import (radv rejects mmap'd
        // shared mappings for external_memory_host). Proven by
        // tools/testing/test_udmabuf.sh.
        if (hostBounce) {
            if (::fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
                ::munmap(map, static_cast<size_t>(mapBytes));
                ::close(memfd);
                throw ls::error("F_ADD_SEALS(F_SEAL_SHRINK) failed for udmabuf staging");
            }
            int udev = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
            if (udev < 0) {
                ::munmap(map, static_cast<size_t>(mapBytes));
                ::close(memfd);
                throw ls::error("open /dev/udmabuf failed for host bounce");
            }
            udmabuf_create uc{};
            uc.memfd = static_cast<uint32_t>(memfd);
            uc.flags = UDMABUF_FLAGS_CLOEXEC;
            uc.offset = 0;
            uc.size = bytes;   // page-aligned by construction; excludes the seq word
            const int udfd = ::ioctl(udev, UDMABUF_CREATE, &uc);
            ::close(udev);
            if (udfd < 0) {
                ::munmap(map, static_cast<size_t>(mapBytes));
                ::close(memfd);
                throw ls::error("UDMABUF_CREATE failed for host bounce");
            }
            state.udmaFds.at(i) = udfd;
        }

        state.shmMaps.at(i) = map;
        if (hostBounce) {
            // The bounce's whole point: our GPU DMAs out of the SAME physical
            // pages the render card DMAs into, so there is no CPU-side staging
            // buffer at all. Leave hostPtrs null - that null IS the signal the
            // shm branch uses to skip its 14 MiB memcpy - and leave
            // sourceImages empty so the fallback at presentation.cpp:2403
            // cannot pull us back onto the copy path either.
            state.dmaMaps.at(i) = map;
            LSFG_FRAME_DBG("host-bounce staging slot %zu size=%llu (no CPU copy)",
                i, (unsigned long long)bytes);
        } else {
        void* host = nullptr;
        if (::posix_memalign(&host, 4096, static_cast<size_t>(bytes)) != 0) {
            ::munmap(map, static_cast<size_t>(mapBytes));
            ::close(memfd);
            throw ls::error("posix_memalign staging failed");
        }
        ::memset(host, 0, static_cast<size_t>(bytes));
        state.sourceImages.at(i).emplace(vk, VkExtent2D{ w, h }, fmt,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                | VK_IMAGE_USAGE_SAMPLED_BIT,
            host, bytes);
        state.hostPtrs.at(i) = host;
            LSFG_FRAME_DBG("posix-shm staging slot %zu size=%llu",
                i, (unsigned long long)bytes);
        }
        state.shmSeq.at(i) = reinterpret_cast<uint32_t*>(
            static_cast<char*>(map) + static_cast<size_t>(bytes));
        state.shmSeen.at(i) = 0;
        state.shmBytes = static_cast<size_t>(bytes);
        const int sendFd = ::dup(memfd);
        ::close(memfd);
        if (sendFd < 0)
            throw ls::error("dup() failed for POSIX staging memfd");
        conn.attachFd(sendFd);
        conn.send(Staging{});
        continue;
        }
        state.sourceImages.at(i).emplace(vk, VkExtent2D{ w, h },
            state.captureFormat, imgUsage,
            std::nullopt /*importFd*/, std::nullopt /*exportFd*/, layout,
            sourceSharing, concurrentFamilies);
        auto exp = state.sourceImages.at(i).mut().exportDmaBuf(vk);
        conn.attachFd(exp.fd);
        conn.send(Staging{});
    }
    const VkImageUsageFlags genUsage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
        | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    for (size_t i = 0; i < STAGING_RING_DEPTH; ++i) {
        state.genSources.at(i).emplace(vk, VkExtent2D{ w, h }, fmt, genUsage,
            std::nullopt, std::nullopt, layout,
            sourceSharing, concurrentFamilies);
        auto exp = state.genSources.at(i).mut().exportDmaBuf(vk);
        const int dupForDescriptor = ::dup(exp.fd);
        if (dupForDescriptor < 0) {
            ::close(exp.fd);
            throw ls::error("dup() failed for gen-source descriptor fd");
        }
        ::close(exp.fd);
        handedSourceFds.push_back(dupForDescriptor);
        sourceDescs.push_back({
            .fd = dupForDescriptor,
            .allocationSize = exp.allocationSize,
            .rowPitch = exp.rowPitch,
            .modifier = neg.modifier,
            .format = fmt,
            .extent = VkExtent2D{ w, h }
        });
    }

    // 4. create (multiplier-1) B-local destination images natively, self-export
    //    each, and hand the backend a dup of the export fd as the destination
    //    descriptor. the backend generates one frame per destination image per
    //    cycle (multiplier = dests + 1), so the display cadence is
    //    multiplier x the game's (config parser already enforces multiplier > 1).
    const size_t destCount = conf.multiplier - 1;
    std::vector<vk::ExchangeDescriptor> destDescs;
    std::vector<int> handedDestFds;
    state.destinationImages.reserve(destCount);
    for (size_t i = 0; i < destCount; ++i) {
        state.destinationImages.emplace_back();
        state.destinationImages.at(i).emplace(vk, VkExtent2D{ w, h }, fmt, imgUsage,
            std::nullopt /*importFd*/, std::nullopt /*exportFd*/, layout);
        auto exp = state.destinationImages.at(i).mut().exportDmaBuf(vk);
        const int dupForDescriptor = ::dup(exp.fd);
        if (dupForDescriptor < 0) {
            ::close(exp.fd);
            throw ls::error("dup() failed for destination descriptor fd");
        }
        // exp.fd is unowned (image made with exportFd=nullopt) and the backend
        // only consumes the dup: closing it here avoids a 1-fd/stream leak.
        ::close(exp.fd);
        handedDestFds.push_back(dupForDescriptor);
        destDescs.push_back({
            .fd = dupForDescriptor,
            .allocationSize = exp.allocationSize,
            .rowPitch = exp.rowPitch,
            .modifier = neg.modifier,
            .format = fmt,
            .extent = VkExtent2D{ w, h }
        });
    }

    // 5. open the cross-device frame-gen context with EXACTLY 2 source +
    //    (multiplier-1) destination descriptors. hdr=false because the exchange
    //    (lsfgvk.hpp:103-105: false => R8G8B8A8_UNORM, true => RGBA16F). the
    //    backend infers the format from hdr, so a wrong hdr would reject the
    //    source descriptors' format. syncFd is ignored on the cross-device path.
    // exporterDeviceUUID must be the GAME's uuid, not this device's: the
    // backend uses it only to select the cross-device (sync-fd) handshake
    // path, the only external-presentation path that works on this RADV -
    // the same-device timeline-semaphore path fails at import with
    // VK_ERROR_IMPORT_NOT_ALLOWED. the source images being B-local does not
    // change which handshake path is needed: the capture-completion fd still
    // crosses from the game device every cycle.
    //    on any throw before a successful import, close the handed dups (the
    //    backend has consumed none yet).
    // Import the first shared buffer up front, on OUR device, before the
    // backend context opens. The udmabuf precondition below verifies this
    // import really happened and matches the staging slot, so it cannot be a
    // check that passes because nothing has been tried yet.
    if (conf.transport == ls::Transport::Udmabuf && !state.udmaFds.empty()
            && state.udmaFds.front() >= 0) {
        state.udmaBufs.front().emplace(
            vk, ::dup(state.udmaFds.front()),
            static_cast<size_t>(state.shmBytes),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        LSFG_FRAME_DBG("udmabuf: pre-imported slot 0 fd=%d bytes=%llu",
            state.udmaFds.front(), (unsigned long long)state.shmBytes);
    }

    try {
        LSFG_FRAME_DBG("runStream: openContext start");
        auto& ctx = backend.openContext(
            std::span<const vk::ExchangeDescriptor>(sourceDescs),
            std::span<const vk::ExchangeDescriptor>(destDescs),
            state.gameUuid, neg.modifier, -1 /*syncFd ignored cross-device*/,
            w, h, false /*hdr: R8G8B8A8 staging, never format>57*/,
            1.0F / conf.flow_scale, conf.performance_mode);
        LSFG_FRAME_DBG("runStream: openContext done");
        // The precondition depends on the transport, so the check does too.
        //
        // p2p / dmabuf: the backend imports the render GPU's IMAGE directly, so
        // a same-device context means the import silently landed on the wrong
        // card. That is fatal, and the check stands unchanged.
        //
        // udmabuf: the backend never sees the render GPU's image at all. The
        // shared slot crosses as a BUFFER that the backend's own device
        // imports, and this app's GPU does the copy. "cross-device" is
        // therefore not the property this path needs; what it needs is that
        // the buffer really was imported on the doubler, and that its size
        // and row length match the staging slot the app allocated. Checking
        // those instead of deleting the check keeps a broken setup from
        // passing here and faulting at the first frame.
        if (conf.transport == ls::Transport::Udmabuf) {
            if (backend.isCrossDevice(ctx))
                throw ls::error(
                    "udmabuf: backend context is cross-device, but this "
                    "transport must import the shared buffer on the doubler "
                    "device, not the render GPU's image");
            const uint64_t wantBytes = static_cast<uint64_t>(state.shmBytes);
            if (state.udmaBufs.empty() || !state.udmaBufs.front().has_value())
                throw ls::error("udmabuf: shared buffer not imported on the "
                    "doubler before the backend context opened");
            const size_t rowPitch = static_cast<size_t>(state.rowPitch);
            const bool sizeOk = state.udmaBufs.front()->byteSize() == wantBytes;
            // row length is in TEXELS; the pitch the app computed must be
            // exactly width * bytesPerPixel or the bounce copies garbage.
            const bool pitchOk = rowPitch
                == static_cast<size_t>(w) * 4;
            if (!sizeOk || !pitchOk)
                throw ls::error("udmabuf: shared buffer does not match the "
                    "staging slot (buffer=" + std::to_string(
                        state.udmaBufs.front()->byteSize()) + " want="
                    + std::to_string(wantBytes) + ", rowPitch="
                    + std::to_string(rowPitch) + " want="
                    + std::to_string(static_cast<size_t>(w) * 4) + ")");
            LSFG_FRAME_DBG("udmabuf: backend precondition ok (buffer=%llu pitch=%zu)",
                (unsigned long long)wantBytes, rowPitch);
        } else if (!backend.isCrossDevice(ctx)) {
            throw ls::error("backend context is not cross-device");
        }
        state.context = ls::owned_ptr<ls::R<lsfgvk::backend::Context>>(
            new ls::R<lsfgvk::backend::Context>(ctx),
            [backend = &backend](ls::R<lsfgvk::backend::Context>& c) {
                backend->closeContext(c);
            });
        std::cerr << "lsfg-vk-app: context created on '"
                  << (backend.selectedDeviceSupportsDmaBuf() ? "dma-buf" : "?")
                  << "' cross-device=" << backend.isCrossDevice(ctx) << "\n";
    } catch (const std::exception& e) {
        for (int fd : handedSourceFds) if (fd >= 0) ::close(fd);
        for (int fd : handedDestFds) if (fd >= 0) ::close(fd);
        throw ls::error("failed to open backend context", e);
    }

    // 6. READY: stream is live.
    conn.send(ls::ipc::Ready{});

    // 7. steady state: present the backend-generated + real captured frames on
    //    the output swapchain (task 8). Hand off to runPresent, which owns the
    //    window, surface and swapchain for this stream and tears them all down
    //    on every return path. The handshake above already opened the backend
    //    context that runPresent drives via backend.scheduleFrames.
    //
    //    Session 13.23: a stale display swapchain (VK_ERROR_OUT_OF_DATE /
    //    SUBOPTIMAL-then-failure: output mode change, compositor reflow) must
    //    NOT kill the IPC stream - the game's connection would EPIPE and die
    //    (observed: black screen). Rebuild the whole present session (window,
    //    surface, swapchain, threads); the connection and backend context
    //    survive. The layer keeps sending FRAMEs; the first few may land while
    //    no window exists yet - the input thread's staging slot recycle
    //    handles that (slots free after Release, no dependency on presents).
    for (int attempt = 0; attempt < 8; ++attempt) {
        try {
            ls::presentation::runPresent(conn, state, vk, backend, conf, session, stop);
            break;  // clean return (stop requested / game disconnected)
        } catch (const ls::vulkan_error& e) {
            const auto res = e.error();
            if (res != VK_ERROR_OUT_OF_DATE_KHR && res != VK_SUBOPTIMAL_KHR
                    && res != VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
                throw;
            std::cerr << "lsfg-vk-app: swapchain stale ("
                << (res == VK_ERROR_OUT_OF_DATE_KHR ? "OUT_OF_DATE" : "mode change")
                << "), rebuilding present session (attempt " << attempt + 1 << ")\n";
        }
        if (stop.load(std::memory_order_relaxed))
            break;
    }
}
}  // namespace ls::ipc
