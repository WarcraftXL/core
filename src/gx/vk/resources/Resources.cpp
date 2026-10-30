// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// VMA over volk's entry points, and the buffers and images made through it.

#include "gx/vk/resources/Resources.hpp"

#include "gx/vk/Log.hpp"
#include "gx/vk/Stats.hpp"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdio>

namespace wxl::vk::resources
{
    bool Allocator::Create(const device::Context& context, char* why, uint32_t why_size)
    {
        Destroy();
        VmaAllocatorCreateInfo info{};
        info.flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        if (context.Chosen().extensions.memory_budget) info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        info.physicalDevice   = context.Physical();
        info.device           = context.Device();
        info.instance         = context.Instance();
        info.vulkanApiVersion = kFloorApi;
        // VMA maps a whole block of memory to hand out a mapped part of it: in a 32-bit process each
        // mapped block costs its size in address space, so blocks are smaller there.
        info.preferredLargeHeapBlockSize = kBlockSize;

        VmaVulkanFunctions functions{};
        VkResult           result = vmaImportVulkanFunctionsFromVolk(&info, &functions);
        if (result == VK_SUCCESS)
        {
            info.pVulkanFunctions = &functions;
            result                = vmaCreateAllocator(&info, &vma_);
        }
        if (result != VK_SUCCESS)
        {
            vma_ = nullptr;
            std::snprintf(why, why_size, "the memory allocator (VMA) did not start: %s", ResultName(result));
            return false;
        }
        device_ = context.Device();

        // The heap the CPU maps and the GPU reads at its speed: 256 MiB on a discrete card without
        // resizable BAR, most of its memory with it, all of it where both share memory.
        bar_ = Bar{};
        const VkPhysicalDeviceMemoryProperties* memory = nullptr;
        vmaGetMemoryProperties(vma_, &memory);
        constexpr VkMemoryPropertyFlags kBar = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        for (uint32_t t = 0; memory && t < memory->memoryTypeCount; ++t)
        {
            const VkMemoryHeap& heap = memory->memoryHeaps[memory->memoryTypes[t].heapIndex];
            if (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) bar_.device = std::max<uint64_t>(bar_.device, heap.size);
            if ((memory->memoryTypes[t].propertyFlags & kBar) == kBar) bar_.heap = std::max<uint64_t>(bar_.heap, heap.size);
        }
        bar_.large = bar_.heap && bar_.heap >= std::max<uint64_t>(uint64_t{1} << 30, bar_.device / 2);
        bar_bytes_ = bar_fallbacks_ = 0;
        return true;
    }

    void Allocator::Destroy()
    {
        if (vma_) vmaDestroyAllocator(vma_);
        vma_    = nullptr;
        device_ = VK_NULL_HANDLE;
    }

