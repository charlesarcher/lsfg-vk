/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-app/hud.hpp"

#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/vulkan/buffer.hpp"

#include <chrono>
#include <cctype>
#include <algorithm>
#include <vector>

namespace ls::hud {
namespace {
    /// seven-segment digit in a 5x7 cell (x:0..4, y:0..6)
    enum Seg : uint8_t {
        SEG_A = 1, SEG_B = 2, SEG_C = 4, SEG_D = 8,
        SEG_E = 16, SEG_F = 32, SEG_G = 64,
    };
    constexpr uint8_t kSegment[7] = { SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F, SEG_G };
    /// segment set per digit
    constexpr uint8_t kDigit[10] = {
        SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,                    // 0
        SEG_B | SEG_C,                                                    // 1
        SEG_A | SEG_B | SEG_G | SEG_E | SEG_D,                            // 2
        SEG_A | SEG_B | SEG_G | SEG_C | SEG_D,                            // 3
        SEG_F | SEG_G | SEG_B | SEG_C,                                    // 4
        SEG_A | SEG_F | SEG_G | SEG_C | SEG_D,                            // 5
        SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D,                    // 6
        SEG_A | SEG_B | SEG_C,                                            // 7
        SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,            // 8
        SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,                    // 9
    };
    /// the pixels of one segment within the 5x7 cell
    bool segPixel(uint8_t seg, int x, int y) {
        switch (seg) {
        case SEG_A: return y == 0 && x >= 1 && x <= 3;
        case SEG_B: return x == 3 && (y == 1 || y == 2);
        case SEG_C: return x == 3 && (y == 4 || y == 5);
        case SEG_D: return y == 6 && x >= 1 && x <= 3;
        case SEG_E: return x == 1 && (y == 4 || y == 5);
        case SEG_F: return x == 1 && (y == 1 || y == 2);
        case SEG_G: return y == 3 && x >= 1 && x <= 3;
        }
        return false;
    }
    /// the '.' glyph in a 5x7 cell: a single 2x2 dot at the bottom-left
    bool dotPixel(int x, int y) {
        return (x == 0 || x == 1) && (y == 5 || y == 6);
    }
    /// the '-' glyph in a 5x7 cell: the middle horizontal bar
    bool dashPixel(int x, int y) {
        return y == 3 && x >= 1 && x <= 3;
    }
    /// the '+' glyph in a 5x7 cell: cross in the center
    bool plusPixel(int x, int y) {
        return (x == 2 && y >= 1 && y <= 5) || (y == 3 && x >= 1 && x <= 3);
    }
    /// the '/' glyph in a 5x7 cell (a 1-2 px thick diagonal)
    bool slashPixel(int x, int y) {
        switch (y) {
        case 0: return x == 4;
        case 1: return x == 3 || x == 4;
        case 2: return x == 3;
        case 3: return x == 2;
        case 4: return x == 1;
        case 5: return x == 0;
        default: return false;
        }
    }
    /// generic 5x7 bitmap glyphs (bit4 = leftmost pixel) for HUD labels
    struct Glyph57 { uint8_t rows[7]; };
    constexpr Glyph57 kGlyphs[]{
        {{0x1F,0x10,0x1E,0x10,0x10,0x10,0x10}}, // F
        {{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}}, // P
        {{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, // S
        {{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}}, // L
        {{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}}, // I
        {{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}}, // G
        {{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, // C
        {{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}}, // D
        {{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, // M
        {{0x11,0x19,0x15,0x13,0x11,0x11,0x11}}, // N
        {{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, // A
        {{0x02,0x04,0x08,0x10,0x08,0x04,0x02}}, // V
        {{0x07,0x05,0x05,0x05,0x09,0x0D,0x11}}, // R — approximate
    };
    bool labelPixel(char c, int x, int y) {
        const char* set = "FPSLIGCDMNAV R";
        for (int i = 0; set[i]; ++i)
            if (set[i] == c && c != ' ') {
                const uint8_t row = kGlyphs[i > 12 ? 12 : i].rows[y & 7];
                return x < 5 && ((row >> (4 - x)) & 1u) != 0;
            }
        return false;
    }
    bool glyphPixel(char c, int x, int y) {
        if (c >= 'A' && c <= 'Z')
            return labelPixel(c, x, y);
        if (c == '.')
            return dotPixel(x, y);
        if (c == '-')
            return dashPixel(x, y);
        if (c == '+')
            return plusPixel(x, y);
        if (c >= '0' && c <= '9') {
            const uint8_t segs = kDigit[static_cast<size_t>(c - '0')];
            for (const uint8_t s : kSegment)
                if ((segs & s) != 0 && segPixel(s, x, y))
                    return true;
            return false;
        }
        if (c == '/')
            return slashPixel(x, y);
        return false;
    }
    // box / text colors (rgb)
    constexpr uint8_t kBox[3] = { 14, 16, 22 };
    constexpr uint8_t kText[3] = { 240, 245, 250 };

    /// store one pixel into the buffer in the memory byte order of @p format
    void storePxl(VkFormat format, uint8_t* p, uint8_t r, uint8_t g, uint8_t b) {
        if (format == VK_FORMAT_B8G8R8A8_UNORM) {
            p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
        } else { // A8B8G8R8_UNORM_PACK32
            p[0] = 255; p[1] = b; p[2] = g; p[3] = r;
        }
    }
} // namespace

Hud::Hud(const vk::Vulkan& vk, uint32_t outputHeight, VkFormat format)
    : vk(vk), format(format), cmdbuf(vk) {
    if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_A8B8G8R8_UNORM_PACK32)
        throw ls::error("hud: unsupported swapchain format "
            + std::to_string(static_cast<int>(format)));
    // display scale: legible at 1080p, grows with the output resolution
    uint32_t s = outputHeight / 240;
    s = s < 3 ? 3 : (s > 10 ? 10 : s);
    this->scale = s;
    // the box is sized for 6 glyph cells ("NN/NNN") plus (Session 40) an
    // optional second latency row — the box is permanently 2x11 rows tall;
    // update() centers the single row when no second row is rendered.
    this->boxExtent = VkExtent2D{ 34 * s, 22 * s };
    for (uint8_t i = 0; i < 2; ++i) {
        this->slotImage[i].emplace(vk, this->boxExtent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        this->slotFence[i].emplace(vk);
    }
}

void Hud::update(std::string_view text) {
    this->update(text, {});
}

void Hud::update(std::string_view text, std::string_view second) {
    this->twoRows = !second.empty();
    const uint8_t next = static_cast<uint8_t>(this->active ^ 1);
    auto& img = *this->slotImage[next];
    auto& fence = *this->slotFence[next];

    // the slot was last uploaded a full stats interval (>=1 s) ago, so this
    // wait is the fast path; the bound keeps a wedged upload from stalling the
    // present loop indefinitely. a slot's fence is never-signaled until its
    // first upload, so the wait is skipped on the slot's first use.
    if (this->slotFenceSignaled[next] && !fence.wait(this->vk, 100ULL * 1000 * 1000))
        throw ls::vulkan_error(VK_TIMEOUT, "hud: upload fence wait timed out");
    fence.reset(this->vk);

    std::vector<uint8_t> px(static_cast<size_t>(this->boxExtent.width)
        * this->boxExtent.height * 4);
    // first (fps) row centered in the top half when the second row is in
    // use, else centered in the whole box (single-row layout).
    if (this->twoRows) {
        this->rasterize(text, px, true /* topHalf */);
        this->rasterize(second, px, false /* bottomHalf */);
    } else {
        this->rasterize(text, px, false /* center whole box */);
    }

    const vk::Buffer buf(this->vk, px.data(), px.size(),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    this->cmdbuf.begin(this->vk);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = this->slotGeneral[next] ? this->slotLastAccess[next]
                                                    : VK_ACCESS_NONE;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = this->slotGeneral[next] ? VK_IMAGE_LAYOUT_GENERAL
                                                : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img.handle();
    barrier.subresourceRange = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };
    this->vk.df().CmdPipelineBarrier(this->cmdbuf.raw(),
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier);
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { this->boxExtent.width, this->boxExtent.height, 1 },
    };
    this->vk.df().CmdCopyBufferToImage(this->cmdbuf.raw(), buf.handle(),
        img.handle(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    this->cmdbuf.end(this->vk);
    this->cmdbuf.submit(this->vk,
        {}, VK_NULL_HANDLE, 0,
        {}, VK_NULL_HANDLE, 0,
        fence.handle()
    );
    this->slotGeneral[next] = true;
    this->slotFenceSignaled[next] = true;
    this->slotLastAccess[next] = VK_ACCESS_TRANSFER_WRITE_BIT;
    this->active = next;
}

void Hud::rasterize(std::string_view text, std::vector<uint8_t>& out,
        bool topHalf) const {
    const uint32_t w = this->boxExtent.width;
    const uint32_t h = this->twoRows ? this->boxExtent.height / 2 : this->boxExtent.height;
    const uint32_t yOff = topHalf ? 0 : this->boxExtent.height / 2;
    /* S40 FIX: clear ONLY this row's half — the old loop wiped the whole
       box each call, so the second rasterize() erased the fps row (the
       overlay showed latency digits only and the fps row read 'empty'). */
    for (uint32_t y = yOff; y < yOff + h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            storePxl(this->format,
                out.data() + (static_cast<size_t>(y) * w + x) * 4,
                kBox[0], kBox[1], kBox[2]);

    const uint32_t len = text.empty() ? 1 : static_cast<uint32_t>(text.size());
    // fit the text into the fixed box: scale down for longer texts, never
    // beyond the box scale
    uint32_t se = (this->scale * 6) / len;
    if (se > this->scale)
        se = this->scale;
    if (se < 1)
        se = 1;
    const uint32_t textW = 5 * len * se, textH = 7 * se;
    const uint32_t x0 = (w - textW) / 2, y0 = yOff + (h - textH) / 2;
    for (uint32_t c = 0; c < len && c < text.size(); ++c) {
        for (int gx = 0; gx < 5; ++gx) {
            for (int gy = 0; gy < 7; ++gy) {
                if (!glyphPixel(text[c], gx, gy))
                    continue;
                for (uint32_t sx = 0; sx < se; ++sx) {
                    for (uint32_t sy = 0; sy < se; ++sy) {
                        const uint32_t x = x0 + c * 5 * se + static_cast<uint32_t>(gx) * se + sx;
                        const uint32_t y = y0 + static_cast<uint32_t>(gy) * se + sy;
                        storePxl(this->format,
                            out.data() + (static_cast<size_t>(y) * w + x) * 4,
                            kText[0], kText[1], kText[2]);
                    }
                }
            }
        }
    }
}

} // namespace ls::hud