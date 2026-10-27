// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw: the shapes mods draw in the world, kept here by mod, built into geometry in world
// coordinates (gx/shapes/Geometry.hpp) and drawn once a frame, after the world's opaque geometry and
// its liquids, by the Vulkan renderer (gx/vk/render/Shapes.cpp) or the Direct3D 9 device
// (gx/d3d9/Shapes.cpp), whichever the client draws with: the same vertices, the same programs.
//
// An immediate shape is built when it is given (laid on the ground then) and dropped once a world
// has drawn it; a kept one is built when it is added or changed, and again when what it lies on or
// follows moved: wxl.terrain's ground changed (its generation), its object moved or turned. Each
// frame writes every shape's vertices from the eye (their colours with the pulse and the fades of
// the moment) and draws them in runs, tested against the depth then on top: plain insides, then
// surfaces (an inside with a texture, a feather, a fade or a blend: one run each, with its
// numbers), volumes far to near (a volume's back faces left out), the lines' shadows, the lines.
// A surface's texture is opened through the client (texture_open) when a shape first names it,
// shared by path, readied on the device each frame it is drawn (gx_texture_ready) and closed a few
// seconds after the last shape that used it went. Refusals are logged once per mod and call under
// `draw`, shapes left out past the frame's budget once a second.

#include "services/DrawService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "gx/Vulkan.hpp"
#include "gx/d3d9/Shapes.hpp"
#include "gx/shapes/Geometry.hpp"
#include "gx/shapes/Particles.hpp"
#include "gx/vk/gxfront/Front.hpp"
#include "gx/vk/render/Shapes.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"
#include "services/ScriptService.hpp"
#include "services/TerrainService.hpp"

