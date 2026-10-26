// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Vulkan device the renderer draws with: the loader, the instance, the physical device taken
// among those that meet the floor, the logical device with its features and its graphics queue,
// and whether the device was lost. One per process.
//
// Every way of not getting a device answers false with one sentence saying why, which the runtime
// logs on the line that says the client stays on D3D9: no loader, a loader or a device below 1.3,
// a missing feature, a device that would not be created.

#pragma once

#include "gx/vk/device/Floor.hpp"

#include <atomic>
#include <cstdint>

namespace wxl::vk::device
{
    /// Whether a queue family of a physical device can present to a window; supplied by present/.
    using PresentQuery = bool (*)(VkInstance instance, VkPhysicalDevice physical, uint32_t family);

    struct Options
    {
        const char*         application = "World of Warcraft";
        Floor               floor;
        const char*         gpu = nullptr;              ///< "[gx] gpu": an index or a part of a name
        bool                validation = false;         ///< VK_LAYER_KHRONOS_validation when installed
        const char* const*  instance_extensions = nullptr; ///< required besides the renderer's own (the surface's)
        uint32_t            instance_extension_count = 0;
        PresentQuery        presents = nullptr;         ///< asked for each graphics family when floor.present
    };

    class Context
    {
    public:
        Context() = default;
        Context(const Context&)            = delete;
        Context& operator=(const Context&) = delete;
        ~Context() { Destroy(); }

        /**
         * @brief Loads the loader, makes the instance and the device.
         * @param why  one sentence when it answers false: what is missing, on which device.
         */
        bool Create(const Options& options, char* why, uint32_t why_size);

        /// Waits for the device to be idle, destroys it and the instance, and unloads the loader.
        void Destroy();

        bool Ready() const { return device_ != VK_NULL_HANDLE; }

        VkInstance       Instance() const { return instance_; }
        VkPhysicalDevice Physical() const { return physical_; }
        VkDevice         Device() const { return device_; }
        VkQueue          Queue() const { return queue_; }
        uint32_t         QueueFamily() const { return chosen_.family; }
        uint32_t         InstanceApi() const { return instance_api_; }

        /// The facts of the device taken, and what is enabled on it.
        const Candidate& Chosen() const { return chosen_; }
        const VkPhysicalDeviceProperties& Properties() const { return properties_; }
        bool             Validation() const { return validation_; }

        /**
         * @brief Notes a result of a submit, a wait or a present: VK_ERROR_DEVICE_LOST marks the
         *        device lost for good, until it is made again.
         * @return whether the result was a success
         */
        bool Check(VkResult result, const char* what);
        bool Lost() const { return lost_.load(std::memory_order_relaxed); }
        void MarkLost(const char* what);

    private:
        bool CreateInstance(const Options& options, char* why, uint32_t why_size);
        bool ChoosePhysical(const Options& options, char* why, uint32_t why_size);
        bool CreateDevice(char* why, uint32_t why_size);

        bool                       loader_ = false;
        uint32_t                   instance_api_ = 0;
        VkInstance                 instance_ = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT   messenger_ = VK_NULL_HANDLE;
        VkPhysicalDevice           physical_ = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties properties_ = {};
        Candidate                  chosen_;
        VkDevice                   device_ = VK_NULL_HANDLE;
        VkQueue                    queue_ = VK_NULL_HANDLE;
        bool                       validation_ = false;
        std::atomic<bool>          lost_{false};
    };

    /// Gathers what Judge reads of @p physical; the caller judges it.
    void Describe(VkInstance instance, VkPhysicalDevice physical, uint32_t index, PresentQuery presents,
                  Candidate& out);
}
