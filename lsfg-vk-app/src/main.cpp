/* SPDX-License-Identifier: GPL-3.0-or-later */

// lsfg-vk-app: the receiving side of one-way (external) dual-GPU frame
// generation. It binds the IPC listener, completes the handshake with the
// task-4 layer, holds per-stream state, and opens real cross-device
// frame-generation contexts via the lsfg-vk backend. WSI/presentation
// (creating the output swapchain on the processing GPU) is a later task;
// this only needs a real vk::Vulkan on the processing device plus a backend
// Instance so it can negotiate, import staging, and open a context.

// glibc keeps struct sigaction / sigemptyset behind __USE_POSIX, which is only
// enabled when a POSIX feature-test macro is defined before any include.
#define _POSIX_C_SOURCE 200809L

#include "lsfg-vk-app/stream.hpp"

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/paths.hpp"
#include "lsfg-vk-common/ipc/socket.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <atomic>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <getopt.h> // NOLINT (IWYU)
#include <bits/getopt_core.h>
#include <bits/getopt_ext.h>

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vk_layer.h>

namespace {
    /// shutdown flag flipped by the SIGINT handler
    std::atomic<bool> g_stop{false};
    /// write end of the SIGINT self-pipe; the handler signals via it
    int g_selfPipeWrite{-1};

    /// process-level frame-gen backend instance (never freed: makeLeaking).
    /// created once before the accept loop, alive for the whole run. its
    /// selection predicate mirrors the vk::Vulkan PhysicalDeviceSelector: the
    /// profile 'gpu' key (deviceName | ids | pci).
    static lsfgvk::backend::Instance* g_backend{nullptr};

    /// SIGINT handler: write a byte to the self-pipe so the blocking accept()
    /// poll() returns (SIGPIPE is ignored so a peer's death does not kill us).
    void onSigInt(int /*sig*/) noexcept {
        char b{'S'};
        ssize_t n{};
        do {
            n = ::write(g_selfPipeWrite, &b, 1);
        } while (n < 0 && errno == EINTR);
    }

    /// format a 32-bit id as "0xVVVV:0xDDDD" (mirrors instance.cpp)
    std::string toHexId(uint32_t id) {
        static constexpr std::array<char, 17> chars = std::to_array("0123456789ABCDEF");
        std::string result = "0x";
        result += chars.at((id >> 12) & 0xF);
        result += chars.at((id >> 8) & 0xF);
        result += chars.at((id >> 4) & 0xF);
        result += chars.at(id & 0xF);
        return result;
    }

    /// identity tuple of a physical device: name | ids | pci bus id
    struct DeviceId {
        std::string name;
        std::string ids;
        std::optional<std::string> pci;
    };

    /// probe a physical device's identity, matching the layer's probeDevice so
    /// the app can select its processing GPU the same way the profile 'gpu' key
    /// is documented (deviceName | ids | pci)
    DeviceId probeDevice(const vk::VulkanInstanceFuncs& fi, VkPhysicalDevice dev) {
        uint32_t count{};
        fi.EnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> exts(count);
        fi.EnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data());

        bool hasPci{false};
        for (const auto& e : exts) {
            if (std::string(std::to_array(e.extensionName).data())
                    == VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)
                hasPci = true;
        }

        VkPhysicalDeviceIDProperties idProps{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES
        };
        VkPhysicalDevicePCIBusInfoPropertiesEXT pciInfo{};
        VkPhysicalDeviceProperties2 props{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = hasPci ? &pciInfo : nullptr
        };
        fi.GetPhysicalDeviceProperties2(dev, &props);

        std::array<char, 256> devname = std::to_array(props.properties.deviceName);
        devname.at(255) = '\0';

