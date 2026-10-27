// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// V7's cases: the bone palette (GxBonePaletteSet) the client's skinned programs read in the place
// of the registers from c31, past the 64 bones the registers hold, beside the renderer's own pixel
// program and in a batch drawn again; index pools of 32-bit indices; textures of 4096 and 8192;
// wxl.render's compute pipelines, dispatches and bone palettes, and a compute pipeline made again
// on the next device.

#include "gx/vk/render/Render.hpp"
#include "gx/vk/resources/Ring.hpp"
#include "gx/vk/shaders/Translate.hpp"

#include "Asm.hpp"
#include "Check.hpp"
#include "Rig.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace render = wxl::vk::render;
    namespace shaders = wxl::vk::shaders;
    namespace a      = wxl::vk::test::dx9asm;
    using namespace wxl::vk::test;
    using namespace wxl::vk::test::rig;

#include "render_bones_frag.h"
#include "render_fill_comp.h"
#include "render_flat_frag.h"
#include "render_rows_frag.h"

    std::vector<uint32_t> Words(const uint32_t* code, size_t bytes) { return std::vector<uint32_t>(code, code + bytes / 4); }

    /// vs_3_0 as the client's skinned programs read their bones: the position through c0..c3, the
    /// colour row @p row of the bone a0.x = c4.x names (c[a0.x + 31 + row]).
    std::vector<uint8_t> SkinVs(uint32_t row)
    {
        using a::Reg;
        using O = wxl::vk::shaders::dx9::Opcode;
        a::Asm v(false, 3, 0);
        v.Dcl(0, 0, a::Dst(Reg::Input, 0));
        v.Dcl(0, 0, a::Dst(Reg::Output, 0));
        v.Dcl(10, 0, a::Dst(Reg::Output, 1));
        v.Def(5, 0, 0, 0, 1);
        v.Op(O::kMov, {a::Dst(Reg::Temp, 0), a::Src(Reg::Const, 5)});
        v.Op(O::kMov, {a::Dst(Reg::Temp, 0, 0x7), a::Src(Reg::Input, 0)});
        for (uint32_t r = 0; r < 4; ++r) v.Op(O::kDp4, {a::Dst(Reg::Output, 0, 1u << r), a::Src(Reg::Temp, 0), a::Src(Reg::Const, r)});
        v.Op(O::kMova, {a::Dst(Reg::Address, 0, 1), a::Src(Reg::Const, 4, a::Swz(0, 0, 0, 0))});
        v.Op(O::kMov, {a::Dst(Reg::Output, 1), a::Src(Reg::Const, 31 + row, a::kXYZW, a::SrcMod::None, true), a::Rel(Reg::Address, 0)});
        return v.Bytes();
    }

    /// ps_3_0: the colour as it came.
    std::vector<uint8_t> PassPs()
    {
        using a::Reg;
        a::Asm p(true, 3, 0);
        p.Dcl(10, 0, a::Dst(Reg::Input, 0));
        p.Op(wxl::vk::shaders::dx9::kMov, {a::Dst(Reg::ColorOut, 0), a::Src(Reg::Input, 0)});
        return p.Bytes();
    }

    /// vs_3_0 reading c[a0.x + 8]: a relative read that is no bone.
    std::vector<uint8_t> RelativeVs()
    {
        using a::Reg;
        using O = wxl::vk::shaders::dx9::Opcode;
        a::Asm v(false, 3, 0);
        v.Dcl(0, 0, a::Dst(Reg::Input, 0));
        v.Dcl(0, 0, a::Dst(Reg::Output, 0));
        v.Op(O::kMova, {a::Dst(Reg::Address, 0, 1), a::Src(Reg::Const, 4, a::Swz(0, 0, 0, 0))});
        v.Op(O::kMov, {a::Dst(Reg::Output, 0), a::Src(Reg::Const, 8, a::kXYZW, a::SrcMod::None, true), a::Rel(Reg::Address, 0)});
        return v.Bytes();
    }

    /// A bone as the engine holds it (C34Matrix: a0 a1 a2 b0 b1 b2 c0 c1 c2 d0 d1 d2) whose row
    /// @p row (a_row b_row c_row d_row) is @p value, the others zero.
    void Bone(float* m, uint32_t row, const float value[4])
    {
        std::memset(m, 0, 48);
        m[0 + row] = value[0];
        m[3 + row] = value[1];
        m[6 + row] = value[2];
        m[9 + row] = value[3];
    }

    void PureCases()
    {
        shaders::Options    options;
        shaders::Translation t;
        std::string          why;
        const std::vector<uint8_t> skin = SkinVs(0);
        Check(shaders::Translate(skin.data(), skin.size(), options, t, why) && t.info.palette && t.info.const_rows == 5,
              "a program reading c31..c33 through a0 reads bones: its registers are the rows it reads directly (c0..c4)");
        const std::vector<uint8_t> relative = RelativeVs();
        Check(shaders::Translate(relative.data(), relative.size(), options, t, why) && !t.info.palette && t.info.const_rows == shaders::kRegisterRows,
              "a relative read that is no bone reads the whole register file");

        // 32-bit index pools: the index buffer's offset in bytes, four an index.
        FakeEngine fake;
        fake.Reset(32, 16);
        draw::Pool narrow{}, wide{};
        narrow.target = draw::kIndexPool;
        wide.target   = draw::kIndex32Pool;
        fake.pools[1] = &wide;
        fake.buffers[1].m_pool  = reinterpret_cast<engine::CGxPool*>(uintptr_t{2});
        fake.buffers[1].m_index = 8;
        fake.device.m_primIndexBuf = reinterpret_cast<engine::CGxBuf*>(&fake.buffers[1]);
        vk::gxfront::EngineView view;
        view.device           = &fake.device;
        view.states           = fake.states;
        view.vertex_constants = fake.constants;
        fake.pools[0] = &narrow; // any pool of the vertices
        draw::Packet       packet;
        const wxl_gx_batch batch{3, 1, 6, 0, 5};
        Check(vk::gxfront::BuildDraw(view, batch, 1, fake, packet) == vk::gxfront::Verdict::Draw && packet.first_index == 3 &&
                  packet.indices == &wide,
              "a 32-bit index buffer 8 bytes into its pool starts 2 indices in");
        fake.pools[1] = &narrow;
        Check(vk::gxfront::BuildDraw(view, batch, 1, fake, packet) == vk::gxfront::Verdict::Draw && packet.first_index == 5,
              "a 16-bit one 8 bytes in starts 4 indices in");
    }

    struct Bench
    {
        Live           live;
        FakeEngine     fake;
        draw::Pool*    pool = nullptr;
        uint32_t       at = 0;
        render::Render render;

        bool Make(uint32_t w, uint32_t h)
        {
            char why[256] = {};
            if (!live.Make(w, h, why, sizeof why))
            {
                std::printf("  V7 bench: %s\n", why);
                return false;
            }
            fake.Reset(w, h);
            pool          = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 1u << 16);
            fake.pools[0] = pool;
            return render.Create(live.context, live.allocator, live.timeline, live.display, live.renderer);
        }

        bool Begin()
        {
            if (!live.Begin(Argb(0, 0, 0))) return false;
            render.FrameBegun();
            return true;
        }

        /// A quad over the pixels [x0, x1) x [y0, y1), drawn as the engine draws (kept when @p kept).
        bool Draw(float x0, float y0, float x1, float y1, draw::Batch* kept = nullptr)
        {
            Vertex quad[6];
            Quad(quad, x0, y0, x1, y1, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            live.renderer.KeepBatches(kept != nullptr);
            live.renderer.ForgetLastBatch();
            const bool drawn = Record(live, fake, 6);
            live.renderer.KeepBatches(false);
            if (kept && live.renderer.LastBatch()) *kept = *live.renderer.LastBatch();
            return drawn;
        }

        ~Bench()
        {
            if (live.timeline.Ready() && live.timeline.Last()) live.timeline.WaitFor(live.timeline.Last());
            render.Destroy();
            if (pool) live.renderer.DestroyPool(pool);
        }
    };

    uint32_t Pixel(const std::vector<uint32_t>& p, const Live& live, uint32_t x, uint32_t y)
    {
        return p.size() == static_cast<size_t>(live.width) * live.height ? p[y * live.width + x] : 0u;
    }

    void PaletteCases(Bench& b)
    {
        Live&      live = b.live;
        FakeEngine& fake = b.fake;
        const float w = static_cast<float>(live.width), h = static_cast<float>(live.height);
        const std::vector<uint8_t> skin0 = SkinVs(0), skin2 = SkinVs(2), ps = PassPs();
        const int vp0 = live.renderer.Program(false, "Skin:0", skin0.data(), skin0.size());
        const int vp2 = live.renderer.Program(false, "Skin:2", skin2.data(), skin2.size());
        const int pp  = live.renderer.Program(true, "Pass", ps.data(), ps.size());
        Check(vp0 >= 0 && vp2 >= 0 && pp >= 0, "skinned client records become programs");
        if (vp0 < 0 || vp2 < 0 || pp < 0) return;

        const float projection[16] = {2.0f / w, 0, 0, -1.0f, 0, -2.0f / h, 0, 1.0f, 0, 0, 1.0f, 0, 0, 0, 0, 1.0f};
        const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1}, yellow[4] = {1, 1, 0, 1};
        auto bone = [&](float index) {
            const float row[4] = {index * 3.0f, 0, 0, 0};
            live.renderer.Constants(false, 4, 1, row);
        };
        fake.StateP(vk::gxfront::kVertexShader, &fake);
        fake.StateP(vk::gxfront::kPixelShader, &fake);
        fake.programs[1] = vp0;
        fake.programs[0] = pp;

        // Without a palette the bones are the registers' (D3D9's way): bone 2's row 0 at c37.
        live.renderer.BonePalette(nullptr, 0);
        if (b.Begin())
        {
            live.renderer.Constants(false, 0, 4, projection);
            live.renderer.Constants(false, 31 + 6, 1, red);
            bone(2);
            const bool drawn = b.Draw(0, 0, 2, 2);
            const std::vector<uint32_t> p = live.End();
            Check(drawn && Pixel(p, live, 0, 0) == Argb(255, 0, 0), "without a palette a skinned program reads its bones from c31 (red)");
        }

        // A palette of 256 bones: bone 2 blue, bone 200 green; the registers' red is no longer read.
        std::vector<float> bones(256 * 12, 0.0f);
        Bone(&bones[2 * 12], 0, blue);
        Bone(&bones[200 * 12], 0, green);
        Bone(&bones[255 * 12], 2, yellow);
        live.renderer.BonePalette(bones.data(), 256);
        Check(live.renderer.PaletteBones() == 256, "the palette holds 256 bones");
        if (b.Begin())
        {
            live.renderer.Constants(false, 0, 4, projection);
            live.renderer.Constants(false, 31 + 6, 1, red);
            bone(2);
            const bool first = b.Draw(0, 0, 2, 2);
            bone(200);
            const bool second = b.Draw(2, 0, 4, 2);
            fake.programs[1] = vp2;
            bone(255);
            const bool third = b.Draw(4, 0, 6, 2);
            fake.programs[1] = vp0;
            const std::vector<uint32_t> p = live.End();
            Check(first && Pixel(p, live, 0, 0) == Argb(0, 0, 255), "with a palette a skinned program reads bone 2 there, not at c37 (blue)");
            Check(second && Pixel(p, live, 2, 0) == Argb(0, 255, 0), "bone 200, past the 64 bones of the registers, is read (green)");
            Check(third && Pixel(p, live, 4, 0) == Argb(255, 255, 0), "row 2 of the last bone (c33 + 3 * 255) is read (yellow)");
        }

        // The palette stays from frame to frame, copied into each, as the registers do.
        if (b.Begin())
        {
            live.renderer.Constants(false, 0, 4, projection);
            bone(200);
            const bool drawn = b.Draw(0, 0, 2, 2);
            const std::vector<uint32_t> p = live.End();
            Check(drawn && Pixel(p, live, 0, 0) == Argb(0, 255, 0), "the palette set in an earlier frame is read in the next");
        }

        // Beside the renderer's own pixel program (fixed function), which reads its data at
        // Push::data: the palette goes to the pixel registers' place, and a batch drawn again
        // reads it through batch.glsl's WxlBoneRow.
        fake.programs[0] = -1;
        fake.StateP(vk::gxfront::kPixelShader, nullptr);
        render::PipelineSpec spec;
        spec.kind             = render::kBatch;
        spec.fragment         = Words(render_bones_frag, sizeof render_bones_frag);
        spec.color_formats[0] = render::kBgra8;
        spec.cull             = render::kCullBatch;
        std::string       why;
        render::Pipeline* bonesProgram = b.render.CreatePipeline(spec, why);
        Check(bonesProgram != nullptr, "a batch program reading the batch's bones is made");
        if (b.Begin())
        {
            live.renderer.Constants(false, 0, 4, projection);
            bone(200);
            draw::Batch kept;
            const bool  drawn = b.Draw(0, 0, 2, 2, &kept);
            Check(drawn && (kept.push.flags & shaders::kFlagPaletteAtPixel) && ((kept.push.flags >> shaders::kFlagBonesShift) & shaders::kFlagBonesMask) == 256,
                  "beside the fixed-function pixel program the palette is at the pixel registers' place, its bones in the flags");
            const bool beside = b.Draw(2, 0, 4, 2);
            render::PassSpec pass;
            pass.color_count    = 1;
            pass.scene_color[0] = true;
            const float which[4] = {2.0f, 0, 0, 0};
            const bool  again    = bonesProgram && b.render.Begin(pass) && b.render.DrawBatch(kept, bonesProgram, which, sizeof which);
            if (b.render.InPass()) b.render.End();
            const std::vector<uint32_t> p = live.End();
            Check(beside && Pixel(p, live, 2, 0) == Argb(0, 255, 0), "the fixed-function pixel program draws the colour the palette's bone gave (green)");
            Check(again && Pixel(p, live, 0, 0) == Argb(0, 0, 255), "the batch drawn again reads bone 2 of its palette through WxlBoneRow (blue)");
        }
        if (bonesProgram) b.render.DestroyPipeline(bonesProgram);
        live.renderer.BonePalette(nullptr, 0);
        fake.StateP(vk::gxfront::kVertexShader, nullptr);
        fake.programs[0] = fake.programs[1] = -1;
    }

    void Index32Cases(Bench& b)
    {
        Live&       live = b.live;
        FakeEngine& fake = b.fake;
        // 70000 vertices: the quad at vertices 65536.. (past 16 bits) over the pixels [0, 2) x [0, 2), magenta.
        constexpr uint32_t kVertices = 70000;
        draw::Pool* vertices = live.renderer.CreatePool(draw::kVertexPool, draw::kDynamic, kVertices * sizeof(Vertex));
        draw::Pool* indices  = live.renderer.CreatePool(draw::kIndex32Pool, draw::kDynamic, 6 * sizeof(uint32_t));
        uint8_t*    vbytes   = vertices ? live.renderer.PoolMemory(vertices) : nullptr;
        uint8_t*    ibytes   = indices ? live.renderer.PoolMemory(indices) : nullptr;
        Check(vbytes && ibytes, "a vertex pool of 70000 vertices and a 32-bit index pool are made");
        if (!vbytes || !ibytes) return;
        std::memset(vbytes, 0, kVertices * sizeof(Vertex));
        Vertex quad[6];
        Quad(quad, 0, 0, 2, 2, Argb(255, 0, 255));
        std::memcpy(vbytes + 65536 * sizeof(Vertex), quad, sizeof quad);
        const uint32_t idx[6] = {65536, 65537, 65538, 65539, 65540, 65541};
        std::memcpy(ibytes, idx, sizeof idx);
        live.renderer.PoolWritten(vertices, 0, kVertices * sizeof(Vertex));
        live.renderer.PoolWritten(indices, 0, sizeof idx);

        fake.pools[0]           = vertices;
        fake.buffers[0].m_index = 0;
        fake.pools[1]           = indices;
        fake.buffers[1].m_pool  = reinterpret_cast<engine::CGxPool*>(uintptr_t{2});
        fake.buffers[1].m_index = 0;
        fake.device.m_primIndexBuf = reinterpret_cast<engine::CGxBuf*>(&fake.buffers[1]);
        if (b.Begin())
        {
            vk::gxfront::EngineView view;
            view.device           = &fake.device;
            view.states           = fake.states;
            view.vertex_constants = fake.constants;
            draw::Packet       packet;
            const wxl_gx_batch batch{3, 0, 6, 0, 0};
            live.renderer.KeepBatches(true);
            live.renderer.ForgetLastBatch();
            const bool drawn = vk::gxfront::BuildDraw(view, batch, 1, fake, packet) == vk::gxfront::Verdict::Draw && live.renderer.Draw(packet);
            live.renderer.KeepBatches(false);
            const std::vector<uint32_t> p = live.End();
            Check(drawn && Pixel(p, live, 0, 0) == Argb(255, 0, 255) && Pixel(p, live, 1, 1) == Argb(255, 0, 255) &&
                      Pixel(p, live, 3, 3) == Argb(0, 0, 0),
                  "32-bit indices past vertex 65535 draw the quad they name (magenta)");
            Check(live.renderer.LastBatch() && live.renderer.LastBatch()->index32, "the batch kept of a 32-bit index draw says so");
        }
        // Kept and drawn again with a batch program, as wxl.highlight and wxl.render draw it: its 32-bit indices again.
        render::PipelineSpec spec;
        spec.kind             = render::kBatch;
        spec.fragment         = Words(render_flat_frag, sizeof render_flat_frag);
        spec.color_formats[0] = render::kBgra8;
        spec.cull             = render::kCullBatch;
        std::string       why;
        render::Pipeline* flat = b.render.CreatePipeline(spec, why);
        if (flat && b.Begin())
        {
            vk::gxfront::EngineView view;
            view.device           = &fake.device;
            view.states           = fake.states;
            view.vertex_constants = fake.constants;
            draw::Packet       packet;
            const wxl_gx_batch batch{3, 0, 6, 0, 0};
            live.renderer.KeepBatches(true);
            live.renderer.ForgetLastBatch();
            const bool drawn = vk::gxfront::BuildDraw(view, batch, 1, fake, packet) == vk::gxfront::Verdict::Draw && live.renderer.Draw(packet);
            live.renderer.KeepBatches(false);
            const draw::Batch* kept = live.renderer.LastBatch();
            render::PassSpec   pass;
            pass.color_count    = 1;
            pass.scene_color[0] = true;
            const float cyan[4] = {0, 1, 1, 1};
            const bool  again   = drawn && kept && b.render.Begin(pass) && b.render.DrawBatch(*kept, flat, cyan, sizeof cyan);
            if (b.render.InPass()) b.render.End();
            const std::vector<uint32_t> p = live.End();
            Check(again && Pixel(p, live, 0, 0) == Argb(0, 255, 255) && Pixel(p, live, 3, 3) == Argb(0, 0, 0),
                  "a batch of 32-bit indices is drawn again by a batch program where it was drawn (cyan)");
        }
        if (flat) b.render.DestroyPipeline(flat);
        fake.pools[0]              = b.pool;
        fake.pools[1]              = nullptr;
        fake.device.m_primIndexBuf = nullptr;
        live.renderer.DestroyPool(vertices);
        live.renderer.DestroyPool(indices);
    }

    /// A texture of @p size x @p size: its 4x4 texels at (size - 4, size - 4) written with @p bytes
    /// (a block when compressed), drawn on one pixel with nearest sampling at that corner.
    bool LargeTexture(Bench& b, uint32_t format, uint32_t size, const void* bytes, uint32_t pitch, uint32_t& seen)
    {
        Live&       live = b.live;
        FakeEngine& fake = b.fake;
        draw::TextureDesc desc;
        desc.format         = format;
        desc.width          = size;
        desc.height         = size;
        draw::Texture* t    = live.renderer.CreateTexture(desc);
        if (!t || !b.Begin()) return false;
        uint32_t to_pitch = 0;
        uint8_t* to       = live.renderer.LockTexture(t, 0, 0, size - 4, size - 4, 4, 4, to_pitch);
        if (to)
        {
            const uint32_t rows = draw::Formats::Rows(t->info, 4);
            for (uint32_t r = 0; r < rows; ++r) std::memcpy(to + r * to_pitch, static_cast<const uint8_t*>(bytes) + r * pitch, pitch);
            live.renderer.UnlockTexture(t);
        }
        fake.textures[0]                    = t;
        fake.tex_fields[0].m_flags          = {};
        fake.tex_fields[0].m_flags.m_filter = 0;
        fake.StateP(vk::gxfront::kTexture0, &fake.tex_fields[0]);
        fake.State(vk::gxfront::kColorOp0, 0);
        fake.State(vk::gxfront::kAlphaOp0, 0);
        const float u = (static_cast<float>(size) - 2.5f) / static_cast<float>(size);
        Vertex      quad[6];
        Quad(quad, 0, 0, 1, 1, 0xFFFFFFFFu, u, u, u, u);
        Stream(live, fake, b.pool, quad, 6, b.at);
        const bool drawn = Record(live, fake, 6);
        const std::vector<uint32_t> p = live.End();
        seen = Pixel(p, live, 0, 0);
        fake.StateP(vk::gxfront::kTexture0, nullptr);
        fake.textures[0] = nullptr;
        live.renderer.DestroyTexture(t);
        return to && drawn;
    }

    void TextureCases(Bench& b)
    {
        uint32_t texels[16];
        for (uint32_t& t : texels) t = Argb(0, 200, 100);
        uint32_t seen = 0;
        Check(LargeTexture(b, draw::kTexArgb8888, 4096, texels, 16, seen) && Near(seen, Argb(0, 200, 100), 1),
              "a 4096 x 4096 texture is made, written at its far corner and sampled there");
        if (!Environment("WXL_VK_NO_BC") && b.live.renderer.TextureFormats().Of(draw::kTexDxt1).format != VK_FORMAT_UNDEFINED)
        {
            // One DXT1 block, all of it colour 0 (pure red).
            const uint8_t block[8] = {0x00, 0xF8, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00};
            Check(LargeTexture(b, draw::kTexDxt1, 8192, block, 8, seen) && Near(seen, Argb(255, 0, 0), 1),
                  "an 8192 x 8192 DXT1 texture is made, written at its far corner and sampled there");
        }
    }

    void ComputeCases(Bench& b)
    {
        render::Render& r = b.render;
        Live&           live = b.live;
        std::string     why;
        render::PipelineSpec fill;
        fill.kind    = render::kCompute;
        fill.compute = Words(render_fill_comp, sizeof render_fill_comp);
        render::Pipeline* filler = r.CreatePipeline(fill, why);
        Check(filler != nullptr, "a compute pipeline is made");
        render::PipelineSpec wrong = fill;
        wrong.fragment = Words(render_rows_frag, sizeof render_rows_frag);
        Check(!r.CreatePipeline(wrong, why) && !why.empty(), "a compute pipeline with a fragment program is refused");
        render::PipelineSpec rowsSpec;
        rowsSpec.kind             = render::kFullscreen;
        rowsSpec.fragment         = Words(render_rows_frag, sizeof render_rows_frag);
        rowsSpec.color_formats[0] = render::kBgra8;
        render::Pipeline* rows    = r.CreatePipeline(rowsSpec, why);
        render::Buffer*   buffer  = r.CreateBuffer(256 * 16);
        if (!filler || !rows || !buffer) return;

        struct FillPush
        {
            uint64_t rows;
            uint32_t count;
            uint32_t pad;
            float    color[4];
        };
        struct RowsPush
        {
            uint64_t rows;
            uint32_t index;
        };
        // A dispatch fills the buffer, a pass draws one of its rows: (17, 128, 64).
        if (b.Begin())
        {
            const FillPush push{r.Address(buffer), 256, 0, {0.0f, 128.0f / 255.0f, 64.0f / 255.0f, 1.0f}};
            const bool dispatched = r.Dispatch(filler, 4, 1, 1, &push, sizeof push, &why);
            render::PassSpec pass;
            pass.color_count    = 1;
            pass.scene_color[0] = true;
            Check(r.Begin(pass), "a pass after a dispatch");
            Check(!r.Dispatch(filler, 4, 1, 1, &push, sizeof push), "a dispatch inside a pass is refused");
            Check(!r.Draw(filler, 3, 1, nullptr, 0), "a compute pipeline does not draw");
            const RowsPush read{r.Address(buffer), 17};
            const bool     drawn = r.Fullscreen(rows, &read, sizeof read);
            r.End();
            Check(!r.Dispatch(rows, 1, 1, 1, nullptr, 0), "a fullscreen pipeline is not dispatched");
            const std::vector<uint32_t> p = live.End();
            Check(dispatched && drawn && Near(Pixel(p, live, 5, 5), Argb(17, 128, 64), 1),
                  "what a dispatch writes through an address is read by the pass after it");
        }

        // A mod's bone palette: 300 matrices (past 256), matrix 299's row 2 read by address.
        if (b.Begin())
        {
            std::vector<float> matrices(300 * 12, 0.0f);
            const float last[4] = {0.25f, 0.5f, 0.75f, 1.0f};
            std::memcpy(&matrices[299 * 12 + 8], last, sizeof last);
            VkDeviceAddress address = 0;
            const bool      written = r.BonePalette(matrices.data(), 300, address);
            render::PassSpec pass;
            pass.color_count    = 1;
            pass.scene_color[0] = true;
            const RowsPush read{address, 299 * 3 + 2};
            const bool     drawn = r.Begin(pass) && r.Fullscreen(rows, &read, sizeof read);
            if (r.InPass()) r.End();
            const std::vector<uint32_t> p = live.End();
            Check(written && address != 0 && drawn && Near(Pixel(p, live, 3, 3), Argb(64, 128, 191), 1),
                  "a mod's palette of 300 matrices is read by address, its last matrix's last row");
        }
        VkDeviceAddress none = 1;
        Check(!r.BonePalette(nullptr, 1, none) && none == 0, "a palette of nothing is refused");

        // The device goes and comes back: the compute pipeline is made again at its next dispatch.
        live.timeline.WaitFor(live.timeline.Last());
        r.Release();
        r.Create(live.context, live.allocator, live.timeline, live.display, live.renderer);
        if (b.Begin())
        {
            const FillPush push{r.Address(buffer), 256, 0, {0.0f, 1.0f, 1.0f, 1.0f}};
            const bool     again = r.Dispatch(filler, 4, 1, 1, &push, sizeof push);
            render::PassSpec pass;
            pass.color_count    = 1;
            pass.scene_color[0] = true;
            const RowsPush read{r.Address(buffer), 255};
            const bool     drawn = r.Begin(pass) && r.Fullscreen(rows, &read, sizeof read);
            if (r.InPass()) r.End();
            const std::vector<uint32_t> p = live.End();
            Check(again && drawn && Near(Pixel(p, live, 1, 1), Argb(255, 255, 255), 1), "a compute pipeline dispatches again on the next device");
        }
        r.DestroyBuffer(buffer);
        r.DestroyPipeline(rows);
        r.DestroyPipeline(filler);
        Check(r.Counters().dispatches >= 2 && r.Counters().palettes >= 1, "dispatches and palettes are counted");
    }
}

