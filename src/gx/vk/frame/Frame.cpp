// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Serials of a timeline semaphore, bounded waits on them, and the frames in flight.

#include "gx/vk/frame/Frame.hpp"

#include "gx/vk/Log.hpp"

#include <cstdio>

namespace wxl::vk::frame
{
    const char* WaitName(Wait wait)
    {
        switch (wait)
        {
        case Wait::Done: return "done";
        case Wait::Timeout: return "timed out";
        case Wait::Lost: return "device lost";
        default: return "failed";
        }
    }

    VkSemaphore MakeSemaphore(VkDevice device)
    {
        VkSemaphoreCreateInfo info{};
        info.sType            = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        const VkResult result = vkCreateSemaphore(device, &info, nullptr, &semaphore);
        if (result != VK_SUCCESS)
        {
            Log(LogLevel::Warn, "a semaphore could not be made: %s", ResultName(result));
            return VK_NULL_HANDLE;
        }
        return semaphore;
    }

    // --- Timeline ----------------------------------------------------------------------------------

    bool Timeline::Create(device::Context& context, char* why, uint32_t why_size)
    {
        Destroy();
        context_ = &context;
        VkSemaphoreTypeCreateInfo type{};
        type.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue  = 0;
        VkSemaphoreCreateInfo info{};
        info.sType            = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext            = &type;
        const VkResult result = vkCreateSemaphore(context.Device(), &info, nullptr, &semaphore_);
        if (result != VK_SUCCESS)
        {
            semaphore_ = VK_NULL_HANDLE;
            std::snprintf(why, why_size, "the timeline semaphore could not be made: %s", ResultName(result));
            return false;
        }
        return true;
    }

    void Timeline::Destroy()
    {
        if (semaphore_ && context_ && context_->Ready()) vkDestroySemaphore(context_->Device(), semaphore_, nullptr);
        semaphore_ = VK_NULL_HANDLE;
        serial_    = 0;
    }

