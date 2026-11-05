// SPDX-License-Identifier: GPL-3.0-or-later
// The unit seams: OnUnitDeath after the client ran a unit's death, OnUnitNameChanged after a name update.

#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "runtime/objects/Objects.hpp"
#include "wxl/game/Unit.hpp"
#include "wxl/game/World.hpp"
#include "wxl/offsets/CGGameUI.hpp"
#include "wxl/offsets/CGUnit_C.hpp"

namespace
{
    namespace ev    = wxl::events;
    namespace off   = wxl::offsets;
    namespace world = wxl::game::world;

    off::CGUnit_C::OnDeathFn        g_origOnDeath        = nullptr;
    off::CGGameUI::UnitNameUpdateFn g_origUnitNameUpdate = nullptr;

    void __fastcall OnDeathDetour(void* unit, void* edx)
    {
        g_origOnDeath(unit, edx);
        if (!ev::Any(ev::Event::OnUnitDeath)) return;
        const uint64_t guid = world::Guid(unit);
        const ev::UnitDeathArgs args{ wxl::runtime::objects::HandleOf(guid), guid };
        ev::EmitFor(ev::Event::OnUnitDeath, args.unit, &args);
    }

    /// The client raises UNIT_NAME_UPDATE here; units out of view raise nothing.
    void __cdecl UnitNameUpdateDetour(const uint64_t* guid)
    {
        g_origUnitNameUpdate(guid);
        if (!guid || !ev::Any(ev::Event::OnUnitNameChanged)) return;
        const WXL_Handle handle = wxl::runtime::objects::HandleOf(*guid);
        void* unit = wxl::runtime::objects::PointerOf(handle, world::kTypeMaskUnit);
        if (!unit) return;
        const ev::UnitNameArgs args{ handle, *guid, wxl::game::unit::Name(unit) };
        ev::EmitFor(ev::Event::OnUnitNameChanged, handle, &args);
    }

    bool InstallUnitState()
    {
        using wxl::runtime::hookpoints::Attach;
        const bool death = Attach("CGUnit_C::OnDeath", &OnDeathDetour, &g_origOnDeath);
        const bool name  = Attach("CGGameUI::UnitNameUpdate", &UnitNameUpdateDetour, &g_origUnitNameUpdate);
        return death && name;
    }
}

WXL_REGISTER_FEATURE("unit-state", true, InstallUnitState)
