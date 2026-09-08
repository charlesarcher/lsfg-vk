/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "generate.hpp"
#include "../helpers/utils.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <cstddef>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace lsfgvk::backend;

Generate::Generate(const Ctx& ctx, size_t idx,
        const std::vector<vk::Image>& sourceImages,
        const vk::Image& inputImage1,
        const vk::Image& inputImage2,
        const vk::Image& inputImage3,
        const vk::Image& outputImage) {
    // create descriptor sets
    const auto& shader = ctx.hdr ?
        ctx.shaders.get().generate_hdr : ctx.shaders.get().generate;
    // one set per source image; set k binds (prev = sources[(k+N-1)%N],
    // current = sources[k]) and render() picks the set of the current frame
    // (idx % N). for N=2 this reproduces the historic pair-swap sets exactly.
    this->sets.reserve(sourceImages.size());
    for (size_t k = 0; k < sourceImages.size(); ++k) {
        const size_t prev = (k + sourceImages.size() - 1) % sourceImages.size();
        this->sets.emplace_back(ManagedShaderBuilder()
            .sampled(sourceImages.at(prev))
            .sampled(sourceImages.at(k))
            .sampled(inputImage1)
            .sampled(inputImage2)
            .sampled(inputImage3)
            .storage(outputImage)
            .sampler(ctx.bnbSampler)
            .sampler(ctx.eabSampler)
            .buffer(ctx.constantBuffers.at(idx))
            .build(ctx.vk, ctx.pool, shader));
    }

    // store dispatch extent
    this->dispatchExtent = backend::add_shift_extent(ctx.sourceExtent, 15, 4);
}

void Generate::render(const vk::Vulkan& vk, const vk::CommandBuffer& cmd, size_t idx) const {
    this->sets.at(idx % this->sets.size()).dispatch(vk, cmd, this->dispatchExtent);
}
