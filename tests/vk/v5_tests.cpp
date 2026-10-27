// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// V5's renderer, what it measures of itself and what it does ahead:
//
//   - the distributions of the session's lines (percentiles in buckets 2.5 % apart), the main
//     thread's charges by cause (an inner scope's time the outer one's), the profiler's zones
//     through the host's hooks and nothing without them (pure);
//   - each pass timed on the GPU (the upload pass, the scene, a texture target, the present pass),
//     read once its frame has completed, and nothing timed once turned off;
//   - pools in the device's mapped device-local memory (ReBAR): written in place when no frame
//     reads them, through staging when one does, the image the same either way and with it off;
//   - the programs and pipelines of an earlier launch made at the renderer's start, before the
//     engine binds them: the first draw finds its pipeline, nothing translated, linked or compiled
//     on the main thread; and a draw into a texture finds its program's part for the texture made.

#include "gx/vk/Log.hpp"
#include "gx/vk/Stats.hpp"
#include "gx/vk/draw/Draw.hpp"
#include "gx/vk/gxfront/Engine.hpp"
#include "gx/vk/present/Display.hpp"

#include "Check.hpp"
#include "Rig.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace wxl::vk::test
{
    std::vector<uint8_t> WorldVs();
    std::vector<uint8_t> WorldPs();
}

namespace
{
    using namespace wxl::vk::test::rig;
    using wxl::vk::test::Check;
    namespace pipelines = wxl::vk::pipelines;
    namespace stats     = wxl::vk::stats;

    // --- Pure ------------------------------------------------------------------------------------------

    void DistributionCases()
    {
        stats::Distribution d;
        Check(d.Count() == 0 && d.Percentile(0.5) == 0.0 && d.Max() == 0.0, "an empty distribution answers zeros");
        for (int ms = 1; ms <= 100; ++ms) d.Add(static_cast<double>(ms));
        const double p50 = d.Percentile(0.50), p99 = d.Percentile(0.99);
        Check(d.Count() == 100 && std::fabs(d.Mean() - 50.5) < 1e-9 && d.Max() == 100.0, "count, mean and maximum are exact");
        Check(std::fabs(p50 - 51.0) / 51.0 < 0.03 && std::fabs(p99 - 100.0) / 100.0 < 0.03 && d.Percentile(1.0) <= 100.0,
              "percentiles to the bucket (2.5 %), never past the maximum");
        stats::Distribution small;
        small.Add(0.0004);
        small.Add(0.25);
        Check(small.Percentile(0.0) < 0.001 && std::fabs(small.Percentile(0.99) - 0.25) / 0.25 < 0.03, "below a microsecond and a quarter millisecond");
        d.Clear();
        Check(d.Count() == 0 && d.Max() == 0.0, "cleared");
    }

