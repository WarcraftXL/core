// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The swapchain on a surface: what is chosen for it (pure, so the rules are tested without a
// window), and the swapchain itself. Every state a surface can be in that is not a lost device is
// absorbed: out of date, suboptimal, a zero extent (the window minimised) and a lost surface are
// answers that ask for a rebuild, never errors.

#pragma once

#include "gx/vk/device/Device.hpp"
#include "gx/vk/resources/Resources.hpp"

#include <cstdint>
#include <vector>

namespace wxl::vk::present
{
    /// How a frame shows: the client's gxVSync.
    enum class Sync
    {
        Off, ///< as soon as it is drawn: MAILBOX, else IMMEDIATE, else FIFO
        On,  ///< at the display's refresh: FIFO
    };

    /// The present mode for @p sync among the @p count modes the surface offers. FIFO is always there.
    VkPresentModeKHR ChoosePresentMode(Sync sync, const VkPresentModeKHR* modes, uint32_t count);

    /// A UNORM format the scene can be drawn to as it is: B8G8R8A8, then R8G8B8A8, then A2B10G10R10, then
    /// any non-sRGB one, then the first. The engine does its own gamma, so never an sRGB encoding.
    VkSurfaceFormatKHR ChooseFormat(const VkSurfaceFormatKHR* formats, uint32_t count);

    /// The surface's extent when it decides, else @p wanted clamped to what it allows. Zero when the
    /// window is minimised.
    VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D wanted);

    /// One image more than the surface's minimum (two for FIFO, three for MAILBOX at least), within its maximum.
    uint32_t ChooseImageCount(const VkSurfaceCapabilitiesKHR& caps, VkPresentModeKHR mode);

    const char* PresentModeName(VkPresentModeKHR mode);

    /// What an acquire or a present came to.
    enum class Outcome
    {
        Ok,          ///< done
        Suboptimal,  ///< done, but the swapchain should be rebuilt
        Rebuild,     ///< not done: the swapchain no longer matches the surface (out of date)
        SurfaceLost, ///< not done: the surface itself must be made again
        Timeout,     ///< not done in the time given (acquire only)
        Lost,        ///< the device was lost
        Failed,      ///< another error
    };

    Outcome Classify(VkResult result);
    const char* OutcomeName(Outcome outcome);

    class Swapchain
    {
    public:
        Swapchain() = default;
        Swapchain(const Swapchain&)            = delete;
        Swapchain& operator=(const Swapchain&) = delete;
        ~Swapchain() { Destroy(); }

        /**
         * @brief Makes the swapchain on @p surface, or makes it again: the previous one (if any) is
         *        retired into @p graveyard at @p serial (the last submission that may still use it).
         * @param wanted  the window's client size; the surface may decide otherwise
         * @return false with @p why; a zero extent answers false with @p why empty and Zero() true:
         *         nothing to present to until the window comes back.
         */
        bool Build(device::Context& context, VkSurfaceKHR surface, VkExtent2D wanted, Sync sync,
                   resources::Graveyard& graveyard, uint64_t serial, char* why, uint32_t why_size);

        /// Destroys it now: whatever used it must be complete (or the device lost).
        void Destroy();

        /// Hands the swapchain and what depends on it to @p graveyard at @p serial.
        void Retire(resources::Graveyard& graveyard, uint64_t serial);

        bool       Ready() const { return swapchain_ != VK_NULL_HANDLE; }
        bool       Zero() const { return zero_; }
        VkFormat   Format() const { return format_.format; }
        VkExtent2D Extent() const { return extent_; }
        VkPresentModeKHR Mode() const { return mode_; }
        uint32_t   ImageCount() const { return static_cast<uint32_t>(images_.size()); }
        VkImage    Image(uint32_t index) const { return images_[index]; }
        VkImageView View(uint32_t index) const { return views_[index]; }
        /// The semaphore a frame signals when it is done drawing image @p index, which its present waits on.
        VkSemaphore Rendered(uint32_t index) const { return rendered_[index]; }

        /// Acquires the next image within @p timeout_ns, signalling @p semaphore.
        Outcome Acquire(VkSemaphore semaphore, uint64_t timeout_ns, uint32_t& index);

        /// Presents image @p index once its Rendered semaphore is signalled.
        Outcome Present(uint32_t index);

    private:
        device::Context*         context_ = nullptr;
        VkSwapchainKHR           swapchain_ = VK_NULL_HANDLE;
        VkSurfaceFormatKHR       format_{};
        VkExtent2D               extent_{};
        VkPresentModeKHR         mode_ = VK_PRESENT_MODE_FIFO_KHR;
        bool                     zero_ = false;
        std::vector<VkImage>     images_;
        std::vector<VkImageView> views_;
        std::vector<VkSemaphore> rendered_;
    };
}
