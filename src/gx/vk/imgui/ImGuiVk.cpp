// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Dear ImGui's Vulkan backend over the renderer's device, its font atlas uploaded by the caller's frame.

#include "gx/vk/imgui/ImGuiVk.hpp"

#include "gx/vk/Log.hpp"

#include <imgui.h>

#include "backends/imgui_impl_vulkan.h"

#include <cstdio>
#include <cstring>

namespace
{
    void CheckResult(VkResult result)
    {
        // The backend asks after every call; only failures are worth a line, and only a few of them.
        static int s_reported = 0;
        if (result < 0 && s_reported < 8)
        {
            ++s_reported;
            wxl::vk::Log(wxl::vk::LogLevel::Warn, "Dear ImGui's Vulkan backend: %s", wxl::vk::ResultName(result));
        }
    }
}

namespace wxl::vk::imgui
{
    bool Renderer::Create(device::Context& context, resources::Allocator& allocator, VkFormat color, uint32_t samples, uint32_t frames,
                          char* why, uint32_t why_size)
    {
        Destroy();
        context_   = &context;
        allocator_ = &allocator;
        format_    = color;
        samples_   = samples ? samples : 1;

        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion          = kFloorApi;
        info.Instance            = context.Instance();
        info.PhysicalDevice      = context.Physical();
        info.Device              = context.Device();
        info.QueueFamily         = context.QueueFamily();
        info.Queue               = context.Queue();
        info.DescriptorPoolSize  = 64; // the font atlas, and the textures mods show
        info.MinImageCount       = 2;
        // The backend keeps one vertex and index buffer per "image" and reuses them in turn: one per
        // frame in flight, so a buffer is written again only once the frame that read it completed.
        info.ImageCount          = frames < 2 ? 2 : frames;
        info.MSAASamples         = static_cast<VkSampleCountFlagBits>(samples_);
        info.UseDynamicRendering = true;
        info.PipelineRenderingCreateInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        info.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
        info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format_;
        info.CheckVkResultFn     = &CheckResult;
        if (!ImGui_ImplVulkan_Init(&info))
        {
            std::snprintf(why, why_size, "Dear ImGui's Vulkan backend did not start");
            return false;
        }
        ready_ = true;

        VkSamplerCreateInfo sampler{};
        sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter    = VK_FILTER_LINEAR;
        sampler.minFilter    = VK_FILTER_LINEAR;
        sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod       = 1000.0f;
        if (vkCreateSampler(context.Device(), &sampler, nullptr, &sampler_) != VK_SUCCESS)
        {
            sampler_ = VK_NULL_HANDLE;
            std::snprintf(why, why_size, "the font atlas's sampler could not be made");
            Destroy();
            return false;
        }
        return true;
    }

    void Renderer::Destroy()
    {
        if (ready_)
        {
            ImGui_ImplVulkan_Shutdown();
            // The atlas's descriptor set went with the backend's pool.
            if (ImGui::GetCurrentContext() && ImGui::GetIO().Fonts->TexID == (ImTextureID)(font_set_))
                ImGui::GetIO().Fonts->SetTexID(ImTextureID{});
        }
        if (context_ && context_->Ready())
        {
            allocator_->Destroy(font_);
            if (sampler_) vkDestroySampler(context_->Device(), sampler_, nullptr);
        }
        sampler_  = VK_NULL_HANDLE;
        font_set_ = VK_NULL_HANDLE;
        ready_    = false;
    }

    void Renderer::NewFrame(VkCommandBuffer cmd, const std::function<void(std::function<void()>)>& after_frame)
    {
        if (!ready_) return;
        ImGuiIO& io = ImGui::GetIO();
        if (font_set_ && io.Fonts->TexID == (ImTextureID)(font_set_)) return;

        unsigned char* pixels = nullptr;
        int            width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width <= 0 || height <= 0) return;
        const VkDeviceSize size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;

        // A previous atlas goes once the frames that drew with it complete.
        if (font_.image || font_set_)
        {
            resources::Image     old       = font_;
            resources::Allocator* allocator = allocator_;
            const VkDescriptorSet set       = font_set_;
            after_frame([allocator, old, set]() mutable {
                if (set) ImGui_ImplVulkan_RemoveTexture(set);
                allocator->Destroy(old);
            });
            font_     = resources::Image{};
            font_set_ = VK_NULL_HANDLE;
        }

        resources::Buffer staging;
        if (!allocator_->CreateImage(static_cast<uint32_t>(width), static_cast<uint32_t>(height), VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, font_) ||
            !allocator_->CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, resources::Access::Upload, staging) || !staging.mapped)
        {
            allocator_->Destroy(font_);
            allocator_->Destroy(staging);
            return;
        }
        std::memcpy(staging.mapped, pixels, static_cast<size_t>(size));

        VkImageMemoryBarrier2 barrier{};
        barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask     = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstStageMask     = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask    = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.image            = font_.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, font_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dependency);

        resources::Allocator* allocator = allocator_;
        after_frame([allocator, staging]() mutable { allocator->Destroy(staging); });

        font_set_ = ImGui_ImplVulkan_AddTexture(sampler_, font_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        io.Fonts->SetTexID((ImTextureID)(font_set_));
    }

    void Renderer::Record(VkCommandBuffer cmd, ImDrawData* data)
    {
        if (!ready_ || !data || !font_set_) return;
        // A command recorded before the atlas had its descriptor names none: the atlas it meant.
        for (int n = 0; n < data->CmdListsCount; ++n)
            for (ImDrawCmd& command : data->CmdLists[n]->CmdBuffer)
                if (!command.UserCallback && command.TextureId == ImTextureID{}) command.TextureId = (ImTextureID)(font_set_);
        ImGui_ImplVulkan_RenderDrawData(data, cmd);
    }
}
