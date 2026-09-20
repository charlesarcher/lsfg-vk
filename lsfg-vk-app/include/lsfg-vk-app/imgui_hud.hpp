/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"

#include <vulkan/vulkan_core.h>
#include <cstdint>
#include "lsfg-vk-common/helpers/pointers.hpp"
#include <string>
#include <string_view>
#include <memory>
#include <array>

namespace ls::hud {

    /// dear imgui overlay for the doubler window (S40+ Phase A).
    /// renders an interactive OFFSCREEN tooltip-card (imgui) into a small
    /// RGBA render-target on the app's main (graphics-capable) queue, at a
    /// low tick rate (~15 Hz), then draws it into the swapchain with the
    /// same blit path as hud.cpp. TOGGLE: SIGUSR1 flips `visibility` with a
    /// fade; toggled-off = zero present-path work (no NewFrame / no blit).
    class ImGuiHud {
    public:
        /// create the draw context + offscreen target
        /// @param vk a GRAPHICAL vk::Vulkan (needs VK_QUEUE_GRAPHICS_BIT fam)
        /// @param outputExtent size of the swapchain (for the HUD base scale)
        /// @param format swapchain format for the offscreen RT (B8G8R8A8)
        /// @throws ls::error if graphics queue missing / setup fails
        ImGuiHud(const vk::Vulkan& vk, VkExtent2D outputExtent, VkFormat format);

        ~ImGuiHud();

        ImGuiHud(const ImGuiHud&) = delete;
        ImGuiHud& operator=(const ImGuiHud&) = delete;

        /// stats snapshot that drives the widgets (thread-safe setters)
        struct Stats {
            float gameFps{0.f}, presentedFps{0.f};
            float clickP50{0.f}, clickP99{0.f};   // 0 = not measured (dashes)
            float ipcMs{0.f}, genMs{0.f}, scanMs{0.f}; // pipeline segments
            float genExtraMs{0.f};                // E row (GEN adds)
            bool  genExtraLive{false};
            float frameTimesMs[180]{};            // sparkline ring
            uint32_t frameTimesIdx{0}, frameTimesCount{0};
        };
        /// thread-safe snapshot setter (producer = present/stats thread)
        static void publish(const Stats& s);
        [[nodiscard]] static Stats latest();

        /// queue a toggle (any thread): fade-out -> hidden, fade-in -> shown
        static void toggle();
        /// query whether the overlay is (still) drawing — false stops blits
        [[nodiscard]] static bool drawing();

        /// render one frame (call at ~15 Hz from the OUTPUT thread):
        /// NewFrame -> widgets -> submit RT upload. cheap when hidden.
        void tick(float dt);

        /// image to blit + its extent + whether to blit this frame
        [[nodiscard]] VkImage rtImage() const;
        [[nodiscard]] VkExtent2D rtExtent() const;
        /// alpha the compositor-blit should carry this frame (1 = solid)
        [[nodiscard]] float blitAlpha() const;
        /// last GPU access mask for the RT src barrier in the present cb
        [[nodiscard]] VkAccessFlags lastAccess() const;
        void markRead();

        /// where the blit lands (matches Hud::ORIGIN semantics: inset from
        /// the RIGHT and TOP edges of the swapchain)
        struct Origin { int32_t x, y; };
        [[nodiscard]] Origin origin() const;

    private:
        void drawWidgets(const struct Stats& s);   // the actual imgui frame
        void setupThemeAndFont();
        void ensureFade(float dt);
        void blitUploadFenceGuard();

        const vk::Vulkan& vk;
        VkExtent2D outExtent;
        VkFormat format;
        uint32_t baseScale{4};                    // px per imgui point-ish
        VkExtent2D rtSize;                        // offscreen RT size
        std::unique_ptr<vk::Image> rt[2];         // ping-pong RT pair
        uint8_t active{0};
        VkRenderPass renderpass{VK_NULL_HANDLE};
        VkFramebuffer framebuffer[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        ls::lazy<vk::Fence> rtFence[2];
        std::array<bool, 2> rtFenceSignaled{false, false};
        std::array<VkAccessFlags, 2> rtLastAccess{
            VK_ACCESS_NONE, VK_ACCESS_NONE };
        bool init{false};
        bool fontsBuilt{false};
        bool fontFenceSignaled{false};   // font fence lives in imgui backend
        bool rtGeneral[2]{false, false};
        class vk::CommandBuffer cmdbuf;
        float fadeLock{0.f};                     // 0..1 synthesized alpha
        bool visible{true};
        std::string fontPath;
    };

} // namespace ls::hud
