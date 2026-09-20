/* SPDX-License-Identifier: GPL-3.0-or-later */

/* S40+ Phase A — dear imgui overlay for the doubler window.
 * Offscreen render target on the app device, ticked at low rate from the
 * present thread; drawn into the swapchain by the same blit path as hud.cpp.
 * Toggle = SIGUSR1 / app.sock control op (fade); hidden = zero present cost.
 * Standing fence discipline (S40): EVERY submit signals its fence; any wait
 * on an unrefreshed slot parks forever. */

#include "lsfg-vk-app/imgui_hud.hpp"
#include <algorithm>
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>



namespace ls::hud {

    // ---- cross-thread toggle + stats --------------------------------------
    namespace {
        std::atomic<bool> g_visible{true};
        std::atomic<bool> g_allocDone{false};
        std::atomic<float> g_alpha{1.0f};       // theoretical fade
        std::mutex g_statsMtx;
        ImGuiHud::Stats g_stats{};
        void toggleImpl() {
            const bool v = g_visible.load();
            g_visible.store(!v);
        }

        template <typename T>
        inline T devPfn(const vk::Vulkan& vk, const char* name) {
            return reinterpret_cast<T>(::vkGetInstanceProcAddr(vk.inst(), name));
        }
    }

    void ImGuiHud::pushLatencyMs(float ms) {
        std::lock_guard<std::mutex> lk(g_statsMtx);
        const uint32_t idx = g_stats.latencySamplesIdx % 256;
        g_stats.latencySamples[idx] = ms;
        g_stats.latencySamplesIdx++;
        g_stats.latencySamplesCount = std::min<uint32_t>(256, g_stats.latencySamplesCount + 1);
    }
    void ImGuiHud::pushFrameMs(float ms) {
        std::lock_guard<std::mutex> lk(g_statsMtx);
        const uint32_t idx = g_stats.frameTimesIdx % 180;
        g_stats.frameTimesMs[idx] = ms;
        g_stats.frameTimesIdx = (g_stats.frameTimesIdx + 1) % 100000;
        g_stats.frameTimesCount = std::min<uint32_t>(180, g_stats.frameTimesCount + 1);
    }
    void ImGuiHud::publish(const Stats& s) {
        /* S40+ merge: the gears of the ring counters (frametime/latency
           rings) belong to the OUTPUT thread; the 1 Hz stats publish MUST
           NOT clobber them (was: whole-struct → ring wiped every second =
           sparkline/latency never visible). Merge only the scalar fields. */
        std::lock_guard<std::mutex> lk(g_statsMtx);
        g_stats.gameFps = s.gameFps;
        g_stats.presentedFps = s.presentedFps;
        g_stats.ipcMs = s.ipcMs;
        g_stats.genMs = s.genMs;
        g_stats.scanMs = s.scanMs;
        g_stats.genExtraMs = s.genExtraMs;
        g_stats.genExtraLive = s.genExtraLive;
    }
    ImGuiHud::Stats ImGuiHud::latest() {
        std::lock_guard<std::mutex> lk(g_statsMtx);
        return g_stats;
    }
    void ImGuiHud::toggle() { toggleImpl(); }
    bool ImGuiHud::drawing() { return g_visible.load() || g_alpha.load() > 0.02f; }

