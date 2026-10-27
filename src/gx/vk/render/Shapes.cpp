// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's shapes on the Vulkan renderer (see Shapes.hpp).

#include "gx/vk/render/Shapes.hpp"

#include <algorithm>
#include <cstring>

namespace wxl::vk::render
{
    namespace spirv
    {
        // The programs, compiled when the runtime is built (cmake/wxl_vk.cmake).
#include "draw_frag.h"
#include "draw_particle_frag.h"
#include "draw_surface_frag.h"
#include "draw_vert.h"
    }

    namespace
    {
        std::vector<uint32_t> Words(const uint32_t* code, size_t bytes) { return std::vector<uint32_t>(code, code + bytes / 4); }

        /// The programs' push constants (glsl/draw.vert, draw_surface.frag).
        struct Push
        {
            float    constants[24];
            uint32_t address[2];
            uint32_t surface[2];
        };
        static_assert(sizeof(Push) <= kPushBytes, "the push constants hold the view and two addresses");

        /// A surface's numbers as draw_surface.frag reads them.
        struct SurfaceRows
        {
            float    rows[6][4];
            uint32_t picture[4];
        };
        static_assert(sizeof(SurfaceRows) == 112, "six rows and the picture's slots");

        constexpr uint32_t kBlendOf[3] = {kBlendAlphaKeepAlpha, kBlendAddKeepAlpha, kBlendMultiplyKeepAlpha};

        uint32_t DepthOf(const gx::shapes::Run& run) { return run.on_top ? 1u : run.behind ? 2u : 0u; }
    }

    std::vector<OwnProgram> ShapePrograms()
    {
        return {{"draw.vert", spirv::draw_vert, sizeof spirv::draw_vert / 4},
                {"draw.frag", spirv::draw_frag, sizeof spirv::draw_frag / 4},
                {"draw_surface.frag", spirv::draw_surface_frag, sizeof spirv::draw_surface_frag / 4},
                {"draw_particle.frag", spirv::draw_particle_frag, sizeof spirv::draw_particle_frag / 4}};
    }

    void Shapes::Detach()
    {
        if (render_)
            for (auto& byBlend : pipelines_)
                for (auto& byDepth : byBlend)
                    for (Pipeline*& p : byDepth)
                        if (p) render_->DestroyPipeline(p);
        Forget();
    }

    void Shapes::Forget()
    {
        for (auto& byBlend : pipelines_)
            for (auto& byDepth : byBlend)
                for (Pipeline*& p : byDepth) p = nullptr;
        render_ = nullptr;
    }

    Pipeline* Shapes::PipelineFor(uint8_t program, uint8_t blend, uint32_t depth, std::string* why)
    {
        const uint32_t prog = program == gx::shapes::kSurface ? 1u : program == gx::shapes::kParticle ? 2u : 0u;
        const uint32_t b    = prog ? std::min<uint32_t>(blend, 2u) : 0u;
        Pipeline*&     slot = pipelines_[prog][b][std::min(depth, 2u)];
        if (slot) return slot;
        static const char* const kNames[3][3] = {{"wxl.draw's shapes", "", ""},
                                                 {"wxl.draw's surfaces", "wxl.draw's added surfaces", "wxl.draw's modulated surfaces"},
                                                 {"wxl.draw's particles", "wxl.draw's added particles", "wxl.draw's modulated particles"}};
        PipelineSpec spec;
        spec.kind             = kGraphics;
        static const char* const kDepths[3] = {"", " on top", " behind the scene"};
        spec.name             = std::string(kNames[prog][b]) + kDepths[std::min(depth, 2u)];
        spec.vertex           = Words(spirv::draw_vert, sizeof spirv::draw_vert);
        spec.fragment         = prog == 2 ? Words(spirv::draw_particle_frag, sizeof spirv::draw_particle_frag)
                                : prog == 1 ? Words(spirv::draw_surface_frag, sizeof spirv::draw_surface_frag)
                                            : Words(spirv::draw_frag, sizeof spirv::draw_frag);
        spec.color_formats[0] = kBgra8;
        spec.depth_format     = kDepth32f;
        spec.blend[0]         = kBlendOf[b];
        spec.depth_test       = depth == 1 ? kDepthOff : depth == 2 ? kDepthGreater : kDepthLessEqual;
        spec.depth_write      = false;
        spec.cull             = kCullNone;
        spec.topology         = kTriangles;
        std::string refusal;
        slot = render_->CreatePipeline(spec, refusal);
        if (!slot && why) *why = "the device refused " + spec.name + "' pipeline" + (refusal.empty() ? std::string() : ": " + refusal);
        return slot;
    }