#include "wxl/engine/gx.hpp"
#include "wxl/host/mod_host.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace wxl::core;
    namespace sh = wxl::gx::shapes;

    constexpr const char* kTag = "draw";
    constexpr double      kPi  = 3.14159265358979323846;

    using Clock = std::chrono::steady_clock;

    double Now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

    struct Style
    {
        uint32_t color = 0xFFFFFFu;
        float    alpha = 1.0f;
        float    fill = 0.0f;
        float    width = 2.0f;
        uint32_t depth = WXL_DRAW_DEPTH_TESTED;
        float    pulse = 0.0f;
        float    fade_in = 0.0f;
        float    fade_out = 0.0f;
        // abi 0.1.38
        uint32_t        fill_color = 0xFFFFFFu; ///< the inside's: fill_color under WXL_DRAW_STYLE_FILL_COLOR, color otherwise
        float           softness = 0.0f;
        bool            along = false;
        float           alpha_start = 1.0f, alpha_end = 1.0f;
        uint32_t        shadow_color = 0;
        float           shadow_alpha = 0.0f, shadow_x = 0.0f, shadow_y = 0.0f, shadow_softness = 0.0f;
        std::string     texture; ///< the key it is held under (Textures), empty for none
        sh::SurfaceLook look;
        float           occluded = 0.0f; ///< abi 0.1.39: what the scene hides drawn at this part of the opacity

        bool      Surface() const { return look.textured || look.feather > 0.0f || look.radial > 0.0f || look.blend != sh::kBlendAlpha; }
        sh::Parts Parts() const { return sh::Parts{alpha > 0.0f, fill > 0.0f, look.feather}; }
    };

    // --- Textures ----------------------------------------------------------------------------------

    /// A texture the client loads, held while shapes use it.
    struct Texture
    {
        std::string path;
        void*       handle = nullptr; ///< the client's (texture_open)
        uint32_t    refs = 0;         ///< kept and immediate shapes that name it
        double      unused_since = 0.0;
        uint64_t    frame = 0;        ///< the last frame it was readied in
        void*       device = nullptr; ///< what the device draws it with this frame; null while it loads
    };

    std::unordered_map<std::string, Texture> g_textures; ///< by path, lower case with backslashes
    uint64_t                                 g_frame = 0;
    constexpr double                         kTextureLinger = 3.0; ///< seconds a texture no shape uses is kept

    std::string TextureKey(const char* path)
    {
        std::string key(path);
        for (char& c : key) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return key;
    }

    /// The texture of @p key, opened when no shape held it; false when the client makes none.
    bool HoldTexture(const std::string& key, const std::string& path)
    {
        Texture& t = g_textures[key];
        if (!t.handle)
        {
            t.path   = path;
            t.handle = binding::texture_open(path.c_str());
            if (!t.handle)
            {
                g_textures.erase(key);
                return false;
            }
        }
        ++t.refs;
        return true;
    }

    void LetGoTexture(const std::string& key)
    {
        if (key.empty()) return;
        const auto at = g_textures.find(key);
        if (at == g_textures.end() || !at->second.refs) return;
        if (--at->second.refs == 0) at->second.unused_since = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    struct Kept
    {
        uint32_t   mod = 0;
        wxl_handle handle = WXL_HANDLE_NONE;
        sh::Desc   desc;       ///< as given; offsets from its object's position while attached
        Style      style;
        double     born = 0.0;
        double     ends = 0.0; ///< 0: until removed
        wxl_handle target = WXL_HANDLE_NONE;
        uint32_t   attach = 0;
        // What it was built as.
        sh::Mesh   mesh;
        bool       built = false;
        uint64_t   generation = 0; ///< wxl.terrain's, for a shape on the ground
        sh::V3     placed;         ///< the object's position it was built at
        float      turned = 0.0f;  ///< and its facing
        ~Kept() { LetGoTexture(style.texture); }
    };

    struct Immediate
    {
        uint32_t mod = 0;
        Style    style;
        sh::Mesh mesh;
        // What the mesh was built from: a shape given again as it was (the same mod, the same slot of
        // the frame, the ground unchanged) is drawn without being built again.
        sh::Desc  desc;
        sh::Parts parts;
        uint64_t  generation = 0;
        bool      valid = false;

        /// Drawn or dropped: its texture is no longer held for it.
        void Done()
        {
            LetGoTexture(style.texture);
            style.texture.clear();
        }
    };

    std::vector<std::unique_ptr<Kept>>        g_kept; ///< oldest first
    std::unordered_map<wxl_handle, Kept*>     g_byHandle;
    uint64_t                                  g_serial = 0;
    std::vector<Immediate>                    g_immediate; ///< the first g_immediateCount are live; the rest keep their memory
    size_t                                    g_immediateCount = 0;
    std::unordered_map<uint32_t, uint32_t>    g_immediateOf; ///< by mod, since the last world drawn
    bool                                      g_worldThisFrame = false;
    double                                    g_immediateSeconds = 0.0; ///< spent in draw() since the last world drawn

    wxl_draw_stats g_stats{};
    uint32_t       g_leftOutSince = 0; ///< shapes left out since the last line said so
    double         g_leftOutSaidAt = 0.0;
    std::set<std::pair<uint32_t, std::string>> g_said;
    std::vector<std::string>                   g_failures; ///< the device's refusals, each logged once

    sh::Ground*              g_testGround = nullptr;
    wxl::vk::render::Render* g_testRender = nullptr;
    wxl::vk::render::Shapes  g_vulkan;
    wxl::vk::render::Render* g_attached = nullptr;

    // The frame's vertices, their memory kept from frame to frame.
    /// What a frame draws of a class of shapes, by part: plain insides, surfaces (a run each),
    /// volumes far to near (plain runs and their faces' surface runs), lines' shadows, lines. Run
    /// ranges are within each part's own vertices until the frame's stream is put together.
    struct Bucket
    {
        std::vector<sh::Vertex> fill, surfaces, volumes, shadows, lines, particles;
        std::vector<sh::Run>    surfaceRuns, volumeRuns, particleRuns;
        float                   occluded = 0.0f; ///< an occluded shape's own bucket: its hidden part's opacity

        void Clear()
        {
            fill.clear();
            surfaces.clear();
            volumes.clear();
            shadows.clear();
            lines.clear();
            surfaceRuns.clear();
            volumeRuns.clear();
            particles.clear();
            particleRuns.clear();
        }
    };

    Bucket                   g_buckets[2];  ///< tested, on top
    std::deque<Bucket>       g_occluded;    ///< a bucket per shape with an occluded_alpha (a deque: volumes hold them by address while more are made); the first g_occludedCount live
    size_t                   g_occludedCount = 0;
    std::vector<sh::Vertex>  g_stream;
    std::vector<sh::Run>     g_runs;
    std::vector<sh::Surface> g_surfaces;
    uint32_t                 g_reused = 0; ///< immediate shapes not built again since the last world drawn

    bool SameDesc(const sh::Desc& a, const sh::Desc& b)
    {
        const auto same = [](sh::V3 p, sh::V3 q) { return p.x == q.x && p.y == q.y && p.z == q.z; };
        if (a.kind != b.kind || a.flags != b.flags || !same(a.center, b.center) || a.radius != b.radius || a.inner != b.inner ||
            a.angle != b.angle || a.facing != b.facing || a.pitch != b.pitch || a.roll != b.roll || !same(a.size, b.size) ||
            a.spacing != b.spacing || a.points.size() != b.points.size() || a.alphas != b.alphas)
            return false;
        for (size_t i = 0; i < a.points.size(); ++i)
            if (!same(a.points[i], b.points[i])) return false;
        return true;
    }

    /// wxl.terrain's ground, its chunks' heights read once per change of the ground.
    class TerrainGround final : public sh::Ground
    {
    public:
        bool Present() override
        {
            Sync();
            if (presence_ < 0) presence_ = services::terrain::Present() ? 1 : 0;
            return presence_ == 1;
        }

        bool Chunk(int32_t chunk_x, int32_t chunk_y, const float*& heights, uint16_t& holes) override
        {
            Sync();
            const uint32_t key = static_cast<uint32_t>(chunk_y) * 1024u + static_cast<uint32_t>(chunk_x);
            auto           at  = chunks_.find(key);
            if (at == chunks_.end())
            {
                Entry entry;
                entry.loaded = services::terrain::ChunkGround(chunk_x, chunk_y, entry.heights, entry.holes);
                at           = chunks_.emplace(key, entry).first;
            }
            if (!at->second.loaded) return false;
            heights = at->second.heights;
            holes   = at->second.holes;
            return true;
        }

        void Forget()
        {
            chunks_.clear();
            presence_ = -1;
        }

    private:
        struct Entry
        {
            float    heights[145];
            uint16_t holes = 0;
            bool     loaded = false;
        };

        void Sync()
        {
            const uint64_t now = services::terrain::Generation();
            if (now == generation_) return;
            generation_ = now;
            Forget();
        }

        std::unordered_map<uint32_t, Entry> chunks_;
        uint64_t                            generation_ = 0;
        int                                 presence_ = -1;
    };

    TerrainGround g_terrain;

    sh::Ground& GroundOf() { return g_testGround ? *g_testGround : static_cast<sh::Ground&>(g_terrain); }
    uint64_t    GenerationOf() { return g_testGround ? 0 : services::terrain::Generation(); }

    wxl::host::ModHost* HostOf() { return Host(); }

    uint32_t Caller()
    {
        const wxl::host::ModHost* host = HostOf();
        return host ? host->CurrentMod() : 0u;
    }

    std::string ModName(uint32_t mod)
    {
        const wxl::host::ModHost* host = HostOf();
        return host && mod && mod <= host->Loaded().size() ? host->Loaded()[mod - 1].id : std::string("a mod");
    }

    /// A refusal, logged once per mod and call.
    void Refuse(const char* call, const std::string& why)
    {
        const uint32_t mod = Caller();
        if (!g_said.insert({mod, call}).second) return;
        WLOG_WARN(kTag, "wxl.draw.%s from %s was refused: %s", call, ModName(mod).c_str(), why.c_str());
    }

    bool OnMainThread(const char* call)
    {
        static std::atomic<bool> said{false};
        const wxl::host::ModHost* host = HostOf();
        if (!host || host->OnMainThread()) return true;
        if (!said.exchange(true))
            WLOG_WARN(kTag, "wxl.draw.%s was called off the main thread, where the world is drawn; it was refused", call);
        return false;
    }

    /// The Vulkan renderer's wxl.render, when the client draws through it.
    wxl::vk::render::Render* VulkanRender()
    {
        if (g_testRender) return g_testRender;
        wxl::vk::gxfront::Front* front = gx::VulkanFront();
        return front ? &front->Mods() : nullptr;
    }

    /// Null when shapes can be drawn now (or once a device is up); otherwise why not.
    const char* Refusal()
    {
        if (VulkanRender()) return nullptr;
        if (!gx::d3d9::shapes::HasDevice()) return nullptr; // no device yet, or one being made again
        return gx::d3d9::shapes::Unsupported();
    }

    // --- Descriptions --------------------------------------------------------------------------------

    sh::V3 V(wxl_vec3 v) { return sh::V3{v.x, v.y, v.z}; }

    bool ReadShape(const char* call, const wxl_draw_shape* in, sh::Desc& out)
    {
        if (!in)
        {
            Refuse(call, "the shape is null");
            return false;
        }
        if (in->struct_size < WXL_DRAW_SHAPE_SIZE_0136)
        {
            Refuse(call, "the shape's struct_size is not set: set it to sizeof(wxl_draw_shape)");
            return false;
        }
        out.kind    = in->kind;
        out.flags   = in->flags;
        out.center  = V(in->center);
        out.radius  = in->radius;
        out.inner   = in->inner;
        out.angle   = in->angle;
        out.facing  = in->facing;
        out.pitch   = in->pitch;
        out.roll    = in->roll;
        out.size    = V(in->size);
        out.spacing = in->spacing;
        out.points.clear();
        if (in->point_count)
        {
            if (!in->points)
            {
                Refuse(call, "the shape has a point_count and no points");
                return false;
            }
            if (in->point_count > WXL_DRAW_MAX_POINTS)
            {
                Refuse(call, "the shape has " + std::to_string(in->point_count) + " points, more than a shape may (" +
                                 std::to_string(WXL_DRAW_MAX_POINTS) + ")");
                return false;
            }
            out.points.resize(in->point_count);
            for (uint32_t i = 0; i < in->point_count; ++i) out.points[i] = V(in->points[i]);
        }
        out.alphas.clear();
        if (in->struct_size >= sizeof(wxl_draw_shape) && in->point_alphas && in->point_count)
            out.alphas.assign(in->point_alphas, in->point_alphas + in->point_count);
        if (std::string why = sh::Validate(out); !why.empty())
        {
            Refuse(call, "the shape: " + why);
            return false;
        }
        return true;
    }

    bool ReadStyle(const char* call, const wxl_draw_style* in, Style& out)
    {
        out = Style{};
        if (!in) return true;
        if (in->struct_size < WXL_DRAW_STYLE_SIZE_0136)
        {
            Refuse(call, "the style's struct_size is not set: set it to sizeof(wxl_draw_style)");
            return false;
        }
        const float numbers[] = {in->alpha, in->fill_alpha, in->width, in->pulse, in->fade_in, in->fade_out};
        for (float n : numbers)
            if (!std::isfinite(n) || n < 0.0f)
            {
                Refuse(call, "the style's alpha, fill_alpha, width, pulse and fades must be numbers from 0");
                return false;
            }
        if (in->depth > WXL_DRAW_DEPTH_ON_TOP)
        {
            Refuse(call, "the style's depth must be WXL_DRAW_DEPTH_TESTED (0) or WXL_DRAW_DEPTH_ON_TOP (1)");
            return false;
        }
        out.color    = in->color & 0xFFFFFFu;
        out.alpha    = std::min(in->alpha, 1.0f);
        out.fill     = std::min(in->fill_alpha, 1.0f);
        out.width    = std::clamp(in->width, 0.5f, WXL_DRAW_MAX_WIDTH);
        out.depth    = in->depth;
        out.pulse    = in->pulse;
        out.fade_in  = in->fade_in;
        out.fade_out = in->fade_out;
        out.fill_color = out.color;
        if (in->struct_size < WXL_DRAW_STYLE_SIZE_0138) return true; // abi 0.1.36's look; abi 0.1.38's goes on below

        const auto refuse = [&](const char* why) {
            Refuse(call, std::string("the style's ") + why);
            return false;
        };
        const auto number = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
        if (in->flags & ~(WXL_DRAW_STYLE_FILL_COLOR | WXL_DRAW_STYLE_LINE_ALPHA | WXL_DRAW_STYLE_MASK))
            return refuse("flags take WXL_DRAW_STYLE_FILL_COLOR (1), WXL_DRAW_STYLE_LINE_ALPHA (2) and WXL_DRAW_STYLE_MASK (4) alone");
        if (!number(in->softness, 0.0f, 1e9f) || !number(in->shadow_softness, 0.0f, 1e9f))
            return refuse("softness and shadow_softness must be pixels from 0");
        if (!number(in->shadow_alpha, 0.0f, 1.0f)) return refuse("shadow_alpha must be an opacity from 0 to 1");
        if (!std::isfinite(in->shadow_x) || !std::isfinite(in->shadow_y)) return refuse("shadow_x and shadow_y must be finite pixels");
        if ((in->flags & WXL_DRAW_STYLE_LINE_ALPHA) && (!number(in->alpha_start, 0.0f, 1.0f) || !number(in->alpha_end, 0.0f, 1.0f)))
            return refuse("alpha_start and alpha_end must be opacities from 0 to 1");
        if (in->uv > WXL_DRAW_UV_WORLD) return refuse("uv must be WXL_DRAW_UV_FIT (0), WXL_DRAW_UV_TILE (1) or WXL_DRAW_UV_WORLD (2)");
        if (in->blend > WXL_DRAW_BLEND_MODULATE)
            return refuse("blend must be WXL_DRAW_BLEND_ALPHA (0), WXL_DRAW_BLEND_ADD (1) or WXL_DRAW_BLEND_MODULATE (2)");
        if (!number(in->texture_right, 0.0f, 1.0f) || !number(in->texture_bottom, 0.0f, 1.0f))
            return refuse("texture_right and texture_bottom must be parts of the texture from 0 to 1 (0: 1)");
        if (!number(in->uv_size_x, 0.0f, 1e6f) || !number(in->uv_size_y, 0.0f, 1e6f) || !std::isfinite(in->uv_rotation) ||
            !std::isfinite(in->uv_offset_x) || !std::isfinite(in->uv_offset_y))
            return refuse("uv_size must be sizes from 0 (0: 1), uv_rotation and uv_offset finite numbers");
        if (!number(in->feather, 0.0f, WXL_DRAW_MAX_EXTENT)) return refuse("feather must be yards from 0 to WXL_DRAW_MAX_EXTENT");
        if (!number(in->radial_fade, 0.0f, 1.0f)) return refuse("radial_fade must be a part of the radius from 0 to 1");
        std::string path;
        if (in->texture && *in->texture)
        {
            const size_t length = strnlen(in->texture, WXL_DRAW_MAX_TEXTURE_PATH);
            if (length >= WXL_DRAW_MAX_TEXTURE_PATH) return refuse("texture path is longer than WXL_DRAW_MAX_TEXTURE_PATH");
            path.assign(in->texture, length);
        }
        out.fill_color      = (in->flags & WXL_DRAW_STYLE_FILL_COLOR) ? (in->fill_color & 0xFFFFFFu) : out.color;
        out.softness        = std::min(in->softness, WXL_DRAW_MAX_SOFTNESS);
        out.along           = (in->flags & WXL_DRAW_STYLE_LINE_ALPHA) != 0;
        out.alpha_start     = out.along ? in->alpha_start : 1.0f;
        out.alpha_end       = out.along ? in->alpha_end : 1.0f;
        out.shadow_color    = in->shadow_color & 0xFFFFFFu;
        out.shadow_alpha    = in->shadow_alpha;
        out.shadow_x        = std::clamp(in->shadow_x, -WXL_DRAW_MAX_SHADOW_OFFSET, WXL_DRAW_MAX_SHADOW_OFFSET);
        out.shadow_y        = std::clamp(in->shadow_y, -WXL_DRAW_MAX_SHADOW_OFFSET, WXL_DRAW_MAX_SHADOW_OFFSET);
        out.shadow_softness = std::min(in->shadow_softness, WXL_DRAW_MAX_SOFTNESS);
        out.look.textured   = !path.empty();
        out.look.right      = in->texture_right > 0.0f ? in->texture_right : 1.0f;
        out.look.bottom     = in->texture_bottom > 0.0f ? in->texture_bottom : 1.0f;
        out.look.uv         = in->uv;
        out.look.size_x     = in->uv_size_x > 0.0f ? in->uv_size_x : 1.0f;
        out.look.size_y     = in->uv_size_y > 0.0f ? in->uv_size_y : 1.0f;
        out.look.rotation   = in->uv_rotation;
        out.look.offset_x   = in->uv_offset_x;
        out.look.offset_y   = in->uv_offset_y;
        out.look.blend      = in->blend;
        out.look.feather    = in->feather;
        out.look.radial     = in->radial_fade;
        if (in->struct_size >= WXL_DRAW_STYLE_SIZE_0139)
        {
            if (!number(in->occluded_alpha, 0.0f, 1.0f)) return refuse("occluded_alpha must be an opacity from 0 to 1");
            out.occluded = in->occluded_alpha;
        }
        if (in->struct_size >= sizeof(wxl_draw_style))
        {
            if (!number(in->texture_blur, 0.0f, 8.0f)) return refuse("texture_blur must be from 0 to 8");
            out.look.blur = in->texture_blur;
            out.look.mask = (in->flags & WXL_DRAW_STYLE_MASK) != 0;
        }
        if (!path.empty())
        {
            // Held from here: the caller lets it go when the style is dropped (or refused later).
            const std::string key = TextureKey(path.c_str());
            if (!HoldTexture(key, path)) return refuse(("texture '" + path + "' was not made by the client").c_str());
            out.texture = key;
        }
        return true;
    }

    /// The shape placed on its object: centre and points turned and moved, facing turned.
    sh::Desc Placed(const Kept& k, sh::V3 at, float facing)
    {
        sh::Desc d = k.desc;
        float    c = 1.0f, s = 0.0f;
        if (k.attach & WXL_DRAW_ATTACH_TURN)
        {
            c = std::cos(facing);
            s = std::sin(facing);
            d.facing += facing;
            for (sh::V3& p : d.points) p = sh::V3{p.x * c - p.y * s, p.x * s + p.y * c, p.z};
        }
        const sh::V3 o = k.desc.center;
        d.center       = sh::V3{at.x + o.x * c - o.y * s, at.y + o.x * s + o.y * c, at.z + o.z};
        return d;
    }

    Kept* Find(wxl_handle handle)
    {
        const auto at = g_byHandle.find(handle);
        if (at == g_byHandle.end() || at->second->mod != Caller()) return nullptr;
        return at->second;
    }

    void Drop(Kept* k)
    {
        g_byHandle.erase(k->handle);
        g_kept.erase(std::remove_if(g_kept.begin(), g_kept.end(), [&](const std::unique_ptr<Kept>& p) { return p.get() == k; }),
                     g_kept.end());
    }

    /// Builds a kept shape where it stands now: false when it could not be (logged once).
    bool BuildKept(Kept& k, const sh::Desc& desc)
    {
        std::string why;
        k.generation = GenerationOf();
        k.built      = sh::Build(desc, k.style.Parts(), &GroundOf(), k.mesh, &why);
        if (!k.built && g_said.insert({k.mod, "build:" + why}).second)
            WLOG_WARN(kTag, "a shape of %s could not be drawn where it is now: %s", ModName(k.mod).c_str(), why.c_str());
        return k.built;
    }

    // --- The table -----------------------------------------------------------------------------------

    wxl_bool WXL_CALL Supported() { return Refusal() ? WXL_FALSE : WXL_TRUE; }

    wxl_str WXL_CALL UnsupportedReason()
    {
        const char* why = Refusal();
        if (!why) why = "";
        return wxl_str{why, static_cast<uint32_t>(std::strlen(why))};
    }

    wxl_result WXL_CALL DrawNow(const wxl_draw_shape* shape, const wxl_draw_style* style)
    {
        if (!OnMainThread("draw")) return WXL_E_INVALID;
        const auto started = Clock::now();
        if (const char* why = Refusal())
        {
            Refuse("draw", why);
            return WXL_E_UNSUPPORTED;
        }
        const uint32_t mod   = Caller();
        uint32_t&      count = g_immediateOf[mod];
        if (count >= WXL_DRAW_MAX_IMMEDIATE)
        {
            Refuse("draw", "the mod drew " + std::to_string(WXL_DRAW_MAX_IMMEDIATE) +
                               " shapes since the world was last drawn, as many as a frame takes");
            return WXL_E_INVALID;
        }
        static sh::Desc desc; // its memory kept from call to call
        Style           look;
        if (!ReadShape("draw", shape, desc) || !ReadStyle("draw", style, look)) return WXL_E_INVALID;
        if (g_immediateCount == g_immediate.size()) g_immediate.emplace_back();
        Immediate&      slot  = g_immediate[g_immediateCount];
        std::string     why;
        const sh::Parts parts = look.Parts();
        const uint64_t  ground = GenerationOf();
        if (slot.valid && slot.mod == mod && slot.generation == ground && slot.parts.lines == parts.lines && slot.parts.fill == parts.fill &&
            slot.parts.feather == parts.feather && SameDesc(slot.desc, desc))
            ++g_reused; // as the frame before: its mesh stands
        else
        {
            slot.valid = false;
            if (!sh::Build(desc, parts, &GroundOf(), slot.mesh, &why))
            {
                LetGoTexture(look.texture);
                Refuse("draw", "the shape: " + why);
                return WXL_E_INVALID;
            }
            slot.desc       = desc;
            slot.parts      = parts;
            slot.generation = ground;
            slot.valid      = true;
        }
        slot.mod   = mod;
        slot.style = look; // the texture held for it until it is drawn (Immediate::Done)
        ++g_immediateCount;
        ++count;
        g_immediateSeconds += std::chrono::duration<double>(Clock::now() - started).count();
        return WXL_OK;
    }

    wxl_handle WXL_CALL AddShape(const wxl_draw_shape* shape, const wxl_draw_style* style, float duration)
    {
        if (!OnMainThread("add")) return WXL_HANDLE_NONE;
        if (const char* why = Refusal())
        {
            Refuse("add", why);
            return WXL_HANDLE_NONE;
        }
        const uint32_t mod  = Caller();
        size_t         held = 0;
        for (const auto& k : g_kept) held += k->mod == mod ? 1 : 0;
        if (held >= WXL_DRAW_MAX_SHAPES)
        {
            Refuse("add", "the mod keeps " + std::to_string(WXL_DRAW_MAX_SHAPES) + " shapes, as many as it may: remove some first");
            return WXL_HANDLE_NONE;
        }
        if (!std::isfinite(duration) || duration < 0.0f)
        {
            Refuse("add", "the duration must be seconds from 0 (0: until removed)");
            return WXL_HANDLE_NONE;
        }
        auto k = std::make_unique<Kept>();
        if (!ReadShape("add", shape, k->desc) || !ReadStyle("add", style, k->style)) return WXL_HANDLE_NONE;
        std::string why;
        k->generation = GenerationOf();
        if (!sh::Build(k->desc, k->style.Parts(), &GroundOf(), k->mesh, &why))
        {
            Refuse("add", "the shape: " + why);
            return WXL_HANDLE_NONE;
        }
        k->built  = true;
        k->mod    = mod;
        k->born   = Now();
        k->ends   = duration > 0.0f ? k->born + duration : 0.0;
        k->handle = static_cast<wxl_handle>(++g_serial) | (static_cast<wxl_handle>(0xD7u) << 56);
        const wxl_handle handle = k->handle;
        g_byHandle[handle]      = k.get();
        g_kept.push_back(std::move(k));
        return handle;
    }

    wxl_result WXL_CALL Update(wxl_handle handle, const wxl_draw_shape* shape)
    {
        if (!OnMainThread("update")) return WXL_E_INVALID;
        Kept* k = Find(handle);
        if (!k) return WXL_E_NOT_FOUND;
        sh::Desc desc;
        if (!ReadShape("update", shape, desc)) return WXL_E_INVALID;
        sh::Mesh    mesh;
        std::string why;
        // Built where it stands now, so that a shape refused is refused here, not at the next
        // frame; an attached one is built on its object at the next frame.
        if (!k->target && !sh::Build(desc, k->style.Parts(), &GroundOf(), mesh, &why))
        {
            Refuse("update", "the shape: " + why);
            return WXL_E_INVALID;
        }
        k->desc = std::move(desc);
        if (!k->target)
        {
            k->mesh       = std::move(mesh);
            k->built      = true;
            k->generation = GenerationOf();
        }
        else
            k->built = false; // built on its object at the next frame
        return WXL_OK;
    }

    wxl_result WXL_CALL Move(wxl_handle handle, wxl_vec3 center, float facing)
    {
        if (!OnMainThread("move")) return WXL_E_INVALID;
        Kept* k = Find(handle);
        if (!k) return WXL_E_NOT_FOUND;
        if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) || !std::isfinite(facing))
        {
            Refuse("move", "the center and the facing must be finite numbers");
            return WXL_E_INVALID;
        }
        k->desc.center = V(center);
        k->desc.facing = facing;
        k->built       = false;
        return WXL_OK;
    }

    wxl_result WXL_CALL SetStyle(wxl_handle handle, const wxl_draw_style* style)
    {
        if (!OnMainThread("set_style")) return WXL_E_INVALID;
        Kept* k = Find(handle);
        if (!k) return WXL_E_NOT_FOUND;
        Style look;
        if (!ReadStyle("set_style", style, look)) return WXL_E_INVALID;
        const sh::Parts before = k->style.Parts(), after = look.Parts();
        LetGoTexture(k->style.texture);
        k->style = look;
        if (before.lines != after.lines || before.fill != after.fill || (k->desc.kind == sh::kPolygon && before.feather != after.feather))
            k->built = false;
        return WXL_OK;
    }

    wxl_result WXL_CALL SetDuration(wxl_handle handle, float seconds)
    {
        if (!OnMainThread("set_duration")) return WXL_E_INVALID;
        Kept* k = Find(handle);
        if (!k) return WXL_E_NOT_FOUND;
        if (!std::isfinite(seconds) || seconds < 0.0f)
        {
            Refuse("set_duration", "the duration must be seconds from 0 (0: until removed)");
            return WXL_E_INVALID;
        }
        k->ends = seconds > 0.0f ? Now() + seconds : 0.0;
        return WXL_OK;
    }

    wxl_result WXL_CALL Attach(wxl_handle handle, wxl_handle target, uint32_t flags)
    {
        if (!OnMainThread("attach")) return WXL_E_INVALID;
        Kept* k = Find(handle);
        if (!k) return WXL_E_NOT_FOUND;
        if (!objects::Service().Handles().Resolve(target)) return WXL_E_NOT_FOUND;
        if (flags & ~WXL_DRAW_ATTACH_TURN)
        {
            Refuse("attach", "the flags take WXL_DRAW_ATTACH_TURN (1) alone");
            return WXL_E_INVALID;
        }
        k->target = target;
        k->attach = flags;
        k->built  = false;
        return WXL_OK;
    }

    wxl_result WXL_CALL Detach(wxl_handle handle)
    {
        if (!OnMainThread("detach")) return WXL_E_INVALID;
        Kept* k = Find(handle);
        if (!k) return WXL_E_NOT_FOUND;
        if (!k->target) return WXL_OK;
        k->desc   = Placed(*k, k->placed, k->turned);
        k->target = WXL_HANDLE_NONE;
        k->attach = 0;
        k->built  = false;
        return WXL_OK;
    }

    void WXL_CALL Remove(wxl_handle handle)
    {
        if (!OnMainThread("remove")) return;
        if (Kept* k = Find(handle)) Drop(k);
    }

    void WXL_CALL ClearMine()
    {
        if (!OnMainThread("clear")) return;
        services::draw::ForgetMod(Caller());
    }

    wxl_result WXL_CALL Stats(wxl_draw_stats* out)
    {
        if (!out || out->struct_size < sizeof(uint32_t)) return WXL_E_INVALID;
        wxl_draw_stats stats = g_stats;
        stats.struct_size    = out->struct_size;
        stats.shapes         = static_cast<uint32_t>(g_kept.size());
        stats.textures       = static_cast<uint32_t>(g_textures.size());
        stats.textures_loading = 0;
        for (const auto& [key, t] : g_textures) stats.textures_loading += t.refs && !t.device ? 1u : 0u;
        std::memcpy(out, &stats, std::min<size_t>(out->struct_size, sizeof stats));
        return WXL_OK;
    }

    // --- Particles (abi 0.1.40) ---------------------------------------------------------------------

    sh::Camera g_camera; ///< read with the world's view each frame

    /// An emitter: kept (a handle) or thrown (emit, or a kept one stopped), and its particles.
    struct Emitter
    {
        uint32_t    mod = 0;
        wxl_handle  handle = WXL_HANDLE_NONE; ///< none once thrown or stopped
        sh::Emitter desc;
        float       rate = 0.0f;
        double      ends = 0.0;   ///< its rate stops then; 0 never
        bool        stopped = false;
        bool        paused = false; ///< the object it followed left view: no rate until placed again
        float       carry = 0.0f;   ///< the part of a particle its rate owes
        sh::Look    look;
        std::string texture;      ///< the key it is held under (Textures)
        uint32_t    blend = WXL_DRAW_BLEND_ALPHA;
        uint32_t    depth = WXL_DRAW_DEPTH_TESTED;
        float       occluded = 0.0f;
        wxl_handle  target = WXL_HANDLE_NONE;
        uint32_t    attach = 0;
        sh::V3      at;           ///< where it throws from now
        float       facing = 0.0f;
        sh::Pool    pool;
        sh::Random  random;
        ~Emitter() { LetGoTexture(texture); }
    };

    std::vector<std::unique_ptr<Emitter>>  g_emitters;
    std::unordered_map<wxl_handle, Emitter*> g_emitterByHandle;
    uint64_t                               g_emitterSerial = 0;
    uint32_t                               g_particlesAlive = 0;
    std::unordered_map<uint32_t, uint32_t> g_particlesOf; ///< alive by mod
    uint32_t                               g_emittedFrame = 0, g_culled = 0;
    double                                 g_lastStep = 0.0;

    Emitter* FindEmitter(wxl_handle handle)
    {
        const auto at = g_emitterByHandle.find(handle);
        return at == g_emitterByHandle.end() || at->second->mod != Caller() ? nullptr : at->second;
    }

    void DropEmitter(size_t index)
    {
        Emitter& e = *g_emitters[index];
        if (e.handle) g_emitterByHandle.erase(e.handle);
        g_particlesAlive -= static_cast<uint32_t>(std::min<size_t>(e.pool.Size(), g_particlesAlive));
        uint32_t& mine = g_particlesOf[e.mod];
        mine -= static_cast<uint32_t>(std::min<size_t>(e.pool.Size(), mine));
        g_emitters.erase(g_emitters.begin() + static_cast<std::ptrdiff_t>(index));
    }

    /// Throws up to @p count particles, within the budgets; the rest are counted as culled.
    void Throw(Emitter& e, uint32_t count)
    {
        uint32_t& mine    = g_particlesOf[e.mod];
        const uint32_t room = std::min(WXL_DRAW_MAX_PARTICLES - std::min(g_particlesAlive, WXL_DRAW_MAX_PARTICLES),
                                       WXL_DRAW_MAX_MOD_PARTICLES - std::min(mine, WXL_DRAW_MAX_MOD_PARTICLES));
        const uint32_t n    = std::min(count, room);
        g_culled += count - n;
        if (!n) return;
        e.pool.Spawn(e.desc, e.at, e.facing, n, e.random);
        g_particlesAlive += n;
        mine += n;
        g_emittedFrame += n;
    }

    uint32_t EmittersOf(uint32_t mod)
    {
        uint32_t n = 0;
        for (const auto& e : g_emitters) n += e->mod == mod ? 1u : 0u;
        return n;
    }

    /// An emitter's description, checked: false (logged) when refused.
    bool ReadEmitter(const char* call, const wxl_draw_emitter* in, sh::Emitter& out, uint32_t& burst, float& rate, float& duration,
                     uint32_t& seed)
    {
        if (!in)
        {
            Refuse(call, "the emitter is null");
            return false;
        }
        if (in->struct_size < sizeof(wxl_draw_emitter))
        {
            Refuse(call, "the emitter's struct_size is not set: set it to sizeof(wxl_draw_emitter)");
            return false;
        }
        const auto refuse = [&](const char* why) {
            Refuse(call, std::string("the emitter's ") + why);
            return false;
        };
        const auto finite = [](wxl_vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        const auto range  = [](float a, float b, float lo, float hi) {
            return std::isfinite(a) && std::isfinite(b) && a >= lo && b >= lo && a <= hi && b <= hi;
        };
        if (in->spawn > WXL_DRAW_SPAWN_SEGMENT) return refuse("spawn must be WXL_DRAW_SPAWN_POINT, DISC, BOX or SEGMENT (0 to 3)");
        if (in->ground > WXL_DRAW_GROUND_SETTLE) return refuse("ground must be WXL_DRAW_GROUND_NONE, BOUNCE or SETTLE (0 to 2)");
        if (!finite(in->center) || !finite(in->extent) || !finite(in->direction) || !finite(in->attractor) || !std::isfinite(in->facing))
            return refuse("center, extent, direction, attractor and facing must be finite numbers");
        if ((in->spawn == WXL_DRAW_SPAWN_DISC || in->spawn == WXL_DRAW_SPAWN_BOX) &&
            (in->extent.x < 0.0f || in->extent.y < 0.0f || in->extent.z < 0.0f || in->extent.x > WXL_DRAW_MAX_EXTENT ||
             in->extent.y > WXL_DRAW_MAX_EXTENT || in->extent.z > WXL_DRAW_MAX_EXTENT))
            return refuse("extent of a disc or a box must be yards from 0 to WXL_DRAW_MAX_EXTENT");
        if (!(in->spread >= 0.0f && in->spread <= 3.1416f)) return refuse("spread must be radians from 0 to pi");
        if (in->burst > WXL_DRAW_MAX_BURST) return refuse("burst is more than WXL_DRAW_MAX_BURST particles");
        if (!(in->rate >= 0.0f && in->rate <= WXL_DRAW_MAX_RATE)) return refuse("rate must be particles a second from 0 to WXL_DRAW_MAX_RATE");
        if (!(in->duration >= 0.0f) || !std::isfinite(in->duration)) return refuse("duration must be seconds from 0 (0: until stopped)");
        if (!range(in->speed_min, in->speed_max, 0.0f, 1000.0f)) return refuse("speed_min and speed_max must be yards a second from 0 to 1000");
        if (!range(in->life_min, in->life_max, 0.001f, WXL_DRAW_MAX_PARTICLE_LIFE))
            return refuse("life_min and life_max must be seconds above 0, up to WXL_DRAW_MAX_PARTICLE_LIFE");
        if (!range(in->size_min, in->size_max, 0.0f, WXL_DRAW_MAX_PARTICLE_SIZE))
            return refuse("size_min and size_max must be yards from 0 to WXL_DRAW_MAX_PARTICLE_SIZE");
        if (!range(in->spin_min, in->spin_max, -100.0f, 100.0f)) return refuse("spin_min and spin_max must be radians a second, -100 to 100");
        if (!std::isfinite(in->gravity) || !std::isfinite(in->attraction)) return refuse("gravity and attraction must be finite numbers");
        if (!(in->drag >= 0.0f && in->drag <= 100.0f)) return refuse("drag must be from 0 to 100 a second");
        if (!(in->restitution >= 0.0f && in->restitution <= 1.0f)) return refuse("restitution must be from 0 to 1");
        if (!(in->friction >= 0.0f && in->friction <= 100.0f)) return refuse("friction must be from 0 to 100");
        if (!(in->attract_radius >= 0.0f) || !std::isfinite(in->attract_radius)) return refuse("attract_radius must be yards from 0");
        out.spawn          = in->spawn;
        out.center         = V(in->center);
        out.extent         = V(in->extent);
        out.facing         = in->facing;
        out.direction      = V(in->direction);
        out.spread         = in->spread;
        out.speed_min      = std::min(in->speed_min, in->speed_max);
        out.speed_max      = std::max(in->speed_min, in->speed_max);
        out.life_min       = std::min(in->life_min, in->life_max);
        out.life_max       = std::max(in->life_min, in->life_max);
        out.size_min       = std::min(in->size_min, in->size_max);
        out.size_max       = std::max(in->size_min, in->size_max);
        out.spin_min       = std::min(in->spin_min, in->spin_max);
        out.spin_max       = std::max(in->spin_min, in->spin_max);
        out.gravity        = in->gravity;
        out.drag           = in->drag;
        out.ground         = in->ground;
        out.restitution    = in->restitution;
        out.friction       = in->friction;
        out.attractor      = V(in->attractor);
        out.attraction     = in->attraction;
        out.attract_radius = in->attract_radius;
        burst              = in->burst;
        rate               = in->rate;
        duration           = in->duration;
        seed               = in->seed;
        return true;
    }

    /// A look, checked; null: soft white blots fading out. Its texture is held from here when
    /// accepted (the caller lets it go).
    bool ReadLook(const char* call, const wxl_draw_particles* in, Emitter& out)
    {
        sh::Look look;
        uint32_t blend = WXL_DRAW_BLEND_ALPHA, depth = WXL_DRAW_DEPTH_TESTED;
        float    occluded = 0.0f;
        std::string key;
        if (in)
        {
            if (in->struct_size < sizeof(wxl_draw_particles))
            {
                Refuse(call, "the look's struct_size is not set: set it to sizeof(wxl_draw_particles)");
                return false;
            }
            const auto refuse = [&](const char* why) {
                Refuse(call, std::string("the look's ") + why);
                return false;
            };
            const auto unit = [](float v) { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; };
            if (in->mode > WXL_DRAW_BILLBOARD_STRETCHED) return refuse("mode must be WXL_DRAW_BILLBOARD_CAMERA, GROUND or STRETCHED (0 to 2)");
            if (in->blend > WXL_DRAW_BLEND_MODULATE) return refuse("blend must be WXL_DRAW_BLEND_ALPHA, ADD or MODULATE (0 to 2)");
            if (in->depth > WXL_DRAW_DEPTH_ON_TOP) return refuse("depth must be WXL_DRAW_DEPTH_TESTED or WXL_DRAW_DEPTH_ON_TOP");
            if (!unit(in->texture_right) || !unit(in->texture_bottom)) return refuse("texture_right and texture_bottom must be from 0 to 1 (0: 1)");
            const uint32_t fx = std::max(in->frames_x, 1u), fy = std::max(in->frames_y, 1u);
            if (fx > 64 || fy > 64) return refuse("frames_x and frames_y must be 64 at most");
            if (!(in->frame_rate >= 0.0f && in->frame_rate <= 240.0f)) return refuse("frame_rate must be frames a second from 0 to 240");
            if (!unit(in->occluded_alpha)) return refuse("occluded_alpha must be an opacity from 0 to 1");
            if (!(in->stretch >= 0.0f && in->stretch <= 10.0f)) return refuse("stretch must be seconds from 0 to 10");
            if (!(in->softness >= 0.0f && in->softness <= WXL_DRAW_MAX_PARTICLE_SIZE)) return refuse("softness must be yards from 0");
            if (in->key_count > WXL_DRAW_MAX_PARTICLE_KEYS) return refuse("key_count is more than WXL_DRAW_MAX_PARTICLE_KEYS");
            if (in->flags & ~WXL_DRAW_PARTICLES_MASK) return refuse("flags take WXL_DRAW_PARTICLES_MASK (1) alone");
            if (!(in->texture_blur >= 0.0f && in->texture_blur <= 8.0f)) return refuse("texture_blur must be from 0 to 8");
            look.mask = (in->flags & WXL_DRAW_PARTICLES_MASK) != 0;
            look.blur = in->texture_blur;
            float last = 0.0f;
            for (uint32_t i = 0; i < in->key_count; ++i)
            {
                const wxl_draw_particle_key& k = in->keys[i];
                if (!unit(k.t) || k.t < last || !unit(k.alpha) || !(k.size >= 0.0f && k.size <= 100.0f))
                    return refuse("keys must go in order of t (0 to 1), with sizes from 0 and opacities from 0 to 1");
                last = k.t;
                look.keys[i] = sh::Key{k.t, k.size, k.color & 0xFFFFFFu, k.alpha};
            }
            look.key_count  = in->key_count;
            look.mode       = in->mode;
            look.right      = in->texture_right > 0.0f ? in->texture_right : 1.0f;
            look.bottom     = in->texture_bottom > 0.0f ? in->texture_bottom : 1.0f;
            look.frames_x   = fx;
            look.frames_y   = fy;
            look.frame_rate = in->frame_rate;
            look.stretch    = in->stretch;
            look.softness   = in->softness;
            blend           = in->blend;
            depth           = in->depth;
            occluded        = in->occluded_alpha;
            if (in->texture && *in->texture)
            {
                const size_t length = strnlen(in->texture, WXL_DRAW_MAX_TEXTURE_PATH);
                if (length >= WXL_DRAW_MAX_TEXTURE_PATH) return refuse("texture path is longer than WXL_DRAW_MAX_TEXTURE_PATH");
                const std::string path(in->texture, length);
                key = TextureKey(path.c_str());
                if (!HoldTexture(key, path)) return refuse(("texture '" + path + "' was not made by the client").c_str());
                look.textured = true;
            }
        }
        LetGoTexture(out.texture);
        out.texture  = key;
        out.look     = look;
        out.blend    = blend;
        out.depth    = depth;
        out.occluded = occluded;
        return true;
    }

    /// A new emitter of the caller's, from a description and a look; null when refused.
    Emitter* MakeEmitter(const char* call, const wxl_draw_emitter* desc, const wxl_draw_particles* look, bool kept)
    {
        if (!OnMainThread(call)) return nullptr;
        if (const char* why = Refusal())
        {
            Refuse(call, why);
            return nullptr;
        }
        const uint32_t mod = Caller();
        if (EmittersOf(mod) >= WXL_DRAW_MAX_EMITTERS)
        {
            Refuse(call, "the mod holds " + std::to_string(WXL_DRAW_MAX_EMITTERS) + " emitters, as many as it may: stop some first");
            return nullptr;
        }
        auto     e = std::make_unique<Emitter>();
        uint32_t burst = 0, seed = 0;
        float    duration = 0.0f;
        if (!ReadEmitter(call, desc, e->desc, burst, e->rate, duration, seed) || !ReadLook(call, look, *e)) return nullptr;
        if (!kept && !burst)
        {
            Refuse(call, "the emitter's burst is 0: emit throws one burst, and this one throws nothing");
            return nullptr;
        }
        static uint32_t counter = 0;
        e->random = sh::Random(seed ? seed : 0x9E3779B9u ^ (++counter * 0x85EBCA6Bu) ^ static_cast<uint32_t>(Now() * 1000.0));
        e->mod    = mod;
        e->at     = e->desc.center;
        e->facing = e->desc.facing;
        e->ends   = kept && duration > 0.0f ? Now() + duration : 0.0;
        e->stopped = !kept;
        if (kept)
        {
            e->handle                     = static_cast<wxl_handle>(++g_emitterSerial) | (static_cast<wxl_handle>(0xE7u) << 56);
            g_emitterByHandle[e->handle]  = e.get();
        }
        Emitter* made = e.get();
        g_emitters.push_back(std::move(e));
        Throw(*made, burst);
        return made;
    }

    wxl_result WXL_CALL EmitNow(const wxl_draw_emitter* emitter, const wxl_draw_particles* look)
    {
        return MakeEmitter("emit", emitter, look, false) ? WXL_OK : (Refusal() ? WXL_E_UNSUPPORTED : WXL_E_INVALID);
    }

    wxl_handle WXL_CALL AddEmitter(const wxl_draw_emitter* emitter, const wxl_draw_particles* look)
    {
        Emitter* e = MakeEmitter("add_emitter", emitter, look, true);
        return e ? e->handle : WXL_HANDLE_NONE;
    }

    wxl_result WXL_CALL UpdateEmitter(wxl_handle handle, const wxl_draw_emitter* desc)
    {
        if (!OnMainThread("update_emitter")) return WXL_E_INVALID;
        Emitter* e = FindEmitter(handle);
        if (!e) return WXL_E_NOT_FOUND;
        sh::Emitter next;
        uint32_t    burst = 0, seed = 0;
        float       rate = 0.0f, duration = 0.0f;
        if (!ReadEmitter("update_emitter", desc, next, burst, rate, duration, seed)) return WXL_E_INVALID;
        e->desc = next;
        e->rate = rate;
        e->ends = duration > 0.0f ? Now() + duration : 0.0;
        if (!e->target)
        {
            e->at     = next.center;
            e->facing = next.facing;
        }
        return WXL_OK;
    }

    wxl_result WXL_CALL SetEmitterStyle(wxl_handle handle, const wxl_draw_particles* look)
    {
        if (!OnMainThread("set_emitter_style")) return WXL_E_INVALID;
        Emitter* e = FindEmitter(handle);
        if (!e) return WXL_E_NOT_FOUND;
        return ReadLook("set_emitter_style", look, *e) ? WXL_OK : WXL_E_INVALID;
    }

    wxl_result WXL_CALL Burst(wxl_handle handle, uint32_t count)
    {
        if (!OnMainThread("burst")) return WXL_E_INVALID;
        Emitter* e = FindEmitter(handle);
        if (!e) return WXL_E_NOT_FOUND;
        if (count > WXL_DRAW_MAX_BURST)
        {
            Refuse("burst", "a burst of more than WXL_DRAW_MAX_BURST particles");
            return WXL_E_INVALID;
        }
        Throw(*e, count);
        return WXL_OK;
    }

    wxl_result WXL_CALL Place(wxl_handle handle, wxl_vec3 center, float facing)
    {
        if (!OnMainThread("place")) return WXL_E_INVALID;
        Emitter* e = FindEmitter(handle);
        if (!e) return WXL_E_NOT_FOUND;
        if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) || !std::isfinite(facing))
        {
            Refuse("place", "the center and the facing must be finite numbers");
            return WXL_E_INVALID;
        }
        e->target      = WXL_HANDLE_NONE;
        e->paused      = false;
        e->desc.center = V(center);
        e->desc.facing = facing;
        e->at          = e->desc.center;
        e->facing      = facing;
        return WXL_OK;
    }

    wxl_result WXL_CALL Follow(wxl_handle handle, wxl_handle target, uint32_t flags)
    {
        if (!OnMainThread("follow")) return WXL_E_INVALID;
        Emitter* e = FindEmitter(handle);
        if (!e) return WXL_E_NOT_FOUND;
        if (!objects::Service().Handles().Resolve(target)) return WXL_E_NOT_FOUND;
        if (flags & ~WXL_DRAW_ATTACH_TURN)
        {
            Refuse("follow", "the flags take WXL_DRAW_ATTACH_TURN (1) alone");
            return WXL_E_INVALID;
        }
        e->target = target;
        e->attach = flags;
        e->paused = false;
        return WXL_OK;
    }

    void WXL_CALL StopEmitter(wxl_handle handle)
    {
        if (!OnMainThread("stop")) return;
        Emitter* e = FindEmitter(handle);
        if (!e) return;
        g_emitterByHandle.erase(handle);
        e->handle  = WXL_HANDLE_NONE;
        e->stopped = true;
    }

    /// The particles move on @p dt seconds: emitters throw at their rate from where they stand, their
    /// particles step, and the emitters with nothing more to throw nor to show go.
    void StepParticles(double now, float dt)
    {
        for (size_t i = g_emitters.size(); i-- > 0;)
        {
            Emitter& e = *g_emitters[i];
            if (e.target)
            {
                void*    object = objects::Service().Handles().Resolve(e.target);
                wxl_vec3 at{};
                float    facing = 0.0f;
                if (!object || binding::object_position(object, &at) != WXL_TRUE)
                {
                    // It left view: the emitter throws no more until it is placed again.
                    e.target = WXL_HANDLE_NONE;
                    e.paused = true;
                }
                else
                {
                    binding::object_facing(object, &facing);
                    float c = 1.0f, s = 0.0f, turn = 0.0f;
                    if (e.attach & WXL_DRAW_ATTACH_TURN)
                    {
                        c    = std::cos(facing);
                        s    = std::sin(facing);
                        turn = facing;
                    }
                    const sh::V3 o = e.desc.center;
                    e.at           = sh::V3{at.x + o.x * c - o.y * s, at.y + o.x * s + o.y * c, at.z + o.z};
                    e.facing       = e.desc.facing + turn;
                }
            }
            if (e.ends > 0.0 && now >= e.ends) e.stopped = true;
            if (!e.stopped && !e.paused && e.rate > 0.0f)
            {
                e.carry += e.rate * dt;
                const uint32_t n = static_cast<uint32_t>(e.carry);
                e.carry -= static_cast<float>(n);
                if (n) Throw(e, n);
            }
            const size_t before = e.pool.Size();
            e.pool.Step(e.desc, dt, &GroundOf(), e.at.z, e.look.softness > 0.0f || e.look.mode == sh::kBillboardGround);
            const uint32_t gone = static_cast<uint32_t>(before - e.pool.Size());
            g_particlesAlive -= std::min(gone, g_particlesAlive);
            uint32_t& mine = g_particlesOf[e.mod];
            mine -= std::min(gone, mine);
            if (e.stopped && !e.handle && e.pool.Size() == 0) DropEmitter(i);
            else if (e.stopped && e.handle && e.ends > 0.0 && e.pool.Size() == 0) DropEmitter(i); // its duration over, its particles gone
        }
    }

    // --- A frame -------------------------------------------------------------------------------------

    uint32_t Color(uint32_t rgb, float alpha)
    {
        const uint32_t a = static_cast<uint32_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
        return (a << 24) | (rgb & 0xFFFFFFu);
    }

    /// The opacity a style has at @p now: its beat, and a kept shape's fades.
    float Beat(const Style& s, double now, double born, double ends)
    {
        float m = 1.0f;
        if (s.pulse > 0.0f) m *= 0.6f + 0.4f * static_cast<float>(std::cos(2.0 * kPi * now / s.pulse));
        if (born > 0.0 && s.fade_in > 0.0f) m *= static_cast<float>(std::clamp((now - born) / s.fade_in, 0.0, 1.0));
        if (ends > 0.0 && s.fade_out > 0.0f) m *= static_cast<float>(std::clamp((ends - now) / s.fade_out, 0.0, 1.0));
        return m;
    }

    /// The world's view as the client draws it now, for both devices.
    bool ReadView(sh::View& out)
    {
        wxl_core_world_view w{};
        if (binding::gx_world_view(&w) != WXL_TRUE) return false;
        // view * projection, rows of the engine's matrices (clip = [p 1] * view * projection).
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += w.view[4 * i + k] * w.projection[4 * k + j];
                out.vp[4 * i + j] = sum;
            }
        // The viewport as the device makes it from its box (CGxDeviceD3d::IXformSetViewport).
        const float width = w.window[0], height = w.window[1];
        const float x = std::floor(w.viewport[0] * width + 0.5f);
        const float y = std::floor((1.0f - w.viewport[3]) * height + 0.5f);
        out.viewport[0] = x;
        out.viewport[1] = y;
        out.viewport[2] = std::floor(w.viewport[1] * width - x + 0.5f);
        out.viewport[3] = std::floor((1.0f - w.viewport[2]) * height - y + 0.5f);
        out.viewport[4] = w.viewport[4];
        out.viewport[5] = w.viewport[5];
        out.eye         = V(w.eye);
        out.scale       = height > 0.0f ? height / 1080.0f : 1.0f;
        // The camera's axes in the world: the view matrix's columns (row vectors: p * view).
        const auto axis = [&](int c) {
            sh::V3      a{w.view[c], w.view[4 + c], w.view[8 + c]};
            const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
            return l > 1e-6f ? sh::V3{a.x / l, a.y / l, a.z / l} : a;
        };
        g_camera.right   = axis(0);
        g_camera.up      = axis(1);
        g_camera.forward = axis(2);
        return out.viewport[2] > 0.0f && out.viewport[3] > 0.0f;
    }

    void Failed(const std::string& why)
    {
        if (why.empty() || std::find(g_failures.begin(), g_failures.end(), why) != g_failures.end() || g_failures.size() >= 32) return;
        g_failures.push_back(why);
        WLOG_INFO(kTag, "wxl.draw: shapes not drawn: %s", why.c_str());
    }
}

