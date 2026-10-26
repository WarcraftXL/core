// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The renderer's memory: VMA (deps/vma) on the device, buffers and images made through it, and
// what the driver says of its budget. Pools, rings, textures and the bindless table build on this
// (step V2).

#pragma once

#include "gx/vk/device/Device.hpp"

#include <cstdint>

// The allocator's handles, declared here so that only Resources.cpp includes VMA.
struct VmaAllocator_T;
struct VmaAllocation_T;

namespace wxl::vk::resources
{
    /// Where a buffer's memory lives for the CPU.
    enum class Access
    {
        Device,   ///< device-local, not mapped
        Upload,   ///< mapped, written by the CPU in order (staging, rings)
        Readback, ///< mapped and cached, read by the CPU after the GPU wrote it
    };

    struct Buffer
    {
        VkBuffer         buffer = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkDeviceSize     size = 0;
        void*            mapped = nullptr; ///< for Upload and Readback, for the buffer's life
    };

    struct Image
    {
        VkImage          image = VK_NULL_HANDLE;
        VkImageView      view = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkFormat         format = VK_FORMAT_UNDEFINED;
        uint32_t         width = 0;
        uint32_t         height = 0;
    };

    struct Budget
    {
        uint64_t budget = 0; ///< bytes the driver lets the process use, over the device-local heaps
        uint64_t usage = 0;  ///< bytes it uses now
    };

    class Allocator
    {
    public:
        Allocator() = default;
        Allocator(const Allocator&)            = delete;
        Allocator& operator=(const Allocator&) = delete;
        ~Allocator() { Destroy(); }

        bool Create(const device::Context& context, char* why, uint32_t why_size);
        void Destroy();
        bool Ready() const { return vma_ != nullptr; }

        bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Access access, Buffer& out);
        void Destroy(Buffer& buffer);

        /// A 2D image with one mip and one layer, and its colour view.
        bool CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, Image& out);
        void Destroy(Image& image);

        /// Makes the CPU's reads of a Readback buffer see what the GPU wrote.
        void Invalidate(const Buffer& buffer);

        Budget CurrentBudget() const;

    private:
        VkDevice        device_ = VK_NULL_HANDLE;
        VmaAllocator_T* vma_ = nullptr;
    };
}
