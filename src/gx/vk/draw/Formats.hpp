// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The engine's texture formats (EGxTexFormat) as Vulkan images: the format, the view's swizzle
// where the bytes are not in Vulkan's order, and how the bytes are laid out (texels or 4x4
// blocks). The bytes the engine hands over are D3D9's; nothing is converted on the CPU: where
// Vulkan has no format in D3D9's order, a format with the same bits and a swizzle reads them.
//
//   Abgr8888  R8G8B8A8_UNORM            Argb8888  B8G8R8A8_UNORM
//   Argb4444  A4R4G4B4_UNORM_PACK16 (formatA4R4G4B4), else B4G4R4A4_UNORM_PACK16 read (g, r, a, b)
//   Argb1555  A1R5G5B5_UNORM_PACK16     Rgb565    R5G6B5_UNORM_PACK16
//   Dxt1/3/5  BC1_RGBA / BC2 / BC3_UNORM_BLOCK (textureCompressionBC)
//   Uv88      R8G8_SNORM read (r, g, 1, 1), D3D9's V8U8
//   Gr1616F   R16G16_SFLOAT read (r, g, 1, 1)    R32F  R32_SFLOAT read (r, 1, 1, 1): D3D9 reads a
//             missing channel as 1
//   D24X8     the depth format the device draws into (a render target's depth)

#pragma once

#include "gx/vk/Vk.hpp"

#include <cstdint>

namespace wxl::vk::draw
{
    /// EGxTexFormat.
    enum TexFormat : uint32_t
    {
        kTexUnknown  = 0,
        kTexAbgr8888 = 1,
        kTexArgb8888 = 2,
        kTexArgb4444 = 3,
        kTexArgb1555 = 4,
        kTexRgb565   = 5,
        kTexDxt1     = 6,
        kTexDxt3     = 7,
        kTexDxt5     = 8,
        kTexUv88     = 9,
        kTexGr1616F  = 10,
        kTexR32F     = 11,
        kTexD24X8    = 12,
        kTexFormats  = 13,
    };

    struct FormatInfo
    {
        VkFormat           format = VK_FORMAT_UNDEFINED;
        VkComponentMapping swizzle = {};
        uint32_t           block = 1;          ///< 4 for the compressed formats: rows of 4x4 blocks
        uint32_t           bytes = 4;          ///< per texel, or per block
        bool               depth = false;
    };

    /// The device's choice for each format, made once per device.
    class Formats
    {
    public:
        /// Reads the device's support; @p depth is the depth format of D24X8 targets.
        void Choose(VkPhysicalDevice physical, bool bc, VkFormat depth);

        /// @p format's image; format UNDEFINED when the device cannot sample it.
        const FormatInfo& Of(uint32_t format) const;

        /// Bytes per row of @p width texels (of 4x4 blocks when compressed), and the rows of @p height.
        static uint32_t Pitch(const FormatInfo& info, uint32_t width);
        static uint32_t Rows(const FormatInfo& info, uint32_t height);

    private:
        FormatInfo table_[kTexFormats];
    };
}
