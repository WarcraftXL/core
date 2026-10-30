// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The reads of wxl.object that are more than a binding fact read as is.

#include "objects/Objects.hpp"

#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace
{
    /// Byte n of UNIT_FIELD_BYTES_0, from the low end: race, class, sex, power type.
    uint32_t Bytes0(void* object, unsigned byte)
    {
        return (wxl::core::binding::unit_bytes0(object) >> (8u * byte)) & 0xFFu;
    }
}

namespace wxl::core::objects
{
    // --- Any object ----------------------------------------------------------------------------

    wxl_guid ClientObjects::Guid(void* object) const { return binding::object_guid(object); }

    wxl_object_type ClientObjects::Type(void* object) const
    {
        const uint32_t id = binding::object_type_id(object);
        return id <= WXL_OBJECT_CORPSE ? static_cast<wxl_object_type>(id) : WXL_OBJECT_UNKNOWN;
    }

    bool ClientObjects::Name(void* object, const char*& out, uint32_t& size) const
    {
        const char* name = binding::object_name(object);
        if (!name) return false;
        out  = name;
        size = static_cast<uint32_t>(std::strlen(name));
        return true;
    }

    void* ClientObjects::LocalPlayer() const
    {
        return const_cast<void*>(binding::local_player_object());
    }

    void* ClientObjects::FindByGuid(wxl_guid guid) const
    {
        return const_cast<void*>(binding::object_by_guid(guid));
    }

    uint32_t* ClientObjects::Descriptor(void* object) const
    {
        return static_cast<uint32_t*>(binding::object_descriptor(object));
    }

    bool ClientObjects::WatchElement(wxl_object_type kind, uint32_t first, uint32_t dwords, bool local) const
    {
        return objects::WatchElement(kind, first, dwords, local);
    }

    bool ClientObjects::UnwatchElement(wxl_object_type kind, uint32_t first, uint32_t dwords, bool local) const
    {
        return objects::UnwatchElement(kind, first, dwords, local);
    }

    void ClientObjects::Report(const char* line) const
    {
        // Each refusal once: a mod that asks the same wrong thing every frame writes one line.
        static std::set<std::string> said;
        if (said.size() >= 256 || !said.insert(line).second) return;
        WLOG_WARN(kTag, "wxl.object: %s", line);
    }

    bool ClientObjects::Position(void* object, wxl_vec3& out) const
    {
        return binding::object_position(object, &out) == WXL_TRUE;
    }

    bool ClientObjects::Facing(void* object, float& out) const
    {
        return binding::object_facing(object, &out) == WXL_TRUE;
    }

    bool ClientObjects::Entry(void* object, uint32_t& out) const
    {
        out = binding::object_entry(object);
        return true;
    }

    bool ClientObjects::Scale(void* object, float& out) const
    {
        out = binding::object_scale(object);
        return true;
    }

    // --- Units ---------------------------------------------------------------------------------

    bool ClientObjects::UnitTarget(void* object, wxl_guid& out) const
    {
        out = binding::unit_target(object);
        return true;
    }

    bool ClientObjects::UnitPower(void* object, uint32_t index, uint32_t& out) const
    {
        return binding::unit_power(object, index, &out) == WXL_TRUE;
    }

    bool ClientObjects::UnitMaxPower(void* object, uint32_t index, uint32_t& out) const
    {
        return binding::unit_max_power(object, index, &out) == WXL_TRUE;
    }

    bool ClientObjects::UnitPowerType(void* object, uint32_t& out) const
    {
        out = Bytes0(object, 3);
        return true;
    }

    bool ClientObjects::UnitRace(void* object, uint32_t& out) const
    {
        out = Bytes0(object, 0);
        return true;
    }

    bool ClientObjects::UnitClass(void* object, uint32_t& out) const
    {
        out = Bytes0(object, 1);
        return true;
    }

    bool ClientObjects::UnitSex(void* object, uint32_t& out) const
    {
        out = Bytes0(object, 2);
        return true;
    }

    // --- Players -------------------------------------------------------------------------------

    bool ClientObjects::PlayerVisibleItem(void* object, uint32_t slot, uint32_t& out) const
    {
        int32_t id = 0;
        if (binding::player_visible_item_id(object, slot, &id) != WXL_TRUE) return false;
        out = static_cast<uint32_t>(std::abs(id));
        return true;
    }

    // --- Containers ----------------------------------------------------------------------------

