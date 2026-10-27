// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.animation over the binding. A play is the client's CGUnit_C::PlayBaseAnimation, as an emote
// is played; the runtime keeps one play per unit, with what the model showed once it was played
// (the model's own animation after the client's fallback), and follows it at each
// CGUnit_C::PreAnimate of its unit: a one-shot ends after one play of its length (handed back
// with CGUnit_C::UpdateBaseAnimation when the model would go on) or when the game showed
// something else; a loop is played again when the unit is idle (or at once under override, which
// a one-shot ignores).

#include "services/AnimationService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/mod_host.hpp"

#include <atomic>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace wxl::core;

    constexpr const char* kTag = "animation";

    /// PlayBaseAnimation's own flags: no blend, and the id as given (no fallback).
    constexpr uint32_t kClientNoBlend = 0x1;
    constexpr uint32_t kClientExact   = 0x4;

    /// What the client shows a unit with nothing else to do (CGUnit_C::ChooseDefaultAnim):
    /// Stand, SwimIdle, StealthStand, Hover.
    bool IsIdle(uint32_t anim) { return anim == 0 || anim == 41 || anim == 120 || anim == 193; }

    /// A one-shot that showed this share of its length before the game chose something else
    /// ended by itself.
    constexpr double kFinishedShare = 0.9;

    struct Play
    {
        uint32_t   mod;
        wxl_handle unit;
        uint32_t   anim;    ///< as asked
        uint32_t   shown;   ///< what the model showed once it was played
        uint32_t   flags;   ///< WXL_ANIMATION_*
        float      speed;
        uint32_t   start;   ///< os_time_ms of the last start
        uint32_t   length;  ///< one play of `shown` at speed 1, ms; 0 unknown
    };

    std::vector<Play>                     g_plays;
    std::vector<wxl_unit_animation_args>  g_ended;

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
            WLOG_WARN(kTag, "wxl.animation.%s was called off the main thread, where units animate; it was refused", call);
        return false;
    }

    /// A refusal, logged once per mod and call.
    void Refuse(const char* call, const char* why)
    {
        static std::set<std::pair<uint32_t, std::string>> said;
        const uint32_t mod = Caller();
        if (!said.insert({mod, call}).second) return;
        WLOG_WARN(kTag, "wxl.animation.%s was refused: %s", call, why);
    }

    void* Resolve(wxl_handle unit, uint32_t& kind)
    {
        kind = WXL_OBJECT_UNKNOWN;
        return objects::Service().Handles().Resolve(unit, &kind);
    }

    bool IsUnit(uint32_t kind) { return kind == WXL_OBJECT_UNIT || kind == WXL_OBJECT_PLAYER; }

    uint32_t ClientFlags(uint32_t flags)
    {
        return ((flags & WXL_ANIMATION_NO_BLEND) ? kClientNoBlend : 0u) | ((flags & WXL_ANIMATION_EXACT) ? kClientExact : 0u);
    }

    /// One play of the animation at its speed, in ms; 0 when its length is not known.
    uint32_t Duration(const Play& play)
    {
        return play.length ? static_cast<uint32_t>(play.length / play.speed) : 0u;
    }

    void End(const Play& play, wxl_animation_end reason)
    {
        wxl_unit_animation_args args{};
        args.unit    = play.unit;
        args.anim_id = play.anim;
        args.reason  = static_cast<uint32_t>(reason);
        args.looped  = (play.flags & WXL_ANIMATION_LOOP) ? WXL_TRUE : WXL_FALSE;
        g_ended.push_back(args);
    }

    /// Has the client play the animation on the unit; the model's animation then, or
    /// 0xFFFFFFFF when the client did not play it.
    uint32_t Start(void* unit, uint32_t anim, uint32_t flags, float speed, uint32_t heldShown)
    {
        const uint32_t before = binding::unit_shown_animation(unit);
        if (before == 0xFFFFFFFFu) return 0xFFFFFFFFu;
        binding::unit_play_animation(unit, anim, ClientFlags(flags));
        const uint32_t after = binding::unit_shown_animation(unit);
        // The model shows what it showed: the client refused, unless that is the animation asked
        // for (or, played again, the one it falls back to).
        if (after == 0xFFFFFFFFu || (after == before && after != anim && after != heldShown)) return 0xFFFFFFFFu;
        if (speed != 1.0f) binding::unit_animation_speed(unit, after, speed);
        return after;
    }

    bool Alive(const void* unit) { return binding::unit_health(unit) > 0; }

    wxl_result WXL_CALL AnimationPlay(wxl_handle unit, uint32_t anim, uint32_t flags, double speed)
    {
        if (!OnGameThread("Play")) return WXL_E_INVALID;
        if (anim > WXL_ANIMATION_MAX_ID)
        {
            Refuse("Play", "an animation id past the client's (AnimationData ends below 506)");
            return WXL_E_INVALID;
        }
        if (flags & ~WXL_ANIMATION_FLAGS)
        {
            Refuse("Play", "a flag it does not know (loop 1, no_blend 2, override 4, exact 8)");
            return WXL_E_INVALID;
        }
        if (speed == 0.0) speed = 1.0;
        if (!std::isfinite(speed) || speed < WXL_ANIMATION_MIN_SPEED || speed > WXL_ANIMATION_MAX_SPEED)
        {
            Refuse("Play", "a speed outside 0.05 to 20");
            return WXL_E_INVALID;
        }
        uint32_t kind = 0;
        void*    self = Resolve(unit, kind);
        if (!self) return WXL_E_NOT_FOUND;
        if (!IsUnit(kind)) return WXL_E_INVALID;
        if (!Alive(self)) return WXL_E_FAILED;

        uint32_t heldShown = 0xFFFFFFFFu;
        for (const Play& play : g_plays)
            if (play.unit == unit) heldShown = play.shown;

        const float    rate  = static_cast<float>(speed);
        const uint32_t shown = Start(self, anim, flags, rate, heldShown);
        if (shown == 0xFFFFFFFFu) return WXL_E_FAILED;

        for (size_t i = g_plays.size(); i-- > 0;)
            if (g_plays[i].unit == unit)
            {
                End(g_plays[i], WXL_ANIMATION_END_REPLACED);
                g_plays.erase(g_plays.begin() + static_cast<std::ptrdiff_t>(i));
            }
        Play play{Caller(), unit, anim, shown, flags, rate, binding::os_time_ms(), binding::unit_animation_length(self, shown)};
        g_plays.push_back(play);
        return WXL_OK;
    }

    wxl_result WXL_CALL AnimationStop(wxl_handle unit)
    {
        if (!OnGameThread("Stop")) return WXL_E_INVALID;
        const uint32_t mod = Caller();
        for (size_t i = 0; i < g_plays.size(); ++i)
        {
            const Play play = g_plays[i];
            if (play.unit != unit || play.mod != mod) continue;
            g_plays.erase(g_plays.begin() + static_cast<std::ptrdiff_t>(i));
            uint32_t kind = 0;
            void*    self = Resolve(unit, kind);
            if (self && binding::unit_shown_animation(self) == play.shown) binding::unit_update_animation(self);
            End(play, WXL_ANIMATION_END_STOPPED);
            return WXL_OK;
        }
        return WXL_E_NOT_FOUND;
    }

    wxl_result WXL_CALL AnimationPlaying(wxl_handle unit, uint32_t* out)
    {
        if (!OnGameThread("Playing")) return WXL_E_INVALID;
        if (!out) return WXL_E_INVALID;
        for (const Play& play : g_plays)
            if (play.unit == unit)
            {
                *out = play.anim;
                return WXL_OK;
            }
        return WXL_E_NOT_FOUND;
    }

    wxl_result WXL_CALL AnimationCurrent(wxl_handle unit, uint32_t* out)
    {
        if (!OnGameThread("Current")) return WXL_E_INVALID;
        if (!out) return WXL_E_INVALID;
        uint32_t kind = 0;
        void*    self = Resolve(unit, kind);
        if (!self) return WXL_E_NOT_FOUND;
        if (!IsUnit(kind)) return WXL_E_INVALID;
        const uint32_t shown = binding::unit_shown_animation(self);
        if (shown == 0xFFFFFFFFu) return WXL_E_FAILED;
        *out = shown;
        return WXL_OK;
    }

    /// Follows one play at its unit's PreAnimate; false when it ended.
    bool Follow(Play& play, void* unit)
    {
        const uint32_t now      = binding::os_time_ms();
        const uint32_t shown    = binding::unit_shown_animation(unit);
        const uint32_t duration = Duration(play);
        const uint32_t elapsed  = now - play.start;
        const bool     loop     = (play.flags & WXL_ANIMATION_LOOP) != 0;
        const bool     override = (play.flags & WXL_ANIMATION_OVERRIDE) != 0;

        if (shown == 0xFFFFFFFFu) return true; // the model reloads (a new appearance): wait for it
        if (shown == play.shown)
        {
            if (!loop && duration && elapsed >= duration)
            {
                // One play is over; a model whose sequence goes on is handed back to the game.
                binding::unit_update_animation(unit);
                End(play, WXL_ANIMATION_END_FINISHED);
                return false;
            }
            if (loop && duration && elapsed >= duration) play.start = now;
            return true;
        }

        // The game shows something else.
        if (!loop)
        {
            if (duration && elapsed >= static_cast<uint32_t>(duration * kFinishedShare))
            {
                End(play, WXL_ANIMATION_END_FINISHED);
                return false;
            }
            // Override is a loop's: a one-shot the game replaced has ended.
            End(play, WXL_ANIMATION_END_INTERRUPTED);
            return false;
        }
        if (!(override ? Alive(unit) : IsIdle(shown)))
        {
            return true; // a loop waits for the unit to be idle again
        }
        const uint32_t again = Start(unit, play.anim, play.flags, play.speed, play.shown);
        if (again != 0xFFFFFFFFu)
        {
            play.shown = again;
            play.start = now;
        }
        return true;
    }
}

