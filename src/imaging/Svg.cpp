// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// SVG through nanosvg (deps/nanosvg, zlib license): parsed into shapes, restyled by id, rasterized.

#include "imaging/Imaging.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace wxl::core::imaging
{
    namespace
    {
        struct ImageDeleter
        {
            void operator()(NSVGimage* image) const { nsvgDelete(image); }
        };
        struct RasterizerDeleter
        {
            void operator()(NSVGrasterizer* rasterizer) const { nsvgDeleteRasterizer(rasterizer); }
        };
        using ImagePtr = std::unique_ptr<NSVGimage, ImageDeleter>;

        /// Parses a copy of the text: nanosvg writes into what it parses and wants it terminated.
        ImagePtr Parse(const uint8_t* data, size_t size, std::string* error)
        {
            std::string text(reinterpret_cast<const char*>(data), size);
            ImagePtr    image(nsvgParse(text.data(), "px", 96.0f));
            if (!image)
            {
                if (error) *error = "the SVG could not be parsed";
                return nullptr;
            }
            if (!(image->width > 0.0f) || !(image->height > 0.0f))
            {
                if (error) *error = "the SVG declares no size: give its root a width and a height, or a viewBox";
                return nullptr;
            }
            return image;
        }

        /// 0xAARRGGBB as nanosvg holds a colour: red in the low byte, alpha in the high one.
        unsigned int NanoColor(uint32_t argb)
        {
            const unsigned int a = (argb >> 24) & 0xFFu, r = (argb >> 16) & 0xFFu, g = (argb >> 8) & 0xFFu,
                               b = argb & 0xFFu;
            return r | (g << 8) | (b << 16) | (a << 24);
        }

        void Recolor(NSVGpaint& paint, uint32_t argb)
        {
            if (paint.type == NSVG_PAINT_NONE || paint.type == NSVG_PAINT_UNDEF) return;
            nsvg__deletePaint(&paint);
            paint.type  = NSVG_PAINT_COLOR;
            paint.color = NanoColor(argb);
        }

        void Apply(NSVGshape& shape, const GroupStyle& style)
        {
            if (style.setVisible)
            {
                if (style.visible) shape.flags |= NSVG_FLAGS_VISIBLE;
                else shape.flags &= static_cast<unsigned char>(~NSVG_FLAGS_VISIBLE);
            }
            if (style.setColor)
            {
                Recolor(shape.fill, style.color);
                Recolor(shape.stroke, style.color);
                shape.opacity = 1.0f;
            }
            if (style.setWidth)
            {
                if (style.width <= 0.0f)
                {
                    nsvg__deletePaint(&shape.stroke);
                    shape.stroke.type = NSVG_PAINT_NONE;
                    shape.strokeWidth = 0.0f;
                }
                else
                {
                    // A multiple of the shape's own stroke; its dashes keep their proportion to it.
                    shape.strokeWidth *= style.width;
                    for (int i = 0; i < shape.strokeDashCount; ++i) shape.strokeDashArray[i] *= style.width;
                    shape.strokeDashOffset *= style.width;
                }
            }
        }

        uint32_t Ceil(float value)
        {
            const double up = std::ceil(static_cast<double>(value) - 1e-4);
            return up < 1.0 ? 1u : static_cast<uint32_t>(up);
        }
    }

    Kind Sniff(const uint8_t* data, size_t size)
    {
        static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (size >= 8 && std::memcmp(data, kPng, 8) == 0) return Kind::Png;
        // Text with an <svg element in its first kilobytes (after an XML declaration, a doctype,
        // comments).
        const size_t      look = size < 4096 ? size : 4096;
        const std::string head(reinterpret_cast<const char*>(data), look);
        return head.find("<svg") != std::string::npos ? Kind::Svg : Kind::Unknown;
    }

    bool SvgSize(const uint8_t* data, size_t size, uint32_t* width, uint32_t* height, std::string* error)
    {
        ImagePtr image = Parse(data, size, error);
        if (!image) return false;
        *width  = Ceil(image->width);
        *height = Ceil(image->height);
        return true;
    }

    bool RenderSvg(const uint8_t* data, size_t size, uint32_t width, uint32_t height, const GroupStyle* styles,
                   size_t styleCount, Picture* out, std::string* error, std::vector<uint32_t>* matched)
    {
        if (width == 0 || height == 0 || width > kMaxEdge || height > kMaxEdge)
        {
            if (error) *error = "the size must be from 1 to 2048 pixels each way";
            return false;
        }
        ImagePtr image = Parse(data, size, error);
        if (!image) return false;

        if (matched) matched->assign(styleCount, 0);
        for (NSVGshape* shape = image->shapes; shape; shape = shape->next)
            for (size_t i = 0; i < styleCount; ++i)
                if (styles[i].id == shape->id)
                {
                    Apply(*shape, styles[i]);
                    if (matched) ++(*matched)[i];
                }

        std::unique_ptr<NSVGrasterizer, RasterizerDeleter> rasterizer(nsvgCreateRasterizer());
        if (!rasterizer)
        {
            if (error) *error = "out of memory";
            return false;
        }

        // Fitted and centred: the default preserveAspectRatio.
        const float sx    = static_cast<float>(width) / image->width;
        const float sy    = static_cast<float>(height) / image->height;
        const float scale = sx < sy ? sx : sy;
        const float tx    = (static_cast<float>(width) - image->width * scale) * 0.5f;
        const float ty    = (static_cast<float>(height) - image->height * scale) * 0.5f;

        std::vector<unsigned char> rgba(static_cast<size_t>(width) * height * 4u);
        nsvgRasterize(rasterizer.get(), image.get(), tx, ty, scale, rgba.data(), static_cast<int>(width),
                      static_cast<int>(height), static_cast<int>(width * 4u));

        // nanosvg answers straight colour; premultiplied from here on.
        out->width  = width;
        out->height = height;
        out->pixels.resize(static_cast<size_t>(width) * height * 4u);
        for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i)
        {
            const float a        = rgba[i * 4 + 3] / 255.0f;
            out->pixels[i * 4 + 0] = rgba[i * 4 + 0] / 255.0f * a;
            out->pixels[i * 4 + 1] = rgba[i * 4 + 1] / 255.0f * a;
            out->pixels[i * 4 + 2] = rgba[i * 4 + 2] / 255.0f * a;
            out->pixels[i * 4 + 3] = a;
        }
        return true;
    }
}