        DeviceId d{};
        d.name = std::string(devname.data());
        d.ids = toHexId(props.properties.vendorID) + ":" + toHexId(props.properties.deviceID);
        if (hasPci && (pciInfo.pciBus != 0 || pciInfo.pciDevice != 0
                || pciInfo.pciFunction != 0))
            d.pci = std::to_string(pciInfo.pciBus) + ":" +
                std::to_string(pciInfo.pciDevice) + "." +
                std::to_string(pciInfo.pciFunction);
        return d;
    }

    /// whether a probed device matches the profile 'gpu' selector
    bool matchesSelector(const DeviceId& d, const std::string& gpu) {
        return d.name == gpu
            || d.ids == gpu
            || (d.pci.has_value() && *d.pci == gpu);
    }

    /// query the device name of the wrapped physical device (for the banner)
    std::string deviceName(const vk::Vulkan& vk) {
        VkPhysicalDeviceProperties2 props{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
        };
        vk.fi().GetPhysicalDeviceProperties2(vk.physdev(), &props);
        std::array<char, 256> devname = std::to_array(props.properties.deviceName);
        devname.at(255) = '\0';
        return std::string(devname.data());
    }

    /// print usage (used for help and as a suffix to named errors)
    void usage(const char* prog) {
        const std::string text =
            std::string("lsfg-vk-app - receiving side of one-way external dual-GPU frame generation.\n\n")
            + "USAGE:\n    " + prog + " [OPTIONS]\n\n"
            + "OPTIONS:\n"
            + "    -p, --profile <name>    REQUIRED: profile selecting the processing GPU"
            + "    -o, --output <name>     OPTIONAL: output name for later presentation tasks"
            + "    -v, --verbose           Verbose per-frame logging"
            + "    -h, --help              Show this help\n";
        std::cerr << text;
    }

    struct Options {
        std::optional<std::string> profile;
        std::optional<std::string> output;
        bool verbose{false};
    };

    /// parse CLI args; --profile is REQUIRED (hard-named error if absent)
    Options parseArgs(int argc, char** argv) {
        Options opts{};

        const std::array<option, 4> GETOPT {{
            { "profile",   required_argument, nullptr, 'p' },
            { "output",    required_argument, nullptr, 'o' },
            { "verbose",     no_argument,       nullptr, 'v' },
            { "help",        no_argument,       nullptr, 'h' }
        }};

        int c{};
        while ((c = getopt_long(argc, argv, "vo:h", GETOPT.data(), nullptr)) != -1) {
            switch (c) {
                case 'p': opts.profile.emplace(optarg); break;
                case 'o': opts.output.emplace(optarg); break;
                case 'v': opts.verbose = true; break;
                case 'h': usage(*argv); std::exit(EXIT_SUCCESS);
                case '?':
                default:
                    usage(*argv);
                    std::exit(EXIT_FAILURE);
            }
        }

        if (!opts.profile.has_value()) {
            std::cerr << "lsfg-vk-app: --profile <name> is required\n\n";
            usage(*argv);
            std::exit(EXIT_FAILURE);
        }

        return opts;
    }

    /// process-wide output override (from --output); selectProfile applies it
    /// to the chosen profile before returning it
    std::optional<std::string> g_outputOverride;

    /// load conf.toml and select the profile by its name; hard error if the
    /// name is absent or matches more than one profile (names must be unique)
    ls::GameConf selectProfile(const std::string& name) {
        const auto path = ls::findConfigurationFile();
        ls::ConfigFile config{path};

        const auto& profiles = config.profiles();
        std::size_t matchCount{};
        const ls::GameConf* selected{nullptr};
        for (const auto& p : profiles) {
            if (p.name == name) {
                if (++matchCount > 1)
                    break;
                selected = &p;
            }
        }
        if (selected == nullptr)
            throw ls::error("no profile named '" + name + "' in " + path.string());
        if (matchCount > 1)
            throw ls::error("multiple profiles named '" + name + "' in " + path.string()
                + "; profile names must be unique");

        // config is destroyed on return, so copy out first and apply the
        // --output override to the copy; the caller must never observe a
        // reference into freed storage.
        ls::GameConf result = *selected;
        if (g_outputOverride.has_value())
            result.output = g_outputOverride;

        return result;
    }
}