    ImGuiHud::ImGuiHud(const vk::Vulkan& vkIn, VkExtent2D outputExtent, VkFormat format)
        : vk(vkIn), outExtent(outputExtent), format(format), cmdbuf(vkIn) {
        // RT size: ~output/4 with a sane floor, capped so ImGui's vertex count
        // stays negligible vs the present loop.
        const uint32_t w = std::max<uint32_t>(320, outputExtent.width / 4);
        const uint32_t h = std::max<uint32_t>(200, outputExtent.height / 4);
        this->rtSize = VkExtent2D{ w, h };
        this->baseScale = outputExtent.height / 540 ? outputExtent.height / 540 : 4;
        for (uint8_t i = 0; i < 2; ++i) {
            this->rt[i] = std::make_unique<vk::Image>(this->vk, this->rtSize,
                VK_FORMAT_B8G8R8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            this->rtFence[i].emplace(this->vk, false);
        }
        // renderpass for the RT (color, not sampled in place)
        VkAttachmentDescription color{};
        color.format = VK_FORMAT_B8G8R8A8_UNORM;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rpci{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &color,
            .subpassCount = 1, .pSubpasses = &sub,
            .dependencyCount = 1, .pDependencies = &dep,
        };
        if (const auto pfn = devPfn<PFN_vkCreateRenderPass>(vk, "vkCreateRenderPass");
            !pfn || pfn(vk.dev(), &rpci, nullptr, &this->renderpass) != VK_SUCCESS)
            throw ls::error("imgui_hud: CreateRenderPass failed");
        for (uint8_t i = 0; i < 2; ++i) {
            VkImageView iv = this->rt[i]->imageview();
            PFN_vkCreateFramebuffer dcrp_f = reinterpret_cast<PFN_vkCreateFramebuffer>(
            ::vkGetInstanceProcAddr(vk.inst(), "vkCreateFramebuffer"));
        VkFramebufferCreateInfo fci{
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = this->renderpass,
                .attachmentCount = 1, .pAttachments = &iv,
                .width = this->rtSize.width, .height = this->rtSize.height,
                .layers = 1,
            };
            if ((dcrp_f ? dcrp_f(vk.dev(), &fci, nullptr, &this->framebuffer[i])
                : VK_ERROR_EXTENSION_NOT_PRESENT) != VK_SUCCESS)
                throw ls::error("imgui_hud: CreateFramebuffer failed");
        }
        // ---- imgui context + vulkan backend --------------------------------
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        this->setupThemeAndFont();
        const char* font = this->fontPath.empty()
            ? nullptr : this->fontPath.c_str();
        (void)font;
        // load Noto Sans / Roboto if present (mangohud-class legibility)
        ImFontConfig fc{};
        fc.OversampleH = 2; fc.OversampleV = 2;
        bool fontLoaded = false;
        for (const char* cand : {
                "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                "/usr/share/fonts/TTF/NotoSans-Regular.ttf",
                "/usr/share/fonts/noto-sans/NotoSans-Regular.ttf",
                "/usr/share/fonts/roboto/Roboto-Regular.ttf",
                "/usr/share/fonts/TTF/Roboto-Regular.ttf"}) {
            FILE* f = fopen(cand, "rb");
            if (!f) continue;
            fclose(f);
            /* S40+ unobtrusive pass: 16 px * (H/1080) ≈ 13 px @1440p — small,
           readable, does not sit on the crosshair. */
        const float sz = 16.0f * static_cast<float>(this->baseScale) * 0.5f;
            ImGui::GetIO().Fonts->AddFontFromFileTTF(cand, sz, &fc);
            fontLoaded = true;
            break;
        }
        if (!fontLoaded)
            ImGui::GetIO().Fonts->AddFontDefault();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)this->rtSize.width, (float)this->rtSize.height);
        // Vulkan backend init (own descriptor pool, own renderpass)
        ImGui_ImplVulkan_InitInfo init{};
        init.Instance = vk.inst();
        init.PhysicalDevice = vk.physdev();
        init.Device = vk.dev();
        init.QueueFamily = vk.queueFamilyIndex();
        init.Queue = vk.queue();
        init.RenderPass = this->renderpass;
        init.MinImageCount = 2; init.ImageCount = 2;
        init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init.DescriptorPoolSize = 8;
        init.PipelineCache = vk.cache();
        init.CheckVkResultFn = nullptr;
        if (!ImGui_ImplVulkan_Init(&init))
            throw ls::error("imgui_hud: ImGui_ImplVulkan_Init failed");
        if (!ImGui_ImplVulkan_CreateFontsTexture())
            throw ls::error("imgui_hud: font upload failed");
        this->init = true;
        g_allocDone.store(true);
    }

    ImGuiHud::~ImGuiHud() {
        if (!this->init) return;
        vkQueueWaitIdle(this->vk.queue());
        ImGui_ImplVulkan_Shutdown();
        for (uint8_t i = 0; i < 2; ++i)
            if (this->framebuffer[i])
                if (auto dfb = devPfn<PFN_vkDestroyFramebuffer>(vk, "vkDestroyFramebuffer"); dfb)
                    dfb(vk.dev(), this->framebuffer[i], nullptr);
        if (this->renderpass)
            if (auto drp = devPfn<PFN_vkDestroyRenderPass>(vk, "vkDestroyRenderPass"); drp)
                drp(vk.dev(), this->renderpass, nullptr);
        ImGui::DestroyContext();
    }

    void ImGuiHud::setupThemeAndFont() {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;
        style.WindowRounding = 8.0f;
        style.FrameRounding = 5.0f;
        style.ChildRounding = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding = 5.0f;
        style.WindowBorderSize = 1.0f;
        style.ItemSpacing = ImVec2(10.f, 6.f);
        const ImVec4 purple{ 0.54f, 0.17f, 0.89f, 1.00f };
        const ImVec4 purpleH{ 0.61f, 0.19f, 1.00f, 1.00f };
        colors[ImGuiCol_Text]          = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
        colors[ImGuiCol_TextDisabled]  = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
        colors[ImGuiCol_WindowBg]      = ImVec4(0.10f, 0.11f, 0.14f, 0.86f); // translucent under game
        colors[ImGuiCol_ChildBg]       = ImVec4(0.16f, 0.17f, 0.21f, 0.90f);
        colors[ImGuiCol_Border]        = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
        colors[ImGuiCol_FrameBg]       = ImVec4(0.13f, 0.15f, 0.18f, 1.00f);
        colors[ImGuiCol_PlotLines]     = purple;
        colors[ImGuiCol_PlotHistogram] = purple;
        colors[ImGuiCol_PlotLinesHovered] = purpleH;
        colors[ImGuiCol_PlotHistogramHovered] = purpleH;
        colors[ImGuiCol_Button]        = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = purpleH;
        colors[ImGuiCol_Header]        = purple;
    }

    void ImGuiHud::ensureFade(float dt) {
        const bool want = g_visible.load();
        const float speed = 6.0f;
        if (want) this->fadeLock = std::min(1.0f, this->fadeLock + speed * dt);
        else      this->fadeLock = std::max(0.0f, this->fadeLock - speed * dt);
        g_alpha.store(this->fadeLock);
    }

    void ImGuiHud::tick(float dt) {
        this->ensureFade(dt);
        if (this->fadeLock <= 0.02f) return;          // zero-cost hidden path
        // pick inactive slot + fence discipline (S40: submit WITH fence, wait
        // BLOCKING on the fence of the slot we are about to overwrite)
        const uint8_t next = static_cast<uint8_t>(this->active ^ 1);
        auto& fence = *this->rtFence[next];
        if (this->rtFenceSignaled[next]) {
            (void)fence.wait(this->vk, UINT64_MAX);
            fence.reset(this->vk);
        }
        // ---- imgui frame -------------------------------------------------
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        this->drawWidgets(latest());
        ImGui::Render();
        // ---- record RT renderpass + imgui vertices ------------------------
        this->cmdbuf.begin(this->vk);
        VkClearValue clear{ .color = { .float32 = { 0.f, 0.f, 0.f, 0.f } } };
        VkRenderPassBeginInfo rbi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = this->renderpass,
            .framebuffer = this->framebuffer[next],
            .renderArea = { { 0, 0 }, { this->rtSize.width, this->rtSize.height } },
        };
        rbi.clearValueCount = 1;
        rbi.pClearValues = &clear;
        if (auto brp = devPfn<PFN_vkCmdBeginRenderPass>(vk, "vkCmdBeginRenderPass"); brp)
            brp(this->cmdbuf.raw(), &rbi, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), this->cmdbuf.raw());
        if (auto erp = devPfn<PFN_vkCmdEndRenderPass>(vk, "vkCmdEndRenderPass"); erp)
            erp(this->cmdbuf.raw());
        this->cmdbuf.end(this->vk);
        this->cmdbuf.submit(this->vk, {}, VK_NULL_HANDLE, 0,
            {}, VK_NULL_HANDLE, 0, fence.handle());
        this->rtFenceSignaled[next] = true;
        this->rtGeneral[next] = true;
        this->rtLastAccess[next] = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        this->active = next;
    }

    void ImGuiHud::drawWidgets(const Stats& s) {
        ImGuiIO& io = ImGui::GetIO();
        /* S40+ review pass: NeverClip + corner-anchor. The card auto-sizes to
           its content; anchored by the (1,0) pivot so it hugs the top-right
           with a 12 px inset on ANY resolution. No scrollbar, ever. */
        ImGui::SetNextWindowPos(ImVec2(
            static_cast<float>(this->rtSize.width) - 12.f, 12.f),
            ImGuiCond_Always, ImVec2(1.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 5.f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 2.f));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.f);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0,0,0,0));
        ImGui::Begin("lsfg-vk", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoNav);
        // FPS table: label dim-left, value white-right (tabular alignment)
        ImGui::TextColored(ImVec4(0.60f, 0.63f, 0.70f, 1.00f), "game");
        ImGui::SameLine(56.f);
        ImGui::TextUnformatted("fps");
        ImGui::SameLine(78.f);
        ImGui::Text("%u", static_cast<unsigned>(std::lround(s.gameFps)));
        ImGui::TextColored(ImVec4(0.60f, 0.63f, 0.70f, 1.00f), "doubled");
        ImGui::SameLine(56.f);
        ImGui::TextUnformatted("fps");
        ImGui::SameLine(78.f);
        if (s.presentedFps > s.gameFps + 1.f)
            ImGui::Text("%u  (2x)", static_cast<unsigned>(std::lround(s.presentedFps)));
        else
            ImGui::Text("%u", static_cast<unsigned>(std::lround(s.presentedFps)));
        // frametime sparkline, fixed y-range (0..25 ms) — the shape can't lie
        if (s.frameTimesCount > 2) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.60f, 0.63f, 0.70f, 1.00f),
                "frametime (ms)");
            ImGui::PlotLines("##ft", s.frameTimesMs,
                static_cast<int>(s.frameTimesCount),
                static_cast<int>(s.frameTimesIdx % 180),
                nullptr, 0.f, 25.f,
                ImVec2(ImGui::GetContentRegionAvail().x, 15.f));
        }
        // click->photon percentiles from the live ring (dash while empty)
        ImGui::Spacing();
        if (s.latencySamplesCount >= 8) {
            float ls[256];
            for (uint32_t k = 0; k < s.latencySamplesCount; ++k)
                ls[k] = s.latencySamples[k];
            std::sort(ls, ls + s.latencySamplesCount);
            const float p50 = ls[s.latencySamplesCount / 2];
            const float p99 = ls[static_cast<uint32_t>(
                std::min<uint32_t>(255, s.latencySamplesCount * 99 / 100))];
            ImGui::TextColored(ImVec4(0.60f, 0.63f, 0.70f, 1.00f),
                "latency");
            ImGui::SameLine();
            ImGui::Text("p50 %.1f p99 %.1f",
                static_cast<double>(p50), static_cast<double>(p99));
        }
        // pipeline segments (I=ipc G=gen S=scan E=GEN adds), fixed 4 slots
        // width so the line never wraps: E shows "--" when not live.
        char pipe[160];
        if (s.genExtraLive)
            std::snprintf(pipe, sizeof(pipe), "I %.1f  G %.1f  S %.1f  E %+.1f",
                static_cast<double>(s.ipcMs), static_cast<double>(s.genMs),
                static_cast<double>(s.scanMs), static_cast<double>(s.genExtraMs));
        else
            std::snprintf(pipe, sizeof(pipe), "I %.1f  G %.1f  S %.1f  E--",
                static_cast<double>(s.ipcMs), static_cast<double>(s.genMs),
                static_cast<double>(s.scanMs));
        ImGui::TextDisabled("%s", pipe);
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }

    VkImage ImGuiHud::rtImage() const { return this->rt[this->active]->handle(); }
    VkExtent2D ImGuiHud::rtExtent() const { return this->rtSize; }
    float ImGuiHud::blitAlpha() const { return g_alpha.load(); }
    VkAccessFlags ImGuiHud::lastAccess() const { return this->rtLastAccess[this->active]; }
    void ImGuiHud::markRead() { this->rtLastAccess[this->active] = VK_ACCESS_TRANSFER_READ_BIT; }
    ImGuiHud::Origin ImGuiHud::origin() const {
        return Origin {
            static_cast<int32_t>(this->outExtent.width) -
                static_cast<int32_t>(this->rtSize.width) - 8,
            8,
        };
    }

} // namespace ls::hud
