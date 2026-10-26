// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.selectioncircle: the looks mods give the game's circles, kept here. Each circle the game
// draws passes through a drawer's seam (the target's, the pet's target's, the corpse's, the move
// marker's): Begin picks the look that applies to it and puts a mod's texture in the static the
// game binds, ProjectTex.Render2d's gate skips a hidden circle or points the projection at a
// scaled box and another colour, End puts the game's texture back. The same on both runtimes:
// what differs is only how the bindings reach the client.

#include "services/SelectionCircleService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/mod_host.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace wxl::core;

    constexpr uint32_t kKinds     = 3;
    constexpr uint32_t kReactions = 9; ///< the eight wxl_reaction values, then none
    constexpr uint32_t kNone      = 8; ///< the reaction slot of a circle under no unit

    /// A texture a mod gave, made once per path and let go with its last look.
    struct Texture
    {
        std::string path;
        void*       handle;
        uint32_t    looks;
    };

    /// What a mod made of some circles: hidden, or drawn with a texture, colour or scale of its
    /// own (null, 0 and 0 keeping the game's).
    struct Look
    {
        bool     hidden  = false;
        void*    texture = nullptr;
        uint32_t color   = 0;
        float    scale   = 0.0f;
        uint64_t order   = 0; ///< the later set wins among mods
    };

    /// A look for one kind and one reaction.
    struct CellRule
    {
        uint32_t mod;
        uint32_t kind;     ///< 0 target, 1 pet target, 2 move marker
        uint32_t reaction; ///< a wxl_reaction, or kNone
        Look     look;
    };

    /// A look for one object.
    struct ObjectRule
    {
        uint32_t   mod;
        wxl_handle target;
        Look       look;
    };

    /// A CAaBox: its two corners.
    struct Box
    {
        float b[3];
        float t[3];
    };

    std::vector<Texture>    g_textures;
    std::vector<CellRule>   g_cells;
    std::vector<ObjectRule> g_objects;
    uint64_t                g_order = 0;

    // The circle being drawn, between a drawer's Before and After.
    bool  g_inCircle  = false;
    bool  g_styled    = false; ///< a look applies to it
    Look  g_look;
    bool  g_swapped   = false; ///< the static holds a mod's texture
    void* g_gameTexture = nullptr;
    Box      g_box{};   ///< the projection's box, scaled: Render2d reads it through its argument
    uint32_t g_color = 0; ///< and its colour, a CImVector's dword (0xAARRGGBB)

    // The counts of the world frame being drawn, and of the last one drawn.
    uint32_t g_drawn = 0, g_hidden = 0;
    uint32_t g_lastDrawn = 0, g_lastHidden = 0;

    uint32_t Caller()
    {
        const wxl::host::ModHost* host = Host();
        return host ? host->CurrentMod() : 0u;
    }

    bool OnGameThread(const char* call)
    {
        static std::atomic<bool> said{false};
        const wxl::host::ModHost* host = Host();
        if (!host || host->OnMainThread()) return true;
        if (!said.exchange(true))
            WLOG_WARN(kTag, "wxl.selectioncircle.%s was called off the main thread, where the circles are drawn; it was refused",
                      call);
        return false;
    }

    bool MasksValid(uint32_t kinds, uint32_t reactions)
    {
        return !(kinds & ~WXL_SELECTIONCIRCLE_ALL_KINDS) && !(reactions & ~WXL_SELECTIONCIRCLE_ALL_REACTIONS);
    }

    bool ScaleValid(float scale) { return std::isfinite(scale) && scale >= 0.0f && scale <= WXL_SELECTIONCIRCLE_MAX_SCALE; }

    bool SamePath(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            char x = a[i], y = b[i];
            if (x == '/') x = '\\';
            if (y == '/') y = '\\';
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
            if (x != y) return false;
        }
        return true;
    }

    /// The texture of a path, made at its first look; null when the client makes none.
    void* TakeTexture(const std::string& path)
    {
        for (Texture& texture : g_textures)
            if (SamePath(texture.path, path))
            {
                ++texture.looks;
                return texture.handle;
            }
        void* handle = binding::texture_open(path.c_str());
        if (!handle) return nullptr;
        g_textures.push_back(Texture{path, handle, 1});
        return handle;
    }

    /// One look let go of its texture; the texture goes with the last.
    void DropTexture(void* handle)
    {
        if (!handle) return;
        for (size_t i = 0; i < g_textures.size(); ++i)
            if (g_textures[i].handle == handle)
            {
                if (--g_textures[i].looks) return;
                binding::texture_close(handle);
                g_textures.erase(g_textures.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
    }

    /// A look from a call's arguments: its texture taken, or WXL_E_NOT_FOUND when the client
    /// makes none from the path.
    wxl_result MakeLook(bool hidden, wxl_str path, uint32_t color, float scale, uint32_t looks, Look& out)
    {
        out        = Look{};
        out.hidden = hidden;
        out.color  = color;
        out.scale  = scale;
        out.order  = ++g_order;
        if (!hidden && path.data && path.size)
        {
            const std::string name(path.data, path.size);
            out.texture = TakeTexture(name);
            if (!out.texture) return WXL_E_NOT_FOUND;
            // One handle per look that holds it.
            for (uint32_t i = 1; i < looks; ++i) TakeTexture(name);
        }
        return WXL_OK;
    }

    /// Calls `fn(kind, reaction)` for every cell of the masks, 0 meaning all of either.
    template <class Fn>
    void ForCells(uint32_t kinds, uint32_t reactions, Fn&& fn)
    {
        if (!kinds) kinds = WXL_SELECTIONCIRCLE_ALL_KINDS;
        if (!reactions) reactions = WXL_SELECTIONCIRCLE_ALL_REACTIONS;
        for (uint32_t k = 0; k < kKinds; ++k)
            for (uint32_t r = 0; r < kReactions; ++r)
                if ((kinds & (1u << k)) && (reactions & (1u << r))) fn(k, r);
    }

    uint32_t CellCount(uint32_t kinds, uint32_t reactions)
    {
        uint32_t count = 0;
        ForCells(kinds, reactions, [&](uint32_t, uint32_t) { ++count; });
        return count;
    }

    /// Takes the calling mod's cells of the masks off.
    void RemoveCells(uint32_t mod, uint32_t kinds, uint32_t reactions)
    {
        ForCells(kinds, reactions, [&](uint32_t k, uint32_t r) {
            for (size_t i = g_cells.size(); i-- > 0;)
                if (g_cells[i].mod == mod && g_cells[i].kind == k && g_cells[i].reaction == r)
                {
                    DropTexture(g_cells[i].look.texture);
                    g_cells.erase(g_cells.begin() + static_cast<std::ptrdiff_t>(i));
                }
        });
    }

    wxl_result SetCells(bool hidden, uint32_t kinds, uint32_t reactions, wxl_str texture, uint32_t color, float scale)
    {
        if (!MasksValid(kinds, reactions)) return WXL_E_INVALID;
        if (!hidden && !ScaleValid(scale)) return WXL_E_INVALID;
        Look             look;
        const wxl_result made = MakeLook(hidden, texture, color, scale, CellCount(kinds, reactions), look);
        if (made != WXL_OK) return made;
        const uint32_t mod = Caller();
        RemoveCells(mod, kinds, reactions);
        ForCells(kinds, reactions, [&](uint32_t k, uint32_t r) { g_cells.push_back(CellRule{mod, k, r, look}); });
        return WXL_OK;
    }

    /// Drops the looks of objects that left view.
    void PruneObjects()
    {
        for (size_t i = g_objects.size(); i-- > 0;)
            if (!objects::Service().Handles().Resolve(g_objects[i].target))
            {
                DropTexture(g_objects[i].look.texture);
                g_objects.erase(g_objects.begin() + static_cast<std::ptrdiff_t>(i));
            }
    }

    wxl_result SetObject(bool hidden, wxl_handle target, wxl_str texture, uint32_t color, float scale)
    {
        if (!hidden && !ScaleValid(scale)) return WXL_E_INVALID;
        if (!objects::Service().Handles().Resolve(target)) return WXL_E_NOT_FOUND;
        PruneObjects();
        const uint32_t mod  = Caller();
        const auto     held = std::find_if(g_objects.begin(), g_objects.end(),
                                           [&](const ObjectRule& rule) { return rule.mod == mod && rule.target == target; });
        if (held == g_objects.end() && g_objects.size() >= WXL_SELECTIONCIRCLE_MAX_OBJECTS) return WXL_E_INVALID;
        Look             look;
        const wxl_result made = MakeLook(hidden, texture, color, scale, 1, look);
        if (made != WXL_OK) return made;
        if (held != g_objects.end())
        {
            DropTexture(held->look.texture);
            held->look = look;
        }
        else
            g_objects.push_back(ObjectRule{mod, target, look});
        return WXL_OK;
    }

    wxl_result WXL_CALL Hide(uint32_t kinds, uint32_t reactions)
    {
        if (!OnGameThread("hide")) return WXL_E_INVALID;
        return SetCells(true, kinds, reactions, wxl_str{nullptr, 0}, 0, 0.0f);
    }

    wxl_result WXL_CALL SetStyle(uint32_t kinds, uint32_t reactions, wxl_str texture, uint32_t color, float scale)
    {
        if (!OnGameThread("set_style")) return WXL_E_INVALID;
        return SetCells(false, kinds, reactions, texture, color, scale);
    }

    wxl_result WXL_CALL Reset(uint32_t kinds, uint32_t reactions)
    {
        if (!OnGameThread("reset")) return WXL_E_INVALID;
        if (!MasksValid(kinds, reactions)) return WXL_E_INVALID;
        RemoveCells(Caller(), kinds, reactions);
        return WXL_OK;
    }

    wxl_result WXL_CALL HideObject(wxl_handle target)
    {
        if (!OnGameThread("hide_object")) return WXL_E_INVALID;
        return SetObject(true, target, wxl_str{nullptr, 0}, 0, 0.0f);
    }

    wxl_result WXL_CALL SetObjectStyle(wxl_handle target, wxl_str texture, uint32_t color, float scale)
    {
        if (!OnGameThread("set_object_style")) return WXL_E_INVALID;
        return SetObject(false, target, texture, color, scale);
    }

    wxl_result WXL_CALL ResetObject(wxl_handle target)
    {
        if (!OnGameThread("reset_object")) return WXL_E_INVALID;
        const uint32_t mod  = Caller();
        const auto     held = std::find_if(g_objects.begin(), g_objects.end(),
                                           [&](const ObjectRule& rule) { return rule.mod == mod && rule.target == target; });
        if (held == g_objects.end()) return WXL_E_NOT_FOUND;
        DropTexture(held->look.texture);
        g_objects.erase(held);
        return WXL_OK;
    }

    void WXL_CALL ResetAll()
    {
        if (!OnGameThread("reset_all")) return;
        services::selectioncircle::ForgetMod(Caller());
    }

    uint32_t WXL_CALL Drawn() { return g_lastDrawn; }
    uint32_t WXL_CALL Hidden() { return g_lastHidden; }

    /// The look of a circle: an object's own first, then its kind's and reaction's; the later set
    /// among mods. Null when none applies.
    const Look* Choose(uint32_t kind, const void* object, uint32_t reaction)
    {
        const Look* chosen = nullptr;
        if (!g_objects.empty())
        {
            PruneObjects();
            for (const ObjectRule& rule : g_objects)
                if (objects::Service().Handles().Resolve(rule.target) == object && (!chosen || chosen->order < rule.look.order))
                    chosen = &rule.look;
            if (chosen) return chosen;
        }
        for (const CellRule& rule : g_cells)
            if (rule.kind == kind && rule.reaction == reaction && (!chosen || chosen->order < rule.look.order))
                chosen = &rule.look;
        return chosen;
    }

    uint8_t Channel(uint32_t color, int shift) { return static_cast<uint8_t>((color >> shift) & 0xFFu); }
}

namespace wxl::core::services
{
    const wxl_selectioncircle_api& SelectionCircleApi()
    {
        static const wxl_selectioncircle_api api = [] {
            wxl_selectioncircle_api table{};
            table.struct_size      = sizeof table;
            table.hide             = &Hide;
            table.set_style        = &SetStyle;
            table.reset            = &Reset;
            table.hide_object      = &HideObject;
            table.set_object_style = &SetObjectStyle;
            table.reset_object     = &ResetObject;
            table.reset_all        = &ResetAll;
            table.drawn            = &Drawn;
            table.hidden           = &Hidden;
            return table;
        }();
        return api;
    }

    namespace selectioncircle
    {
        void Begin(uint32_t kind, const void* object, bool unit)
        {
            g_inCircle = true;
            g_styled   = false;
            g_swapped  = false;
            if (g_cells.empty() && g_objects.empty()) return;

            uint32_t index = 0;
            while (index < kKinds && kind != (1u << index)) ++index;
            if (index == kKinds) return;
            uint32_t reaction = kNone;
            if (unit && object)
            {
                const int32_t read = binding::unit_reaction(object, binding::local_player_object());
                if (read >= 0 && read < static_cast<int32_t>(kNone)) reaction = static_cast<uint32_t>(read);
            }
            const Look* look = Choose(index, object, reaction);
            if (!look) return;
            g_look   = *look;
            g_styled = true;
            if (!g_look.hidden && g_look.texture)
            {
                g_gameTexture = binding::circle_swap_texture(g_look.texture);
                g_swapped     = true;
            }
        }

        void End()
        {
            if (g_swapped) binding::circle_swap_texture(g_gameTexture);
            g_swapped  = false;
            g_styled   = false;
            g_inCircle = false;
        }

        bool Project(const void** box, const void** color)
        {
            if (!g_inCircle || !box || !color) return false;
            if (g_styled && g_look.hidden)
            {
                ++g_hidden;
                return true;
            }
            ++g_drawn;
            if (!g_styled) return false;
            if (g_look.scale > 0.0f && *box)
            {
                // Scaled around its centre: across the ground by the scale, up and down never
                // less than the game's, so the circle still reaches a slope.
                std::memcpy(&g_box, *box, sizeof g_box);
                for (int axis = 0; axis < 3; ++axis)
                {
                    const float factor = axis == 2 ? std::max(g_look.scale, 1.0f) : g_look.scale;
                    const float centre = (g_box.b[axis] + g_box.t[axis]) * 0.5f;
                    const float half   = (g_box.t[axis] - g_box.b[axis]) * 0.5f * factor;
                    g_box.b[axis]      = centre - half;
                    g_box.t[axis]      = centre + half;
                }
                *box = &g_box;
            }
            if (g_look.color && *color)
            {
                // The mod's colour, its alpha times the game's: the game's own fades (the
                // closing rings, the marker's pulse) still apply over it.
                uint32_t game = 0;
                std::memcpy(&game, *color, sizeof game);
                const uint32_t alpha = (Channel(g_look.color, 24) * Channel(game, 24) + 127u) / 255u;
                g_color              = (g_look.color & 0x00FFFFFFu) | (alpha << 24);
                *color               = &g_color;
            }
            return false;
        }

        void WorldEnd()
        {
            g_lastDrawn  = g_drawn;
            g_lastHidden = g_hidden;
            g_drawn = g_hidden = 0;
        }

        void ForgetMod(uint32_t mod)
        {
            for (size_t i = g_cells.size(); i-- > 0;)
                if (g_cells[i].mod == mod)
                {
                    DropTexture(g_cells[i].look.texture);
                    g_cells.erase(g_cells.begin() + static_cast<std::ptrdiff_t>(i));
                }
            for (size_t i = g_objects.size(); i-- > 0;)
                if (g_objects[i].mod == mod)
                {
                    DropTexture(g_objects[i].look.texture);
                    g_objects.erase(g_objects.begin() + static_cast<std::ptrdiff_t>(i));
                }
        }

        void Clear()
        {
            if (g_swapped) binding::circle_swap_texture(g_gameTexture);
            g_swapped  = false;
            g_inCircle = false;
            g_styled   = false;
            g_cells.clear();
            g_objects.clear();
            for (const Texture& texture : g_textures) binding::texture_close(texture.handle);
            g_textures.clear();
            g_drawn = g_hidden = g_lastDrawn = g_lastHidden = 0;
        }
    }
}
