// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// How work reaches the GPU: command buffers recorded on the main thread, submitted to the one
// graphics queue with synchronization2, each submission a value of one timeline semaphore (its
// serial). Waits always end: a wait that runs out says so and is the caller's to treat as a hung
// device, never an infinite stall.
//
//   Timeline   the queue's serials: submit, wait for a serial within a bound, what has completed
//   Submitter  one command buffer for work outside the frames (a check at start, a test)
//   Frames     the frames in flight: each slot a command pool, its two command buffers (the upload
//              pass, submitted first, and the frame's main one) and the semaphore its swapchain image
//              is acquired with; a slot is reused once its last serial completed

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

    class Timeline
    {
    public:
        Timeline() = default;
        Timeline(const Timeline&)            = delete;
        Timeline& operator=(const Timeline&) = delete;
        ~Timeline() { Destroy(); }

        bool Create(device::Context& context, char* why, uint32_t why_size);
        /// Releases the semaphore. Whoever submitted waits for its own work first.
        void Destroy();
        bool Ready() const { return semaphore_ != VK_NULL_HANDLE; }

        /// A binary semaphore the submission waits on, and the stage that waits.
        struct WaitOn
        {
            VkSemaphore           semaphore;
            VkPipelineStageFlags2 stage;
        };

        /**
         * @brief Submits @p count command buffers (at most 4), waiting on @p waits and signalling
         *        @p signals (binary semaphores, at most 4 each) besides the next serial.
         * @return the serial, or 0 when the queue refused it (a lost device is noted).
         */
        uint64_t Submit(const VkCommandBuffer* buffers, uint32_t count, const WaitOn* waits = nullptr,
                        uint32_t wait_count = 0, const VkSemaphore* signals = nullptr, uint32_t signal_count = 0);

        /// Waits for @p serial at most @p timeout_ns, a device marked lost included (its work may
        /// still run); Lost when the device is lost and the serial did not complete.
        Wait WaitFor(uint64_t serial, uint64_t timeout_ns = kDefaultWaitNs);

        /// The last serial the GPU has completed.
        uint64_t Completed() const;

        /// The last serial handed out.
        uint64_t Last() const { return serial_; }

        device::Context& Context() const { return *context_; }

    private:
        device::Context* context_ = nullptr;
        VkSemaphore      semaphore_ = VK_NULL_HANDLE;
        uint64_t         serial_ = 0;
    };

    /// One command buffer on a timeline, for work outside the frames.
    class Submitter
    {
    public:
        Submitter() = default;
        Submitter(const Submitter&)            = delete;
        Submitter& operator=(const Submitter&) = delete;
        ~Submitter() { Destroy(); }

        bool Create(Timeline& timeline, char* why, uint32_t why_size);
        /// Waits (bounded) for its last submission, then releases its pool.
        void Destroy();

        /// A command buffer to record into, reset; one at a time. Waits (bounded) for its last use.
        VkCommandBuffer Begin();

        /// Ends and submits what Begin handed out; its serial, or 0 when it was not submitted.
        uint64_t Submit();

        Wait     WaitFor(uint64_t serial, uint64_t timeout_ns = kDefaultWaitNs) { return timeline_->WaitFor(serial, timeout_ns); }
        uint64_t Completed() const { return timeline_ ? timeline_->Completed() : 0; }
        uint64_t Last() const { return timeline_ ? timeline_->Last() : 0; }

    private:
        Timeline*       timeline_ = nullptr;
        VkCommandPool   pool_ = VK_NULL_HANDLE;
        VkCommandBuffer buffer_ = VK_NULL_HANDLE;
        uint64_t        used_ = 0; ///< the serial of its last submission
        bool            recording_ = false;
    };

    /// The frames in flight.
    class Frames
    {
    public:
        static constexpr uint32_t kMax = 3;

        Frames() = default;
        Frames(const Frames&)            = delete;
        Frames& operator=(const Frames&) = delete;
        ~Frames() { Destroy(); }

        /// @p count frames in flight, 1 to kMax.
        bool Create(Timeline& timeline, uint32_t count, char* why, uint32_t why_size);
        /// Releases the slots. Their work must be complete (or the device lost).
        void Destroy();
        bool Ready() const { return count_ != 0; }

        uint32_t Count() const { return count_; }
        uint32_t Index() const { return index_; }

        /**
         * @brief Opens the next slot: waits (bounded) for its last frame, resets its pool and
         *        begins its command buffer.
         * @return the command buffer, or null with @p wait saying why (Timeout: the GPU did not
         *         finish a frame within the bound; Lost; Failed).
         */
        VkCommandBuffer Begin(Wait& wait, uint64_t timeout_ns = kDefaultWaitNs);

        /// Whether a slot is open (between Begin and Submit or Abandon).
        bool Open() const { return open_; }

        /// The open slot's command buffer, null outside Begin..Submit.
        VkCommandBuffer Current() const { return open_ ? slots_[index_].buffer : VK_NULL_HANDLE; }

        /// The open slot's upload command buffer: copies into what the frame's draws have not named
        /// yet, submitted before Current() in the same submission. Null outside Begin..Submit.
        VkCommandBuffer Upload() const { return open_ ? slots_[index_].upload : VK_NULL_HANDLE; }

        /// The semaphore the open slot acquires its swapchain image with.
        VkSemaphore Acquire() const { return slots_[index_].acquired; }

        /**
         * @brief The acquire semaphore was signalled but nothing will wait on it (the frame was not
         *        submitted after a successful acquire): a fresh one replaces it, and the spent one is
         *        handed back for the caller to release once the device is done with it. Null when
         *        no fresh one could be made (the old one stays).
         */
        VkSemaphore RenewAcquire();

        /// Ends and submits the open slot, its upload buffer first; its serial, or 0 when the queue refused it.
        uint64_t Submit(const Timeline::WaitOn* waits = nullptr, uint32_t wait_count = 0,
                        const VkSemaphore* signals = nullptr, uint32_t signal_count = 0);

        /// Ends the open slot without submitting it (the frame is dropped).
        void Abandon();

    private:
        struct Slot
        {
            VkCommandPool   pool = VK_NULL_HANDLE;
            VkCommandBuffer buffer = VK_NULL_HANDLE;
            VkCommandBuffer upload = VK_NULL_HANDLE;
            VkSemaphore     acquired = VK_NULL_HANDLE;
            uint64_t        serial = 0;
        };

        Timeline* timeline_ = nullptr;
        Slot      slots_[kMax];
        uint32_t  count_ = 0;
        uint32_t  index_ = 0;
        bool      open_ = false;
        bool      started_ = false; ///< a slot was opened once: the next Begin moves on
    };

    /// A binary semaphore, or null; logged when it cannot be made.
    VkSemaphore MakeSemaphore(VkDevice device);
}
