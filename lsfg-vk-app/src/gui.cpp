#include "gui.hpp"

#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/paths.hpp"

#include <iostream>
#include <thread>
#include <chrono>

namespace lsfgvk::gui {

    GuiState g_guiState;
    static std::thread g_serverThread;
    static std::atomic<bool> g_serverStop{false};

    void stopServerWorker() {
        if (g_guiState.serviceRunning.load()) {
            g_serverStop.store(true);
            g_guiState.serviceRunning.store(false);
            if (g_serverThread.joinable()) {
                g_serverThread.join();
            }
        }
    }

    void startServerWorker(const std::string& profileName, const std::string& sessionMode) {
        stopServerWorker();
        g_serverStop.store(false);
        g_guiState.serviceRunning.store(true);
        g_serverThread = std::thread([profileName, sessionMode]() {
            try {
                // Run headless server instance in background thread
                std::string cmd = "/home/archerc/code/lsfg-vk/build/lsfg-vk-app/lsfg-vk-app -p \"" + profileName + "\"";
                if (!sessionMode.empty() && sessionMode != "auto") {
                    cmd += " -s " + sessionMode;
                }
                std::cerr << "lsfg-vk-gui: starting server: " << cmd << "\n";
                // When g_serverStop fires or process exits, update flag
                FILE* pipe = popen(cmd.c_str(), "r");
                if (pipe) {
                    char buf[256];
                    while (!g_serverStop.load() && fgets(buf, sizeof(buf), pipe)) {
                        // Forward server output
                    }
                    pclose(pipe);
                }
            } catch (...) {}
            g_guiState.serviceRunning.store(false);
        });
    }