    uint32_t Shapes::Draw(const gx::shapes::Vertex* vertices, uint32_t tested, uint32_t on_top, const float constants[24],
                          const float viewport[6], std::string* why)
    {
        gx::shapes::Run runs[2];
        runs[0].first  = 0;
        runs[0].count  = tested;
        runs[1].first  = tested;
        runs[1].count  = on_top;
        runs[1].on_top = true;
        return Draw(vertices, runs, 2, nullptr, constants, viewport, why);
    }

    uint32_t Shapes::Draw(const gx::shapes::Vertex* vertices, const gx::shapes::Run* runs, uint32_t run_count,
                          const gx::shapes::Surface* surfaces, const float constants[24], const float viewport[6], std::string* why)
    {
        uint32_t total = 0;
        for (uint32_t i = 0; i < run_count; ++i) total += runs[i].count;
        if (!total) return 0;
        if (!render_ || !render_->Ready())
        {
            if (why) *why = "the Vulkan device's wxl.render is not up";
            return 0;
        }
        Render& r = *render_;
        // Every pipeline the runs need, before the pass.
        for (uint32_t i = 0; i < run_count; ++i)
            if (runs[i].count && !PipelineFor(runs[i].program, runs[i].blend, DepthOf(runs[i]), why)) return 0;

        PassSpec onto;
        onto.color_count    = 1;
        onto.scene_color[0] = true;
        onto.scene_depth    = true;
        std::string refusal;
        if (!r.Begin(onto, &refusal))
        {
            if (why) *why = "the shapes' pass could not begin: " + refusal;
            return 0;
        }
        // The world's viewport and depth range, moved by half a pixel to Direct3D 9's centres as the
        // renderer draws the world.
        VkViewport v{};
        v.x        = viewport[0] + 0.5f;
        v.y        = viewport[1] + 0.5f;
        v.width    = std::max(viewport[2], 1.0f);
        v.height   = std::max(viewport[3], 1.0f);
        v.minDepth = std::clamp(viewport[4], 0.0f, 1.0f);
        v.maxDepth = std::clamp(viewport[5], 0.0f, 1.0f);
        vkCmdSetViewport(r.Screen().Cmd(), 0, 1, &v);

        Push push{};
        std::memcpy(push.constants, constants, sizeof push.constants);
        const uint64_t frame  = r.Screen().FrameNumber();
        uint32_t       drawn  = 0;
        bool           failed = false;
        for (uint32_t i = 0; i < run_count && !failed; ++i)
        {
            const gx::shapes::Run& run      = runs[i];
            Pipeline*              pipeline = PipelineFor(run.program, run.blend, DepthOf(run), why);
            if (!run.count || !pipeline) continue;
            push.constants[22] = run.alpha;
            push.surface[0] = push.surface[1] = 0;
            if ((run.program == gx::shapes::kSurface || run.program == gx::shapes::kParticle) && surfaces)
            {
                const gx::shapes::Surface& surface = surfaces[run.surface];
                SurfaceRows                rows{};
                std::memcpy(rows.rows, surface.constants.k, sizeof rows.rows);
                if (auto* texture = static_cast<draw::Texture*>(surface.texture))
                {
                    texture->used   = frame; // read by this frame: kept until it completes
                    rows.picture[0] = texture->slot;
                    rows.picture[1] = r.Sampler(kSamplerLinear | kSamplerMips | (surface.repeats ? kSamplerWrap : 0u));
                }
                else
                    rows.rows[2][3] = 0.0f; // no picture to read
                VkDeviceAddress address = 0;
                if (!r.Engine().FrameData(&rows, sizeof rows, address))
                {
                    if (why) *why = "the frame's memory refused a surface's numbers";
                    failed = true;
                    break;
                }
                push.surface[0] = static_cast<uint32_t>(address);
                push.surface[1] = static_cast<uint32_t>(address >> 32);
            }
            for (uint32_t at = 0; at < run.count && !failed; at += kVerticesPerDraw)
            {
                const uint32_t  n = std::min(kVerticesPerDraw, run.count - at);
                VkDeviceAddress address = 0;
                if (!r.Engine().FrameData(vertices + run.first + at, n * static_cast<uint32_t>(sizeof(gx::shapes::Vertex)), address))
                {
                    if (why) *why = "the frame's memory refused the shapes' vertices";
                    failed = true;
                    break;
                }
                push.address[0] = static_cast<uint32_t>(address);
                push.address[1] = static_cast<uint32_t>(address >> 32);
                if (!r.Draw(pipeline, n, 1, &push, sizeof push))
                {
                    if (why) *why = "the renderer refused to draw the shapes";
                    failed = true;
                    break;
                }
                drawn += n;
            }
        }
        r.End();
        return drawn;
    }
}
