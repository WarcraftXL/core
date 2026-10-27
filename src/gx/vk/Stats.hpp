// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What the renderer measures of itself, for the end-of-session lines and for a profiler:
//
//   Charges       the main thread's time in the renderer's slow work, by cause (pipelines compiled,
//                 programs translated, uploads copied, memory made, the GPU waited for, a readback,
//                 the present), and the bytes uploaded, since the host last took them: what a long
//                 frame is put down to
//   Distribution  values (milliseconds) in buckets 2.5 % apart: percentiles, mean and maximum in
//                 a few kilobytes, however long the session
//   Zones         the profiler's CPU zones on the renderer's hot paths: a static location per
//                 place, begun and ended through hooks the host sets while a viewer is connected
//                 (Tracy in WarcraftXL.dll); without hooks a zone costs one load and a branch
//
// Charges are main-thread state (the renderer's frame thread); the zones may be used on any thread.

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace wxl::vk::stats
{
    /// A monotonic clock, in nanoseconds.
    uint64_t Now();

    enum class Cause : uint8_t
    {
        Pipelines, ///< a pipeline or a part compiled or linked on the main thread, or waited for
        Shaders,   ///< a client's record translated on the main thread
        Uploads,   ///< bytes copied towards the device on the main thread (pools, staging)
        Memory,    ///< images, buffers and ring chunks made (new device memory)
        GpuWait,   ///< a frame slot waited for: the GPU had not finished the frame before
        Readback,  ///< the frame submitted and waited for in its middle (screenshots, portraits)
        Present,   ///< the swapchain's image acquired and presented
        Count,
    };

    const char* CauseName(Cause cause);

    struct Charges
    {
        uint64_t ns[static_cast<int>(Cause::Count)] = {};
        uint64_t upload_bytes = 0;
        uint64_t draw_ns = 0; ///< the main thread in the engine's draws (reading and recording), on timed frames
        uint64_t draws = 0;   ///< the draws timed
    };

    /// Adds @p ns to @p cause.
    void Charge(Cause cause, uint64_t ns);
    void Uploaded(uint64_t bytes);
    void DrawTimed(uint64_t ns);
    /// What was charged since the last Take, and starts again.
    Charges Take();

    /// Charges the time from its making to its end to a cause; inside another scope of the main
    /// thread, the outer one takes it all (a ring's chunk made for an upload is the upload's).
    class Scope
    {
    public:
        explicit Scope(Cause cause);
        ~Scope();
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Cause    cause_;
        uint64_t begin_ = 0;
        bool     outer_ = false;
    };

    /// Values in buckets 2.5 % apart from 1 microsecond (milliseconds in, milliseconds out).
    class Distribution
    {
    public:
        void     Add(double ms);
        /// The value below which @p fraction (0..1) of the values lie, to the bucket's middle.
        double   Percentile(double fraction) const;
        double   Mean() const { return count_ ? total_ / static_cast<double>(count_) : 0.0; }
        double   Max() const { return max_; }
        double   Total() const { return total_; }
        uint64_t Count() const { return count_; }
        void     Clear();

    private:
        static constexpr uint32_t kBuckets = 900; ///< up to 1.025^900 us, beyond any frame
        std::vector<uint32_t> buckets_;
        uint64_t              count_ = 0;
        double                total_ = 0.0;
        double                max_ = 0.0;
    };

    // --- The profiler's zones ---------------------------------------------------------------------

    /// A place a zone is named after: the layout of Tracy's ___tracy_source_location_data.
    struct Location
    {
        const char* name;
        const char* function;
        const char* file;
        uint32_t    line;
        uint32_t    color;
    };

    struct ZoneHooks
    {
        uint64_t (*begin)(const Location* location); ///< an opaque zone, 0 for none
        void (*end)(uint64_t zone);
    };

    /// The hooks zones go through from now on; null turns them off. The host keeps them alive.
    void SetZoneHooks(const ZoneHooks* hooks);
    extern std::atomic<const ZoneHooks*> g_zone_hooks;

    class Zone
    {
    public:
        explicit Zone(const Location* location)
        {
            const ZoneHooks* hooks = g_zone_hooks.load(std::memory_order_relaxed);
            if (hooks)
            {
                hooks_ = hooks;
                zone_  = hooks->begin(location);
            }
        }
        ~Zone()
        {
            if (hooks_) hooks_->end(zone_);
        }
        Zone(const Zone&)            = delete;
        Zone& operator=(const Zone&) = delete;

    private:
        const ZoneHooks* hooks_ = nullptr;
        uint64_t         zone_ = 0;
    };
}

#define WXL_VK_ZONE_CAT2(a, b) a##b
#define WXL_VK_ZONE_CAT(a, b) WXL_VK_ZONE_CAT2(a, b)
/// A profiler zone named @p name from here to the end of the scope.
#define WXL_VK_ZONE(name)                                                                                                              \
    static const ::wxl::vk::stats::Location WXL_VK_ZONE_CAT(wxl_vk_location_, __LINE__){name, __func__, __FILE__, __LINE__, 0x2E86C1}; \
    ::wxl::vk::stats::Zone WXL_VK_ZONE_CAT(wxl_vk_zone_, __LINE__)(&WXL_VK_ZONE_CAT(wxl_vk_location_, __LINE__))