    static void setupDarkTheme() {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        // Window & Layout
        style.WindowRounding = 8.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 5.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding = 5.0f;
        style.TabRounding = 6.0f;
        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.ItemSpacing = ImVec2(10.0f, 8.0f);
        style.FramePadding = ImVec2(8.0f, 5.0f);

        // WinUI 3 Dark & Purple Accent Palette
        colors[ImGuiCol_Text]                  = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
        colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
        colors[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.11f, 0.14f, 1.00f); // #1a1d24
        colors[ImGuiCol_ChildBg]               = ImVec4(0.16f, 0.17f, 0.21f, 1.00f); // #282c35
        colors[ImGuiCol_PopupBg]               = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
        colors[ImGuiCol_Border]                = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
        colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg]               = ImVec4(0.13f, 0.15f, 0.18f, 1.00f); // #21252d
        colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
        colors[ImGuiCol_FrameBgActive]         = ImVec4(0.22f, 0.24f, 0.29f, 1.00f);
        colors[ImGuiCol_TitleBg]               = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
        colors[ImGuiCol_TitleBgActive]         = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);
        colors[ImGuiCol_MenuBarBg]             = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);
        colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.10f, 0.11f, 0.14f, 0.60f);
        colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.30f, 0.34f, 0.42f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.38f, 0.43f, 0.52f, 1.00f);

        // Purple Accents (#8a2be2 / #9b30ff)
        const ImVec4 purpleAccent             = ImVec4(0.54f, 0.17f, 0.89f, 1.00f);
        const ImVec4 purpleHover              = ImVec4(0.61f, 0.19f, 1.00f, 1.00f);
        const ImVec4 purpleActive             = ImVec4(0.70f, 0.25f, 1.00f, 1.00f);

        colors[ImGuiCol_CheckMark]             = purpleAccent;
        colors[ImGuiCol_SliderGrab]            = purpleAccent;
        colors[ImGuiCol_SliderGrabActive]      = purpleActive;
        colors[ImGuiCol_Button]                = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
        colors[ImGuiCol_ButtonHovered]         = purpleHover;
        colors[ImGuiCol_ButtonActive]          = purpleActive;
        colors[ImGuiCol_Header]                = purpleAccent;
        colors[ImGuiCol_HeaderHovered]         = purpleHover;
        colors[ImGuiCol_HeaderActive]          = purpleActive;
        colors[ImGuiCol_Separator]             = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
    }

    static void populateConfigData() {
        try {
            const auto confPath = ls::findConfigurationFile();
            ls::ConfigFile config{confPath};
            std::lock_guard<std::mutex> lk(g_guiState.mtx);
            g_guiState.availableProfiles.clear();
            for (const auto& p : config.profiles()) {
                g_guiState.availableProfiles.push_back(p.name);
            }
        } catch (...) {}

        // Known home-lab devices on kennykiller
        std::lock_guard<std::mutex> lk(g_guiState.mtx);
        g_guiState.availableGpus = {
            {"AMD Radeon RX 9060 XT (RADV GFX1200)", "0000:08:00.0 (renderD130)"},
            {"Intel(R) Graphics (ARL)", "0000:00:02.0 (renderD128)"},
            {"AMD Radeon RX 9070 XT (RADV GFX1201)", "0000:04:00.0 (renderD129)"},
        };
    }

    int runGui(int argc, char** argv) {
        if (!glfwInit()) {
            std::cerr << "lsfg-vk-app: failed to initialize GLFW\n";
            return 1;
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        GLFWwindow* window = glfwCreateWindow(980, 840, "Lossless Scaling Frame Generator (lsfg-vk)", nullptr, nullptr);
        if (!window) {
            std::cerr << "lsfg-vk-app: failed to create GLFW window\n";
            glfwTerminate();
            return 1;
        }

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1); // Enable vsync for UI

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        setupDarkTheme();
        populateConfigData();

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330");

        while (!glfwWindowShouldClose(window) && !g_guiState.shouldClose.load()) {
            glfwPollEvents();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            int displayW, displayH;
            glfwGetFramebufferSize(window, &displayW, &displayH);

            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(displayW), static_cast<float>(displayH)));
            ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;

            ImGui::Begin("MainPanel", nullptr, windowFlags);

            // ── Header Bar ─────────────────────────────────────────────────────────────
            ImGui::TextColored(ImVec4(0.70f, 0.35f, 1.00f, 1.0f), "LOSSLESS SCALING FRAME GENERATION (lsfg-vk)");
            ImGui::SameLine(ImGui::GetWindowWidth() - 220);
            if (g_guiState.serviceRunning.load()) {
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "[ SERVICE ACTIVE ]");
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "[ SERVICE STANDBY ]");
            }
            ImGui::Separator();
            ImGui::Spacing();

            // ── Telemetry Banner ───────────────────────────────────────────────────────
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
            ImGui::BeginChild("TelemetryBar", ImVec2(0, 65), true);
            ImGui::Columns(4, "telemetry_cols", false);
            ImGui::TextDisabled("Rendered (Real) Rate");
            ImGui::Text("%.1f FPS (%.2f ms)", g_guiState.currentFpsReal.load(), g_guiState.currentLatencyRealMs.load());
            ImGui::NextColumn();
            ImGui::TextDisabled("Doubled Output Rate");
            ImGui::TextColored(ImVec4(0.6f, 0.3f, 1.0f, 1.0f), "%.1f Presents/s (%.2f ms)",
                               g_guiState.currentFpsGen.load(), g_guiState.currentLatencyGenMs.load());
            ImGui::NextColumn();
            ImGui::TextDisabled("Total Presentations");
            ImGui::Text("%llu (Real: %llu, Gen: %llu)",
                        static_cast<unsigned long long>(g_guiState.totalPresents.load()),
                        static_cast<unsigned long long>(g_guiState.totalRealPresents.load()),
                        static_cast<unsigned long long>(g_guiState.totalGenPresents.load()));
            ImGui::NextColumn();
            ImGui::TextDisabled("Active Stream");
            ImGui::Text("%s", g_guiState.streamActive.load() ? "Connected (1440p)" : "No Stream Connected");
            ImGui::Columns(1);
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Spacing();

            // ── 2-Column Modular Cards ─────────────────────────────────────────────────
            const float colWidth = (ImGui::GetContentRegionAvail().x - 16.0f) * 0.5f;

            // ── Left Column ────────────────────────────────────────────────────────────
            ImGui::BeginChild("LeftColumn", ImVec2(colWidth, 0), false, ImGuiWindowFlags_NoScrollbar);

            // Card 1: Frame Generation
            ImGui::BeginChild("CardFG", ImVec2(0, 210), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::Text("Frame Generation");
            ImGui::Separator();
            ImGui::Spacing();

            const char* fgTypes[] = { "LSFG 3.x", "LSFG 2.x", "LSFG 1.x", "Off" };
            static int fgTypeIdx = 0;
            ImGui::Combo("Type", &fgTypeIdx, fgTypes, IM_ARRAYSIZE(fgTypes));

            const char* fgModes[] = { "Fixed", "Fractional", "Adaptive" };
            static int fgModeIdx = 0;
            ImGui::Combo("Mode", &fgModeIdx, fgModes, IM_ARRAYSIZE(fgModes));

            int mult = g_guiState.multiplier;
            if (ImGui::SliderInt("Multiplier", &mult, 2, 4)) {
                g_guiState.multiplier = mult;
            }

            int flow = g_guiState.flowScale;
            if (ImGui::SliderInt("Flow scale", &flow, 25, 100, "%d%%")) {
                g_guiState.flowScale = flow;
            }

            bool perf = g_guiState.performanceMode;
            if (ImGui::Checkbox("Performance Mode", &perf)) {
                g_guiState.performanceMode = perf;
            }
            ImGui::EndChild();

            ImGui::Spacing();

            // Card 2: Capture
            ImGui::BeginChild("CardCapture", ImVec2(0, 130), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::Text("Capture");
            ImGui::Separator();
            ImGui::Spacing();

            const char* capApis[] = { "Vulkan Layer Hook (vkQueuePresentKHR)", "Wayland DMABUF", "X11 Composite" };
            static int capApiIdx = 0;
            ImGui::Combo("Capture API", &capApiIdx, capApis, IM_ARRAYSIZE(capApis));

            int ring = g_guiState.ringDepth;
            if (ImGui::SliderInt("Queue target (Ring)", &ring, 1, 8)) {
                g_guiState.ringDepth = ring;
            }
            ImGui::EndChild();

            ImGui::Spacing();

            // Card 3: Cursor
            ImGui::BeginChild("CardCursor", ImVec2(0, 170), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::Text("Cursor");
            ImGui::Separator();
            ImGui::Spacing();
            static bool clipCursor = false;
            static bool adjSpeed = false;
            static bool hideCursor = false;
            static bool scaleCursor = false;
            ImGui::Checkbox("Clip cursor", &clipCursor);
            ImGui::Checkbox("Adjust cursor speed", &adjSpeed);
            ImGui::Checkbox("Hide cursor", &hideCursor);
            ImGui::Checkbox("Scale cursor", &scaleCursor);
            ImGui::EndChild();

            ImGui::EndChild(); // LeftColumn

            ImGui::SameLine();

            // ── Right Column ───────────────────────────────────────────────────────────
            ImGui::BeginChild("RightColumn", ImVec2(colWidth, 0), false, ImGuiWindowFlags_NoScrollbar);

            // Card 4: GPU & Display
            ImGui::BeginChild("CardGPU", ImVec2(0, 150), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::Text("GPU & Display");
            ImGui::Separator();
            ImGui::Spacing();

            std::vector<const char*> gpuNames;
            {
                std::lock_guard<std::mutex> lk(g_guiState.mtx);
                for (const auto& dev : g_guiState.availableGpus) {
                    gpuNames.push_back(dev.name.c_str());
                }
            }
            int gpuIdx = g_guiState.selectedGpuIndex;
            if (!gpuNames.empty()) {
                if (ImGui::Combo("Preferred GPU", &gpuIdx, gpuNames.data(), static_cast<int>(gpuNames.size()))) {
                    g_guiState.selectedGpuIndex = gpuIdx;
                }
            }

            const char* sessions[] = { "Wayland (native)", "X11 (XCB)", "Auto" };
            static int sessionIdx = 0;
            if (ImGui::Combo("Session Mode", &sessionIdx, sessions, IM_ARRAYSIZE(sessions))) {
                if (sessionIdx == 0) g_guiState.sessionMode = "wayland";
                else if (sessionIdx == 1) g_guiState.sessionMode = "x11";
                else g_guiState.sessionMode = "auto";
            }
            ImGui::EndChild();

            ImGui::Spacing();

            // Card 5: Rendering & Pacing
            ImGui::BeginChild("CardRendering", ImVec2(0, 225), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::Text("Rendering");
            ImGui::Separator();
            ImGui::Spacing();

            const char* syncModes[] = { "Off (Allow tearing / Immediate)", "Mailbox (Fast sync)", "FIFO (VSync)" };
            static int syncIdx = 0;
            ImGui::Combo("Sync mode", &syncIdx, syncModes, IM_ARRAYSIZE(syncModes));

            static int maxLatency = 4;
            ImGui::SliderInt("Max frame latency", &maxLatency, 1, 10);

            static bool hdr = false;
            static bool gsync = true;
            static bool drawFps = true;
            ImGui::Checkbox("HDR support", &hdr);
            ImGui::Checkbox("VRR / Adaptive Sync", &gsync);
            ImGui::Checkbox("Draw FPS overlay (MangoHud)", &drawFps);
            ImGui::EndChild();

            ImGui::Spacing();

            // Card 6: Profile & Action
            ImGui::BeginChild("CardAction", ImVec2(0, 150), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::Text("Active Profile & Control");
            ImGui::Separator();
            ImGui::Spacing();

            std::vector<const char*> profileNames;
            int curProfIdx = 0;
            {
                std::lock_guard<std::mutex> lk(g_guiState.mtx);
                for (size_t i = 0; i < g_guiState.availableProfiles.size(); ++i) {
                    profileNames.push_back(g_guiState.availableProfiles[i].c_str());
                    if (g_guiState.availableProfiles[i] == g_guiState.activeProfile) {
                        curProfIdx = static_cast<int>(i);
                    }
                }
            }
            if (!profileNames.empty()) {
                if (ImGui::Combo("Profile", &curProfIdx, profileNames.data(), static_cast<int>(profileNames.size()))) {
                    std::lock_guard<std::mutex> lk(g_guiState.mtx);
                    g_guiState.activeProfile = g_guiState.availableProfiles[curProfIdx];
                }
            }

            ImGui::Spacing();
            if (ImGui::Button("Launch / Restart Stream Service", ImVec2(-1, 35))) {
                std::string prof;
                std::string sess;
                {
                    std::lock_guard<std::mutex> lk(g_guiState.mtx);
                    prof = g_guiState.activeProfile;
                    sess = g_guiState.sessionMode;
                }
                startServerWorker(prof, sess);
            }
            ImGui::EndChild();

            ImGui::EndChild(); // RightColumn

            ImGui::End();

            ImGui::Render();
            glViewport(0, 0, displayW, displayH);
            glClearColor(0.10f, 0.11f, 0.14f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            static int frameCount = 0;
            if (++frameCount == 5 || std::getenv("LSFGVK_UI_DUMP")) {
                std::vector<uint8_t> pixels(static_cast<size_t>(displayW) * displayH * 4);
                glReadPixels(0, 0, displayW, displayH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                if (FILE* f = std::fopen("/tmp/lsfg_ui_window.ppm", "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", displayW, displayH);
                    std::vector<uint8_t> rgb(static_cast<size_t>(displayW) * displayH * 3);
                    for (int y = 0; y < displayH; ++y) {
                        for (int x = 0; x < displayW; ++x) {
                            const size_t srcIdx = (static_cast<size_t>(displayH - 1 - y) * displayW + x) * 4;
                            const size_t dstIdx = (static_cast<size_t>(y) * displayW + x) * 3;
                            rgb[dstIdx + 0] = pixels[srcIdx + 0];
                            rgb[dstIdx + 1] = pixels[srcIdx + 1];
                            rgb[dstIdx + 2] = pixels[srcIdx + 2];
                        }
                    }
                    std::fwrite(rgb.data(), 1, rgb.size(), f);
                    std::fclose(f);
                    std::cerr << "lsfg-vk-app: dumped GUI framebuffer to /tmp/lsfg_ui_window.ppm\n";
                }
            }

            glfwSwapBuffers(window);
        }

        g_guiState.shouldClose.store(true);
        stopServerWorker();

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

} // namespace lsfgvk::gui
