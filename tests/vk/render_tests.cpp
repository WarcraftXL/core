// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.render and wxl.highlight on the machine's driver (src/gx/vk/render/): images, buffers and
// pipelines beside the engine's draws, passes into images and onto the scene, an engine draw kept
// and drawn again with a batch program (against the scene's depth: never through a wall), the
// scene's colour and depth copied for sampling, GPU zones, the device made again; and the highlight
// drawn on synthetic batches compared, pixel by pixel, with the Direct3D 9 renderer's algorithm
// (gx/d3d9/shaders/highlight.hlsl) computed here on the CPU.

#include "gx/vk/render/Highlight.hpp"
#include "gx/vk/render/Render.hpp"

#include "Check.hpp"
#include "Rig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(WXL_VK_SPIRV_VAL)
#include <spirv-tools/libspirv.hpp>
#endif

namespace wxl::vk::test
{
    // Client programs of the V3 cases (draw_tests.cpp): the position through c0..c3, the colour times c4.
    std::vector<uint8_t> WorldVs();
    std::vector<uint8_t> WorldPs();
}

namespace
{
    namespace render = wxl::vk::render;
    using namespace wxl::vk::test;
    using namespace wxl::vk::test::rig;

#include "outline_edge_frag.h"
#include "outline_mask_frag.h"
#include "render_depth_frag.h"
#include "render_flat_frag.h"
#include "render_pulled_frag.h"
#include "render_pulled_vert.h"
#include "render_sample_frag.h"
#include "render_tint_frag.h"

    std::vector<uint32_t> Words(const uint32_t* code, size_t bytes) { return std::vector<uint32_t>(code, code + bytes / 4); }

    uint32_t Red(uint32_t argb) { return (argb >> 16) & 0xFF; }
    uint32_t Green(uint32_t argb) { return (argb >> 8) & 0xFF; }
    uint32_t Blue(uint32_t argb) { return argb & 0xFF; }

    struct Setup
    {
        Live           live;
        FakeEngine     fake;
        draw::Pool*    pool = nullptr;
        uint32_t       at = 0;
        render::Render render;

        bool Make(uint32_t w, uint32_t h, uint32_t samples = 1)
        {
            char why[256] = {};
            if (!live.Make(w, h, why, sizeof why)) return false;
            if (samples > 1) live.display.SetSamples(samples);
            fake.Reset(w, h);
            pool          = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 1u << 16);
            fake.pools[0] = pool;
            return render.Create(live.context, live.allocator, live.timeline, live.display, live.renderer);
        }

        bool Begin(uint32_t clear = Argb(0, 0, 0))
        {
            if (!live.Begin(clear)) return false;
            render.FrameBegun();
            return true;
        }

        /// An engine quad over the pixels [x0, x1) x [y0, y1) at depth z, kept as a batch.
        bool Quad(float x0, float y0, float x1, float y1, uint32_t color, float z, draw::Batch& kept)
        {
            Vertex quad[6];
            rig::Quad(quad, x0, y0, x1, y1, color);
            for (Vertex& v : quad) v.z = z;
            Stream(live, fake, pool, quad, 6, at);
            live.renderer.KeepBatches(true);
            live.renderer.ForgetLastBatch();
            const bool drawn = Record(live, fake, 6);
            live.renderer.KeepBatches(false);
            if (!live.renderer.LastBatch()) return false;
            kept = *live.renderer.LastBatch();
            return drawn;
        }

