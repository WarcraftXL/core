// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The renderer's charges by cause, its distributions, its profiler zones' hooks.

#include "gx/vk/Stats.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace wxl::vk::stats
{
    namespace
    {
        Charges g_charges;
        constexpr double kStep = 1.025;
    }

    std::atomic<const ZoneHooks*> g_zone_hooks{nullptr};

    uint64_t Now()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    const char* CauseName(Cause cause)
    {
        switch (cause)
        {
        case Cause::Pipelines: return "pipelines compiled";
        case Cause::Shaders: return "shaders translated";
        case Cause::Uploads: return "uploads";
        case Cause::Memory: return "memory made";
        case Cause::GpuWait: return "waiting for the GPU";
        case Cause::Readback: return "readbacks";
        case Cause::Present: return "present";
        default: return "?";
        }
    }

    void Charge(Cause cause, uint64_t ns) { g_charges.ns[static_cast<int>(cause)] += ns; }

    namespace
    {
        thread_local uint32_t t_depth = 0;
    }

    Scope::Scope(Cause cause) : cause_(cause)
    {
        outer_ = t_depth++ == 0;
        if (outer_) begin_ = Now();
    }

    Scope::~Scope()
    {
        --t_depth;
        if (outer_) Charge(cause_, Now() - begin_);
    }
    void Uploaded(uint64_t bytes) { g_charges.upload_bytes += bytes; }

    void DrawTimed(uint64_t ns)
    {
        g_charges.draw_ns += ns;
        ++g_charges.draws;
    }

    Charges Take()
    {
        const Charges taken = g_charges;
        g_charges           = Charges{};
        return taken;
    }

    void Distribution::Add(double ms)
    {
        if (!(ms >= 0.0)) return;
        if (buckets_.empty()) buckets_.assign(kBuckets, 0);
        const double us    = ms * 1000.0;
        uint32_t     index = 0;
        if (us >= 1.0) index = std::min<uint32_t>(kBuckets - 1, 1 + static_cast<uint32_t>(std::log(us) / std::log(kStep)));
        ++buckets_[index];
        ++count_;
        total_ += ms;
        max_ = std::max(max_, ms);
    }

    double Distribution::Percentile(double fraction) const
    {
        if (!count_) return 0.0;
        const uint64_t rank = std::min<uint64_t>(count_ - 1, static_cast<uint64_t>(fraction * static_cast<double>(count_)));
        uint64_t       seen = 0;
        for (uint32_t i = 0; i < kBuckets; ++i)
        {
            seen += buckets_[i];
            if (seen > rank)
            {
                if (i == 0) return 0.0005;
                // The bucket's middle, never past the largest value seen.
                return std::min(max_, std::pow(kStep, static_cast<double>(i - 1) + 0.5) / 1000.0);
            }
        }
        return max_;
    }

    void Distribution::Clear()
    {
        std::fill(buckets_.begin(), buckets_.end(), 0u);
        count_ = 0;
        total_ = 0.0;
        max_   = 0.0;
    }

    void SetZoneHooks(const ZoneHooks* hooks) { g_zone_hooks.store(hooks, std::memory_order_relaxed); }
}
