/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/ipc/udmabuf_ring.hpp"

#include "lsfg-vk-common/helpers/errors.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/udmabuf.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace ls::ipc {

namespace {

/// memfd + seal + UDMABUF_CREATE, the exact sequence the existing staging path
/// uses (capture_context.cpp createRawDmaBuf). MFD_ALLOW_SEALING is mandatory:
/// without it the kernel refuses F_SEAL_SHRINK with EPERM.
int createUdmabuf(std::size_t bytes) {
    const int mfd = static_cast<int>(::syscall(SYS_memfd_create, "lsfg-ring",
        static_cast<long>(MFD_CLOEXEC | MFD_ALLOW_SEALING)));
    if (mfd < 0)
        throw ls::error("udmabuf ring: memfd_create failed");
    if (::ftruncate(mfd, static_cast<off_t>(bytes)) != 0) {
        ::close(mfd);
        throw ls::error("udmabuf ring: ftruncate failed");
    }
    if (::fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
        ::close(mfd);
        throw ls::error("udmabuf ring: F_SEAL_SHRINK failed");
    }
    int udev = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udev < 0) {
        ::close(mfd);
        throw ls::error("udmabuf ring: open /dev/udmabuf failed");
    }
    udmabuf_create uc{};
    uc.memfd = static_cast<std::uint32_t>(mfd);
    uc.flags = UDMABUF_FLAGS_CLOEXEC;
    uc.offset = 0;
    uc.size = bytes;
    const int udfd = ::ioctl(udev, UDMABUF_CREATE, &uc);
    const int err = errno;
    ::close(udev);
    if (udfd < 0) {
        ::close(mfd);
        errno = err;
        throw ls::error("udmabuf ring: UDMABUF_CREATE failed");
    }
    return udfd;
}

int createMemfd(std::size_t bytes, const char* name) {
    const int fd = static_cast<int>(::syscall(SYS_memfd_create, name,
        static_cast<long>(MFD_CLOEXEC | MFD_ALLOW_SEALING)));
    if (fd < 0)
        throw ls::error("control block: memfd_create failed");
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        ::close(fd);
        throw ls::error("control block: ftruncate failed");
    }
    return fd;
}

}  // namespace

UdmabufRing::UdmabufRing(std::uint32_t count, std::size_t slotBytes,
    std::uint32_t width, std::uint32_t height, std::uint32_t format) {
    if (count == 0)
        throw ls::error("udmabuf ring: K must be at least 1");
    if (slotBytes == 0)
        throw ls::error("udmabuf ring: slot size must be non-zero");

    this->bytes = (slotBytes + 4095u) & ~static_cast<std::size_t>(4095u);

    // Build all K buffers before publishing anything: a partial ring must never
    // reach the peer, or the app would map a count it cannot satisfy.
    std::vector<RingBuffer> made;
    made.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        RingBuffer b{};
        b.bytes = this->bytes;
        // memfd first so a UDMABUF_CREATE failure has something to close.
        b.memfd = createMemfd(this->bytes, "lsfg-slot");
        b.udmabuf = createUdmabuf(this->bytes);
        made.push_back(b);
    }

    this->ctrlBytes = sizeof(RingHeader) + sizeof(RingSlot) * count;
    this->ctrlFd = createMemfd(this->ctrlBytes, "lsfg-cb");
    this->ctrlMap = ::mmap(nullptr, this->ctrlBytes,
        PROT_READ | PROT_WRITE, MAP_SHARED, this->ctrlFd, 0);
    if (this->ctrlMap == MAP_FAILED) {
        this->ctrlMap = nullptr;
        for (auto& b : made) {
            ::close(b.udmabuf);
            ::close(b.memfd);
        }
        ::close(this->ctrlFd);
        this->ctrlFd = -1;
        throw ls::error("control block: mmap failed");
    }
    ::memset(this->ctrlMap, 0, this->ctrlBytes);

    this->cb = std::make_unique<ControlBlock>(this->ctrlMap, this->ctrlBytes, true);
    this->cb->init(count, this->bytes, width, height, format);
    this->bufs = std::move(made);
}

UdmabufRing::~UdmabufRing() {
    if (this->cb != nullptr)
        this->cb.reset();
    if (this->ctrlMap != nullptr)
        ::munmap(this->ctrlMap, this->ctrlBytes);
    if (this->ctrlFd >= 0)
        ::close(this->ctrlFd);
    for (auto& b : this->bufs) {
        if (b.udmabuf >= 0)
            ::close(b.udmabuf);
        if (b.memfd >= 0)
            ::close(b.memfd);
    }
}

UdmabufRing::UdmabufRing(UdmabufRing&& other) noexcept
    : bufs(std::move(other.bufs)), ctrlMap(other.ctrlMap),
      ctrlBytes(other.ctrlBytes), ctrlFd(other.ctrlFd), bytes(other.bytes),
      cb(std::move(other.cb)) {
    other.ctrlMap = nullptr;
    other.ctrlBytes = 0;
    other.ctrlFd = -1;
    other.bytes = 0;
}

UdmabufRing& UdmabufRing::operator=(UdmabufRing&& other) noexcept {
    if (this != &other) {
        this->~UdmabufRing();
        bufs = std::move(other.bufs);
        ctrlMap = other.ctrlMap;
        ctrlBytes = other.ctrlBytes;
        ctrlFd = other.ctrlFd;
        bytes = other.bytes;
        cb = std::move(other.cb);
        other.ctrlMap = nullptr;
        other.ctrlBytes = 0;
        other.ctrlFd = -1;
        other.bytes = 0;
    }
    return *this;
}

int UdmabufRing::releaseControlFd() {
    const int fd = this->ctrlFd;
    this->ctrlFd = -1;
    return fd;
}

}  // namespace ls::ipc