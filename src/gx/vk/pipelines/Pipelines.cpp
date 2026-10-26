// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The pipelines of the engine's draws, and the renderer's programs they are made from.

#include "gx/vk/pipelines/Pipelines.hpp"

#include "gx/vk/Log.hpp"

#include <cstdio>
#include <cstring>

namespace wxl::vk::pipelines
{
    namespace spirv
    {
        // The programs, compiled from shaders/glsl/ when the runtime is built (cmake/wxl_glsl.cmake).
#include "ff_vert.h"
#include "ui_vert.h"
#include "ui_stereo_vert.h"
#include "ff_frag.h"
#include "ui_frag.h"
#include "desaturate_frag.h"
    }

    const uint32_t* VertexCode(VertexProgram program, size_t& words)
    {
        switch (program)
        {
        case VertexProgram::Interface: words = sizeof spirv::ui_vert / 4; return spirv::ui_vert;
        case VertexProgram::InterfaceStereo: words = sizeof spirv::ui_stereo_vert / 4; return spirv::ui_stereo_vert;
        default: words = sizeof spirv::ff_vert / 4; return spirv::ff_vert;
        }
    }

    const uint32_t* PixelCode(PixelProgram program, size_t& words)
    {
        switch (program)
        {
        case PixelProgram::Interface: words = sizeof spirv::ui_frag / 4; return spirv::ui_frag;
        case PixelProgram::Desaturate: words = sizeof spirv::desaturate_frag / 4; return spirv::desaturate_frag;
        default: words = sizeof spirv::ff_frag / 4; return spirv::ff_frag;
        }
    }

    Blend BlendOf(uint32_t mode)
    {
        switch (mode)
        {
        case 2: return {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};           // Alpha
        case 3: return {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE};                           // Add
        case 4: return {true, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO};                          // Mod
        case 5: return {true, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR};                     // Mod2x
        case 6: return {true, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE};                           // ModAdd
        case 7: return {true, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE};                 // InvSrcAlphaAdd
        case 8: return {true, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ZERO};                // InvSrcAlphaOpaque
        case 9: return {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ZERO};                          // SrcAlphaOpaque
        case 10: return {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE};                                // NoAlphaAdd
        case 11: return {true, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR}; // ConstantAlpha
        case 12: return {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};                // BlendAdd
        default: return {false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO};                              // Opaque, AlphaKey
        }
    }

