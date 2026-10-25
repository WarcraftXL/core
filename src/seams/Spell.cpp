// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Spell seams: what the runtime does as the server reports casts, channels and cooldowns,
// and as the local player asks for a cast or cancels one, on both builds.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"
#include "seams/Message.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstddef>
#include <cstdint>

namespace
{
    namespace binding = wxl::core::binding;
    namespace objects = wxl::core::objects;
    using wxl::core::seams::MessageReader;

    /// The opcodes the shared handlers serve, as the client numbers them.
    constexpr uint32_t kSpellStart     = 0x131; ///< SMSG_SPELL_START
    constexpr uint32_t kSpellGo        = 0x132; ///< SMSG_SPELL_GO
    constexpr uint32_t kCooldownEvent  = 0x135; ///< SMSG_COOLDOWN_EVENT
    constexpr uint32_t kClearCooldown  = 0x1DE; ///< SMSG_CLEAR_COOLDOWN
    constexpr uint32_t kModifyCooldown = 0x491; ///< SMSG_MODIFY_COOLDOWN

    /// The bits of the server's target flags that put an object target, as a packed guid,
    /// first in the target block: a unit, a unit's minipet, a game object, a corpse.
    constexpr uint32_t kObjectTargetFlags = 0x18A02;

    /// The miss reason the server follows with a reflect result byte.
    constexpr uint8_t kMissReflect = 11;

    /// The most targets one SMSG_SPELL_GO can list in either of its lists: a byte count.
    constexpr size_t kMaxTargets = 255;

    /// The bits of the target flags for an item target (a packed guid), a source point and a
    /// destination point (a packed transport guid and three floats each).
    constexpr uint32_t kItemTargetFlags = 0x110;
    constexpr uint32_t kSourceLocation  = 0x20;
    constexpr uint32_t kDestLocation    = 0x40;

    bool ReadPoint(MessageReader& in, wxl_vec3& out)
    {
        uint64_t transport = 0;
        return in.PackedGuid(transport) && in.F32(out.x) && in.F32(out.y) && in.F32(out.z);
    }

    /// The target block both cast messages end with: the flags, the object target, the item
    /// target, the source and the destination point, each when the flags name it. A block cut
    /// short keeps what was read: the object target is all older readers wanted.
    bool ReadTarget(MessageReader& in, wxl_spell_cast_args& args)
    {
        if (!in.U32(args.target_flags)) return false;
        if ((args.target_flags & kObjectTargetFlags) && !in.PackedGuid(args.target_guid)) return false;
        uint64_t item = 0;
        if ((args.target_flags & kItemTargetFlags) && !in.PackedGuid(item)) return true;
        if (args.target_flags & kSourceLocation)
        {
            if (!ReadPoint(in, args.source)) return true;
            args.has_source = WXL_TRUE;
        }
        if (args.target_flags & kDestLocation)
        {
            if (!ReadPoint(in, args.dest)) return true;
            args.has_dest = WXL_TRUE;
        }
        return true;
    }

    /// A handle for a guid the server named: the object's when the client holds it.
    wxl_handle HandleFor(uint64_t guid)
    {
        return guid ? objects::FromGuid(guid) : WXL_HANDLE_NONE;
    }

    /// The client object behind a handle, for a binding read; null when it is not in view.
    void* ObjectOf(wxl_handle handle)
    {
        return handle == WXL_HANDLE_NONE ? nullptr : objects::Service().Handles().Resolve(handle);
    }
}

namespace wxl::seam::Spell::Packet_Group_30
{
    void WXL_CALL Before(void* param, uint32_t msg_id, uint32_t time, void* msg)
    {
        (void)param;
        (void)time;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        const bool start = msg_id == kSpellStart;
        if (!start && msg_id != kSpellGo) return;

        // The header both messages share.
        MessageReader       in(msg);
        wxl_spell_cast_args args{};
        uint8_t             count = 0;
        if (!in.PackedGuid(args.cast_item) || !in.PackedGuid(args.caster_guid) || !in.U8(count) ||
            !in.U32(args.spell_id) || !in.U32(args.cast_flags))
            return;
        args.cast_count = count;

        uint64_t missGuids[kMaxTargets];
        uint8_t  missReasons[kMaxTargets];
        if (start)
        {
            if (!in.U32(args.cast_time)) return;
        }
        else
        {
            // The server's time, the hits as full guids, the misses as a full guid and a
            // reason each, with a reflect result after a reflected one. The hits are handed
            // over as they lie in the message; the misses are gathered, the reflect results
            // left out.
            uint32_t serverTime = 0;
            uint8_t  hits       = 0;
            uint8_t  misses     = 0;
            if (!in.U32(serverTime) || !in.U8(hits)) return;
            const uint8_t* hitBytes = in.At();
            if (!in.Skip(hits * 8u) || !in.U8(misses)) return;
            for (uint8_t i = 0; i < misses; ++i)
            {
                if (!in.U64(missGuids[i]) || !in.U8(missReasons[i])) return;
                if (missReasons[i] == kMissReflect && !in.Skip(1)) return;
            }
            args.hit_count    = hits;
            args.miss_count   = misses;
            args.hits         = hits ? hitBytes : nullptr;
            args.hits_size    = hits * 8u;
            args.misses       = misses ? missGuids : nullptr;
            args.misses_size  = misses * 8u;
            args.miss_reasons = misses ? missReasons : nullptr;
        }
        if (!ReadTarget(in, args)) return;

        args.caster = HandleFor(args.caster_guid);
        args.target = HandleFor(args.target_guid);
        if (start) host->Raise(WXL_EV_SpellCastStart, &args);
        else host->Raise(WXL_EV_SpellCastGo, &args);
    }
}

