/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain.hpp"
#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/exchange.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>
#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::layer;

namespace {
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

    /// import a sync fd into a binary semaphore as a temporary payload.
    /// on success the fd is consumed by the implementation, on failure it is
    /// closed here before throwing, so ownership never leaks either way.
    void importSyncFd(const vk::Vulkan& vk, VkSemaphore semaphore, int fd) {
        const VkImportSemaphoreFdInfoKHR importInfo{
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = fd
        };
        const auto res = vk.df().ImportSemaphoreFdKHR(vk.dev(), &importInfo);
        if (res != VK_SUCCESS) {
            close(fd);
            throw ls::vulkan_error(res, "vkImportSemaphoreFdKHR() failed");
        }
    }

    /// closes any not-yet-consumed sync fds if an exception escapes the loop
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes, cppcoreguidelines-avoid-const-or-ref-data-members, cppcoreguidelines-special-member-functions)
    struct FdJanitor {
        std::vector<int>& fds;
        size_t consumed{};

        ~FdJanitor() {
            for (size_t i = this->consumed; i < this->fds.size(); ++i)
                if (this->fds.at(i) >= 0) close(this->fds.at(i));
        }
    };
    // NOLINTEND(misc-non-private-member-variables-in-classes, cppcoreguidelines-avoid-const-or-ref-data-members, cppcoreguidelines-special-member-functions)

    /// format a 16-byte uuid as lowercase hex, mirroring the backend's init log.
    /// backend::Instance exposes no device-name accessor, so cross-device
    /// context logs identify the processing device by uuid instead
    std::string uuidToHex(const std::array<uint8_t, 16>& uuid) {
        static constexpr std::array<char, 17> chars = std::to_array("0123456789abcdef");

        std::string out{};
        out.reserve(2 * uuid.size());
        for (const uint8_t byte : uuid) {
            out += chars.at((byte >> 4) & 0xF);
            out += chars.at(byte & 0xF);
        }
        return out;
    }

    /// closes the layer-owned copies of the exported exchange fds when an
    /// exception escapes context creation; the backend receives duplicates
    /// instead, so these closes can never race an fd already consumed by a
    /// successful import (whose ownership moved to the driver)
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes, cppcoreguidelines-avoid-const-or-ref-data-members, cppcoreguidelines-special-member-functions)
    struct ExportFdJanitor {
        const std::vector<int>& fds;
        bool disarmed{false};

        ~ExportFdJanitor() {
            if (this->disarmed) return;
            for (const int fd : this->fds)
                if (fd >= 0) close(fd);
        }
    };
    // NOLINTEND(misc-non-private-member-variables-in-classes, cppcoreguidelines-avoid-const-or-ref-data-members, cppcoreguidelines-special-member-functions)
}

void layer::context_ModifySwapchainCreateInfo(const ls::GameConf& profile, uint32_t maxImages,
        VkSwapchainCreateInfoKHR& createInfo) {
    if (profile.presentation == ls::Presentation::External) {
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        return;
    }

    createInfo.imageUsage |=
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    switch (profile.pacing) {
        case ls::Pacing::None:
            createInfo.minImageCount += profile.multiplier;
            if (maxImages && createInfo.minImageCount > maxImages)
                createInfo.minImageCount = maxImages;

            createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            break;
    }
}

