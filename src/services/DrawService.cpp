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
// the moment) and draws them in two runs, tested against the depth then on top; fills before lines,
// volumes far to near, a volume's back faces left out. Refusals are logged once per mod and call
// under `draw`, shapes left out past the frame's budget once a second.

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

#include "wxl/host/mod_host.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
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

        sh::Parts Parts() const { return sh::Parts{alpha > 0.0f, fill > 0.0f}; }
    };

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
    };

    struct Immediate
    {
        uint32_t mod = 0;
        Style    style;
        sh::Mesh mesh;
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
    std::vector<sh::Vertex> g_testedFill, g_testedLines, g_topFill, g_topLines, g_stream;

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
        if (in->struct_size < sizeof(wxl_draw_shape))
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
        if (in->struct_size < sizeof(wxl_draw_style))
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
            Refuse("draw", "the shape: " + why);
            return WXL_E_INVALID;
        }
        slot.mod   = mod;
        slot.style = look;
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
        k->style = look;
        if (before.lines != after.lines || before.fill != after.fill) k->built = false;
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
        local list = {}
        for i, p in ipairs(shape.points) do
            local x, y, z = point(p, "point " .. i)
            list[#list + 1] = format("%.9g,%.9g,%.9g", x, y, z)
        end
        pts = concat(list, ";")
    end
    style = style or {}
    if type(style) ~= "table" then return nil, "WxlDraw: the style must be a table {color = 0xRRGGBB, alpha, fill_alpha, width, depth, pulse}" end
    local depth = style.depth == "on_top" and 1 or 0
    return native(kind, cx, cy, cz, num(shape, "radius", 0), num(shape, "inner", 0), num(shape, "angle", 0), num(shape, "facing", 0),
                  num(shape, "pitch", 0), num(shape, "roll", 0), sx, sy, sz, num(shape, "spacing", 0), flags, pts,
                  num(style, "color", 0xFFFFFF), num(style, "alpha", 1), num(style, "fill_alpha", 0), num(style, "width", 2), depth,
                  num(style, "pulse", 0))
end
)lua";

    void WXL_CALL InterfaceDraw(wxl_script_call* call, void*)
    {
        const wxl_script_api& script = services::ScriptApi();
        double                n[22]  = {};
        for (uint32_t i = 0; i < 22; ++i)
            if (i != 15) script.arg_number(call, i, &n[i]);
        wxl_str text{};
        script.arg_string(call, 15, &text);
        std::vector<wxl_vec3> points;
        const std::string     list(text.data ? text.data : "", text.size);
        for (size_t at = 0; at < list.size();)
        {
            const size_t end = std::min(list.find(';', at), list.size());
            wxl_vec3     p{};
            if (std::sscanf(list.substr(at, end - at).c_str(), "%f,%f,%f", &p.x, &p.y, &p.z) == 3) points.push_back(p);
            at = end + 1;
        }
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
        shape.points      = points.empty() ? nullptr : points.data();
        shape.point_count = static_cast<uint32_t>(points.size());
        wxl_draw_style style{};
        style.struct_size = sizeof style;
        style.color       = static_cast<uint32_t>(n[16]) & 0xFFFFFFu;
        style.alpha       = f(17);
        style.fill_alpha  = f(18);
        style.width       = f(19);
        style.depth       = static_cast<uint32_t>(n[20]);
        style.pulse       = f(21);
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

            // The frame's vertices, kept first (oldest first), then the immediate ones: past the
            // budget the newest go first.
            g_testedFill.clear();
            g_testedLines.clear();
            g_topFill.clear();
            g_topLines.clear();
            std::vector<Job> volumes[2];
            uint32_t         total = 0, drawn = 0, leftOut = 0;
            const auto take = [&](const sh::Mesh& mesh, const Style& style, float beat) {
                if (!mesh.lines.size() && !mesh.triangles.size()) return;
                const size_t n = mesh.Vertices();
                if (total + n > WXL_DRAW_MAX_FRAME_VERTICES)
                {
                    ++leftOut;
                    return;
                }
                total += static_cast<uint32_t>(n);
                ++drawn;
                const bool                 top   = style.depth == WXL_DRAW_DEPTH_ON_TOP;
                std::vector<sh::Vertex>&   lines = top ? g_topLines : g_testedLines;
                const uint32_t             lc    = Color(style.color, style.alpha * beat);
                const uint8_t              width = sh::WidthCode(style.width);
                for (const sh::Mesh::Segment& s : mesh.lines) sh::PushSegment(lines, s, view.eye, lc, width, mesh.ground);
                if (mesh.triangles.empty()) return;
                if (!mesh.normals.empty())
                {
                    const float dx = mesh.middle.x - view.eye.x, dy = mesh.middle.y - view.eye.y, dz = mesh.middle.z - view.eye.z;
                    volumes[top ? 1 : 0].push_back(Job{&mesh, &style, beat, dx * dx + dy * dy + dz * dz});
                    return;
                }
                std::vector<sh::Vertex>& fill = top ? g_topFill : g_testedFill;
                const uint32_t           fc   = Color(style.color, style.fill * beat);
                for (size_t t = 0; t + 2 < mesh.triangles.size(); t += 3)
                    sh::PushTriangle(fill, mesh.triangles[t], mesh.triangles[t + 1], mesh.triangles[t + 2], view.eye, fc, mesh.ground);
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
                    std::vector<sh::Vertex>& fill = top ? g_topFill : g_testedFill;
                    for (const Job& job : volumes[top])
                    {
                        const uint32_t fc = Color(job.style->color, job.style->fill * job.beat);
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
            g_immediateCount = 0;
            g_immediateOf.clear();

            uint32_t sent = 0;
            if (seen)
            {
                g_stream.clear();
                g_stream.insert(g_stream.end(), g_testedFill.begin(), g_testedFill.end());
                g_stream.insert(g_stream.end(), g_testedLines.begin(), g_testedLines.end());
                const uint32_t tested = static_cast<uint32_t>(g_stream.size());
                g_stream.insert(g_stream.end(), g_topFill.begin(), g_topFill.end());
                g_stream.insert(g_stream.end(), g_topLines.begin(), g_topLines.end());
                const uint32_t onTop = static_cast<uint32_t>(g_stream.size()) - tested;
                float          constants[24];
                sh::Constants(view, constants);
                std::string why;
                if (wxl::vk::render::Render* r = VulkanRender())
                {
                    if (g_attached != r)
                    {
                        g_vulkan.Forget();
                        g_vulkan.Attach(*r);
                        g_attached = r;
                    }
                    sent = g_vulkan.Draw(g_stream.data(), tested, onTop, constants, view.viewport, &why);
                }
                else
                    sent = gx::d3d9::shapes::Draw(g_stream.data(), tested, onTop, constants, view.viewport, &why);
                if (!sent && tested + onTop) Failed(why);
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
            ++g_stats.frames;
            g_immediateSeconds = 0.0;
            g_leftOutSince += leftOut;
        }

        void Present()
        {
            if (!g_worldThisFrame)
            {
                // No world this frame (a loading screen): what was drawn for it goes.
                g_immediateCount = 0;
                g_immediateOf.clear();
            }
            g_worldThisFrame = false;
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
                if (g_immediate[i].mod == mod) continue;
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
            g_immediate.clear();
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
