// SPDX-License-Identifier: GPL-3.0-or-later
// The damage seams: OnUnitDamage from each combat-log entry of damage, after the client logged it.

#include "client/CGUnit_C/DamageArgs.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "runtime/objects/Objects.hpp"
#include "wxl/offsets/free/U.hpp"

namespace
{
    namespace ev     = wxl::events;
    namespace off    = wxl::offsets;
    namespace damage = wxl::client::damage;

    off::UnitCombatLogAttackRoundFn  g_origAttackRound  = nullptr;
    off::UnitCombatLogSpellDamageFn  g_origSpellDamage  = nullptr;
    off::UnitCombatLogSplitDamageFn  g_origSplitDamage  = nullptr;
    off::UnitCombatLogDamageShieldFn g_origDamageShield = nullptr;
    off::UnitCombatLogEnvironmentFn  g_origEnvironment  = nullptr;

    bool Listening() { return ev::Any(ev::Event::OnUnitDamage); }

    void Raise(ev::UnitDamageArgs args)
    {
        if (!damage::Carries(args)) return;
        args.source = wxl::runtime::objects::HandleOf(args.sourceGuid);
        args.target = wxl::runtime::objects::HandleOf(args.targetGuid);
        ev::EmitFor(ev::Event::OnUnitDamage, args.target, &args);
    }

    void __cdecl AttackRoundDetour(const void* info)
    {
        g_origAttackRound(info);
        if (info && Listening()) Raise(damage::FromAttackRound(*static_cast<const off::layout::ATTACKROUNDINFO*>(info)));
    }

    void __cdecl SpellDamageDetour(const uint64_t* source, const uint64_t* dest, int32_t spellId, int32_t amount,
                                   int32_t overkill, uint32_t school, int32_t absorbed, int32_t resisted,
                                   int32_t blocked, uint32_t hitInfo, uint8_t periodic, uint32_t timeOffset)
    {
        g_origSpellDamage(source, dest, spellId, amount, overkill, school, absorbed, resisted, blocked, hitInfo,
                          periodic, timeOffset);
        if (!Listening()) return;
        const damage::SpellEntry entry{ *source, *dest, spellId, amount, overkill, school, absorbed, resisted, blocked, hitInfo };
        Raise(damage::FromSpell(entry, periodic ? ev::DamageKind::Periodic : ev::DamageKind::Spell));
    }

    void __cdecl SplitDamageDetour(const uint64_t* source, const uint64_t* dest, int32_t spellId, int32_t amount,
                                   int32_t overkill, uint32_t school, int32_t absorbed, int32_t resisted,
                                   int32_t blocked, uint32_t hitInfo, uint32_t timeOffset)
    {
        g_origSplitDamage(source, dest, spellId, amount, overkill, school, absorbed, resisted, blocked, hitInfo, timeOffset);
        if (!Listening()) return;
        const damage::SpellEntry entry{ *source, *dest, spellId, amount, overkill, school, absorbed, resisted, blocked, hitInfo };
        Raise(damage::FromSpell(entry, ev::DamageKind::Split));
    }

    void __cdecl DamageShieldDetour(const uint64_t* source, const uint64_t* target, int32_t spellId, int32_t amount,
                                    uint32_t overkill, uint32_t schoolMask)
    {
        g_origDamageShield(source, target, spellId, amount, overkill, schoolMask);
        if (Listening()) Raise(damage::FromShield(*source, *target, spellId, amount, int32_t(overkill), schoolMask));
    }

    void __cdecl EnvironmentDetour(const uint64_t* guid, uint32_t type, int32_t amount, int32_t absorbed, int32_t resisted)
    {
        g_origEnvironment(guid, type, amount, absorbed, resisted);
        if (Listening()) Raise(damage::FromEnvironment(*guid, type, amount, absorbed, resisted));
    }

    bool InstallDamage()
    {
        using wxl::runtime::hookpoints::Attach;
        const bool round  = Attach("UnitCombatLogAttackRound", &AttackRoundDetour, &g_origAttackRound);
        const bool spell  = Attach("UnitCombatLogSpellDamage", &SpellDamageDetour, &g_origSpellDamage);
        const bool split  = Attach("UnitCombatLogSplitDamage", &SplitDamageDetour, &g_origSplitDamage);
        const bool shield = Attach("UnitCombatLogDamageShield", &DamageShieldDetour, &g_origDamageShield);
        const bool world  = Attach("UnitCombatLogEnvironment", &EnvironmentDetour, &g_origEnvironment);
        return round && spell && split && shield && world;
    }
}

WXL_REGISTER_FEATURE("unit-damage", true, InstallDamage)