    /// Bounded by the bag's own slot count, not by the size of the array the client keeps.
    bool ClientObjects::ContainerSlot(void* object, uint32_t index, wxl_guid& out) const
    {
        if (index >= binding::container_num_slots(object)) return false;
        return binding::container_slot(object, index, &out) == WXL_TRUE;
    }

    // --- Game objects --------------------------------------------------------------------------
    //
    // Bytes the client keeps signed; a mod is handed them as the unsigned values they are.

    bool ClientObjects::GameObjectState(void* object, uint32_t& out) const
    {
        out = static_cast<uint8_t>(binding::gameobject_state(object));
        return true;
    }

    bool ClientObjects::GameObjectType(void* object, uint32_t& out) const
    {
        out = static_cast<uint8_t>(binding::gameobject_type_id(object));
        return true;
    }

    bool ClientObjects::GameObjectArtKit(void* object, uint32_t& out) const
    {
        out = static_cast<uint8_t>(binding::gameobject_art_kit(object));
        return true;
    }

    // --- 0.1.17: the typed reads -----------------------------------------------------------

    namespace
    {
        /// The native unit a handle names, or null for anything else.
        void* UnitOf(wxl_handle handle)
        {
            uint32_t kind   = WXL_OBJECT_UNKNOWN;
            void*    native = Service().Handles().Resolve(handle, &kind);
            return native && (kind == WXL_OBJECT_UNIT || kind == WXL_OBJECT_PLAYER) ? native : nullptr;
        }

        /// Whether a time on the client's clock is still to come.
        bool Ahead(uint32_t when) { return static_cast<int32_t>(binding::os_time_ms() - when) < 0; }
    }

    bool ClientObjects::UnitTargetGuid(void* object, wxl_guid& out) const
    {
        out = binding::unit_target(object);
        return true;
    }

    bool ClientObjects::UnitReaction(void* object, uint32_t& out) const
    {
        const void* player = binding::local_player_object();
        if (!player) return false;
        const int32_t reaction = binding::unit_reaction(object, player);
        if (reaction < 0) return false;
        out = static_cast<uint32_t>(reaction);
        return true;
    }

    bool ClientObjects::UnitReactionTo(void* object, wxl_handle other, uint32_t& out) const
    {
        const void* unit = UnitOf(other);
        if (!unit) return false;
        const int32_t reaction = binding::unit_reaction(object, unit);
        if (reaction < 0) return false;
        out = static_cast<uint32_t>(reaction);
        return true;
    }

    bool ClientObjects::UnitIsMounted(void* object, wxl_bool& out) const
    {
        // CGUnit_C::IsMounted: a mount model, and not the client's dismount-pending flag.
        out = binding::unit_mount_model(object) > 0 && !(binding::unit_client_flags(object) & 0x10000000u)
                  ? WXL_TRUE
                  : WXL_FALSE;
        return true;
    }

    bool ClientObjects::UnitIsCasting(void* object, wxl_bool& out) const
    {
        // Script_UnitCastingInfo: a cast spell whose end is still to come.
        out = binding::unit_casting_spell_id(object) && Ahead(binding::unit_cast_end(object)) ? WXL_TRUE : WXL_FALSE;
        return true;
    }

    bool ClientObjects::UnitIsChanneling(void* object, wxl_bool& out) const
    {
        out = binding::unit_channel_spell_id(object) && Ahead(binding::unit_channel_end(object)) ? WXL_TRUE
                                                                                                : WXL_FALSE;
        return true;
    }

    bool ClientObjects::UnitAuraCount(void* object, uint32_t& out) const
    {
        out = binding::unit_aura_count(object);
        return true;
    }

    bool ClientObjects::UnitAura(void* object, uint32_t slot, wxl_aura& out) const
    {
        const void* aura = binding::unit_aura_at(object, slot);
        if (!aura) return false;
        out.spell_id   = static_cast<uint32_t>(binding::aura_spell_id(aura));
        out.caster     = binding::aura_caster(aura);
        out.stacks     = binding::aura_stacks(aura);
        out.level      = binding::aura_level(aura);
        out.flags      = binding::aura_flags(aura);
        out.duration   = binding::aura_duration(aura);
        out.expiration = static_cast<uint32_t>(binding::aura_end_time(aura));
        return true;
    }

    bool ClientObjects::UnitCreatureType(void* object, uint32_t& out) const
    {
        out = static_cast<uint32_t>(binding::unit_creature_type(object));
        return true;
    }

