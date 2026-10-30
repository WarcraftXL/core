// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What the renderer costs the main thread, measured on this machine's driver (lavapipe here: the
// CPU side is what it measures well). A made-up zone drawn frame after frame through the engine
// reading (gxfront/Engine.cpp) and the renderer (draw/), as the client drives them: the client's
// own programs translated from their records (a corpus folder laid out as the archives' Shaders\),
// the registers written between draws as the M2, map object and terrain renderers write them,
// static pools and textures made as a zone loads, a stream pool for the interface.
//
//   wxl-vk-bench [--corpus <folder>] [--draws N] [--frames N] [--cache <folder>] [--quick]
//
// Prints, for the steady frames: the main thread's time per frame and per draw in the engine
// reading and in the renderer, and how many Vulkan commands a draw records; for the frame a zone
// loads in (textures and pools made, filled and drawn at once): the main thread's time in the
// renderer and the GPU's in the frame's upload pass; for a cache folder given twice in a row (two
// launches): the draws left out while their pipelines compiled, and the main thread's stalls.
// Exit 0 when it ran, 77 without a Vulkan 1.3 device.

#include "gx/vk/Log.hpp"
#include "gx/vk/draw/Draw.hpp"
#include "gx/vk/gxfront/Engine.hpp"
#include "gx/vk/present/Display.hpp"

