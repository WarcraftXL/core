// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.highlight's passes on the Vulkan renderer (see Highlight.hpp).

#include "gx/vk/render/Highlight.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace wxl::vk::render
{
    namespace spirv
    {
        // The passes' programs, compiled when the runtime is built (cmake/wxl_vk.cmake).
#include "highlight_blur_frag.h"
#include "highlight_composite_frag.h"
#include "highlight_mask_frag.h"
#include "fullscreen_vert.h"
    }

    std::vector<OwnProgram> OwnPrograms()
    {
        return {{"fullscreen.vert", spirv::fullscreen_vert, sizeof spirv::fullscreen_vert / 4},
                {"highlight_mask.frag", spirv::highlight_mask_frag, sizeof spirv::highlight_mask_frag / 4},
                {"highlight_blur.frag", spirv::highlight_blur_frag, sizeof spirv::highlight_blur_frag / 4},
                {"highlight_composite.frag", spirv::highlight_composite_frag, sizeof spirv::highlight_composite_frag / 4}};
    }

    namespace
    {
        // The Direct3D 9 renderer's numbers (gx/d3d9/Highlight.cpp).
        constexpr float kWidthCode       = 32.0f;  ///< the mask's width: its alpha times this many pixels
        constexpr float kMaxOutline      = 32.0f;  ///< pixels, once scaled to the render's height
        constexpr float kMaxGlow         = 128.0f;
        constexpr float kReferenceHeight = 1080.0f; ///< widths are given for this many lines
        constexpr float kGlowGain        = 2.0f;   ///< the blurred edge is half as bright as the object's

        float Channel(uint32_t color, int shift) { return static_cast<float>((color >> shift) & 0xFFu) / 255.0f; }

        std::vector<uint32_t> Words(const uint32_t* code, size_t bytes) { return std::vector<uint32_t>(code, code + bytes / 4); }

        float Bits(uint32_t value)
        {
            float f = 0.0f;
            std::memcpy(&f, &value, sizeof f);
            return f;
        }

        struct BlurPush
        {
            float    step[2];
            uint32_t image;
            uint32_t sampler;
        };

        struct CompositePush
        {
            float    screen[4];
            float    gain[4];
            uint32_t color;
            uint32_t extra;
            uint32_t glow;
            uint32_t nearest;
            uint32_t linear;
        };
    }

    void Highlight::Detach()
    {
        kept_.clear();
        roots_.clear();
        if (!render_) return;
        for (Image** image : {&color_, &extra_, &glow_a_, &glow_b_})
        {
            if (*image) render_->DestroyImage(*image);
            *image = nullptr;
        }
        for (Pipeline** pipeline : {&mask_, &blur_, &composite_})
        {
            if (*pipeline) render_->DestroyPipeline(*pipeline);
            *pipeline = nullptr;
        }
        render_ = nullptr;
    }

    void Highlight::Forget()
    {
        kept_.clear();
        roots_.clear();
        color_ = extra_ = glow_a_ = glow_b_ = nullptr;
        mask_ = blur_ = composite_ = nullptr;
        render_ = nullptr;
    }

    bool Highlight::Refuse(const char* reason)
    {
        if (counts_.refusal.empty() && reason) counts_.refusal = reason;
        return false;
    }

    void Highlight::Begin()
    {
        kept_.clear();
        roots_.clear();
    }

    void Highlight::World(const Root* roots, uint32_t count)
    {
        kept_.clear();
        roots_.assign(roots, roots + (roots ? count : 0));
        counts_       = Counts{};
        counts_.roots = static_cast<uint32_t>(roots_.size());
    }

    bool Highlight::Batch(const void* root, uint32_t blend, const draw::Batch* last)
    {
        if (roots_.empty()) return false;
        ++counts_.models;
        const auto at = std::find_if(roots_.begin(), roots_.end(), [&](const Root& r) { return r.model == root; });
        if (at == roots_.end()) return false;
        ++counts_.seen;
        // Translucent parts (blended: a weapon's enchant, a ghost) are not the object's silhouette.
        if (blend > 1) return false;
        if (!last) return Refuse("the renderer kept no record of a highlighted model's draw (it was not drawn)");
        // As on Direct3D 9: a model drawn by the fixed function has no program of its own.
        if (last->vertex < static_cast<uint16_t>(pipelines::VertexProgram::Count))
            return Refuse("a highlighted model is drawn without a vertex program of the client's (fixed function), which is not highlighted");
        return Keep(*last, at->style);
    }

    bool Highlight::Make()
    {
        if (composite_) return true;
        std::string refusal;
        PipelineSpec mask;
        mask.kind             = kBatch;
        mask.name             = "wxl.highlight's masks";
        mask.fragment         = Words(spirv::highlight_mask_frag, sizeof spirv::highlight_mask_frag);
        mask.color_count      = 2;
        mask.color_formats[0] = kRgba8;
        mask.color_formats[1] = kRgba8;
        mask.depth_format     = kDepth32f; // the scene's depth, tested
        mask.depth_test       = kDepthLessEqual;
        mask.cull             = kCullNone;

        PipelineSpec blur;
        blur.kind             = kFullscreen;
        blur.name             = "wxl.highlight's blur";
        blur.fragment         = Words(spirv::highlight_blur_frag, sizeof spirv::highlight_blur_frag);
        blur.color_formats[0] = kRgba8;
        blur.samples          = 1;

        PipelineSpec composite;
        composite.kind             = kFullscreen;
        composite.name             = "wxl.highlight's composite";
        composite.fragment         = Words(spirv::highlight_composite_frag, sizeof spirv::highlight_composite_frag);
        composite.color_formats[0] = kBgra8;
        composite.blend[0]         = kBlendPremultiplied;

        mask_      = render_->CreatePipeline(mask, refusal);
        blur_      = mask_ ? render_->CreatePipeline(blur, refusal) : nullptr;
        composite_ = blur_ ? render_->CreatePipeline(composite, refusal) : nullptr;
        color_     = render_->CreateImage(ImageSpec{0, 0, 1, kRgba8, 0});
        extra_     = render_->CreateImage(ImageSpec{0, 0, 1, kRgba8, 0});
        glow_a_    = render_->CreateImage(ImageSpec{0, 0, 2, kRgba8, 1});
        glow_b_    = render_->CreateImage(ImageSpec{0, 0, 2, kRgba8, 1});
        if (mask_ && blur_ && composite_ && color_ && extra_ && glow_a_ && glow_b_) return true;
        Refuse(("the device refused the highlight's programs or images" + (refusal.empty() ? std::string() : ": " + refusal)).c_str());
        Render* render = render_;
        Detach();
        render_ = render;
        return false;
    }

    bool Highlight::Keep(const draw::Batch& batch, uint32_t style)
    {
        // A batch drawn into a texture (a portrait, the minimap) is no part of the scene.
        if (!batch.scene) return Refuse("a highlighted model's batch was drawn into a texture, not into the scene");
        if (kept_.size() >= kMaxBatches) return Refuse("more batches of highlighted models than are kept in a frame (512)");
        kept_.push_back(KeptBatch{batch, style});
        ++counts_.kept;
        return true;
    }

    uint32_t Highlight::Draw(const Style* styles, uint32_t count, const char** why)
    {
        const uint32_t drawn = DrawPasses(styles, count);
        if (why) *why = counts_.refusal.empty() ? nullptr : counts_.refusal.c_str();
        kept_.clear();
        return drawn;
    }

    uint32_t Highlight::DrawPasses(const Style* styles, uint32_t count)
    {
        if (kept_.empty()) return 0;
        if (!render_ || !render_->Ready()) return Refuse("the Vulkan device's wxl.render is not up"), 0;
        if (!Make()) return 0;
        Render& r = *render_;

        // Widths are given for a render 1080 lines high, and drawn the same at any height.
        const VkExtent2D scene = r.Screen().SceneExtent();
        const float      scale = static_cast<float>(scene.height) / kReferenceHeight;
        std::vector<Style> scaled(styles, styles + count);
        for (Style& style : scaled)
        {
            style.outline = std::min(style.outline * scale, kMaxOutline);
            style.glow    = std::min(style.glow * scale, kMaxGlow);
        }

        // The masks: the kept batches against the scene's depth, nothing written to it.
        PassSpec masks;
        masks.color_count = 2;
        masks.colors[0]   = color_;
        masks.colors[1]   = extra_;
        masks.clear       = 0x3;
        masks.scene_depth = true;
        std::string refusal;
        if (!r.Begin(masks, &refusal)) return Refuse(("the masks' pass could not begin: " + refusal).c_str()), 0;
        const uint32_t texture = r.Sampler(kSamplerLinear | kSamplerWrap | kSamplerMips);
        uint32_t       drawn   = 0;
        for (const KeptBatch& kept : kept_)
        {
            if (kept.style >= count) continue;
            const Style& style = scaled[kept.style];
            const float  red = Channel(style.color, 16), green = Channel(style.color, 8), blue = Channel(style.color, 0);
            // The alpha test the client drew with, on its first texture (-1: none, every texel kept).
            const float alpha = kept.batch.push.alpha_ref > 0.0f ? kept.batch.push.alpha_ref / 255.0f : -1.0f;
            const float rows[12] = {red, green, blue, 1.0f,
                                    red * style.strength, green * style.strength, blue * style.strength, style.outline / kWidthCode,
                                    alpha, Bits(texture), 0.0f, 0.0f};
            if (r.DrawBatch(kept.batch, mask_, rows, sizeof rows)) ++drawn;
            else if (kept.batch.frame != r.Screen().FrameNumber()) Refuse("a kept batch is of another frame");
            else
                Refuse("the renderer refused to draw a kept batch again (the masks' pipeline for the client's vertex program, or the "
                       "frame's memory; the gx log may say more)");
        }
        r.End();
        counts_.drawn = drawn;
        if (!drawn) return 0;

        // The blur reaches past the widest outline, which the composite searches only where the
        // blurred mask reaches.
        float widest = 0.0f, radius = 2.0f;
        bool  glow   = false;
        for (const Style& style : scaled)
        {
            widest = std::max(widest, style.outline);
            radius = std::max({radius, style.glow, style.outline + 2.0f});
            glow   = glow || (style.glow > 0.0f && style.strength > 0.0f);
        }
        const VkExtent2D full    = r.Extent(color_);
        const float      step    = radius / 4.0f; // four taps each side reach the radius
        const uint32_t   linear  = r.Sampler(kSamplerLinear);
        const uint32_t   nearest = r.Sampler(0);

        PassSpec across;
        across.color_count = 1;
        across.colors[0]   = glow_a_;
        if (!r.Begin(across, &refusal)) return Refuse(("the glow's pass could not begin: " + refusal).c_str()), 0;
        const BlurPush a{{step / static_cast<float>(full.width), 0.0f}, r.Slot(extra_), linear};
        r.Fullscreen(blur_, &a, sizeof a);
        r.End();

        PassSpec down;
        down.color_count = 1;
        down.colors[0]   = glow_b_;
        if (!r.Begin(down, &refusal)) return Refuse(("the glow's pass could not begin: " + refusal).c_str()), 0;
        const BlurPush b{{0.0f, step / static_cast<float>(full.height)}, r.Slot(glow_a_), linear};
        r.Fullscreen(blur_, &b, sizeof b);
        r.End();

        // The outline and the glow onto the scene.
        PassSpec onto;
        onto.color_count    = 1;
        onto.scene_color[0] = true;
        if (!r.Begin(onto, &refusal)) return Refuse(("the composite's pass could not begin: " + refusal).c_str()), 0;
        const CompositePush c{{1.0f / static_cast<float>(full.width), 1.0f / static_cast<float>(full.height), widest, glow ? 1.0f : 0.0f},
                              {kGlowGain, 0.0f, 0.0f, 0.0f},
                              r.Slot(color_),
                              r.Slot(extra_),
                              r.Slot(glow_b_),
                              nearest,
                              linear};
        const bool composited = r.Fullscreen(composite_, &c, sizeof c);
        r.End();
        if (!composited) return Refuse("the renderer refused the composite onto the scene"), 0;
        counts_.composited = true;
        return drawn;
    }
}
