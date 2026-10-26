// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The engine's draws through the renderer (src/gx/vk/draw/, gxfront/Engine.cpp), on the machine's
// driver, read back and compared with what D3D9 would show:
//
//   - the pure tables: D3D9's blend modes, samplers, format layouts, the engine reading's verdicts,
//     its viewport, scissor, colour mask and texture stages;
//   - each texture format the engine creates, uploaded in D3D9's layout and drawn texel for texel;
//   - the thirteen blend modes against D3D9's formula, the alpha test, the colour mask, the scissor;
//   - fixed function's stage cascade (the six ops on stages 0 and 1), the interface's programs
//     "UI" and "Desaturate", D3D9's pixel centres;
//   - a stream pool renamed when the engine discards it inside a frame, a static pool and a
//     texture written again after a draw read them in the same frame (the copy goes between the
//     draws), mips made from the first level, a cube's first face;
//   - a frame capture made up in the client's format, drawn again and compared with its SHOT;
//   - V3's world: client programs translated from their DX9 records with the engine's registers
//     copied as they change between draws, a texture target drawn into and sampled, a clear into it,
//     a copy from the scene, an occlusion query counting samples, D3D9's fixed-function lighting,
//     fog and user clip plane; a capture with translated programs, a target and a copy replayed.

#include "gx/capture/Replay.hpp"
#include "gx/vk/Log.hpp"
#include "gx/vk/draw/Draw.hpp"
#include "gx/vk/gxfront/Engine.hpp"
#include "gx/vk/present/Display.hpp"