#include "Rig.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    namespace vk     = wxl::vk;
    namespace draw   = wxl::vk::draw;
    namespace engine = wxl::engine;
    namespace fs     = std::filesystem;
    namespace rig    = wxl::vk::test::rig;
    using Clock      = std::chrono::steady_clock;

    double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

    void Quiet(vk::LogLevel level, const char* text, void*)
    {
        if (level == vk::LogLevel::Warn || level == vk::LogLevel::Error) std::printf("  [vk] %s\n", text);
    }

    /// The records of one .bls container (their bytes), the first @p most of them.
    std::vector<std::vector<uint8_t>> Records(const fs::path& file, size_t most)
    {
        std::vector<std::vector<uint8_t>> out;
        std::ifstream                     in(file, std::ios::binary);
        std::vector<uint8_t>              data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.size() < 12) return out;
        uint32_t count = 0;
        std::memcpy(&count, data.data() + 8, 4);
        size_t at = 12;
        for (uint32_t r = 0; r < count && at + 16 <= data.size() && out.size() < most; ++r)
        {
            uint32_t size = 0;
            std::memcpy(&size, data.data() + at + 12, 4);
            at += 16;
            if (at + size > data.size()) break;
            out.emplace_back(data.begin() + static_cast<ptrdiff_t>(at), data.begin() + static_cast<ptrdiff_t>(at + size));
            at = (at + size + 3) & ~size_t(3);
        }
        return out;
    }

    /// The engine's objects the bench draws with, looked up as the live client's front looks them up
    /// (a set of the objects it made, then the object's own pointer to the renderer's).
    struct Engine final : vk::gxfront::Resolver
    {
        engine::CGxDevice              device{};
        wxl_gx_app_state               states[vk::gxfront::kRenderStates]{};
        float                          constants[256 * 4] = {};
        std::vector<engine::CGxBuf>    buffers;
        std::vector<engine::CGxPool>   pools;
        std::vector<engine::CGxTex>    textures;
        std::vector<draw::Texture*>    made;
        std::unordered_set<const void*> known_pools, known_textures;
        std::vector<int>               programs; ///< by a made-up shader pointer: (index + 1)
        std::unordered_set<const void*> known_shaders;

        const engine::CGxBuf* Buffer(const void* buf) override { return static_cast<const engine::CGxBuf*>(buf); }
        draw::Pool* Pool(const void* pool) override
        {
            if (!pool || !known_pools.count(pool)) return nullptr;
            return static_cast<draw::Pool*>(static_cast<const engine::CGxPool*>(pool)->m_apiSpecific);
        }
        draw::Texture* Texture(const void* texture, const engine::CGxTex*& fields) override
        {
            fields = static_cast<const engine::CGxTex*>(texture);
            if (!texture || !known_textures.count(texture)) return nullptr;
            return made[static_cast<size_t>(fields - textures.data())];
        }
        int Program(const void* shader, bool) override
        {
            if (!known_shaders.count(shader)) return -1;
            return programs[reinterpret_cast<uintptr_t>(shader) - 1];
        }
        void State(uint32_t which, int32_t value) { states[which].value.data.p = reinterpret_cast<void*>(static_cast<intptr_t>(value)); }
        void StateP(uint32_t which, const void* value) { states[which].value.data.p = const_cast<void*>(value); }
    };

    struct Program
    {
        bool                 pixel = false;
        std::string          name;
        std::vector<uint8_t> code;
    };

    /// The client's programs the zone draws with: vertex and pixel records of its M2, map object and
    /// terrain shaders from the corpus (the renderer's own fixed function without one).
    std::vector<Program> Corpus(const std::string& folder)
    {
        std::vector<Program> out;
        if (folder.empty()) return out;
        const struct
        {
            const char* file;
            bool        pixel;
            size_t      most;
        } wanted[] = {
            {"Vertex/vs_3_0/Diffuse_T1.bls", false, 6},     {"Vertex/vs_3_0/Diffuse_T1_T2.bls", false, 4},
            {"Vertex/vs_3_0/MapObjDiffuse_T1.bls", false, 4}, {"Vertex/vs_3_0/Terrain.bls", false, 2},
            {"Pixel/ps_3_0/Combiners_Mod.bls", true, 4},    {"Pixel/ps_3_0/Combiners_Opaque.bls", true, 4},
            {"Pixel/ps_3_0/Combiners_Mod_Mod.bls", true, 4}, {"Pixel/ps_3_0/MapObjDiffuse.bls", true, 4},
            {"Pixel/ps_3_0/Terrain1.bls", true, 2},
        };
        for (const auto& w : wanted)
        {
            const auto records = Records(fs::path(folder) / w.file, w.most);
            for (size_t i = 0; i < records.size(); ++i)
                out.push_back(Program{w.pixel, std::string(fs::path(w.file).stem().string()) + ":" + std::to_string(i), records[i]});
        }
        return out;
    }

    struct Options
    {
        std::string corpus;
        std::string cache;
        uint32_t    draws = 700;
        uint32_t    frames = 300;
        uint32_t    width = 640, height = 360;
        bool        quick = false;
        bool        rebar = false; ///< buffers in mapped device-local memory, pools written in place
    };

    /// A zone: its programs, textures and pools, made in the bench's renderer.
    struct Zone
    {
        std::vector<int>             vertex, pixel;        ///< the client's programs' ids (own ones when none)
        std::vector<draw::Pool*>     vertex_pools, index_pools;
        draw::Pool*                  stream = nullptr;
        uint32_t                     stream_at = 0;
    };

    constexpr uint32_t kStride       = 24; // PCT
    constexpr uint32_t kPoolVertices = 2048;
    constexpr uint32_t kPoolIndices  = 6144;

    /// A static vertex pool and its index pool, filled (a model's buffers, made as the zone loads).
    void MakeModel(rig::Live& live, Engine& e, Zone& z)
    {
        (void)e;
        draw::Pool* v = live.renderer.CreatePool(draw::kVertexPool, draw::kStatic, kPoolVertices * kStride);
        draw::Pool* i = live.renderer.CreatePool(draw::kIndexPool, draw::kStatic, kPoolIndices * 2);
        uint8_t*    vm = live.renderer.PoolMemory(v);
        for (uint32_t k = 0; k < kPoolVertices; ++k)
        {
            rig::Vertex vx{static_cast<float>(k % 64) * 0.5f, static_cast<float>(k / 64) * 0.5f, 0.5f, 0xFF808080u, 0.0f, 0.0f};
            std::memcpy(vm + k * kStride, &vx, kStride);
        }
        live.renderer.PoolWritten(v, 0, kPoolVertices * kStride);
        auto* im = reinterpret_cast<uint16_t*>(live.renderer.PoolMemory(i));
        for (uint32_t k = 0; k < kPoolIndices; ++k) im[k] = static_cast<uint16_t>((k * 7) % kPoolVertices);
        live.renderer.PoolWritten(i, 0, kPoolIndices * 2);
        z.vertex_pools.push_back(v);
        z.index_pools.push_back(i);
    }

    /// A 256x256 DXT1 texture with its mips, its bytes written through Lock/Unlock as the client does.
    draw::Texture* MakeTexture(rig::Live& live, uint32_t seed)
    {
        draw::TextureDesc desc;
        desc.format = draw::kTexDxt1;
        desc.width  = 256;
        desc.height = 256;
        desc.levels = 9;
        draw::Texture* t = live.renderer.CreateTexture(desc);
        if (!t) return nullptr;
        for (uint32_t level = 0; level < desc.levels; ++level)
        {
            const uint32_t w = std::max(256u >> level, 1u), h = std::max(256u >> level, 1u);
            uint32_t       pitch = 0;
            uint8_t*       to    = live.renderer.LockTexture(t, 0, level, 0, 0, w, h, pitch);
            if (!to) continue;
            const uint32_t rows = draw::Formats::Rows(t->info, h);
            for (uint32_t r = 0; r < rows; ++r) std::memset(to + r * pitch, static_cast<int>((seed + r) & 0xFF), pitch);
            live.renderer.UnlockTexture(t);
        }
        return t;
    }

    /// Lays the engine's device out for the bench: the viewport, depth on, PCT vertices.
    void ResetDevice(Engine& e, uint32_t w, uint32_t h)
    {
        std::memset(&e.device, 0, sizeof e.device);
        std::memset(e.states, 0, sizeof e.states);
        e.device.m_curWindowRect = {0.0f, 0.0f, static_cast<float>(h), static_cast<float>(w)};
        const wxl_gx_viewport vp{0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
        std::memcpy(e.device.m_viewport, &vp, sizeof vp);
        e.device.m_appMasterEnables = 0x1FF;
        e.device.m_caps.m_depthBias = 1;
        e.State(vk::gxfront::kColorWrite, 0xF);
        e.State(vk::gxfront::kDepthTest, 1);
        e.State(vk::gxfront::kDepthWrite, 1);
        e.State(vk::gxfront::kMultisample, 1);
        for (auto* stack : {&e.device.m_xforms[0], &e.device.m_xforms[1], &e.device.m_xforms[8], &e.device.m_xforms[10], &e.device.m_texGen[0],
                            &e.device.m_texGen[1]})
        {
            float* m = &stack->m_mtx[0].a0;
            std::memset(m, 0, 64);
            m[0] = m[5] = m[10] = m[15] = 1.0f;
            stack->m_flags[0] = 1;
        }
        float* p = &e.device.m_projNative.a0;
        std::memset(p, 0, 64);
        p[0]  = 2.0f / static_cast<float>(w);
        p[5]  = -2.0f / static_cast<float>(h);
        p[10] = 1.0f;
        p[12] = -1.0f;
        p[13] = 1.0f;
        p[15] = 1.0f;
        auto* attribs = reinterpret_cast<wxl_gx_vertex_attrib*>(e.device.m_primVertexFormatAttrib);
        attribs[0]    = {0, 4, 0, 24};
        attribs[4]    = {4, 0, 12, 24};
        attribs[6]    = {6, 3, 16, 24};
        e.device.m_primVertexMask = (1u << 0) | (1u << 4) | (1u << 6);
    }

    struct FrameCost
    {
        double read_ms = 0.0;   ///< the engine reading (BuildDraw)
        double record_ms = 0.0; ///< the renderer's Draw
        double other_ms = 0.0;  ///< the registers written, frame begin and end
        uint32_t drawn = 0;
        uint32_t skipped = 0;
    };

    /// One frame of the zone: @p draws draws, 15 in 100 the interface's (own programs, the stream
    /// pool), the others the world's (the client's programs, static pools, bones and registers).
    FrameCost DrawFrame(rig::Live& live, Engine& e, Zone& z, uint32_t draws, uint32_t frame)
    {
        FrameCost       cost;
        const auto      begin = Clock::now();
        vk::frame::Wait wait{};
        if (!live.display.BeginFrame(wait)) return cost;
        const float black[4] = {0, 0, 0, 1};
        live.display.Clear(true, true, black, 1.0f, {{0, 0}, {live.width, live.height}});
        cost.other_ms += Ms(begin, Clock::now());
        const uint64_t skipped0 = live.renderer.Counters().skipped;

        // Zeros: every vertex of the client's programs lands on one point, so the driver rasterises
        // nothing and the frame measures the CPU (the copies cost the same whatever the values).
        static float bones[3 * 64 * 4] = {};
        (void)frame;
        const size_t programs = std::min(z.vertex.size(), z.pixel.size());
        for (uint32_t d = 0; d < draws; ++d)
        {
            const bool ui = (d % 100) >= 85 || !programs;
            auto       t0 = Clock::now();
            draw::Pool* vpool = nullptr;
            draw::Pool* ipool = nullptr;
            uint32_t    count = 0, start = 0;
            if (ui)
            {
                // The interface: a quad in the stream pool, the renderer's own programs.
                rig::Vertex quad[6];
                rig::Quad(quad, static_cast<float>(d % 50) * 8, static_cast<float>(d % 30) * 8, static_cast<float>(d % 50) * 8 + 6,
                          static_cast<float>(d % 30) * 8 + 6, 0xFFFFFFFFu);
                uint8_t* memory = live.renderer.PoolMemory(z.stream);
                if (z.stream_at + sizeof quad > z.stream->size)
                {
                    live.renderer.DiscardPool(z.stream);
                    memory      = live.renderer.PoolMemory(z.stream);
                    z.stream_at = 0;
                }
                std::memcpy(memory + z.stream_at, quad, sizeof quad);
                live.renderer.PoolWritten(z.stream, z.stream_at, sizeof quad);
                e.buffers[0].m_pool  = &e.pools[0];
                e.buffers[0].m_index = z.stream_at;
                z.stream_at += sizeof quad;
                vpool = z.stream;
                count = 6;
                e.StateP(vk::gxfront::kVertexShader, nullptr);
                e.StateP(vk::gxfront::kPixelShader, nullptr);
                e.State(vk::gxfront::kBlendingMode, 2);
                e.State(vk::gxfront::kDepthTest, 0);
                e.State(vk::gxfront::kDepthWrite, 0);
            }
            else
            {
                const size_t m = d % z.vertex_pools.size();
                e.buffers[0].m_pool  = &e.pools[1 + m * 2];
                e.buffers[0].m_index = 0;
                e.buffers[1].m_pool  = &e.pools[2 + m * 2];
                e.buffers[1].m_index = 0;
                vpool = z.vertex_pools[m];
                ipool = z.index_pools[m];
                start = (d * 3 % 200) * 3;
                count = 30;
                const size_t p = d % programs;
                e.StateP(vk::gxfront::kVertexShader, reinterpret_cast<const void*>(uintptr_t{1} + p));
                e.StateP(vk::gxfront::kPixelShader, reinterpret_cast<const void*>(uintptr_t{1} + z.vertex.size() + p));
                e.State(vk::gxfront::kBlendingMode, d % 7 == 0 ? 2 : 0);
                e.State(vk::gxfront::kDepthTest, 1);
                e.State(vk::gxfront::kDepthWrite, 1);
                // What the M2 renderer writes before a batch: its matrices and its bones; the pixel
                // side its colours.
                live.renderer.Constants(false, 0, 8, bones);
                live.renderer.Constants(false, 31, 3 * 24, bones + 32);
                live.renderer.Constants(true, 0, 4, bones + 16);
            }
            for (auto* tex : {&e.states[vk::gxfront::kTexture0], &e.states[vk::gxfront::kTexture0 + 1]})
                tex->value.data.p = &e.textures[(d + (tex == &e.states[vk::gxfront::kTexture0] ? 0u : 7u)) % e.textures.size()];
            e.device.m_primVertexFormatBuf[0] = e.device.m_primVertexFormatBuf[4] = e.device.m_primVertexFormatBuf[6] =
                reinterpret_cast<engine::CGxBuf*>(&e.buffers[0]);
            e.device.m_primIndexBuf = reinterpret_cast<engine::CGxBuf*>(&e.buffers[1]);
            (void)vpool;
            (void)ipool;
            auto t1 = Clock::now();
            cost.other_ms += Ms(t0, t1);

            vk::gxfront::EngineView view;
            view.device           = &e.device;
            view.states           = e.states;
            view.vertex_constants = live.renderer.Registers(false);
            draw::Packet       packet;
            const wxl_gx_batch batch{3, start, count, 0, static_cast<uint16_t>(kPoolVertices - 1)};
            const vk::gxfront::Verdict verdict = vk::gxfront::BuildDraw(view, batch, ui ? 0 : 1, e, packet);
            auto                       t2      = Clock::now();
            cost.read_ms += Ms(t1, t2);
            if (verdict == vk::gxfront::Verdict::Draw && live.renderer.Draw(packet)) ++cost.drawn;
            cost.record_ms += Ms(t2, Clock::now());
        }
        const auto end = Clock::now();
        live.display.EndFrame();
        cost.other_ms += Ms(end, Clock::now());
        cost.skipped = static_cast<uint32_t>(live.renderer.Counters().skipped - skipped0);
        return cost;
    }

    bool MakeZone(rig::Live& live, Engine& e, Zone& z, const std::vector<Program>& corpus, uint32_t models, uint32_t textures)
    {
        // The engine's objects: pool 0 the stream pool, then each model's vertex and index pools.
        e.pools.assign(1 + models * 2, engine::CGxPool{});
        e.buffers.assign(2, engine::CGxBuf{});
        e.textures.assign(textures, engine::CGxTex{});
        e.made.assign(textures, nullptr);
        z.stream = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 256 * 1024);
        e.pools[0].m_apiSpecific = z.stream;
        e.known_pools.insert(&e.pools[0]);
        vk::frame::Wait wait{};
        if (!live.display.BeginFrame(wait)) return false;
        for (uint32_t m = 0; m < models; ++m)
        {
            MakeModel(live, e, z);
            e.pools[1 + m * 2].m_apiSpecific = z.vertex_pools.back();
            e.pools[2 + m * 2].m_apiSpecific = z.index_pools.back();
            e.known_pools.insert(&e.pools[1 + m * 2]);
            e.known_pools.insert(&e.pools[2 + m * 2]);
        }
        for (uint32_t t = 0; t < textures; ++t)
        {
            e.made[t] = MakeTexture(live, t);
            e.textures[t].m_flags.m_filter = 1;
            e.known_textures.insert(&e.textures[t]);
        }
        live.display.EndFrame();
        // The programs, as the client creates them (each record once).
        std::vector<int> vertex, pixel;
        for (const Program& p : corpus)
        {
            const int id = live.renderer.Program(p.pixel, p.name.c_str(), p.code.data(), p.code.size());
            if (id < 0) continue;
            (p.pixel ? pixel : vertex).push_back(id);
        }
        // Pairs: as many as the smaller side has, each pixel program behind a vertex one.
        const size_t n = std::min(vertex.size(), pixel.size());
        z.vertex.assign(vertex.begin(), vertex.begin() + static_cast<ptrdiff_t>(n));
        z.pixel.assign(pixel.begin(), pixel.begin() + static_cast<ptrdiff_t>(n));
        e.programs.clear();
        for (int id : z.vertex) e.programs.push_back(id);
        for (int id : z.pixel) e.programs.push_back(id);
        for (size_t i = 0; i < e.programs.size(); ++i) e.known_shaders.insert(reinterpret_cast<const void*>(uintptr_t{1} + i));
        return true;
    }

    void FreeZone(rig::Live& live, Engine& e, Zone& z)
    {
        live.display.WaitSubmitted();
        for (draw::Pool* p : z.vertex_pools) live.renderer.DestroyPool(p);
        for (draw::Pool* p : z.index_pools) live.renderer.DestroyPool(p);
        live.renderer.DestroyPool(z.stream);
        for (draw::Texture* t : e.made) live.renderer.DestroyTexture(t);
        z = Zone{};
        e.made.clear();
    }

    /// Steady frames: every pipeline ready, nothing loading.
    void Steady(const Options& o, const std::vector<Program>& corpus)
    {
        rig::Live live;
        char      why[256] = {};
        draw::Renderer::Paths paths;
        paths.async_pipelines = false;
        if (!live.Make(o.width, o.height, why, sizeof why, paths))
        {
            std::printf("no device: %s\n", why);
            std::exit(77);
        }
        Engine e;
        ResetDevice(e, o.width, o.height);
        Zone z;
        if (!MakeZone(live, e, z, corpus, 64, 64)) return;
        // Warm up: every pipeline made, the rings at their size.
        for (uint32_t f = 0; f < 20; ++f) DrawFrame(live, e, z, o.draws, f);
        live.display.WaitSubmitted();
        FrameCost total;
        std::vector<double> per_frame;
        const uint64_t      draws0 = live.renderer.Counters().draws;
        for (uint32_t f = 0; f < o.frames; ++f)
        {
            const FrameCost c = DrawFrame(live, e, z, o.draws, 20 + f);
            total.read_ms += c.read_ms;
            total.record_ms += c.record_ms;
            total.other_ms += c.other_ms;
            total.drawn += c.drawn;
            per_frame.push_back(c.read_ms + c.record_ms);
        }
        const uint64_t drawn = live.renderer.Counters().draws - draws0;
        std::sort(per_frame.begin(), per_frame.end());
        const double frames = o.frames;
        std::printf("steady: %u draws a frame (%u programs' pairs), %u frames\n", o.draws, static_cast<unsigned>(z.vertex.size()), o.frames);
        std::printf("  main thread per frame: engine reading %.3f ms, renderer %.3f ms (median of both %.3f ms), other %.3f ms\n",
                    total.read_ms / frames, total.record_ms / frames, per_frame[per_frame.size() / 2], total.other_ms / frames);
        std::printf("  per draw: reading %.2f us, renderer %.2f us; %llu drawn of %llu\n", 1000.0 * total.read_ms / std::max(1u, total.drawn),
                    1000.0 * total.record_ms / std::max(1u, total.drawn), static_cast<unsigned long long>(drawn),
                    static_cast<unsigned long long>(o.draws) * o.frames);
        const auto& r = live.renderer.Counters();
        std::printf("  registers copied: %.1f KiB a frame; pipelines %llu\n", static_cast<double>(r.register_bytes) / 1024.0 / (frames + 20),
                    static_cast<unsigned long long>(r.pipelines));
        FreeZone(live, e, z);
    }

    /// The frame a zone loads in: its textures and pools made, filled and drawn at once.
    void FirstVisit(const Options& o, const std::vector<Program>& corpus)
    {
        rig::Live live;
        char      why[256] = {};
        draw::Renderer::Paths paths;
        paths.async_pipelines = false;
        paths.direct_pools    = o.rebar;
        if (!live.Make(o.width, o.height, why, sizeof why, paths, o.rebar)) return;
        live.display.SetPassTiming(true);
        Engine e;
        ResetDevice(e, o.width, o.height);
        // A first zone, its pipelines made, then a second one loaded in one frame.
        Zone z;
        if (!MakeZone(live, e, z, corpus, 8, 8)) return;
        for (uint32_t f = 0; f < 10; ++f) DrawFrame(live, e, z, o.draws, f);
        live.display.WaitSubmitted();
        FreeZone(live, e, z);
        const uint64_t bytes0 = live.renderer.Counters().upload_bytes;
        const auto     begin  = Clock::now();
        const uint32_t models = o.quick ? 16 : 96, textures = o.quick ? 16 : 96;
        if (!MakeZone(live, e, z, corpus, models, textures)) return;
        const double made_ms = Ms(begin, Clock::now());
        const FrameCost first = DrawFrame(live, e, z, o.draws, 0);
        live.display.WaitSubmitted();
        const uint64_t bytes = live.renderer.Counters().upload_bytes - bytes0;
        // Two more frames: the slot of the loading frame comes back and its timestamps are read.
        for (uint32_t f = 1; f <= 2; ++f) DrawFrame(live, e, z, o.draws, f);
        double upload_max = 0.0, scene_mean = 0.0;
        for (const auto& pass : live.display.PassTimes())
        {
            if (!std::strcmp(pass.name, "upload pass")) upload_max = pass.per_frame.Max();
            if (!std::strcmp(pass.name, "scene")) scene_mean = pass.per_frame.Mean();
        }
        std::printf("first visit: %u models (%u KiB each) and %u textures (256x256 DXT1, 9 levels) made in one frame\n", models,
                    (kPoolVertices * kStride + kPoolIndices * 2) / 1024, textures);
        const auto& rc = live.renderer.Counters();
        std::printf("  main thread: %.2f ms making and filling them, %.2f ms in the first frame's draws; %.1f MiB uploaded (pools: %.1f MiB "
                    "written in place, %.1f MiB staged)%s\n",
                    made_ms, first.read_ms + first.record_ms, static_cast<double>(bytes) / 1048576.0,
                    static_cast<double>(rc.direct_bytes) / 1048576.0, static_cast<double>(rc.staged_bytes) / 1048576.0,
                    o.rebar ? ", ReBAR on" : "");
        std::printf("  GPU: the loading frame's upload pass %.2f ms (a frame's scene %.2f ms)\n", upload_max, scene_mean);
        FreeZone(live, e, z);
    }

    /// Two launches with the same cache folder: the second should draw everything at once.
    void Launches(const Options& o, const std::vector<Program>& corpus)
    {
        if (o.cache.empty() || corpus.empty()) return;
        std::error_code ec;
        fs::remove_all(o.cache, ec);
        fs::create_directories(o.cache, ec);
        for (int launch = 1; launch <= 2; ++launch)
        {
            rig::Live live;
            char      why[256] = {};
            draw::Renderer::Paths paths;
            paths.async_pipelines = true;
            paths.pipeline_cache  = o.cache + "/pipelines.bin";
            paths.shader_pack     = o.cache + "/shaders.pack";
            paths.pipeline_keys   = o.cache + "/pipelines.keys";
            if (!live.Make(o.width, o.height, why, sizeof why, paths)) return;
            Engine e;
            ResetDevice(e, o.width, o.height);
            Zone z;
            const auto begin = Clock::now();
            if (!MakeZone(live, e, z, corpus, 8, 8)) return;
            // The client's login and loading screens before the world: the workers' time.
            const auto drain = Clock::now();
            if (launch > 1) live.renderer.PipelineCache().Drain();
            const double drained = Ms(drain, Clock::now());
            uint32_t skipped = 0, frames_until_whole = 0;
            double   worst = 0.0;
            for (uint32_t f = 0; f < 120; ++f)
            {
                const auto      t = Clock::now();
                const FrameCost c = DrawFrame(live, e, z, std::min(o.draws, 400u), f);
                worst             = std::max(worst, Ms(t, Clock::now()));
                skipped += c.skipped;
                if (c.skipped) frames_until_whole = f + 1;
            }
            live.display.WaitSubmitted();
            const auto p = live.renderer.PipelineCache().Counters();
            std::printf("launch %d: %u draws left out while their pipeline compiled, over %u frame(s); %llu linked, %llu compiled on the main "
                        "thread (%.0f ms) and %llu parts (%.0f ms), %llu prewarmed of %llu known (%.0f ms of workers before the first frame); "
                        "longest frame %.1f ms; %.0f ms to the end\n",
                        launch, skipped, frames_until_whole, static_cast<unsigned long long>(p.linked),
                        static_cast<unsigned long long>(p.compiled_main), p.compiled_main_ms, static_cast<unsigned long long>(p.parts_main),
                        p.parts_main_ms, static_cast<unsigned long long>(p.prewarmed), static_cast<unsigned long long>(p.prewarm_known), drained, worst,
                        Ms(begin, Clock::now()));
            FreeZone(live, e, z);
        }
    }
}

int main(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--corpus" && i + 1 < argc) o.corpus = argv[++i];
        else if (a == "--cache" && i + 1 < argc) o.cache = argv[++i];
        else if (a == "--draws" && i + 1 < argc) o.draws = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (a == "--frames" && i + 1 < argc) o.frames = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (a == "--quick") o.quick = true;
        else if (a == "--rebar") o.rebar = true;
    }
    if (o.quick)
    {
        o.draws  = std::min(o.draws, 120u);
        o.frames = std::min(o.frames, 20u);
    }
    vk::SetLogSink(Quiet, nullptr);
    const std::vector<Program> corpus = Corpus(o.corpus);
    std::printf("wxl-vk-bench: %zu client records%s\n", corpus.size(), corpus.empty() ? " (no corpus: the renderer's own programs)" : "");
    Steady(o, corpus);
    FirstVisit(o, corpus);
    Launches(o, corpus);
    return 0;
}
