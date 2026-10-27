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
    std::vector<sh::Vertex>  g_fill[2], g_surfaceVertices[2], g_volumes[2], g_shadows[2], g_lines[2], g_stream;
    std::vector<sh::Run>     g_surfaceRuns[2], g_runs;
    std::vector<sh::Surface> g_surfaces;

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
        if (in->struct_size < sizeof(wxl_draw_style)) return true; // abi 0.1.36's look

        const auto refuse = [&](const char* why) {
            Refuse(call, std::string("the style's ") + why);
            return false;
        };
        const auto number = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
        if (in->flags & ~(WXL_DRAW_STYLE_FILL_COLOR | WXL_DRAW_STYLE_LINE_ALPHA))
            return refuse("flags take WXL_DRAW_STYLE_FILL_COLOR (1) and WXL_DRAW_STYLE_LINE_ALPHA (2) alone");
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
        Immediate&  slot = g_immediate[g_immediateCount];
        std::string why;
        if (!sh::Build(desc, look.Parts(), &GroundOf(), slot.mesh, &why))
        {
            LetGoTexture(look.texture);
            Refuse("draw", "the shape: " + why);
            return WXL_E_INVALID;
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

    struct Job
    {
        const sh::Mesh* mesh;
        const Style*    style;
        float           beat;
        float           distance; ///< of a volume, to draw them far to near
    };

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
local kinds = { line = 0, circle = 1, ring = 2, sector = 3, rectangle = 4, polygon = 5, grid = 6, box = 7, sphere = 8, cylinder = 9 }
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
                  num(style, "feather", 0), num(style, "radial_fade", 0))
