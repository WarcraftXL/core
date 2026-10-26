// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The swapchain and the rules it is made by.

#include "gx/vk/present/Swapchain.hpp"

#include "gx/vk/Log.hpp"
#include "gx/vk/frame/Frame.hpp"

#include <algorithm>
#include <cstdio>

namespace wxl::vk::present
{
    VkPresentModeKHR ChoosePresentMode(Sync sync, const VkPresentModeKHR* modes, uint32_t count)
    {
        if (sync == Sync::On) return VK_PRESENT_MODE_FIFO_KHR;
        bool mailbox = false, immediate = false;
        for (uint32_t i = 0; i < count; ++i)
        {
            mailbox   = mailbox || modes[i] == VK_PRESENT_MODE_MAILBOX_KHR;
            immediate = immediate || modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        return mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    }

    namespace
    {
        bool IsSrgb(VkFormat format)
        {
            switch (format)
            {
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            case VK_FORMAT_B8G8R8_SRGB:
            case VK_FORMAT_R8G8B8_SRGB: return true;
            default: return false;
            }
        }
    }

    VkSurfaceFormatKHR ChooseFormat(const VkSurfaceFormatKHR* formats, uint32_t count)
    {
        static const VkFormat kPreferred[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                              VK_FORMAT_A2B10G10R10_UNORM_PACK32};
        for (VkFormat want : kPreferred)
            for (uint32_t i = 0; i < count; ++i)
                if (formats[i].format == want && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return formats[i];
        for (uint32_t i = 0; i < count; ++i)
            if (!IsSrgb(formats[i].format) && formats[i].format != VK_FORMAT_UNDEFINED) return formats[i];
        if (count) return formats[0];
        return VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    }

    VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D wanted)
    {
        if (caps.currentExtent.width != 0xFFFFFFFFu) return caps.currentExtent;
        VkExtent2D out;
        out.width  = std::clamp(wanted.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        out.height = std::clamp(wanted.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        if (wanted.width == 0 || wanted.height == 0) out = {0, 0};
        return out;
    }

    uint32_t ChooseImageCount(const VkSurfaceCapabilitiesKHR& caps, VkPresentModeKHR mode)
    {
        uint32_t count = caps.minImageCount + 1;
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR && count < 3) count = 3;
        if (count < 2) count = 2;
        if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
        return count;
    }

    const char* PresentModeName(VkPresentModeKHR mode)
    {
        switch (mode)
        {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default: return "another mode";
        }
    }

    Outcome Classify(VkResult result)
    {
        switch (result)
        {
        case VK_SUCCESS: return Outcome::Ok;
        case VK_SUBOPTIMAL_KHR: return Outcome::Suboptimal;
        case VK_ERROR_OUT_OF_DATE_KHR: return Outcome::Rebuild;
        case VK_ERROR_SURFACE_LOST_KHR: return Outcome::SurfaceLost;
        case VK_TIMEOUT:
        case VK_NOT_READY: return Outcome::Timeout;
        case VK_ERROR_DEVICE_LOST: return Outcome::Lost;
        // Windows takes exclusive fullscreen away from a swapchain that had it implicitly (some
        // drivers do so for a window covering the screen): a rebuild, like out of date.
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return Outcome::Rebuild;
        default: return Outcome::Failed;
        }
    }

    const char* OutcomeName(Outcome outcome)
    {
        switch (outcome)
        {
        case Outcome::Ok: return "ok";
        case Outcome::Suboptimal: return "suboptimal";
        case Outcome::Rebuild: return "out of date";
        case Outcome::SurfaceLost: return "surface lost";
        case Outcome::Timeout: return "timed out";
        case Outcome::Lost: return "device lost";
        default: return "failed";
        }
    }

    bool Swapchain::Build(device::Context& context, VkSurfaceKHR surface, VkExtent2D wanted, Sync sync,
                          resources::Graveyard& graveyard, uint64_t serial, char* why, uint32_t why_size)
    {
        if (why_size) why[0] = '\0';
        context_                    = &context;
        const VkPhysicalDevice phys = context.Physical();
        const VkDevice         dev  = context.Device();
        zero_                       = false;

        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, context.QueueFamily(), surface, &supported);
        if (!supported)
        {
            std::snprintf(why, why_size, "queue family %u cannot present to this surface", context.QueueFamily());
            return false;
        }

        VkSurfaceCapabilitiesKHR caps{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
        if (result != VK_SUCCESS)
        {
            std::snprintf(why, why_size, "the surface's capabilities could not be read: %s", ResultName(result));
            if (result == VK_ERROR_DEVICE_LOST) context.MarkLost("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
            return false;
        }
        const VkExtent2D extent = ChooseExtent(caps, wanted);
        if (extent.width == 0 || extent.height == 0)
        {
            // Minimised: the old swapchain goes, a new one comes when the window does.
            Retire(graveyard, serial);
            zero_ = true;
            return false;
        }

        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        if (count) vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, formats.data());
        formats.resize(count);
        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        if (modeCount) vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &modeCount, modes.data());
        modes.resize(modeCount);

        const VkSurfaceFormatKHR format = ChooseFormat(formats.data(), count);
        const VkPresentModeKHR   mode   = ChoosePresentMode(sync, modes.data(), modeCount);

        VkSwapchainCreateInfoKHR info{};
        info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface          = surface;
        info.minImageCount    = ChooseImageCount(caps, mode);
        info.imageFormat      = format.format;
        info.imageColorSpace  = format.colorSpace;
        info.imageExtent      = extent;
        info.imageArrayLayers = 1;
        info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform     = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                                    : caps.currentTransform;
        info.compositeAlpha   = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                    ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                    : static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1u));
        info.presentMode      = mode;
        info.clipped          = VK_TRUE;
        info.oldSwapchain     = swapchain_;

