// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// How the frames came (Pacing.hpp).

#include "gx/Pacing.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "gx/Vulkan.hpp"
#include "gx/vk/Stats.hpp"
#include "gx/vk/gxfront/Front.hpp"
#include "runtime/Code.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_M_IX86) || defined(__i386__)
#define WXL_CVAR_CALLBACK __cdecl
#else
#define WXL_CVAR_CALLBACK
#endif

namespace
{
    constexpr char kTag[] = "gx";
    namespace stats       = wxl::vk::stats;

    constexpr int      kCauses       = static_cast<int>(stats::Cause::Count);
    constexpr int      kOutside      = kCauses; ///< the client's own work: the renderer took less than half of the excess
    constexpr uint32_t kRecent       = 120;     ///< the frames the median is taken over
    constexpr uint32_t kWarmFrames   = 30;      ///< frames before a hitch is judged
    constexpr double   kPauseMs      = 1000.0;  ///< longer: a pause (alt-tab, a breakpoint), not a frame
    constexpr double   kHitchFactor  = 2.0;
    constexpr double   kHitchOverMs  = 8.0;
    constexpr uint32_t kHitchLines   = 200;     ///< hitches logged one by one, at most

    /// What a stretch of frames was.
    struct Span
    {
        stats::Distribution frames;     ///< milliseconds between two painted frames
        stats::Distribution upload_kib; ///< KiB uploaded per frame (Vulkan)
        uint64_t            upload_bytes = 0;
        uint64_t            upload_most = 0;
        uint64_t            hitches = 0;
        uint64_t            by_cause[kCauses + 1] = {};
        uint64_t            over[3] = {}; ///< 25, 50, 100 ms or more
        uint64_t            pauses = 0;
        double              seconds = 0.0;
        bool                vulkan = false; ///< a frame of it was drawn by the Vulkan renderer
    };

    Span     g_session, g_segment;
    uint64_t g_last = 0; ///< stats::Now() at the last painted frame; 0 after a frame not painted
    float    g_recent[kRecent] = {};
    uint32_t g_recent_count = 0, g_recent_at = 0;
    uint32_t g_hitch_lines = 0;
    void*    g_variable = nullptr;
    bool     g_marking = false;

    double Median()
    {
        float sorted[kRecent];
        std::copy(g_recent, g_recent + g_recent_count, sorted);
        std::nth_element(sorted, sorted + g_recent_count / 2, sorted + g_recent_count);
        return sorted[g_recent_count / 2];
    }

    const char* CauseName(int cause)
    {
        return cause == kOutside ? "the client's own work (not the renderer)" : stats::CauseName(static_cast<stats::Cause>(cause));
    }

    void Add(Span& span, double ms, const stats::Charges& charges, int hitch, bool vulkan)
    {
        span.frames.Add(ms);
        span.seconds += ms / 1000.0;
        span.vulkan = span.vulkan || vulkan;
        if (vulkan)
        {
            span.upload_kib.Add(static_cast<double>(charges.upload_bytes) / 1024.0);
            span.upload_bytes += charges.upload_bytes;
            span.upload_most = std::max(span.upload_most, charges.upload_bytes);
        }
        if (hitch < 0) return;
        ++span.hitches;
        ++span.by_cause[hitch];
        if (ms >= 25.0) ++span.over[0];
        if (ms >= 50.0) ++span.over[1];
        if (ms >= 100.0) ++span.over[2];
    }

    /// The lines of @p span, under @p when; @p segment: the Vulkan line's renderer numbers since the last mark.
    void Lines(const Span& span, const char* when, bool segment)
    {
        const stats::Distribution& f = span.frames;
        if (!f.Count())
        {
            WLOG_INFO(kTag, "frames %s: none painted", when);
            return;
        }
        const double p50 = f.Percentile(0.50);
        char         causes[512] = {};
        if (span.vulkan && span.hitches)
        {
            size_t used = 0;
            for (int c = 0; c <= kCauses; ++c)
            {
                if (!span.by_cause[c]) continue;
                const int wrote = std::snprintf(causes + used, sizeof causes - used, "%s%s %llu", used ? ", " : "; by cause: ", CauseName(c),
                                                static_cast<unsigned long long>(span.by_cause[c]));
                if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof causes - used) break;
                used += static_cast<size_t>(wrote);
            }
        }
        WLOG_INFO(kTag,
                  "frames %s (%s): %llu frames in %.0f s, frame time p50 %.2f ms (%.0f fps), p95 %.2f ms, p99 %.2f ms, max %.1f ms; %llu "
                  "hitches (a frame at least twice the median and 8 ms over it; %llu of 25 ms or more, %llu of 50, %llu of 100)%s; %llu "
                  "pause(s) of a second or more left out",
                  when, span.vulkan ? "Vulkan" : "Direct3D 9", static_cast<unsigned long long>(f.Count()), span.seconds, p50,
                  p50 > 0.0 ? 1000.0 / p50 : 0.0, f.Percentile(0.95), f.Percentile(0.99), f.Max(), static_cast<unsigned long long>(span.hitches),
                  static_cast<unsigned long long>(span.over[0]), static_cast<unsigned long long>(span.over[1]),
                  static_cast<unsigned long long>(span.over[2]), causes, static_cast<unsigned long long>(span.pauses));
        if (!span.vulkan) return;
        char renderer[2048] = {};
        if (wxl::vk::gxfront::Front* front = wxl::core::gx::VulkanFront()) front->DescribeFrames(renderer, sizeof renderer, segment);
        const stats::Distribution& u = span.upload_kib;
        WLOG_INFO(kTag, "frames %s (Vulkan): uploads %.1f MiB, %.0f KiB a frame on average, p99 %.0f KiB, the most in one frame %.1f MiB%s%s",
                  when, static_cast<double>(span.upload_bytes) / 1048576.0, u.Mean(), u.Percentile(0.99),
                  static_cast<double>(span.upload_most) / 1048576.0, renderer[0] ? "; " : "", renderer);
    }

    bool WXL_CVAR_CALLBACK Variable(void*, const char*, const char* value, void*)
    {
        if (!g_marking) wxl::core::gx::pacing::Mark(value && value[0] ? value : "(no label)");
        return true;
    }
}