end
)lua";

    void WXL_CALL InterfaceDraw(wxl_script_call* call, void*)
    {
        const wxl_script_api& script = services::ScriptApi();
        constexpr uint32_t kArgs = 43, kPoints = 15, kTexture = 31;
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
            return table;
        }();
        return api;
    }

    namespace draw
    {
        void Render()
        {
            g_worldThisFrame = true;
            if (g_kept.empty() && !g_immediateCount) return;
            const auto started = Clock::now();
            const double now   = Now();

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
            for (int top = 0; top < 2; ++top)
            {
                g_fill[top].clear();
                g_surfaceVertices[top].clear();
                g_surfaceRuns[top].clear();
                g_volumes[top].clear();
                g_shadows[top].clear();
                g_lines[top].clear();
            }
            g_surfaces.clear();
            std::vector<Job> volumes[2];
            uint32_t         total = 0, drawn = 0, leftOut = 0, textured = 0, shadowed = 0;
            const auto take = [&](const sh::Mesh& mesh, const Style& style, float beat) {
                if (!mesh.lines.size() && !mesh.triangles.size()) return;
                const bool   shadow = style.shadow_alpha > 0.0f && !mesh.lines.empty();
                const size_t n      = mesh.Vertices() + (shadow ? mesh.lines.size() * 6 : 0);
                if (total + n > WXL_DRAW_MAX_FRAME_VERTICES)
                {
                    ++leftOut;
                    return;
                }
                total += static_cast<uint32_t>(n);
                ++drawn;
                const int top = style.depth == WXL_DRAW_DEPTH_ON_TOP ? 1 : 0;
                if (!mesh.lines.empty())
                {
                    sh::LineLook look;
                    look.color       = Color(style.color, style.alpha * beat);
                    look.width       = sh::WidthCode(style.width);
                    look.soft        = sh::SoftCode(style.softness);
                    look.alpha_start = style.alpha_start;
                    look.alpha_end   = style.alpha_end;
                    for (const sh::Mesh::Segment& seg : mesh.lines) sh::PushSegment(g_lines[top], seg, view.eye, look, mesh.ground);
                    if (shadow)
                    {
                        // The same segments once more, under every line: wider by the softness,
                        // fading over it, moved on the screen.
                        look.color = Color(style.shadow_color, style.shadow_alpha * beat);
                        look.width = sh::WidthCode(style.width + style.shadow_softness);
                        look.soft  = sh::SoftCode(style.shadow_softness);
                        look.right = sh::OffsetCode(style.shadow_x);
                        look.down  = sh::OffsetCode(style.shadow_y);
                        for (const sh::Mesh::Segment& seg : mesh.lines) sh::PushSegment(g_shadows[top], seg, view.eye, look, mesh.ground);
                        ++shadowed;
                    }
                }
                if (mesh.triangles.empty()) return;
                if (!mesh.normals.empty())
                {
                    const float dx = mesh.middle.x - view.eye.x, dy = mesh.middle.y - view.eye.y, dz = mesh.middle.z - view.eye.z;
                    volumes[top].push_back(Job{&mesh, &style, beat, dx * dx + dy * dy + dz * dz});
                    return;
                }
                const uint32_t fc = Color(style.fill_color, style.fill * beat);
                if (!style.Surface())
                {
                    for (size_t t = 0; t + 2 < mesh.triangles.size(); t += 3)
                        sh::PushTriangle(g_fill[top], mesh.triangles[t], mesh.triangles[t + 1], mesh.triangles[t + 2], view.eye, fc, mesh.ground);
                    return;
                }
                // A surface: its run and numbers; a texture still loading leaves the inside out.
                sh::Surface surface;
                if (style.look.textured && !(surface.texture = deviceTexture(style.texture))) return;
                surface.constants = sh::MakeSurface(mesh, style.look);
                surface.repeats   = sh::Repeats(style.look.uv) && style.look.right >= 1.0f && style.look.bottom >= 1.0f;
                sh::Run run;
                run.first   = static_cast<uint32_t>(g_surfaceVertices[top].size());
                run.on_top  = top == 1;
                run.program = sh::kSurface;
                run.blend   = static_cast<uint8_t>(style.look.blend);
                run.surface = static_cast<uint32_t>(g_surfaces.size());
                const bool edged = mesh.edge.size() == mesh.triangles.size();
                for (size_t t = 0; t + 2 < mesh.triangles.size(); t += 3)
                {
                    sh::V3 plane[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        plane[k]   = sh::InPlane(mesh, mesh.triangles[t + k]);
                        plane[k].z = edged ? mesh.edge[t + k] : 0.0f;
                    }
                    sh::PushSurfaceTriangle(g_surfaceVertices[top], &mesh.triangles[t], plane, view.eye, fc, mesh.ground);
                }
                run.count = static_cast<uint32_t>(g_surfaceVertices[top].size()) - run.first;
                g_surfaces.push_back(surface);
                g_surfaceRuns[top].push_back(run);
                ++textured;
            };
            if (seen)
            {
                for (const auto& k : g_kept)
                    if (k->built) take(k->mesh, k->style, Beat(k->style, now, k->born, k->ends));
                for (size_t i = 0; i < g_immediateCount; ++i) take(g_immediate[i].mesh, g_immediate[i].style, Beat(g_immediate[i].style, now, 0.0, 0.0));
                // Volumes' fills far to near, their front faces only.
                for (int top = 0; top < 2; ++top)
                {
                    std::sort(volumes[top].begin(), volumes[top].end(), [](const Job& a, const Job& b) { return a.distance > b.distance; });
                    std::vector<sh::Vertex>& fill = g_volumes[top];
                    for (const Job& job : volumes[top])
                    {
                        const uint32_t fc = Color(job.style->fill_color, job.style->fill * job.beat);
                        const sh::Mesh& m = *job.mesh;
                        // The faces turned to the eye; from inside the volume (none is), its inside.
                        auto facing = [&](size_t t, size_t f) {
                            const sh::V3& a = m.triangles[t];
                            const sh::V3& n = m.normals[f];
                            return n.x * (a.x - view.eye.x) + n.y * (a.y - view.eye.y) + n.z * (a.z - view.eye.z) < 0.0f;
                        };
                        bool outside = false;
                        for (size_t t = 0, f = 0; t + 2 < m.triangles.size() && !outside; t += 3, ++f) outside = facing(t, f);
                        for (size_t t = 0, f = 0; t + 2 < m.triangles.size(); t += 3, ++f)
                            if (facing(t, f) == outside)
                                sh::PushTriangle(fill, m.triangles[t], m.triangles[t + 1], m.triangles[t + 2], view.eye, fc, false);
                    }
                }
            }
            const uint32_t immediate = static_cast<uint32_t>(g_immediateCount);
            for (size_t i = 0; i < g_immediateCount; ++i) g_immediate[i].Done();
            g_immediateCount = 0;
            g_immediateOf.clear();

            uint32_t sent = 0;
            if (seen)
            {
                // Per depth: plain insides; each surface; volumes, the lines' shadows and the lines.
                g_stream.clear();
                g_runs.clear();
                const auto plain = [&](bool top, std::initializer_list<const std::vector<sh::Vertex>*> parts) {
                    sh::Run run;
                    run.first  = static_cast<uint32_t>(g_stream.size());
                    run.on_top = top;
                    for (const auto* part : parts) g_stream.insert(g_stream.end(), part->begin(), part->end());
                    run.count = static_cast<uint32_t>(g_stream.size()) - run.first;
                    if (!run.count) return;
                    // Right after a plain run of the same depth (no surface between): one run.
                    if (!g_runs.empty() && g_runs.back().program == sh::kPlain && g_runs.back().on_top == top &&
                        g_runs.back().first + g_runs.back().count == run.first)
                        g_runs.back().count += run.count;
                    else
                        g_runs.push_back(run);
                };
                for (int top = 0; top < 2; ++top)
                {
                    plain(top == 1, {&g_fill[top]});
                    const uint32_t base = static_cast<uint32_t>(g_stream.size());
                    g_stream.insert(g_stream.end(), g_surfaceVertices[top].begin(), g_surfaceVertices[top].end());
                    for (sh::Run run : g_surfaceRuns[top])
                    {
                        run.first += base;
                        g_runs.push_back(run);
                    }
                    plain(top == 1, {&g_volumes[top], &g_shadows[top], &g_lines[top]});
                }
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
