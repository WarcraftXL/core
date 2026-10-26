// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The engine's texture formats on the device.

#include "gx/vk/draw/Formats.hpp"

namespace wxl::vk::draw
{
    namespace
    {
        constexpr VkComponentMapping kIdentity = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                                  VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

        bool Samples(VkPhysicalDevice physical, VkFormat format)
        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(physical, format, &props);
            return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        }

        FormatInfo Plain(VkFormat format, uint32_t bytes, VkComponentMapping swizzle = kIdentity)
        {
            FormatInfo info;
            info.format  = format;
            info.bytes   = bytes;
            info.swizzle = swizzle;
            return info;
        }

        FormatInfo Block(VkFormat format, uint32_t bytes)
        {
            FormatInfo info = Plain(format, bytes);
            info.block      = 4;
            return info;
        }
    }

    void Formats::Choose(VkPhysicalDevice physical, bool bc, VkFormat depth)
    {
        for (FormatInfo& info : table_) info = FormatInfo{};
        table_[kTexAbgr8888] = Plain(VK_FORMAT_R8G8B8A8_UNORM, 4);
        table_[kTexArgb8888] = Plain(VK_FORMAT_B8G8R8A8_UNORM, 4);
        // D3D9's A4R4G4B4 holds a r g b from the top bits down. Vulkan 1.3's A4R4G4B4 is the same
        // (an optional feature); B4G4R4A4, which every device samples, holds b g r a there: read
        // through a swizzle, its b is the alpha, its g the red, its r the green, its a the blue.
        if (Samples(physical, VK_FORMAT_A4R4G4B4_UNORM_PACK16))
            table_[kTexArgb4444] = Plain(VK_FORMAT_A4R4G4B4_UNORM_PACK16, 2);
        else
            table_[kTexArgb4444] = Plain(VK_FORMAT_B4G4R4A4_UNORM_PACK16, 2,
                                         {VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_B});
        table_[kTexArgb1555] = Plain(VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2);
        table_[kTexRgb565]   = Plain(VK_FORMAT_R5G6B5_UNORM_PACK16, 2);
        if (bc)
        {
            table_[kTexDxt1] = Block(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8);
            table_[kTexDxt3] = Block(VK_FORMAT_BC2_UNORM_BLOCK, 16);
            table_[kTexDxt5] = Block(VK_FORMAT_BC3_UNORM_BLOCK, 16);
        }
        table_[kTexUv88] = Plain(VK_FORMAT_R8G8_SNORM, 2,
                                 {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE});
        // D3D9 reads the channels a format lacks as 1, Vulkan its colour channels as 0.
        table_[kTexGr1616F] = Plain(VK_FORMAT_R16G16_SFLOAT, 4,
                                    {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE});
        table_[kTexR32F]    = Plain(VK_FORMAT_R32_SFLOAT, 4,
                                    {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE});
        table_[kTexD24X8]   = Plain(depth, 4);
        table_[kTexD24X8].depth = true;
        for (uint32_t i = 1; i < kTexD24X8; ++i)
            if (table_[i].format != VK_FORMAT_UNDEFINED && !Samples(physical, table_[i].format)) table_[i] = FormatInfo{};
    }

    const FormatInfo& Formats::Of(uint32_t format) const
    {
        static const FormatInfo kNone;
        return format < kTexFormats ? table_[format] : kNone;
    }

    uint32_t Formats::Pitch(const FormatInfo& info, uint32_t width)
    {
        return info.block > 1 ? (width + info.block - 1) / info.block * info.bytes : width * info.bytes;
    }

    uint32_t Formats::Rows(const FormatInfo& info, uint32_t height)
    {
        return info.block > 1 ? (height + info.block - 1) / info.block : height;
    }
}
