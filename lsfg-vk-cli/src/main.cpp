/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "tools/benchmark.hpp"
#include "tools/copybench.hpp"
#include "tools/debug.hpp"
#include "tools/validate.hpp"

#include <array>
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include <getopt.h> // NOLINT (IWYU)
#include <bits/getopt_core.h>
#include <bits/getopt_ext.h>

using namespace lsfgvk::cli;

namespace {
    /// print usage information
    void usage(const std::string& prog) {
        std::cerr <<
R"(Validate, benchmark, and debug lsfg-vk.

USAGE:
    )" << prog << R"( <COMMAND> [OPTIONS] [ARGS]

COMMANDS:
    validate    Validate a configuration file
    benchmark   Run a benchmark
    copybench   Run dma-buf copy benchmark
    debug       Run lsfg-vk on a set of images

SUBCOMMAND OPTIONS:

    validate
        -c, --config <PATH>             Optional path to the configuration file

    benchmark & debug
        -d, --dll <PATH>                Path to Lossless.dll
        -a, --allow-fp16                Allow FP16 acceleration
        -w, --width <INT>               Width of the input frames
        -h, --height <INT>              Height of the input frames
        --hdr                           Use HDR format (R16G16B16A16_SFLOAT)
        -f, --flow <FLOAT>              Flow scale
        -m, --multiplier <INT>          Multiplier
        -p, --performance-mode          Use performance mode
        -g, --gpu <STRING>              GPU to use
        --timing-csv <PATH>             Write per-frame timing CSV

    benchmark
        -t, --duration <SECONDS>        Benchmark duration in seconds

    copybench
        -r, --render-gpu <STRING>       GPU for the frame source/exporter side (required)
        -g, --gpu <STRING>              GPU for the processing/import side (required)
        -w, --width <INT>               Width of the images (default 1920)
        -h, --height <INT>              Height of the images (default 1080)
        --hdr                           Use HDR format (R16G16B16A16_SFLOAT)
        --iters <INT>                   Number of copy iterations (default 2000)
        --timing-csv <PATH>             Write per-copy timing CSV

    debug
        -r, --render-gpu <STRING>       GPU for the frame source/exporter side
        <folder>                        Path to the debug frames)" << '\n';
    }

    /// parse the validate command options
    [[noreturn]] void on_validate(int argc, char** argv) {
        validate::Options opts{};

        const std::array<option, 3> GETOPT {{
            { "config", required_argument, nullptr, 'c' },
            { nullptr,        no_argument, nullptr,  0  }
        }};

        int c{0};
        while ((c = getopt_long(argc, argv, "c:", GETOPT.data(), nullptr)) != -1) {
            switch (c) {
                case 'c':
                    opts.config.emplace(optarg);
                    break;
                case '?':
                default:
                    usage(*argv);
                    std::exit(EXIT_FAILURE);
            }
        }

        if (optind < argc) {
            usage(*argv);
            std::exit(EXIT_FAILURE);
        }

        std::exit(validate::run(opts));
    }

    /// parse the benchmark command options
    [[noreturn]] void on_benchmark(int argc, char** argv) {
        benchmark::Options opts{};

        const std::array<option, 12> GETOPT {{
            { "dll",              required_argument, nullptr, 'd' },
            { "allow-fp16",       no_argument,       nullptr, 'a' },
            { "width",            required_argument, nullptr, 'w' },
            { "height",           required_argument, nullptr, 'h' },
            { "hdr",              no_argument,       nullptr,  2  },
            { "flow",             required_argument, nullptr, 'f' },
            { "multiplier",       required_argument, nullptr, 'm' },
            { "performance-mode",       no_argument, nullptr, 'p' },
            { "gpu",              required_argument, nullptr, 'g' },
            { "duration",         required_argument, nullptr, 't' },
            { "timing-csv",       required_argument, nullptr,  1  },
            { nullptr,                  no_argument, nullptr,  0  }
        }};

        int c{0};
        while ((c = getopt_long(argc, argv, "d:aw:h:f:m:pg:t:", GETOPT.data(), nullptr)) != -1) {
            switch (c) {
                case 'd':
                    opts.dll.emplace(optarg);
                    break;
                case 'a':
                    opts.allow_fp16 = true;
                    break;
                case 'w':
                    opts.width = std::stoi(optarg);
                    break;
                case 'h':
                    opts.height = std::stoi(optarg);
                    break;
                case 2:
                    opts.hdr = true;
                    break;
                case 'f':
                    opts.flow = std::stof(optarg);
                    break;
                case 'm':
                    opts.multiplier = std::stoi(optarg);
                    break;
                case 'p':
                    opts.performance_mode = true;
                    break;
                case 'g':
                    opts.gpu.emplace(optarg);
                    break;
                case 't':
                    opts.duration = std::stoi(optarg);
                    break;
                case 1:
                    opts.timing_csv.emplace(optarg);
                    break;
                case '?':
                default:
                    usage(*argv);
                    std::exit(EXIT_FAILURE);
            }
        }

        if (optind < argc) {
            usage(*argv);
            std::exit(EXIT_FAILURE);
        }

        std::exit(benchmark::run(opts));
    }

    /// parse the debug command options
    [[noreturn]] void on_debug(int argc, char** argv) {
        debug::Options opts{};

        const std::array<option, 12> GETOPT {{
            { "dll",              required_argument, nullptr, 'd' },
            { "allow-fp16",       no_argument,       nullptr, 'a' },
            { "width",            required_argument, nullptr, 'w' },
            { "height",           required_argument, nullptr, 'h' },
            { "hdr",              no_argument,       nullptr,  2  },
            { "flow",             required_argument, nullptr, 'f' },
            { "multiplier",       required_argument, nullptr, 'm' },
            { "performance-mode",       no_argument, nullptr, 'p' },
            { "gpu",              required_argument, nullptr, 'g' },
            { "render-gpu",       required_argument, nullptr, 'r' },
            { "timing-csv",       required_argument, nullptr,  1  },
            { nullptr,                  no_argument, nullptr,  0  }
        }};

        int c{0};
        while ((c = getopt_long(argc, argv, "d:aw:h:f:m:pg:r:", GETOPT.data(), nullptr)) != -1) {
            switch (c) {
                case 'd':
                    opts.dll.emplace(optarg);
                    break;
                case 'a':
                    opts.allow_fp16 = true;
                    break;
                case 'w':
                    opts.width = std::stoi(optarg);
                    break;
                case 'h':
                    opts.height = std::stoi(optarg);
                    break;
                case 2:
                    opts.hdr = true;
                    break;
                case 'f':
                    opts.flow = std::stof(optarg);
                    break;
                case 'm':
                    opts.multiplier = std::stoi(optarg);
                    break;
                case 'p':
                    opts.performance_mode = true;
                    break;
                case 'g':
                    opts.gpu.emplace(optarg);
                    break;
                case 'r':
                    opts.render_gpu.emplace(optarg);
                    break;
                case 1:
                    opts.timing_csv.emplace(optarg);
                    break;
                case '?':
                default:
                    usage(*argv);
                    std::exit(EXIT_FAILURE);
            }
        }

        if ((optind + 1) != argc) {
            usage(*argv);
            std::exit(EXIT_FAILURE);
        }

        opts.path = argv[optind];

        std::exit(debug::run(opts));
    }

    /// parse the copybench command options
    [[noreturn]] void on_copybench(int argc, char** argv) {
        copybench::Options opts{};

        const std::array<option, 9> GETOPT {{
            { "render-gpu",       required_argument, nullptr, 'r' },
            { "gpu",              required_argument, nullptr, 'g' },
            { "width",            required_argument, nullptr, 'w' },
            { "height",           required_argument, nullptr, 'h' },
            { "hdr",              no_argument,       nullptr,  1  },
            { "iters",            required_argument, nullptr,  2  },
            { "timing-csv",       required_argument, nullptr,  3  },
            { nullptr,                  no_argument, nullptr,  0  }
        }};

        int c{0};
        while ((c = getopt_long(argc, argv, "r:g:w:h:", GETOPT.data(), nullptr)) != -1) {
            switch (c) {
                case 'r':
                    opts.render_gpu = optarg;
                    break;
                case 'g':
                    opts.gpu = optarg;
                    break;
                case 'w':
                    opts.width = std::stoi(optarg);
                    break;
                case 'h':
                    opts.height = std::stoi(optarg);
                    break;
                case 1:
                    opts.hdr = true;
                    break;
                case 2:
                    opts.iters = std::stoi(optarg);
                    break;
                case 3:
                    opts.timing_csv.emplace(optarg);
                    break;
                case '?':
                default:
                    usage(*argv);
                    std::exit(EXIT_FAILURE);
            }
        }

        if (optind < argc) {
            usage(*argv);
            std::exit(EXIT_FAILURE);
        }

        std::exit(copybench::run(opts));
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(*argv);
        return EXIT_FAILURE;
    }

    const std::string command{argv[1]};
    if (command == "validate")
        on_validate(argc - 1, argv + 1);
    else if (command == "benchmark")
        on_benchmark(argc - 1, argv + 1);
    else if (command == "copybench")
        on_copybench(argc - 1, argv + 1);
    else if (command == "debug")
        on_debug(argc - 1, argv + 1);

    usage(*argv);
    return EXIT_FAILURE;
}
