/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "validate.hpp"
#include "lsfg-vk-common/configuration/config.hpp"

#include <exception>
#include <filesystem>
#include <iostream>

using namespace lsfgvk::cli;
using namespace lsfgvk::cli::validate;

int validate::run(const Options& opts) {
    std::filesystem::path path{ls::findConfigurationFile()};
    if (opts.config.has_value())
        path = *opts.config;

    if (!std::filesystem::exists(path)) {
        std::cerr << "Validation failed: configuration file does not exist\n";
        return 1;
    }

    try {
        const ls::ConfigFile config{path};
        for (const auto& profile : config.profiles()) {
            const auto* const presentation = profile.presentation == ls::Presentation::External
                ? "external" : "game";
            std::cerr << "  " << profile.name
                      << ": presentation=" << presentation
                      << ", output=" << (profile.output ? *profile.output : "-") << '\n';
        }
        std::cerr << "Validation success\n";
    } catch (const std::exception& e) {
        std::cerr << "Validation failed: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
