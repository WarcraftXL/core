// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// How work reaches the GPU: command buffers recorded on the main thread, submitted to the one
// graphics queue with synchronization2, each submission a value of one timeline semaphore (its
// serial), and waits that always end: a wait that runs out says so and is the caller's to treat
// as a hung device, never an infinite stall. Frames in flight, the upload pass, dynamic rendering
// and target switches build on this (steps V1-V2).

#pragma once

#include "gx/vk/device/Device.hpp"

#include <cstdint>

namespace wxl::vk::frame
{
    /// Waits are bounded: two seconds unless the caller says otherwise.
    constexpr uint64_t kDefaultWaitNs = 2'000'000'000ull;

    enum class Wait
    {
        Done,    ///< the serial completed
        Timeout, ///< it did not within the time given
        Lost,    ///< the device was lost
        Failed,  ///< another error, logged
    };

    const char* WaitName(Wait wait);

    class Submitter
    {
    public:
        Submitter() = default;
        Submitter(const Submitter&)            = delete;
        Submitter& operator=(const Submitter&) = delete;
        ~Submitter() { Destroy(); }

        bool Create(device::Context& context, char* why, uint32_t why_size);
        void Destroy();

        /// A command buffer to record into, reset; one at a time.
        VkCommandBuffer Begin();

        /// Ends and submits what Begin handed out; its serial, or 0 when it was not submitted.
        uint64_t Submit();

        /// Waits for @p serial at most @p timeout_ns.
        Wait WaitFor(uint64_t serial, uint64_t timeout_ns = kDefaultWaitNs);

        /// The last serial the GPU has completed.
        uint64_t Completed() const;

        /// The last serial handed out.
        uint64_t Last() const { return serial_; }

    private:
        device::Context* context_ = nullptr;
        VkCommandPool    pool_ = VK_NULL_HANDLE;
        VkCommandBuffer  buffer_ = VK_NULL_HANDLE;
        VkSemaphore      timeline_ = VK_NULL_HANDLE;
        uint64_t         serial_ = 0;
        bool             recording_ = false;
    };
}
