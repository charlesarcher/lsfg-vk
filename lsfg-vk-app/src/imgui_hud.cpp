/* SPDX-License-Identifier: GPL-3.0-or-later */

/* S40+ Phase A — dear imgui overlay for the doubler window.
 * Offscreen render target on the app device, ticked at low rate from the
 * present thread; drawn into the swapchain by the same blit path as hud.cpp.
 * Toggle = SIGUSR1 / app.sock control op (fade); hidden = zero present cost.
 * Standing fence discipline (S40): EVERY submit signals its fence; any wait
 * on an unrefreshed slot parks forever. */

#include "lsfg-vk-app/imgui_hud.hpp"
#include "imgui_hud_pm_spv.hpp"
#include <algorithm>
#include <cfloat>
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




namespace {
    VkImageMemoryBarrier makeImgBarrier(VkImage img, VkAccessFlags srcAcc,
            VkImageLayout srcLayout, VkAccessFlags dstAcc, VkImageLayout dstLayout) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = srcAcc;
        b.dstAccessMask = dstAcc;
        b.oldLayout = srcLayout;
        b.newLayout = dstLayout;
        b.image = img;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return b;
    }
} // namespace
namespace ls::hud {
    static std::atomic<float> wsMinW{ 0.f };   /* sticky text-driven width */

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
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
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
        /* S40+ FIX: dependency must be EXTERNAL→subpass0 (zero-init made
           srcSubpass=0 = a SELF-dep with framebuffer-space src + non-fb
           dst = invalid; RADV SEGV'd on it inside the PM/rp path). */
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        VkSubpassDependency dep2{};
        dep2.srcSubpass = 0;
        dep2.dstSubpass = VK_SUBPASS_EXTERNAL;
        dep2.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep2.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        VkSubpassDependency deps[2] = { dep, dep2 };
        VkRenderPassCreateInfo rpci{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &color,
            .subpassCount = 1, .pSubpasses = &sub,
            .dependencyCount = 2, .pDependencies = deps };
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
        /* S40+ PM pass: premultiplied-rgb variant pipelined in-tick; the
           present-side blit samples the PM image so the swapchain's
           PRE_MULTIPLIED compositeAlpha truly blends the card 40%. */
        this->buildPmPass();
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

    
    void ImGuiHud::buildPmPass() {
        /* dedicated PM output image (3rd slot, not the 2-slot rotation) */
        this->pmImg = std::make_unique<vk::Image>(this->vk, this->rtSize,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT);
        /* linear-clamped sampler (1:1 blit scale — filter irrelevant) */
        VkSamplerCreateInfo sci{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
        PFN_vkCreateSampler cSamp = devPfn<PFN_vkCreateSampler>(vk, "vkCreateSampler");
        if (!cSamp || cSamp(vk.dev(), &sci, nullptr, &this->pmSampler) != VK_SUCCESS)
            throw ls::error("imgui_hud: PM sampler failed");
        /* S40+ PM pass: fullscreen triangle pass over a "shadow" RT? No: we
           don't pay a second RT: emit PM in the SAME descriptor blit stage
           — this helper pass writes the PM pixel values into a SECONDARY
           image used as the blit source. To keep present-path simple and
           deterministic, the PM pass runs INSIDE tick() writing rt[1-a]
           as the PM variant; presentation's drawHud blits rt[1-a]. */
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dsl{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1, .pBindings = &b };
        PFN_vkCreateDescriptorSetLayout cDsl = devPfn<PFN_vkCreateDescriptorSetLayout>(vk, "vkCreateDescriptorSetLayout");
        VkDescriptorSetLayout layout{};
        if (!cDsl || cDsl(vk.dev(), &dsl, nullptr, &layout) != VK_SUCCESS)
            throw ls::error("imgui_hud: PM descriptor layout failed");
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.offset = 0, pc.size = 4;      /* float fadeMul */
        VkPipelineLayoutCreateInfo plci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &pc };
        PFN_vkCreatePipelineLayout cPl = devPfn<PFN_vkCreatePipelineLayout>(vk, "vkCreatePipelineLayout");
        if (!cPl || cPl(vk.dev(), &plci, nullptr, &this->pmPipelineLayout) != VK_SUCCESS)
            throw ls::error("imgui_hud: PM pipeline layout failed");
        /* (layout is owned by pmPipelineLayout's set; store the raw handle
           for descriptor alloc below.) */
        this->pmSetLayout = layout;
        VkShaderModule vs{}, fs{};
        PFN_vkCreateShaderModule cSm = devPfn<PFN_vkCreateShaderModule>(vk, "vkCreateShaderModule");
        VkShaderModuleCreateInfo sm{ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        sm.codeSize = sizeof(pmVert); sm.pCode = pmVert;
        if (!cSm || cSm(vk.dev(), &sm, nullptr, &vs) != VK_SUCCESS)
            throw ls::error("imgui_hud: PM vert shader failed");
        sm.codeSize = sizeof(pmFrag); sm.pCode = pmFrag;
        if (!cSm || cSm(vk.dev(), &sm, nullptr, &fs) != VK_SUCCESS) {
            PFN_vkDestroyShaderModule dSm = devPfn<PFN_vkDestroyShaderModule>(vk, "vkDestroyShaderModule");
            if (dSm) dSm(vk.dev(), vs, nullptr);
            throw ls::error("imgui_hud: PM frag shader failed");
        }
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" };
        st[1] = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" };
        VkPipelineVertexInputStateCreateInfo vi{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineViewportStateCreateInfo vp{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineDepthStencilStateCreateInfo ds{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        VkPipelineColorBlendAttachmentState cba{};
        /* the PM math is in the shader: blend = replace (no blend) */
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &cba };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkGraphicsPipelineCreateInfo gpi{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2, .pStages = st,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia,
            .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pDepthStencilState = &ds,
            .pColorBlendState = &cb, .pDynamicState = &dy,
            .layout = this->pmPipelineLayout,
            .renderPass = this->renderpass, .subpass = 0 };
        PFN_vkCreateGraphicsPipelines cGp = devPfn<PFN_vkCreateGraphicsPipelines>(vk, "vkCreateGraphicsPipelines");
        VkPipeline pipe{};
        VkResult res = cGp ? cGp(vk.dev(), VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe)
                           : VK_ERROR_INITIALIZATION_FAILED;
        PFN_vkDestroyShaderModule dSm = devPfn<PFN_vkDestroyShaderModule>(vk, "vkDestroyShaderModule");
        if (dSm) { dSm(vk.dev(), vs, nullptr); dSm(vk.dev(), fs, nullptr); }
        if (res != VK_SUCCESS)
            throw ls::error("imgui_hud: PM graphics pipeline failed");
        this->pmPipeline = pipe;
        /* dedicated PM framebuffer (target = pmImg) */
        {
            VkImageView iv2 = this->pmImg->imageview();       /* owned_ptr → raw */
            VkFramebufferCreateInfo pfci{
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = this->renderpass,
                .attachmentCount = 1, .pAttachments = &iv2,
                .width = this->rtSize.width, .height = this->rtSize.height,
                .layers = 1 };
            PFN_vkCreateFramebuffer pFc = devPfn<PFN_vkCreateFramebuffer>(vk, "vkCreateFramebuffer");
            if (!pFc || pFc(vk.dev(), &pfci, nullptr, &this->pmFramebuffer)
                    != VK_SUCCESS)
                throw ls::error("imgui_hud: PM framebuffer failed");
        }
        /* descriptor pool + sets for both RT ping-pongs */
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 2;
        VkDescriptorPoolCreateInfo pool{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &poolSize };
        PFN_vkCreateDescriptorPool cDp = devPfn<PFN_vkCreateDescriptorPool>(vk, "vkCreateDescriptorPool");
        if (!cDp || cDp(vk.dev(), &pool, nullptr, &this->pmPool) != VK_SUCCESS)
            throw ls::error("imgui_hud: PM descriptor pool failed");
        for (uint8_t i = 0; i < 2; ++i) {
            VkDescriptorImageInfo di{
                .sampler = this->pmSampler,
                .imageView = this->rt[i]->imageview(),
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = VK_NULL_HANDLE, .dstBinding = 0,
                .descriptorCount = 1, .pImageInfo = &di };
            PFN_vkAllocateDescriptorSets cAl = devPfn<PFN_vkAllocateDescriptorSets>(vk, "vkAllocateDescriptorSets");
            VkDescriptorSetAllocateInfo ai{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = this->pmPool,
                .descriptorSetCount = 1, .pSetLayouts = &this->pmSetLayout };
            if (!cAl || cAl(vk.dev(), &ai, &this->pmSet[i]) != VK_SUCCESS)
                throw ls::error("imgui_hud: PM descriptor alloc failed");
            w.dstSet = this->pmSet[i];
            PFN_vkUpdateDescriptorSets cUp = devPfn<PFN_vkUpdateDescriptorSets>(vk, "vkUpdateDescriptorSets");
            if (cUp) cUp(vk.dev(), 1, &w, 0, nullptr);
        }
        this->pmBuilt = true;
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
        colors[ImGuiCol_WindowBg]      = ImVec4(0.10f, 0.11f, 0.14f, 0.40f); // S40+ 40% cardr game
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


    void ImGuiHud::hostDump() {
        if (!this->dumpPending) return;
        /* map + write /tmp/imgui_pm.pam (PPM header + BGRA rows) */
        FILE* f = fopen("/tmp/imgui_pm.pam", "wb");
        if (!f) return;
        std::fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\n"
            "TUPLTYPE RGB_ALPHA\nENDHDR\n", this->rtSize.width, this->rtSize.height);
        uint8_t* p = nullptr;
        if (auto pfn = devPfn<PFN_vkMapMemory>(vk, "vkMapMemory"); pfn)
            pfn(vk.dev(), this->dumpMemory, 0, VK_WHOLE_SIZE, 0,
                reinterpret_cast<void**>(&p));
        if (p) {
            for (uint32_t y = 0; y < this->rtSize.height; ++y)
                fwrite(p + y * this->rtSize.width * 4, 1,
                    this->rtSize.width * 4, f);
        }
        fclose(f);
        std::fprintf(stderr, "imgui_hud: dumped /tmp/imgui_pm.pam\n");
        this->dumpPending = false;
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
            this->hostDump();   /* diag: dump the pm image readback */
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
        /* S40+ PM pass: rt[active] holds straight-alpha imgui output; render
           the PREMULTIPLIED copy into rt[other] with the pm pipeline. The
           present-side blit samples rt[other] (markRead swaps the roles). */
        const bool pmOn = true;   /* S40 DIAG-B: PM on, dump off */
        if (pmOn) {
            /* sample the slot THIS tick's imgui pass just wrote (=next) */
            const uint8_t srcSlot = next;   /* next == imgui output of now */
            const VkImageMemoryBarrier toShader = makeImgBarrier(
                this->rt[srcSlot]->handle(),
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            const VkImageMemoryBarrier bars2[1] = { toShader };
            PFN_vkCmdPipelineBarrier barrier2 = devPfn<PFN_vkCmdPipelineBarrier>(vk, "vkCmdPipelineBarrier");
            if (barrier2)
                barrier2(this->cmdbuf.raw(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, bars2);
            if (auto brp = devPfn<PFN_vkCmdBeginRenderPass>(vk, "vkCmdBeginRenderPass"); brp) {
                VkClearValue zc{ .color = { .float32 = { 0.f, 0.f, 0.f, 0.f } } };
                VkRenderPassBeginInfo rbi2{
                    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                    .renderPass = this->renderpass,
                    .framebuffer = this->pmFramebuffer,   /* dedicated PM image */
                    .renderArea = { { 0, 0 }, { this->rtSize.width, this->rtSize.height } },
                };
                rbi2.clearValueCount = 1;
                rbi2.pClearValues = &zc;
                brp(this->cmdbuf.raw(), &rbi2, VK_SUBPASS_CONTENTS_INLINE);
            }
            if (auto bsc = devPfn<PFN_vkCmdBindPipeline>(vk, "vkCmdBindPipeline"); bsc)
                bsc(this->cmdbuf.raw(), VK_PIPELINE_BIND_POINT_GRAPHICS, this->pmPipeline);
            VkViewport vpM{ 0.f, 0.f,
                static_cast<float>(this->rtSize.width),
                static_cast<float>(this->rtSize.height), 0.f, 1.f };
            VkRect2D scM{ { 0, 0 }, { this->rtSize.width, this->rtSize.height } };
            if (auto sv = devPfn<PFN_vkCmdSetViewport>(vk, "vkCmdSetViewport"); sv)
                sv(this->cmdbuf.raw(), 0, 1, &vpM);
            if (auto ss = devPfn<PFN_vkCmdSetScissor>(vk, "vkCmdSetScissor"); ss)
                ss(this->cmdbuf.raw(), 0, 1, &scM);
            if (auto bds = devPfn<PFN_vkCmdBindDescriptorSets>(vk, "vkCmdBindDescriptorSets"); bds)
                bds(this->cmdbuf.raw(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                    this->pmPipelineLayout, 0, 1, &this->pmSet[srcSlot], 0, nullptr);
            if (auto draw = devPfn<PFN_vkCmdDraw>(vk, "vkCmdDraw"); draw)
                draw(this->cmdbuf.raw(), 3, 1, 0, 0);
            if (auto erp = devPfn<PFN_vkCmdEndRenderPass>(vk, "vkCmdEndRenderPass"); erp)
                erp(this->cmdbuf.raw());
        }
        this->cmdbuf.end(this->vk);
        /* S40+ DIAG dump: LSFGVK_IMGUI_DUMP=1 writes the PM image once. */
        static const bool dumpPm = getenv("LSFGVK_IMGUI_DUMP") != nullptr;
        if (dumpPm && !this->dumpedPmImage) {
            VkBufferCreateInfo bci{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = static_cast<VkDeviceSize>(this->rtSize.width)
                    * this->rtSize.height * 4,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
            VkBuffer buf{ VK_NULL_HANDLE };
            if (auto pfn = devPfn<PFN_vkCreateBuffer>(vk, "vkCreateBuffer"); pfn)
                pfn(vk.dev(), &bci, nullptr, &buf);
            VkMemoryRequirements mr{};
            if (auto pfn = devPfn<PFN_vkGetBufferMemoryRequirements>(vk, "vkGetBufferMemoryRequirements"); pfn)
                pfn(vk.dev(), buf, &mr);
            static const VkMemoryPropertyFlags props =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            VkMemoryAllocateInfo mai{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = mr.size };
            uint32_t mi = 0;
            VkPhysicalDeviceMemoryProperties mp{};
            if (auto pfn = devPfn<PFN_vkGetPhysicalDeviceMemoryProperties>(vk, "vkGetPhysicalDeviceMemoryProperties"); pfn)
                pfn(vk.physdev(), &mp);
            for (uint32_t k2 = 0; k2 < mp.memoryTypeCount; ++k2)
                if ((mr.memoryTypeBits & (1u << k2)) &&
                    (mp.memoryTypes[k2].propertyFlags & props) == props) {
                    mi = k2; break;
                }
            mai.memoryTypeIndex = mi;
            VkDeviceMemory mem{ VK_NULL_HANDLE };
            if (auto pfn = devPfn<PFN_vkAllocateMemory>(vk, "vkAllocateMemory"); pfn)
                pfn(vk.dev(), &mai, nullptr, &mem);
            if (auto pfn = devPfn<PFN_vkBindBufferMemory>(vk, "vkBindBufferMemory"); pfn)
                pfn(vk.dev(), buf, mem, 0);
            // within the SAME cb (before submit): pm image is
            // SHADER_READ after the PM pass: barrier to TRANSFER_SRC:
            VkImageMemoryBarrier bar = makeImgBarrier(this->pmImg->handle(),
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            if (auto pfn = devPfn<PFN_vkCmdPipelineBarrier>(vk, "vkCmdPipelineBarrier"); pfn)
                pfn(this->cmdbuf.raw(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
            VkBufferImageCopy reg{ .bufferOffset = 0,
                .bufferRowLength = this->rtSize.width,
                .bufferImageHeight = this->rtSize.height,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { 0, 0, 0 },
                .imageExtent = { this->rtSize.width, this->rtSize.height, 1 } };
            if (auto pfn = devPfn<PFN_vkCmdCopyImageToBuffer>(vk, "vkCmdCopyImageToBuffer"); pfn)
                pfn(this->cmdbuf.raw(), this->pmImg->handle(),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
            this->dumpedPmImage = true;
            // read after the fence below:
            this->dumpBuffer = buf;
            this->dumpMemory = mem;
            this->dumpPending = true;
        }
        this->cmdbuf.submit(this->vk, {}, VK_NULL_HANDLE, 0,
            {}, VK_NULL_HANDLE, 0, fence.handle());
        this->rtFenceSignaled[next] = true;
        this->rtGeneral[next] = true;
        this->rtLastAccess[next] = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        this->active = next;
        this->pmLastAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    void ImGuiHud::drawWidgets(const Stats& s) {
        ImGuiIO& io = ImGui::GetIO();
        /* S40+ compact contract: the CARD is sized by the TEXT rows and only
           the text rows. We measure the widest text line and pin the window
           width to it BEFORE Begin(); the sparkline (drawn last) then fills
           GetContentRegionAvail().x exactly — full inner width, and it
           CANNOT widen the card because the width is already pinned.
           AlwaysAutoResize handles the height only... with a pinned width
           auto-resize still sizes height to content, which is what we want. */
        char pipePare[128];
        std::snprintf(pipePare, sizeof(pipePare), "I %.1f  G %.1f  S %.1f  E %+.1f",
            88.8, 88.8, 88.8, -88.8);   /* widest possible pipe row */
        char latPare[128];
        std::snprintf(latPare, sizeof(latPare), "p50 %.1f p99 %.1f", 99.9, 99.9);
        float maxW = 0.f;
        maxW = std::max(maxW, ImGui::CalcTextSize("doubled").x
            + ImGui::CalcTextSize(" ").x + ImGui::CalcTextSize("fps").x
            + ImGui::CalcTextSize(" ").x + ImGui::CalcTextSize("88 (2x)").x);
        maxW = std::max(maxW, ImGui::CalcTextSize("markers").x);
        const ImVec2 latSz = ImGui::CalcTextSize(latPare);
        const float latRowW = ImGui::CalcTextSize("latency").x
            + ImGui::CalcTextSize(" ").x + latSz.x;
        maxW = std::max(maxW, latRowW);
        maxW = std::max(maxW, ImGui::CalcTextSize(pipePare).x);
        maxW = std::max(maxW, wsMinW.load());
        const float pad2 = 12.f;             /* 2x WindowPadding.x */
        ImGui::SetNextWindowSizeConstraints(ImVec2(maxW + pad2, 0.f),
            ImVec2(FLT_MAX, FLT_MAX));
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
        // remember the widest ACTUAL row for the next frame's min-width:
        float realMax = 0.f;
        realMax = std::max(realMax, ImGui::GetItemRectSize().x);
        wsMinW.store(std::max(wsMinW.load(), maxW), std::memory_order_relaxed);
        // frametime sparkline fills the pinned inner width exactly:
        if (s.frameTimesCount > 2) {
            ImGui::Spacing();
            ImGui::PlotLines("##ft", s.frameTimesMs,
                static_cast<int>(s.frameTimesCount),
                static_cast<int>(s.frameTimesIdx % 180),
                nullptr, 0.f, 25.f,
                ImVec2(ImGui::GetContentRegionAvail().x, 15.f));
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }

    VkImage ImGuiHud::rtImage() const {
        /* S40: present the PREMULTIPLIED card (pmImg) */
        return this->pmImg->handle();
    }
    VkExtent2D ImGuiHud::rtExtent() const { return this->rtSize; }
    float ImGuiHud::blitAlpha() const { return g_alpha.load(); }
    VkAccessFlags ImGuiHud::lastAccess() const { return this->pmLastAccess; }
    void ImGuiHud::markRead() { this->pmLastAccess = VK_ACCESS_TRANSFER_READ_BIT; }
    ImGuiHud::Origin ImGuiHud::origin() const {
        return Origin {
            static_cast<int32_t>(this->outExtent.width) -
                static_cast<int32_t>(this->rtSize.width) - 8,
            8,
        };
    }

} // namespace ls::hud
