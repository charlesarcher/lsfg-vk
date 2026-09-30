/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

// One-frame color measurement. Off unless LSFGVK_COLOR_DUMP=1.
// Writes /tmp/lsfg-color/<stage>.ppm (format-correct RGB), .rgba (packed
// memory bytes), and .txt (format, fourcc, modifier, the byte order written).
// Does not change the copy. A caller that needs the source layout preserved
// must pass that layout as oldLayout; the readback restores it.

#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <vector>

#include <sys/stat.h>
#include <vulkan/vulkan_core.h>

namespace ls {

inline bool colorDumpOn() {
    static const bool on = [] {
        const char* e = std::getenv("LSFGVK_COLOR_DUMP");
        return e && e[0] == '1' && !e[1];
    }();
    return on;
}

inline uint64_t colorDumpFidx() {
    static const uint64_t id = [] {
        const char* e = std::getenv("LSFGVK_COLOR_DUMP_FIDX");
        if (!e || !e[0])
            return 80ull;
        return std::strtoull(e, nullptr, 10);
    }();
    return id;
}

inline bool colorDumpWant(uint64_t fidx) {
    static const std::vector<uint64_t> ids = [] {
        std::vector<uint64_t> out;
        const char* list = std::getenv("LSFGVK_COLOR_DUMP_FIDXS");
        if (list && list[0]) {
            const char* p = list;
            while (*p) {
                char* end = nullptr;
                const unsigned long long v = std::strtoull(p, &end, 10);
                if (end == p)
                    break;
                out.push_back(static_cast<uint64_t>(v));
                p = end;
                if (*p == ',')
                    ++p;
            }
        }
        if (out.empty())
            out.push_back(colorDumpFidx());
        return out;
    }();
    for (const uint64_t id : ids)
        if (id == fidx)
            return true;
    return false;
}

inline const char* vkFormatName(int fmt) {
    switch (fmt) {
    case 37: return "R8G8B8A8_UNORM";
    case 43: return "R8G8B8A8_SRGB";
    case 44: return "B8G8R8A8_UNORM";
    case 50: return "B8G8R8A8_SRGB";
    default: return "other";
    }
}

struct PpmMap {
    int r;
    int g;
    int b;
    const char* order;
};

// Format-correct: BGRA memory byte 2 is red. RGBA memory byte 0 is red.
inline PpmMap ppmMap(int fmt) {
    if (fmt == 44 || fmt == 50)
        return { 2, 1, 0,
            "PPM R=mem[2] G=mem[1] B=mem[0] (B8G8R8A8 format-correct)" };
    if (fmt == 37 || fmt == 43)
        return { 0, 1, 2,
            "PPM R=mem[0] G=mem[1] B=mem[2] (R8G8B8A8 format-correct)" };
    return { 0, 1, 2,
        "PPM R=mem[0] G=mem[1] B=mem[2] (unrecognized format, raw byte0 as R)" };
}

inline void writeColorStage(const char* stage, uint32_t w, uint32_t h,
        const uint8_t* px, int fmt, uint64_t fidx,
        const char* fourcc, const char* modifier) {
    ::mkdir("/tmp/lsfg-color", 0755);
    const PpmMap map = ppmMap(fmt);
    char ppmPath[160], rawPath[160], metaPath[160];
    std::snprintf(ppmPath, sizeof(ppmPath), "/tmp/lsfg-color/%s-f%llu.ppm",
        stage, static_cast<unsigned long long>(fidx));
    std::snprintf(rawPath, sizeof(rawPath), "/tmp/lsfg-color/%s-f%llu.rgba",
        stage, static_cast<unsigned long long>(fidx));
    std::snprintf(metaPath, sizeof(metaPath), "/tmp/lsfg-color/%s-f%llu.txt",
        stage, static_cast<unsigned long long>(fidx));
    const size_t npx = static_cast<size_t>(w) * h;
    uint64_t rs = 0, gs = 0, bs = 0;
    FILE* out = std::fopen(ppmPath, "wb");
    if (!out) {
        std::fprintf(stderr, "color-dump stage=%s fopen failed %s\n", stage, ppmPath);
        return;
    }
    std::fprintf(out, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> rgb(npx * 3);
    for (size_t i = 0; i < npx; ++i) {
        const uint8_t r = px[i * 4 + map.r];
        const uint8_t g = px[i * 4 + map.g];
        const uint8_t b = px[i * 4 + map.b];
        rgb[i * 3 + 0] = r;
        rgb[i * 3 + 1] = g;
        rgb[i * 3 + 2] = b;
        rs += r;
        gs += g;
        bs += b;
    }
    std::fwrite(rgb.data(), 1, rgb.size(), out);
    std::fclose(out);
    if (FILE* raw = std::fopen(rawPath, "wb")) {
        std::fwrite(px, 1, npx * 4, raw);
        std::fclose(raw);
    }
    const uint64_t mr = npx ? rs / npx : 0;
    const uint64_t mg = npx ? gs / npx : 0;
    const uint64_t mb = npx ? bs / npx : 0;
    if (FILE* meta = std::fopen(metaPath, "w")) {
        std::fprintf(meta,
            "stage=%s fidx=%llu %ux%u VkFormat=%d (%s)\n"
            "fourcc=%s\nmodifier=%s\norder=%s\n"
            "packed=w*4 assumed\nmean-ppm-RGB %llu %llu %llu\n"
            "first-pixel mem %u %u %u %u\n",
            stage, (unsigned long long)fidx, w, h, fmt, vkFormatName(fmt),
            fourcc ? fourcc : "n/a", modifier ? modifier : "n/a", map.order,
            (unsigned long long)mr, (unsigned long long)mg, (unsigned long long)mb,
            px[0], px[1], px[2], px[3]);
        std::fclose(meta);
    }
    std::fprintf(stderr,
        "color-dump stage=%s fidx=%llu fmt=%d(%s) fourcc=%s modifier=%s order=%s mean-ppm-RGB %llu %llu %llu file=%s\n",
        stage, (unsigned long long)fidx, fmt, vkFormatName(fmt),
        fourcc ? fourcc : "n/a", modifier ? modifier : "n/a", map.order,
        (unsigned long long)mr, (unsigned long long)mg, (unsigned long long)mb,
        ppmPath);
    std::fflush(stderr);
}

inline VkImageMemoryBarrier colorBarrier(VkImage image,
        VkAccessFlags srcAccess, VkAccessFlags dstAccess,
        VkImageLayout oldLayout, VkImageLayout newLayout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = srcAccess,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1
        }
    };
}

