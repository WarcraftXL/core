// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Where the renderer follows the D3D9 device to the pixel, on the machine's driver:
//
//   - the software cursor as the engine draws it into the back buffer (CGxDevice::ICursorDraw:
//     a 32x32 quad corrected by half a pixel, linear filter, alpha key at 224), read back by the
//     screenshot's path texel for texel, with one sample per pixel and multisampled;
//   - an overlay (Dear ImGui) drawn between the engine's draws and the cursor, multisampled: the
//     cursor drawn after it lies over it and the resolve keeps both, as in D3D9's back buffer;
//   - GxRs_Multisample 0 (D3DRS_MULTISAMPLEANTIALIAS off) in a multisampled scene: where the
//     device has sample locations, every sample of a pixel takes its centre's coverage, so the
//     edge is as hard as with one sample (D3D9's rule, computed here); where it has none, the draw
//     stays antialiased and is counted; into a texture (one sample) it changes nothing.

#include "gx/vk/draw/Draw.hpp"
#include "gx/vk/gxfront/Engine.hpp"
#include "gx/vk/pipelines/Pipelines.hpp"
#include "gx/vk/present/Display.hpp"

#include "Check.hpp"
#include "Rig.hpp"

#include <cstdio>
#include <vector>

namespace
{
    using namespace wxl::vk::test::rig;
    using wxl::vk::test::Check;
    namespace pipelines = wxl::vk::pipelines;

    constexpr uint32_t kTexArgb8888 = 2; // EGxTexFormat GxTex_Argb8888
    constexpr uint32_t kLinear      = 1; // EGxTexFilter GxTex_Linear
    constexpr int32_t  kAlphaKey    = 1; // EGxBlend GxBlend_AlphaKey

