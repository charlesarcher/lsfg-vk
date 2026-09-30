/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ls {

    /// global configuration
    struct GlobalConf {
        /// optional dll override
        std::optional<std::string> dll;
        /// should fp16 be allowed
        bool allow_fp16{};
        /// default IPC socket path override
        std::optional<std::string> socket_path;
        /// global debug logging
        bool debug{false};
    };

    /// pacing methods
    enum class Pacing : uint8_t {
        /// do not perform any pacing (vsync+novrr)
        None
    };

    /// presentation mode for a profile
    enum class Presentation : uint8_t {
        /// present frames in the game process itself
        Game,
        /// present frames in an external companion process
        External
    };

    /// transport mode for cross-device one-way external presentation
    enum class Transport : uint8_t {
        /// POSIX shared memory CPU copy (CopyHop pixel walk, ~2-6ms latency)
        PosixShm,
        /// DRM PRIME DMA-BUF zero-copy export (parks on implicit-sync ~30ms)
        DmaBuf,
        /// Host-buffer bounce: render GPU DMAs into a shared buffer both
        /// processes have mapped, the app's GPU DMAs out of that same mapping
        /// into the backend's source image. No p2p and no CPU frame copy - the
        /// CPU only moves readiness. This is the path for platforms that do NOT
        /// support cross-GPU import (e.g. kennykiller, where importing the
        /// render card's dma-buf into the doubler GPUVM faults with
        /// MAPPING_ERROR even though pcie_p2p is enabled module-wide).
        /// The enum value keeps its historical name; the old "decoupled
        /// dual-host" implementation was gated on the isolated/fake swapchain
        /// and never ran in the external one-way path.
        DecoupledDma,
        /// udmabuf bounce. The shared slot is one udmabuf in system memory,
        /// imported by BOTH GPUs as a plain VkBuffer. The render side DMAs
        /// with vkCmdCopyImageToBuffer, the app side with
        /// vkCmdCopyBufferToImage, both on a transfer-only family with
        /// bufferRowLength/bufferImageHeight set explicitly.
        ///
        /// This is the kennykiller path and it is proven: 200 iterations at
        /// 2560x1440 across both legs, no p2p and no CPU frame copy
        /// (tools/testing/udmabuf_transport_probe.cpp). The buffer hop is
        /// deliberate - a dma-buf backed LINEAR IMAGE import queries back
        /// with compatibleHandleTypes==0 on this driver, and a buffer has no
        /// modifier or pitch for the two GPUs to disagree about. DmaBuf
        /// (p2p) is unchanged and still selected by transport="dmabuf".
        Udmabuf
    };

    /// game profile configuration
    struct GameConf {
        /// name of the profile
        std::string name{"Profile"};
        /// optional activation string/array
        std::vector<std::string> active_in;
        /// gpu to use (in case of multiple)
        std::optional<std::string> gpu;
        /// multiplier for frame generation
        size_t multiplier{2};
        /// non-inverted flow scale
        float flow_scale{1.00F};
        /// use performance mode
        bool performance_mode{false};
        /// pacing method
        Pacing pacing{Pacing::None};
        /// where the frames get presented
        Presentation presentation{Presentation::Game};
        /// optional output name for external presentation
        std::optional<std::string> output;
        /// transport mode for one-way external presentation
        ls::Transport transport{ls::Transport::PosixShm};
        /// false when the profile did not set transport. Cross-GPU then
        /// selects decoupled. udmabuf is never the default.
        bool transportExplicit{false};
        /// custom IPC socket path override
        std::optional<std::string> socket_path;
        /// emulate isolated/fake swapchain for headless/resizing games
        bool fake_swapchain{false};
        /// use Wayland layer-shell (zwlr_layer_shell_v1)
        bool layer_shell{false};
        /// fullscreen presentation window
        bool fullscreen{true};
        /// show telemetry HUD
        bool hud{true};
        /// enable debug logging
        bool debug{false};
    };

    /// parsed configuration file
    class ConfigFile {
    public:
        /// create a default configuration file at the given path
        /// @param path path to configuration file
        /// @throws ls::error on failure
        static void createDefaultConfigFile(const std::filesystem::path& path);

        /// load the default configuration
        /// @throws ls::error on failure
        ConfigFile();
        /// load configuration from file
        /// @param path path to configuration file
        /// @throws ls::error on failure
        ConfigFile(const std::filesystem::path& path);

        /// get the global configuration
        /// @return global configuration
        [[nodiscard]] auto& global() { return this->globalConf; }
        /// get the game profiles
        /// @return list of game profiles
        [[nodiscard]] auto& profiles() { return this->profileConfs; }

        /// get the global configuration
        /// @return global configuration
        [[nodiscard]] const auto& global() const { return this->globalConf; }
        /// get the game profiles
        /// @return list of game profiles
        [[nodiscard]] const auto& profiles() const { return this->profileConfs; }

        /// write the configuration back to file
        /// @param path path to configuration file
        /// @throws ls::error on failure
        void write(const std::filesystem::path& path) const;
    private:
        GlobalConf globalConf{};
        std::vector<GameConf> profileConfs;
    };

    /// configuration watcher with additional environment support
    class WatchedConfig {
    public:
        /// create a new configuration watcher
        /// @throws ls::error on failure
        WatchedConfig();

        /// reload the configuration from disk if it has changed
        /// @throws ls::error on failure
        /// @return true if the configuration was reloaded
        bool update();

        /// access the underlying configuration file
        /// @return configuration file
        [[nodiscard]] const auto& get() const { return this->configFile; }
    private:
        ConfigFile configFile;

        std::filesystem::path path;
        std::chrono::time_point<std::chrono::file_clock> last_timestamp;
    };

    /// find the configuration file in the most common locations
    /// @return path to configuration file
    std::filesystem::path findConfigurationFile();

    /// Resolve an omitted transport. When the profile did not set one and the
    /// two Vulkan device names differ, select decoupled. An explicit udmabuf
    /// logs the stall warning and is left as the opt-in.
    void resolveUnsetTransport(GameConf& conf,
        std::string_view localDevice, std::string_view otherDevice);

}