namespace wxl::seam::Spell::Packet_SMSG_SPELL_FAILURE
{
    void WXL_CALL Before(void* param, uint32_t msg_id, uint32_t time, void* msg)
    {
        (void)param;
        (void)msg_id;
        (void)time;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        MessageReader       in(msg);
        wxl_spell_fail_args args{};
        uint8_t             count  = 0;
        uint8_t             reason = 0;
        if (!in.PackedGuid(args.caster_guid) || !in.U8(count) || !in.U32(args.spell_id) ||
            !in.U8(reason))
            return;
        args.cast_count   = count;
        args.reason       = reason;
        args.custom_error = -1;
        args.custom_arg   = -1;
        args.caster       = HandleFor(args.caster_guid);
        host->Raise(WXL_EV_SpellCastInterrupted, &args);
    }
}

namespace wxl::seam::Spell::SpellFailed
{
    void WXL_CALL Before(void* cast, const void* spell, int32_t reason, int32_t misc1,
                         int32_t misc2, uint8_t reset_cooldown)
    {
        (void)spell;
        (void)reset_cooldown;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !cast) return;

        wxl_spell_fail_args args{};
        args.caster_guid  = binding::pending_cast_caster(cast);
        args.caster       = HandleFor(args.caster_guid);
        args.spell_id     = binding::pending_cast_spell_id(cast);
        args.cast_count   = binding::pending_cast_count(cast);
        args.reason       = static_cast<uint32_t>(reason);
        args.custom_error = misc1;
        args.custom_arg   = misc2;
        host->Raise(WXL_EV_SpellCastFailed, &args);
    }
}

namespace wxl::seam::Spell::CastSpell
{
    bool WXL_CALL Gate(uint8_t* result, void** caster, int32_t* spell_id, const void** item,
                       uint64_t* target, void** cast, uint8_t* confirmed)
    {
        (void)cast;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return false;

        wxl_bool               refused = WXL_FALSE;
        wxl_spell_request_args args{};
        args.caster      = *caster ? objects::HandleOf(*caster) : WXL_HANDLE_NONE;
        args.spell_id    = static_cast<uint32_t>(*spell_id);
        args.item        = *item ? objects::HandleOf(const_cast<void*>(*item)) : WXL_HANDLE_NONE;
        args.target_guid = *target;
        args.confirmed   = *confirmed ? WXL_TRUE : WXL_FALSE;
        args.refused     = &refused;

        host->Raise(WXL_EV_SpellCastRequest, &args);
        if (!refused) return false;

        // What the client answers for a spell it cannot cast: false, nothing sent, nothing
        // pending.
        *result = 0;
        return true;
    }
}

namespace wxl::seam::Spell::Packet_MSG_CHANNEL_START
{
    void WXL_CALL Before(void* param, uint32_t msg_id, uint32_t time, void* msg)
    {
        (void)param;
        (void)msg_id;
        (void)time;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        MessageReader           in(msg);
        wxl_spell_progress_args args{};
        uint32_t                duration = 0;
        if (!in.PackedGuid(args.caster_guid) || !in.U32(args.spell_id) || !in.U32(duration))
            return;
        // A length of zero or less is a channel the client never shows.
        if (static_cast<int32_t>(duration) <= 0) return;

        args.caster    = HandleFor(args.caster_guid);
        args.duration  = duration;
        args.remaining = duration;
        host->Raise(WXL_EV_SpellChannelStart, &args);
    }
}

namespace wxl::seam::Spell::Packet_MSG_CHANNEL_UPDATE
{
    void WXL_CALL Before(void* param, uint32_t msg_id, uint32_t time, void* msg)
    {
        (void)param;
        (void)msg_id;
        (void)time;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        MessageReader           in(msg);
        wxl_spell_progress_args args{};
        if (!in.PackedGuid(args.caster_guid) || !in.U32(args.remaining)) return;

        args.caster = HandleFor(args.caster_guid);
        if (void* unit = ObjectOf(args.caster)) args.spell_id = binding::unit_channel_spell_id(unit);
        if (args.remaining) host->Raise(WXL_EV_SpellChannelUpdate, &args);
        else host->Raise(WXL_EV_SpellChannelStop, &args);
    }
}