Swapchain::Swapchain(const vk::Vulkan& vk, backend::Instance& backend,
            ls::GameConf profile, SwapchainInfo info,
            const std::string& gameDeviceName) :
        instance(backend),
        profile(std::move(profile)), info(std::move(info)) {
    const VkExtent2D extent = this->info.extent;
    const bool hdr = this->info.format > 57;
    const VkFormat format = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;

    // per-context transport matrix decision: THIS context's game device uuid
    // decides the mode (multi-device processes wrap every device separately)
    const auto gameUuid = vk.deviceUUID();
    const bool crossDevice = gameUuid != backend.selectedDeviceUUID();

    // dual-gpu mode info lines (grep gates for the e2e matrix runner)
    if (crossDevice)
        std::cerr << "lsfg-vk: processing on '" << uuidToHex(backend.selectedDeviceUUID())
            << "' (game on '" << gameDeviceName << "')\n";
    else
        std::cerr << "lsfg-vk: frame generation on the game's own device '"
            << gameDeviceName << "'\n";

    // negotiate the exchange layout for cross-device contexts.
    // SANCTIONED SIMPLIFICATION: the layer only holds a vk::Vulkan wrapper for
    // the game device, so the processing device's real modifier caps are not
    // fetchable here - and the frozen backend api trusts the caller's
    // negotiation without exposing a caps channel. crossing devices therefore
    // negotiates against a LINEAR-only proxy gated on the backend's extension
    // accessors; empirically Intel∩AMD proper-modifier intersection is empty,
    // so LINEAR is the rig reality regardless. todo 4's pure
    // negotiateExchangeLayout stays in the loop for the usage-bit check so the
    // logic remains table-testable; true dual-caps negotiation lands when an
    // upstream caps channel exists.
    uint64_t negotiatedModifier = backend::EXCHANGE_MODIFIER_OPAQUE;
    if (crossDevice) {
        if (!backend.selectedDeviceSupportsDmaBuf()
                || !backend.selectedDeviceSupportsDrmModifierImages())
            throw ls::error("processing device (uuid "
                + uuidToHex(backend.selectedDeviceUUID())
                + ") lacks the dma-buf exchange extensions required for"
                " dual-gpu frame generation");

        constexpr VkFormatFeatureFlags2 usageNeeds =
            VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT
            | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT
            | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT
            | VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;

        try {
            const auto gameCaps = vk.exchangeCaps(format);
            // the proxy claims exactly the usages the exchange requires of the
            // processing device, reducing negotiation to checking the game
            // device's LINEAR entry against the full requirement set
            const vk::DeviceExchangeCaps processingCaps{
                { format, {{ vk::EXCHANGE_MODIFIER_LINEAR, usageNeeds }} }
            };
            const auto layout = vk::negotiateExchangeLayout(
                gameCaps, processingCaps, format, usageNeeds);
            if (layout.kind() != vk::ExchangeLayoutKind::LinearFallback)
                throw ls::error("negotiated a drm-modifier layout, which the"
                    " layer's linear-only dual-gpu transport does not support");
            negotiatedModifier = layout.modifier;
        } catch (const std::exception& e) {
            throw ls::error("failed to negotiate an exchange layout between"
                " game device '" + gameDeviceName + "' and processing device"
                " (uuid " + uuidToHex(backend.selectedDeviceUUID()) + ")", e);
        }
    }

    // create the exchange images: same-device keeps the legacy optimal-tiled
    // OPAQUE_FD path byte-identically, cross-device creates LINEAR dma-buf
    // exchange images whose fds are exported below
    const vk::ImageLayout exchangeLayout = crossDevice
        ? vk::ImageLayout{ .mode = vk::ImageMode::Linear }
        : vk::ImageLayout{};

    // fds start at -1 so the janitors below skip slots whose image has not
    // been created yet (a value-initialized 0 would alias stdin)
    std::vector<int> sourceFds(2, -1);
    std::vector<int> destinationFds(this->profile.multiplier - 1, -1);

    this->sourceImages.reserve(sourceFds.size());
    for (int& fd : sourceFds)
        this->sourceImages.emplace_back(vk,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt,
            crossDevice ? std::nullopt : std::optional<int*>(&fd),
            exchangeLayout);

    this->destinationImages.reserve(destinationFds.size());
    for (int& fd : destinationFds)
        this->destinationImages.emplace_back(vk,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt,
            crossDevice ? std::nullopt : std::optional<int*>(&fd),
            exchangeLayout);

    // a throw between fd creation and backend ownership transfer must close
    // every layer-owned fd exactly once; these janitors cover the windows
    // before fds are registered in exportedFds below
    ExportFdJanitor sourceJanitor{sourceFds};
    ExportFdJanitor destinationJanitor{destinationFds};

    // produce exchange descriptors: cross-device exports each image as dma-buf
    // carrying the negotiated layout, same-device wraps the legacy opaque fds
    // into EXCHANGE_MODIFIER_OPAQUE sentinel descriptors
    std::vector<vk::ExchangeDescriptor> sourceDescs{};
    std::vector<vk::ExchangeDescriptor> destDescs{};
    sourceDescs.reserve(sourceFds.size());
    destDescs.reserve(destinationFds.size());

    // every layer-owned exchange fd is registered here as produced, so the
    // janitor closes exactly the unconsumed originals on any failure below;
    // all push_backs are non-throwing (capacity reserved up front)
    std::vector<int> exportedFds{};
    exportedFds.reserve(sourceFds.size() + destinationFds.size() + 1);
    ExportFdJanitor janitor{exportedFds};

    if (crossDevice) {
        for (const auto& sourceImage : this->sourceImages) {
            const auto exp = sourceImage.exportDmaBuf(vk);
            exportedFds.push_back(exp.fd);
            sourceDescs.push_back({ exp.fd, exp.allocationSize, exp.rowPitch,
                negotiatedModifier, format, extent });
        }
        for (auto& destinationImage : this->destinationImages) {
            const auto exp = destinationImage.exportDmaBuf(vk);
            exportedFds.push_back(exp.fd);
            destDescs.push_back({ exp.fd, exp.allocationSize, exp.rowPitch,
                negotiatedModifier, format, extent });
        }
    } else {
        for (const int fd : sourceFds)
            sourceDescs.push_back({ fd, 0, 0,
                backend::EXCHANGE_MODIFIER_OPAQUE, format, extent });
        for (const int fd : destinationFds)
            destDescs.push_back({ fd, 0, 0,
                backend::EXCHANGE_MODIFIER_OPAQUE, format, extent });
        for (const auto& desc : sourceDescs) exportedFds.push_back(desc.fd);
        for (const auto& desc : destDescs) exportedFds.push_back(desc.fd);
    }

    // descriptor fds now have a single canonical owner (exportedFds); retire
    // the per-vector janitors so no fd can ever be closed twice
    sourceJanitor.disarmed = true;
    destinationJanitor.disarmed = true;

    int syncFd{};
    this->syncSemaphore.emplace(vk, 0, std::nullopt, &syncFd);
    if (syncFd >= 0) exportedFds.push_back(syncFd);

    // hand the descriptors to the backend via duplicated fds: the exported
    // originals stay layer-owned until openContext reports success, so any
    // throw closes each exported fd exactly once and can never double-close
    // an fd already consumed by a successful import (transfer semantics make
    // those driver-owned)
    std::vector<int> handedFds{};
    handedFds.reserve(exportedFds.size());
    for (const int exported : exportedFds) {
        const int dupe = dup(exported);
        if (dupe < 0) {
            for (const int fd : handedFds) close(fd);
            throw ls::error("dup() failed while handing off exchange descriptors");
        }
        handedFds.push_back(dupe);
    }
    for (size_t i = 0; i < sourceDescs.size(); ++i)
        sourceDescs.at(i).fd = handedFds.at(i);
    for (size_t i = 0; i < destDescs.size(); ++i)
        destDescs.at(i).fd = handedFds.at(sourceDescs.size() + i);

    // the backend decides cross-device from the exact uuid comparison made
    // above, so cross-device contexts provably never consume the sync fd:
    // hand the original there (still janitor-owned) and retire the unused
    // duplicate; same-device contexts hand a duplicate because whether the
    // backend has consumed it when a failure unwinds is undecidable here
    const size_t syncFdSlot = sourceDescs.size() + destDescs.size();
    int backendSyncFd{};
    if (crossDevice) {
        if (syncFd >= 0) close(handedFds.at(syncFdSlot));
        backendSyncFd = syncFd;
    } else {
        backendSyncFd = syncFd >= 0 ? handedFds.at(syncFdSlot) : -1;
    }

    try {
        this->ctx = ls::owned_ptr<ls::R<backend::Context>>(
            new ls::R<backend::Context>(backend.openContext(
                sourceDescs, destDescs, gameUuid, negotiatedModifier,
                backendSyncFd, extent.width, extent.height,
                hdr, 1.0F / this->profile.flow_scale, this->profile.performance_mode
            )),
            [backend = &backend](ls::R<backend::Context>& ctx) {
                backend->closeContext(ctx);
            }
        );

        backend::makeLeaking(); // don't worry about it :3
    } catch (const std::exception& e) {
        throw ls::error("failed to create swapchain context", e);
    }
    janitor.disarmed = true;
    for (const int fd : exportedFds) close(fd);

    this->crossDevice = backend.isCrossDevice(this->ctx.get());
    if (this->crossDevice) {
        try {
            // import-only ring for observing done fds; capture semaphores are
            // created fresh per present inside present() instead of pooled
            for (size_t i = 0; i < this->destinationImages.size(); ++i)
                this->doneWaitSemaphores.emplace_back(vk,
                    std::nullopt, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
        } catch (const std::exception& e) {
            throw ls::error("failed to create sync-fd semaphore pools", e);
        }
    }

    this->renderCommandBuffer.emplace(vk);
    this->renderFence.emplace(vk);
    for (size_t i = 0; i < this->destinationImages.size(); i++) {
        this->passes.emplace_back(RenderPass {
            .commandBuffer = vk::CommandBuffer(vk),
            .acquireSemaphore = vk::Semaphore(vk)
        });
    }

    const size_t frames = std::max(this->info.images.size(), this->destinationImages.size() + 2);
    for (size_t i = 0; i < frames; i++) {
        this->postCopySemaphores.emplace_back(
            vk::Semaphore(vk),
            vk::Semaphore(vk)
        );
    }
}

VkResult Swapchain::present(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores) {
    const auto& swapchainImage = this->info.images.at(imageIdx);
    const auto& sourceImage = this->sourceImages.at(this->fidx % 2);

    std::vector<int> doneFds{};

    // update present mode when not using pacing
    if (this->profile.pacing == ls::Pacing::None) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
        auto* info = reinterpret_cast<VkSwapchainPresentModeInfoEXT*>(next_chain);
        while (info) {
            if (info->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
                for (size_t i = 0; i < info->swapchainCount; i++)
                    const_cast<VkPresentModeKHR*>(info->pPresentModes)[i] =
                        VK_PRESENT_MODE_FIFO_KHR;
            }

            info = reinterpret_cast<VkSwapchainPresentModeInfoEXT*>(const_cast<void*>(info->pNext));
        }
#pragma clang diagnostic pop
    }

    // record the capture blit (shared by both sync modes)
    const auto& cmdbuf = *this->renderCommandBuffer;
    cmdbuf.begin(vk);

    cmdbuf.blitImage(vk,
        {
            barrierHelper(swapchainImage,
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            ),
            barrierHelper(sourceImage.handle(),
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            ),
        },
        { swapchainImage, sourceImage.handle() },
        sourceImage.getExtent(),
        {
            barrierHelper(swapchainImage,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            ),
        }
    );

    if (this->crossDevice) {
        // cross-device order: retire the previous frame first, then capture
        // and export its sync fd, then schedule the backend with it - the fd
        // must exist before the backend enqueues the pre-pass that waits it
        if (this->fidx && !this->renderFence->wait(vk, 150ULL * 1000 * 1000))
            throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");
        this->renderFence->reset(vk);

        // retire last cycle's capture semaphore: the fence gate above proved
        // everything through the previous present's final destination pass
        // completed, and that pass chain observed this semaphore's signal via
        // the backend's done fds - so its batch is done and destruction here
        // is legal (immediate destruction after enqueue would trip VUID-05149,
        // and re-waiting it after its export deadlocks RADV, hence no pooling)
        this->retiredCaptureSignal = std::move(this->captureSignal);
        this->captureSignal.emplace(vk, std::nullopt,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);

        cmdbuf.end(vk);
        cmdbuf.submit(vk,
            semaphores, VK_NULL_HANDLE, 0,
            { this->captureSignal->handle() }, VK_NULL_HANDLE, 0
        );

        // export right after enqueueing the signal: sync fds have copy
        // transference, so the fd snapshots the pending payload and is
        // signaled once the capture blit completes on the device.
        // ownership of the fd transfers to scheduleFrames in all cases
        const int captureFd = this->captureSignal->exportFd(vk);

        try {
            doneFds = this->instance.get().scheduleFrames(this->ctx.get(), captureFd);
        } catch (const std::exception& e) {
            throw ls::error("failed to schedule frames", e);
        }
    } else {
        // schedule frame generation
        try {
            this->instance.get().scheduleFrames(this->ctx.get());
        } catch (const std::exception& e) {
            throw ls::error("failed to schedule frames", e);
        }

        // wait for completion of previous frame
        if (this->fidx && !this->renderFence->wait(vk, 150ULL * 1000 * 1000))
            throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");
        this->renderFence->reset(vk);

        cmdbuf.end(vk);
        cmdbuf.submit(vk,
            semaphores, VK_NULL_HANDLE, 0,
            {}, this->syncSemaphore->handle(), this->idx++
        );
    }

    FdJanitor janitor{doneFds};

    for (size_t i = 0; i < this->destinationImages.size(); i++) {
        auto& pcs = this->postCopySemaphores.at(this->idx % this->postCopySemaphores.size());
        auto& destinationImage = this->destinationImages.at(i);
        auto& pass = this->passes.at(i);

        // acquire swapchain image
        uint32_t aqImageIdx{};
        auto res = vk.df().AcquireNextImageKHR(vk.dev(), swapchain,
            UINT64_MAX, pass.acquireSemaphore.handle(),
            VK_NULL_HANDLE,
            &aqImageIdx
        );
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(res, "vkAcquireNextImageKHR() failed");

        const auto& aquiredSwapchainImage = this->info.images.at(aqImageIdx);

        // copy backend destination image into swapchain image
        auto& cmdbuf = pass.commandBuffer;
        cmdbuf.begin(vk);

        cmdbuf.blitImage(vk,
            {
                barrierHelper(destinationImage.handle(),
                    VK_ACCESS_NONE,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                ),
                barrierHelper(aquiredSwapchainImage,
                    VK_ACCESS_NONE,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                ),
            },
            { destinationImage.handle(), aquiredSwapchainImage },
            destinationImage.getExtent(),
            {
                barrierHelper(aquiredSwapchainImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                ),
            }
        );

        std::vector<VkSemaphore> waitSemaphores{ pass.acquireSemaphore.handle() };
        if (i) { // non-first pass
            const auto& prevPCS = this->postCopySemaphores.at((this->idx - 1) % this->postCopySemaphores.size());
            waitSemaphores.push_back(prevPCS.second.handle());
        }

        const std::vector<VkSemaphore> signalSemaphores{
            pcs.first.handle(),
            pcs.second.handle()
        };

        VkFence renderFence =
            i == this->destinationImages.size() - 1 ? this->renderFence->handle() : VK_NULL_HANDLE;

        cmdbuf.end(vk);
        if (this->crossDevice) {
            // ownership transfers unconditionally into importSyncFd
            // (consumes on success, closes on failure), so the janitor must
            // stop tracking before the transfer happens
            janitor.consumed = i + 1;

            // wait for this generated frame via its sync fd; an fd of -1
            // means the frame already completed, so the wait is skipped
            if (doneFds.at(i) >= 0) {
                importSyncFd(vk,
                    this->doneWaitSemaphores.at(i).handle(), doneFds.at(i));
                waitSemaphores.push_back(this->doneWaitSemaphores.at(i).handle());
            }

            cmdbuf.submit(vk,
                waitSemaphores, VK_NULL_HANDLE, 0,
                signalSemaphores, VK_NULL_HANDLE, 0,
                renderFence
            );
        } else {
            janitor.consumed = i + 1;

            cmdbuf.submit(vk,
                waitSemaphores, this->syncSemaphore->handle(), this->idx,
                signalSemaphores, VK_NULL_HANDLE, 0,
                renderFence
            );
        }

        // present swapchain image
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = i ? nullptr : next_chain,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &pcs.first.handle(),
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &aqImageIdx,
        };
        res = vk.df().QueuePresentKHR(queue,
            &presentInfo);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

        this->idx++;
    }

    // present original swapchain image
    auto& lastPCS = this->postCopySemaphores.at((this->idx - 1) % this->postCopySemaphores.size());
    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &lastPCS.second.handle(),
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIdx,
    };
    auto res = vk.df().QueuePresentKHR(queue, &presentInfo);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

    this->fidx++;
    return res;
}
