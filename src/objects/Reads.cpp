// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The reads of wxl.object that are more than a binding fact read as is.

#include "objects/Objects.hpp"

#include <cstdlib>
#include <cstring>

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

    bool ClientObjects::UnitReaction(void* unit, void* other, uint32_t& out) const
    {
        const int32_t reaction = binding::unit_reaction(unit, other);
        if (reaction < 0) return false;
        out = static_cast<uint32_t>(reaction);
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
}
