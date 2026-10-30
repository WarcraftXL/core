// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The corpus's translated programs made into modules and pipelines on the machine's driver: each
// vertex program paired with a pixel program that reads every varying, each pixel program with a
// vertex program that writes every varying, against the renderer's descriptor set and push
// constants, drawing into a B8G8R8A8 colour and a D24S8 depth.

#include "gx/vk/device/Device.hpp"
#include "gx/vk/shaders/Interface.hpp"
#include "gx/vk/shaders/Spirv.hpp"
#include "gx/vk/shaders/Translate.hpp"

#include <cstdio>
#include <vector>

namespace wxl::vk::test
{
    namespace
    {
        using spirv::Id;

        /// A vertex program that writes every varying and the position.
        std::vector<uint32_t> WritesEveryVarying()
        {
            spirv::Module m(SpvExecutionModelVertex);
            m.SetVersion(spirv::Module::kVersion15);
            const Id f32 = m.Float(), vec4 = m.Vector(f32, 4);
            m.BeginMain({});
            const Id one = m.ConstComposite(vec4, {m.ConstFloat(0.5f), m.ConstFloat(0.5f), m.ConstFloat(0.5f), m.ConstFloat(1.0f)});
            for (uint32_t loc = 0; loc < shaders::kVaryings; ++loc)
            {
                const Id var = m.Variable(m.Pointer(SpvStorageClassOutput, vec4), SpvStorageClassOutput);
                m.Decorate(var, SpvDecorationLocation, {loc});
                m.AddInterface(var);
                m.OpVoid(SpvOpStore, {var, one});
            }
            const Id pos = m.Variable(m.Pointer(SpvStorageClassOutput, vec4), SpvStorageClassOutput);
            m.Decorate(pos, SpvDecorationBuiltIn, {SpvBuiltInPosition});
            m.AddInterface(pos);
            m.OpVoid(SpvOpStore, {pos, one});
            m.EndMain();
            return m.Words();
        }

        /// A pixel program that writes its colour input.
        std::vector<uint32_t> ReadsColor()
        {
            spirv::Module m(SpvExecutionModelFragment);
            m.SetVersion(spirv::Module::kVersion15);
            const Id f32 = m.Float(), vec4 = m.Vector(f32, 4);
            m.BeginMain({});
            const Id in  = m.Variable(m.Pointer(SpvStorageClassInput, vec4), SpvStorageClassInput);
            const Id out = m.Variable(m.Pointer(SpvStorageClassOutput, vec4), SpvStorageClassOutput);
            m.Decorate(in, SpvDecorationLocation, {0});
            m.Decorate(out, SpvDecorationLocation, {0});
            m.AddInterface(in);
            m.AddInterface(out);
            m.ExecutionMode(SpvExecutionModeOriginUpperLeft);
            m.OpVoid(SpvOpStore, {out, m.Op(SpvOpLoad, vec4, {in})});
            m.EndMain();
            return m.Words();
        }

        VkShaderModule Module(VkDevice device, const std::vector<uint32_t>& words)
        {
            VkShaderModuleCreateInfo info{};
            info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize = words.size() * 4;
            info.pCode    = words.data();
            VkShaderModule made = VK_NULL_HANDLE;
            return vkCreateShaderModule(device, &info, nullptr, &made) == VK_SUCCESS ? made : VK_NULL_HANDLE;
        }
    }