        ~Setup()
        {
            if (live.timeline.Ready() && live.timeline.Last()) live.timeline.WaitFor(live.timeline.Last());
            render.Destroy();
            if (pool) live.renderer.DestroyPool(pool);
        }
    };

    render::Pipeline* Fullscreen(render::Render& r, const uint32_t* code, size_t bytes, uint32_t format, uint32_t blend, uint32_t samples)
    {
        render::PipelineSpec spec;
        spec.kind             = render::kFullscreen;
        spec.fragment         = Words(code, bytes);
        spec.color_formats[0] = format;
        spec.blend[0]         = blend;
        spec.samples          = samples;
        std::string why;
        render::Pipeline* p = r.CreatePipeline(spec, why);
        if (!p) std::printf("  pipeline refused: %s\n", why.c_str());
        return p;
    }

    render::PassSpec Onto(render::Image* image, bool clear = false)
    {
        render::PassSpec pass;
        pass.color_count = 1;
        if (image) pass.colors[0] = image;
        else pass.scene_color[0] = true;
        pass.clear = clear ? 1u : 0u;
        return pass;
    }

    struct SamplePush
    {
        uint32_t image;
        uint32_t sampler;
    };

    // --- The Direct3D 9 algorithm, on the CPU ----------------------------------------------------------

    struct Rgba
    {
        float r = 0, g = 0, b = 0, a = 0;
    };

    struct Plane
    {
        uint32_t          w = 0, h = 0;
        std::vector<Rgba> texels;
        Rgba&       At(int x, int y) { return texels[static_cast<size_t>(y) * w + x]; }
        const Rgba& At(int x, int y) const { return texels[static_cast<size_t>(y) * w + x]; }
        Rgba Point(float u, float v) const
        {
            const int x = std::clamp(static_cast<int>(std::floor(u * static_cast<float>(w))), 0, static_cast<int>(w) - 1);
            const int y = std::clamp(static_cast<int>(std::floor(v * static_cast<float>(h))), 0, static_cast<int>(h) - 1);
            return At(x, y);
        }
        Rgba Linear(float u, float v) const
        {
            const float tx = u * static_cast<float>(w) - 0.5f, ty = v * static_cast<float>(h) - 0.5f;
            const int   x0 = static_cast<int>(std::floor(tx)), y0 = static_cast<int>(std::floor(ty));
            const float fx = tx - static_cast<float>(x0), fy = ty - static_cast<float>(y0);
            auto        at = [&](int x, int y) {
                return At(std::clamp(x, 0, static_cast<int>(w) - 1), std::clamp(y, 0, static_cast<int>(h) - 1));
            };
            const Rgba a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
            auto mix = [&](float p, float q, float r, float s) { return (p * (1 - fx) + q * fx) * (1 - fy) + (r * (1 - fx) + s * fx) * fy; };
            return {mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g), mix(a.b, b.b, c.b, d.b), mix(a.a, b.a, c.a, d.a)};
        }
        void Quantize()
        {
            for (Rgba& t : texels)
                for (float* c : {&t.r, &t.g, &t.b, &t.a}) *c = std::round(std::clamp(*c, 0.0f, 1.0f) * 255.0f) / 255.0f;
        }
    };

    /// The highlight of one object covering @p inside, over @p scene (ARGB), as the Direct3D 9 passes draw it.
    std::vector<uint32_t> ReferenceHighlight(const std::vector<uint32_t>& scene, uint32_t w, uint32_t h, const std::vector<bool>& inside,
                                             const render::Highlight::Style& style)
    {
        const float scale   = static_cast<float>(h) / 1080.0f;
        const float outline = std::min(style.outline * scale, 32.0f), glowWidth = std::min(style.glow * scale, 128.0f);
        const float r = ((style.color >> 16) & 0xFF) / 255.0f, g = ((style.color >> 8) & 0xFF) / 255.0f, b = (style.color & 0xFF) / 255.0f;
        Plane color{w, h, std::vector<Rgba>(w * h)}, extra{w, h, std::vector<Rgba>(w * h)};
        for (uint32_t i = 0; i < w * h; ++i)
            if (inside[i])
            {
                color.texels[i] = {r, g, b, 1.0f};
                extra.texels[i] = {r * style.strength, g * style.strength, b * style.strength, outline / 32.0f};
            }
        color.Quantize();
        extra.Quantize();
        const float  radius   = std::max({2.0f, glowWidth, outline + 2.0f});
        const float  step     = radius / 4.0f;
        const float  kW[5]    = {0.2270270270f, 0.1945945946f, 0.1216216216f, 0.0540540541f, 0.0162162162f};
        const uint32_t hw     = (w + 1) / 2, hh = (h + 1) / 2;
        auto blur = [&](const Plane& from, float du, float dv) {
            Plane to{hw, hh, std::vector<Rgba>(hw * hh)};
            for (uint32_t y = 0; y < hh; ++y)
                for (uint32_t x = 0; x < hw; ++x)
                {
                    const float u = (x + 0.5f) / hw, v = (y + 0.5f) / hh;
                    Rgba        sum;
                    for (int k = -4; k <= 4; ++k)
                    {
                        const Rgba  s  = from.Linear(u + du * k, v + dv * k);
                        const float wk = kW[std::abs(k)];
                        sum.r += s.r * wk;
                        sum.g += s.g * wk;
                        sum.b += s.b * wk;
                        sum.a += s.a * wk;
                    }
                    to.At(static_cast<int>(x), static_cast<int>(y)) = sum;
                }
            to.Quantize();
            return to;
        };
        const Plane glowA = blur(extra, step / static_cast<float>(w), 0.0f);
        const Plane glowB = blur(glowA, 0.0f, step / static_cast<float>(h));
        const bool  glow  = style.glow > 0.0f && style.strength > 0.0f;
        static const float kDirs[12][2] = {{1, 0},          {0.8660254f, 0.5f},   {0.5f, 0.8660254f},   {0, 1},
                                           {-0.5f, 0.8660254f}, {-0.8660254f, 0.5f}, {-1, 0},            {-0.8660254f, -0.5f},
                                           {-0.5f, -0.8660254f}, {0, -1},          {0.5f, -0.8660254f},  {0.8660254f, -0.5f}};
        std::vector<uint32_t> out(scene);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
                const float in   = color.Point(u, v).a;
                const Rgba  near = glowB.Linear(u, v);
                float       best = 0.0f;
                float       cr = 0, cg = 0, cb = 0;
                if (near.a > 0.0f)
                    for (float distance : {outline / 3.0f, outline * 2.0f / 3.0f, outline})
                        for (const auto& d : kDirs)
                        {
                            const float au = u + d[0] * distance / w, av = v + d[1] * distance / h;
                            const Rgba  s        = color.Point(au, av);
                            const float coverage = std::max(s.a, 0.004f);
                            const float width    = extra.Point(au, av).a / coverage * 32.0f;
                            const float cover    = std::clamp(width - distance + 1.0f, 0.0f, 1.0f) * (s.a >= 0.004f ? 1.0f : 0.0f);
                            if (cover >= best + 0.0001f)
                            {
                                best = cover;
                                cr   = s.r / coverage;
                                cg   = s.g / coverage;
                                cb   = s.b / coverage;
                            }
                        }
                const float o     = best * (1.0f - in);
                const float gain  = 2.0f * (glow ? 1.0f : 0.0f) * (1.0f - in);
                const float src[4] = {cr * o + near.r * gain * (1 - o), cg * o + near.g * gain * (1 - o), cb * o + near.b * gain * (1 - o), o};
                uint32_t&   px     = out[y * w + x];
                const float dst[3] = {Red(px) / 255.0f, Green(px) / 255.0f, Blue(px) / 255.0f};
                uint32_t    c[3];
                for (int i = 0; i < 3; ++i)
                    c[i] = static_cast<uint32_t>(std::round(std::clamp(src[i] + dst[i] * (1.0f - src[3]), 0.0f, 1.0f) * 255.0f));
                px = (px & 0xFF000000u) | c[0] << 16 | c[1] << 8 | c[2];
            }
        return out;
    }

    // --- The cases -------------------------------------------------------------------------------------

    struct Zones final : render::ZoneSink
    {
        std::vector<std::string> names;
        bool                     ordered = true;
        void Zone(const char* name, int64_t begin, int64_t end) override
        {
            names.emplace_back(name);
            ordered = ordered && end >= begin;
        }
    };

    void PassCases(Setup& s)
    {
        render::Render& r = s.render;
        const uint32_t  W = s.live.width, H = s.live.height;

        // Images: a fixed size, the screen's halves.
        render::Image* image = r.CreateImage(render::ImageSpec{W, H, 0, render::kRgba8, 1});
        render::Image* half  = r.CreateImage(render::ImageSpec{0, 0, 2, render::kRgba8, 1});
        std::string    why;
        Check(!r.CreateImage(render::ImageSpec{0, 0, 0, 99, 1}, &why) && !why.empty(), "an unknown image format is refused, with a reason");
        Check(image && r.Slot(image) != render::kNoSlot, "a fixed image is made with a bindless slot");
        Check(half && r.Extent(half).width == (W + 1) / 2 && r.Extent(half).height == (H + 1) / 2, "a screen image of divisor 2 is half the scene, rounded up");

        render::Pipeline* tint   = Fullscreen(r, render_tint_frag, sizeof render_tint_frag, render::kBgra8, render::kBlendAlpha, 0);
        render::Pipeline* sample = Fullscreen(r, render_sample_frag, sizeof render_sample_frag, render::kBgra8, render::kBlendAlpha, 0);
        render::PipelineSpec flatSpec;
        flatSpec.kind             = render::kBatch;
        flatSpec.fragment         = Words(render_flat_frag, sizeof render_flat_frag);
        flatSpec.color_formats[0] = render::kRgba8;
        flatSpec.samples          = 1;
        render::Pipeline* flat    = r.CreatePipeline(flatSpec, why);
        render::PipelineSpec walled = flatSpec;
        walled.depth_format = render::kDepth32f;
        walled.depth_test   = render::kDepthLessEqual;
        walled.samples      = 0;
        render::Pipeline* wall = r.CreatePipeline(walled, why);
        render::PipelineSpec bad = flatSpec;
        bad.fragment = {0x07230203u, 0x00010500u};
        Check(!r.CreatePipeline(bad, why), "a truncated SPIR-V module is refused");
        render::PipelineSpec mixed = flatSpec;
        mixed.depth_test = render::kDepthLess;
        mixed.depth_format = 0;
        Check(!r.CreatePipeline(mixed, why), "a depth test without a depth format is refused");
        Check(tint && sample && flat && wall, "fullscreen and batch pipelines are made");
        if (!tint || !sample || !flat || !wall || !image) return;

        // A tint onto the scene, alpha-blended over black.
        if (!s.Begin()) return;
        Check(r.Begin(Onto(nullptr)), "a pass onto the scene's colour begins");
        const float half_red[4] = {1.0f, 0.0f, 0.0f, 0.5f};
        Check(r.Fullscreen(tint, half_red, sizeof half_red), "a fullscreen draw is recorded");
        Check(!r.Draw(flat, 3, 1, nullptr, 0), "a batch pipeline is no own draw's");
        r.End();
        std::vector<uint32_t> p = s.live.End();
        Check(!p.empty() && Near(p[W * H / 2 + W / 2], Argb(128, 0, 0), 1), "a FULLSCREEN pipeline blends its colour over the whole scene");

        // An engine draw kept, drawn again into an image with a batch program, sampled back onto the scene.
        if (!s.Begin()) return;
        draw::Batch batch;
        Check(s.Quad(16, 12, 48, 36, Argb(0, 255, 0), 0.5f, batch), "an engine draw is kept as a batch");
        render::PassSpec into = Onto(image, true);
        Check(r.Begin(into), "a pass into an image begins");
        const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        Check(r.DrawBatch(batch, flat, blue, sizeof blue), "the batch is drawn again with the mod's program");
        r.End();
        Check(r.Begin(Onto(nullptr)), "a pass onto the scene again");
        const SamplePush read{r.Slot(image), r.Sampler(0)};
        r.Fullscreen(sample, &read, sizeof read);
        r.End();
        p = s.live.End();
        Check(!p.empty() && Near(p[20 * W + 30], Argb(0, 0, 255), 1) && Near(p[4 * W + 4], Argb(0, 0, 0), 1) &&
                  Near(p[11 * W + 30], Argb(0, 0, 0), 1) && Near(p[12 * W + 30], Argb(0, 0, 255), 1),
              "the batch is redrawn where the engine drew it, to the pixel");

        // Never through a wall: the scene's depth nearer than the batch over its left half.
        if (!s.Begin()) return;
        s.fake.State(wxl::vk::gxfront::kDepthTest, 1);
        s.fake.State(wxl::vk::gxfront::kDepthWrite, 1);
        Check(s.Quad(16, 12, 48, 36, Argb(0, 255, 0), 0.5f, batch), "a batch drawn testing depth is kept");
        s.fake.State(wxl::vk::gxfront::kDepthTest, 0);
        s.fake.State(wxl::vk::gxfront::kDepthWrite, 0);
        const float black[4] = {0, 0, 0, 1};
        s.live.display.Clear(false, true, black, 0.25f, {{0, 0}, {32, H}});
        render::PassSpec masked = Onto(image, true);
        masked.scene_depth      = true;
        Check(r.Begin(masked), "a pass into an image with the scene's depth begins");
        Check(r.DrawBatch(batch, wall, blue, sizeof blue), "the batch is drawn against the scene's depth");
        r.End();
        Check(r.Begin(Onto(nullptr)), "onto the scene");
        r.Fullscreen(sample, &read, sizeof read);
        r.End();
        p = s.live.End();
        Check(!p.empty() && Near(p[20 * W + 40], Argb(0, 0, 255), 1) && Near(p[20 * W + 20], Argb(0, 255, 0), 1),
              "where something nearer covers it the batch is not drawn again (never through a wall), where nothing does it is");

        // A pipeline testing depth does not fit a pass without depth.
        if (!s.Begin()) return;
        Check(s.Quad(16, 12, 48, 36, Argb(0, 255, 0), 0.5f, batch) && r.Begin(Onto(image, true)), "a pass without depth begins");
        Check(!r.DrawBatch(batch, wall, blue, sizeof blue), "a pipeline testing depth is refused in a pass without depth");
        Check(r.DrawBatch(batch, flat, blue, sizeof blue), "one without depth draws there");
        r.End();
        s.live.End();

        // A stale batch is refused.
        if (!s.Begin()) return;
        render::PassSpec later = Onto(image, true);
        later.scene_depth      = true;
        Check(r.Begin(later), "a pass begins");
        Check(!r.DrawBatch(batch, wall, blue, sizeof blue), "a batch of an earlier frame is refused");
        r.End();
        s.live.End();
    }

    void BufferCases(Setup& s)
    {
        render::Render& r = s.render;
        const uint32_t  W = s.live.width, H = s.live.height;
        render::PipelineSpec spec;
        spec.kind             = render::kGraphics;
        spec.vertex           = Words(render_pulled_vert, sizeof render_pulled_vert);
        spec.fragment         = Words(render_pulled_frag, sizeof render_pulled_frag);
        spec.color_formats[0] = render::kBgra8;
        std::string       why;
        render::Pipeline* pulled = r.CreatePipeline(spec, why);
        render::Buffer*   vertices = r.CreateBuffer(4 * 32);
        render::Buffer*   indices  = r.CreateBuffer(6 * 4);
        Check(pulled && vertices && indices, "a graphics pipeline and two buffers are made");
        if (!pulled || !vertices || !indices) return;
        // The right half of the scene, in clip space, yellow.
        const float quad[4][8] = {{0, -1, 0, 0, 1, 1, 0, 1}, {1, -1, 0, 0, 1, 1, 0, 1}, {0, 1, 0, 0, 1, 1, 0, 1}, {1, 1, 0, 0, 1, 1, 0, 1}};
        const uint32_t idx[6]  = {0, 1, 2, 1, 3, 2};
        Check(r.WriteBuffer(vertices, 0, quad, sizeof quad) && r.WriteBuffer(indices, 0, idx, sizeof idx), "buffers are written outside a frame");
        Check(!r.WriteBuffer(indices, 20, idx, sizeof idx), "a write past a buffer's end is refused");
        if (!s.Begin()) return;
        Check(r.Address(vertices) != 0, "a buffer has an address once made");
        const uint64_t address = r.Address(vertices);
        Check(r.Begin(Onto(nullptr)), "a pass onto the scene");
        Check(r.DrawIndexed(pulled, indices, 0, 6, 1, &address, sizeof address), "an indexed draw of the mod's own vertices");
        Check(!r.DrawIndexed(pulled, indices, 4, 6, 1, &address, sizeof address), "indices past the buffer's end are refused");
        r.End();
        std::vector<uint32_t> p = s.live.End();
        Check(!p.empty() && Near(p[(H / 2) * W + W * 3 / 4], Argb(255, 255, 0), 1) && Near(p[(H / 2) * W + W / 4], Argb(0, 0, 0), 1),
              "vertices read by address and 32-bit indices draw where they say");
        r.DestroyBuffer(vertices);
        r.DestroyBuffer(indices);
        r.DestroyPipeline(pulled);
    }

    void SceneCases(Setup& s)
    {
        render::Render& r = s.render;
        const uint32_t  W = s.live.width, H = s.live.height;
        render::Pipeline* sample = Fullscreen(r, render_sample_frag, sizeof render_sample_frag, render::kBgra8, render::kBlendNone, 0);
        render::Pipeline* depth  = Fullscreen(r, render_depth_frag, sizeof render_depth_frag, render::kBgra8, render::kBlendNone, 0);
        if (!sample || !depth) return;

        // The scene's colour copied, the scene cleared, the copy drawn back: the same pixels.
        if (!s.Begin(Argb(10, 20, 30))) return;
        draw::Batch batch;
        s.Quad(8, 8, 24, 24, Argb(200, 100, 50), 0.5f, batch);
        const uint32_t color = r.SceneColorSlot(1);
        Check(color != render::kNoSlot && r.SceneColorSlot(1) == color, "the scene's colour is copied once per point");
        const float black[4] = {0, 0, 0, 1};
        s.live.display.Clear(true, false, black, 1.0f, {{0, 0}, {W, H}});
        Check(r.Begin(Onto(nullptr)), "onto the scene");
        Check(r.SceneColorSlot(2) == render::kNoSlot, "no copy of the scene inside a pass");
        const SamplePush back{color, r.Sampler(0)};
        r.Fullscreen(sample, &back, sizeof back);
        r.End();
        std::vector<uint32_t> p = s.live.End();
        Check(!p.empty() && Near(p[16 * W + 16], Argb(200, 100, 50), 1) && Near(p[2 * W + 2], Argb(10, 20, 30), 1),
              "the scene's colour as drawn so far, sampled back to the pixel");

        // The scene's depth, as one channel.
        if (!s.Begin()) return;
        s.live.display.Clear(false, true, black, 0.5f, {{0, 0}, {W / 2, H}});
        const uint32_t z = r.SceneDepthSlot(3);
        Check(z != render::kNoSlot, "the scene's depth is copied");
        Check(r.Begin(Onto(nullptr)), "onto the scene");
        const SamplePush zread{z, r.Sampler(0)};
        r.Fullscreen(depth, &zread, sizeof zread);
        r.End();
        p = s.live.End();
        Check(!p.empty() && Near(Red(p[4 * W + 4]), 128, 1) && Red(p[4 * W + W - 4]) == 255, "the scene's depth, sampled: 0.5 where it was cleared to it, 1 elsewhere");
    }

    /// The same pieces on a multisampled scene: an image of its samples drawn against its depth and
    /// resolved for sampling, its depth's first sample copied.
    void MultisampleCases(Setup& s)
    {
        render::Render& r = s.render;
        const uint32_t  W = s.live.width;
        if (!s.Begin()) return;
        if (s.live.display.Samples() == 1)
        {
            s.live.End();
            std::printf("  (no multisampled scene on this device)\n");
            return;
        }
        render::Image* image = r.CreateImage(render::ImageSpec{0, 0, 1, render::kRgba8, 0});
        render::PipelineSpec spec;
        spec.kind             = render::kBatch;
        spec.fragment         = Words(render_flat_frag, sizeof render_flat_frag);
        spec.color_formats[0] = render::kRgba8;
        spec.depth_format     = render::kDepth32f;
        spec.depth_test       = render::kDepthLessEqual;
        std::string       why;
        render::Pipeline* wall   = r.CreatePipeline(spec, why);
        render::Pipeline* sample = Fullscreen(r, render_sample_frag, sizeof render_sample_frag, render::kBgra8, render::kBlendAlpha, 0);
        draw::Batch batch;
        s.fake.State(wxl::vk::gxfront::kDepthTest, 1);
        s.fake.State(wxl::vk::gxfront::kDepthWrite, 1);
        Check(s.Quad(16, 12, 48, 36, Argb(0, 255, 0), 0.5f, batch), "a batch is kept in a multisampled scene");
        s.fake.State(wxl::vk::gxfront::kDepthTest, 0);
        s.fake.State(wxl::vk::gxfront::kDepthWrite, 0);
        const float black[4] = {0, 0, 0, 1};
        s.live.display.Clear(false, true, black, 0.25f, {{0, 0}, {32, s.live.height}});
        render::PassSpec masked = Onto(image, true);
        masked.scene_depth      = true;
        Check(image && wall && sample && r.Begin(masked), "a pass of the scene's samples against its depth begins");
        const float blue[4] = {0, 0, 1, 1};
        Check(r.DrawBatch(batch, wall, blue, sizeof blue), "the batch is drawn again, multisampled");
        r.End();
        const uint32_t z = r.SceneDepthSlot(7);
        Check(z != render::kNoSlot, "the multisampled depth's first sample is copied");
        Check(r.Begin(Onto(nullptr)), "onto the multisampled scene");
        const SamplePush read{r.Slot(image), r.Sampler(0)};
        r.Fullscreen(sample, &read, sizeof read);
        r.End();
        std::vector<uint32_t> p = s.live.End();
        Check(!p.empty() && Near(p[20 * W + 40], Argb(0, 0, 255), 1) && Near(p[20 * W + 20], Argb(0, 255, 0), 1),
              "resolved, the mask is where the batch is seen and not behind the nearer depth");
        r.DestroyImage(image);
        r.DestroyPipeline(wall);
        r.DestroyPipeline(sample);
    }

    bool HighlightCase(uint32_t samples)
    {
        Setup s;
        const uint32_t W = 160, H = 1080; // 1080 lines: widths drawn as given
        if (!s.Make(W, H, samples)) return false;
        render::Highlight highlight;
        highlight.Attach(s.render);
        const render::Highlight::Style style{0xFF4020u, 6.0f, 24.0f, 0.6f};

        if (!s.Begin()) return true;
        draw::Batch batch;
        s.fake.State(wxl::vk::gxfront::kDepthTest, 1);
        s.fake.State(wxl::vk::gxfront::kDepthWrite, 1);
        Check(s.Quad(40, 400, 120, 680, Argb(30, 60, 90), 0.5f, batch), "a highlighted model's batch is kept");
        s.fake.State(wxl::vk::gxfront::kDepthTest, 0);
        s.fake.State(wxl::vk::gxfront::kDepthWrite, 0);
        draw::Batch translucent = batch;
        translucent.scene       = false;
        highlight.Begin();
        Check(highlight.Keep(batch, 0) && !highlight.Keep(translucent, 0), "a batch drawn into the scene is kept, one drawn into a texture is not");
        const char* why = nullptr;
        const uint32_t drawn = highlight.Draw(&style, 1, &why);
        Check(drawn == 1, "the kept batch is drawn again into the masks");
        std::vector<uint32_t> gpu = s.live.End();
        if (gpu.empty()) return true;

        // The same picture by the Direct3D 9 algorithm: the quad's pixels inside, the scene black around it.
        std::vector<bool>     inside(W * H, false);
        std::vector<uint32_t> scene(W * H, Argb(0, 0, 0));
        for (uint32_t y = 400; y < 680; ++y)
            for (uint32_t x = 40; x < 120; ++x)
            {
                inside[y * W + x] = true;
                scene[y * W + x]  = Argb(30, 60, 90);
            }
        const std::vector<uint32_t> cpu = ReferenceHighlight(scene, W, H, inside, style);
        uint32_t worst = 0, off = 0;
        for (uint32_t i = 0; i < W * H; ++i)
            for (int shift = 0; shift < 24; shift += 8)
            {
                const int d = std::abs(static_cast<int>((gpu[i] >> shift) & 0xFF) - static_cast<int>((cpu[i] >> shift) & 0xFF));
                worst       = std::max(worst, static_cast<uint32_t>(d));
                if (d > 2) ++off;
            }
        // Multisampled masks are resolved (edges averaged) on both devices; the reference has one
        // sample, so only the one-sample picture is held to it pixel by pixel.
        // The scene's samples are chosen at the first frame: the most the device draws, up to those asked.
        std::printf("  highlight at %u sample(s) (%u asked) against the Direct3D 9 algorithm: worst channel %u, %u channels off by more than 2\n",
                    s.live.display.Samples(), samples, worst, off);
        if (s.live.display.Samples() == 1)
        {
            char what[160];
            std::snprintf(what, sizeof what, "the highlight matches the Direct3D 9 algorithm (worst channel %u, %u channels off by more than 2)",
                          worst, off);
            Check(worst <= 6 && off <= W * H * 3 / 1000, what);
        }
        Check(Near(gpu[540 * W + 80], Argb(30, 60, 90), 0), "over the object nothing is drawn");
        Check(Red(gpu[540 * W + 37]) > 200 && Green(gpu[540 * W + 37]) < 120, "the outline lies just outside the object, in its colour");
        Check(Red(gpu[540 * W + 22]) > 5 && Red(gpu[540 * W + 22]) < 200, "the glow fades beyond the outline");
        Check(Near(gpu[100 * W + 80], Argb(0, 0, 0), 0), "far from it nothing is drawn");
        highlight.Detach();
        return true;
    }

    /// The world pass as wxl.highlight's service drives it on the recompiled client: the highlighted
    /// top models handed over, the renderer keeping each draw while they are, every model batch the
    /// engine draws (with a client vertex program, as M2 batches are) offered with the renderer's
    /// record of that draw, then the passes after the world. Another model's batch and the object's
    /// blended one are not kept; the picture is the Direct3D 9 algorithm's.
    void HighlightPathCase(uint32_t samples)
    {
        Setup s;
        const uint32_t W = 160, H = 1080;
        if (!s.Make(W, H, samples)) return;
        draw::Renderer& engine = s.live.renderer;
        const std::vector<uint8_t> vs = wxl::vk::test::WorldVs(), ps = wxl::vk::test::WorldPs();
        const int vp = engine.Program(false, "World:0", vs.data(), vs.size());
        const int pp = engine.Program(true, "World", ps.data(), ps.size());
        Check(vp >= static_cast<int>(wxl::vk::pipelines::VertexProgram::Count) && pp >= 0, "the models' client programs are translated");
        if (vp < 0 || pp < 0) return;

        render::Highlight highlight;
        highlight.Attach(s.render);
        const render::Highlight::Style style{0xFF4020u, 6.0f, 24.0f, 0.6f};
        int object = 0, another = 0; // two top models
        const render::Highlight::Root roots[1] = {{&object, 0}};
        if (!s.Begin()) return;

        // The world pass begins: the object's model is highlighted, so the renderer keeps each draw.
        highlight.World(roots, 1);
        engine.KeepBatches(highlight.Capturing());
        const float w = static_cast<float>(W), h = static_cast<float>(H);
        const float projection[16] = {2.0f / w, 0, 0, -1.0f, 0, -2.0f / h, 0, 1.0f, 0, 0, 1.0f, 0, 0, 0, 0, 1.0f};
        const float white[4]       = {1, 1, 1, 1};
        engine.Constants(false, 0, 4, projection);
        engine.Constants(false, 4, 1, white);
        engine.Constants(true, 0, 1, white);
        s.fake.programs[1] = vp;
        s.fake.programs[0] = pp;
        s.fake.StateP(wxl::vk::gxfront::kVertexShader, &s.fake);
        s.fake.StateP(wxl::vk::gxfront::kPixelShader, &s.fake);
        s.fake.State(wxl::vk::gxfront::kDepthTest, 1);
        s.fake.State(wxl::vk::gxfront::kDepthWrite, 1);
        // A model batch as CM2SceneRender::DrawBatch makes it, then M2.DrawBatch with the record of it.
        auto modelBatch = [&](const void* root, uint32_t blend, float x0, float y0, float x1, float y1, uint32_t color) {
            Vertex quad[6];
            rig::Quad(quad, x0, y0, x1, y1, color);
            Stream(s.live, s.fake, s.pool, quad, 6, s.at);
            engine.ForgetLastBatch(); // as the device's Draw does first
            const bool drawn = Record(s.live, s.fake, 6);
            return highlight.Batch(root, blend, engine.LastBatch()) && drawn;
        };
        const bool kept    = modelBatch(&object, 0, 40, 400, 120, 680, Argb(30, 60, 90));
        const bool other   = modelBatch(&another, 0, 0, 0, 30, 100, Argb(0, 0, 0));
        const bool blended = modelBatch(&object, 2, 60, 450, 100, 500, Argb(30, 60, 90));
        Check(kept && !other && !blended, "the object's opaque batch is kept; another model's and its blended one are not");
        s.fake.State(wxl::vk::gxfront::kDepthTest, 0);
        s.fake.State(wxl::vk::gxfront::kDepthWrite, 0);
        s.fake.StateP(wxl::vk::gxfront::kVertexShader, nullptr);
        s.fake.StateP(wxl::vk::gxfront::kPixelShader, nullptr);
        s.fake.programs[0] = s.fake.programs[1] = -1;

        // After the world.
        const char*    why   = nullptr;
        const uint32_t drawn = highlight.Draw(&style, 1, &why);
        engine.KeepBatches(false);
        const render::Highlight::Counts& c = highlight.Pass();
        char what[200];
        std::snprintf(what, sizeof what,
                      "the pass counts 1 top model, 3 model batches, 2 of them the object's, 1 kept, 1 drawn into the masks, composited "
                      "(got %u, %u, %u, %u, %u, %s%s%s)",
                      c.roots, c.models, c.seen, c.kept, c.drawn, c.composited ? "composited" : "not composited", why ? ": " : "", why ? why : "");
        Check(drawn == 1 && c.roots == 1 && c.models == 3 && c.seen == 2 && c.kept == 1 && c.drawn == 1 && c.composited && c.refusal.empty(),
              what);
        std::vector<uint32_t> gpu = s.live.End();
        if (gpu.empty()) return;

        std::vector<bool>     inside(W * H, false);
        std::vector<uint32_t> scene(W * H, Argb(0, 0, 0));
        for (uint32_t y = 400; y < 680; ++y)
            for (uint32_t x = 40; x < 120; ++x)
            {
                inside[y * W + x] = true;
                scene[y * W + x]  = Argb(30, 60, 90);
            }
        const std::vector<uint32_t> cpu = ReferenceHighlight(scene, W, H, inside, style);
        uint32_t worst = 0, off = 0;
        for (uint32_t i = 0; i < W * H; ++i)
            for (int shift = 0; shift < 24; shift += 8)
            {
                const int d = std::abs(static_cast<int>((gpu[i] >> shift) & 0xFF) - static_cast<int>((cpu[i] >> shift) & 0xFF));
                worst       = std::max(worst, static_cast<uint32_t>(d));
                if (d > 2) ++off;
            }
        std::printf("  highlight through the service's path at %u sample(s) (%u asked) against the Direct3D 9 algorithm: worst channel %u, %u "
                    "channels off by more than 2\n",
                    s.live.display.Samples(), samples, worst, off);
        if (s.live.display.Samples() == 1)
        {
            std::snprintf(what, sizeof what, "through the service's path, the highlight matches the Direct3D 9 algorithm (worst channel %u, %u "
                                             "channels off by more than 2)", worst, off);
            Check(worst <= 6 && off <= W * H * 3 / 1000, what);
        }
        Check(Near(gpu[540 * W + 80], Argb(30, 60, 90), 0), "over the object nothing is drawn");
        Check(Red(gpu[540 * W + 37]) > 200 && Green(gpu[540 * W + 37]) < 120, "the outline lies just outside the object, in its colour");
        Check(Red(gpu[540 * W + 22]) > 5 && Red(gpu[540 * W + 22]) < 200, "the glow fades beyond the outline");
        Check(Near(gpu[50 * W + 15], Argb(0, 0, 0), 0) && Near(gpu[100 * W + 80], Argb(0, 0, 0), 0),
              "around the other model and far from the object nothing is drawn");

        // A pass in which the renderer kept no record of the object's draw says why, and draws nothing.
        if (s.Begin())
        {
            highlight.World(roots, 1);
            Check(!highlight.Batch(&object, 0, nullptr) && highlight.Draw(&style, 1, &why) == 0 && why &&
                      std::string(why).find("kept no record") != std::string::npos && !highlight.Pass().composited,
                  "a batch without the renderer's record is refused with its reason, and nothing is drawn");
            s.live.End();
        }
        highlight.Detach();
    }

    /// The SDK's example mod wxl.outline, its calls made here on a target half behind a wall: the
    /// outline around what is seen, none over the wall.
    void OutlineExampleCase()
    {
        Setup s;
        const uint32_t W = 96, H = 64;
        if (!s.Make(W, H)) return;
        render::Render& r = s.render;
        render::Image*  mask = r.CreateImage(render::ImageSpec{0, 0, 1, render::kR8, 0});
        render::PipelineSpec maskSpec;
        maskSpec.kind             = render::kBatch;
        maskSpec.fragment         = Words(outline_mask_frag, sizeof outline_mask_frag);
        maskSpec.color_formats[0] = render::kR8;
        maskSpec.depth_format     = render::kDepth32f;
        maskSpec.depth_test       = render::kDepthLessEqual;
        render::PipelineSpec edgeSpec;
        edgeSpec.kind     = render::kFullscreen;
        edgeSpec.fragment = Words(outline_edge_frag, sizeof outline_edge_frag);
        edgeSpec.blend[0] = render::kBlendAlpha;
        std::string       why;
        render::Pipeline* maskPipeline = r.CreatePipeline(maskSpec, why);
        render::Pipeline* edgePipeline = r.CreatePipeline(edgeSpec, why);
        Check(mask && maskPipeline && edgePipeline, "wxl.outline's image and programs are made");
        if (!mask || !maskPipeline || !edgePipeline || !s.Begin()) return;

        // The target over [32, 64) x [16, 48), its left half behind a nearer wall.
        draw::Batch batch;
        s.fake.State(wxl::vk::gxfront::kDepthTest, 1);
        s.fake.State(wxl::vk::gxfront::kDepthWrite, 1);
        s.Quad(32, 16, 64, 48, Argb(40, 40, 40), 0.5f, batch);
        s.fake.State(wxl::vk::gxfront::kDepthTest, 0);
        s.fake.State(wxl::vk::gxfront::kDepthWrite, 0);
        const float wall[4] = {0, 0, 0, 1};
        s.live.display.Clear(false, true, wall, 0.25f, {{0, 0}, {48, H}});

        render::PassSpec masked = Onto(mask, true);
        masked.scene_depth      = true;
        Check(r.Begin(masked) && r.DrawBatch(batch, maskPipeline, nullptr, 0), "the target's batch is drawn into the mask");
        r.End();
        struct
        {
            float    color[4];
            float    pixel[2];
            float    width;
            uint32_t mask;
            uint32_t nearest;
        } push{{1.0f, 0.8f, 0.2f, 1.0f}, {1.0f / W, 1.0f / H}, 3.0f, r.Slot(mask), r.Sampler(0)};
        Check(r.Begin(Onto(nullptr)) && r.Fullscreen(edgePipeline, &push, sizeof push), "the edge is drawn onto the frame");
        r.End();
        std::vector<uint32_t> p = s.live.End();
        if (p.empty()) return;
        Check(Near(p[32 * W + 66], Argb(255, 204, 51), 1), "the outline lies right of the seen half");
        Check(Near(p[32 * W + 56], Argb(40, 40, 40), 0), "over the target nothing is drawn");
        Check(Near(p[32 * W + 30], Argb(0, 0, 0), 0) && Near(p[14 * W + 40], Argb(0, 0, 0), 0),
              "no outline around the half behind the wall");
        Check(Near(p[14 * W + 56], Argb(255, 204, 51), 1), "and one above the seen half");
    }

    void ZoneAndDeviceCases(Setup& s)
    {
        render::Render& r = s.render;
        Zones           zones;
        r.SetZoneSink(&zones);
        if (!r.Timestamps())
            std::printf("  (the queue has no timestamps: GPU zones are not recorded)\n");
        else
        {
            if (!s.Begin()) return;
            Check(r.ZoneBegin("outer") && r.ZoneBegin("inner"), "GPU zones open in a frame");
            r.ZoneEnd();
            r.CloseZones();
            s.live.End();
            for (int i = 0; i < 3; ++i)
                if (s.Begin()) s.live.End();
            Check(zones.names.size() == 2 && zones.ordered, "GPU zones' times come back once their frame has completed, each ending after it began");
        }
        r.SetZoneSink(nullptr);

        // The device goes and comes back: the mod's image and pipeline are made again.
        render::Image*    image = r.CreateImage(render::ImageSpec{8, 8, 0, render::kRgba8, 1});
        render::Pipeline* tint  = Fullscreen(r, render_tint_frag, sizeof render_tint_frag, render::kRgba8, render::kBlendNone, 1);
        s.live.timeline.WaitFor(s.live.timeline.Last());
        r.Release();
        Check(!r.Ready() && image && tint, "the device's objects go, the mod's records stay");
        r.Create(s.live.context, s.live.allocator, s.live.timeline, s.live.display, s.live.renderer);
        if (!s.Begin()) return;
        Check(r.Slot(image) != render::kNoSlot, "an image is made again on the next device at its first use");
        Check(r.Begin(Onto(image, true)), "a pass into it begins");
        const float white[4] = {1, 1, 1, 1};
        Check(r.Fullscreen(tint, white, sizeof white), "its pipeline draws again");
        r.End();
        s.live.End();
        r.DestroyImage(image);
        r.DestroyPipeline(tint);
    }

    void OwnProgramCases()
    {
#if defined(WXL_VK_SPIRV_VAL)
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        for (const render::OwnProgram& p : render::OwnPrograms())
        {
            char what[96];
            std::snprintf(what, sizeof what, "spirv-val passes %s", p.name);
            Check(tools.Validate(p.code, p.words), what);
        }
#endif
    }
}

namespace wxl::vk::test
{
    bool RenderLiveCases()
    {
        OwnProgramCases();
        {
            Setup s;
            if (!s.Make(64, 48)) return false;
            PassCases(s);
            BufferCases(s);
            SceneCases(s);
            ZoneAndDeviceCases(s);
        }
        {
            Setup s;
            if (s.Make(64, 48, 4))
            {
                MultisampleCases(s);
                SceneCases(s);
            }
        }
        HighlightCase(1);
        HighlightCase(4);
        HighlightCase(8);
        HighlightPathCase(1);
        HighlightPathCase(8);
        OutlineExampleCase();
        return true;
    }
}
