// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The present pass: the scene drawn onto the image that is shown, stretched to it (bilinear, or
// nearest by setting), each colour through the gamma ramp. It is the only place the image shown
// is touched; the scene never is (the software cursor is the engine's draw into the scene, as on
// D3D9).
//
// The ramp is D3D9's: 256 entries per channel of 0..65535. It is applied where the D3D9 device
// applies its ramp (in fullscreen; the caller decides); an identity ramp otherwise.

#pragma once

#include "gx/vk/device/Device.hpp"
#include "gx/vk/resources/Resources.hpp"

#include <cstdint>
#include <vector>

namespace wxl::vk::present
{
    struct GammaRamp
    {
        uint16_t red[256];
        uint16_t green[256];
        uint16_t blue[256];
    };

    enum class Filter
    {
        Linear,
        Nearest,
    };

    class Pass
    {
    public:
        Pass() = default;
        Pass(const Pass&)            = delete;
        Pass& operator=(const Pass&) = delete;
        ~Pass() { Destroy(); }

        /// For @p slots frames in flight.
        bool Create(device::Context& context, resources::Allocator& allocator, uint32_t slots, char* why, uint32_t why_size);
        /// Its work must be complete (or the device lost).
        void Destroy();
        bool Ready() const { return layout_ != VK_NULL_HANDLE; }

        void SetFilter(Filter filter) { filter_ = filter; }
        Filter GetFilter() const { return filter_; }

        /// The ramp to apply from the next frame; null for none (identity).
        void SetGamma(const GammaRamp* ramp);
        bool HasGamma() const { return gamma_on_; }

        /// Records what has to happen before Record in @p slot: the ramp written to its buffer.
        void Prepare(uint32_t slot);

        /**
         * @brief Draws @p source (SHADER_READ_ONLY) onto @p target (COLOR_ATTACHMENT_OPTIMAL),
         *        beginning and ending the rendering itself.
         * @return false when no pipeline could be made for @p target_format (logged once).
         */
        bool Record(VkCommandBuffer cmd, uint32_t slot, VkImageView source, VkImageView target, VkFormat target_format,
                    VkExtent2D target_extent);

    private:
        struct Pipelines
        {
            VkFormat   format = VK_FORMAT_UNDEFINED;
            VkPipeline opaque = VK_NULL_HANDLE;
        };

        struct Slot
        {
            VkDescriptorSet  scene = VK_NULL_HANDLE;
            resources::Buffer gamma;          ///< 256 vec4 rows, mapped
            uint64_t         gamma_version = 0;
        };

        const Pipelines* PipelinesFor(VkFormat format);

        device::Context*      context_ = nullptr;
        resources::Allocator* allocator_ = nullptr;
        VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
        VkPipelineLayout      layout_ = VK_NULL_HANDLE;
        VkDescriptorPool      pool_ = VK_NULL_HANDLE;
        VkShaderModule        vertex_ = VK_NULL_HANDLE;
        VkShaderModule        fragment_ = VK_NULL_HANDLE;
        VkSampler             linear_ = VK_NULL_HANDLE;
        VkSampler             nearest_ = VK_NULL_HANDLE;
        std::vector<Pipelines> pipelines_;
        std::vector<VkFormat>  refused_;
        Slot                  slots_[3];
        uint32_t              slot_count_ = 0;
        Filter                filter_ = Filter::Linear;

        float    ramp_[256 * 4] = {};
        uint64_t ramp_version_ = 1;
        bool     gamma_on_ = false;
    };
}