    bool CorpusDevice(const std::vector<std::vector<uint32_t>>& modules, const std::vector<shaders::ProgramInfo>& infos, uint32_t& made,
                      uint32_t& failed)
    {
        device::Context context;
        device::Options options;
        options.floor.present = false;
        char why[256]         = {};
        if (!context.Create(options, why, sizeof why))
        {
            std::printf("%s\n", why);
            return false;
        }
        const VkDevice dev = context.Device();

        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[0].descriptorCount = 4096;
        bindings[0].stageFlags      = VK_SHADER_STAGE_ALL_GRAPHICS;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[1].descriptorCount = 256;
        bindings[1].stageFlags      = VK_SHADER_STAGE_ALL_GRAPHICS;
        VkDescriptorBindingFlags flags[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagInfo{};
        flagInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagInfo.bindingCount  = 2;
        flagInfo.pBindingFlags = flags;
        VkDescriptorSetLayoutCreateInfo setInfo{};
        setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.pNext        = &flagInfo;
        setInfo.bindingCount = 2;
        setInfo.pBindings    = bindings;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(dev, &setInfo, nullptr, &setLayout);
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(shaders::Push)};
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount         = 1;
        layoutInfo.pSetLayouts            = &setLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &push;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &layout);

        const VkShaderModule every = Module(dev, WritesEveryVarying());
        const VkShaderModule color = Module(dev, ReadsColor());

        for (size_t i = 0; i < modules.size(); ++i)
        {
            const VkShaderModule mine = Module(dev, modules[i]);
            if (!mine)
            {
                ++failed;
                continue;
            }
            const bool pixel = infos[i].pixel;
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = pixel ? every : mine;
            stages[0].pName  = "main";
            stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = pixel ? mine : color;
            stages[1].pName  = "main";

            VkVertexInputAttributeDescription attributes[16]{};
            uint32_t                          count = 0;
            for (uint32_t loc = 0; loc < shaders::kVertexInputs; ++loc)
                if (!pixel && (infos[i].inputs >> loc & 1)) attributes[count++] = {loc, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            const VkVertexInputBindingDescription binding{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
            VkPipelineVertexInputStateCreateInfo input{};
            input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            input.vertexBindingDescriptionCount   = count ? 1 : 0;
            input.pVertexBindingDescriptions      = &binding;
            input.vertexAttributeDescriptionCount = count;
            input.pVertexAttributeDescriptions    = attributes;
            VkPipelineInputAssemblyStateCreateInfo assembly{};
            assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{};
            viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewport.viewportCount = 1;
            viewport.scissorCount  = 1;
            VkPipelineRasterizationStateCreateInfo raster{};
            raster.sType     = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{};
            ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth{};
            depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.colorWriteMask = 0xF;
            VkPipelineColorBlendStateCreateInfo blend{};
            blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            blend.attachmentCount = 1;
            blend.pAttachments    = &attachment;
            const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{};
            dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic.dynamicStateCount = 2;
            dynamic.pDynamicStates    = dynamics;
            const VkFormat colorFormat = VK_FORMAT_B8G8R8A8_UNORM;
            VkPipelineRenderingCreateInfo rendering{};
            rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            rendering.colorAttachmentCount    = 1;
            rendering.pColorAttachmentFormats = &colorFormat;
            VkGraphicsPipelineCreateInfo info{};
            info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            info.pNext               = &rendering;
            info.stageCount          = 2;
            info.pStages             = stages;
            info.pVertexInputState   = &input;
            info.pInputAssemblyState = &assembly;
            info.pViewportState      = &viewport;
            info.pRasterizationState = &raster;
            info.pMultisampleState   = &ms;
            info.pDepthStencilState  = &depth;
            info.pColorBlendState    = &blend;
            info.pDynamicState       = &dynamic;
            info.layout              = layout;
            VkPipeline pipeline      = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) == VK_SUCCESS)
            {
                ++made;
                vkDestroyPipeline(dev, pipeline, nullptr);
            }
            else
                ++failed;
            vkDestroyShaderModule(dev, mine, nullptr);
            if ((made + failed) % 500 == 0) std::printf("  %u of %zu\n", made + failed, modules.size());
        }
        vkDestroyShaderModule(dev, every, nullptr);
        vkDestroyShaderModule(dev, color, nullptr);
        vkDestroyPipelineLayout(dev, layout, nullptr);
        vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
        return true;
    }
}