int main(int argc, char** argv) {
    try {
        const Options opts = parseArgs(argc, argv);
        g_outputOverride = opts.output;

        const ls::GameConf conf = selectProfile(*opts.profile);
        if (!conf.gpu.has_value())
            throw ls::error("profile '" + conf.name + "' has no 'gpu'; the app needs one to "
                "select the processing device");

        // --- create a real vk::Vulkan on the processing GPU ----------------
        // the layer is an ICD-loader GLOBAL layer, so without disabling it
        // here it would inject into THIS process's instance. mirror the layer's
        // own disable dance (instance.cpp:354/386/393): setenv before the
        // nest-load, unsetenv on BOTH the success and the failure paths so a
        // failed construction can never leave the env altered.
        setenv("DISABLE_LSFGVK", "1", 1);

        std::optional<vk::Vulkan> vk;
        try {
            vk.emplace(
                "lsfg-vk-app", vk::version{2, 0, 0},
                "lsfg-vk-app-engine", vk::version{2, 0, 0},
                [&conf](const vk::VulkanInstanceFuncs& fi,
                        const std::vector<VkPhysicalDevice>& devices) -> VkPhysicalDevice {
                    const std::string& wanted = *conf.gpu;
                    for (const VkPhysicalDevice& dev : devices) {
                        const auto id = probeDevice(fi, dev);
                        if (matchesSelector(id, wanted))
                            return dev;
                    }
                    throw ls::error("failed to find processing GPU '" + wanted + "'");
                },
                false, // isGraphical (app-ctor creates compute-capable device funcs;
                       // swapchain funcs are null anyway until a later task)
                std::nullopt,   // setLoaderData
                std::nullopt,   // cachefile
                true            // enableDmaBufExtensions (full exchange extension set)
            );
        } catch (const std::exception& e) {
            unsetenv("DISABLE_LSFGVK");
            throw ls::error("failed to create processing device for profile '"
                + conf.name + "'", e);
        }
        unsetenv("DISABLE_LSFGVK");

        const std::string devName = deviceName(*vk);

        // --- create the process-level frame-gen backend instance -----------
        // The backend dlopens the shader DLL and must select the SAME processing
        // GPU the vk::Vulkan above targets (match profile.gpu via
        // deviceName | ids | pci, mirroring the selector lambda above). mirror
        // the vk::Vulkan disable dance: setenv before the ctor, unsetenv on
        // BOTH the success and the failure paths. makeLeaking() is the loader
        // workaround that prevents the Vulkan loader from trying to destroy the
        // instance/device (a known loader bug) - do it once, right after the
        // backend is constructed.
        ls::ConfigFile cfg{ls::findConfigurationFile()};
        const auto& g = cfg.global();
        const std::filesystem::path shaderDll =
            g.dll.has_value() ? std::filesystem::path(*g.dll) : ls::findShaderDll();
        {
            const char* kDisable = "DISABLE_LSFGVK";
            setenv(kDisable, "1", 1);
            bool backendOk{false};
            try {
                g_backend = new lsfgvk::backend::Instance{
                    [conf = conf](const std::string& name,
                                  std::pair<const std::string&, const std::string&> ids,
                                  const std::optional<std::string>& pci) {
                        const std::string& wanted = *conf.gpu;
                        return (name == wanted)
                            || (ids.first + ":" + ids.second) == wanted
                            || (pci.has_value() && *pci == wanted);
                    },
                    shaderDll,
                    g.allow_fp16,
                    true  // enableDmaBufExtensions=true (cross-device path)
                };
                lsfgvk::backend::makeLeaking();
                backendOk = true;
            } catch (const std::exception& e) {
                unsetenv(kDisable);
                throw ls::error("failed to create frame-gen backend for profile '"
                    + conf.name + "'", e);
            }
            unsetenv(kDisable);
            (void)backendOk;
        }

        // --- bind the listener + SIGINT self-pipe --------------------------
        struct sigaction sa{};
        sa.sa_handler = onSigInt;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        if (sigaction(SIGINT, &sa, nullptr) < 0)
            throw ls::error("sigaction(SIGINT) failed", std::runtime_error(std::strerror(errno)));
        // a peer closing the socket must not kill the app (the layer's present
        // hook handles EPIPE itself); a lost peer surfaces as a stream error
        struct sigaction saPipe{}; // C++ needs the 'struct' keyword here
        saPipe.sa_handler = SIG_IGN;
        sigemptyset(&saPipe.sa_mask);
        if (sigaction(SIGPIPE, &saPipe, nullptr) < 0)
            throw ls::error("sigaction(SIGPIPE) failed", std::runtime_error(std::strerror(errno)));

        int selfPipe[2]{};
        if (::pipe(selfPipe) < 0)
            throw ls::error("pipe() failed", std::runtime_error(std::strerror(errno)));
        g_selfPipeWrite = selfPipe[1];

        ls::ipc::Listener listener{ls::ipc::Listener::defaultPath()};

        // SO_RCVTIMEO bounds accept() so a SIGINT that lands mid-accept cannot
        // hang it forever (accept() on the common socket loops on EINTR)
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 500'000;
        if (::setsockopt(listener.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
            throw ls::error("setsockopt(SO_RCVTIMEO) on listener", std::runtime_error(std::strerror(errno)));

        std::cerr << "lsfg-vk-app: listening on " << ls::ipc::Listener::defaultPath().string()
                  << " (processing on '" << devName << "')\n";

        // --- accept loop ---------------------------------------------------
        // registry of live streams keyed by accepted connection fd. each entry
        // owns its staging fds and closes them when the entry is erased below.
        std::map<int, ls::ipc::StreamState> streams;

        while (!g_stop.load()) {
            pollfd pfd[2] = {
                { selfPipe[0], POLLIN, 0 },
                { listener.fd(), POLLIN, 0 }
            };
            const int r = ::poll(pfd, 2, 200);
            if (r < 0) {
                if (errno == EINTR) continue;
                throw ls::ipc::socket_error("poll() on listener", errno);
            }
            if (r == 0) continue;                 // 200ms timeout: re-check stop
            if (pfd[0].revents & POLLIN) {        // SIGINT: drain and break
                char b{};
                while (::read(selfPipe[0], &b, 1) > 0) {}
                break;
            }
            if (!(pfd[1].revents & POLLIN)) continue;

            // accept() returns a Connection by value (move-only, no default
            // ctor); bind it before touching the registry so a failed accept
            // cannot leave a stray fd open.
            try {
                auto conn = listener.accept();

                // own the stream's registry entry for the connection's lifetime;
                // erasing it after runStream returns destroys the StreamState,
                // which closes every staging fd the layer handed off (leak-free)
                const int key = conn.fd();
                auto it = streams.emplace(key, ls::ipc::StreamState{});
                if (!it.second) {
                    // fd collision (a prior fd was reused before this entry was
                    // erased) - drop this connection's stream
                    std::cerr << "lsfg-vk-app: dropped stream on fd " << key << "\n";
                    streams.erase(it.first);
                    continue;
                }
                try {
                    ls::ipc::runStream(conn, it.first->second, g_stop, *vk, *g_backend, conf);
                } catch (const std::exception& e) {
                    std::cerr << "lsfg-vk-app: stream ended: " << e.what() << "\n";
                }
                streams.erase(it.first);  // StreamState dtor closes stored fds
            } catch (const ls::ipc::socket_error& e) {
                if (g_stop.load())
                    break;
                std::cerr << "lsfg-vk-app: accept failed: " << e.what() << "\n";
            }
        }

        // --- shutdown: RAII closes the Listener socket + unlinks the file,
        //     and destroys the vk::Vulkan (device + instance) ---------------
        std::cerr << "lsfg-vk-app: shutting down\n";
    } catch (const std::exception& e) {
        std::cerr << "lsfg-vk-app: fatal: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