namespace
{
    // --- WxlDraw, in the game's interface --------------------------------------------------------

    /// What the interface calls: the native half, which the wrapper keeps as an upvalue and takes
    /// out of the globals.
    constexpr char kNativeName[] = "__wxl_draw";

    /// The interface's half: the shape's and the style's tables flattened into numbers and the
    /// points into a string (the native half reads no table), then the native call. Run with no
    /// taint, so that add-ons call it freely.
    constexpr char kWrapper[] = R"lua(
local native = __wxl_draw
__wxl_draw = nil
local type, ipairs, tostring, format, concat = type, ipairs, tostring, string.format, table.concat
local kinds = { line = 0, circle = 1, ring = 2, sector = 3, rectangle = 4, polygon = 5, grid = 6, box = 7, sphere = 8, cylinder = 9, beam = 10 }
local uvs = { fit = 0, tile = 1, world = 2 }
local blends = { alpha = 0, add = 1, modulate = 2 }
local function num(t, k, default)
    local v = t[k]
    if v == nil then return default end
    if type(v) ~= "number" then error("WxlDraw: '" .. k .. "' must be a number", 3) end
    return v
end
local function point(p, what)
    if type(p) ~= "table" then error("WxlDraw: " .. what .. " must be a point {x, y, z}", 3) end
    local x, y, z = p.x or p[1], p.y or p[2], p.z or p[3] or 0
    if type(x) ~= "number" or type(y) ~= "number" or type(z) ~= "number" then
        error("WxlDraw: " .. what .. " must be a point {x, y, z} of numbers", 3)
    end
    return x, y, z
end
local function pair(t, k, what, default, number_ok)
    local v = t[k]
    if v == nil then return default, default end
    if number_ok and type(v) == "number" then return v, v end
    if type(v) ~= "table" then error("WxlDraw: '" .. k .. "' must be " .. what, 3) end
    local x, y = v.x or v[1], v.y or v[2]
    if type(x) ~= "number" or type(y) ~= "number" then error("WxlDraw: '" .. k .. "' must be " .. what, 3) end
    return x, y
end
function WxlDraw(shape, style)
    if type(shape) ~= "table" then return nil, "WxlDraw: the shape must be a table {kind = \"circle\", center = {x, y, z}, radius = 5, ...}" end
    local kind = kinds[shape.kind]
    if not kind then return nil, "WxlDraw: the shape's kind must be line, circle, ring, sector, rectangle, polygon, grid, box, sphere or cylinder" end
    local cx, cy, cz = 0, 0, 0
    if shape.center ~= nil then cx, cy, cz = point(shape.center, "the center") end
    local sx, sy, sz = 0, 0, 0
    if shape.size ~= nil then sx, sy, sz = point(shape.size, "the size") end
    sx, sy, sz = num(shape, "length", sx), num(shape, "width", sy), num(shape, "height", sz)
    local flags = (shape.flat and 1 or 0) + (shape.on_ground and 2 or 0) + (shape.closed and 4 or 0)
    local pts = ""
    if shape.points ~= nil then
        if type(shape.points) ~= "table" then return nil, "WxlDraw: the points must be a list of points {x, y, z}" end
        local alphas = shape.alphas
        if alphas ~= nil and (type(alphas) ~= "table" or #alphas ~= #shape.points) then
            return nil, "WxlDraw: the shape's alphas must be a list of opacities, one per point"
        end
        local list = {}
        for i, p in ipairs(shape.points) do
            local x, y, z = point(p, "point " .. i)
            if alphas then
                if type(alphas[i]) ~= "number" then return nil, "WxlDraw: alpha " .. i .. " must be a number" end
                list[#list + 1] = format("%.9g,%.9g,%.9g,%.9g", x, y, z, alphas[i])
            else
                list[#list + 1] = format("%.9g,%.9g,%.9g", x, y, z)
            end
        end
        pts = concat(list, ";")
    end
    style = style or {}
    if type(style) ~= "table" then return nil, "WxlDraw: the style must be a table {color = 0xRRGGBB, alpha, fill_alpha, width, depth, pulse}" end
    local depth = style.depth == "on_top" and 1 or 0
    local uv = style.uv == nil and 0 or uvs[style.uv]
    if uv == nil then return nil, "WxlDraw: the style's uv must be fit, tile or world" end
    local blend = style.blend == nil and 0 or blends[style.blend]
    if blend == nil then return nil, "WxlDraw: the style's blend must be alpha, add or modulate" end
    local texture, right, bottom = style.texture, num(style, "texture_right", 0), num(style, "texture_bottom", 0)
    if type(texture) == "table" then texture, right, bottom = texture[1], texture[2] or 0, texture[3] or 0 end
    if texture ~= nil and type(texture) ~= "string" then return nil, "WxlDraw: the style's texture must be a path, or {path, right, bottom}" end
    local shadow = num(style, "shadow_alpha", 0)
    local sxo, syo = pair(style, "shadow_offset", "{x, y}", shadow > 0 and 1.5 or 0)
    local usx, usy = pair(style, "uv_size", "{x, y} or a number", 0, true)
    local uox, uoy = pair(style, "uv_offset", "{x, y}", 0)
    local along = style.alpha_start ~= nil or style.alpha_end ~= nil
    return native(kind, cx, cy, cz, num(shape, "radius", 0), num(shape, "inner", 0), num(shape, "angle", 0), num(shape, "facing", 0),
                  num(shape, "pitch", 0), num(shape, "roll", 0), sx, sy, sz, num(shape, "spacing", 0), flags, pts,
                  num(style, "color", 0xFFFFFF), num(style, "alpha", 1), num(style, "fill_alpha", 0), num(style, "width", 2), depth,
                  num(style, "pulse", 0), num(style, "fill_color", -1), num(style, "softness", 0),
                  along and num(style, "alpha_start", 1) or -1, along and num(style, "alpha_end", 1) or -1,
                  num(style, "shadow_color", 0), shadow, sxo, syo, num(style, "shadow_softness", shadow > 0 and 1 or 0),
                  texture or "", right, bottom, uv, usx, usy, num(style, "uv_rotation", 0), uox, uoy, blend,
                  num(style, "feather", 0), num(style, "radial_fade", 0), num(style, "occluded_alpha", 0))
end
)lua";

    void WXL_CALL InterfaceDraw(wxl_script_call* call, void*)
    {
        const wxl_script_api& script = services::ScriptApi();
        constexpr uint32_t kArgs = 44, kPoints = 15, kTexture = 31;
        double             n[kArgs] = {};
        n[22] = n[24] = n[25] = -1.0; // fill_color, alpha_start, alpha_end: not given
        const uint32_t given = std::min(script.arg_count(call), kArgs);
        for (uint32_t i = 0; i < given; ++i)
        {
            double value = 0.0;
            if (i != kPoints && i != kTexture && script.arg_number(call, i, &value) == WXL_TRUE) n[i] = value;
        }
        wxl_str text{};
        script.arg_string(call, kPoints, &text);
        std::vector<wxl_vec3> points;
        std::vector<float>    alphas;
        const std::string     list(text.data ? text.data : "", text.size);
        for (size_t at = 0; at < list.size();)
        {
            const size_t end = std::min(list.find(';', at), list.size());
            wxl_vec3     p{};
            float        a = 1.0f;
            const int    read = std::sscanf(list.substr(at, end - at).c_str(), "%f,%f,%f,%f", &p.x, &p.y, &p.z, &a);
            if (read >= 3)
            {
                points.push_back(p);
                if (read == 4) alphas.push_back(a);
            }
            at = end + 1;
        }
        wxl_str texture{};
        if (given > kTexture) script.arg_string(call, kTexture, &texture);
        const std::string path(texture.data ? texture.data : "", texture.size);
        const auto f = [&](int i) { return static_cast<float>(n[i]); };
        wxl_draw_shape shape{};
        shape.struct_size = sizeof shape;
        shape.kind        = static_cast<uint32_t>(n[0]);
        shape.center      = wxl_vec3{f(1), f(2), f(3)};
        shape.radius      = f(4);
        shape.inner       = f(5);
        shape.angle       = f(6);
        shape.facing      = f(7);
        shape.pitch       = f(8);
        shape.roll        = f(9);
        shape.size        = wxl_vec3{f(10), f(11), f(12)};
        shape.spacing     = f(13);
        shape.flags       = static_cast<uint32_t>(n[14]);
        shape.points       = points.empty() ? nullptr : points.data();
        shape.point_count  = static_cast<uint32_t>(points.size());
        shape.point_alphas = !alphas.empty() && alphas.size() == points.size() ? alphas.data() : nullptr;
        wxl_draw_style style{};
        style.struct_size = sizeof style;
        style.color       = static_cast<uint32_t>(n[16]) & 0xFFFFFFu;
        style.alpha       = f(17);
        style.fill_alpha  = f(18);
        style.width       = f(19);
        style.depth       = static_cast<uint32_t>(n[20]);
        style.pulse       = f(21);
        if (n[22] >= 0.0)
        {
            style.flags |= WXL_DRAW_STYLE_FILL_COLOR;
            style.fill_color = static_cast<uint32_t>(n[22]) & 0xFFFFFFu;
        }
        style.softness = f(23);
        if (n[24] >= 0.0 || n[25] >= 0.0)
        {
            style.flags |= WXL_DRAW_STYLE_LINE_ALPHA;
            style.alpha_start = f(24);
            style.alpha_end   = f(25);
        }
        style.shadow_color    = static_cast<uint32_t>(n[26]) & 0xFFFFFFu;
        style.shadow_alpha    = f(27);
        style.shadow_x        = f(28);
        style.shadow_y        = f(29);
        style.shadow_softness = f(30);
        style.texture         = path.empty() ? nullptr : path.c_str();
        style.texture_right   = f(32);
        style.texture_bottom  = f(33);
        style.uv              = static_cast<uint32_t>(n[34]);
        style.uv_size_x       = f(35);
        style.uv_size_y       = f(36);
        style.uv_rotation     = f(37);
        style.uv_offset_x     = f(38);
        style.uv_offset_y     = f(39);
        style.blend           = static_cast<uint32_t>(n[40]);
        style.feather         = f(41);
        style.radial_fade     = f(42);
        style.occluded_alpha  = f(43);
        const wxl_result result = DrawNow(&shape, &style);
        if (result == WXL_OK)
        {
            script.push_bool(call, WXL_TRUE);
            return;
        }
        const char* why = result == WXL_E_UNSUPPORTED ? "unsupported" : "invalid (the log says why, under the tag draw)";
        script.push_nil(call);
        script.push_string(call, wxl_str{why, static_cast<uint32_t>(std::strlen(why))});
    }
}

namespace wxl::core::services
{
    const wxl_draw_api& DrawApi()
    {
        static const wxl_draw_api api = [] {
            wxl_draw_api table{};
            table.struct_size        = sizeof table;
            table.supported          = &Supported;
            table.unsupported_reason = &UnsupportedReason;
            table.draw               = &DrawNow;
            table.add                = &AddShape;
            table.update             = &Update;
            table.move               = &Move;
            table.set_style          = &SetStyle;
            table.set_duration       = &SetDuration;
            table.attach             = &Attach;
            table.detach             = &Detach;
            table.remove             = &Remove;
            table.clear              = &ClearMine;
            table.stats              = &Stats;
            table.emit               = &EmitNow;
            table.add_emitter        = &AddEmitter;
            table.update_emitter     = &UpdateEmitter;
            table.set_emitter_style  = &SetEmitterStyle;
            table.burst              = &Burst;
            table.place              = &Place;
            table.follow             = &Follow;
            table.stop               = &StopEmitter;
            return table;
        }();
        return api;
    }

    namespace draw
    {
        void Render()
        {
            g_worldThisFrame = true;
            const auto   started = Clock::now();
            const double now     = Now();
            // The particles move on by the time since the last world drawn (at most a tenth of a
            // second: a frame held longer does not throw them far).
            const float dt   = g_lastStep > 0.0 ? static_cast<float>(std::clamp(now - g_lastStep, 0.0, 0.1)) : 0.0f;
            g_lastStep       = now;
            g_emittedFrame   = 0;
            StepParticles(now, dt);
            const float stepped = std::chrono::duration<float, std::micro>(Clock::now() - started).count();
            if (g_kept.empty() && !g_immediateCount && g_emitters.empty())
            {
                g_stats.particles             = g_particlesAlive;
                g_stats.emitters              = 0;
                g_stats.particles_emitted     = g_emittedFrame;
                g_stats.particle_draws        = 0;
                g_stats.particle_microseconds = stepped;
                g_stats.particles_culled      = g_culled;
                g_culled                      = 0;
                return;
            }

            // Kept shapes: those whose time is up go, those whose object left view go, those that
            // moved or whose ground changed are built again.
            uint32_t rebuilt = 0;
            const uint64_t generation = GenerationOf();
            for (size_t i = g_kept.size(); i-- > 0;)
            {
                Kept& k = *g_kept[i];
                if (k.ends > 0.0 && now >= k.ends)
                {
                    Drop(&k);
                    continue;
                }
                if (k.target)
                {
                    void*    object = objects::Service().Handles().Resolve(k.target);
                    wxl_vec3 at{};
                    float    facing = 0.0f;
                    if (!object || binding::object_position(object, &at) != WXL_TRUE)
                    {
                        Drop(&k); // it left view
                        continue;
                    }
                    binding::object_facing(object, &facing);
                    const sh::V3 p = V(at);
                    const bool moved = std::fabs(p.x - k.placed.x) > 1e-3f || std::fabs(p.y - k.placed.y) > 1e-3f ||
                                       std::fabs(p.z - k.placed.z) > 1e-3f ||
                                       ((k.attach & WXL_DRAW_ATTACH_TURN) && std::fabs(facing - k.turned) > 1e-4f);
                    if (moved || !k.built || (k.mesh.ground && k.generation != generation))
                    {
                        k.placed = p;
                        k.turned = facing;
                        BuildKept(k, Placed(k, p, facing));
                        ++rebuilt;
                    }
                }
                else if (!k.built || (k.mesh.ground && k.generation != generation))
                {
                    BuildKept(k, k.desc);
                    ++rebuilt;
                }
            }

            const char* refusal = Refusal();
            sh::View    view;
            const bool  seen = !refusal && ReadView(view);
            ++g_frame;
            wxl::vk::render::Render*  vulkan = seen ? VulkanRender() : nullptr;
            wxl::vk::gxfront::Front*  front  = vulkan && !g_testRender ? gx::VulkanFront() : nullptr;

            // A texture as the device draws it this frame, readied once a frame; null while it loads.
            const auto deviceTexture = [&](const std::string& key) -> void* {
                const auto at = g_textures.find(key);
                if (at == g_textures.end()) return nullptr;
                Texture& t = at->second;
                if (t.frame != g_frame)
                {
                    t.frame  = g_frame;
                    t.device = nullptr;
                    if (void* gx = binding::gx_texture_ready(t.handle))
                    {
                        if (vulkan) t.device = front ? static_cast<void*>(front->TextureOf(gx)) : nullptr;
                        else t.device = static_cast<wxl::engine::CGxTex*>(gx)->m_apiSpecificData;
                    }
                }
                return t.device;
            };

            // The frame's vertices, kept first (oldest first), then the immediate ones: past the
            // budget the newest go first.
            for (Bucket& bucket : g_buckets) bucket.Clear();
            g_occludedCount = 0;
            g_surfaces.clear();
            struct Volume
            {
                const sh::Mesh* mesh;
                const Style*    style;
                float           beat;
                float           distance; ///< to draw them far to near
                Bucket*         bucket;
            };
            std::vector<Volume> volumes;
            uint32_t            total = 0, drawn = 0, leftOut = 0, textured = 0, shadowed = 0, occluded = 0;

            // A surface's run in @p runs over the vertices @p into gets from here on, its numbers kept.
            const auto surfaceRun = [&](std::vector<sh::Run>& runs, const std::vector<sh::Vertex>& into, const Style& style, bool top,
                                        const sh::SurfaceConstants& constants, void* texture) {
                sh::Surface surface;
                surface.constants = constants;
                surface.texture   = texture;
                surface.repeats   = sh::Repeats(style.look.uv) && style.look.right >= 1.0f && style.look.bottom >= 1.0f;
                sh::Run run;
                run.first   = static_cast<uint32_t>(into.size());
                run.on_top  = top;
                run.program = sh::kSurface;
                run.blend   = static_cast<uint8_t>(style.look.blend);
                run.surface = static_cast<uint32_t>(g_surfaces.size());
                g_surfaces.push_back(surface);
                runs.push_back(run);
            };
            const auto take = [&](const sh::Mesh& mesh, const Style& style, float beat) {
                if (mesh.beam.on)
                {
                    // A beam: its quad turned towards the eye this frame; a surface when it is soft
                    // across, textured or blended, else a plain inside.
                    if (!(style.fill > 0.0f)) return;
                    const size_t n = sh::kBeamSegments * 6;
                    if (total + n > WXL_DRAW_MAX_FRAME_VERTICES)
                    {
                        ++leftOut;
                        return;
                    }
                    total += static_cast<uint32_t>(n);
                    ++drawn;
                    const bool top    = style.depth == WXL_DRAW_DEPTH_ON_TOP;
                    Bucket*    bucket = &g_buckets[top ? 1 : 0];
                    if (style.occluded > 0.0f && !top)
                    {
                        if (g_occludedCount == g_occluded.size()) g_occluded.emplace_back();
                        bucket = &g_occluded[g_occludedCount++];
                        bucket->Clear();
                        bucket->occluded = style.occluded;
                        ++occluded;
                    }
                    const uint32_t fc = Color(style.fill_color, style.fill * beat);
                    if (!style.Surface())
                    {
                        sh::WriteBeam(mesh, view.eye, fc, false, bucket->fill);
                        return;
                    }
                    void* texture = nullptr;
                    if (style.look.textured && !(texture = deviceTexture(style.texture))) return;
                    surfaceRun(bucket->surfaceRuns, bucket->surfaces, style, top, sh::MakeBeamSurface(mesh, style.look), texture);
                    sh::WriteBeam(mesh, view.eye, fc, false, bucket->surfaces);
                    bucket->surfaceRuns.back().count = static_cast<uint32_t>(bucket->surfaces.size()) - bucket->surfaceRuns.back().first;
                    ++textured;
                    return;
                }
                if (!mesh.lines.size() && !mesh.triangles.size()) return;
                const bool   shadow = style.shadow_alpha > 0.0f && !mesh.lines.empty();
                const bool   xray   = style.occluded > 0.0f && style.depth == WXL_DRAW_DEPTH_TESTED;
                const size_t n      = mesh.Vertices() + (shadow ? mesh.lines.size() * 6 : 0);
                if (total + n > WXL_DRAW_MAX_FRAME_VERTICES)
                {
                    ++leftOut;
                    return;
                }
                total += static_cast<uint32_t>(n);
                ++drawn;
                const bool top = style.depth == WXL_DRAW_DEPTH_ON_TOP;
                Bucket*    bucket = &g_buckets[top ? 1 : 0];
                if (xray)
                {
                    // A bucket of its own, drawn twice: where the scene hides it, then where it is seen.
                    if (g_occludedCount == g_occluded.size()) g_occluded.emplace_back();
                    bucket = &g_occluded[g_occludedCount++];
                    bucket->Clear();
                    bucket->occluded = style.occluded;
                    ++occluded;
                }
                if (!mesh.lines.empty())
                {
                    sh::LineLook look;
                    look.color       = Color(style.color, style.alpha * beat);
                    look.width       = sh::WidthCode(style.width);
                    look.soft        = sh::SoftCode(style.softness);
                    look.alpha_start = style.alpha_start;
                    look.alpha_end   = style.alpha_end;
                    for (const sh::Mesh::Segment& seg : mesh.lines) sh::PushSegment(bucket->lines, seg, view.eye, look, mesh.ground);
                    if (shadow)
                    {
                        // The same segments once more, under every line: wider by the softness,
                        // fading over it, moved on the screen.
                        look.color = Color(style.shadow_color, style.shadow_alpha * beat);
                        look.width = sh::WidthCode(style.width + style.shadow_softness);
                        look.soft  = sh::SoftCode(style.shadow_softness);
                        look.right = sh::OffsetCode(style.shadow_x);
                        look.down  = sh::OffsetCode(style.shadow_y);
                        for (const sh::Mesh::Segment& seg : mesh.lines) sh::PushSegment(bucket->shadows, seg, view.eye, look, mesh.ground);
                        ++shadowed;
                    }
                }
                if (mesh.triangles.empty()) return;
                if (!mesh.normals.empty())
                {
                    const float dx = mesh.middle.x - view.eye.x, dy = mesh.middle.y - view.eye.y, dz = mesh.middle.z - view.eye.z;
                    volumes.push_back(Volume{&mesh, &style, beat, dx * dx + dy * dy + dz * dz, bucket});
                    return;
                }
                const uint32_t fc = Color(style.fill_color, style.fill * beat);
                if (!style.Surface())
                {
                    for (size_t t = 0; t + 2 < mesh.triangles.size(); t += 3)
                        sh::PushTriangle(bucket->fill, mesh.triangles[t], mesh.triangles[t + 1], mesh.triangles[t + 2], view.eye, fc, mesh.ground);
                    return;
                }
                // A surface: its run and numbers; a texture still loading leaves the inside out.
                void* texture = nullptr;
                if (style.look.textured && !(texture = deviceTexture(style.texture))) return;
                surfaceRun(bucket->surfaceRuns, bucket->surfaces, style, top, sh::MakeSurface(mesh, style.look), texture);
                const bool edged = mesh.edge.size() == mesh.triangles.size();
                for (size_t t = 0; t + 2 < mesh.triangles.size(); t += 3)
                {
                    sh::V3 plane[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        plane[k]   = sh::InPlane(mesh, mesh.triangles[t + k]);
                        plane[k].z = edged ? mesh.edge[t + k] : 0.0f;
                    }
                    sh::PushSurfaceTriangle(bucket->surfaces, &mesh.triangles[t], plane, view.eye, fc, mesh.ground);
                }
                bucket->surfaceRuns.back().count = static_cast<uint32_t>(bucket->surfaces.size()) - bucket->surfaceRuns.back().first;
                ++textured;
            };
            if (seen)
            {
                for (const auto& k : g_kept)
                    if (k->built) take(k->mesh, k->style, Beat(k->style, now, k->born, k->ends));
                for (size_t i = 0; i < g_immediateCount; ++i) take(g_immediate[i].mesh, g_immediate[i].style, Beat(g_immediate[i].style, now, 0.0, 0.0));
                // Volumes far to near, their faces turned to the eye (from inside one, its inside);
                // a textured box's or cylinder's faces each a surface in its own frame.
                std::stable_sort(volumes.begin(), volumes.end(), [](const Volume& a, const Volume& b) { return a.distance > b.distance; });
                for (const Volume& v : volumes)
                {
                    const sh::Mesh& m      = *v.mesh;
                    const Style&    style  = *v.style;
                    Bucket&         bucket = *v.bucket;
                    const bool      top    = style.depth == WXL_DRAW_DEPTH_ON_TOP;
                    const uint32_t  fc     = Color(style.fill_color, style.fill * v.beat);
                    auto facing = [&](size_t t) {
                        const sh::V3& a = m.triangles[t];
                        const sh::V3& n = m.normals[t / 3];
                        return n.x * (a.x - view.eye.x) + n.y * (a.y - view.eye.y) + n.z * (a.z - view.eye.z) < 0.0f;
                    };
                    bool outside = false;
                    for (size_t t = 0; t + 2 < m.triangles.size() && !outside; t += 3) outside = facing(t);
                    const bool faced = style.Surface() && !m.faces.empty() && m.face_plane.size() == m.triangles.size();
                    void*      texture = nullptr;
                    if (faced && style.look.textured && !(texture = deviceTexture(style.texture))) continue; // loading
                    if (!faced)
                    {
                        sh::Run run;
                        run.first  = static_cast<uint32_t>(bucket.volumes.size());
                        run.on_top = top;
                        for (size_t t = 0; t + 2 < m.triangles.size(); t += 3)
                            if (facing(t) == outside)
                                sh::PushTriangle(bucket.volumes, m.triangles[t], m.triangles[t + 1], m.triangles[t + 2], view.eye, fc, false);
                        run.count = static_cast<uint32_t>(bucket.volumes.size()) - run.first;
                        if (run.count) bucket.volumeRuns.push_back(run);
                        continue;
                    }
                    for (const sh::Mesh::Face& face : m.faces)
                    {
                        if (!(face.count >= 3 && facing(face.first) == outside)) continue;
                        surfaceRun(bucket.volumeRuns, bucket.volumes, style, top, sh::MakeFaceSurface(face, style.look), texture);
                        for (uint32_t t = face.first; t + 2 < face.first + face.count; t += 3)
                            if (facing(t) == outside)
                                sh::PushSurfaceTriangle(bucket.volumes, &m.triangles[t], &m.face_plane[t], view.eye, fc, false);
                        bucket.volumeRuns.back().count = static_cast<uint32_t>(bucket.volumes.size()) - bucket.volumeRuns.back().first;
                    }
                    ++textured;
                }
            }
            // Particles: an emitter's in one run, sorted far to near when blended by their opacity;
            // emitters far to near too. A texture still loading leaves them out of the frame.
            const auto particlesFrom = Clock::now();
            if (seen && !g_emitters.empty())
            {
                std::vector<std::pair<float, Emitter*>> order;
                order.reserve(g_emitters.size());
                for (const auto& e : g_emitters)
                    if (e->pool.Size())
                    {
                        const float dx = e->at.x - view.eye.x, dy = e->at.y - view.eye.y, dz = e->at.z - view.eye.z;
                        order.push_back({dx * dx + dy * dy + dz * dz, e.get()});
                    }
                std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                for (const auto& [distance, e] : order)
                {
                    void* texture = nullptr;
                    if (e->look.textured && !(texture = deviceTexture(e->texture))) continue;
                    const bool top    = e->depth == WXL_DRAW_DEPTH_ON_TOP;
                    Bucket*    bucket = &g_buckets[top ? 1 : 0];
                    if (e->occluded > 0.0f && !top)
                    {
                        if (g_occludedCount == g_occluded.size()) g_occluded.emplace_back();
                        bucket = &g_occluded[g_occludedCount++];
                        bucket->Clear();
                        bucket->occluded = e->occluded;
                        ++occluded;
                    }
                    sh::Surface surface;
                    surface.constants = sh::ParticleConstants(texture != nullptr, e->blend == WXL_DRAW_BLEND_MODULATE, e->look.mask, e->look.blur);
                    surface.texture   = texture;
                    sh::Run run;
                    run.first   = static_cast<uint32_t>(bucket->particles.size());
                    run.on_top  = top;
                    run.program = sh::kParticle;
                    run.blend   = static_cast<uint8_t>(e->blend);
                    run.surface = static_cast<uint32_t>(g_surfaces.size());
                    e->pool.Write(bucket->particles, e->look, g_camera, view.eye, e->blend == WXL_DRAW_BLEND_ALPHA);
                    run.count = static_cast<uint32_t>(bucket->particles.size()) - run.first;
                    g_surfaces.push_back(surface);
                    bucket->particleRuns.push_back(run);
                }
            }
            const float particlesWritten = std::chrono::duration<float, std::micro>(Clock::now() - particlesFrom).count();

            const uint32_t immediate = static_cast<uint32_t>(g_immediateCount);
            for (size_t i = 0; i < g_immediateCount; ++i) g_immediate[i].Done();
            g_immediateCount = 0;
            g_immediateOf.clear();
            const uint32_t reused = g_reused;
            g_reused              = 0;

            uint32_t sent = 0;
            if (seen)
            {
                // Per class: plain insides; each surface; volumes (plain runs and faces), the lines'
                // shadows and the lines. An occluded shape's runs go twice: behind the scene at its
                // occluded opacity, then as usual.
                g_stream.clear();
                g_runs.clear();
                const auto add = [&](sh::Run run) {
                    if (!run.count) return;
                    const sh::Run* last = g_runs.empty() ? nullptr : &g_runs.back();
                    if (last && run.program == sh::kPlain && last->program == sh::kPlain && last->on_top == run.on_top &&
                        last->behind == run.behind && last->alpha == run.alpha && last->first + last->count == run.first)
                        g_runs.back().count += run.count;
                    else
                        g_runs.push_back(run);
                };
                // The bucket's vertices into the stream; its runs, in order, into @p out.
                std::vector<sh::Run> runs;
                const auto lay = [&](const Bucket& b, bool top) {
                    runs.clear();
                    const auto plain = [&](const std::vector<sh::Vertex>& part) {
                        sh::Run run;
                        run.first  = static_cast<uint32_t>(g_stream.size());
                        run.on_top = top;
                        run.count  = static_cast<uint32_t>(part.size());
                        g_stream.insert(g_stream.end(), part.begin(), part.end());
                        if (run.count) runs.push_back(run);
                    };
                    const auto withRuns = [&](const std::vector<sh::Vertex>& part, const std::vector<sh::Run>& own) {
                        const uint32_t base = static_cast<uint32_t>(g_stream.size());
                        g_stream.insert(g_stream.end(), part.begin(), part.end());
                        for (sh::Run run : own)
                        {
                            run.first += base;
                            if (run.count) runs.push_back(run);
                        }
                    };
                    plain(b.fill);
                    withRuns(b.surfaces, b.surfaceRuns);
                    withRuns(b.volumes, b.volumeRuns);
                    plain(b.shadows);
                    plain(b.lines);
                    withRuns(b.particles, b.particleRuns);
                };
                lay(g_buckets[0], false);
                for (const sh::Run& run : runs) add(run);
                for (size_t i = 0; i < g_occludedCount; ++i)
                {
                    const Bucket& b = g_occluded[i];
                    lay(b, false);
                    for (sh::Run run : runs)
                    {
                        run.behind = true;
                        run.alpha  = b.occluded;
                        add(run);
                    }
                    for (const sh::Run& run : runs) add(run);
                }
                lay(g_buckets[1], true);
                for (const sh::Run& run : runs) add(run);
                float constants[24];
                sh::Constants(view, constants);
                std::string why;
                if (vulkan)
                {
                    if (g_attached != vulkan)
                    {
                        g_vulkan.Forget();
                        g_vulkan.Attach(*vulkan);
                        g_attached = vulkan;
                    }
                    sent = g_vulkan.Draw(g_stream.data(), g_runs.data(), static_cast<uint32_t>(g_runs.size()), g_surfaces.data(), constants,
                                         view.viewport, &why);
                }
                else
                    sent = gx::d3d9::shapes::Draw(g_stream.data(), g_runs.data(), static_cast<uint32_t>(g_runs.size()), g_surfaces.data(),
                                                  constants, view.viewport, &why);
                if (!sent && !g_stream.empty()) Failed(why);
            }
            else if (refusal)
                Failed(refusal);

            const float spent = std::chrono::duration<float, std::micro>(Clock::now() - started).count();
            g_stats.immediate              = immediate;
            g_stats.drawn                  = drawn;
            g_stats.vertices               = sent;
            g_stats.left_out               = leftOut;
            g_stats.rebuilt                = rebuilt;
            g_stats.build_microseconds     = spent;
            g_stats.max_build_microseconds = std::max(g_stats.max_build_microseconds, spent);
            g_stats.immediate_microseconds = static_cast<float>(g_immediateSeconds * 1e6);
            g_stats.textured               = textured;
            g_stats.shadows                = shadowed;
            g_stats.occluded               = occluded;
            g_stats.reused                 = reused;
            uint32_t particleDraws = 0;
            for (const sh::Run& run : g_runs) particleDraws += seen && run.program == sh::kParticle ? 1u : 0u;
            g_stats.particles             = g_particlesAlive;
            g_stats.particles_emitted     = g_emittedFrame;
            g_stats.particle_draws        = particleDraws;
            g_stats.emitters              = static_cast<uint32_t>(g_emitters.size());
            g_stats.particle_microseconds = stepped + particlesWritten;
            g_stats.particles_culled      = g_culled;
            g_culled                      = 0;
            ++g_stats.frames;
            g_immediateSeconds = 0.0;
            g_leftOutSince += leftOut;
        }

        void Present()
        {
            if (!g_worldThisFrame)
            {
                // No world this frame (a loading screen): what was drawn for it goes.
                for (size_t i = 0; i < g_immediateCount; ++i) g_immediate[i].Done();
                g_immediateCount = 0;
                g_immediateOf.clear();
            }
            g_worldThisFrame = false;
            // Textures no shape has used for a while are let go of.
            if (!g_textures.empty())
            {
                const double at = Now();
                for (auto it = g_textures.begin(); it != g_textures.end();)
                {
                    if (!it->second.refs && at - it->second.unused_since > kTextureLinger)
                    {
                        binding::texture_close(it->second.handle);
                        it = g_textures.erase(it);
                    }
                    else
                        ++it;
                }
            }
            if (!g_leftOutSince) return;
            const double now = Now();
            if (now - g_leftOutSaidAt < 1.0) return;
            WLOG_WARN(kTag, "wxl.draw: %u shape(s) left out of the frames of the last second: past the %u vertices a frame draws",
                      g_leftOutSince, WXL_DRAW_MAX_FRAME_VERTICES);
            g_leftOutSaidAt = now;
            g_leftOutSince  = 0;
        }

        void DeviceGone()
        {
            g_vulkan.Forget();
            g_attached = nullptr;
        }

        void ForgetMod(uint32_t mod)
        {
            for (size_t i = g_emitters.size(); i-- > 0;)
                if (g_emitters[i]->mod == mod) DropEmitter(i);
            for (size_t i = g_kept.size(); i-- > 0;)
                if (g_kept[i]->mod == mod) Drop(g_kept[i].get());
            size_t live = 0;
            for (size_t i = 0; i < g_immediateCount; ++i)
            {
                if (g_immediate[i].mod == mod)
                {
                    g_immediate[i].Done();
                    continue;
                }
                if (live != i) std::swap(g_immediate[live], g_immediate[i]);
                ++live;
            }
            g_immediateCount = live;
            g_immediateOf.erase(mod);
        }

        void Clear()
        {
            g_emitters.clear();
            g_emitterByHandle.clear();
            g_particlesAlive = 0;
            g_particlesOf.clear();
            g_culled         = 0;
            g_lastStep       = 0.0;
            g_kept.clear();
            g_byHandle.clear();
            for (size_t i = 0; i < g_immediateCount; ++i) g_immediate[i].Done();
            g_immediate.clear();
            for (auto& [key, t] : g_textures) binding::texture_close(t.handle);
            g_textures.clear();
            g_immediateCount = 0;
            g_immediateOf.clear();
            g_said.clear();
            g_failures.clear();
            g_stats = wxl_draw_stats{};
            g_terrain.Forget();
            g_vulkan.Detach();
            g_attached = nullptr;
            gx::d3d9::shapes::Shutdown();
        }

        void OnScriptReady()
        {
            const wxl_script_api& script = ScriptApi();
            if (script.register_function(kNativeName, &InterfaceDraw, nullptr) != WXL_OK)
            {
                WLOG_WARN(kTag, "WxlDraw could not be put in the interface: %s is taken", kNativeName);
                return;
            }
            script.run(wxl_str{kWrapper, static_cast<uint32_t>(sizeof kWrapper - 1)}, "WarcraftXL\\WxlDraw");
        }

        void UseGround(wxl::gx::shapes::Ground* ground) { g_testGround = ground; }

        void UseVulkan(wxl::vk::render::Render* render)
        {
            g_testRender = render;
            g_vulkan.Forget();
            g_attached = nullptr;
        }
    }
}