namespace wxl::seam::Spell::Packet_SMSG_UNIT_SPELLCAST_START
{
    void WXL_CALL Before(void* param, uint32_t msg_id, uint32_t time, void* msg)
    {
        (void)param;
        (void)msg_id;
        (void)time;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        MessageReader           in(msg);
        wxl_spell_progress_args args{};
        if (!in.PackedGuid(args.caster_guid) || !in.PackedGuid(args.target_guid) ||
            !in.U32(args.spell_id) || !in.U32(args.remaining) || !in.U32(args.duration))
            return;

        args.caster = HandleFor(args.caster_guid);
        args.target = HandleFor(args.target_guid);
        host->Raise(WXL_EV_SpellCastResume, &args);
    }
}

namespace wxl::seam::Spell::Packet_SMSG_SPELL_DELAYED
{
    void WXL_CALL Before(void* param, uint32_t msg_id, uint32_t time, void* msg)
    {
        (void)param;
        (void)msg_id;
        (void)time;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        MessageReader        in(msg);
        wxl_spell_delay_args args{};
        uint32_t             delay = 0;
        if (!in.PackedGuid(args.caster_guid) || !in.U32(delay)) return;

        args.delay  = static_cast<int32_t>(delay);
        args.caster = HandleFor(args.caster_guid);
        if (void* unit = ObjectOf(args.caster)) args.spell_id = binding::unit_casting_spell_id(unit);
        host->Raise(WXL_EV_SpellCastDelayed, &args);
    }
}

namespace wxl::seam::Spell::Packet_SMSG_SPELL_COOLDOWN
{
    void WXL_CALL Before(void* param, uint32_t msg_id, uint32_t time, void* msg)
    {
        (void)param;
        (void)msg_id;
        (void)time;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        MessageReader           in(msg);
        wxl_spell_cooldown_args args{};
        uint8_t                 flags = 0;
        if (!in.U64(args.unit_guid) || !in.U8(flags)) return;
        args.flags = flags;
        args.unit  = HandleFor(args.unit_guid);

        // A spell and its duration per pair, to the end of the message; a trailing part of a
        // pair is not one.
        while (in.Left() >= 8)
        {
            if (!in.U32(args.spell_id) || !in.U32(args.duration)) return;
            host->Raise(WXL_EV_SpellCooldown, &args);
        }
    }
}

namespace wxl::seam::Spell::Packet_SMSG_ITEM_COOLDOWN
{
    void WXL_CALL Before(void* param, uint32_t msg_id, uint32_t time, void* msg)
    {
        (void)param;
        (void)msg_id;
        (void)time;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        MessageReader           in(msg);
        wxl_spell_cooldown_args args{};
        if (!in.U64(args.item_guid) || !in.U32(args.spell_id)) return;

        args.item = HandleFor(args.item_guid);
        host->Raise(WXL_EV_SpellCooldown, &args);
    }
}

namespace wxl::seam::Spell::Packet_Group_31
{
    void WXL_CALL Before(void* param, uint32_t msg_id, uint32_t time, void* msg)
    {
        (void)param;
        (void)time;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        MessageReader                  in(msg);
        wxl_spell_cooldown_change_args args{};
        if (!in.U32(args.spell_id) || !in.U64(args.unit_guid)) return;

        switch (msg_id)
        {
        case kCooldownEvent: args.change = 0; break;
        case kClearCooldown: args.change = 1; break;
        case kModifyCooldown:
        {
            uint32_t delay = 0;
            if (!in.U32(delay)) return;
            args.change = 2;
            args.delay  = static_cast<int32_t>(delay);
            break;
        }
        default: return;
        }

        args.unit = HandleFor(args.unit_guid);
        host->Raise(WXL_EV_SpellCooldownChanged, &args);
    }
}

namespace wxl::seam::Spell::CancelSpell
{
    void WXL_CALL Before(void* cast, uint8_t stop_recast, uint8_t notify_server, int32_t reason)
    {
        (void)stop_recast;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !cast) return;

        wxl_spell_cancel_args args{};
        args.caster_guid   = binding::pending_cast_caster(cast);
        args.caster        = HandleFor(args.caster_guid);
        args.spell_id      = binding::pending_cast_spell_id(cast);
        args.cast_count    = binding::pending_cast_count(cast);
        args.reason        = static_cast<uint32_t>(reason);
        args.notify_server = notify_server ? WXL_TRUE : WXL_FALSE;
        host->Raise(WXL_EV_SpellCastCancel, &args);
    }
}
