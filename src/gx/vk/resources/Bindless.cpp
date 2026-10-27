// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The bindless table.

#include "gx/vk/resources/Bindless.hpp"

#include "gx/vk/Log.hpp"

#include <algorithm>
#include <cstdio>

namespace wxl::vk::resources
{
    namespace
    {
        constexpr uint32_t kSamplers = 256;
    }

    VkSamplerCreateInfo SamplerInfo(const SamplerKey& key, float max_anisotropy)
    {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        // D3D9's filters by EGxTexFilter (the D3D9 device's s_filterModes): min/mag, then the mip
        // filter, NONE reading the first level only.
        const bool nearest = key.filter == 0 || key.filter == 2;
        info.magFilter     = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        info.minFilter     = info.magFilter;
        const bool mips    = key.filter >= 2;
        info.mipmapMode    = key.filter >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.minLod        = 0.0f;
        info.maxLod        = mips ? VK_LOD_CLAMP_NONE : 0.25f;
        info.addressModeU  = key.wrap_u ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV  = key.wrap_v ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (key.filter == 5 && max_anisotropy > 1.0f && key.anisotropy > 1)
        {
            info.anisotropyEnable = VK_TRUE;
            info.maxAnisotropy    = std::min(static_cast<float>(key.anisotropy), max_anisotropy);
        }
        info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        return info;
    }

    bool Bindless::Create(device::Context& context, uint32_t images, VkImageView black, char* why, uint32_t why_size)
    {
        Destroy();
        context_                     = &context;
        const VkPhysicalDeviceLimits& limits = context.Properties().limits;
        VkPhysicalDeviceVulkan12Properties v12{};
        v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &v12;
        vkGetPhysicalDeviceProperties2(context.Physical(), &props);
        capacity_ = std::min({images, v12.maxDescriptorSetUpdateAfterBindSampledImages,
                              v12.maxPerStageDescriptorUpdateAfterBindSampledImages, 1u << 20});
        const uint32_t samplers = std::min({kSamplers, v12.maxDescriptorSetUpdateAfterBindSamplers,
                                            v12.maxPerStageDescriptorUpdateAfterBindSamplers});
        max_anisotropy_ = context.Chosen().features.samplerAnisotropy ? limits.maxSamplerAnisotropy : 0.0f;

        const VkDevice dev = context.Device();
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[0].descriptorCount = capacity_;
        bindings[0].stageFlags      = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[1].descriptorCount = samplers;
        bindings[1].stageFlags      = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
        const VkDescriptorBindingFlags flags[2] = {
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{};
        bindingFlags.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bindingFlags.bindingCount  = 2;
        bindingFlags.pBindingFlags = flags;
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext        = &bindingFlags;
        layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings    = bindings;
        VkResult result         = vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &layout_);
        if (result == VK_SUCCESS)
        {
            const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity_}, {VK_DESCRIPTOR_TYPE_SAMPLER, samplers}};
            VkDescriptorPoolCreateInfo pool{};
            pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
            pool.maxSets       = 1;
            pool.poolSizeCount = 2;
            pool.pPoolSizes    = sizes;
            result             = vkCreateDescriptorPool(dev, &pool, nullptr, &pool_);
        }
        if (result == VK_SUCCESS)
        {
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool     = pool_;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts        = &layout_;
            result                      = vkAllocateDescriptorSets(dev, &allocate, &set_);
        }
        if (result != VK_SUCCESS || capacity_ < 2)
        {
            std::snprintf(why, why_size, "the bindless table (%u images, %u samplers) could not be made: %s", capacity_, samplers,
                          ResultName(result));
            Destroy();
            return false;
        }
        sampler_capacity_ = samplers;
        Write(0, black);
        used_ = 1;
        Sampler(SamplerKey{}); // slot 0: linear, clamped
        return true;
    }

    void Bindless::Destroy()
    {
        if (context_ && context_->Ready())
        {
            const VkDevice dev = context_->Device();
            for (VkSampler s : samplers_) vkDestroySampler(dev, s, nullptr);
            if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
            if (layout_) vkDestroyDescriptorSetLayout(dev, layout_, nullptr);
        }
        samplers_.clear();
        sampler_slots_.clear();
        free_.clear();
        pool_     = VK_NULL_HANDLE;
        layout_   = VK_NULL_HANDLE;
        set_      = VK_NULL_HANDLE;
        capacity_ = 0;
        sampler_capacity_ = 0;
        used_     = 0;
        next_     = 1;
        context_  = nullptr;
    }

    void Bindless::Write(uint32_t slot, VkImageView view)
    {
        VkDescriptorImageInfo image{};
        image.imageView   = view;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set_;
        write.dstBinding      = 0;
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo      = &image;
        vkUpdateDescriptorSets(context_->Device(), 1, &write, 0, nullptr);
    }

    uint32_t Bindless::AddImage(VkImageView view)
    {
        if (!set_ || !view) return kNoSlot;
        uint32_t slot;
        if (!free_.empty())
        {
            slot = free_.back();
            free_.pop_back();
        }
        else if (next_ < capacity_)
            slot = next_++;
        else
            return kNoSlot;
        Write(slot, view);
        ++used_;
        return slot;
    }

    void Bindless::ReleaseImage(uint32_t slot)
    {
        if (slot == kNoSlot || slot == 0 || slot >= capacity_) return;
        free_.push_back(slot);
        --used_;
    }

    uint32_t Bindless::Sampler(const SamplerKey& key)
    {
        const uint32_t packed = key.Packed();
        const auto     found  = sampler_slots_.find(packed);
        if (found != sampler_slots_.end()) return found->second;
        if (samplers_.size() >= sampler_capacity_) return 0;

        const VkSamplerCreateInfo info = SamplerInfo(key, max_anisotropy_);
        VkSampler                 made = VK_NULL_HANDLE;
        if (vkCreateSampler(context_->Device(), &info, nullptr, &made) != VK_SUCCESS) return 0;
        const uint32_t slot = static_cast<uint32_t>(samplers_.size());
        samplers_.push_back(made);
        VkDescriptorImageInfo image{};
        image.sampler = made;
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set_;
        write.dstBinding      = 1;
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo      = &image;
        vkUpdateDescriptorSets(context_->Device(), 1, &write, 0, nullptr);
        sampler_slots_.emplace(packed, slot);
        return slot;
    }
}