    void ChargeCases()
    {
        stats::Take();
        {
            stats::Scope outer(stats::Cause::Uploads);
            {
                stats::Scope inner(stats::Cause::Memory);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        stats::Uploaded(1000);
        stats::DrawTimed(500);
        const stats::Charges c = stats::Take();
        Check(c.ns[static_cast<int>(stats::Cause::Uploads)] >= 1'500'000 && c.ns[static_cast<int>(stats::Cause::Memory)] == 0,
              "an inner scope's time is the outer scope's cause");
        Check(c.upload_bytes == 1000 && c.draw_ns == 500 && c.draws == 1, "bytes uploaded and draws timed");
        const stats::Charges again = stats::Take();
        Check(again.upload_bytes == 0 && again.ns[static_cast<int>(stats::Cause::Uploads)] == 0, "taken charges start again");
        Check(std::strcmp(stats::CauseName(stats::Cause::Pipelines), "pipelines compiled") == 0, "causes have names");
    }

    int g_begun = 0, g_ended = 0;
    const stats::Location* g_location = nullptr;
    uint64_t HookBegin(const stats::Location* location)
    {
        ++g_begun;
        g_location = location;
        return 42;
    }
    void HookEnd(uint64_t zone) { g_ended += zone == 42 ? 1 : 100; }

    void ZoneCases()
    {
        static const stats::ZoneHooks hooks{&HookBegin, &HookEnd};
        {
            WXL_VK_ZONE("no hooks");
        }
        Check(g_begun == 0 && g_ended == 0, "without hooks a zone calls nothing");
        stats::SetZoneHooks(&hooks);
        {
            WXL_VK_ZONE("a test zone");
        }
        stats::SetZoneHooks(nullptr);
        {
            WXL_VK_ZONE("after");
        }
        Check(g_begun == 1 && g_ended == 1 && g_location && std::strcmp(g_location->name, "a test zone") == 0 && g_location->line > 0,
              "with hooks a zone begins and ends once, at its static location");
    }

    // --- Live ------------------------------------------------------------------------------------------

    bool QuadOf(Live& live, FakeEngine& fake, draw::Pool* pool, uint32_t color, uint32_t& at)
    {
        Vertex quad[6];
        Quad(quad, 0, 0, static_cast<float>(live.width), static_cast<float>(live.height), color);
        Stream(live, fake, pool, quad, 6, at);
        return Record(live, fake, 6);
    }

    const vk::present::PassTime* PassNamed(const vk::present::Display& display, const char* name)
    {
        for (const vk::present::PassTime& t : display.PassTimes())
            if (std::strcmp(t.name, name) == 0) return &t;
        return nullptr;
    }

    void PassTimingCases()
    {
        char why[256] = {};
        Live live;
        if (!live.Make(16, 8, why, sizeof why))
        {
            Check(false, "a renderer for the pass timing cases");
            return;
        }
        if (!live.display.SetPassTiming(true))
        {
            std::printf("(no timestamps on this queue: the pass timing cases are skipped)\n");
            return;
        }
        FakeEngine fake;
        fake.Reset(live.width, live.height);
        draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
        fake.pools[0]    = pool;
        draw::TextureDesc desc;
        desc.width            = 4;
        desc.height           = 4;
        desc.target           = true;
        draw::Texture* target = live.renderer.CreateTexture(desc);
        uint32_t       at     = 0;
        for (int frame = 0; frame < 4; ++frame)
        {
            if (!live.Begin(Argb(0, 0, 0))) break;
            QuadOf(live, fake, pool, Argb(255, 0, 0), at);
            if (frame == 0 && target)
            {
                fake.device.m_textureTarget[0].m_texture = reinterpret_cast<engine::CGxTex*>(&fake.tex_fields[1]);
                live.renderer.SetTarget(target, 0, nullptr);
                const float blue[4] = {0, 0, 1, 1};
                live.renderer.Clear(true, true, blue, 1.0f, {{0, 0}, {4, 4}});
                fake.device.m_textureTarget[0].m_texture = nullptr;
                live.renderer.SetTarget(nullptr, 0, nullptr);
            }
            const uint32_t own = live.display.PassBegin("a pass of the test's");
            live.display.PassEnd(own);
            live.End();
        }
        const vk::present::PassTime* upload  = PassNamed(live.display, "upload pass");
        const vk::present::PassTime* scene   = PassNamed(live.display, "scene");
        const vk::present::PassTime* present = PassNamed(live.display, "present pass");
        const vk::present::PassTime* texture = PassNamed(live.display, "texture targets");
        const vk::present::PassTime* own     = PassNamed(live.display, "a pass of the test's");
        Check(upload && scene && present && own && live.display.PassFrames() >= 2 && scene->per_frame.Count() == live.display.PassFrames() &&
                  scene->per_frame.Max() >= 0.0,
              "each frame's upload pass, scene, present pass and a pass of the caller's are timed once the frame completed");
        Check(texture && texture->per_frame.Count() == 1, "a texture target's rendering is timed in the frame that had one");
        Check(live.display.PassSegmentFrames() == live.display.PassFrames() && !live.display.PassSegment().empty(), "the segment holds them too");
        live.display.NewPassSegment();
        const uint64_t frames = live.display.PassFrames();
        live.display.SetPassTiming(false);
        for (int frame = 0; frame < 3; ++frame)
            if (live.Begin(Argb(0, 0, 0)))
            {
                QuadOf(live, fake, pool, Argb(255, 0, 0), at);
                live.End();
            }
        Check(live.display.PassFrames() <= frames + 1 && live.display.PassSegment().size() <= 4, "turned off, nothing more is timed");
        live.display.WaitSubmitted();
        if (target) live.renderer.DestroyTexture(target);
        live.renderer.DestroyPool(pool);
    }

    /// A static pool of six vertices of @p color, written as the engine writes one.
    void WriteStatic(Live& live, draw::Pool* pool, uint32_t color)
    {
        Vertex quad[6];
        Quad(quad, 0, 0, static_cast<float>(live.width), static_cast<float>(live.height), color);
        std::memcpy(live.renderer.PoolMemory(pool), quad, sizeof quad);
        live.renderer.PoolWritten(pool, 0, sizeof quad);
    }

    void RebarCases(bool on)
    {
        char why[256] = {};
        Live live;
        draw::Renderer::Paths paths;
        paths.direct_pools = on;
        if (!live.Make(8, 4, why, sizeof why, paths, on))
        {
            Check(false, "a renderer for the ReBAR cases");
            return;
        }
        const vk::resources::Bar& bar = live.allocator.BarMemory();
        const bool direct = on && bar.on && bar.large && sizeof(void*) == 8;
        if (on) Check(bar.on == (bar.heap != 0), "the BAR heap is used where the device has one");
        else Check(!bar.on, "vk_rebar off: no buffer in it");
        FakeEngine fake;
        fake.Reset(live.width, live.height);
        draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStatic, 6 * sizeof(Vertex));
        fake.pools[0]    = pool;
        fake.buffers[0].m_index = 0;
        const char* how = on ? " (ReBAR)" : " (staging)";
        char        what[256];

        // Written before any draw: in place with ReBAR, no frame reads it.
        WriteStatic(live, pool, Argb(255, 0, 0));
        std::vector<uint32_t> px;
        if (live.Begin(Argb(0, 0, 0)))
        {
            Record(live, fake, 6);
            // Written again after a draw of the same frame read it: through staging, between the draws.
            WriteStatic(live, pool, Argb(0, 255, 0));
            Record(live, fake, 6);
            px = live.End();
        }
        std::snprintf(what, sizeof what, "a static pool drawn, written again in the same frame and drawn: the second colour%s", how);
        Check(!px.empty() && px[0] == Argb(0, 255, 0), what);
        const uint64_t direct_first = live.renderer.Counters().direct_bytes;
        std::snprintf(what, sizeof what, "the first write in place only with ReBAR, the one after a draw through staging%s", how);
        Check((direct ? direct_first > 0 : direct_first == 0) && live.renderer.Counters().staged_bytes > 0, what);

        // The frames that read it completed: in place again.
        live.display.WaitSubmitted();
        if (live.Begin(Argb(0, 0, 0)))
        {
            WriteStatic(live, pool, Argb(0, 0, 255));
            Record(live, fake, 6);
            px = live.End();
        }
        std::snprintf(what, sizeof what, "written once the frames reading it completed, then drawn: the new colour%s", how);
        Check(!px.empty() && px[0] == Argb(0, 0, 255) && (direct ? live.renderer.Counters().direct_bytes > direct_first : true), what);
        live.display.WaitSubmitted();
        live.renderer.DestroyPool(pool);
    }

    void PrewarmKnownCases()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path  dir = fs::temp_directory_path(ec) / "wxl-vk-v5-prewarm-test";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        draw::Renderer::Paths paths;
        paths.pipeline_keys   = (dir / "pipelines.keys").string();
        paths.pipeline_cache  = (dir / "pipelines.bin").string();
        paths.shader_pack     = (dir / "shaders.pack").string();
        paths.async_pipelines = true;
        const std::vector<uint8_t> vs = wxl::vk::test::WorldVs(), ps = wxl::vk::test::WorldPs();
        char why[256] = {};

        // Draws one quad with the client's programs (made now unless the renderer has them).
        auto draw = [&](Live& live, int& vp, int& pp) {
            FakeEngine fake;
            fake.Reset(live.width, live.height);
            vp = live.renderer.Program(false, "World:0", vs.data(), vs.size());
            pp = live.renderer.Program(true, "World", ps.data(), ps.size());
            fake.programs[0] = pp;
            fake.programs[1] = vp;
            fake.StateP(vk::gxfront::kVertexShader, &fake);
            fake.StateP(vk::gxfront::kPixelShader, &fake);
            draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
            fake.pools[0]    = pool;
            uint32_t at      = 0;
            bool     drawn   = false;
            if (live.Begin(Argb(0, 0, 0)))
            {
                const float w = static_cast<float>(live.width), h = static_cast<float>(live.height);
                const float projection[16] = {2.0f / w, 0, 0, -1.0f, 0, -2.0f / h, 0, 1.0f, 0, 0, 1.0f, 0, 0, 0, 0, 1.0f};
                const float white[4]       = {1, 1, 1, 1};
                live.renderer.Constants(false, 0, 4, projection);
                live.renderer.Constants(false, 4, 1, white);
                live.renderer.Constants(true, 0, 1, white);
                drawn = QuadOf(live, fake, pool, Argb(255, 0, 0), at);
                live.End();
            }
            live.display.WaitSubmitted();
            live.renderer.DestroyPool(pool);
            return drawn;
        };
        {
            Live live;
            if (!live.Make(16, 8, why, sizeof why, paths))
            {
                Check(false, "a renderer for the earlier launch");
                return;
            }
            int vp = -1, pp = -1;
            draw(live, vp, pp);
            live.renderer.PipelineCache().Drain();
            draw(live, vp, pp);
            Check(vp >= 0 && pp >= 0 && live.renderer.Counters().ahead == 0, "the first launch translates its programs at their bind");
        }
        {
            Live live;
            if (!live.Make(16, 8, why, sizeof why, paths))
            {
                Check(false, "a renderer for the next launch");
                return;
            }
            live.renderer.PipelineCache().Drain(); // the login screen's time
            const auto before = live.renderer.PipelineCache().Counters();
            const auto translated = live.renderer.ShaderCache().Counters().translated;
            Check(live.renderer.Counters().ahead == 2, "the next launch makes the earlier launch's programs from the shader pack at its start");
            int        vp = -1, pp = -1;
            const bool drawn = draw(live, vp, pp);
            const auto after = live.renderer.PipelineCache().Counters();
            Check(drawn && live.renderer.Counters().skipped == 0 && after.linked == before.linked && after.compiled_main == 0 &&
                      after.parts_main == 0 && live.renderer.ShaderCache().Counters().translated == translated,
                  "its first draw with them finds its pipeline: nothing left out, translated, linked or compiled on the main thread");
            Check(before.prewarmed >= 1 && before.prewarmed == before.prewarm_known, "every pipeline the earlier launch drew compiled ahead");
        }
        {
            paths.prewarm_known = false;
            Live live;
            if (live.Make(16, 8, why, sizeof why, paths))
                Check(live.renderer.Counters().ahead == 0, "vk_prewarm off: nothing made ahead of the engine");
        }
        fs::remove_all(dir, ec);
    }