// Copy src to a same-format host image, wait, write the stage files.
// Restores src to oldLayout. Returns false on failure; does not throw.
inline bool colorDumpReadback(const vk::Vulkan& vk, VkQueue queue,
        VkImage src, VkFormat format, VkExtent2D extent, VkImageLayout oldLayout,
        const char* stage, uint64_t fidx,
        const char* fourcc, const char* modifier) {
    const size_t nb = static_cast<size_t>(extent.width) * extent.height * 4;
    if (nb == 0)
        return false;
    void* p = nullptr;
    if (::posix_memalign(&p, 4096, nb) != 0) {
        std::fprintf(stderr, "color-dump stage=%s memalign failed\n", stage);
        return false;
    }
    std::memset(p, 0, nb);
    try {
        vk::Image dump(vk, extent, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            p, nb);
        vk::CommandBuffer cb(vk);
        cb.begin(vk);
        cb.copyImage(vk,
            {
                colorBarrier(src, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_READ_BIT,
                    oldLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                colorBarrier(dump.handle(), VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
            },
            { src, dump.handle() },
            extent,
            {
                colorBarrier(src, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, oldLayout),
            });
        cb.end(vk);
        vk::Fence fence(vk);
        cb.submit(vk, {}, VK_NULL_HANDLE, 0, {}, VK_NULL_HANDLE, 0,
            fence.handle(), queue);
        if (!fence.wait(vk, UINT64_MAX)) {
            std::fprintf(stderr, "color-dump stage=%s fence timeout\n", stage);
            std::fflush(stderr);
            ::free(p);
            return false;
        }
        writeColorStage(stage, extent.width, extent.height,
            static_cast<const uint8_t*>(p), static_cast<int>(format), fidx,
            fourcc, modifier);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "color-dump stage=%s failed: %s\n", stage, e.what());
        std::fflush(stderr);
        ::free(p);
        return false;
    }
    ::free(p);
    return true;
}

} // namespace ls
