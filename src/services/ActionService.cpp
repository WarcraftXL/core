// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.action over the binding: each call checks the thread, the world and the object it is
// handed, then asks the client to do what the player's click or key would.

#include "services/ActionService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/mod_host.hpp"

#include <atomic>
#include <cmath>

namespace
{
    using namespace wxl::core;

    /// One kind of refusal, said once for each call that meets it.
    struct Said
    {
        std::atomic<bool> offThread{false};
    };

    /// Whether the call runs where the client's objects live; says once why not.
    bool OnGameThread(const char* call, Said& said)
    {
        const wxl::host::ModHost* host = Host();
        if (!host || host->OnMainThread()) return true;
        if (!said.offThread.exchange(true))
            WLOG_WARN(kTag, "wxl.action.%s was called off the main thread: an action runs on the thread "
                            "that runs the game, where the mod's handlers of world events run; it was refused",
                      call);
        return false;
    }

    /// The native object of a handle, of a kind in `kinds` (one bit per wxl_object_type), and
    /// the answer for a handle that does not give one.
    wxl_result Resolve(wxl_handle handle, uint32_t kinds, void*& native)
    {
        uint32_t kind = WXL_OBJECT_UNKNOWN;
        native        = objects::Service().Handles().Resolve(handle, &kind);
        if (!native) return WXL_E_NOT_FOUND;
        return kind < 32u && (kinds & (1u << kind)) ? WXL_OK : WXL_E_INVALID;
    }

    constexpr uint32_t kAny   = 0xFEu;
    constexpr uint32_t kUnits = (1u << WXL_OBJECT_UNIT) | (1u << WXL_OBJECT_PLAYER);
    constexpr uint32_t kItems = (1u << WXL_OBJECT_ITEM) | (1u << WXL_OBJECT_CONTAINER);

    wxl_result Done(wxl_bool taken) { return taken == WXL_TRUE ? WXL_OK : WXL_E_FAILED; }

    /// The checks every call starts with: the thread, then the world (an active player).
    #define WXL_ACTION_ENTER(call)                                    \
        static Said said;                                             \
        if (!OnGameThread(call, said)) return WXL_E_INVALID;          \
        const void* player = binding::local_player_object();          \
        if (!player) return WXL_E_UNSUPPORTED;                        \
        (void)player

    wxl_result WXL_CALL Target(wxl_handle object)
    {
        WXL_ACTION_ENTER("Target");
        void*            native = nullptr;
        const wxl_result found  = Resolve(object, kAny, native);
        if (found != WXL_OK) return found;
        return Done(binding::action_target(binding::object_guid(native)));
    }

    wxl_result WXL_CALL ClearTarget()
    {
        WXL_ACTION_ENTER("ClearTarget");
        return Done(binding::action_target(0));
    }

    wxl_result WXL_CALL Interact(wxl_handle object)
    {
        WXL_ACTION_ENTER("Interact");
        void*            native = nullptr;
        const wxl_result found  = Resolve(object, kAny, native);
        if (found != WXL_OK) return found;
        return Done(binding::action_interact(binding::object_guid(native)));
    }

    wxl_result WXL_CALL CastSpell(uint32_t spell, wxl_handle target)
    {
        WXL_ACTION_ENTER("CastSpell");
        if (!spell) return WXL_E_INVALID;
        uint64_t guid = 0;
        if (target != WXL_HANDLE_NONE)
        {
            void*            native = nullptr;
            const wxl_result found  = Resolve(target, kAny, native);
            if (found != WXL_OK) return found;
            guid = binding::object_guid(native);
        }
        return Done(binding::action_cast_spell(spell, guid));
    }

    wxl_result WXL_CALL StopCasting()
    {
        WXL_ACTION_ENTER("StopCasting");
        return Done(binding::action_stop_casting());
    }

    wxl_result WXL_CALL UseItem(wxl_handle item, wxl_handle target)
    {
        WXL_ACTION_ENTER("UseItem");
        void*            native = nullptr;
        const wxl_result found  = Resolve(item, kItems, native);
        if (found != WXL_OK) return found;
        uint64_t guid = 0;
        if (target != WXL_HANDLE_NONE)
        {
            void*            on    = nullptr;
            const wxl_result there = Resolve(target, kAny, on);
            if (there != WXL_OK) return there;
            guid = binding::object_guid(on);
        }
        return Done(binding::action_use_item(native, guid));
    }

    wxl_result WXL_CALL Follow(wxl_handle unit)
    {
        WXL_ACTION_ENTER("Follow");
        void*            native = nullptr;
        const wxl_result found  = Resolve(unit, kUnits, native);
        if (found != WXL_OK) return found;
        return Done(binding::action_follow(player, binding::object_guid(native)));
    }

    wxl_result WXL_CALL Assist(wxl_handle unit)
    {
        WXL_ACTION_ENTER("Assist");
        void*            native = nullptr;
        const wxl_result found  = Resolve(unit, kUnits, native);
        if (found != WXL_OK) return found;
        // Script_AssistUnit: the unit's target, when it has one.
        const uint64_t target = binding::unit_target(native);
        if (!target) return WXL_E_FAILED;
        return Done(binding::action_target(target));
    }

    wxl_result WXL_CALL MoveTo(wxl_vec3 point)
    {
        WXL_ACTION_ENTER("MoveTo");
        return Done(binding::action_move_to(player, point));
    }

    wxl_result WXL_CALL Face(double facing)
    {
        WXL_ACTION_ENTER("Face");
        if (!std::isfinite(facing)) return WXL_E_INVALID;
        // A facing as the client keeps it, in [0, 2 pi).
        constexpr double kTurn = 6.283185307179586;
        double           wrapped = std::fmod(facing, kTurn);
        if (wrapped < 0.0) wrapped += kTurn;
        return Done(binding::action_face(player, static_cast<float>(wrapped)));
    }

    #undef WXL_ACTION_ENTER
}

namespace wxl::core::services
{
    const wxl_action_api& ActionApi()
    {
        static const wxl_action_api api = [] {
            wxl_action_api table{};
            table.struct_size  = sizeof table;
            table.target       = &Target;
            table.clear_target = &ClearTarget;
            table.interact     = &Interact;
            table.cast_spell   = &CastSpell;
            table.stop_casting = &StopCasting;
            table.use_item     = &UseItem;
            table.follow       = &Follow;
            table.assist       = &Assist;
            table.move_to      = &MoveTo;
            table.face         = &Face;
            return table;
        }();
        return api;
    }
}
