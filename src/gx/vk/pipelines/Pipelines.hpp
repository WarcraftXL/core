// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The pipelines the engine's draws are recorded with, and the programs they are made from: the
// renderer's own (GLSL compiled when the renderer is built: D3D9's fixed function, the client's
// interface programs) and the client's own, translated from its DX9 records (shaders/Translate.hpp)
// and added as the engine creates them. A program keeps its id for the life of the device, so a
// pipeline's key never names another program.
//
// A pipeline is found by a 64-bit key of small ids (the programs, the vertex layout, the blend
// mode, the colour mask, the topology's class, the fill, the target's formats), kept in a hash
// map; whatever Vulkan 1.3 lets be dynamic is (viewport, scissor, cull, front face, topology within
// its class, depth test, write, compare and bias, blend constants), so a state change outside the
// key costs no pipeline. A missing pipeline is made on the spot, through a VkPipelineCache that
// persists on disk between launches; there are no pipeline libraries and no background compiles.
//
// One layout for all: the bindless set, and the 128 bytes of push constants (shaders::Push).

#pragma once

#include "gx/vk/device/Device.hpp"
#include "gx/vk/shaders/Translate.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::vk::pipelines
{
    /// The renderer's own vertex programs (shaders/glsl/), the first ids of the vertex table.
    enum class VertexProgram : uint16_t
    {
        Fixed,           ///< D3D9's fixed function (ff.vert)
        Interface,       ///< the client's "UI:0" (ui.vert)
        InterfaceStereo, ///< the client's "UI:1" (ui.vert, WXL_STEREO)
        Count,
    };

    /// The renderer's own pixel programs, the first ids of the pixel table.
    enum class PixelProgram : uint16_t
    {
        Fixed,      ///< D3D9's fixed function, stages 0 and 1 (ff.frag)
        Interface,  ///< the client's "UI" (ui.frag)
        Desaturate, ///< the client's "Desaturate" (desaturate.frag)
        Count,
    };

    /// The embedded SPIR-V of an own program (the fixed-function vertex program with the user clip
    /// plane when @p clip).
    const uint32_t* VertexCode(VertexProgram program, size_t& words, bool clip = false);
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
        uint16_t      vertex = 0;   ///< a vertex program's id
        uint16_t      pixel = 0;    ///< a pixel program's id
        uint16_t      layout = 0;   ///< LayoutId()
        uint8_t       blend = 0;    ///< EGxBlend
        uint8_t       color_mask = 0xF;
        TopologyClass topology = TopologyClass::Triangles;
        bool          wireframe = false;
        uint8_t       target = 0;   ///< TargetId(): the colour and depth formats drawn into

        uint64_t Packed() const
        {
            return static_cast<uint64_t>(vertex) | static_cast<uint64_t>(pixel) << 16 | static_cast<uint64_t>(layout & 0xFFF) << 32 |
                   static_cast<uint64_t>(blend & 0xF) << 44 | static_cast<uint64_t>(color_mask & 0xF) << 48 |
                   static_cast<uint64_t>(topology) << 52 | static_cast<uint64_t>(wireframe) << 54 | static_cast<uint64_t>(target & 0x1F) << 55;
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
        static constexpr uint32_t kPushBytes = 128;

        Pipelines() = default;
        Pipelines(const Pipelines&)            = delete;
        Pipelines& operator=(const Pipelines&) = delete;
        ~Pipelines() { Destroy(); }

        /// Reading the bindless set of @p set_layout; @p cache_file (may be empty) holds the
        /// VkPipelineCache between launches.
        bool Create(device::Context& context, VkDescriptorSetLayout set_layout, const std::string& cache_file, char* why, uint32_t why_size);
        /// Writes the pipeline cache to its file first.
        void Destroy();
        bool Ready() const { return layout_ != VK_NULL_HANDLE; }

        VkPipelineLayout Layout() const { return layout_; }

        /// A translated program added: its id, or -1 when its module cannot be made.
        int AddProgram(const std::vector<uint32_t>& words, const shaders::ProgramInfo& info);
        /// What a program reads (the own programs' as the interface fixes them).
        const shaders::ProgramInfo& Info(bool pixel, uint16_t id) const;
        size_t Programs(bool pixel) const { return pixel ? pixel_.size() : vertex_.size(); }

        /// The id of @p layout, given at its first sight.
        uint16_t LayoutId(const VertexLayout& layout);
        /// The id of a colour format and a depth format (UNDEFINED: none), given at its first sight.
        uint8_t TargetId(VkFormat color, VkFormat depth);

        /// The pipeline of @p key, made now when it is missing; null when it cannot be made (logged once).
        VkPipeline Get(const Key& key);

        size_t Count() const { return pipelines_.size(); }
        size_t Layouts() const { return layouts_.size(); }
        /// Saves the pipeline cache to its file now.
        void Save();

    private:
        struct Program
        {
            VkShaderModule       module = VK_NULL_HANDLE;
            shaders::ProgramInfo info;
        };
        struct Target
        {
            VkFormat color;
            VkFormat depth;
        };

        VkPipeline Make(const Key& key);
        bool       Own(bool pixel, const uint32_t* code, size_t words, const shaders::ProgramInfo& info);

        device::Context*                           context_ = nullptr;
        VkPipelineLayout                           layout_ = VK_NULL_HANDLE;
        VkPipelineCache                            cache_ = VK_NULL_HANDLE;
        std::string                                cache_file_;
        std::vector<Program>                       vertex_;
        std::vector<Program>                       pixel_;
        std::vector<Target>                        targets_;
        std::unordered_map<uint64_t, VkPipeline>   pipelines_;
        std::vector<VertexLayout>                  layouts_;
        std::unordered_map<std::string, uint16_t>  layout_ids_; ///< by the layout's bytes
    };
}