    namespace
    {
        VkShaderModule Module(VkDevice device, const uint32_t* code, size_t words)
        {
            VkShaderModuleCreateInfo info{};
            info.sType          = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize       = words * sizeof(uint32_t);
            info.pCode          = code;
            VkShaderModule made = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &info, nullptr, &made) != VK_SUCCESS) return VK_NULL_HANDLE;
            return made;
        }

        bool HasStencil(VkFormat format)
        {
            return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT;
        }
    }

    bool Pipelines::Create(device::Context& context, VkDescriptorSetLayout set_layout, VkFormat color, VkFormat depth, char* why,
                           uint32_t why_size)
    {
        Destroy();
        context_           = &context;
        color_             = color;
        depth_             = depth;
        stencil_           = HasStencil(depth);
        const VkDevice dev = context.Device();

        const VkPushConstantRange  push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes};
        VkPipelineLayoutCreateInfo info{};
        info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount         = 1;
        info.pSetLayouts            = &set_layout;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges    = &push;
        VkResult result             = vkCreatePipelineLayout(dev, &info, nullptr, &layout_);
        if (result == VK_SUCCESS)
        {
            VkPipelineCacheCreateInfo cache{};
            cache.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
            result      = vkCreatePipelineCache(dev, &cache, nullptr, &cache_);
        }
        bool modules = result == VK_SUCCESS;
        for (int i = 0; modules && i < static_cast<int>(VertexProgram::Count); ++i)
        {
            size_t          words = 0;
            const uint32_t* code  = VertexCode(static_cast<VertexProgram>(i), words);
            vertex_[i]            = Module(dev, code, words);
            modules               = vertex_[i] != VK_NULL_HANDLE;
        }
        for (int i = 0; modules && i < static_cast<int>(PixelProgram::Count); ++i)
        {
            size_t          words = 0;
            const uint32_t* code  = PixelCode(static_cast<PixelProgram>(i), words);
            pixel_[i]             = Module(dev, code, words);
            modules               = pixel_[i] != VK_NULL_HANDLE;
        }
        if (!modules)
        {
            std::snprintf(why, why_size, "the engine's pipeline layout, cache or programs could not be made: %s", ResultName(result));
            Destroy();
            return false;
        }
        return true;
    }

    void Pipelines::Destroy()
    {
        if (context_ && context_->Ready())
        {
            const VkDevice dev = context_->Device();
            for (auto& entry : pipelines_)
                if (entry.second) vkDestroyPipeline(dev, entry.second, nullptr);
            for (VkShaderModule& m : vertex_)
                if (m) vkDestroyShaderModule(dev, m, nullptr);
            for (VkShaderModule& m : pixel_)
                if (m) vkDestroyShaderModule(dev, m, nullptr);
            if (cache_) vkDestroyPipelineCache(dev, cache_, nullptr);
            if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
        }
        pipelines_.clear();
        layouts_.clear();
        layout_ids_.clear();
        for (VkShaderModule& m : vertex_) m = VK_NULL_HANDLE;
        for (VkShaderModule& m : pixel_) m = VK_NULL_HANDLE;
        cache_   = VK_NULL_HANDLE;
        layout_  = VK_NULL_HANDLE;
        context_ = nullptr;
    }

    uint16_t Pipelines::LayoutId(const VertexLayout& layout)
    {
        std::string bytes(reinterpret_cast<const char*>(&layout.attribute_count), sizeof layout.attribute_count);
        bytes.append(reinterpret_cast<const char*>(layout.attributes), sizeof(VkVertexInputAttributeDescription) * layout.attribute_count);
        bytes.append(reinterpret_cast<const char*>(layout.bindings), sizeof(VkVertexInputBindingDescription) * layout.binding_count);
        const auto found = layout_ids_.find(bytes);
        if (found != layout_ids_.end()) return found->second;
        const uint16_t id = static_cast<uint16_t>(layouts_.size());
        layouts_.push_back(layout);
        layout_ids_.emplace(std::move(bytes), id);
        return id;
    }

    VkPipeline Pipelines::Get(const Key& key)
    {
        const uint64_t packed = key.Packed();
        const auto     found  = pipelines_.find(packed);
        if (found != pipelines_.end()) return found->second;
        const VkPipeline made = Make(key);
        // A key that cannot be made is remembered too: it is not tried again at every draw.
        pipelines_.emplace(packed, made);
        if (!made)
            Log(LogLevel::Warn, "a pipeline could not be made (programs %d/%d, layout %u, blend %u): its draws are dropped",
                static_cast<int>(key.vertex), static_cast<int>(key.pixel), key.layout, key.blend);
        return made;
    }

    VkPipeline Pipelines::Make(const Key& key)
    {
        if (key.layout >= layouts_.size()) return VK_NULL_HANDLE;
        const VertexLayout& layout = layouts_[key.layout];

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_[static_cast<int>(key.vertex)];
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = pixel_[static_cast<int>(key.pixel)];
        stages[1].pName  = "main";

        VkPipelineVertexInputStateCreateInfo input{};
        input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        input.vertexBindingDescriptionCount   = layout.binding_count;
        input.pVertexBindingDescriptions      = layout.bindings;
        input.vertexAttributeDescriptionCount = layout.attribute_count;
        input.pVertexAttributeDescriptions    = layout.attributes;

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = key.topology == TopologyClass::Points  ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                            : key.topology == TopologyClass::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                    : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = key.wireframe && context_->Chosen().features.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        raster.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

        const Blend                         blend = BlendOf(key.blend);
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable         = blend.enable ? VK_TRUE : VK_FALSE;
        attachment.srcColorBlendFactor = blend.source;
        attachment.dstColorBlendFactor = blend.destination;
        attachment.colorBlendOp        = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = blend.source;
        attachment.dstAlphaBlendFactor = blend.destination;
        attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
        attachment.colorWriteMask      = key.color_mask;
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments    = &attachment;

        static const VkDynamicState kDynamic[] = {
            VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,           VK_DYNAMIC_STATE_CULL_MODE,
            VK_DYNAMIC_STATE_FRONT_FACE,        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_BIAS,        VK_DYNAMIC_STATE_BLEND_CONSTANTS,   VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
            VK_DYNAMIC_STATE_STENCIL_OP,        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
            VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<uint32_t>(sizeof kDynamic / sizeof kDynamic[0]);
        dynamic.pDynamicStates    = kDynamic;

        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount    = 1;
        rendering.pColorAttachmentFormats = &color_;
        rendering.depthAttachmentFormat   = depth_;
        rendering.stencilAttachmentFormat = stencil_ ? depth_ : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo info{};
        info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext               = &rendering;
        info.stageCount          = 2;
        info.pStages             = stages;
        info.pVertexInputState   = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState      = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisample;
        info.pDepthStencilState  = &depth;
        info.pColorBlendState    = &colorBlend;
        info.pDynamicState       = &dynamic;
        info.layout              = layout_;
        VkPipeline made          = VK_NULL_HANDLE;
        const VkResult result    = vkCreateGraphicsPipelines(context_->Device(), cache_, 1, &info, nullptr, &made);
        if (result != VK_SUCCESS)
        {
            Log(LogLevel::Warn, "vkCreateGraphicsPipelines: %s", ResultName(result));
            return VK_NULL_HANDLE;
        }
        return made;
    }
}
