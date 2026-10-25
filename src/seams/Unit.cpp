// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Unit seams: what the runtime does at a unit's death and aura changes, on both builds.

#include "wxl/seams/seams.h"

#include "services/PoseService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"
#include "seams/Gameplay.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstdint>
#include <cstring>

namespace
{
    /// The movement opcodes that start a movement, and the ones that end it; the rest (run
    /// mode, facing, heartbeat, acks, ...) raise nothing.
    bool IsMoveStart(uint32_t opcode)
    {
        switch (opcode)
        {
        case 181: // MSG_MOVE_START_FORWARD
        case 182: // MSG_MOVE_START_BACKWARD
        case 184: // MSG_MOVE_START_STRAFE_LEFT
        case 185: // MSG_MOVE_START_STRAFE_RIGHT
        case 187: // MSG_MOVE_JUMP
        case 188: // MSG_MOVE_START_TURN_LEFT
        case 189: // MSG_MOVE_START_TURN_RIGHT
        case 191: // MSG_MOVE_START_PITCH_UP
        case 192: // MSG_MOVE_START_PITCH_DOWN
        case 0x359: // MSG_MOVE_START_ASCEND
        case 0x3A7: // MSG_MOVE_START_DESCEND
            return true;
        default: return false;
        }
    }

    bool IsMoveStop(uint32_t opcode)
    {
        switch (opcode)
        {
        case 183: // MSG_MOVE_STOP
        case 186: // MSG_MOVE_STOP_STRAFE
        case 190: // MSG_MOVE_STOP_TURN
        case 193: // MSG_MOVE_STOP_PITCH
        case 201: // MSG_MOVE_FALL_LAND
        case 0x35A: // MSG_MOVE_STOP_ASCEND
            return true;
        default: return false;
        }
    }

    /// Raises UnitAuraChanged for a tracked unit; an untracked one is logged and skipped.
    void AuraChanged(void* self, int32_t spellId, wxl_bool removed)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        wxl_unit_aura_args args{};
        args.unit = wxl::core::objects::HandleOf(self);
        if (args.unit == WXL_HANDLE_NONE)
        {
            WLOG_DEBUG(wxl::core::kTag, "aura change on an untracked unit; no event");
            return;
        }
        args.spell_id = static_cast<uint32_t>(spellId);
        args.removed  = removed;
        host->Raise(WXL_EV_UnitAuraChanged, &args);
    }
}

namespace wxl::seam::Unit::OnDeath
{
    void WXL_CALL After(void* self)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        wxl_unit_args args{};
        args.unit = wxl::core::objects::HandleOf(self);
        if (args.unit == WXL_HANDLE_NONE)
        {
            WLOG_DEBUG(wxl::core::kTag, "death of an untracked unit; no event");
            return;
        }
        host->Raise(WXL_EV_UnitDeath, &args);
    }
}

namespace wxl::seam::Unit::OnAuraAdded
{
    void WXL_CALL After(void* self, uint32_t index, uint8_t, void* aura, int32_t spell_id)
    {
        AuraChanged(self, spell_id, WXL_FALSE);
        wxl::host::ModHost* host = wxl::core::seams::gameplay::Listener({ WXL_EV_UnitAuraApplied });
        if (!host) return;
        wxl_aura_args args{};
        if (!wxl::core::seams::gameplay::Aura(self, aura, index, args)) return;
        host->Raise(WXL_EV_UnitAuraApplied, &args);
    }
}

namespace wxl::seam::Unit::OnAuraRemoved
{
    void WXL_CALL After(void* self, uint32_t index, uint8_t, void* aura, int32_t spell_id)
    {
        AuraChanged(self, spell_id, WXL_TRUE);
        wxl::host::ModHost* host = wxl::core::seams::gameplay::Listener({ WXL_EV_UnitAuraRemoved });
        if (!host) return;
        // `aura` is the copy of the slot from before the update: what the aura was.
        wxl_aura_args args{};
        if (!wxl::core::seams::gameplay::Aura(self, aura, index, args)) return;
        host->Raise(WXL_EV_UnitAuraRemoved, &args);
    }
}

namespace wxl::seam::Unit::SendMovementUpdate
{
    void WXL_CALL Before(void* self, uint32_t, uint32_t msg_id, float, uint32_t, uint64_t, uint8_t)
    {
        const bool start = IsMoveStart(msg_id);
        if (!start && !IsMoveStop(msg_id)) return;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !self) return;

        wxl_player_move_args args{};
        args.unit      = wxl::core::objects::HandleOf(self);
        args.unit_guid = wxl::core::binding::object_guid(self);
        args.opcode    = msg_id;
        args.flags     = wxl::core::binding::unit_movement_flags(self);
        wxl::core::binding::object_position(self, &args.position);
        wxl::core::binding::object_facing(self, &args.facing);
        if (start) host->Raise(WXL_EV_PlayerMoveStart, &args);
        else host->Raise(WXL_EV_PlayerMoveStop, &args);
    }
}

namespace wxl::seam::Unit::MountDisplayIDUpdateHandler
{
    void WXL_CALL Before(uint64_t guid, uint32_t, uint32_t, const void* old_value, void*)
    {
        constexpr uint32_t kMountDisplayId = 0x45; ///< UNIT_FIELD_MOUNTDISPLAYID
        wxl::host::ModHost* host = wxl::core::seams::gameplay::Listener({ WXL_EV_UnitMountChanged });
        if (!host || !old_value) return;
        void* unit = wxl::core::seams::gameplay::ObjectOf(guid);
        wxl_mount_args args{};
        if (!unit || !wxl::core::seams::gameplay::Field(unit, kMountDisplayId, args.display_id)) return;
        std::memcpy(&args.previous_display_id, old_value, sizeof args.previous_display_id);
        if (args.previous_display_id == args.display_id) return;
        args.unit      = wxl::core::objects::HandleOf(unit);
        args.unit_guid = guid;
        host->Raise(WXL_EV_UnitMountChanged, &args);
    }
}

namespace wxl::seam::Unit::PreAnimate
{
    void WXL_CALL After(void* self, void*)
    {
        // The game has set its own twist on the unit's head bone; a mod's turn goes over it.
        wxl::core::services::pose::PreAnimate(self);
    }
}
