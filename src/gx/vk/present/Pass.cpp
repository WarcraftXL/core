// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The present pass: two pipelines per target format (the scene, the alpha-keyed cursor) over one
// layout, a descriptor set of each per frame in flight, the gamma ramp in a mapped buffer per frame.

#include "gx/vk/present/Pass.hpp"

#include "gx/vk/Log.hpp"
#include "gx/vk/shaders/Own.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wxl::vk::present
{
    namespace
    {
        VkShaderModule Module(VkDevice device, const std::vector<uint32_t>& words)
        {
            VkShaderModuleCreateInfo info{};
            info.sType          = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize       = words.size() * sizeof(uint32_t);
            info.pCode          = words.data();
            VkShaderModule made = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &info, nullptr, &made) != VK_SUCCESS) return VK_NULL_HANDLE;
            return made;
        }

        VkSampler Sampler(VkDevice device, VkFilter filter)
        {
            VkSamplerCreateInfo info{};
            info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            info.magFilter    = filter;
            info.minFilter    = filter;
            info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.maxLod       = 0.0f;
            VkSampler made    = VK_NULL_HANDLE;
            if (vkCreateSampler(device, &info, nullptr, &made) != VK_SUCCESS) return VK_NULL_HANDLE;
            return made;
        }

        void Identity(float* ramp)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                const float v   = static_cast<float>(i) / 255.0f;
                ramp[i * 4 + 0] = v;
                ramp[i * 4 + 1] = v;
                ramp[i * 4 + 2] = v;
                ramp[i * 4 + 3] = 0.0f;
            }
        }
    }

    bool Pass::Create(device::Context& context, resources::Allocator& allocator, uint32_t slots, char* why, uint32_t why_size)
    {
        Destroy();
        context_           = &context;
        allocator_         = &allocator;
        slot_count_        = std::clamp(slots, 1u, 3u);
        const VkDevice dev = context.Device();

        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo setInfo{};
        setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = 2;
        setInfo.pBindings    = bindings;
        VkResult result      = vkCreateDescriptorSetLayout(dev, &setInfo, nullptr, &set_layout_);

        if (result == VK_SUCCESS)
        {
            const VkPushConstantRange  rect{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
            VkPipelineLayoutCreateInfo layout{};
            layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layout.setLayoutCount         = 1;
            layout.pSetLayouts            = &set_layout_;
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges    = &rect;
            result                = vkCreatePipelineLayout(dev, &layout, nullptr, &layout_);
        }
        if (result == VK_SUCCESS)
        {
            VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * slot_count_},
                                             {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * slot_count_}};
            VkDescriptorPoolCreateInfo pool{};
            pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.maxSets       = 2 * slot_count_;
            pool.poolSizeCount = 2;
            pool.pPoolSizes    = sizes;
            result             = vkCreateDescriptorPool(dev, &pool, nullptr, &pool_);
        }
        for (uint32_t i = 0; result == VK_SUCCESS && i < slot_count_; ++i)
        {
            VkDescriptorSetLayout       layouts[2] = {set_layout_, set_layout_};
            VkDescriptorSet             sets[2]    = {};
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool     = pool_;
            allocate.descriptorSetCount = 2;
            allocate.pSetLayouts        = layouts;
            result                      = vkAllocateDescriptorSets(dev, &allocate, sets);
            slots_[i].scene             = sets[0];
            slots_[i].cursor            = sets[1];
            if (result == VK_SUCCESS &&
                (!allocator.CreateBuffer(shaders::kGammaRows * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, resources::Access::Upload,
                                         slots_[i].gamma) ||
                 !allocator.CreateBuffer(32 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, resources::Access::Upload, slots_[i].staging)))
                result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        if (result != VK_SUCCESS)
        {
            std::snprintf(why, why_size, "the present pass's layout, descriptors or buffers could not be made: %s", ResultName(result));
            Destroy();
            return false;
        }

        vertex_   = Module(dev, shaders::PresentVertex());
        fragment_ = Module(dev, shaders::PresentFragment(false));
        linear_   = Sampler(dev, VK_FILTER_LINEAR);
        nearest_  = Sampler(dev, VK_FILTER_NEAREST);
        if (!vertex_ || !fragment_ || !linear_ || !nearest_ ||
            !allocator.CreateImage(32, 32, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, cursor_))
        {
            std::snprintf(why, why_size, "the present pass's shaders, samplers or cursor image could not be made");
            Destroy();
            return false;
        }
        Identity(ramp_);
        ++ramp_version_;
        gamma_on_ = false;
        return true;
    }

    void Pass::Destroy()
    {
        if (context_ && context_->Ready())
        {
            const VkDevice dev = context_->Device();
            for (Pipelines& p : pipelines_)
            {
                if (p.opaque) vkDestroyPipeline(dev, p.opaque, nullptr);
                if (p.keyed) vkDestroyPipeline(dev, p.keyed, nullptr);
            }
            for (Slot& slot : slots_)
            {
                allocator_->Destroy(slot.gamma);
                allocator_->Destroy(slot.staging);
            }
            allocator_->Destroy(cursor_);
            if (vertex_) vkDestroyShaderModule(dev, vertex_, nullptr);
            if (fragment_) vkDestroyShaderModule(dev, fragment_, nullptr);
            if (linear_) vkDestroySampler(dev, linear_, nullptr);
            if (nearest_) vkDestroySampler(dev, nearest_, nullptr);
            if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
            if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
            if (set_layout_) vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
        }
        pipelines_.clear();
        refused_.clear();
        for (Slot& slot : slots_) slot = Slot{};
        vertex_ = fragment_ = VK_NULL_HANDLE;
        linear_ = nearest_ = VK_NULL_HANDLE;
        pool_              = VK_NULL_HANDLE;
        layout_            = VK_NULL_HANDLE;
        set_layout_        = VK_NULL_HANDLE;
        slot_count_        = 0;
        cursor_ready_      = false;
        cursor_dirty_      = false;
    }

    void Pass::SetGamma(const GammaRamp* ramp)
    {
        if (!ramp)
        {
            if (!gamma_on_) return;
            Identity(ramp_);
            gamma_on_ = false;
        }
        else
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                ramp_[i * 4 + 0] = static_cast<float>(ramp->red[i]) / 65535.0f;
                ramp_[i * 4 + 1] = static_cast<float>(ramp->green[i]) / 65535.0f;
                ramp_[i * 4 + 2] = static_cast<float>(ramp->blue[i]) / 65535.0f;
                ramp_[i * 4 + 3] = 0.0f;
            }
            gamma_on_ = true;
        }
        ++ramp_version_;
    }

    void Pass::SetCursor(const uint32_t* argb)
    {
        if (!argb) return;
        std::memcpy(cursor_pixels_, argb, sizeof cursor_pixels_);
        cursor_dirty_ = true;
    }

    void Pass::Prepare(VkCommandBuffer cmd, uint32_t slot)
    {
        Slot& s = slots_[slot % slot_count_];
        if (s.gamma_version != ramp_version_ && s.gamma.mapped)
        {
            // The slot's previous frame is complete: its buffer is the CPU's to write.
            std::memcpy(s.gamma.mapped, ramp_, sizeof ramp_);
            s.gamma_version = ramp_version_;
        }
        if (!cursor_dirty_ || !s.staging.mapped) return;

        // 0xAARRGGBB in memory is B, G, R, A: B8G8R8A8 as it is.
        std::memcpy(s.staging.mapped, cursor_pixels_, sizeof cursor_pixels_);
        VkImageMemoryBarrier2 toCopy{};
        toCopy.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toCopy.srcStageMask     = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toCopy.srcAccessMask    = VK_ACCESS_2_NONE;
        toCopy.dstStageMask     = VK_PIPELINE_STAGE_2_COPY_BIT;
        toCopy.dstAccessMask    = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toCopy.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED; // the whole image is written
        toCopy.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toCopy.image            = cursor_.image;
        toCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers    = &toCopy;
        vkCmdPipelineBarrier2(cmd, &dependency);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {32, 32, 1};
        vkCmdCopyBufferToImage(cmd, s.staging.buffer, cursor_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier2 toRead = toCopy;
        toRead.srcStageMask          = VK_PIPELINE_STAGE_2_COPY_BIT;
        toRead.srcAccessMask         = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toRead.dstStageMask          = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toRead.dstAccessMask         = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toRead.oldLayout             = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout             = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dependency.pImageMemoryBarriers = &toRead;
        vkCmdPipelineBarrier2(cmd, &dependency);
        cursor_dirty_ = false;
        cursor_ready_ = true;
    }

    const Pass::Pipelines* Pass::PipelinesFor(VkFormat format)
    {
        for (const Pipelines& p : pipelines_)
            if (p.format == format) return &p;
        for (VkFormat f : refused_)
            if (f == format) return nullptr;

        const VkDevice dev       = context_->Device();
        VkShaderModule alphaKey  = Module(dev, shaders::PresentFragment(true));
        Pipelines      made;
        made.format              = format;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_;
        stages[1].pName  = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;
        raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth   = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments    = &attachment;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamicStates;
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount    = 1;
        rendering.pColorAttachmentFormats = &format;

        VkGraphicsPipelineCreateInfo info{};
        info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext               = &rendering;
        info.stageCount          = 2;
        info.pStages             = stages;
        info.pVertexInputState   = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState      = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisample;
        info.pColorBlendState    = &blend;
        info.pDynamicState       = &dynamic;
        info.layout              = layout_;

        VkResult result = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &made.opaque);
        if (result == VK_SUCCESS && alphaKey)
        {
            stages[1].module = alphaKey;
            result           = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &made.keyed);
        }
        if (alphaKey) vkDestroyShaderModule(dev, alphaKey, nullptr);
        if (result != VK_SUCCESS || !alphaKey)
        {
            Log(LogLevel::Warn, "the present pass has no pipeline for format %d: %s", static_cast<int>(format), ResultName(result));
            if (made.opaque) vkDestroyPipeline(dev, made.opaque, nullptr);
            if (made.keyed) vkDestroyPipeline(dev, made.keyed, nullptr);
            refused_.push_back(format);
            return nullptr;
        }
        pipelines_.push_back(made);
        return &pipelines_.back();
    }

    bool Pass::Record(VkCommandBuffer cmd, uint32_t slot, VkImageView source, VkImageView target, VkFormat target_format,
                      VkExtent2D target_extent, const CursorDraw* cursor)
    {
        const Pipelines* pipelines = PipelinesFor(target_format);
        if (!pipelines) return false;
        Slot& s = slots_[slot % slot_count_];

        // The slot's sets are not in use (its previous frame is complete): written every frame.
        VkDescriptorImageInfo  sceneImage{filter_ == Filter::Nearest ? nearest_ : linear_, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo  cursorImage{nearest_, cursor_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo gamma{s.gamma.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet   writes[4]{};
        for (VkWriteDescriptorSet& w : writes)
        {
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.descriptorCount = 1;
        }
        writes[0].dstSet         = s.scene;
        writes[0].dstBinding     = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo     = &sceneImage;
        writes[1].dstSet         = s.scene;
        writes[1].dstBinding     = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo    = &gamma;
        writes[2]                = writes[0];
        writes[2].dstSet         = s.cursor;
        writes[2].pImageInfo     = &cursorImage;
        writes[3]                = writes[1];
        writes[3].dstSet         = s.cursor;
        vkUpdateDescriptorSets(context_->Device(), 4, writes, 0, nullptr);

        VkRenderingAttachmentInfo color{};
        color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView   = target;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // every pixel is drawn
        color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{};
        rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea           = {{0, 0}, target_extent};
        rendering.layerCount           = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments    = &color;
        vkCmdBeginRendering(cmd, &rendering);

        VkViewport whole{0.0f, 0.0f, static_cast<float>(target_extent.width), static_cast<float>(target_extent.height), 0.0f, 1.0f};
        VkRect2D   all{{0, 0}, target_extent};
        vkCmdSetViewport(cmd, 0, 1, &whole);
        vkCmdSetScissor(cmd, 0, 1, &all);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->opaque);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &s.scene, 0, nullptr);
        const float whole_rect[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof whole_rect, whole_rect);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        if (cursor && cursor->visible && cursor_ready_ && cursor->size)
        {
            // The part of the square inside the target.
            const int32_t right  = std::min<int32_t>(cursor->x + static_cast<int32_t>(cursor->size), static_cast<int32_t>(target_extent.width));
            const int32_t bottom = std::min<int32_t>(cursor->y + static_cast<int32_t>(cursor->size), static_cast<int32_t>(target_extent.height));
            const int32_t left   = std::max(cursor->x, 0);
            const int32_t top    = std::max(cursor->y, 0);
            if (right > left && bottom > top)
            {
                // The square in clip space over the whole target's viewport; the scissor keeps it in.
                const float w      = static_cast<float>(target_extent.width);
                const float h      = static_cast<float>(target_extent.height);
                const float rect[4] = {2.0f * static_cast<float>(cursor->x) / w - 1.0f, 2.0f * static_cast<float>(cursor->y) / h - 1.0f,
                                       2.0f * static_cast<float>(cursor->x + static_cast<int32_t>(cursor->size)) / w - 1.0f,
                                       2.0f * static_cast<float>(cursor->y + static_cast<int32_t>(cursor->size)) / h - 1.0f};
                VkRect2D clip{{left, top}, {static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)}};
                vkCmdSetScissor(cmd, 0, 1, &clip);
                vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof rect, rect);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->keyed);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &s.cursor, 0, nullptr);
                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
        }
        vkCmdEndRendering(cmd);
        return true;
    }
}
