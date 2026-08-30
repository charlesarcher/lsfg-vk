/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <optional>
#include <string>

namespace lsfgvk::cli::copybench {

    /// options for the "copybench" command
    struct Options {
        /// GPU for the frame source/exporter side (required)
        std::string render_gpu;
        /// GPU for the processing/import side (required)
        std::string gpu;
        /// Width of the images (default 1920)
        int width{1920};
        /// Height of the images (default 1080)
        int height{1080};
        /// Use HDR format (R16G16B16A16_SFLOAT)
        bool hdr{false};
        /// Number of copy iterations (default 2000)
        int iters{2000};
        /// Optional timing CSV output path
        std::optional<std::string> timing_csv;
    };

    /// run the "copybench" command
    /// @param opts the command options
    int run(const Options& opts);

}