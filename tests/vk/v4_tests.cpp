// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// V4's renderer on the machine's driver, read back and compared with what D3D9 would show:
//
//   - the back buffer read in the middle of a frame (D3D9's DeviceReadPixels: screenshots, the
//     portraits' check): the frame so far submitted and waited for, then the frame goes on and
//     shows both halves; a rectangle partly outside the scene reads zeros there;
//   - a multisampled scene (gxMultisample): an edge resolved to the mean of its samples, read back
//     and copied from (D3D9's StretchRect resolves), a texture target drawn beside it, the next
//     frame showing the copy texel for texel;
//   - points as sprites, their size clamped to D3D9's POINTSIZE_MIN..POINTSIZE_MAX (the weather's);
//   - pipelines compiled on workers: a draw into the scene left out until its pipeline is ready,
//     one into a texture never; linked from parts (graphics pipeline libraries) where the driver
//     has them, the whole compile taking their place; the pipelines drawn kept in a file and
//     compiled ahead by the next launch.

#include "gx/vk/Log.hpp"
#include "gx/vk/draw/Draw.hpp"
#include "gx/vk/gxfront/Engine.hpp"
#include "gx/vk/present/Display.hpp"

#include "Check.hpp"
#include "Rig.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    using namespace wxl::vk::test::rig;
    using wxl::vk::test::Check;
    namespace pipelines = wxl::vk::pipelines;

    /// Records one point at (x, y) with the fake engine's state.
    bool Point(Live& live, FakeEngine& fake, draw::Pool* pool, float x, float y, uint32_t color, uint32_t& at)
    {
        const Vertex v{x, y, 0.5f, color, 0.5f, 0.5f};
        Stream(live, fake, pool, &v, 1, at);
        vk::gxfront::EngineView view;
        view.device           = &fake.device;
        view.states           = fake.states;
        view.vertex_constants = fake.constants;
        draw::Packet       packet;
        const wxl_gx_batch batch{0, 0, 1, 0, 0};
        return vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Draw && live.renderer.Draw(packet);
    }

    uint32_t Count(const std::vector<uint32_t>& pixels, uint32_t value)
    {
        uint32_t n = 0;
        for (uint32_t p : pixels) n += p == value;
        return n;
    }

    void ReadbackCases()
    {
        char why[256] = {};
        Live live;
        if (!live.Make(16, 8, why, sizeof why))
        {
            Check(false, "a renderer for the readback cases");
            return;
        }
        FakeEngine fake;
        fake.Reset(live.width, live.height);
        draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
        fake.pools[0]    = pool;
        uint32_t at      = 0;
        Vertex   quad[6];

        if (!live.Begin(Argb(0, 0, 0)))
        {
            Check(false, "a frame for the readback cases");
            return;
        }
        Quad(quad, 0, 0, 4, 4, Argb(255, 0, 0));
        Stream(live, fake, pool, quad, 6, at);
        const bool first = Record(live, fake, 6);
        std::vector<uint32_t> read(16 * 8, 0xDEADBEEFu);
        const bool readOk = live.display.ReadScene({{0, 0}, {16, 8}}, read.data());
        Check(first && readOk && read[0] == Argb(255, 0, 0) && read[3 * 16 + 3] == Argb(255, 0, 0) && read[4] == Argb(0, 0, 0) &&
                  read[7 * 16 + 15] == Argb(0, 0, 0),
              "the back buffer read in the middle of a frame: what was drawn so far");
        // The frame goes on after the readback: a second draw, both shown.
        Quad(quad, 8, 0, 12, 4, Argb(0, 255, 0));
        Stream(live, fake, pool, quad, 6, at);
        const bool second = Record(live, fake, 6);
        std::vector<uint32_t> outside(4 * 4, 0xDEADBEEFu);
        const bool edge = live.display.ReadScene({{-2, -2}, {4, 4}}, outside.data());
        const std::vector<uint32_t> shown = live.End();
        Check(second && shown.size() == 16 * 8 && shown[0] == Argb(255, 0, 0) && shown[8] == Argb(0, 255, 0) && shown[5] == Argb(0, 0, 0),
              "the frame goes on after a readback: the draws before and after it are shown");
        Check(edge && outside[0] == 0 && outside[1 * 4 + 1] == 0 && outside[2 * 4 + 2] == Argb(255, 0, 0) && outside[3 * 4 + 3] == Argb(255, 0, 0),
              "a rectangle partly outside the back buffer reads zeros there");
        Check(live.display.Counters().flushes == 2 && live.display.Counters().readbacks == 2,
              "each readback submitted the frame so far once");
        live.renderer.DestroyPool(pool);
    }

    void MultisampleCases()
    {
        char why[256] = {};
        Live live;
        if (!live.Make(16, 8, why, sizeof why))
        {
            Check(false, "a renderer for the multisample cases");
            return;
        }
        if (!(live.display.SampleCounts() & 4))
        {
            std::printf("  (the device draws no 4-sample scene: multisampling not checked)\n");
            return;
        }
        live.display.SetSamples(4);
        FakeEngine fake;
        fake.Reset(live.width, live.height);
        draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
        fake.pools[0]    = pool;
        uint32_t at      = 0;

        draw::TextureDesc desc;
        desc.width          = 16;
        desc.height         = 8;
        desc.target         = true;
        draw::Texture* copy = live.renderer.CreateTexture(desc);
        desc.width = desc.height = 4;
        draw::Texture* target    = live.renderer.CreateTexture(desc);
        if (!copy || !target || !live.Begin(Argb(0, 0, 0)))
        {
            Check(false, "a multisampled frame and its textures");
            return;
        }
        Check(live.display.Samples() == 4, "gxMultisample 4: the scene has four samples per pixel");
        // A red triangle whose long edge crosses pixels: (0,0) (16,0) (0,8), D3D9's pixel corners.
        const Vertex tri[3] = {{-0.5f, -0.5f, 0.5f, Argb(255, 0, 0), 0, 0},
                               {15.5f, -0.5f, 0.5f, Argb(255, 0, 0), 0, 0},
                               {-0.5f, 7.5f, 0.5f, Argb(255, 0, 0), 0, 0}};
        Stream(live, fake, pool, tri, 3, at);
        const bool drawn = Record(live, fake, 3);

        // A texture target drawn in the same frame: one sample per pixel, its own pipelines.
        fake.device.m_textureTarget[0].m_texture = reinterpret_cast<engine::CGxTex*>(&fake.tex_fields[1]);
        fake.device.m_curWindowRect              = {0.0f, 0.0f, 4.0f, 4.0f};
        float* p                                 = &fake.device.m_projNative.a0;
        p[0]                                     = 2.0f / 4.0f;
        p[5]                                     = -2.0f / 4.0f;
        live.renderer.SetTarget(target, 0, nullptr);
        Vertex quad[6];
        Quad(quad, 0, 0, 4, 4, Argb(0, 0, 255));
        Stream(live, fake, pool, quad, 6, at);
        const bool into = Record(live, fake, 6);
        fake.device.m_textureTarget[0].m_texture = nullptr;
        fake.device.m_curWindowRect              = {0.0f, 0.0f, 8.0f, 16.0f};
        p[0]                                     = 2.0f / 16.0f;
        p[5]                                     = -2.0f / 8.0f;
        live.renderer.SetTarget(nullptr, 0, nullptr);

        std::vector<uint32_t> read(16 * 8, 0);
        const bool readOk = live.display.ReadScene({{0, 0}, {16, 8}}, read.data());
        const bool copied = live.renderer.CopyScene(copy, 0, 0, {{0, 0}, {16, 8}}, {{0, 0}, {16, 8}}, false);
        const std::vector<uint32_t> shown = live.End();
        if (wxl::vk::test::Environment("WXL_VK_TRACE"))
            for (uint32_t y = 0; y < 8 && shown.size() == 128; ++y)
            {
                for (uint32_t x = 0; x < 16; ++x) std::printf(" %08x", shown[y * 16 + x]);
                std::printf("\n");
            }
        // Along the edge the pixels are partly covered: their red is the mean of the samples. The
        // first row and column are half covered too: D3D9's clip volume begins on their centres
        // (its viewport is half a pixel inside the image), and a multisampled pixel shows it.
        uint32_t partial = 0;
        for (uint32_t v : shown)
        {
            const uint32_t red = (v >> 16) & 0xFF;
            partial += red > 16 && red < 240;
        }
        Check(drawn && into && shown.size() == 16 * 8 && shown[1 * 16 + 1] == Argb(255, 0, 0) && shown[7 * 16 + 15] == Argb(0, 0, 0) &&
                  partial >= 4 && Near(shown[1 * 16], Argb(128, 0, 0), 1),
              "a multisampled edge is shown resolved: covered pixels red, the edge's between, the others black, the first column half");
        Check(readOk && read == shown, "the back buffer read back resolved, as D3D9's StretchRect into a lockable surface");

        // The next frame shows the copy texel for texel, and the target drawn beside the scene.
        if (copied && live.Begin(Argb(0, 0, 0)))
        {
            fake.textures[0]                    = copy;
            fake.tex_fields[0].m_flags.m_filter = 0;
            fake.StateP(vk::gxfront::kTexture0, &fake.tex_fields[0]);
            fake.State(vk::gxfront::kColorOp0, 0);
            fake.State(vk::gxfront::kAlphaOp0, 0);
            Quad(quad, 0, 0, 16, 8, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            const bool again = Record(live, fake, 6);
            fake.textures[0] = target;
            Quad(quad, 12, 4, 16, 8, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            const bool beside = Record(live, fake, 6);
            const std::vector<uint32_t> next = live.End();
            bool same = again && beside && next.size() == shown.size();
            for (uint32_t y = 1; same && y < 8; ++y)
                for (uint32_t x = 1; same && x < 12; ++x)
                    same = Near(next[y * 16 + x], shown[y * 16 + x], 1);
            Check(same, "the copy from the multisampled scene is the resolved scene, texel for texel (inside the clip volume)");
            Check(beside && next[7 * 16 + 15] == Argb(0, 0, 255), "the texture target drawn in that frame reads back blue");
        }
        Check(live.display.Counters().resolves >= 2, "the scene was resolved for the copy and for the image shown");
        live.renderer.DestroyTexture(copy);
        live.renderer.DestroyTexture(target);
        live.renderer.DestroyPool(pool);
    }

    void PointCases()
    {
        char why[256] = {};
        Live live;
        if (!live.Make(32, 32, why, sizeof why))
        {
            Check(false, "a renderer for the point cases");
            return;
        }
        if (!live.context.Chosen().features.largePoints || live.context.Properties().limits.pointSizeRange[1] < 16.0f)
        {
            std::printf("  (the device draws no large points: point sizes not checked)\n");
            return;
        }
        FakeEngine fake;
        fake.Reset(live.width, live.height);
        draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
        fake.pools[0]    = pool;
        uint32_t at      = 0;
        fake.State(vk::gxfront::kPointSprite, 1);
        fake.StateF(vk::gxfront::kPointScale, 10.0f);
        fake.StateF(vk::gxfront::kPointScaleMin, 0.0f);
        fake.StateF(vk::gxfront::kPointScaleMax, 4.0f);
        if (live.Begin(Argb(0, 0, 0)))
        {
            const bool drawn = Point(live, fake, pool, 7.5f, 7.5f, Argb(255, 255, 255), at);
            fake.StateF(vk::gxfront::kPointScaleMax, 64.0f);
            const bool large                 = Point(live, fake, pool, 23.5f, 23.5f, Argb(255, 255, 255), at);
            const std::vector<uint32_t> px   = live.End();
            uint32_t                    left = 0, right = 0;
            for (uint32_t y = 0; y < 32; ++y)
                for (uint32_t x = 0; x < 32; ++x)
                    if (px.size() == 32 * 32 && px[y * 32 + x] == Argb(255, 255, 255)) ++(x < 16 && y < 16 ? left : right);
            Check(drawn && large && left == 16 && right == 100,
                  "a point's size is clamped to POINTSIZE_MAX (10 drawn as 4x4), and unclamped below it (10x10)");
        }
        live.renderer.DestroyPool(pool);
    }

    /// Draws a red quad into the scene; true when it was recorded.
    bool RedQuad(Live& live, FakeEngine& fake, draw::Pool* pool, uint32_t& at)
    {
        Vertex quad[6];
        Quad(quad, 0, 0, 4, 4, Argb(255, 0, 0));
        Stream(live, fake, pool, quad, 6, at);
        return Record(live, fake, 6);
    }

    void AsyncCases(bool libraries)
    {
        char                   why[256] = {};
        Live                   live;
        draw::Renderer::Paths  paths;
        paths.async_pipelines    = true;
        paths.pipeline_libraries = libraries;
        if (!live.Make(16, 8, why, sizeof why, paths))
        {
            Check(false, "a renderer for the asynchronous pipeline cases");
            return;
        }
        pipelines::Pipelines& cache = live.renderer.PipelineCache();
        if (libraries && !cache.Libraries())
        {
            std::printf("  (the device has no graphics pipeline libraries: linking not checked)\n");
            return;
        }
        FakeEngine fake;
        fake.Reset(live.width, live.height);
        draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
        fake.pools[0]    = pool;
        uint32_t at      = 0;
        const char* how  = libraries ? " (linked from parts)" : " (compiled whole)";
        char        what[160];

        if (live.Begin(Argb(0, 0, 0)))
        {
            const bool                  drawn = RedQuad(live, fake, pool, at);
            const std::vector<uint32_t> px    = live.End();
            std::snprintf(what, sizeof what, "a draw into the scene whose pipeline is compiling is left out, not waited for%s", how);
            Check(!drawn && live.renderer.Counters().skipped == 1 && px.size() == 16 * 8 && px[0] == Argb(0, 0, 0), what);
        }
        cache.Drain();
        if (live.Begin(Argb(0, 0, 0)))
        {
            const bool                  drawn = RedQuad(live, fake, pool, at);
            const std::vector<uint32_t> px    = live.End();
            std::snprintf(what, sizeof what, "once its pipeline is ready the draw is drawn%s", how);
            Check(drawn && px.size() == 16 * 8 && px[0] == Argb(255, 0, 0), what);
        }
        const pipelines::Pipelines::Stats s = cache.Counters();
        if (libraries)
        {
            Check(s.linked == 1 && s.parts_background >= 2 && s.compiled_main == 0 && s.parts_main == 0,
                  "linked from four parts at the draw, the program's parts compiled on a worker, nothing compiled on the main thread");
            cache.Drain();
            if (live.Begin(Argb(0, 0, 0)))
            {
                const bool                  drawn = RedQuad(live, fake, pool, at);
                const std::vector<uint32_t> px    = live.End();
                Check(drawn && cache.Counters().optimized == 1 && px.size() == 16 * 8 && px[0] == Argb(255, 0, 0),
                      "the whole compile takes the linked pipeline's place and draws the same");
            }
        }
        else
            Check(s.background == 1 && s.compiled_main == 0, "compiled whole on a worker, nothing on the main thread");

        // A draw into a texture is never left out: the engine keeps what it drew there.
        fake.State(vk::gxfront::kBlendingMode, 2);
        draw::TextureDesc desc;
        desc.width             = 4;
        desc.height            = 4;
        desc.target            = true;
        draw::Texture* target  = live.renderer.CreateTexture(desc);
        if (target && live.Begin(Argb(0, 0, 0)))
        {
            fake.device.m_textureTarget[0].m_texture = reinterpret_cast<engine::CGxTex*>(&fake.tex_fields[1]);
            live.renderer.SetTarget(target, 0, nullptr);
            const uint64_t skipped = live.renderer.Counters().skipped;
            const bool     drawn   = RedQuad(live, fake, pool, at);
            live.End();
            Check(drawn && live.renderer.Counters().skipped == skipped, "a draw into a texture target waits for its pipeline");
            live.renderer.SetTarget(nullptr, 0, nullptr);
            fake.device.m_textureTarget[0].m_texture = nullptr;
            live.renderer.DestroyTexture(target);
        }
        live.renderer.DestroyPool(pool);
    }

    void PrewarmCases()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path  dir = fs::temp_directory_path(ec) / "wxl-vk-prewarm-test";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        draw::Renderer::Paths paths;
        paths.pipeline_keys  = (dir / "pipelines.keys").string();
        paths.pipeline_cache = (dir / "pipelines.bin").string();
        char why[256]        = {};
        {
            Live live;
            if (!live.Make(16, 8, why, sizeof why, paths))
            {
                Check(false, "a renderer for the prewarm cases");
                return;
            }
            FakeEngine fake;
            fake.Reset(live.width, live.height);
            draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
            fake.pools[0]    = pool;
            uint32_t at      = 0;
            if (live.Begin(Argb(0, 0, 0)))
            {
                RedQuad(live, fake, pool, at);
                fake.State(vk::gxfront::kBlendingMode, 3);
                RedQuad(live, fake, pool, at);
                live.End();
            }
            live.renderer.DestroyPool(pool);
        }
        Check(fs::exists(paths.pipeline_keys, ec) && fs::file_size(paths.pipeline_keys, ec) > 12, "the pipelines drawn are kept in a file");
        {
            Live live;
            if (!live.Make(16, 8, why, sizeof why, paths))
            {
                Check(false, "a second renderer for the prewarm cases");
                return;
            }
            pipelines::Pipelines& cache = live.renderer.PipelineCache();
            cache.Drain();
            const pipelines::Pipelines::Stats before = cache.Counters();
            FakeEngine fake;
            fake.Reset(live.width, live.height);
            draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
            fake.pools[0]    = pool;
            uint32_t at      = 0;
            bool     drawn   = false;
            if (live.Begin(Argb(0, 0, 0)))
            {
                drawn = RedQuad(live, fake, pool, at);
                live.End();
            }
            const pipelines::Pipelines::Stats after = cache.Counters();
            Check(before.prewarm_known == 2 && before.prewarmed == 2 && drawn && after.linked == before.linked &&
                      after.compiled_main == before.compiled_main,
                  "the next launch compiles the pipelines drawn before ahead, as their programs come: the draw finds its pipeline ready");
            live.renderer.DestroyPool(pool);
        }
        fs::remove_all(dir, ec);
    }
}

namespace wxl::vk::test
{
    bool V4LiveCases()
    {
        ReadbackCases();
        MultisampleCases();
        PointCases();
        AsyncCases(false);
        AsyncCases(true);
        PrewarmCases();
        return true;
    }
}
