// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The frame limiter: frames shown no closer together than an interval. It emulates the D3D9
// intervals two to four (gxVSync 2-4) that Vulkan has no present mode for, over FIFO: one frame
// every N refreshes. Pure: it is given the time and answers how long to wait, so it is tested
// without a clock; the wait itself is never longer than kMaxDelayNs.

#pragma once

#include <cstdint>

namespace wxl::vk::present
{
    class Limiter
    {
    public:
        /// No wait is ever longer than this, whatever the interval: a clock that jumps cannot stall a frame.
        static constexpr uint64_t kMaxDelayNs = 100'000'000ull;

        /// Frames at least @p interval_ns apart; 0 turns the limiter off.
        void SetInterval(uint64_t interval_ns)
        {
            interval_ = interval_ns;
            started_  = false;
        }
        uint64_t Interval() const { return interval_; }

        /// How long to wait at @p now before showing the next frame.
        uint64_t Delay(uint64_t now) const
        {
            if (!interval_ || !started_ || deadline_ <= now) return 0;
            const uint64_t delay = deadline_ - now;
            return delay < kMaxDelayNs ? delay : kMaxDelayNs;
        }

        /// A frame was shown at @p now: the next may come one interval after the last deadline, or
        /// one interval from now when the frames fell behind by more than an interval.
        void Shown(uint64_t now)
        {
            if (!interval_) return;
            if (!started_ || deadline_ + interval_ < now) deadline_ = now + interval_;
            else deadline_ += interval_;
            started_ = true;
        }

    private:
        uint64_t interval_ = 0;
        uint64_t deadline_ = 0;
        bool     started_ = false;
    };
}