namespace wxl::core::gx::pacing
{
    void Present(bool painted)
    {
        const uint64_t       now     = stats::Now();
        const stats::Charges charges = stats::Take();
        if (!painted)
        {
            // A loading screen's present: the next painted frame starts the clock again.
            g_last = 0;
            return;
        }
        const uint64_t last = g_last;
        g_last              = now;
        if (!last) return;
        const double ms = static_cast<double>(now - last) / 1e6;
        if (ms >= kPauseMs)
        {
            ++g_session.pauses;
            ++g_segment.pauses;
            return;
        }
        const bool vulkan = VulkanActive();
        int        hitch  = -1;
        if (g_recent_count >= kWarmFrames)
        {
            const double median = Median();
            if (ms >= kHitchFactor * median && ms >= median + kHitchOverMs)
            {
                // The cause: what the renderer spent most of the frame on, when that is at least
                // half of what the frame took over the median.
                const double excess = ms - median;
                int          top    = kOutside;
                double       most   = 0.0;
                for (int c = 0; c < kCauses; ++c)
                {
                    const double spent = static_cast<double>(charges.ns[c]) / 1e6;
                    if (spent > most)
                    {
                        most = spent;
                        top  = c;
                    }
                }
                hitch = vulkan && most >= excess / 2.0 ? top : kOutside;
                if (g_hitch_lines < kHitchLines)
                {
                    ++g_hitch_lines;
                    if (vulkan)
                        WLOG_DEBUG(kTag,
                                   "hitch: a frame of %.1f ms (the median %.1f ms): %s; the renderer's main thread: pipelines %.1f ms, "
                                   "shaders %.1f ms, uploads %.1f ms (%llu KiB), memory %.1f ms, waiting for the GPU %.1f ms, readbacks %.1f "
                                   "ms, present %.1f ms",
                                   ms, median, CauseName(hitch), charges.ns[0] / 1e6, charges.ns[1] / 1e6, charges.ns[2] / 1e6,
                                   static_cast<unsigned long long>(charges.upload_bytes / 1024), charges.ns[3] / 1e6, charges.ns[4] / 1e6,
                                   charges.ns[5] / 1e6, charges.ns[6] / 1e6);
                    else
                        WLOG_DEBUG(kTag, "hitch: a frame of %.1f ms (the median %.1f ms)", ms, median);
                }
            }
        }
        g_recent[g_recent_at] = static_cast<float>(ms);
        g_recent_at           = (g_recent_at + 1) % kRecent;
        g_recent_count        = std::min(g_recent_count + 1, kRecent);
        Add(g_session, ms, charges, hitch, vulkan);
        Add(g_segment, ms, charges, hitch, vulkan);
    }

    void Mark(const char* label)
    {
        g_marking = true;
        const std::string when = std::string("at gxStats '") + (label ? label : "") + "'";
        Lines(g_segment, when.c_str(), true);
        g_segment = Span{};
        if (wxl::vk::gxfront::Front* front = VulkanFront()) front->NewSegment();
        g_marking = false;
    }

    void Report(const char* when) { Lines(g_session, when, false); }

    void RegisterVariable()
    {
        if (g_variable) return;
        if (!code::Handable(reinterpret_cast<const void*>(&Variable), "gxStats' callback")) return;
        // The client sets the default through the callback: no segment for it.
        g_marking  = true;
        g_variable = binding::cvar_register("gxStats", "Writes the frame times since the last gxStats to WarcraftXL.log, under the label given",
                                            0, "", reinterpret_cast<void*>(&Variable), 1 /* CATEGORY_GRAPHICS */);
        g_marking  = false;
        if (!g_variable)
        {
            WLOG_DEBUG(kTag, "the console variable gxStats could not be made: the frame lines come at exit only");
            return;
        }
        // Never saved to Config.wtf (the interface's own variables' bit), never archived.
        binding::cvar_write_flags(g_variable, (binding::cvar_flags(g_variable) & ~0x1u) | 0x80u);
    }
}
