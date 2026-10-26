// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// VMA over volk's entry points, and the buffers and images made through it.

#include "gx/vk/resources/Resources.hpp"

#include "gx/vk/Log.hpp"

#include <vk_mem_alloc.h>

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
        return true;
    }

    void Allocator::Destroy()
    {
        if (vma_) vmaDestroyAllocator(vma_);
        vma_    = nullptr;
        device_ = VK_NULL_HANDLE;
    }

    bool Allocator::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Access access, Buffer& out)
    {
        out = Buffer{};
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
        return true;
    }

    void Allocator::Destroy(Buffer& buffer)
    {
        if (buffer.buffer) vmaDestroyBuffer(vma_, buffer.buffer, buffer.allocation);
        buffer = Buffer{};
    }

    bool Allocator::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, Image& out)
    {
        out = Image{};
        VkImageCreateInfo info{};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = format;
        info.extent        = {width, height, 1};
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
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
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            result                = vkCreateImageView(device_, &view, nullptr, &out.view);
        }
        if (result != VK_SUCCESS)
        {
            Log(LogLevel::Warn, "a %ux%u image could not be made: %s", width, height, ResultName(result));
            Destroy(out);
            return false;
        }
        out.format = format;
        out.width  = width;
        out.height = height;
        return true;
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
            if (!(memory->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
            budget.budget += heaps[h].budget;
            budget.usage += heaps[h].usage;
        }
        return budget;
    }
}