namespace
{
    /// V8: a ring's chunks past a slot's first that stay unused are given back (address space on
    /// the 32-bit client), the first never; VMA's blocks are smaller in a 32-bit process.
    void RingTrimCases(Bench& b)
    {
        namespace res = wxl::vk::resources;
        res::Ring ring;
        Check(ring.Create(b.live.allocator, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1u << 16, 2), "a ring of 64 KiB chunks over two slots");
        res::Ring::Span span;
        ring.Begin(0);
        bool grew = true;
        for (int i = 0; i < 3; ++i) grew = grew && ring.Allocate(1u << 16, 16, span);
        Check(grew && ring.Counters().chunks == 3 && ring.Counters().mapped == 3u << 16, "a busy frame grows its slot to three mapped chunks");
        for (uint32_t frame = 1; frame <= res::Ring::kIdleFrames; ++frame)
        {
            ring.Begin(0);
            ring.Allocate(256, 16, span);
        }
        Check(ring.Counters().chunks == 3 && !ring.Counters().trimmed, "chunks unused for fewer frames than the limit stay");
        ring.Begin(0);
        Check(ring.Counters().chunks == 1 && ring.Counters().trimmed == 2 && ring.Counters().mapped == 1u << 16,
              "past the limit the slot keeps its first chunk only, the others given back");
        Check(ring.Allocate(1u << 16, 16, span) && span.data, "and grows again when a frame needs it");
        ring.Destroy();
        Check(res::kBlockSize == (sizeof(void*) == 4 ? (VkDeviceSize{32} << 20) : (VkDeviceSize{256} << 20)),
              "VMA's blocks are 32 MiB in a 32-bit process, 256 MiB otherwise");
    }
}

namespace wxl::vk::test
{
    bool V7LiveCases()
    {
        PureCases();
        Bench b;
        if (!b.Make(16, 8)) return false;
        PaletteCases(b);
        Index32Cases(b);
        TextureCases(b);
        ComputeCases(b);
        RingTrimCases(b);
        return true;
    }
}
