// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The one descriptor set every draw of the engine reads: an array of sampled images (binding 0),
// each texture at the slot it was given when it was made, and an array of samplers (binding 1),
// each a combination of the engine's filter, wrap and anisotropy, made at its first use. The set is
// bound once per rendering and never written for a draw: a draw pushes its slots (draw/).
//
// Slots are written as textures are made (update-after-bind, the frames in flight reading other
// slots of the same set) and given back once nothing in flight reads them (the caller retires them
// by serial). Slot 0 is an opaque black 1x1 image, what a program samples where no texture is set.

#pragma once

#include "gx/vk/device/Device.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wxl::vk::resources
{
    /// A sampler as the engine describes one (CGxTexFlags): its EGxTexFilter, its wrap per axis.
    struct SamplerKey
    {
        uint8_t filter = 1;     ///< EGxTexFilter: nearest, linear, nearest-mip-nearest, linear-mip-nearest, trilinear, anisotropic
        uint8_t wrap_u = 0;     ///< 0 clamp, 1 wrap
        uint8_t wrap_v = 0;
        uint8_t anisotropy = 1; ///< for the anisotropic filter, 1..16

        uint32_t Packed() const { return filter | (wrap_u << 8) | (wrap_v << 9) | (static_cast<uint32_t>(anisotropy) << 16); }
    };

    /// The VkSamplerCreateInfo D3D9 sampling of @p key means (its filters, MIPFILTER NONE as a single
    /// level, ADDRESSW clamp), anisotropy capped at @p max_anisotropy (0: no anisotropy).
    VkSamplerCreateInfo SamplerInfo(const SamplerKey& key, float max_anisotropy);

    class Bindless
    {
    public:
        static constexpr uint32_t kNoSlot = UINT32_MAX;

        Bindless() = default;
        Bindless(const Bindless&)            = delete;
        Bindless& operator=(const Bindless&) = delete;
        ~Bindless() { Destroy(); }

        /// The set for at most @p images images (capped by the device) and 256 samplers; @p black
        /// is the view slot 0 holds.
        bool Create(device::Context& context, uint32_t images, VkImageView black, char* why, uint32_t why_size);
        void Destroy();
        bool Ready() const { return set_ != VK_NULL_HANDLE; }

        VkDescriptorSetLayout Layout() const { return layout_; }
        VkDescriptorSet       Set() const { return set_; }

        /// A slot holding @p view, written now; kNoSlot when the table is full.
        uint32_t AddImage(VkImageView view);
        /// Gives @p slot back; the caller makes sure nothing in flight reads it any more.
        void     ReleaseImage(uint32_t slot);

        /// The slot of the sampler @p key describes, made at its first use; 0 (linear, clamped) when
        /// the table of samplers is full.
        uint32_t Sampler(const SamplerKey& key);

        uint32_t ImagesInUse() const { return used_; }
        uint32_t Capacity() const { return capacity_; }
        uint32_t Samplers() const { return static_cast<uint32_t>(samplers_.size()); }

    private:
        void Write(uint32_t slot, VkImageView view);

        device::Context*       context_ = nullptr;
        VkDescriptorSetLayout  layout_ = VK_NULL_HANDLE;
        VkDescriptorPool       pool_ = VK_NULL_HANDLE;
        VkDescriptorSet        set_ = VK_NULL_HANDLE;
        uint32_t               capacity_ = 0;
        uint32_t               sampler_capacity_ = 0;
        uint32_t               used_ = 0;
        std::vector<uint32_t>  free_;
        uint32_t               next_ = 1; ///< the first slot never handed out
        std::vector<VkSampler> samplers_;
        std::unordered_map<uint32_t, uint32_t> sampler_slots_;
        float                  max_anisotropy_ = 0.0f;
    };
}