namespace wxl::core::services
{
    const wxl_animation_api& AnimationApi()
    {
        static const wxl_animation_api api = [] {
            wxl_animation_api table{};
            table.struct_size = sizeof table;
            table.play        = &AnimationPlay;
            table.stop        = &AnimationStop;
            table.playing     = &AnimationPlaying;
            table.current     = &AnimationCurrent;
            return table;
        }();
        return api;
    }

    namespace animation
    {
        bool Busy() { return !g_plays.empty() || !g_ended.empty(); }

        void PreAnimate(void* unit, bool activePlayer)
        {
            if (g_plays.empty() || !unit) return;
            for (size_t i = g_plays.size(); i-- > 0;)
            {
                uint32_t kind = 0;
                void*    self = Resolve(g_plays[i].unit, kind);
                if (!self)
                {
                    // Swept at the active player's frame, which every frame in the world has.
                    if (!activePlayer) continue;
                    End(g_plays[i], WXL_ANIMATION_END_GONE);
                    g_plays.erase(g_plays.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                if (self != unit) continue;
                if (!Follow(g_plays[i], self)) g_plays.erase(g_plays.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }

        void TakeEnded(std::vector<wxl_unit_animation_args>& out)
        {
            out.clear();
            out.swap(g_ended);
        }

        void ForgetMod(uint32_t mod)
        {
            for (size_t i = g_plays.size(); i-- > 0;)
            {
                const Play play = g_plays[i];
                if (play.mod != mod) continue;
                g_plays.erase(g_plays.begin() + static_cast<std::ptrdiff_t>(i));
                uint32_t kind = 0;
                void*    self = Resolve(play.unit, kind);
                if (self && binding::unit_shown_animation(self) == play.shown) binding::unit_update_animation(self);
                End(play, WXL_ANIMATION_END_STOPPED);
            }
        }

        void Clear()
        {
            g_plays.clear();
            g_ended.clear();
        }
    }
}
