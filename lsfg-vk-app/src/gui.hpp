#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <optional>
#include <mutex>

namespace lsfgvk::gui {

    struct DeviceInfo {
        std::string name;
        std::string pciId;
    };

    struct GuiState {
        std::atomic<bool> shouldClose{false};
        std::atomic<bool> serviceRunning{false};

        // UI selections & profile state
        std::mutex mtx;
        std::string activeProfile{"app-oneway"};
        std::vector<std::string> availableProfiles;
        std::vector<DeviceInfo> availableGpus;

        // Current profile controls
        int multiplier{2};
        int selectedGpuIndex{0};
        bool performanceMode{false};
        int ringDepth{4};
        int flowScale{100};
        std::string sessionMode{"wayland"}; // wayland, x11, auto

        // Live runtime telemetry
        std::atomic<uint64_t> totalRealPresents{0};
        std::atomic<uint64_t> totalGenPresents{0};
        std::atomic<uint64_t> totalPresents{0};
        std::atomic<float> currentFpsReal{0.0f};
        std::atomic<float> currentFpsGen{0.0f};
        std::atomic<float> currentLatencyRealMs{0.0f};
        std::atomic<float> currentLatencyGenMs{0.0f};
        std::atomic<bool> streamActive{false};
        std::string activeStreamApp{"None"};
        std::string activeStreamRes{"0x0"};
    };

    extern GuiState g_guiState;

    /// Runs the GLFW + Dear ImGui event loop. Returns when the UI window is closed.
    int runGui(int argc, char** argv);

} // namespace lsfgvk::gui
