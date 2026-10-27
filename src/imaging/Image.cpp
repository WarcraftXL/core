// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// PNG through stb_image (deps/stb, MIT or public domain), the resampler, the tint, the mips and the
// BLP2 writer.

#include "imaging/Imaging.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 8192
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace wxl::core::imaging
{
    namespace
    {
        // --- the BLP2 header -------------------------------------------------------------------

        constexpr uint32_t kBlpMagic          = 0x32504C42; // "BLP2"
        constexpr uint8_t  kColorRaw          = 3;          // COLOR_RAW: BGRA, a mip as it is
        constexpr uint8_t  kAlpha8            = 8;
        constexpr uint8_t  kPixelArgb8888     = 2;          // PIXEL_ARGB8888
        constexpr size_t   kMipSlots          = 16;
        constexpr size_t   kOffsetsAt         = 0x14;
        constexpr size_t   kSizesAt           = 0x54;

        void Put32(uint8_t* at, uint32_t value)
        {
            at[0] = static_cast<uint8_t>(value);
            at[1] = static_cast<uint8_t>(value >> 8);
            at[2] = static_cast<uint8_t>(value >> 16);
            at[3] = static_cast<uint8_t>(value >> 24);
        }

        uint32_t Get32(const uint8_t* at)
        {
            return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
                   (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
        }

        /// The levels down to 1x1, as the client counts them (CalcLevelCount).
        uint32_t LevelCount(uint32_t width, uint32_t height)
        {
            uint32_t levels = 1;
            while (width > 1 || height > 1)
            {
                width  = width > 1 ? width >> 1 : 1;
                height = height > 1 ? height >> 1 : 1;
                ++levels;
            }
            return levels;
        }

        /// The next level: each pixel the mean of the (up to) four it covers, premultiplied.
        Picture Halve(const Picture& p)
        {
            Picture out;
            out.width  = p.width > 1 ? p.width >> 1 : 1;
            out.height = p.height > 1 ? p.height >> 1 : 1;
            out.pixels.assign(static_cast<size_t>(out.width) * out.height * 4u, 0.0f);
            const uint32_t fx = p.width > 1 ? 2 : 1, fy = p.height > 1 ? 2 : 1;
            const float    k  = 1.0f / static_cast<float>(fx * fy);
            for (uint32_t y = 0; y < out.height; ++y)
                for (uint32_t x = 0; x < out.width; ++x)
                {
                    float* o = &out.pixels[(static_cast<size_t>(y) * out.width + x) * 4];
                    for (uint32_t j = 0; j < fy; ++j)
                        for (uint32_t i = 0; i < fx; ++i)
                        {
                            const float* s = &p.pixels[(static_cast<size_t>(y * fy + j) * p.width + x * fx + i) * 4];
                            for (int c = 0; c < 4; ++c) o[c] += s[c] * k;
                        }
                }
            return out;
        }

        uint8_t Byte(float v)
        {
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return static_cast<uint8_t>(v * 255.0f + 0.5f);
        }

        /**
         * @brief Writes a level as straight BGRA. A transparent pixel takes the mean colour of its
         *        visible neighbours, grown out a ring at a time, so filtering towards it blends in
         *        the shape's own colour and no black; what no ring reaches takes the level's mean.
         */
        void WriteLevel(const Picture& p, uint8_t* out)
        {
            const size_t       n = static_cast<size_t>(p.width) * p.height;
            std::vector<float> rgb(n * 3, 0.0f);
            std::vector<uint8_t> known(n, 0);
            double sum[3] = {0, 0, 0}, weight = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const float* s = &p.pixels[i * 4];
                const float  a = s[3];
                if (a > 1.0f / 1024.0f)
                {
                    for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = std::min(1.0f, s[c] / a);
                    known[i] = 1;
                    for (int c = 0; c < 3; ++c) sum[c] += rgb[i * 3 + c] * a;
                    weight += a;
                }
            }
            for (int ring = 0; ring < 8; ++ring)
            {
                std::vector<uint8_t> next = known;
                bool                 grew = false;
                for (uint32_t y = 0; y < p.height; ++y)
                    for (uint32_t x = 0; x < p.width; ++x)
                    {
                        const size_t i = static_cast<size_t>(y) * p.width + x;
                        if (known[i]) continue;
                        float acc[3] = {0, 0, 0};
                        int   count  = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                const int nx = static_cast<int>(x) + dx, ny = static_cast<int>(y) + dy;
                                if (nx < 0 || ny < 0 || nx >= static_cast<int>(p.width) || ny >= static_cast<int>(p.height))
                                    continue;
                                const size_t j = static_cast<size_t>(ny) * p.width + static_cast<size_t>(nx);
                                if (!known[j]) continue;
                                for (int c = 0; c < 3; ++c) acc[c] += rgb[j * 3 + c];
                                ++count;
                            }
                        if (!count) continue;
                        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = acc[c] / static_cast<float>(count);
                        next[i] = 1;
                        grew    = true;
                    }
                known.swap(next);
                if (!grew) break;
            }
            const float mean[3] = {weight > 0 ? static_cast<float>(sum[0] / weight) : 0.0f,
                                   weight > 0 ? static_cast<float>(sum[1] / weight) : 0.0f,
                                   weight > 0 ? static_cast<float>(sum[2] / weight) : 0.0f};
            for (size_t i = 0; i < n; ++i)
            {
                const float* c = known[i] ? &rgb[i * 3] : mean;
                out[i * 4 + 0] = Byte(c[2]);
                out[i * 4 + 1] = Byte(c[1]);
                out[i * 4 + 2] = Byte(c[0]);
                out[i * 4 + 3] = Byte(p.pixels[i * 4 + 3]);
            }
        }

        /// Mitchell-Netravali, B = C = 1/3.
        float Mitchell(float x)
        {
            constexpr float B = 1.0f / 3.0f, C = 1.0f / 3.0f;
            x = std::fabs(x);
            if (x < 1.0f)
                return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6.0f;
            if (x < 2.0f)
                return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) /
                       6.0f;
            return 0.0f;
        }

        /// One pass of the separable filter along one axis: `count` lines of `from` samples each
        /// to `to` samples, `stride` floats between two samples of a line, `pitch` between lines.
        struct Tap
        {
            uint32_t index;
            float    weight;
        };

        std::vector<std::vector<Tap>> Taps(uint32_t from, uint32_t to)
        {
            std::vector<std::vector<Tap>> taps(to);
            const float ratio   = static_cast<float>(from) / static_cast<float>(to);
            const float widen   = ratio > 1.0f ? ratio : 1.0f; // shrinking: the filter spans what it covers
            const float support = 2.0f * widen;
            for (uint32_t o = 0; o < to; ++o)
            {
                const float center = (static_cast<float>(o) + 0.5f) * ratio;
                const int   first  = static_cast<int>(std::floor(center - support));
                const int   last   = static_cast<int>(std::ceil(center + support));
                float       total  = 0.0f;
                for (int s = first; s <= last; ++s)
                {
                    const float w = Mitchell((static_cast<float>(s) + 0.5f - center) / widen);
                    if (w == 0.0f) continue;
                    const int clamped = s < 0 ? 0 : (s >= static_cast<int>(from) ? static_cast<int>(from) - 1 : s);
                    taps[o].push_back(Tap{static_cast<uint32_t>(clamped), w});
                    total += w;
                }
                if (total != 0.0f)
                    for (Tap& t : taps[o]) t.weight /= total;
            }
            return taps;
        }
    }

    bool DecodePng(const uint8_t* data, size_t size, Picture* out, std::string* error)
    {
        int      w = 0, h = 0, channels = 0;
        stbi_uc* rgba = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
        if (!rgba)
        {
            if (error) *error = std::string("the PNG could not be decoded: ") + stbi_failure_reason();
            return false;
        }
        out->width  = static_cast<uint32_t>(w);
        out->height = static_cast<uint32_t>(h);
        out->pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
        for (size_t i = 0, n = static_cast<size_t>(w) * static_cast<size_t>(h); i < n; ++i)
        {
            const float a        = rgba[i * 4 + 3] / 255.0f;
            out->pixels[i * 4 + 0] = rgba[i * 4 + 0] / 255.0f * a;
            out->pixels[i * 4 + 1] = rgba[i * 4 + 1] / 255.0f * a;
            out->pixels[i * 4 + 2] = rgba[i * 4 + 2] / 255.0f * a;
            out->pixels[i * 4 + 3] = a;
        }
        stbi_image_free(rgba);
        return true;
    }

    Picture Resample(const Picture& source, uint32_t width, uint32_t height)
    {
        if (source.width == width && source.height == height) return source;

        // Across, then down.
        const auto across = Taps(source.width, width);
        Picture    mid;
        mid.width  = width;
        mid.height = source.height;
        mid.pixels.assign(static_cast<size_t>(width) * source.height * 4u, 0.0f);
        for (uint32_t y = 0; y < source.height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                float* o = &mid.pixels[(static_cast<size_t>(y) * width + x) * 4];
                for (const Tap& t : across[x])
                {
                    const float* s = &source.pixels[(static_cast<size_t>(y) * source.width + t.index) * 4];
                    for (int c = 0; c < 4; ++c) o[c] += s[c] * t.weight;
                }
            }

        const auto down = Taps(source.height, height);
        Picture    out;
        out.width  = width;
        out.height = height;
        out.pixels.assign(static_cast<size_t>(width) * height * 4u, 0.0f);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                float* o = &out.pixels[(static_cast<size_t>(y) * width + x) * 4];
                for (const Tap& t : down[y])
                {
                    const float* s = &mid.pixels[(static_cast<size_t>(t.index) * width + x) * 4];
                    for (int c = 0; c < 4; ++c) o[c] += s[c] * t.weight;
                }
                // The filter's negative lobes may overshoot: kept a valid premultiplied colour.
                o[3] = std::min(1.0f, std::max(0.0f, o[3]));
                for (int c = 0; c < 3; ++c) o[c] = std::min(o[3], std::max(0.0f, o[c]));
            }
        return out;
    }

    void Tint(Picture& picture, uint32_t color)
    {
        if (color == 0xFFFFFFFFu) return;
        const float a = ((color >> 24) & 0xFF) / 255.0f, r = ((color >> 16) & 0xFF) / 255.0f,
                    g = ((color >> 8) & 0xFF) / 255.0f, b = (color & 0xFF) / 255.0f;
        for (size_t i = 0; i < picture.pixels.size(); i += 4)
        {
            picture.pixels[i + 0] *= r * a;
            picture.pixels[i + 1] *= g * a;
            picture.pixels[i + 2] *= b * a;
            picture.pixels[i + 3] *= a;
        }
    }

    uint32_t TextureEdge(uint32_t edge)
    {
        uint32_t p = kMinTextureEdge;
        while (p < edge) p <<= 1;
        return p;
    }

    std::vector<uint8_t> EncodeBlp(const Picture& picture, uint32_t textureWidth, uint32_t textureHeight)
    {
        // The picture at the top-left of a transparent texture.
        Picture level;
        level.width  = textureWidth;
        level.height = textureHeight;
        level.pixels.assign(static_cast<size_t>(textureWidth) * textureHeight * 4u, 0.0f);
        const uint32_t w = std::min(picture.width, textureWidth), h = std::min(picture.height, textureHeight);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&level.pixels[static_cast<size_t>(y) * textureWidth * 4],
                        &picture.pixels[static_cast<size_t>(y) * picture.width * 4], static_cast<size_t>(w) * 4 * sizeof(float));

        const uint32_t levels = std::min<uint32_t>(LevelCount(textureWidth, textureHeight), kMipSlots);
        size_t         total  = kBlpHeaderSize;
        {
            uint32_t lw = textureWidth, lh = textureHeight;
            for (uint32_t i = 0; i < levels; ++i)
            {
                total += static_cast<size_t>(lw) * lh * 4u;
                lw = lw > 1 ? lw >> 1 : 1;
                lh = lh > 1 ? lh >> 1 : 1;
            }
        }

        std::vector<uint8_t> blp(total, 0);
        Put32(&blp[0], kBlpMagic);
        Put32(&blp[4], 1); // format version
        blp[8]  = kColorRaw;
        blp[9]  = kAlpha8;
        blp[10] = kPixelArgb8888;
        blp[11] = 1; // has mips
        Put32(&blp[12], textureWidth);
        Put32(&blp[16], textureHeight);

        size_t at = kBlpHeaderSize;
        for (uint32_t i = 0; i < levels; ++i)
        {
            const size_t bytes = static_cast<size_t>(level.width) * level.height * 4u;
            Put32(&blp[kOffsetsAt + i * 4], static_cast<uint32_t>(at));
            Put32(&blp[kSizesAt + i * 4], static_cast<uint32_t>(bytes));
            WriteLevel(level, &blp[at]);
            at += bytes;
            if (i + 1 < levels) level = Halve(level);
        }
        return blp;
    }

    std::vector<uint8_t> EncodeBlpData(const uint8_t* rgba, uint32_t width, uint32_t height)
    {
        const uint32_t levels = std::min<uint32_t>(LevelCount(width, height), kMipSlots);
        size_t         total  = kBlpHeaderSize;
        for (uint32_t i = 0, lw = width, lh = height; i < levels; ++i, lw = lw > 1 ? lw >> 1 : 1, lh = lh > 1 ? lh >> 1 : 1)
            total += static_cast<size_t>(lw) * lh * 4u;
        std::vector<uint8_t> blp(total, 0);
        Put32(&blp[0], kBlpMagic);
        Put32(&blp[4], 1);
        blp[8]  = kColorRaw;
        blp[9]  = kAlpha8;
        blp[10] = kPixelArgb8888;
        blp[11] = 1;
        Put32(&blp[12], width);
        Put32(&blp[16], height);
        std::vector<uint8_t> level(rgba, rgba + static_cast<size_t>(width) * height * 4u);
        uint32_t             lw = width, lh = height;
        size_t               at = kBlpHeaderSize;
        for (uint32_t i = 0; i < levels; ++i)
        {
            const size_t bytes = static_cast<size_t>(lw) * lh * 4u;
            Put32(&blp[kOffsetsAt + i * 4], static_cast<uint32_t>(at));
            Put32(&blp[kSizesAt + i * 4], static_cast<uint32_t>(bytes));
            for (size_t t = 0; t < bytes; t += 4) // RGBA in, BGRA out
            {
                blp[at + t + 0] = level[t + 2];
                blp[at + t + 1] = level[t + 1];
                blp[at + t + 2] = level[t + 0];
                blp[at + t + 3] = level[t + 3];
            }
            at += bytes;
            if (i + 1 == levels) break;
            const uint32_t       nw = lw > 1 ? lw >> 1 : 1, nh = lh > 1 ? lh >> 1 : 1;
            std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4u);
            for (uint32_t y = 0; y < nh; ++y)
                for (uint32_t x = 0; x < nw; ++x)
                    for (uint32_t c = 0; c < 4; ++c)
                    {
                        uint32_t sum = 0, n = 0;
                        for (uint32_t dy = 0; dy < 2; ++dy)
                            for (uint32_t dx = 0; dx < 2; ++dx)
                            {
                                const uint32_t sx = std::min(lw - 1, x * 2 + dx), sy = std::min(lh - 1, y * 2 + dy);
                                sum += level[(static_cast<size_t>(sy) * lw + sx) * 4 + c];
                                ++n;
                            }
                        next[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + n / 2) / n);
                    }
            level.swap(next);
            lw = nw;
            lh = nh;
        }
        return blp;
    }

    bool BlpMip(const std::vector<uint8_t>& blp, uint32_t index, uint32_t* width, uint32_t* height,
                const uint8_t** bgra)
    {
        if (blp.size() < kBlpHeaderSize || Get32(&blp[0]) != kBlpMagic || blp[8] != kColorRaw || index >= kMipSlots)
            return false;
        uint32_t w = Get32(&blp[12]), h = Get32(&blp[16]);
        for (uint32_t i = 0; i < index; ++i)
        {
            w = w > 1 ? w >> 1 : 1;
            h = h > 1 ? h >> 1 : 1;
        }
        const uint32_t offset = Get32(&blp[kOffsetsAt + index * 4]), size = Get32(&blp[kSizesAt + index * 4]);
        if (!size || size != static_cast<uint64_t>(w) * h * 4u || offset + static_cast<uint64_t>(size) > blp.size())
            return false;
        *width  = w;
        *height = h;
        *bgra   = &blp[offset];
        return true;
    }

    uint64_t Hash64(const void* data, size_t size, uint64_t seed)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        uint64_t       h = seed;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= p[i];
            h *= 0x100000001B3ull;
        }
        return h;
    }
}
