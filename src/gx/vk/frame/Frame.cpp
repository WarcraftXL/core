// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Submissions as serials of a timeline semaphore, and bounded waits on them.

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

    bool Submitter::Create(device::Context& context, char* why, uint32_t why_size)
    {
        Destroy();
        context_          = &context;
        const VkDevice dev = context.Device();

        VkCommandPoolCreateInfo pool{};
        pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = context.QueueFamily();
        VkResult result       = vkCreateCommandPool(dev, &pool, nullptr, &pool_);

        if (result == VK_SUCCESS)
        {
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool        = pool_;
            allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            result                      = vkAllocateCommandBuffers(dev, &allocate, &buffer_);
        }
        if (result == VK_SUCCESS)
        {
            VkSemaphoreTypeCreateInfo type{};
            type.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            type.initialValue  = 0;
            VkSemaphoreCreateInfo semaphore{};
            semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            semaphore.pNext = &type;
            result          = vkCreateSemaphore(dev, &semaphore, nullptr, &timeline_);
        }
        if (result != VK_SUCCESS)
        {
            std::snprintf(why, why_size, "the command pool or the timeline semaphore could not be made: %s",
                          ResultName(result));
            Destroy();
            return false;
        }
        return true;
    }

    void Submitter::Destroy()
    {
        if (context_ && context_->Ready())
        {
            const VkDevice dev = context_->Device();
            // What was submitted completes before its pool goes; a lost device answers at once.
            if (serial_ && !context_->Lost()) WaitFor(serial_);
            if (timeline_) vkDestroySemaphore(dev, timeline_, nullptr);
            if (pool_) vkDestroyCommandPool(dev, pool_, nullptr);
        }
        pool_      = VK_NULL_HANDLE;
        buffer_    = VK_NULL_HANDLE;
        timeline_  = VK_NULL_HANDLE;
        serial_    = 0;
        recording_ = false;
        context_   = nullptr;
    }

    VkCommandBuffer Submitter::Begin()
    {
        if (!buffer_ || recording_ || context_->Lost()) return VK_NULL_HANDLE;
        // One buffer: the previous submission must be done before it is recorded again.
        if (serial_ && WaitFor(serial_) != Wait::Done) return VK_NULL_HANDLE;
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
        if (!context_->Check(vkEndCommandBuffer(buffer_), "vkEndCommandBuffer")) return 0;

        const uint64_t            serial = serial_ + 1;
        VkCommandBufferSubmitInfo command{};
        command.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command.commandBuffer = buffer_;
        VkSemaphoreSubmitInfo signal{};
        signal.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = timeline_;
        signal.value     = serial;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submit{};
        submit.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount   = 1;
        submit.pCommandBufferInfos      = &command;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos    = &signal;

        const VkResult result = vkQueueSubmit2(context_->Queue(), 1, &submit, VK_NULL_HANDLE);
        if (!context_->Check(result, "vkQueueSubmit2"))
        {
            Log(LogLevel::Warn, "vkQueueSubmit2 failed: %s", ResultName(result));
            return 0;
        }
        serial_ = serial;
        return serial;
    }

    Wait Submitter::WaitFor(uint64_t serial, uint64_t timeout_ns)
    {
        if (!timeline_) return Wait::Failed;
        if (context_->Lost()) return Wait::Lost;
        VkSemaphoreWaitInfo info{};
        info.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        info.semaphoreCount = 1;
        info.pSemaphores    = &timeline_;
        info.pValues        = &serial;
        const VkResult result = vkWaitSemaphores(context_->Device(), &info, timeout_ns);
        if (result == VK_SUCCESS) return Wait::Done;
        if (result == VK_TIMEOUT) return Wait::Timeout;
        if (!context_->Check(result, "vkWaitSemaphores")) return context_->Lost() ? Wait::Lost : Wait::Failed;
        return Wait::Failed;
    }

    uint64_t Submitter::Completed() const
    {
        uint64_t value = 0;
        if (timeline_) vkGetSemaphoreCounterValue(context_->Device(), timeline_, &value);
        return value;
    }
}
