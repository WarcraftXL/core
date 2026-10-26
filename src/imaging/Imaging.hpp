// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Pictures made into textures the client loads as its own: an SVG rasterized at a pixel size with
// its groups restyled, or a PNG decoded and resampled, then tinted, padded to the power-of-two
// texture the client's device takes, given its mips and written as an uncompressed BLP2.
//
// Pixels in flight are premultiplied RGBA in floats, 0 to 1: every filter (the rasterizer's own
// coverage, the resampler, the mips) averages premultiplied colours, so a transparent pixel adds
// nothing to its neighbours. The BLP holds straight (unpremultiplied) BGRA, which is what the
// client's blend expects (source alpha, one minus source alpha); a transparent pixel there takes
// the colour of its visible neighbours, so a filter that reads it blends no dark fringe in.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wxl::core::imaging
{
    /// The largest edge, in pixels, of a picture or of a texture: the client decodes no texture larger.
    constexpr uint32_t kMaxEdge = 2048;

    /// The smallest edge of a texture the client's device creates.
    constexpr uint32_t kMinTextureEdge = 8;

    /// What a file's bytes are.
    enum class Kind
    {
        Unknown,
        Svg,
        Png,
    };

    /// The kind of a file by its first bytes: PNG's signature, or text that holds an <svg element.
    Kind Sniff(const uint8_t* data, size_t size);

    /// A change to the shapes of an SVG whose id, or the id of the nearest enclosing group with
    /// one, is `id`.
    struct GroupStyle
    {
        std::string id;
        bool        setColor   = false;
        uint32_t    color      = 0;     ///< 0xAARRGGBB: replaces every paint, and its opacity
        bool        setWidth   = false;
        float       width      = 1.0f;  ///< the strokes' width times the SVG's own: 0 takes them off
        bool        setVisible = false;
        bool        visible    = true;
    };

    /// A picture: premultiplied RGBA, four floats a pixel, rows from the top.
    struct Picture
    {
        uint32_t           width  = 0;
        uint32_t           height = 0;
        std::vector<float> pixels;
    };

    /// The size an SVG declares (its width and height, else its viewBox), rounded up to pixels.
    bool SvgSize(const uint8_t* data, size_t size, uint32_t* width, uint32_t* height, std::string* error);

    /**
     * @brief Rasterizes an SVG at @p width x @p height pixels, anti-aliased.
     *
     * The drawing keeps its proportions: scaled to fit and centred, as an SVG's default
     * preserveAspectRatio (xMidYMid meet). @p styles apply to the parsed shapes before they are
     * drawn; @p matched, when given, receives how many shapes each style reached.
     */
    bool RenderSvg(const uint8_t* data, size_t size, uint32_t width, uint32_t height, const GroupStyle* styles,
                   size_t styleCount, Picture* out, std::string* error, std::vector<uint32_t>* matched = nullptr);

    /// Decodes a PNG (any bit depth and colour type) into a premultiplied picture.
    bool DecodePng(const uint8_t* data, size_t size, Picture* out, std::string* error);

    /**
     * @brief Resamples a picture to another size with a Mitchell-Netravali filter, widened by the
     *        ratio when it shrinks: a downscale averages every source pixel it covers.
     */
    Picture Resample(const Picture& source, uint32_t width, uint32_t height);

    /// Multiplies every pixel by a colour, 0xAARRGGBB: white leaves it as it is.
    void Tint(Picture& picture, uint32_t color);

    /// The power of two at or above @p edge, and at least kMinTextureEdge.
    uint32_t TextureEdge(uint32_t edge);

    /**
     * @brief The picture as an uncompressed BLP2 (colour encoding 3, eight bits of alpha,
     *        preferred format ARGB8888) with every mip down to 1x1.
     *
     * The texture is @p textureWidth x @p textureHeight, powers of two at least as large as the
     * picture, which sits at its top-left corner; the rest is transparent. The mips are averaged
     * from the premultiplied pixels, then written straight, transparent pixels coloured from
     * their neighbours.
     */
    std::vector<uint8_t> EncodeBlp(const Picture& picture, uint32_t textureWidth, uint32_t textureHeight);

    /// The bytes of a BLP's header, before its first mip.
    constexpr size_t kBlpHeaderSize = 0x94 + 1024;

    /// One mip of a BLP EncodeBlp wrote, as straight BGRA bytes; false when it is not one.
    bool BlpMip(const std::vector<uint8_t>& blp, uint32_t level, uint32_t* width, uint32_t* height,
                const uint8_t** bgra);

    /// The 64-bit FNV-1a hash of bytes, continued from @p seed.
    uint64_t Hash64(const void* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull);
}