#include "Asm.hpp"
#include "Check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    namespace vk     = wxl::vk;
    namespace draw   = wxl::vk::draw;
    namespace engine = wxl::engine;
    using wxl::vk::test::Check;

    uint32_t Argb(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) { return a << 24 | r << 16 | g << 8 | b; }

    bool Near(uint32_t a, uint32_t b, uint32_t tolerance, bool alpha = true)
    {
        for (int shift = 0; shift < (alpha ? 32 : 24); shift += 8)
        {
            const int32_t d = static_cast<int32_t>((a >> shift) & 0xFF) - static_cast<int32_t>((b >> shift) & 0xFF);
            if (static_cast<uint32_t>(d < 0 ? -d : d) > tolerance) return false;
        }
        return true;
    }

    // --- Pure cases -------------------------------------------------------------------------------

    void BlendTableCases()
    {
        using vk::pipelines::BlendOf;
        Check(!BlendOf(0).enable && !BlendOf(1).enable, "Opaque and AlphaKey do not blend");
        Check(BlendOf(2).source == VK_BLEND_FACTOR_SRC_ALPHA && BlendOf(2).destination == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
              "Alpha is SRCALPHA, INVSRCALPHA");
        Check(BlendOf(5).source == VK_BLEND_FACTOR_DST_COLOR && BlendOf(5).destination == VK_BLEND_FACTOR_SRC_COLOR,
              "Mod2x is DESTCOLOR, SRCCOLOR");
        Check(BlendOf(11).source == VK_BLEND_FACTOR_CONSTANT_COLOR && BlendOf(11).destination == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
              "ConstantAlpha is BLENDFACTOR, INVBLENDFACTOR");
        Check(BlendOf(12).source == VK_BLEND_FACTOR_ONE && BlendOf(12).destination == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
              "the fork's BlendAdd is ONE, INVSRCALPHA");
        Check(!BlendOf(99).enable, "a mode outside the table is opaque");
    }

    void SamplerCases()
    {
        vk::resources::SamplerKey key;
        key.filter                     = 0;
        VkSamplerCreateInfo nearest    = vk::resources::SamplerInfo(key, 16.0f);
        Check(nearest.magFilter == VK_FILTER_NEAREST && nearest.maxLod < 1.0f, "GxTex_Nearest: point, the first level only (MIPFILTER NONE)");
        key.filter                     = 4;
        key.wrap_u                     = 1;
        VkSamplerCreateInfo trilinear  = vk::resources::SamplerInfo(key, 16.0f);
        Check(trilinear.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR && trilinear.maxLod == VK_LOD_CLAMP_NONE &&
                  trilinear.addressModeU == VK_SAMPLER_ADDRESS_MODE_REPEAT && trilinear.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
              "GxTex_LinearMipLinear: trilinear, wrap on u only");
        key.filter     = 5;
        key.anisotropy = 8;
        Check(vk::resources::SamplerInfo(key, 4.0f).maxAnisotropy == 4.0f && vk::resources::SamplerInfo(key, 0.0f).anisotropyEnable == VK_FALSE,
              "anisotropy capped by the device, off without the feature");
        Check(key.Packed() != vk::resources::SamplerKey{}.Packed(), "sampler keys pack apart");
    }

    void FormatPitchCases()
    {
        draw::FormatInfo dxt;
        dxt.block = 4;
        dxt.bytes = 8;
        Check(draw::Formats::Pitch(dxt, 13) == 32 && draw::Formats::Rows(dxt, 5) == 2, "a DXT1 row of 13 texels is 4 blocks, 5 rows 2 block rows");
        draw::FormatInfo argb;
        Check(draw::Formats::Pitch(argb, 7) == 28 && draw::Formats::Rows(argb, 3) == 3, "an ARGB8888 row is 4 bytes a texel");
        uint16_t program = 0;
        Check(draw::ProgramByName(true, "UI:0", program) && program == static_cast<uint16_t>(vk::pipelines::VertexProgram::Interface),
              "the client's UI:0 vertex program is the interface's");
        Check(draw::ProgramByName(true, "UI:1", program) && program == static_cast<uint16_t>(vk::pipelines::VertexProgram::InterfaceStereo),
              "UI:1 is the stereo one");
        Check(draw::ProgramByName(false, "Desaturate", program) && program == static_cast<uint16_t>(vk::pipelines::PixelProgram::Desaturate),
              "Desaturate is the renderer's");
        Check(!draw::ProgramByName(false, "Terrain3:17", program) && !draw::ProgramByName(true, "UI", program),
              "the client's other programs are not (UI as a vertex program has permutations)");
    }

    // --- A device of the engine, made up -----------------------------------------------------------

    /// What a test reads the engine through: buffers, pools, textures and programs by their made-up pointers.
    struct FakeEngine final : vk::gxfront::Resolver
    {
        engine::CGxDevice   device{};
        wxl_gx_app_state    states[vk::gxfront::kRenderStates]{};
        float               constants[256 * 4] = {};
        engine::CGxBuf      buffers[4]{};
        draw::Pool*         pools[4] = {};
        engine::CGxTex      tex_fields[2]{};
        draw::Texture*      textures[2] = {};
        int                 programs[2] = {-1, -1}; // [pixel, vertex] answer for any shader

        const engine::CGxBuf* Buffer(const void* buf) override
        {
            for (auto& b : buffers)
                if (&b == buf) return &b;
            return nullptr;
        }
        draw::Pool* Pool(const void* pool) override
        {
            const uintptr_t i = reinterpret_cast<uintptr_t>(pool);
            return i >= 1 && i <= 4 ? pools[i - 1] : nullptr;
        }
        draw::Texture* Texture(const void* texture, const engine::CGxTex*& fields) override
        {
            for (int i = 0; i < 2; ++i)
                if (texture == &tex_fields[i])
                {
                    fields = &tex_fields[i];
                    return textures[i];
                }
            fields = nullptr;
            return nullptr;
        }
        int Program(const void*, bool vertex) override { return programs[vertex ? 1 : 0]; }

        void State(uint32_t which, int32_t value) { states[which].value.data.p = reinterpret_cast<void*>(static_cast<intptr_t>(value)); }
        void StateF(uint32_t which, float value) { states[which].value.data.f[0] = value; }
        void StateP(uint32_t which, const void* value) { states[which].value.data.p = const_cast<void*>(value); }

        /// A window of @p w x @p h, the full viewport, depth off, colour on, a PCT vertex in buffers[0] (pool 1).
        void Reset(uint32_t w, uint32_t h)
        {
            std::memset(&device, 0, sizeof device);
            std::memset(states, 0, sizeof states);
            device.m_curWindowRect = {0.0f, 0.0f, static_cast<float>(h), static_cast<float>(w)};
            const wxl_gx_viewport vp{0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
            std::memcpy(device.m_viewport, &vp, sizeof vp);
            device.m_appMasterEnables = 0x1FF;
            device.m_caps.m_depthBias = 1;
            State(vk::gxfront::kColorWrite, 0xF);
            State(vk::gxfront::kDepthTest, 0);
            // Identity matrices everywhere, their stacks marked identity.
            for (auto* stack : {&device.m_xforms[0], &device.m_xforms[1], &device.m_xforms[8], &device.m_xforms[10], &device.m_texGen[0],
                                &device.m_texGen[1]})
            {
                float* m = &stack->m_mtx[0].a0;
                std::memset(m, 0, 64);
                m[0] = m[5] = m[10] = m[15] = 1.0f;
                stack->m_flags[0] = 1;
            }
            // The projection that puts D3D9's pixel coordinates on the clip space: x' = 2x/w - 1, y' = 1 - 2y/h.
            float* p = &device.m_projNative.a0;
            std::memset(p, 0, 64);
            p[0]  = 2.0f / static_cast<float>(w);
            p[5]  = -2.0f / static_cast<float>(h);
            p[10] = 1.0f;
            p[12] = -1.0f;
            p[13] = 1.0f;
            p[15] = 1.0f;
            // PCT: position float3 at 0, colour at 12, texcoord0 float2 at 16.
            auto* attribs = reinterpret_cast<wxl_gx_vertex_attrib*>(device.m_primVertexFormatAttrib);
            attribs[0]    = {0, 4, 0, 24};
            attribs[4]    = {4, 0, 12, 24};
            attribs[6]    = {6, 3, 16, 24};
            device.m_primVertexMask = (1u << 0) | (1u << 4) | (1u << 6);
            for (uint32_t a : {0u, 4u, 6u}) device.m_primVertexFormatBuf[a] = reinterpret_cast<engine::CGxBuf*>(&buffers[0]);
            buffers[0].m_pool  = reinterpret_cast<engine::CGxPool*>(uintptr_t{1});
            buffers[0].m_index = 0;
            programs[0] = programs[1] = -1;
        }
    };

    void EngineCases()
    {
        FakeEngine fake;
        fake.Reset(64, 32);
        draw::Pool pool;
        fake.pools[0] = &pool;
        draw::Packet         packet;
        vk::gxfront::EngineView view;
        view.device           = &fake.device;
        view.states           = fake.states;
        view.vertex_constants = fake.constants;
        const wxl_gx_batch batch{3, 0, 6, 0, 5};

        Check(vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Draw, "an interface draw is drawn");
        Check(packet.stream_count == 1 && packet.streams[0].stride == 24 && packet.attribute_count == 3 && packet.streams[0].pool == &pool,
              "the PCT vertex is one stream of 24 bytes, three attributes");
        Check(packet.viewport[0] == 0.0f && packet.viewport[2] == 64.0f && packet.viewport[3] == 32.0f && packet.scissor[2] == 64,
              "the viewport and the scissor cover the window");
        Check(packet.vertex == static_cast<uint16_t>(vk::pipelines::VertexProgram::Fixed) && packet.data.ops[0] == -1, "no shader, no texture: fixed function, stage 0 off");

        // The colour mask's bits are red 1, blue 2, green 4, alpha 8.
        fake.State(vk::gxfront::kColorWrite, 2);
        vk::gxfront::BuildDraw(view, batch, 0, fake, packet);
        Check(packet.color_mask == VK_COLOR_COMPONENT_B_BIT, "the engine's colour write bit 2 is blue");
        fake.State(vk::gxfront::kColorWrite, 0xF);
        fake.device.m_appMasterEnables &= ~(1u << 4);
        vk::gxfront::BuildDraw(view, batch, 0, fake, packet);
        Check(packet.color_mask == 0, "the master colour write off writes no colour");
        fake.device.m_appMasterEnables |= 1u << 4;

        // A viewport of the window's right half, y up: D3D9's IXformSetViewport.
        const wxl_gx_viewport half{0.5f, 1.0f, 0.25f, 0.75f, 0.0f, 1.0f};
        std::memcpy(fake.device.m_viewport, &half, sizeof half);
        vk::gxfront::BuildDraw(view, batch, 0, fake, packet);
        Check(packet.viewport[0] == 32.0f && packet.viewport[1] == 8.0f && packet.viewport[2] == 32.0f && packet.viewport[3] == 16.0f,
              "a viewport in [0, 1] of the window, y up, in pixels from the top");
        int32_t clear[4];
        vk::gxfront::ClearRect(fake.device, clear);
        Check(clear[0] == 32 && clear[1] == 8 && clear[2] == 32 && clear[3] == 16, "a clear covers the viewport");

        // The scissor: IStateSyncScissorRect's rounding.
        fake.State(vk::gxfront::kScissorTest, 1);
        fake.device.m_scissorRect = {0.25f, 0.0f, 0.75f, 0.5f}; // minY, minX, maxY, maxX
        vk::gxfront::BuildDraw(view, batch, 0, fake, packet);
        Check(packet.scissor[0] == 0 && packet.scissor[1] == 8 && packet.scissor[2] == 33 && packet.scissor[3] == 17,
              "the scissor rect in pixels, one more on its right and bottom as D3D9's device rounds it");
        fake.State(vk::gxfront::kScissorTest, 0);

        // Blend, alpha, cull, depth write, bias.
        fake.State(vk::gxfront::kBlendingMode, 2);
        fake.State(vk::gxfront::kAlphaRef, 224);
        fake.State(vk::gxfront::kCulling, 2);
        fake.State(vk::gxfront::kDepthWrite, 1);
        fake.StateF(vk::gxfront::kPolygonOffset, 0.001f);
        vk::gxfront::BuildDraw(view, batch, 0, fake, packet);
        Check(packet.blend == 2 && packet.alpha_ref == 224.0f && packet.cull == VK_CULL_MODE_BACK_BIT && packet.depth_write &&
                  packet.depth_bias == -0.001f,
              "blend, alpha reference, CCW culling as the back, depth write, bias negated as D3D9's");
        Check(vk::gxfront::BuildDraw(view, batch, 1, fake, packet) == vk::gxfront::Verdict::Unready, "an indexed draw without an index buffer waits");

        // The verdicts.
        fake.State(vk::gxfront::kDepthTest, 1);
        Check(vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Draw && packet.depth_test,
              "an orthographic draw testing depth is the interface's (the loading screen tests depth)");
        fake.device.m_appMasterEnables &= ~(1u << 2);
        Check(vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Draw && !packet.depth_test,
              "the master depth test off turns it off");
        fake.State(vk::gxfront::kDepthTest, 0);
        const float d3 = fake.device.m_projNative.d3;
        fake.device.m_projNative.c3 = 1.0f;
        fake.device.m_projNative.d3 = 0.0f;
        Check(vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Draw,
              "fixed function in perspective is drawn (the world's, a model's in a frame)");
        fake.device.m_projNative.c3 = 0.0f;
        fake.device.m_projNative.d3 = d3;
        fake.StateP(vk::gxfront::kVertexShader, &fake);
        Check(vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Program, "a client program the translator refused");
        fake.programs[1] = static_cast<int>(vk::pipelines::VertexProgram::Interface);
        fake.constants[0] = 7.0f;
        Check(vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Draw &&
                  packet.vertex == static_cast<uint16_t>(vk::pipelines::VertexProgram::Interface) && packet.data.constants[0][0] == 7.0f,
              "the interface's program, its c0..c3 copied");
        fake.StateP(vk::gxfront::kVertexShader, nullptr);
        engine::CGxTex target{};
        fake.device.m_textureTarget[0].m_texture = reinterpret_cast<engine::CGxTex*>(&target);
        Check(vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Draw, "a draw into a texture target is drawn");
        fake.device.m_textureTarget[0].m_texture = nullptr;
        fake.device.m_textureTarget[1].m_texture = reinterpret_cast<engine::CGxTex*>(&target);
        Check(vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Target, "a draw into a depth texture alone is not");
        fake.device.m_textureTarget[1].m_texture = nullptr;

        // Texture stages: a stage with a texture takes its ops, the sampler its flags.
        draw::Texture tex0, tex1;
        fake.textures[0] = &tex0;
        fake.textures[1] = &tex1;
        fake.tex_fields[0].m_flags.m_filter = 0;
        fake.tex_fields[0].m_flags.m_wrapU  = 1;
        fake.StateP(vk::gxfront::kTexture0, &fake.tex_fields[0]);
        fake.State(vk::gxfront::kColorOp0, 1);
        fake.State(vk::gxfront::kAlphaOp0, 3);
        vk::gxfront::BuildDraw(view, batch, 0, fake, packet);
        Check(packet.textures[0] == &tex0 && packet.data.ops[0] == 1 && packet.data.ops[1] == 3 && packet.data.ops[2] == -1 &&
                  packet.samplers[0].filter == 0 && packet.samplers[0].wrap_u == 1,
              "stage 0's texture, its ops, its sampler; stage 1 off without a texture");
        fake.StateP(vk::gxfront::kTexture0 + 1, &fake.tex_fields[1]);
        fake.State(vk::gxfront::kColorOp0 + 1, 4);
        vk::gxfront::BuildDraw(view, batch, 0, fake, packet);
        Check(packet.data.ops[2] == 4, "stage 1 runs with its texture");

        // Texture transforms (IXformSetTex): TextureShader 1 without texgen is COUNT2, the
        // translation moved from row 3 to row 2.
        fake.device.m_xforms[0].m_mtx[0].d0 = 0.25f;
        fake.State(vk::gxfront::kTextureShader0, 1);
        vk::gxfront::BuildDraw(view, batch, 0, fake, packet);
        Check(packet.data.texgen[1] == 2 && packet.data.texMatrix[0][8] == 0.25f, "a 2D texture transform: COUNT2, its translation in row 2");
        fake.State(vk::gxfront::kTextureShader0, 2);
        vk::gxfront::BuildDraw(view, batch, 0, fake, packet);
        Check(packet.data.texgen[1] == (3 | 0x100), "TextureShader 2: COUNT3 | PROJECTED");

        // Ambient-only colour: no vertex colour and a material that is not white.
        fake.device.m_primVertexMask &= ~(1u << 4);
        fake.State(vk::gxfront::kMatDiffuse, static_cast<int32_t>(0x80FF0000u));
        vk::gxfront::BuildDraw(view, batch, 0, fake, packet);
        Check(packet.data.lighting[1] == 1 && packet.data.lighting[0] == 0 && packet.data.globalAmbient[0] == 1.0f &&
                  packet.data.matDiffuse[0] == 1.0f && packet.data.matDiffuse[1] == 0.0f &&
                  std::fabs(packet.data.matDiffuse[3] - 128.0f / 255.0f) < 1e-6f,
              "no vertex colour and a red material: D3D9's ambient-only lighting, the material's colour");
    }

    // --- Live ------------------------------------------------------------------------------------------

    struct Live
    {
        vk::device::Context   context;
        vk::resources::Allocator allocator;
        vk::frame::Timeline   timeline;
        vk::present::Display  display;
        draw::Renderer        renderer;
        uint32_t              width = 0, height = 0;

        bool Make(uint32_t w, uint32_t h, char* why, uint32_t size)
        {
            vk::device::Options options;
            options.floor.present = false;
            if (!context.Create(options, why, size) || !allocator.Create(context, why, size) || !timeline.Create(context, why, size))
                return false;
            vk::present::Display::Options d;
            d.frames = 2;
            d.filter = vk::present::Filter::Nearest;
            if (!display.Create(context, allocator, timeline, d, why, size) || !display.SetOffscreen({w, h})) return false;
            display.SetSceneExtent({w, h});
            width  = w;
            height = h;
            return renderer.Create(context, allocator, display, {}, why, size);
        }

        bool Begin(uint32_t clear = 0xFF000000u)
        {
            vk::frame::Wait wait{};
            if (!display.BeginFrame(wait)) return false;
            const float rgba[4] = {((clear >> 16) & 0xFF) / 255.0f, ((clear >> 8) & 0xFF) / 255.0f, (clear & 0xFF) / 255.0f,
                                   (clear >> 24) / 255.0f};
            display.Clear(true, true, rgba, 1.0f, {{0, 0}, {width, height}});
            return true;
        }

        std::vector<uint32_t> End()
        {
            std::vector<uint32_t> pixels;
            display.EndFrame();
            display.ReadOffscreen(pixels);
            return pixels;
        }

        ~Live()
        {
            if (timeline.Ready() && timeline.Last()) timeline.WaitFor(timeline.Last());
            renderer.Destroy();
            display.Destroy();
            timeline.Destroy();
            allocator.Destroy();
            context.Destroy();
        }
    };

    struct Vertex
    {
        float    x, y, z;
        uint32_t color;
        float    u, v;
    };

    /// A rectangle covering the pixels [x0, x1) x [y0, y1) as D3D9 places them (corners on the
    /// pixels' edges, half a pixel before their centres), with texture coordinates 0..1.
    void Quad(Vertex* out, float x0, float y0, float x1, float y1, uint32_t color, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1)
    {
        x0 -= 0.5f;
        y0 -= 0.5f;
        x1 -= 0.5f;
        y1 -= 0.5f;
        out[0] = {x0, y0, 0.5f, color, u0, v0};
        out[1] = {x1, y0, 0.5f, color, u1, v0};
        out[2] = {x0, y1, 0.5f, color, u0, v1};
        out[3] = {x1, y0, 0.5f, color, u1, v0};
        out[4] = {x1, y1, 0.5f, color, u1, v1};
        out[5] = {x0, y1, 0.5f, color, u0, v1};
    }

    /// Writes @p count vertices into a stream pool and fills the fake engine's buffer for them.
    void Stream(Live& live, FakeEngine& fake, draw::Pool* pool, const Vertex* vertices, uint32_t count, uint32_t& at)
    {
        uint8_t* memory = live.renderer.PoolMemory(pool);
        if (at + count * sizeof(Vertex) > pool->size)
        {
            live.renderer.DiscardPool(pool);
            memory = live.renderer.PoolMemory(pool);
            at     = 0;
        }
        std::memcpy(memory + at, vertices, count * sizeof(Vertex));
        live.renderer.PoolWritten(pool, at, count * sizeof(Vertex));
        fake.buffers[0].m_index = at;
        at += count * static_cast<uint32_t>(sizeof(Vertex));
    }

    /// Records the fake engine's draw of @p count vertices.
    bool Record(Live& live, FakeEngine& fake, uint32_t count)
    {
        vk::gxfront::EngineView view;
        view.device           = &fake.device;
        view.states           = fake.states;
        view.vertex_constants = fake.constants;
        draw::Packet packet;
        const wxl_gx_batch batch{3, 0, count, 0, static_cast<uint16_t>(count - 1)};
        return vk::gxfront::BuildDraw(view, batch, 0, fake, packet) == vk::gxfront::Verdict::Draw && live.renderer.Draw(packet);
    }

    /// The engine's texture of @p format with @p bytes as its first level, written in the open frame.
    draw::Texture* MakeTexture(Live& live, uint32_t format, uint32_t w, uint32_t h, const void* bytes, uint32_t pitch)
    {
        draw::TextureDesc desc;
        desc.format         = format;
        desc.width          = w;
        desc.height         = h;
        draw::Texture* t    = live.renderer.CreateTexture(desc);
        if (!t) return nullptr;
        uint32_t to_pitch   = 0;
        uint8_t* to         = live.renderer.LockTexture(t, 0, 0, 0, 0, w, h, to_pitch);
        if (!to) return t;
        const uint32_t rows = draw::Formats::Rows(t->info, h);
        for (uint32_t r = 0; r < rows; ++r) std::memcpy(to + r * to_pitch, static_cast<const uint8_t*>(bytes) + r * pitch, std::min(pitch, to_pitch));
        live.renderer.UnlockTexture(t);
        return t;
    }

    void FormatCases(Live& live, FakeEngine& fake, draw::Pool* pool)
    {
        struct Case
        {
            uint32_t    format;
            const char* name;
            uint32_t    texel[4];   ///< four texels in D3D9's layout, as 32-bit words holding their bytes
            uint32_t    bytes;      ///< per texel (0: a 4x4 block)
            uint32_t    expect[4];  ///< 0xAARRGGBB seen
            uint32_t    tolerance;
        };
        const Case cases[] = {
            {draw::kTexArgb8888, "Argb8888", {0xFFFF0000u, 0x8000FF00u, 0xFF0000FFu, 0x40102030u}, 4,
             {0xFFFF0000u, 0x8000FF00u, 0xFF0000FFu, 0x40102030u}, 0},
            {draw::kTexAbgr8888, "Abgr8888", {0xFF0000FFu, 0x8000FF00u, 0xFFFF0000u, 0x40302010u}, 4,
             {0xFFFF0000u, 0x8000FF00u, 0xFF0000FFu, 0x40102030u}, 0},
            {draw::kTexArgb4444, "Argb4444", {0xFF00u, 0x80F0u, 0xF00Fu, 0x4123u}, 2,
             {0xFFFF0000u, 0x8800FF00u, 0xFF0000FFu, 0x44112233u}, 0},
            {draw::kTexArgb1555, "Argb1555", {0xFC00u, 0x03E0u, 0x801Fu, 0xFFFFu}, 2,
             {0xFFFF0000u, 0x0000FF00u, 0xFF0000FFu, 0xFFFFFFFFu}, 0},
            {draw::kTexRgb565, "Rgb565", {0xF800u, 0x07E0u, 0x001Fu, 0xFFFFu}, 2,
             {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu}, 0},
        };
        for (const Case& c : cases)
        {
            if (!live.Begin()) return;
            uint8_t bytes[2 * 2 * 4];
            for (int i = 0; i < 4; ++i) std::memcpy(bytes + i * c.bytes, &c.texel[i], c.bytes);
            draw::Texture* t = MakeTexture(live, c.format, 2, 2, bytes, 2 * c.bytes);
            if (!t)
            {
                live.End();
                Check(false, c.name);
                continue;
            }
            fake.textures[0]              = t;
            fake.tex_fields[0].m_flags    = {};
            fake.tex_fields[0].m_flags.m_filter = 0;
            fake.StateP(vk::gxfront::kTexture0, &fake.tex_fields[0]);
            fake.State(vk::gxfront::kColorOp0, 3); // PassThru... the texel alone: SELECTARG2 is CURRENT; use Mod with white
            fake.State(vk::gxfront::kColorOp0, 0);
            fake.State(vk::gxfront::kAlphaOp0, 0);
            fake.State(vk::gxfront::kBlendingMode, 0);
            Vertex   quad[6];
            uint32_t at = 0;
            Quad(quad, 0, 0, 2, 2, 0xFFFFFFFFu);
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            const std::vector<uint32_t> pixels = live.End();
            bool ok = pixels.size() == live.width * live.height;
            for (int i = 0; ok && i < 4; ++i)
            {
                const uint32_t seen = pixels[(i / 2) * live.width + (i % 2)];
                // The back buffer keeps the alpha the draw wrote (opaque blend).
                if (!Near(seen, c.expect[i], c.tolerance))
                {
                    std::printf("  %s texel %d: %08x, want %08x\n", c.name, i, seen, c.expect[i]);
                    ok = false;
                }
            }
            char what[96];
            std::snprintf(what, sizeof what, "%s texels drawn as D3D9 reads them", c.name);
            Check(ok, what);
            live.renderer.DestroyTexture(t);
        }

        // DXT1: a block of colour0 red, colour1 blue, indices 0 1 2 3 on its first row.
        if (!wxl::vk::test::Environment("WXL_VK_NO_BC") && live.renderer.TextureFormats().Of(draw::kTexDxt1).format != VK_FORMAT_UNDEFINED && live.Begin())
        {
            const uint8_t block[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};
            draw::Texture* t       = MakeTexture(live, draw::kTexDxt1, 4, 4, block, 8);
            fake.textures[0]       = t;
            Vertex   quad[6];
            uint32_t at = 0;
            Quad(quad, 0, 0, 4, 4, 0xFFFFFFFFu);
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            const std::vector<uint32_t> p = live.End();
            Check(p.size() == live.width * live.height && Near(p[0], 0xFFFF0000u, 2) && Near(p[1], 0xFF0000FFu, 2) &&
                      Near(p[2], Argb(170, 0, 85), 3) && Near(p[3], Argb(85, 0, 170), 3),
                  "a DXT1 block decodes to its four colours");
            live.renderer.DestroyTexture(t);
        }

        // V8U8 and the float formats read their missing channels as 1, as D3D9 does.
        if (live.Begin())
        {
            const float    r32[4] = {0.5f, 0.25f, 1.0f, 0.0f};
            draw::Texture* t      = MakeTexture(live, draw::kTexR32F, 2, 2, r32, 8);
            fake.textures[0]      = t;
            Vertex   quad[6];
            uint32_t at = 0;
            Quad(quad, 0, 0, 2, 2, 0xFFFFFFFFu);
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            const std::vector<uint32_t> p = live.End();
            Check(p.size() == live.width * live.height && Near(p[0], Argb(128, 255, 255), 1) && Near(p[1], Argb(64, 255, 255), 1),
                  "R32F reads (r, 1, 1, 1)");
            live.renderer.DestroyTexture(t);
        }
        fake.StateP(vk::gxfront::kTexture0, nullptr);
        fake.textures[0] = nullptr;
    }

    /// D3D9's blend of @p src over @p dst with @p mode, per channel, for the reference.
    uint32_t BlendReference(uint32_t mode, uint32_t src, uint32_t dst, float factor)
    {
        const vk::pipelines::Blend b = vk::pipelines::BlendOf(mode);
        if (!b.enable) return src;
        auto ch = [](uint32_t c, int shift) { return static_cast<float>((c >> shift) & 0xFF) / 255.0f; };
        uint32_t out = 0;
        for (int shift = 0; shift < 32; shift += 8)
        {
            const float s = ch(src, shift), d = ch(dst, shift), sa = ch(src, 24);
            auto f = [&](VkBlendFactor k, bool source) {
                switch (k)
                {
                case VK_BLEND_FACTOR_ZERO: return 0.0f;
                case VK_BLEND_FACTOR_ONE: return 1.0f;
                case VK_BLEND_FACTOR_SRC_ALPHA: return sa;
                case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return 1.0f - sa;
                case VK_BLEND_FACTOR_DST_COLOR: return d;
                case VK_BLEND_FACTOR_SRC_COLOR: return s;
                case VK_BLEND_FACTOR_CONSTANT_COLOR: return factor;
                case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return 1.0f - factor;
                default: (void)source; return 0.0f;
                }
            };
            const float v = std::clamp(s * f(b.source, true) + d * f(b.destination, false), 0.0f, 1.0f);
            out |= static_cast<uint32_t>(std::lround(v * 255.0f)) << shift;
        }
        return out;
    }

    void BlendCases(Live& live, FakeEngine& fake, draw::Pool* pool)
    {
        const uint32_t dst = Argb(40, 120, 200, 160), src = Argb(200, 100, 50, 96);
        bool           all = true;
        for (uint32_t mode = 0; mode < draw::kBlendModes; ++mode)
        {
            if (!live.Begin(dst)) return;
            fake.State(vk::gxfront::kBlendingMode, static_cast<int32_t>(mode));
            fake.StateF(vk::gxfront::kBlendFactor, 0.5f);
            Vertex   quad[6];
            uint32_t at = 0;
            Quad(quad, 0, 0, 4, 4, src);
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            const std::vector<uint32_t> p = live.End();
            const float    factor = static_cast<float>(static_cast<uint32_t>(0.5f * 255.0f)) / 255.0f;
            const uint32_t want   = BlendReference(mode, src, dst, factor);
            if (p.empty() || !Near(p[live.width + 1], want, 2))
            {
                std::printf("  blend mode %u: %08x, want %08x\n", mode, p.empty() ? 0 : p[live.width + 1], want);
                all = false;
            }
        }
        Check(all, "the thirteen blend modes against D3D9's formula");
        fake.State(vk::gxfront::kBlendingMode, 0);

        // The alpha test: the alpha as an 8-bit value, at or above the reference passes.
        if (!live.Begin(Argb(0, 0, 0)))
            return;
        fake.State(vk::gxfront::kAlphaRef, 128);
        Vertex   quads[12];
        uint32_t at = 0;
        Quad(quads, 0, 0, 2, 2, Argb(255, 0, 0, 127));
        Quad(quads + 6, 2, 0, 4, 2, Argb(0, 255, 0, 128));
        Stream(live, fake, pool, quads, 12, at);
        Record(live, fake, 12);
        std::vector<uint32_t> p = live.End();
        Check(!p.empty() && (p[0] & 0xFFFFFF) == 0 && (p[2] & 0xFFFFFF) == 0x00FF00, "the alpha test drops 127 and keeps 128 for a reference of 128");
        fake.State(vk::gxfront::kAlphaRef, 0);

        // The scissor.
        if (!live.Begin(Argb(0, 0, 0))) return;
        fake.State(vk::gxfront::kScissorTest, 1);
        fake.device.m_scissorRect = {0.5f, 0.0f, 1.0f, 0.5f}; // the top left quarter, y up
        at                        = 0;
        Quad(quads, 0, 0, static_cast<float>(live.width), static_cast<float>(live.height), Argb(255, 255, 255));
        Stream(live, fake, pool, quads, 6, at);
        Record(live, fake, 6);
        p = live.End();
        const uint32_t hw = live.width / 2, hh = live.height / 2;
        Check(!p.empty() && (p[0] & 0xFFFFFF) == 0xFFFFFF && (p[(hh - 1) * live.width + hw - 1] & 0xFFFFFF) == 0xFFFFFF &&
                  (p[(hh + 1) * live.width + 0] & 0xFFFFFF) == 0 && (p[hw + 1] & 0xFFFFFF) == 0,
              "the scissor keeps the draw inside its rectangle (and its extra pixel on the right and bottom)");
        fake.State(vk::gxfront::kScissorTest, 0);

        // Culling: the quads are clockwise on the screen; D3D9's CW culls them, CCW keeps them.
        for (int32_t cull : {1, 2})
        {
            if (!live.Begin(Argb(0, 0, 0))) return;
            fake.State(vk::gxfront::kCulling, cull);
            at = 0;
            Quad(quads, 0, 0, 2, 2, Argb(255, 255, 255));
            Stream(live, fake, pool, quads, 6, at);
            Record(live, fake, 6);
            p = live.End();
            Check(!p.empty() && ((p[0] & 0xFFFFFF) == 0) == (cull == 1),
                  cull == 1 ? "D3D9's CW culling drops a quad clockwise on the screen" : "and CCW keeps it");
        }
        fake.State(vk::gxfront::kCulling, 0);

        // Depth: a draw testing LEQUAL against a far depth passes, against a near one not.
        if (!live.Begin(Argb(0, 0, 0))) return;
        fake.State(vk::gxfront::kDepthTest, 1);
        fake.State(vk::gxfront::kDepthFunc, 0);
        at = 0;
        Quad(quads, 0, 0, 2, 2, Argb(255, 255, 255));
        Stream(live, fake, pool, quads, 6, at);
        Record(live, fake, 6);
        const float rgba[4] = {0, 0, 0, 1};
        live.display.Clear(false, true, rgba, 0.25f, {{2, 0}, {2, 2}});
        Quad(quads, 2, 0, 4, 2, Argb(255, 255, 255));
        Stream(live, fake, pool, quads, 6, at);
        Record(live, fake, 6);
        p = live.End();
        Check(!p.empty() && (p[0] & 0xFFFFFF) == 0xFFFFFF && (p[2] & 0xFFFFFF) == 0, "the depth test: LEQUAL passes over the far plane, fails behind a nearer depth");
        fake.State(vk::gxfront::kDepthTest, 0);
    }

    void StageCases(Live& live, FakeEngine& fake, draw::Pool* pool)
    {
        // Stage 0 texel t, diffuse c; D3D9's ops.
        const uint32_t t = Argb(200, 100, 50, 128), c = Argb(100, 200, 50, 64);
        auto chan = [](uint32_t v, int shift) { return static_cast<float>((v >> shift) & 0xFF) / 255.0f; };
        auto op   = [&](int o, float tv, float cv, float da) {
            switch (o)
            {
            case 0: return tv * cv;
            case 1: return 2.0f * tv * cv;
            case 2: return tv + cv;
            case 3: return cv;
            case 4: return cv * da + tv * (1.0f - da);
            default: return tv * da + cv * (1.0f - da);
            }
        };
        bool all = true;
        for (int o = 0; o < 6; ++o)
        {
            if (!live.Begin()) return;
            const uint32_t texel = t;
            draw::Texture* tex   = MakeTexture(live, draw::kTexArgb8888, 1, 1, &texel, 4);
            fake.textures[0]     = tex;
            fake.StateP(vk::gxfront::kTexture0, &fake.tex_fields[0]);
            fake.State(vk::gxfront::kColorOp0, o);
            fake.State(vk::gxfront::kAlphaOp0, o);
            Vertex   quad[6];
            uint32_t at = 0;
            Quad(quad, 0, 0, 2, 2, c);
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            const std::vector<uint32_t> p = live.End();
            uint32_t want = 0;
            for (int shift = 0; shift < 32; shift += 8)
            {
                const float a = shift == 24 ? chan(c, 24) : chan(c, 24);
                want |= static_cast<uint32_t>(std::lround(std::clamp(op(o, chan(t, shift), chan(c, shift), a), 0.0f, 1.0f) * 255.0f)) << shift;
            }
            if (p.empty() || !Near(p[0], want, 2))
            {
                std::printf("  stage op %d: %08x, want %08x\n", o, p.empty() ? 0 : p[0], want);
                all = false;
            }
            live.renderer.DestroyTexture(tex);
        }
        Check(all, "fixed function's six texture ops on stage 0 (colour and alpha)");

        // Stage 1 after stage 0: Mod then Add.
        if (live.Begin())
        {
            const uint32_t a = Argb(128, 128, 128), b = Argb(32, 64, 96);
            draw::Texture* t0 = MakeTexture(live, draw::kTexArgb8888, 1, 1, &a, 4);
            draw::Texture* t1 = MakeTexture(live, draw::kTexArgb8888, 1, 1, &b, 4);
            fake.textures[0]  = t0;
            fake.textures[1]  = t1;
            fake.StateP(vk::gxfront::kTexture0 + 1, &fake.tex_fields[1]);
            fake.State(vk::gxfront::kColorOp0, 0);
            fake.State(vk::gxfront::kAlphaOp0, 0);
            fake.State(vk::gxfront::kColorOp0 + 1, 2);
            fake.State(vk::gxfront::kAlphaOp0 + 1, 3);
            Vertex   quad[6];
            uint32_t at = 0;
            Quad(quad, 0, 0, 2, 2, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            const std::vector<uint32_t> p = live.End();
            Check(!p.empty() && Near(p[0], Argb(160, 192, 224), 2), "stage 1 adds its texel to stage 0's result");
            live.renderer.DestroyTexture(t0);
            live.renderer.DestroyTexture(t1);
            fake.StateP(vk::gxfront::kTexture0 + 1, nullptr);
            fake.textures[1] = nullptr;
        }
        fake.StateP(vk::gxfront::kTexture0, nullptr);
        fake.textures[0] = nullptr;
    }

    void ProgramCases(Live& live, FakeEngine& fake, draw::Pool* pool)
    {
        // The interface's program: c0..c3 as the engine sets them (transpose(view * projNative)).
        const float w = static_cast<float>(live.width), h = static_cast<float>(live.height);
        const float rows[16] = {2.0f / w, 0, 0, -1.0f, 0, -2.0f / h, 0, 1.0f, 0, 0, 1.0f, 0, 0, 0, 0, 1.0f};
        std::memcpy(fake.constants, rows, sizeof rows);
        fake.programs[1] = static_cast<int>(vk::pipelines::VertexProgram::Interface);
        fake.programs[0] = static_cast<int>(vk::pipelines::PixelProgram::Interface);
        fake.StateP(vk::gxfront::kVertexShader, &fake);
        fake.StateP(vk::gxfront::kPixelShader, &fake);
        if (!live.Begin()) return;
        const uint32_t texels[4] = {Argb(255, 0, 0), Argb(0, 255, 0), Argb(0, 0, 255), Argb(255, 255, 255, 0)};
        draw::Texture* t         = MakeTexture(live, draw::kTexArgb8888, 2, 2, texels, 8);
        fake.textures[0]         = t;
        fake.StateP(vk::gxfront::kTexture0, &fake.tex_fields[0]);
        Vertex   quad[12];
        uint32_t at = 0;
        Quad(quad, 0, 0, 2, 2, Argb(255, 255, 255, 255));
        Stream(live, fake, pool, quad, 6, at);
        Record(live, fake, 6);
        std::vector<uint32_t> p = live.End();
        Check(!p.empty() && p[0] == Argb(255, 0, 0) && p[1] == Argb(0, 255, 0) && p[live.width] == Argb(0, 0, 255) &&
                  p[live.width + 1] == Argb(255, 255, 255, 0),
              "the interface's program draws its texture texel for texel");

        // Desaturate: grey of the texel's luminance, its alpha times the colour's.
        if (!live.Begin()) return;
        fake.programs[0] = static_cast<int>(vk::pipelines::PixelProgram::Desaturate);
        at               = 0;
        Quad(quad, 0, 0, 2, 2, Argb(10, 20, 30, 128));
        Stream(live, fake, pool, quad, 6, at);
        Record(live, fake, 6);
        p = live.End();
        const uint32_t red = static_cast<uint32_t>(std::lround(0.299f * 255.0f)), green = static_cast<uint32_t>(std::lround(0.587f * 255.0f));
        Check(!p.empty() && Near(p[0], Argb(red, red, red, 128), 1) && Near(p[1], Argb(green, green, green, 128), 1),
              "Desaturate: the luminance as grey, the alpha times the colour's");

        // D3D9's pixel centres: a rectangle on pixel edges covers those pixels and no more.
        if (!live.Begin(Argb(0, 0, 0))) return;
        fake.programs[0] = static_cast<int>(vk::pipelines::PixelProgram::Interface);
        fake.StateP(vk::gxfront::kTexture0, nullptr);
        fake.textures[0] = nullptr;
        fake.programs[0] = -1;
        fake.StateP(vk::gxfront::kPixelShader, nullptr);
        at = 0;
        Quad(quad, 2, 1, 4, 3, Argb(255, 255, 255));
        Stream(live, fake, pool, quad, 6, at);
        Record(live, fake, 6);
        p = live.End();
        bool exact = !p.empty();
        for (uint32_t y = 0; exact && y < 5; ++y)
            for (uint32_t x = 0; x < 6; ++x)
            {
                const bool inside = x >= 2 && x < 4 && y >= 1 && y < 3;
                if (((p[y * live.width + x] & 0xFFFFFF) != 0) != inside) exact = false;
            }
        Check(exact, "a rectangle placed as D3D9 places pixels covers exactly its pixels");
        fake.StateP(vk::gxfront::kVertexShader, nullptr);
        fake.programs[1] = -1;
        live.renderer.DestroyTexture(t);
    }

    void PoolAndUploadCases(Live& live, FakeEngine& fake, draw::Pool* pool)
    {
        // A stream pool discarded in the middle of a frame: the first draw keeps its vertices.
        if (!live.Begin(Argb(0, 0, 0))) return;
        Vertex   quad[6];
        uint32_t at = 0;
        Quad(quad, 0, 0, 2, 2, Argb(255, 0, 0));
        Stream(live, fake, pool, quad, 6, at);
        Record(live, fake, 6);
        live.renderer.DiscardPool(pool);
        at = 0;
        Quad(quad, 2, 0, 4, 2, Argb(0, 255, 0));
        Stream(live, fake, pool, quad, 6, at);
        Record(live, fake, 6);
        std::vector<uint32_t> p = live.End();
        Check(!p.empty() && (p[0] & 0xFFFFFF) == 0xFF0000 && (p[2] & 0xFFFFFF) == 0x00FF00,
              "a stream pool renamed when discarded: the draw before keeps its vertices");

        // A static pool written again after a draw read it, in the same frame.
        draw::Pool* fixed = live.renderer.CreatePool(draw::kVertexPool, draw::kStatic, 6 * sizeof(Vertex));
        fake.pools[1]     = fixed;
        fake.buffers[1].m_pool  = reinterpret_cast<engine::CGxPool*>(uintptr_t{2});
        fake.buffers[1].m_index = 0;
        for (uint32_t a : {0u, 4u, 6u}) fake.device.m_primVertexFormatBuf[a] = reinterpret_cast<engine::CGxBuf*>(&fake.buffers[1]);
        if (!live.Begin(Argb(0, 0, 0))) return;
        Quad(quad, 0, 0, 2, 2, Argb(0, 0, 255));
        std::memcpy(live.renderer.PoolMemory(fixed), quad, sizeof quad);
        live.renderer.PoolWritten(fixed, 0, sizeof quad);
        Record(live, fake, 6);
        Quad(quad, 2, 0, 4, 2, Argb(255, 255, 0));
        std::memcpy(live.renderer.PoolMemory(fixed), quad, sizeof quad);
        live.renderer.PoolWritten(fixed, 0, sizeof quad);
        const uint64_t breaks = live.renderer.Counters().pass_breaks;
        Record(live, fake, 6);
        p = live.End();
        Check(!p.empty() && (p[0] & 0xFFFFFF) == 0x0000FF && (p[2] & 0xFFFFFF) == 0xFFFF00 && live.renderer.Counters().pass_breaks > breaks,
              "a static pool written after a draw read it: the copy goes between the two draws");
        // The next frame draws it from the device copy, nothing to copy.
        if (live.Begin(Argb(0, 0, 0)))
        {
            const uint64_t bytes = live.renderer.Counters().upload_bytes;
            Record(live, fake, 6);
            p = live.End();
            Check(!p.empty() && (p[2] & 0xFFFFFF) == 0xFFFF00 && live.renderer.Counters().upload_bytes == bytes,
                  "an unchanged static pool is drawn from the device without a copy");
        }
        for (uint32_t a : {0u, 4u, 6u}) fake.device.m_primVertexFormatBuf[a] = reinterpret_cast<engine::CGxBuf*>(&fake.buffers[0]);
        live.renderer.DestroyPool(fixed);
        fake.pools[1] = nullptr;

        // A texture written again after a draw read it (the glyph cache).
        if (!live.Begin(Argb(0, 0, 0))) return;
        uint32_t       texel = Argb(255, 0, 255);
        draw::Texture* t     = MakeTexture(live, draw::kTexArgb8888, 1, 1, &texel, 4);
        fake.textures[0]     = t;
        fake.StateP(vk::gxfront::kTexture0, &fake.tex_fields[0]);
        fake.State(vk::gxfront::kColorOp0, 0);
        fake.State(vk::gxfront::kAlphaOp0, 0);
        at = 0;
        Quad(quad, 0, 0, 2, 2, Argb(255, 255, 255));
        Stream(live, fake, pool, quad, 6, at);
        Record(live, fake, 6);
        uint32_t pitch = 0;
        uint8_t* to    = live.renderer.LockTexture(t, 0, 0, 0, 0, 1, 1, pitch);
        texel          = Argb(0, 255, 255);
        if (to) std::memcpy(to, &texel, 4);
        live.renderer.UnlockTexture(t);
        Quad(quad, 2, 0, 4, 2, Argb(255, 255, 255));
        Stream(live, fake, pool, quad, 6, at);
        Record(live, fake, 6);
        p = live.End();
        Check(!p.empty() && (p[0] & 0xFFFFFF) == 0xFF00FF && (p[2] & 0xFFFFFF) == 0x00FFFF,
              "a texture written after a draw read it: the draw before sees the old texels, the one after the new");
        live.renderer.DestroyTexture(t);

        // Mips made from the first level: a 2x2 of black and white averages to grey at level 1.
        if (live.Begin(Argb(0, 0, 0)))
        {
            draw::TextureDesc desc;
            desc.width         = 2;
            desc.height        = 2;
            desc.generate_mips = true;
            draw::Texture* m   = live.renderer.CreateTexture(desc);
            const uint32_t checker[4] = {Argb(255, 255, 255), Argb(0, 0, 0), Argb(0, 0, 0), Argb(255, 255, 255)};
            if (m)
            {
                uint8_t* bytes = live.renderer.LockTexture(m, 0, 0, 0, 0, 2, 2, pitch);
                if (bytes)
                    for (int r = 0; r < 2; ++r) std::memcpy(bytes + r * pitch, checker + r * 2, 8);
                live.renderer.UnlockTexture(m);
            }
            fake.textures[0]                    = m;
            fake.tex_fields[0].m_flags.m_filter = 4; // trilinear
            at                                  = 0;
            Quad(quad, 0, 0, 1, 1, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            p = live.End();
            Check(m && m->image.levels == 2 && !p.empty() && Near(p[0], Argb(128, 128, 128), 3),
                  "AUTOGENMIPMAP: the levels made from the first, sampled at level 1");
            fake.tex_fields[0].m_flags.m_filter = 0;
            live.renderer.DestroyTexture(m);
        }

        // A cube: its first face is what a 2D stage reads.
        if (live.Begin(Argb(0, 0, 0)))
        {
            draw::TextureDesc desc;
            desc.cube        = true;
            draw::Texture* c = live.renderer.CreateTexture(desc);
            for (uint32_t face = 0; c && face < 6; ++face)
            {
                uint8_t*       bytes = live.renderer.LockTexture(c, face, 0, 0, 0, 1, 1, pitch);
                const uint32_t v     = face == 0 ? Argb(10, 200, 30) : Argb(255, 0, 0);
                if (bytes) std::memcpy(bytes, &v, 4);
                live.renderer.UnlockTexture(c);
            }
            fake.textures[0] = c;
            at               = 0;
            Quad(quad, 0, 0, 1, 1, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            p = live.End();
            Check(c && c->image.layers == 6 && !p.empty() && (p[0] & 0xFFFFFF) == 0x0AC81E, "a cube's six faces, its first read by a 2D stage");
            live.renderer.DestroyTexture(c);
        }
        fake.StateP(vk::gxfront::kTexture0, nullptr);
        fake.textures[0] = nullptr;
    }

    // --- A capture made up in the client's format --------------------------------------------------------

    struct Writer
    {
        std::vector<uint8_t> out;
        void Put(const void* p, size_t n) { out.insert(out.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
        void Put32(uint32_t v) { Put(&v, 4); }
        void Put64(uint64_t v) { Put(&v, 8); }
        size_t Begin(uint32_t id)
        {
            Put32(id);
            Put32(0);
            return out.size();
        }
        void End(size_t start)
        {
            const uint32_t size = static_cast<uint32_t>(out.size() - start);
            std::memcpy(out.data() + start - 4, &size, 4);
            while (out.size() % 4) out.push_back(0);
        }
        void Region(uint32_t region, const void* bytes, uint32_t size)
        {
            const size_t s = Begin(WXL_GX_CAPTURE_MEM);
            Put32(region);
            Put32(size);
            Put32(1);
            Put32(0);
            Put32(size);
            Put(bytes, size);
            End(s);
        }
    };

    void ReplayCases(Live& live)
    {
        const uint32_t w = live.width, h = live.height;
        FakeEngine     fake;
        fake.Reset(w, h);
        // The vertex buffer's id in the capture is the pointer the device names.
        const uint64_t bufId = reinterpret_cast<uint64_t>(&fake.buffers[0]), poolId = 0x5000, texId = 0x6000, vsId = 0x7000, psId = 0x7100;
        fake.buffers[0].m_pool  = reinterpret_cast<engine::CGxPool*>(poolId);
        fake.buffers[0].m_index = 0;
        engine::CGxTex tex{};
        tex.m_width   = 2;
        tex.m_height  = 2;
        tex.m_format  = draw::kTexArgb8888;
        fake.StateP(vk::gxfront::kTexture0, reinterpret_cast<void*>(texId));
        fake.StateP(vk::gxfront::kVertexShader, reinterpret_cast<void*>(vsId));
        fake.StateP(vk::gxfront::kPixelShader, reinterpret_cast<void*>(psId));
        fake.State(vk::gxfront::kBlendingMode, 2);
        // The world's draw: a client program the renderer has none of.
        const uint64_t terrainId = 0x7200;

        Writer f;
        wxl_gx_capture_header header{};
        header.magic              = WXL_GX_CAPTURE_MAGIC;
        header.version            = WXL_GX_CAPTURE_VERSION;
        header.header_size        = sizeof header;
        header.pointer_size       = sizeof(void*);
        header.device_layout_hash = engine::CGxDevice::kLayoutHash;
        header.device_size        = sizeof(engine::CGxDevice);
        header.tex_size           = sizeof(engine::CGxTex);
        header.buf_size           = sizeof(engine::CGxBuf);
        header.state_size         = sizeof(wxl_gx_app_state);
        header.width              = w;
        header.height             = h;
        header.api                = 1;
        f.Put(&header, sizeof header);

        auto shader = [&](uint64_t id, uint32_t target, const char* name) {
            const size_t s = f.Begin(WXL_GX_CAPTURE_SHADER);
            f.Put64(id);
            f.Put32(target);
            f.Put32(static_cast<uint32_t>(std::strlen(name)));
            f.Put(name, std::strlen(name));
            f.Put32(0);
            f.End(s);
        };
        shader(vsId, 0, "UI:0");
        shader(psId, 4, "UI");
        shader(terrainId, 4, "Terrain3:17");

        size_t s = f.Begin(WXL_GX_CAPTURE_POOL);
        f.Put64(poolId);
        f.Put32(draw::kVertexPool);
        f.Put32(draw::kStream);
        f.Put32(4096);
        f.Put32(3);
        f.End(s);

        // The texture: 2x2 red, green, blue, white.
        const uint32_t texels[4] = {Argb(255, 0, 0), Argb(0, 255, 0), Argb(0, 0, 255), Argb(255, 255, 255)};
        s                        = f.Begin(WXL_GX_CAPTURE_TEXTURE);
        f.Put64(texId);
        f.Put(&tex, sizeof tex);
        f.Put32(1);
        for (uint32_t v : {0u, 0u, 2u, 2u, 8u, static_cast<uint32_t>(draw::kTexArgb8888), 16u}) f.Put32(v);
        f.Put(texels, sizeof texels);
        f.End(s);

        // The interface's c0..c3.
        const float rows[16] = {2.0f / w, 0, 0, -1.0f, 0, -2.0f / h, 0, 1.0f, 0, 0, 1.0f, 0, 0, 0, 0, 1.0f};
        s                    = f.Begin(WXL_GX_CAPTURE_CONSTANTS);
        f.Put32(0);
        f.Put32(0);
        f.Put32(4);
        f.Put(rows, sizeof rows);
        f.End(s);

        // A clear to grey over the whole viewport.
        f.Region(WXL_GX_CAPTURE_REGION_DEVICE, &fake.device, sizeof fake.device);
        f.Region(WXL_GX_CAPTURE_REGION_STATES, fake.states, sizeof fake.states);
        s = f.Begin(WXL_GX_CAPTURE_CLEAR);
        f.Put32(3);
        f.Put32(Argb(64, 64, 64));
        f.End(s);

        // The vertices: the texture over the top left 2x2, half transparent over the right 2x2.
        Vertex quads[12];
        Quad(quads, 0, 0, 2, 2, Argb(255, 255, 255, 255));
        Quad(quads + 6, 2, 0, 4, 2, Argb(255, 255, 255, 128));
        s = f.Begin(WXL_GX_CAPTURE_DATA);
        f.Put64(poolId);
        f.Put32(0);
        f.Put32(sizeof quads);
        f.Put32(1);
        f.Put(quads, sizeof quads);
        f.End(s);
        s = f.Begin(WXL_GX_CAPTURE_BUFFER);
        f.Put64(bufId);
        f.Put64(poolId);
        f.Put(&fake.buffers[0], sizeof(engine::CGxBuf));
        f.End(s);
        const wxl_gx_batch batch{3, 0, 12, 0, 11};
        s = f.Begin(WXL_GX_CAPTURE_DRAW);
        f.Put(&batch, sizeof batch);
        f.Put32(0);
        f.End(s);

        // A draw of the world, with a program the renderer lacks: left out.
        fake.StateP(vk::gxfront::kPixelShader, reinterpret_cast<void*>(terrainId));
        f.Region(WXL_GX_CAPTURE_REGION_STATES, fake.states, sizeof fake.states);
        s = f.Begin(WXL_GX_CAPTURE_DRAW);
        f.Put(&batch, sizeof batch);
        f.Put32(0);
        f.End(s);

        // What D3D9 showed: grey, the texels, then the texels at half over grey on the right.
        std::vector<uint32_t> shot(static_cast<size_t>(w) * h, Argb(64, 64, 64));
        shot[0] = texels[0];
        shot[1] = texels[1];
        shot[w] = texels[2];
        shot[w + 1] = texels[3];
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t x = 2 + (i % 2), y = i / 2;
            shot[y * w + x] = BlendReference(2, (texels[i] & 0xFFFFFF) | 0x80000000u, Argb(64, 64, 64), 0.0f) | 0xFF000000u;
        }
        s = f.Begin(WXL_GX_CAPTURE_SHOT);
        f.Put32(w);
        f.Put32(h);
        f.Put(shot.data(), shot.size() * 4);
        f.End(s);
        s = f.Begin(WXL_GX_CAPTURE_END);
        f.End(s);

        if (!live.Begin()) return;
        wxl::gx::capture::Replay replay;
        std::string              why;
        const bool               ran = replay.Run(f.out.data(), f.out.size(), live.renderer, why);
        if (!ran) std::printf("  replay: %s\n", why.c_str());
        std::vector<uint32_t> p = live.End();
        const auto&           stats = replay.Counters();
        Check(ran && stats.draws == 2 && stats.recorded == 1 && stats.verdicts[static_cast<int>(vk::gxfront::Verdict::Program)] == 1 &&
                  stats.programs == 2 && stats.textures == 1 && stats.clears == 1,
              "a capture's shaders, pool, texture, clear and draws replayed; a draw with a refused record counted and left out");
        // The back buffer's alpha is not shown: compared on rgb.
        const wxl::gx::capture::Comparison c = wxl::gx::capture::Compare(p, replay.Shot(), 2);
        Check(ran && c.pixels == static_cast<uint64_t>(w) * h && c.matching == c.pixels, "the replayed frame matches its SHOT on every pixel");
        if (c.matching != c.pixels) std::printf("  %llu of %llu pixels match, worst %u\n", static_cast<unsigned long long>(c.matching),
                                                static_cast<unsigned long long>(c.pixels), c.worst);
        replay.Release();
    }
}

namespace wxl::vk::test
{
    // --- V3: the world's work --------------------------------------------------------------------------

    namespace a = wxl::vk::test::dx9asm;

    /// vs_3_0: the position through c0..c3 (as the interface's projection), the colour times c4.
    std::vector<uint8_t> WorldVs()
    {
        using a::Reg;
        a::Asm v(false, 3, 0);
        v.Dcl(0, 0, a::Dst(Reg::Input, 0));
        v.Dcl(10, 0, a::Dst(Reg::Input, 1));
        v.Dcl(0, 0, a::Dst(Reg::Output, 0));
        v.Dcl(10, 0, a::Dst(Reg::Output, 1));
        v.Def(5, 0, 0, 0, 1);
        v.Op(wxl::vk::shaders::dx9::kMov, {a::Dst(Reg::Temp, 0), a::Src(Reg::Const, 5)});
        v.Op(wxl::vk::shaders::dx9::kMov, {a::Dst(Reg::Temp, 0, 0x7), a::Src(Reg::Input, 0)});
        for (uint32_t row = 0; row < 4; ++row)
            v.Op(wxl::vk::shaders::dx9::kDp4, {a::Dst(Reg::Output, 0, 1u << row), a::Src(Reg::Temp, 0), a::Src(Reg::Const, row)});
        v.Op(wxl::vk::shaders::dx9::kMul, {a::Dst(Reg::Output, 1), a::Src(Reg::Input, 1), a::Src(Reg::Const, 4)});
        return v.Bytes();
    }

    /// ps_3_0: the colour times c0 of the pixel registers.
    std::vector<uint8_t> WorldPs()
    {
        using a::Reg;
        a::Asm p(true, 3, 0);
        p.Dcl(10, 0, a::Dst(Reg::Input, 0));
        p.Op(wxl::vk::shaders::dx9::kMul, {a::Dst(Reg::ColorOut, 0), a::Src(Reg::Input, 0), a::Src(Reg::Const, 0)});
        return p.Bytes();
    }

    void WorldCases(Live& live, FakeEngine& fake, draw::Pool* pool)
    {
        const float w = static_cast<float>(live.width), h = static_cast<float>(live.height);
        Vertex      quad[6];
        uint32_t    at = 0;

        // Client programs translated, the engine's registers changing between two draws.
        const std::vector<uint8_t> vs = WorldVs(), ps = WorldPs();
        const int vp = live.renderer.Program(false, "World:0", vs.data(), vs.size());
        const int pp = live.renderer.Program(true, "World", ps.data(), ps.size());
        Check(vp >= static_cast<int>(vk::pipelines::VertexProgram::Count) && pp >= static_cast<int>(vk::pipelines::PixelProgram::Count) &&
                  live.renderer.Program(false, "World:0", vs.data(), vs.size()) == vp,
              "client records become programs, once each");
        std::string why;
        Check(live.renderer.Program(false, "Bad", ps.data(), ps.size(), &why) < 0 && !why.empty(), "a pixel record where a vertex one is wanted is refused");
        if (vp >= 0 && pp >= 0 && live.Begin())
        {
            const float projection[16] = {2.0f / w, 0, 0, -1.0f, 0, -2.0f / h, 0, 1.0f, 0, 0, 1.0f, 0, 0, 0, 0, 1.0f};
            const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, white[4] = {1, 1, 1, 1};
            live.renderer.Constants(false, 0, 4, projection);
            live.renderer.Constants(false, 4, 1, red);
            live.renderer.Constants(true, 0, 1, white);
            fake.programs[1] = vp;
            fake.programs[0] = pp;
            fake.StateP(vk::gxfront::kVertexShader, &fake);
            fake.StateP(vk::gxfront::kPixelShader, &fake);
            Quad(quad, 0, 0, 2, 2, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            const bool first = Record(live, fake, 6);
            live.renderer.Constants(false, 4, 1, green);
            Quad(quad, 2, 0, 4, 2, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            const bool second = Record(live, fake, 6);
            const std::vector<uint32_t> p = live.End();
            Check(first && second && !p.empty() && p[0] == Argb(255, 0, 0) && p[3] == Argb(0, 255, 0),
                  "translated programs draw with the registers each draw had (red, then green)");
            fake.StateP(vk::gxfront::kVertexShader, nullptr);
            fake.StateP(vk::gxfront::kPixelShader, nullptr);
            fake.programs[0] = fake.programs[1] = -1;
        }

        // A texture target: cleared, drawn into, then sampled in the scene.
        draw::TextureDesc desc;
        desc.width  = 4;
        desc.height = 4;
        desc.target = true;
        draw::Texture* target = live.renderer.CreateTexture(desc);
        if (target && live.Begin(Argb(0, 0, 0)))
        {
            fake.device.m_textureTarget[0].m_texture = reinterpret_cast<engine::CGxTex*>(&fake.tex_fields[1]);
            fake.device.m_curWindowRect = {0.0f, 0.0f, 4.0f, 4.0f};
            float* p = &fake.device.m_projNative.a0;
            p[0] = 2.0f / 4.0f;
            p[5] = -2.0f / 4.0f;
            live.renderer.SetTarget(target, 0, nullptr);
            int32_t rect[4];
            vk::gxfront::ClearRect(fake.device, rect);
            const float blue[4] = {0, 0, 1, 1};
            live.renderer.Clear(true, true, blue, 1.0f, {{rect[0], rect[1]}, {static_cast<uint32_t>(rect[2]), static_cast<uint32_t>(rect[3])}});
            // White over the target's top half (D3D9's rows 0 and 1 of the texture).
            Quad(quad, 0, 0, 4, 2, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            const bool into = Record(live, fake, 6);
            // Back to the scene: the target over its 4x4 top left, texel for texel.
            fake.device.m_textureTarget[0].m_texture = nullptr;
            fake.device.m_curWindowRect = {0.0f, 0.0f, h, w};
            p[0] = 2.0f / w;
            p[5] = -2.0f / h;
            live.renderer.SetTarget(nullptr, 0, nullptr);
            fake.textures[0]                   = target;
            fake.tex_fields[0].m_flags.m_filter = 0;
            fake.StateP(vk::gxfront::kTexture0, &fake.tex_fields[0]);
            fake.State(vk::gxfront::kColorOp0, 0);
            fake.State(vk::gxfront::kAlphaOp0, 0);
            Quad(quad, 0, 0, 4, 4, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            const bool back = Record(live, fake, 6);
            const std::vector<uint32_t> px = live.End();
            Check(into && back && !px.empty() && px[0] == Argb(255, 255, 255) && px[live.width * 1 + 3] == Argb(255, 255, 255) &&
                      px[live.width * 2] == Argb(0, 0, 255) && px[live.width * 3 + 3] == Argb(0, 0, 255),
                  "a texture target cleared and drawn into reads back in the scene, its rows the right way up");
            Check(live.renderer.Counters().target_renderings >= 1, "the target was drawn into by a rendering of its own");
        }

        // A copy from the scene (D3D9's TexCopy and TexStretch from the back buffer).
        desc.width = desc.height = 2;
        draw::Texture* copy = live.renderer.CreateTexture(desc);
        if (copy && live.Begin(Argb(0, 0, 0)))
        {
            fake.StateP(vk::gxfront::kTexture0, nullptr);
            Quad(quad, 0, 0, 1, 1, Argb(255, 0, 0));
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            Quad(quad, 1, 1, 2, 2, Argb(0, 255, 0));
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            const bool copied = live.renderer.CopyScene(copy, 0, 0, {{0, 0}, {2, 2}}, {{0, 0}, {2, 2}}, false);
            fake.textures[0] = copy;
            fake.StateP(vk::gxfront::kTexture0, &fake.tex_fields[0]);
            Quad(quad, 4, 0, 6, 2, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            const std::vector<uint32_t> px = live.End();
            Check(copied && !px.empty() && px[4] == Argb(255, 0, 0) && px[live.width + 5] == Argb(0, 255, 0) && px[5] == Argb(0, 0, 0),
                  "a copy from the scene into a texture, drawn back texel for texel");
            fake.StateP(vk::gxfront::kTexture0, nullptr);
            fake.textures[0] = nullptr;
        }

        // An occlusion query: the samples of a 2x2 quad that passes the depth test, then of one that fails it.
        if (live.renderer.QueriesSupported())
        {
            const uint32_t query = live.renderer.CreateQuery();
            uint32_t       samples = 99;
            Check(live.renderer.QueryResult(query, samples) && samples == 0, "a query never issued reads as known, no samples");
            if (live.Begin())
            {
                fake.State(vk::gxfront::kDepthTest, 1);
                fake.State(vk::gxfront::kDepthWrite, 1);
                live.renderer.BeginQuery(query);
                Quad(quad, 0, 0, 2, 2, Argb(255, 255, 255));
                Stream(live, fake, pool, quad, 6, at);
                Record(live, fake, 6);
                live.renderer.EndQuery(query);
                Check(!live.renderer.QueryResult(query, samples), "a query of a frame not yet completed is not known");
                live.End();
                Check(live.renderer.QueryResult(query, samples) && samples == 4, "the query counts the quad's 4 samples once the frame completed");
            }
            if (live.Begin())
            {
                Quad(quad, 0, 0, 2, 2, Argb(255, 255, 255));
                Stream(live, fake, pool, quad, 6, at);
                Record(live, fake, 6);
                fake.State(vk::gxfront::kDepthFunc, 3); // LESS: the same depth fails
                live.renderer.BeginQuery(query);
                Stream(live, fake, pool, quad, 6, at);
                Record(live, fake, 6);
                live.renderer.EndQuery(query);
                live.End();
                Check(live.renderer.QueryResult(query, samples) && samples == 0, "a quad hidden by the depth test counts none");
                fake.State(vk::gxfront::kDepthFunc, 0);
            }
            fake.State(vk::gxfront::kDepthTest, 0);
            fake.State(vk::gxfront::kDepthWrite, 0);
            live.renderer.DestroyQuery(query);
        }

        // D3D9's fixed-function lighting: a light's ambient on the vertex's colour (no normal).
        if (live.Begin())
        {
            auto* lights   = reinterpret_cast<wxl_gx_light*>(fake.device.m_lights);
            lights[0]      = wxl_gx_light{};
            lights[0].dir[2] = 1.0f;
            lights[0].ambient[0] = 0.25f;
            lights[0].ambient[1] = 0.5f;
            lights[0].diffuse[0] = lights[0].diffuse[1] = lights[0].diffuse[2] = 1.0f;
            lights[0].enable = 1;
            fake.State(vk::gxfront::kLighting, 1);
            Quad(quad, 0, 0, 2, 2, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            fake.State(vk::gxfront::kLighting, 0);
            lights[0].enable = 0;
            const std::vector<uint32_t> px = live.End();
            Check(!px.empty() && Near(px[0], Argb(64, 128, 0), 1), "fixed-function lighting: the light's ambient times the vertex's colour");
        }

        // D3D9's linear vertex fog: at z 0.5 between 0 and 1, half the fog colour.
        if (live.Begin())
        {
            fake.State(vk::gxfront::kFog, 1);
            fake.StateF(vk::gxfront::kFogStart, 0.0f);
            fake.StateF(vk::gxfront::kFogEnd, 1.0f);
            fake.State(vk::gxfront::kFogColor, static_cast<int32_t>(Argb(0, 0, 255)));
            Quad(quad, 0, 0, 2, 2, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            fake.State(vk::gxfront::kFog, 0);
            const std::vector<uint32_t> px = live.End();
            Check(!px.empty() && Near(px[0], Argb(128, 128, 255), 1), "fixed-function fog: half white, half the fog colour");
        }

        // The user clip plane (world space under fixed function): z 0.5 kept by z <= 0.6, clipped by z <= 0.4.
        if (live.renderer.Screen().FrameCount() && live.Begin(Argb(0, 0, 0)))
        {
            fake.device.m_caps.m_maxClipPlanes = 1;
            fake.State(vk::gxfront::kClipPlaneMask, 1);
            fake.device.m_clipPlane[0] = {{0.0f, 0.0f, -1.0f}, 0.6f};
            Quad(quad, 0, 0, 2, 2, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            fake.device.m_clipPlane[0] = {{0.0f, 0.0f, -1.0f}, 0.4f};
            Quad(quad, 2, 0, 4, 2, Argb(255, 255, 255));
            Stream(live, fake, pool, quad, 6, at);
            Record(live, fake, 6);
            fake.State(vk::gxfront::kClipPlaneMask, 0);
            const std::vector<uint32_t> px = live.End();
            if (live.context.Chosen().features.shaderClipDistance)
                Check(!px.empty() && px[0] == Argb(255, 255, 255) && px[2] == Argb(0, 0, 0),
                      "the user clip plane keeps what is in front of it and clips what is behind");
        }
        live.renderer.DestroyTexture(target);
        live.renderer.DestroyTexture(copy);
    }

    /// A capture of V3's work: translated programs (their records in the SHDR chunks), a texture
    /// target cleared, a copy from the back buffer, both drawn back with fixed function.
    void ReplayWorldCases(Live& live)
    {
        const uint32_t w = live.width, h = live.height;
        FakeEngine     fake;
        fake.Reset(w, h);
        const uint64_t bufId = reinterpret_cast<uint64_t>(&fake.buffers[0]), poolId = 0x5100, vsId = 0x8000, psId = 0x8100,
                       copyId = 0x8200, targetId = 0x8300;
        fake.buffers[0].m_pool  = reinterpret_cast<engine::CGxPool*>(poolId);
        fake.buffers[0].m_index = 0;

        Writer f;
        wxl_gx_capture_header header{};
        header.magic              = WXL_GX_CAPTURE_MAGIC;
        header.version            = WXL_GX_CAPTURE_VERSION;
        header.header_size        = sizeof header;
        header.pointer_size       = sizeof(void*);
        header.device_layout_hash = engine::CGxDevice::kLayoutHash;
        header.device_size        = sizeof(engine::CGxDevice);
        header.tex_size           = sizeof(engine::CGxTex);
        header.buf_size           = sizeof(engine::CGxBuf);
        header.state_size         = sizeof(wxl_gx_app_state);
        header.width              = w;
        header.height             = h;
        header.api                = 1;
        f.Put(&header, sizeof header);

        auto shader = [&](uint64_t id, uint32_t target, const char* name, const std::vector<uint8_t>& code) {
            const size_t s = f.Begin(WXL_GX_CAPTURE_SHADER);
            f.Put64(id);
            f.Put32(target);
            f.Put32(static_cast<uint32_t>(std::strlen(name)));
            f.Put(name, std::strlen(name));
            f.Put32(static_cast<uint32_t>(code.size()));
            f.Put(code.data(), code.size());
            f.End(s);
        };
        shader(vsId, 0, "World:0", WorldVs());
        shader(psId, 4, "World", WorldPs());
        size_t s = f.Begin(WXL_GX_CAPTURE_POOL);
        f.Put64(poolId);
        f.Put32(draw::kVertexPool);
        f.Put32(draw::kStream);
        f.Put32(4096);
        f.Put32(3);
        f.End(s);
        auto texture = [&](uint64_t id, uint32_t size) {
            engine::CGxTex t{};
            t.m_width  = size;
            t.m_height = size;
            t.m_format = draw::kTexArgb8888;
            t.m_flags.m_renderTarget = 1;
            const size_t c = f.Begin(WXL_GX_CAPTURE_TEXTURE);
            f.Put64(id);
            f.Put(&t, sizeof t);
            f.Put32(0);
            f.End(c);
        };
        texture(copyId, 2);
        auto constants = [&](uint32_t target, uint32_t first, const float* rows, uint32_t count) {
            const size_t c = f.Begin(WXL_GX_CAPTURE_CONSTANTS);
            f.Put32(target);
            f.Put32(first);
            f.Put32(count);
            f.Put(rows, count * 16);
            f.End(c);
        };
        const float projection[20] = {2.0f / w, 0, 0, -1.0f, 0, -2.0f / h, 0, 1.0f, 0, 0, 1.0f, 0, 0, 0, 0, 1.0f, 1, 0, 0, 1};
        const float white[4]       = {1, 1, 1, 1};
        constants(0, 0, projection, 5);
        constants(1, 0, white, 1);

        // Vertices: a quad at (0,0)-(2,2) for the programs, (4,0)-(8,4) for the target, (8,0)-(10,2) for the copy.
        Vertex quads[18];
        Quad(quads, 0, 0, 2, 2, Argb(255, 255, 255));
        Quad(quads + 6, 4, 0, 8, 4, Argb(255, 255, 255));
        Quad(quads + 12, 8, 0, 10, 2, Argb(255, 255, 255));
        s = f.Begin(WXL_GX_CAPTURE_DATA);
        f.Put64(poolId);
        f.Put32(0);
        f.Put32(sizeof quads);
        f.Put32(1);
        f.Put(quads, sizeof quads);
        f.End(s);
        auto buffer = [&](uint32_t index) {
            fake.buffers[0].m_index = index;
            const size_t c          = f.Begin(WXL_GX_CAPTURE_BUFFER);
            f.Put64(bufId);
            f.Put64(poolId);
            f.Put(&fake.buffers[0], sizeof(engine::CGxBuf));
            f.End(c);
        };
        auto state = [&]() {
            f.Region(WXL_GX_CAPTURE_REGION_DEVICE, &fake.device, sizeof fake.device);
            f.Region(WXL_GX_CAPTURE_REGION_STATES, fake.states, sizeof fake.states);
        };
        const wxl_gx_batch batch{3, 0, 6, 0, 5};
        auto drawChunk = [&]() {
            const size_t c = f.Begin(WXL_GX_CAPTURE_DRAW);
            f.Put(&batch, sizeof batch);
            f.Put32(0);
            f.End(c);
        };

        // Black, then the programs' red quad.
        state();
        s = f.Begin(WXL_GX_CAPTURE_CLEAR);
        f.Put32(3);
        f.Put32(Argb(0, 0, 0));
        f.End(s);
        fake.StateP(vk::gxfront::kVertexShader, reinterpret_cast<void*>(vsId));
        fake.StateP(vk::gxfront::kPixelShader, reinterpret_cast<void*>(psId));
        buffer(0);
        state();
        drawChunk();
        // TexCopy of the back buffer's (0,0)-(2,2) into the 2x2 texture.
        s = f.Begin(WXL_GX_CAPTURE_COPY);
        f.Put32(0);
        f.Put64(copyId);
        for (int32_t v : {0, 0, 0, 0, 2, 2, 0, 0}) f.Put32(static_cast<uint32_t>(v));
        f.End(s);
        // The 4x4 target, named first by the RTGT: cleared to blue.
        fake.device.m_textureTarget[0].m_texture = reinterpret_cast<engine::CGxTex*>(targetId);
        fake.device.m_curWindowRect              = {0.0f, 0.0f, 4.0f, 4.0f};
        state();
        s = f.Begin(WXL_GX_CAPTURE_TARGET);
        f.Put32(0);
        f.Put64(targetId);
        f.Put32(0);
        f.End(s);
        s = f.Begin(WXL_GX_CAPTURE_CLEAR);
        f.Put32(1);
        f.Put32(Argb(0, 0, 255));
        f.End(s);
        fake.device.m_textureTarget[0].m_texture = nullptr;
        fake.device.m_curWindowRect              = {0.0f, 0.0f, static_cast<float>(h), static_cast<float>(w)};
        state();
        s = f.Begin(WXL_GX_CAPTURE_TARGET);
        f.Put32(0);
        f.Put64(0);
        f.Put32(0);
        f.End(s);
        // Fixed function: the target, then the copy, texel for texel.
        fake.StateP(vk::gxfront::kVertexShader, nullptr);
        fake.StateP(vk::gxfront::kPixelShader, nullptr);
        fake.State(vk::gxfront::kColorOp0, 0);
        fake.State(vk::gxfront::kAlphaOp0, 0);
        fake.StateP(vk::gxfront::kTexture0, reinterpret_cast<void*>(targetId));
        buffer(6 * sizeof(Vertex));
        state();
        drawChunk();
        texture(copyId, 2); // the capture reads the copied texture back when a draw names it: the replay keeps its own
        fake.StateP(vk::gxfront::kTexture0, reinterpret_cast<void*>(copyId));
        buffer(12 * sizeof(Vertex));
        state();
        drawChunk();

        std::vector<uint32_t> shot(static_cast<size_t>(w) * h, Argb(0, 0, 0));
        for (uint32_t y = 0; y < 2; ++y)
            for (uint32_t x = 0; x < 2; ++x) shot[y * w + x] = shot[y * w + 8 + x] = Argb(255, 0, 0);
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 4; x < 8; ++x) shot[y * w + x] = Argb(0, 0, 255);
        s = f.Begin(WXL_GX_CAPTURE_SHOT);
        f.Put32(w);
        f.Put32(h);
        f.Put(shot.data(), shot.size() * 4);
        f.End(s);
        s = f.Begin(WXL_GX_CAPTURE_END);
        f.End(s);

        if (!live.Begin()) return;
        wxl::gx::capture::Replay replay;
        std::string              why;
        const bool               ran = replay.Run(f.out.data(), f.out.size(), live.renderer, why);
        if (!ran) std::printf("  replay: %s\n", why.c_str());
        std::vector<uint32_t> p     = live.End();
        const auto&           stats = replay.Counters();
        Check(ran && stats.draws == 3 && stats.recorded == 3 && stats.programs == 2 && stats.targets == 2 && stats.copies == 1 &&
                  stats.copied == 1,
              "a capture's translated programs, target and copy replayed");
        const wxl::gx::capture::Comparison c = wxl::gx::capture::Compare(p, replay.Shot(), 0);
        if (c.matching != c.pixels)
            std::printf("  %llu of %llu pixels match, worst %u\n", static_cast<unsigned long long>(c.matching),
                        static_cast<unsigned long long>(c.pixels), c.worst);
        Check(ran && c.pixels && c.matching == c.pixels, "the replayed world frame matches its SHOT on every pixel");
        replay.Release();
    }

    void DrawPureCases()
    {
        BlendTableCases();
        SamplerCases();
        FormatPitchCases();
        EngineCases();
    }

    bool DrawLiveCases()
    {
        char why[256] = {};
        Live live;
        if (!live.Make(16, 8, why, sizeof why))
        {
            std::printf("  no renderer: %s\n", why);
            Check(false, "a renderer is made on the machine's device");
            return false;
        }
        Check(live.renderer.Ready(), "a renderer is made on the machine's device");
        FakeEngine fake;
        fake.Reset(live.width, live.height);
        draw::Pool* pool = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 4096);
        fake.pools[0]    = pool;

        FormatCases(live, fake, pool);
        BlendCases(live, fake, pool);
        StageCases(live, fake, pool);
        ProgramCases(live, fake, pool);
        PoolAndUploadCases(live, fake, pool);
        fake.Reset(live.width, live.height);
        fake.pools[0] = pool;
        WorldCases(live, fake, pool);
        live.renderer.DestroyPool(pool);
        ReplayCases(live);
        ReplayWorldCases(live);

        const draw::Renderer::Stats& s = live.renderer.Counters();
        std::printf("  renderer: %llu draws, %llu pipelines, %llu pass breaks, %llu bytes uploaded, %llu refused\n",
                    static_cast<unsigned long long>(s.draws), static_cast<unsigned long long>(s.pipelines),
                    static_cast<unsigned long long>(s.pass_breaks), static_cast<unsigned long long>(s.upload_bytes),
                    static_cast<unsigned long long>(s.refused));
        Check(s.refused == 0, "no draw refused for want of memory or a pipeline");
        return true;
    }
}