        VkSwapchainKHR made = VK_NULL_HANDLE;
        result              = vkCreateSwapchainKHR(dev, &info, nullptr, &made);
        // The previous swapchain is retired either way: a failed creation still retires oldSwapchain.
        Retire(graveyard, serial);
        if (result != VK_SUCCESS)
        {
            if (result == VK_ERROR_DEVICE_LOST) context.MarkLost("vkCreateSwapchainKHR");
            std::snprintf(why, why_size, "vkCreateSwapchainKHR (%ux%u, %s) failed: %s", extent.width, extent.height,
                          PresentModeName(mode), ResultName(result));
            return false;
        }
        swapchain_ = made;
        format_    = format;
        extent_    = extent;
        mode_      = mode;

        uint32_t imageCount = 0;
        vkGetSwapchainImagesKHR(dev, swapchain_, &imageCount, nullptr);
        images_.resize(imageCount);
        vkGetSwapchainImagesKHR(dev, swapchain_, &imageCount, images_.data());
        views_.assign(imageCount, VK_NULL_HANDLE);
        rendered_.assign(imageCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < imageCount; ++i)
        {
            VkImageViewCreateInfo view{};
            view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image            = images_[i];
            view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            view.format           = format.format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            result                = vkCreateImageView(dev, &view, nullptr, &views_[i]);
            rendered_[i]          = frame::MakeSemaphore(dev);
            if (result != VK_SUCCESS || !rendered_[i])
            {
                std::snprintf(why, why_size, "the swapchain's image %u view or semaphore could not be made: %s", i, ResultName(result));
                Destroy();
                return false;
            }
        }
        Log(LogLevel::Debug, "swapchain %ux%u, %u images, %s, format %d", extent.width, extent.height, imageCount,
            PresentModeName(mode), static_cast<int>(format.format));
        return true;
    }

    void Swapchain::Retire(resources::Graveyard& graveyard, uint64_t serial)
    {
        if (!context_ || (!swapchain_ && views_.empty() && rendered_.empty())) return;
        const VkDevice           dev       = context_->Device();
        const VkSwapchainKHR     swapchain = swapchain_;
        std::vector<VkImageView> views     = std::move(views_);
        std::vector<VkSemaphore> rendered  = std::move(rendered_);
        // A present waits on its semaphore after the frame's submission completed: what a later
        // submission completing guarantees, so they go one serial later.
        graveyard.Bury(serial + 1, [dev, swapchain, views = std::move(views), rendered = std::move(rendered)] {
            for (VkImageView v : views)
                if (v) vkDestroyImageView(dev, v, nullptr);
            for (VkSemaphore s : rendered)
                if (s) vkDestroySemaphore(dev, s, nullptr);
            if (swapchain) vkDestroySwapchainKHR(dev, swapchain, nullptr);
        });
        swapchain_ = VK_NULL_HANDLE;
        images_.clear();
        views_.clear();
        rendered_.clear();
    }

    void Swapchain::Destroy()
    {
        if (context_ && context_->Ready())
        {
            const VkDevice dev = context_->Device();
            for (VkImageView v : views_)
                if (v) vkDestroyImageView(dev, v, nullptr);
            for (VkSemaphore s : rendered_)
                if (s) vkDestroySemaphore(dev, s, nullptr);
            if (swapchain_) vkDestroySwapchainKHR(dev, swapchain_, nullptr);
        }
        swapchain_ = VK_NULL_HANDLE;
        images_.clear();
        views_.clear();
        rendered_.clear();
        zero_ = false;
    }

    Outcome Swapchain::Acquire(VkSemaphore semaphore, uint64_t timeout_ns, uint32_t& index)
    {
        if (!swapchain_) return Outcome::Rebuild;
        const VkResult result = vkAcquireNextImageKHR(context_->Device(), swapchain_, timeout_ns, semaphore, VK_NULL_HANDLE, &index);
        const Outcome  out    = Classify(result);
        if (out == Outcome::Lost) context_->MarkLost("vkAcquireNextImageKHR");
        return out;
    }

    Outcome Swapchain::Present(uint32_t index)
    {
        VkPresentInfoKHR info{};
        info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores    = &rendered_[index];
        info.swapchainCount     = 1;
        info.pSwapchains        = &swapchain_;
        info.pImageIndices      = &index;
        const VkResult result   = vkQueuePresentKHR(context_->Queue(), &info);
        const Outcome  out      = Classify(result);
        if (out == Outcome::Lost) context_->MarkLost("vkQueuePresentKHR");
        return out;
    }
}
