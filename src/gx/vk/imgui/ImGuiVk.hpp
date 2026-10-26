// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Dear ImGui drawn through the renderer: Dear ImGui's own Vulkan backend (imgui_impl_vulkan, from
// the SDK's copy of Dear ImGui) over this renderer's device, with dynamic rendering into a colour
// target of the caller's, and its font atlas uploaded through the caller's frame rather than by the
// backend's own submission and its unbounded wait. ImTextureID is the backend's descriptor set.
//
// The ImGui context in use must be current around every call. Nothing here knows the engine.

#pragma once

#include "gx/vk/device/Device.hpp"
#include "gx/vk/resources/Resources.hpp"

#include <cstdint>
#include <functional>

struct ImDrawData;

namespace wxl::vk::imgui
{
    class Renderer
    {
    public:
        Renderer() = default;
        Renderer(const Renderer&)            = delete;
        Renderer& operator=(const Renderer&) = delete;
        ~Renderer() { Destroy(); }

        /// For a colour target of @p color with @p samples per pixel (the scene's: Dear ImGui is
        /// drawn into it as into D3D9's back buffer, before the cursor), @p frames frames in flight.
        bool Create(device::Context& context, resources::Allocator& allocator, VkFormat color, uint32_t samples, uint32_t frames,
                    char* why, uint32_t why_size);

        /// Releases the backend. Its work must be complete (or the device lost), and the context
        /// it was made with current.
        void Destroy();
        bool Ready() const { return ready_; }
        VkFormat Format() const { return format_; }
        uint32_t Samples() const { return samples_; }

        /**
         * @brief Before ImGui::NewFrame, in a frame, outside any rendering: builds the font atlas and
         *        uploads it when it has not been (or was rebuilt), so the frame's draw commands name
         *        it. @p after_frame takes what may be released once the frame completes.
         */
        void NewFrame(VkCommandBuffer cmd, const std::function<void(std::function<void()>)>& after_frame);

        /// Inside a rendering of the colour format and samples given at Create: draws @p data.
        void Record(VkCommandBuffer cmd, ImDrawData* data);

    private:
        device::Context*      context_ = nullptr;
        resources::Allocator* allocator_ = nullptr;
        VkFormat              format_ = VK_FORMAT_UNDEFINED;
        uint32_t              samples_ = 1;
        bool                  ready_ = false;
        resources::Image      font_;
        VkSampler             sampler_ = VK_NULL_HANDLE;
        VkDescriptorSet       font_set_ = VK_NULL_HANDLE;
    };
}