    uint64_t Timeline::Submit(const VkCommandBuffer* buffers, uint32_t count, const WaitOn* waits, uint32_t wait_count,
                              const VkSemaphore* signals, uint32_t signal_count)
    {
        if (!semaphore_ || context_->Lost() || count > 4 || wait_count > 4 || signal_count > 4) return 0;

        VkCommandBufferSubmitInfo commands[4]{};
        for (uint32_t i = 0; i < count; ++i)
        {
            commands[i].sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            commands[i].commandBuffer = buffers[i];
        }
        VkSemaphoreSubmitInfo waitInfo[4]{};
        for (uint32_t i = 0; i < wait_count; ++i)
        {
            waitInfo[i].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waitInfo[i].semaphore = waits[i].semaphore;
            waitInfo[i].stageMask = waits[i].stage;
        }
        const uint64_t        serial = serial_ + 1;
        VkSemaphoreSubmitInfo signalInfo[5]{};
        signalInfo[0].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalInfo[0].semaphore = semaphore_;
        signalInfo[0].value     = serial;
        signalInfo[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        for (uint32_t i = 0; i < signal_count; ++i)
        {
            signalInfo[1 + i].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            signalInfo[1 + i].semaphore = signals[i];
            signalInfo[1 + i].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }

        VkSubmitInfo2 submit{};
        submit.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount   = wait_count;
        submit.pWaitSemaphoreInfos      = wait_count ? waitInfo : nullptr;
        submit.commandBufferInfoCount   = count;
        submit.pCommandBufferInfos      = count ? commands : nullptr;
        submit.signalSemaphoreInfoCount = 1 + signal_count;
        submit.pSignalSemaphoreInfos    = signalInfo;

        const VkResult result = vkQueueSubmit2(context_->Queue(), 1, &submit, VK_NULL_HANDLE);
        if (!context_->Check(result, "vkQueueSubmit2"))
        {
            Log(LogLevel::Warn, "vkQueueSubmit2 failed: %s", ResultName(result));
            return 0;
        }
        serial_ = serial;
        return serial;
    }

    Wait Timeline::WaitFor(uint64_t serial, uint64_t timeout_ns)
    {
        if (!semaphore_) return Wait::Failed;
        if (serial == 0) return Wait::Done;
        // Waited for even on a device marked lost: what it was given may still be running (a loss
        // the renderer declared itself, a hung GPU), and must be done before what it reads goes.
        // A device really lost answers at once.
        VkSemaphoreWaitInfo info{};
        info.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        info.semaphoreCount = 1;
        info.pSemaphores    = &semaphore_;
        info.pValues        = &serial;
        const VkResult result = vkWaitSemaphores(context_->Device(), &info, timeout_ns);
        if (result == VK_SUCCESS) return Wait::Done;
        if (result == VK_TIMEOUT) return context_->Lost() ? Wait::Lost : Wait::Timeout;
        context_->Check(result, "vkWaitSemaphores");
        return context_->Lost() ? Wait::Lost : Wait::Failed;
    }

    uint64_t Timeline::Completed() const
    {
        uint64_t value = 0;
        if (semaphore_ && !context_->Lost()) vkGetSemaphoreCounterValue(context_->Device(), semaphore_, &value);
        return value;
    }

    // --- Submitter -----------------------------------------------------------------------------------

    bool Submitter::Create(Timeline& timeline, char* why, uint32_t why_size)
    {
        Destroy();
        timeline_          = &timeline;
        device::Context& c = timeline.Context();

        VkCommandPoolCreateInfo pool{};
        pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = c.QueueFamily();
        VkResult result       = vkCreateCommandPool(c.Device(), &pool, nullptr, &pool_);
        if (result == VK_SUCCESS)
        {
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool        = pool_;
            allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            result                      = vkAllocateCommandBuffers(c.Device(), &allocate, &buffer_);
        }
        if (result != VK_SUCCESS)
        {
            std::snprintf(why, why_size, "a command pool could not be made: %s", ResultName(result));
            Destroy();
            return false;
        }
        return true;
    }

    void Submitter::Destroy()
    {
        if (timeline_ && pool_ && timeline_->Context().Ready())
        {
            // What was submitted completes before its pool goes; a lost device answers at once.
            if (used_) timeline_->WaitFor(used_);
            vkDestroyCommandPool(timeline_->Context().Device(), pool_, nullptr);
        }
        pool_      = VK_NULL_HANDLE;
        buffer_    = VK_NULL_HANDLE;
        used_      = 0;
        recording_ = false;
        timeline_  = nullptr;
    }

    VkCommandBuffer Submitter::Begin()
    {
        if (!buffer_ || recording_ || timeline_->Context().Lost()) return VK_NULL_HANDLE;
        // One buffer: its previous submission must be done before it is recorded again.
        if (used_ && timeline_->WaitFor(used_) != Wait::Done) return VK_NULL_HANDLE;
        vkResetCommandBuffer(buffer_, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(buffer_, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;
        recording_ = true;
        return buffer_;
    }

    uint64_t Submitter::Submit()
    {
        if (!recording_) return 0;
        recording_ = false;
        if (!timeline_->Context().Check(vkEndCommandBuffer(buffer_), "vkEndCommandBuffer")) return 0;
        const uint64_t serial = timeline_->Submit(&buffer_, 1);
        if (serial) used_ = serial;
        return serial;
    }

    // --- Frames ----------------------------------------------------------------------------------------

    bool Frames::Create(Timeline& timeline, uint32_t count, char* why, uint32_t why_size)
    {
        Destroy();
        if (count < 1) count = 1;
        if (count > kMax) count = kMax;
        timeline_          = &timeline;
        device::Context& c = timeline.Context();
        for (uint32_t i = 0; i < count; ++i)
        {
            Slot&                   slot = slots_[i];
            VkCommandPoolCreateInfo pool{};
            pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool.queueFamilyIndex = c.QueueFamily();
            VkResult result       = vkCreateCommandPool(c.Device(), &pool, nullptr, &slot.pool);
            if (result == VK_SUCCESS)
            {
                VkCommandBufferAllocateInfo allocate{};
                allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocate.commandPool        = slot.pool;
                allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocate.commandBufferCount = 2;
                VkCommandBuffer buffers[2]  = {};
                result                      = vkAllocateCommandBuffers(c.Device(), &allocate, buffers);
                slot.upload                 = buffers[0];
                slot.buffer                 = buffers[1];
            }
            if (result == VK_SUCCESS)
            {
                slot.acquired = MakeSemaphore(c.Device());
                if (!slot.acquired) result = VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            if (result != VK_SUCCESS)
            {
                std::snprintf(why, why_size, "frame %u's command pool or semaphore could not be made: %s", i, ResultName(result));
                count_ = i + 1;
                Destroy();
                return false;
            }
        }
        count_   = count;
        index_   = 0;
        open_    = false;
        started_ = false;
        return true;
    }

    void Frames::Destroy()
    {
        if (timeline_ && timeline_->Context().Ready())
        {
            const VkDevice device = timeline_->Context().Device();
            for (uint32_t i = 0; i < count_; ++i)
            {
                Slot& slot = slots_[i];
                if (slot.acquired) vkDestroySemaphore(device, slot.acquired, nullptr);
                if (slot.pool) vkDestroyCommandPool(device, slot.pool, nullptr);
            }
        }
        for (Slot& slot : slots_) slot = Slot{};
        count_   = 0;
        index_   = 0;
        open_    = false;
        started_ = false;
    }

    VkCommandBuffer Frames::Begin(Wait& wait, uint64_t timeout_ns)
    {
        wait = Wait::Failed;
        if (!count_ || open_) return VK_NULL_HANDLE;
        device::Context& c = timeline_->Context();
        if (c.Lost())
        {
            wait = Wait::Lost;
            return VK_NULL_HANDLE;
        }
        const uint32_t next = started_ ? (index_ + 1) % count_ : 0;
        Slot&          slot = slots_[next];
        wait                = timeline_->WaitFor(slot.serial, timeout_ns);
        if (wait != Wait::Done) return VK_NULL_HANDLE;

        vkResetCommandPool(c.Device(), slot.pool, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!c.Check(vkBeginCommandBuffer(slot.upload, &begin), "vkBeginCommandBuffer") ||
            !c.Check(vkBeginCommandBuffer(slot.buffer, &begin), "vkBeginCommandBuffer"))
        {
            wait = c.Lost() ? Wait::Lost : Wait::Failed;
            return VK_NULL_HANDLE;
        }
        index_   = next;
        started_ = true;
        open_    = true;
        return slot.buffer;
    }

    VkSemaphore Frames::RenewAcquire()
    {
        Slot&             slot  = slots_[index_];
        const VkSemaphore fresh = MakeSemaphore(timeline_->Context().Device());
        if (!fresh) return VK_NULL_HANDLE;
        const VkSemaphore spent = slot.acquired;
        slot.acquired           = fresh;
        return spent;
    }

    uint64_t Frames::Submit(const Timeline::WaitOn* waits, uint32_t wait_count, const VkSemaphore* signals, uint32_t signal_count)
    {
        if (!open_) return 0;
        open_                 = false;
        Slot&            slot = slots_[index_];
        device::Context& c    = timeline_->Context();
        if (!c.Check(vkEndCommandBuffer(slot.upload), "vkEndCommandBuffer") || !c.Check(vkEndCommandBuffer(slot.buffer), "vkEndCommandBuffer"))
            return 0;
        const VkCommandBuffer buffers[2] = {slot.upload, slot.buffer};
        const uint64_t        serial     = timeline_->Submit(buffers, 2, waits, wait_count, signals, signal_count);
        if (serial) slot.serial = serial;
        return serial;
    }

    void Frames::Abandon()
    {
        if (!open_) return;
        open_ = false;
        vkEndCommandBuffer(slots_[index_].upload);
        vkEndCommandBuffer(slots_[index_].buffer);
    }
}
