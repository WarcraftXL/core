// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The renderer's memory: VMA (deps/vma) on the device, buffers and images made through it, and
// what the driver says of its budget. The rings (Ring.hpp) and the bindless table (Bindless.hpp)
// build on this, and the engine's pools and textures on them (src/gx/vk/draw/).

#pragma once

#include "gx/vk/device/Device.hpp"

#include <cstdint>
#include <deque>
#include <functional>

// The allocator's handles, declared here so that only Resources.cpp includes VMA.
struct VmaAllocator_T;
struct VmaAllocation_T;

namespace wxl::vk::resources
{
    /// Where a buffer's memory lives for the CPU.
    enum class Access
    {
        Device,   ///< device-local, not mapped
        Upload,   ///< mapped, written by the CPU in order (staging, rings); where the allocator chooses
        Readback, ///< mapped and cached, read by the CPU after the GPU wrote it
        /// Mapped and device-local (a BAR heap, all of it with resizable BAR): what the GPU reads
        /// without a copy and the CPU writes in order. Upload when the device has no such memory,
        /// when it is full, or when the allocator's ReBAR use is off.
        DeviceUpload,
    };

    /// The device's memory the CPU can map and the GPU reads at its full speed.
    struct Bar
    {
        uint64_t heap = 0;       ///< bytes of the largest heap with DEVICE_LOCAL | HOST_VISIBLE memory (0: none)
        uint64_t device = 0;     ///< bytes of the largest DEVICE_LOCAL heap
        bool     large = false;  ///< it is most of the device's memory: resizable BAR, or memory the CPU and GPU share
        bool     on = false;     ///< DeviceUpload uses it ([gx] vk_rebar)
    };

    struct Buffer
    {
        VkBuffer         buffer = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkDeviceSize     size = 0;
        void*            mapped = nullptr; ///< for Upload and Readback, for the buffer's life
        VkDeviceAddress  address = 0;      ///< when made with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    };

    struct Image
    {
        VkImage          image = VK_NULL_HANDLE;
        VkImageView      view = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkFormat         format = VK_FORMAT_UNDEFINED;
        uint32_t         width = 0;
        uint32_t         height = 0;
        uint32_t         levels = 1;
        uint32_t         layers = 1;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    };

    /// What an image is made with beyond its size and format.
    struct ImageInfo
    {
        uint32_t           width = 1;
        uint32_t           height = 1;
        uint32_t           levels = 1;
        bool               cube = false; ///< six layers, viewed as a cube
        VkFormat           format = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags  usage = 0;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        VkComponentMapping swizzle = {}; ///< the view's (identity by default)
    };

    struct Budget
    {
        uint64_t budget = 0; ///< bytes the driver lets the process use, over the device-local heaps
        uint64_t usage = 0;  ///< bytes it uses now
        uint64_t host_usage = 0; ///< bytes it uses of the other heaps (system memory the GPU reads)
    };

    /// The size of VMA's blocks of memory: 32 MiB in a 32-bit process, where a mapped block costs
    /// its size in address space; VMA's own 256 MiB otherwise.
    constexpr VkDeviceSize kBlockSize = sizeof(void*) == 4 ? VkDeviceSize{32} << 20 : VkDeviceSize{256} << 20;

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

        /// Whether DeviceUpload uses the device's mapped device-local memory ([gx] vk_rebar).
        void SetBar(bool on) { bar_.on = on && bar_.heap != 0; }
        const Bar& BarMemory() const { return bar_; }
        /// Bytes made in DeviceUpload memory that went to the BAR heap, and those that fell back.
        uint64_t BarBytes() const { return bar_bytes_; }
        uint64_t BarFallbacks() const { return bar_fallbacks_; }

        /// A buffer of @p access; with @p bar_only, DeviceUpload is the BAR heap or nothing (false).
        bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Access access, Buffer& out, bool bar_only = false);
        void Destroy(Buffer& buffer);

        /// A 2D image with one mip and one layer, and its view of @p aspect (colour unless said),
        /// of @p samples samples per pixel (a multisampled attachment when more than one).
        bool CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, Image& out,
                         VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                         VkImageCreateFlags flags = 0);
        void Destroy(Image& image);

        /// An image of @p info, its levels and layers, and its view (a cube's is a cube view).
        bool CreateImage(const ImageInfo& info, Image& out);

        /// A view of one layer of @p image as a 2D image (a cube's face, where a 2D view is wanted).
        VkImageView CreateLayerView(const Image& image, uint32_t layer, const VkComponentMapping& swizzle);

        /// The number of allocations alive: what a test reads to see nothing leaked.
        uint32_t Allocations() const;

        /// Makes the CPU's reads of a Readback buffer see what the GPU wrote.
        void Invalidate(const Buffer& buffer);

        /// Makes what the CPU wrote into @p size bytes at @p offset of an Upload buffer visible to the
        /// GPU, where its memory is not coherent (nothing to do where it is).
        void Flush(const Buffer& buffer, VkDeviceSize offset, VkDeviceSize size);

        Budget CurrentBudget() const;

    private:
        VkDevice        device_ = VK_NULL_HANDLE;
        VmaAllocator_T* vma_ = nullptr;
        Bar             bar_;
        uint64_t        bar_bytes_ = 0;
        uint64_t        bar_fallbacks_ = 0;
    };

    /**
     * What the GPU may still read is released here, by serial: each release waits for the serial
     * of the last submission that may have used it, and runs once the timeline has passed it. The
     * queue has no fixed size, so nothing is dropped and nothing freed early.
     */
    class Graveyard
    {
    public:
        Graveyard() = default;
        Graveyard(const Graveyard&)            = delete;
        Graveyard& operator=(const Graveyard&) = delete;
        ~Graveyard() { Flush(); }

        /// Runs @p release once @p serial has completed.
        void Bury(uint64_t serial, std::function<void()> release);

        /// Runs every release whose serial is at most @p completed, in the order they came.
        void Collect(uint64_t completed);

        /// Runs every release now: the device is idle or lost.
        void Flush();

        size_t Size() const { return dead_.size(); }

    private:
        struct Dead
        {
            uint64_t              serial;
            std::function<void()> release;
        };
        std::deque<Dead> dead_;
    };
}
