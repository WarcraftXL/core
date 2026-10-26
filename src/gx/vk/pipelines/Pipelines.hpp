// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The pipelines the engine's draws are recorded with. A pipeline is found by a 64-bit key of small
// ids (the programs, the vertex layout, the blend mode, the colour mask, the topology's class, the
// fill), kept in a hash map; whatever Vulkan 1.3 lets be dynamic is (viewport, scissor, cull, front
// face, topology within its class, depth test, write, compare and bias, blend constants), so a
// state change outside the key costs no pipeline. A missing pipeline is made on the spot, through
// a VkPipelineCache (pipeline libraries and background compiles are V5's).
//
// One layout for all: the bindless set, and 32 bytes of push constants for both stages.

#pragma once

#include "gx/vk/device/Device.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::vk::pipelines
{
    /// The renderer's vertex programs (shaders/glsl/).
    enum class VertexProgram : uint8_t
    {
        Fixed,           ///< D3D9's fixed function (ff.vert)
        Interface,       ///< the client's "UI:0" (ui.vert)
        InterfaceStereo, ///< the client's "UI:1" (ui.vert, WXL_STEREO)
        Count,
    };

    /// The renderer's pixel programs.
    enum class PixelProgram : uint8_t
    {
        Fixed,      ///< D3D9's fixed function, stages 0 and 1 (ff.frag)
        Interface,  ///< the client's "UI" (ui.frag)
        Desaturate, ///< the client's "Desaturate" (desaturate.frag)
        Count,
    };

    /// The embedded SPIR-V of a program.
    const uint32_t* VertexCode(VertexProgram program, size_t& words);
    const uint32_t* PixelCode(PixelProgram program, size_t& words);

    /// A vertex layout: its attributes and the buffers they read.
    struct VertexLayout
    {
        uint32_t                          attribute_count = 0;
        VkVertexInputAttributeDescription attributes[16] = {};
        uint32_t                          binding_count = 0;
        VkVertexInputBindingDescription   bindings[16] = {};
    };

    /// The topology classes a pipeline is made for; the topology itself is dynamic inside it.
    enum class TopologyClass : uint8_t
    {
        Points,
        Lines,
        Triangles,
    };

    struct Key
    {
        VertexProgram vertex = VertexProgram::Fixed;
        PixelProgram  pixel = PixelProgram::Fixed;
        uint16_t      layout = 0;   ///< LayoutId()
        uint8_t       blend = 0;    ///< EGxBlend
        uint8_t       color_mask = 0xF;
        TopologyClass topology = TopologyClass::Triangles;
        bool          wireframe = false;

        uint64_t Packed() const
        {
            return static_cast<uint64_t>(vertex) | static_cast<uint64_t>(pixel) << 4 | static_cast<uint64_t>(layout) << 8 |
                   static_cast<uint64_t>(blend) << 24 | static_cast<uint64_t>(color_mask) << 32 |
                   static_cast<uint64_t>(topology) << 40 | static_cast<uint64_t>(wireframe) << 44;
        }
    };

    /// D3D9's blend of an EGxBlend mode (the D3D9 device's s_srcBlend/s_dstBlend): enabled, the
    /// source and destination factors, the same for alpha (D3D9 has no separate alpha blend here).
    struct Blend
    {
        bool          enable;
        VkBlendFactor source;
        VkBlendFactor destination;
    };
    Blend BlendOf(uint32_t mode);

    class Pipelines
    {
    public:
        static constexpr uint32_t kPushBytes = 32;

        Pipelines() = default;
        Pipelines(const Pipelines&)            = delete;
        Pipelines& operator=(const Pipelines&) = delete;
        ~Pipelines() { Destroy(); }

        /// For renderings into @p color with @p depth (and its stencil when it has one), reading
        /// the bindless set of @p set_layout.
        bool Create(device::Context& context, VkDescriptorSetLayout set_layout, VkFormat color, VkFormat depth, char* why,
                    uint32_t why_size);
        void Destroy();
        bool Ready() const { return layout_ != VK_NULL_HANDLE; }

        VkPipelineLayout Layout() const { return layout_; }

        /// The id of @p layout, given at its first sight.
        uint16_t LayoutId(const VertexLayout& layout);

        /// The pipeline of @p key, made now when it is missing; null when it cannot be made (logged once).
        VkPipeline Get(const Key& key);

        size_t Count() const { return pipelines_.size(); }
        size_t Layouts() const { return layouts_.size(); }

    private:
        VkPipeline Make(const Key& key);

        device::Context*                           context_ = nullptr;
        VkPipelineLayout                           layout_ = VK_NULL_HANDLE;
        VkPipelineCache                            cache_ = VK_NULL_HANDLE;
        VkShaderModule                             vertex_[static_cast<int>(VertexProgram::Count)] = {};
        VkShaderModule                             pixel_[static_cast<int>(PixelProgram::Count)] = {};
        VkFormat                                   color_ = VK_FORMAT_UNDEFINED;
        VkFormat                                   depth_ = VK_FORMAT_UNDEFINED;
        bool                                       stencil_ = false;
        std::unordered_map<uint64_t, VkPipeline>   pipelines_;
        std::vector<VertexLayout>                  layouts_;
        std::unordered_map<std::string, uint16_t>  layout_ids_; ///< by the layout's bytes
    };
}