    bool ClientObjects::UnitCreatureFamily(void* object, uint32_t& out) const
    {
        out = binding::unit_creature_family(object);
        return true;
    }

    bool ClientObjects::UnitClassification(void* object, uint32_t& out) const
    {
        out = binding::unit_classification(object);
        return true;
    }

    bool ClientObjects::UnitKnowsSpell(void* object, uint32_t spell, wxl_bool& out) const
    {
        out = binding::unit_knows_spell(object, spell);
        return true;
    }

    bool ClientObjects::UnitSpeed(void* object, uint32_t type, float& out) const
    {
        return binding::unit_speed(object, type, &out) == WXL_TRUE;
    }

    bool ClientObjects::UnitIsSwimming(void* object, wxl_bool& out) const
    {
        out = (binding::unit_client_flags(object) & 0x200000u) ? WXL_TRUE : WXL_FALSE; // Script_IsSwimming
        return true;
    }

    bool ClientObjects::UnitIsOutdoors(void* object, wxl_bool& out) const
    {
        out = binding::unit_is_outdoors(object);
        return true;
    }

    // --- what the active player can do with it -----------------------------------------------

    bool ClientObjects::UnitCanAttack(void* object, wxl_bool& out) const
    {
        const void* player = binding::local_player_object();
        if (!player) return false;
        out = binding::unit_can_attack(player, object);
        return true;
    }

    bool ClientObjects::UnitCanInteract(void* object, wxl_bool& out) const
    {
        // As the speech cursor: the client's CanInteractNow, and something the NPC offers.
        const void* player = binding::local_player_object();
        if (!player) return false;
        out = binding::unit_npc_flags(object) != 0 && binding::unit_can_interact(player, object) ? WXL_TRUE : WXL_FALSE;
        return true;
    }

    bool ClientObjects::GameObjectCanUse(void* object, wxl_bool& out) const
    {
        if (!binding::local_player_object()) return false;
        out = binding::gameobject_can_use(object);
        return true;
    }

    // --- the active player's group -----------------------------------------------------------

    bool ClientObjects::PlayerPartySize(void*, uint32_t& out) const
    {
        out = 0;
        for (uint32_t slot = 0; slot < 4; ++slot)
            if (binding::party_member_guid(slot)) ++out;
        return true;
    }

    bool ClientObjects::PlayerPartyMemberGuid(void*, uint32_t slot, wxl_guid& out) const
    {
        if (slot >= 4) return false;
        out = binding::party_member_guid(slot);
        return true;
    }

    bool ClientObjects::PlayerRaidSize(void*, uint32_t& out) const
    {
        out = binding::raid_member_count();
        return true;
    }

    bool ClientObjects::PlayerRaidMemberGuid(void*, uint32_t slot, wxl_guid& out) const
    {
        if (slot >= 40) return false;
        out = binding::raid_member_guid(slot);
        return true;
    }

    // --- the item cache: false while the client has not received the item's record -----------

#define WXL_CORE_ITEM_STAT(Method, fact)                                     \
    bool ClientObjects::Method(void* object, uint32_t& out) const            \
    {                                                                        \
        const void* stats = binding::item_stats(object);                     \
        if (!stats) return false;                                            \
        out = static_cast<uint32_t>(binding::fact(stats));                   \
        return true;                                                         \
    }
    WXL_CORE_ITEM_STAT(ItemQuality, item_stats_quality)
    WXL_CORE_ITEM_STAT(ItemLevel, item_stats_level)
    WXL_CORE_ITEM_STAT(ItemRequiredLevel, item_stats_required_level)
    WXL_CORE_ITEM_STAT(ItemClass, item_stats_class)
    WXL_CORE_ITEM_STAT(ItemSubclass, item_stats_subclass)
    WXL_CORE_ITEM_STAT(ItemInventoryType, item_stats_inventory_type)
    WXL_CORE_ITEM_STAT(ItemSellPrice, item_stats_sell_price)
#undef WXL_CORE_ITEM_STAT

    bool ClientObjects::ItemMaxStack(void* object, uint32_t& out) const
    {
        const void* stats = binding::item_stats(object);
        if (!stats) return false;
        const int32_t stack = binding::item_stats_stackable(stats);
        out                 = stack > 0 ? static_cast<uint32_t>(stack) : 1u;
        return true;
    }
}