    /// A program added, then drawn into a texture for the first time: its part for the texture's
    /// format compiled in the background meanwhile, no compile on the spot.
    void TargetPartCases()
    {
        char why[256] = {};
        Live live;
        draw::Renderer::Paths paths;
        paths.async_pipelines = true;
        if (!live.Make(16, 8, why, sizeof why, paths) || !live.renderer.PipelineCache().Libraries())
        {
            std::printf("(no graphics pipeline libraries: the texture part case is skipped)\n");
            return;
        }
        const std::vector<uint8_t> vs = wxl::vk::test::WorldVs(), ps = wxl::vk::test::WorldPs();
        const int vp = live.renderer.Program(false, "World:0", vs.data(), vs.size());
        const int pp = live.renderer.Program(true, "World", ps.data(), ps.size());
        live.renderer.PipelineCache().Drain();
        FakeEngine fake;
        fake.Reset(4, 4);
        fake.programs[0] = pp;
        fake.programs[1] = vp;
        fake.StateP(vk::gxfront::kVertexShader, &fake);
        fake.StateP(vk::gxfront::kPixelShader, &fake);
        draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
        fake.pools[0]    = pool;
        draw::TextureDesc desc;
        desc.width            = 4;
        desc.height           = 4;
        desc.target           = true;
        draw::Texture* target = live.renderer.CreateTexture(desc);
        uint32_t       at     = 0;
        bool           drawn  = false;
        if (target && vp >= 0 && pp >= 0 && live.Begin(Argb(0, 0, 0)))
        {
            fake.device.m_textureTarget[0].m_texture = reinterpret_cast<engine::CGxTex*>(&fake.tex_fields[1]);
            live.renderer.SetTarget(target, 0, nullptr);
            drawn = QuadOf(live, fake, pool, Argb(255, 0, 0), at);
            fake.device.m_textureTarget[0].m_texture = nullptr;
            live.renderer.SetTarget(nullptr, 0, nullptr);
            live.End();
        }
        const auto p = live.renderer.PipelineCache().Counters();
        Check(drawn && p.parts_main == 0 && p.compiled_main == 0,
              "the first draw of a program into a texture finds its part for the texture made in the background");
        live.display.WaitSubmitted();
        if (target) live.renderer.DestroyTexture(target);
        live.renderer.DestroyPool(pool);
    }
}

namespace wxl::vk::test
{
    void V5PureCases()
    {
        DistributionCases();
        ChargeCases();
        ZoneCases();
    }

    bool V5LiveCases()
    {
        PassTimingCases();
        RebarCases(false);
        RebarCases(true);
        PrewarmKnownCases();
        TargetPartCases();
        return true;
    }
}