    bool Allocator::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Access access, Buffer& out, bool bar_only)
    {
        stats::Scope made(stats::Cause::Memory);
        out = Buffer{};
        if (access == Access::DeviceUpload)
        {
            if (bar_.on)
            {
                // Mapped device-local memory, or nothing: never the system memory VMA would fall back to.
                VkBufferCreateInfo info{};
                info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                info.size        = size;
                info.usage       = usage;
                info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                VmaAllocationCreateInfo memory{};
                memory.usage         = VMA_MEMORY_USAGE_UNKNOWN;
                memory.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
                memory.flags         = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo allocated{};
                if (vmaCreateBuffer(vma_, &info, &memory, &out.buffer, &out.allocation, &allocated) == VK_SUCCESS && allocated.pMappedData)
                {
                    out.size   = size;
                    out.mapped = allocated.pMappedData;
                    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
                    {
                        VkBufferDeviceAddressInfo address{};
                        address.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
                        address.buffer = out.buffer;
                        out.address    = vkGetBufferDeviceAddress(device_, &address);
                    }
                    bar_bytes_ += size;
                    return true;
                }
                if (out.buffer) vmaDestroyBuffer(vma_, out.buffer, out.allocation);
                out = Buffer{};
                ++bar_fallbacks_;
            }
            if (bar_only) return false;
            access = Access::Upload;
        }
        VkBufferCreateInfo info{};
        info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size        = size;
        info.usage       = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO;
        if (access == Access::Upload)
            memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        else if (access == Access::Readback)
            memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocated{};
        const VkResult    result = vmaCreateBuffer(vma_, &info, &memory, &out.buffer, &out.allocation, &allocated);
        if (result != VK_SUCCESS)
        {
            Log(LogLevel::Warn, "a buffer of %llu bytes could not be made: %s", static_cast<unsigned long long>(size),
                ResultName(result));
            out = Buffer{};
            return false;
        }
        out.size   = size;
        out.mapped = allocated.pMappedData;
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
        {
            VkBufferDeviceAddressInfo address{};
            address.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            address.buffer = out.buffer;
            out.address    = vkGetBufferDeviceAddress(device_, &address);
        }
        return true;
    }

    void Allocator::Destroy(Buffer& buffer)
    {
        if (buffer.buffer) vmaDestroyBuffer(vma_, buffer.buffer, buffer.allocation);
        buffer = Buffer{};
    }

    bool Allocator::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, Image& out,
                                VkImageAspectFlags aspect, VkSampleCountFlagBits samples, VkImageCreateFlags flags)
    {
        stats::Scope made(stats::Cause::Memory);
        out = Image{};
        VkImageCreateInfo info{};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.flags         = flags;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = format;
        info.extent        = {width, height, 1};
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = samples;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = usage;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        VkResult result = vmaCreateImage(vma_, &info, &memory, &out.image, &out.allocation, nullptr);
        if (result == VK_SUCCESS)
        {
            VkImageViewCreateInfo view{};
            view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image            = out.image;
            view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            view.format           = format;
            view.subresourceRange = {aspect, 0, 1, 0, 1};
            result                = vkCreateImageView(device_, &view, nullptr, &out.view);
        }
        if (result != VK_SUCCESS)
        {
            Log(LogLevel::Warn, "a %ux%u image could not be made: %s", width, height, ResultName(result));
            Destroy(out);
            return false;
        }
        out.format  = format;
        out.width   = width;
        out.height  = height;
        out.samples = samples;
        return true;
    }

    bool Allocator::CreateImage(const ImageInfo& info, Image& out)
    {
        stats::Scope made(stats::Cause::Memory);
        out = Image{};
        VkImageCreateInfo create{};
        create.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        create.flags         = info.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        create.imageType     = VK_IMAGE_TYPE_2D;
        create.format        = info.format;
        create.extent        = {info.width, info.height, 1};
        create.mipLevels     = info.levels ? info.levels : 1;
        create.arrayLayers   = info.cube ? 6 : 1;
        create.samples       = VK_SAMPLE_COUNT_1_BIT;
        create.tiling        = VK_IMAGE_TILING_OPTIMAL;
        create.usage         = info.usage;
        create.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        VkResult result = vmaCreateImage(vma_, &create, &memory, &out.image, &out.allocation, nullptr);
        if (result == VK_SUCCESS)
        {
            VkImageViewCreateInfo view{};
            view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image            = out.image;
            view.viewType         = info.cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
            view.format           = info.format;
            view.components       = info.swizzle;
            view.subresourceRange = {info.aspect, 0, create.mipLevels, 0, create.arrayLayers};
            result                = vkCreateImageView(device_, &view, nullptr, &out.view);
        }
        if (result != VK_SUCCESS)
        {
            Log(LogLevel::Warn, "a %ux%u image (%u levels%s) could not be made: %s", info.width, info.height, create.mipLevels,
                info.cube ? ", cube" : "", ResultName(result));
            Destroy(out);
            return false;
        }
        out.format = info.format;
        out.width  = info.width;
        out.height = info.height;
        out.levels = create.mipLevels;
        out.layers = create.arrayLayers;
        return true;
    }

    VkImageView Allocator::CreateLayerView(const Image& image, uint32_t layer, const VkComponentMapping& swizzle)
    {
        VkImageViewCreateInfo view{};
        view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image            = image.image;
        view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view.format           = image.format;
        view.components       = swizzle;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.levels, layer, 1};
        VkImageView made      = VK_NULL_HANDLE;
        if (vkCreateImageView(device_, &view, nullptr, &made) != VK_SUCCESS) return VK_NULL_HANDLE;
        return made;
    }

    void Allocator::Flush(const Buffer& buffer, VkDeviceSize offset, VkDeviceSize size)
    {
        if (buffer.allocation && size) vmaFlushAllocation(vma_, buffer.allocation, offset, size);
    }

    void Allocator::Destroy(Image& image)
    {
        if (image.view) vkDestroyImageView(device_, image.view, nullptr);
        if (image.image) vmaDestroyImage(vma_, image.image, image.allocation);
        image = Image{};
    }

    void Allocator::Invalidate(const Buffer& buffer)
    {
        if (buffer.allocation) vmaInvalidateAllocation(vma_, buffer.allocation, 0, VK_WHOLE_SIZE);
    }

    uint32_t Allocator::Allocations() const
    {
        if (!vma_) return 0;
        VmaTotalStatistics stats{};
        vmaCalculateStatistics(vma_, &stats);
        return stats.total.statistics.allocationCount;
    }

    void Graveyard::Bury(uint64_t serial, std::function<void()> release)
    {
        if (release) dead_.push_back(Dead{serial, std::move(release)});
    }

    void Graveyard::Collect(uint64_t completed)
    {
        // Serials only grow, so the front is the oldest; a release buried out of order waits behind
        // an older one, which is late but never early.
        while (!dead_.empty() && dead_.front().serial <= completed)
        {
            std::function<void()> release = std::move(dead_.front().release);
            dead_.pop_front();
            release();
        }
    }

    void Graveyard::Flush()
    {
        while (!dead_.empty())
        {
            std::function<void()> release = std::move(dead_.front().release);
            dead_.pop_front();
            release();
        }
    }

    Budget Allocator::CurrentBudget() const
    {
        Budget budget;
        if (!vma_) return budget;
        VmaBudget heaps[VK_MAX_MEMORY_HEAPS] = {};
        vmaGetHeapBudgets(vma_, heaps);
        const VkPhysicalDeviceMemoryProperties* memory = nullptr;
        vmaGetMemoryProperties(vma_, &memory);
        for (uint32_t h = 0; memory && h < memory->memoryHeapCount; ++h)
        {
            if (!(memory->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
            {
                budget.host_usage += heaps[h].usage;
                continue;
            }
            budget.budget += heaps[h].budget;
            budget.usage += heaps[h].usage;
        }
        return budget;
    }
}