    /// The cursor's 32x32 pixels: an opaque magenta left half, a yellow column at the alpha key
    /// exactly (224), a cyan one just under it (223), the rest transparent white.
    std::vector<uint32_t> CursorPixels()
    {
        std::vector<uint32_t> px(32 * 32);
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 32; ++x)
                px[y * 32 + x] = x < 16    ? Argb(255, 0, 255)
                                 : x == 16 ? Argb(255, 255, 0, 224)
                                 : x == 17 ? Argb(0, 255, 255, 223)
                                           : Argb(255, 255, 255, 0);
        return px;
    }

    /// What CGxDevice::ICursorDraw sets before its quad, as the fake engine's state.
    void CursorState(FakeEngine& fake, draw::Texture* texture)
    {
        fake.textures[0]                    = texture;
        fake.tex_fields[0].m_flags.m_filter = kLinear;
        fake.StateP(vk::gxfront::kTexture0, &fake.tex_fields[0]);
        fake.State(vk::gxfront::kColorOp0, 0); // GxTexOp_Mod with the vertex's white
        fake.State(vk::gxfront::kAlphaOp0, 0);
        fake.State(vk::gxfront::kBlendingMode, kAlphaKey);
        fake.State(vk::gxfront::kAlphaRef, 224); // GxUpdateAlphaRef for the alpha key
    }

    /// The cursor's quad with its top left corner on pixel (x, y): D3D9's half-pixel correction
    /// puts its corners on the pixels' edges, so each texel lands on one pixel's centre.
    void CursorQuad(Vertex* quad, int32_t x, int32_t y)
    {
        Quad(quad, static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + 32), static_cast<float>(y + 32), Argb(255, 255, 255));
    }

    void CursorCases(uint32_t samples)
    {
        char why[256] = {};
        Live live;
        if (!live.Make(64, 48, why, sizeof why))
        {
            Check(false, "a renderer for the cursor cases");
            return;
        }
        if (samples > 1)
        {
            if (!(live.display.SampleCounts() & samples))
            {
                std::printf("  (the device draws no %u-sample scene: the multisampled cursor not checked)\n", samples);
                return;
            }
            live.display.SetSamples(samples);
        }
        FakeEngine fake;
        fake.Reset(live.width, live.height);
        draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
        fake.pools[0]    = pool;
        uint32_t at      = 0;
        const uint32_t grey = Argb(90, 90, 90);
        if (!live.Begin(grey))
        {
            Check(false, "a frame for the cursor cases");
            return;
        }
        const std::vector<uint32_t> cursor  = CursorPixels();
        draw::Texture*              texture = MakeTexture(live, kTexArgb8888, 32, 32, cursor.data(), 32 * 4);
        CursorState(fake, texture);
        Vertex quad[6];
        CursorQuad(quad, 10, 4);
        Stream(live, fake, pool, quad, 6, at);
        const bool drawn = texture && Record(live, fake, 6);

        // The screenshot: ScenePresent draws the cursor, then DeviceScreenShot reads the back buffer.
        std::vector<uint32_t> shot(64 * 48, 0);
        const bool read = live.display.ReadScene({{0, 0}, {64, 48}}, shot.data());
        const std::vector<uint32_t> shown = live.End();

        bool exact = drawn && read;
        for (uint32_t y = 0; exact && y < 48; ++y)
            for (uint32_t x = 0; exact && x < 64; ++x)
            {
                uint32_t want = grey;
                if (x >= 10 && x < 42 && y >= 4 && y < 36)
                {
                    const uint32_t texel = cursor[(y - 4) * 32 + (x - 10)];
                    if ((texel >> 24) >= 224) want = texel;
                }
                exact = Near(shot[y * 64 + x], want, 0, false);
                if (!exact) std::printf("  pixel (%u,%u): %08x, D3D9 %08x\n", x, y, shot[y * 64 + x], want);
            }
        char what[200];
        std::snprintf(what, sizeof what,
                      "the screenshot holds the software cursor texel for texel, alpha 224 kept and 223 keyed out (%u sample(s))", samples);
        Check(exact, what);
        bool same = shown.size() == shot.size();
        for (size_t i = 0; same && i < shot.size(); ++i) same = Near(shown[i], shot[i], 0, false);
        Check(same, "the frame shown is the screenshot: the cursor is in the scene, not drawn over it later");
        if (texture) live.renderer.DestroyTexture(texture);
        live.renderer.DestroyPool(pool);
    }

    void OverlayCases()
    {
        char why[256] = {};
        Live live;
        if (!live.Make(16, 8, why, sizeof why))
        {
            Check(false, "a renderer for the overlay cases");
            return;
        }
        if (!(live.display.SampleCounts() & 4))
        {
            std::printf("  (the device draws no 4-sample scene: the overlay not checked)\n");
            return;
        }
        live.display.SetSamples(4);
        FakeEngine fake;
        fake.Reset(live.width, live.height);
        draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
        fake.pools[0]    = pool;
        uint32_t at      = 0;
        if (!live.Begin(Argb(0, 0, 0)))
        {
            Check(false, "a frame for the overlay cases");
            return;
        }
        Vertex quad[6];
        Quad(quad, 0, 0, 16, 8, Argb(255, 0, 0));
        Stream(live, fake, pool, quad, 6, at);
        const bool world = Record(live, fake, 6);

        // The overlay: a green rectangle into the scene's colour (what Dear ImGui's rendering gets).
        const VkCommandBuffer cmd = live.display.BeginColorRendering();
        VkClearAttachment     green{};
        green.aspectMask                  = VK_IMAGE_ASPECT_COLOR_BIT;
        green.clearValue.color.float32[1] = 1.0f;
        green.clearValue.color.float32[3] = 1.0f;
        const VkClearRect rect{{{2, 2}, {4, 4}}, 0, 1};
        if (cmd) vkCmdClearAttachments(cmd, 1, &green, 1, &rect);
        live.display.EndRendering();

        // The cursor after it, an engine draw into the scene.
        Quad(quad, 4, 4, 12, 8, Argb(0, 0, 255));
        Stream(live, fake, pool, quad, 6, at);
        const bool after = Record(live, fake, 6);

        std::vector<uint32_t> shot(16 * 8, 0);
        const bool read = live.display.ReadScene({{0, 0}, {16, 8}}, shot.data());
        const std::vector<uint32_t> shown = live.End();
        Check(world && cmd && after && read && shot[3 * 16 + 3] == Argb(0, 255, 0) && shot[5 * 16 + 5] == Argb(0, 0, 255) &&
                  shot[5 * 16 + 10] == Argb(0, 0, 255) && shot[1 * 16 + 10] == Argb(255, 0, 0),
              "multisampled, an overlay and the cursor drawn after it both reach the screenshot, the cursor over the overlay");
        Check(shown == shot, "and the frame shown");
        live.renderer.DestroyPool(pool);
    }

    void MultisampleOffCases()
    {
        char why[256] = {};
        Live live;
        if (!live.Make(16, 8, why, sizeof why))
        {
            Check(false, "a renderer for GxRs_Multisample");
            return;
        }
        if (!(live.display.SampleCounts() & 4))
        {
            std::printf("  (the device draws no 4-sample scene: GxRs_Multisample not checked)\n");
            return;
        }
        live.display.SetSamples(4);
        FakeEngine fake;
        fake.Reset(live.width, live.height);
        draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
        fake.pools[0]    = pool;
        uint32_t at      = 0;

        // A red triangle whose long edge crosses pixels: x + 2y < 14.5 in D3D9's pixel centres,
        // no centre on an edge.
        const Vertex tri[3] = {{-0.5f, -0.5f, 0.5f, Argb(255, 0, 0), 0, 0},
                               {15.5f, -0.5f, 0.5f, Argb(255, 0, 0), 0, 0},
                               {-0.5f, 7.5f, 0.5f, Argb(255, 0, 0), 0, 0}};
        auto frame = [&](int32_t multisample, std::vector<uint32_t>& out) {
            if (!live.Begin(Argb(0, 0, 0))) return false;
            fake.State(vk::gxfront::kMultisample, multisample);
            Stream(live, fake, pool, tri, 3, at);
            const bool drawn = Record(live, fake, 3);
            out              = live.End();
            return drawn && out.size() == 16 * 8;
        };
        std::vector<uint32_t> on, off;
        const bool drewOn  = frame(1, on);
        const draw::Renderer::Stats before = live.renderer.Counters();
        const size_t pipelinesOn = live.renderer.PipelineCache().Count();
        const bool drewOff = frame(0, off);
        const draw::Renderer::Stats after = live.renderer.Counters();

        uint32_t partial = 0;
        for (uint32_t v : on)
        {
            const uint32_t red = (v >> 16) & 0xFF;
            partial += red > 16 && red < 240;
        }
        Check(drewOn && partial >= 4, "GxRs_Multisample 1: the edge is antialiased");

        if (live.display.CentreSamples())
        {
            bool rule = drewOff;
            for (uint32_t y = 0; rule && y < 8; ++y)
                for (uint32_t x = 0; rule && x < 16; ++x)
                {
                    const uint32_t want = static_cast<float>(x) + 2.0f * static_cast<float>(y) < 14.5f ? Argb(255, 0, 0) : Argb(0, 0, 0);
                    rule                = off[y * 16 + x] == want;
                    if (!rule) std::printf("  pixel (%u,%u): %08x, D3D9 %08x\n", x, y, off[y * 16 + x], want);
                }
            Check(rule, "GxRs_Multisample 0: each pixel all or nothing by its centre, as D3D9's MULTISAMPLEANTIALIAS off");
            Check(after.centred == before.centred + 1 && after.antialiased == before.antialiased &&
                      live.renderer.PipelineCache().Count() == pipelinesOn + 1,
                  "drawn with its samples on the pixel's centre, a pipeline of its own");
        }
        else
        {
            std::printf("  (the device has no sample locations: GxRs_Multisample 0 is drawn antialiased, its fallback)\n");
            Check(drewOff && off == on, "without sample locations, GxRs_Multisample 0 draws as 1 (antialiased)");
            Check(after.antialiased == before.antialiased + 1 && after.centred == before.centred &&
                      live.renderer.PipelineCache().Count() == pipelinesOn,
                  "and is counted as such, with the same pipeline");
        }

        // Into a texture (one sample) the state changes nothing, as on D3D9: no count either way.
        draw::TextureDesc desc;
        desc.width             = 4;
        desc.height            = 4;
        desc.target            = true;
        draw::Texture* target  = live.renderer.CreateTexture(desc);
        if (target && live.Begin(Argb(0, 0, 0)))
        {
            fake.device.m_textureTarget[0].m_texture = reinterpret_cast<engine::CGxTex*>(&fake.tex_fields[1]);
            fake.device.m_curWindowRect              = {0.0f, 0.0f, 4.0f, 4.0f};
            float* p                                 = &fake.device.m_projNative.a0;
            p[0]                                     = 2.0f / 4.0f;
            p[5]                                     = -2.0f / 4.0f;
            live.renderer.SetTarget(target, 0, nullptr);
            fake.State(vk::gxfront::kMultisample, 0);
            Vertex quad[6];
            Quad(quad, 0, 0, 4, 4, Argb(0, 0, 255));
            Stream(live, fake, pool, quad, 6, at);
            const draw::Renderer::Stats was = live.renderer.Counters();
            const bool drawn = Record(live, fake, 6);
            live.renderer.SetTarget(nullptr, 0, nullptr);
            live.End();
            const draw::Renderer::Stats now = live.renderer.Counters();
            Check(drawn && now.centred == was.centred && now.antialiased == was.antialiased,
                  "into a texture target (one sample) GxRs_Multisample 0 changes nothing");
            fake.device.m_textureTarget[0].m_texture = nullptr;
            live.renderer.DestroyTexture(target);
        }
        live.renderer.DestroyPool(pool);
    }
}

namespace wxl::vk::test
{
    void ParityPureCases()
    {
        FakeEngine fake;
        fake.Reset(16, 8);
        draw::Pool pool;
        fake.pools[0] = &pool;
        vk::gxfront::EngineView view;
        view.device           = &fake.device;
        view.states           = fake.states;
        view.vertex_constants = fake.constants;
        draw::Packet       packet;
        const wxl_gx_batch batch{3, 0, 3, 0, 2};
        Check(vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Draw && !packet.multisample_off,
              "GxRs_Multisample 1 (the engine's default): multisampled as the target is");
        fake.State(vk::gxfront::kMultisample, 0);
        Check(vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Draw && packet.multisample_off,
              "GxRs_Multisample 0 reaches the draw");
        pipelines::Key key;
        const uint64_t plain = key.Packed();
        key.centre           = true;
        Check(key.Packed() != plain && (key.Packed() ^ plain) == 1ull << 60, "the samples on the centre are a bit of the pipeline's key");
    }

    bool ParityLiveCases()
    {
        CursorCases(1);
        CursorCases(4);
        OverlayCases();
        MultisampleOffCases();
        return true;
    }
}
