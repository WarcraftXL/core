// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What the renderer's draw tests share: a device of the engine made up (its states, buffers,
// textures and programs answered by a Resolver), a renderer drawing into an offscreen image on
// the machine's driver, and the helpers that write a draw into it and read the image back.

#pragma once

#include "gx/vk/draw/Draw.hpp"
#include "gx/vk/gxfront/Engine.hpp"
#include "gx/vk/present/Display.hpp"

#include "Check.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace wxl::vk::test::rig
{
    namespace vk     = wxl::vk;
    namespace draw   = wxl::vk::draw;
    namespace engine = wxl::engine;

    inline uint32_t Argb(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) { return a << 24 | r << 16 | g << 8 | b; }

    inline bool Near(uint32_t a, uint32_t b, uint32_t tolerance, bool alpha = true)
    {
        for (int shift = 0; shift < (alpha ? 32 : 24); shift += 8)
        {
            const int32_t d = static_cast<int32_t>((a >> shift) & 0xFF) - static_cast<int32_t>((b >> shift) & 0xFF);
            if (static_cast<uint32_t>(d < 0 ? -d : d) > tolerance) return false;
        }
        return true;
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

    // --- Live ------------------------------------------------------------------------------------------

    struct Live
    {
        vk::device::Context   context;
        vk::resources::Allocator allocator;
        vk::frame::Timeline   timeline;
        vk::present::Display  display;
        draw::Renderer        renderer;
        uint32_t              width = 0, height = 0;

        bool Make(uint32_t w, uint32_t h, char* why, uint32_t size, const draw::Renderer::Paths& paths = {})
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
            return renderer.Create(context, allocator, display, paths, why, size);
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
    inline void Quad(Vertex* out, float x0, float y0, float x1, float y1, uint32_t color, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1)
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
    inline void Stream(Live& live, FakeEngine& fake, draw::Pool* pool, const Vertex* vertices, uint32_t count, uint32_t& at)
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
    inline bool Record(Live& live, FakeEngine& fake, uint32_t count)
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
    inline draw::Texture* MakeTexture(Live& live, uint32_t format, uint32_t w, uint32_t h, const void* bytes, uint32_t pitch)
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

}
