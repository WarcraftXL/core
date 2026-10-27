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
#include "draw_vert.h"
    }

    namespace
    {
        std::vector<uint32_t> Words(const uint32_t* code, size_t bytes) { return std::vector<uint32_t>(code, code + bytes / 4); }

        /// The vertex program's push constants (glsl/draw.vert).
        struct Push
        {
            float    constants[24];
            uint32_t address[2];
        };
        static_assert(sizeof(Push) <= kPushBytes, "the push constants hold the view and the stream's address");
    }

    std::vector<OwnProgram> ShapePrograms()
    {
        return {{"draw.vert", spirv::draw_vert, sizeof spirv::draw_vert / 4}, {"draw.frag", spirv::draw_frag, sizeof spirv::draw_frag / 4}};
    }

    void Shapes::Detach()
    {
        if (render_)
        {
            if (tested_) render_->DestroyPipeline(tested_);
            if (top_) render_->DestroyPipeline(top_);
        }
        tested_ = top_ = nullptr;
        render_ = nullptr;
    }

    void Shapes::Forget()
    {
        tested_ = top_ = nullptr;
        render_ = nullptr;
    }

    bool Shapes::Make(std::string* why)
    {
        if (tested_ && top_) return true;
        PipelineSpec spec;
        spec.kind             = kGraphics;
        spec.name             = "wxl.draw's shapes";
        spec.vertex           = Words(spirv::draw_vert, sizeof spirv::draw_vert);
        spec.fragment         = Words(spirv::draw_frag, sizeof spirv::draw_frag);
        spec.color_formats[0] = kBgra8;
        spec.depth_format     = kDepth32f;
        spec.blend[0]         = kBlendAlphaKeepAlpha;
        spec.depth_test       = kDepthLessEqual;
        spec.depth_write      = false;
        spec.cull             = kCullNone;
        spec.topology         = kTriangles;
        std::string refusal;
        if (!tested_) tested_ = render_->CreatePipeline(spec, refusal);
        spec.name       = "wxl.draw's shapes on top";
        spec.depth_test = kDepthOff;
        if (tested_ && !top_) top_ = render_->CreatePipeline(spec, refusal);
        if (tested_ && top_) return true;
        if (why) *why = "the device refused wxl.draw's pipelines" + (refusal.empty() ? std::string() : ": " + refusal);
        return false;
    }

    uint32_t Shapes::Draw(const gx::shapes::Vertex* vertices, uint32_t tested, uint32_t on_top, const float constants[24],
                          const float viewport[6], std::string* why)
    {
        if (!tested && !on_top) return 0;
        if (!render_ || !render_->Ready())
        {
            if (why) *why = "the Vulkan device's wxl.render is not up";
            return 0;
        }
        if (!Make(why)) return 0;
        Render& r = *render_;

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
        uint32_t drawn = 0;
        bool     failed = false;
        const auto run = [&](Pipeline* pipeline, const gx::shapes::Vertex* from, uint32_t count) {
            for (uint32_t at = 0; at < count && !failed; at += kVerticesPerDraw)
            {
                const uint32_t  n = std::min(kVerticesPerDraw, count - at);
                VkDeviceAddress address = 0;
                if (!r.Engine().FrameData(from + at, n * static_cast<uint32_t>(sizeof(gx::shapes::Vertex)), address))
                {
                    if (why) *why = "the frame's memory refused the shapes' vertices";
                    failed = true;
                    return;
                }
                push.address[0] = static_cast<uint32_t>(address);
                push.address[1] = static_cast<uint32_t>(address >> 32);
                if (!r.Draw(pipeline, n, 1, &push, sizeof push))
                {
                    if (why) *why = "the renderer refused to draw the shapes";
                    failed = true;
                    return;
                }
                drawn += n;
            }
        };
        run(tested_, vertices, tested);
        run(top_, vertices + tested, on_top);
        r.End();
        return drawn;
    }
}
