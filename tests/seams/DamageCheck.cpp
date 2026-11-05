// SPDX-License-Identifier: GPL-3.0-or-later
// OnUnitDamage from each combat-log damage entry: amounts, kinds, handles, and misses left out.

#include "tests/seams/Harness.hpp"

#include "wxl/offsets/ATTACKROUNDINFO.hpp"
#include "wxl/offsets/free/U.hpp"

namespace harness
{
    namespace
    {
        namespace off = wxl::offsets;

        int g_originals = 0;

        void __cdecl FakeRound(const void*) { ++g_originals; }
        void __cdecl FakeSpell(const uint64_t*, const uint64_t*, int32_t, int32_t, int32_t, uint32_t, int32_t, int32_t,
                               int32_t, uint32_t, uint8_t, uint32_t) { ++g_originals; }
        void __cdecl FakeSplit(const uint64_t*, const uint64_t*, int32_t, int32_t, int32_t, uint32_t, int32_t, int32_t,
                               int32_t, uint32_t, uint32_t) { ++g_originals; }
        void __cdecl FakeShield(const uint64_t*, const uint64_t*, int32_t, int32_t, uint32_t, uint32_t) { ++g_originals; }
        void __cdecl FakeEnvironment(const uint64_t*, uint32_t, int32_t, int32_t, int32_t) { ++g_originals; }

        constexpr uint64_t kHero = 0x42ull;
        constexpr uint64_t kWolf = 0xF130000001000001ull;
    }

    void CheckDamage()
    {
        auto round  = Detour("UnitCombatLogAttackRound", static_cast<off::UnitCombatLogAttackRoundFn>(&FakeRound));
        auto spell  = Detour("UnitCombatLogSpellDamage", static_cast<off::UnitCombatLogSpellDamageFn>(&FakeSpell));
        auto split  = Detour("UnitCombatLogSplitDamage", static_cast<off::UnitCombatLogSplitDamageFn>(&FakeSplit));
        auto shield = Detour("UnitCombatLogDamageShield", static_cast<off::UnitCombatLogDamageShieldFn>(&FakeShield));
        auto world  = Detour("UnitCombatLogEnvironment", static_cast<off::UnitCombatLogEnvironmentFn>(&FakeEnvironment));

        FakeObject hero(kHero, 0x19), wolf(kWolf, 0x09);
        g_source.resident[kHero] = &hero;
        g_source.resident[kWolf] = &wolf;
        WXL_Handle wolfHandle{};
        CHECK(g_table.FromGuid(kWolf, &wolfHandle) == WXL_OK);

        // Nothing listens: the originals run, nothing is built.
        off::layout::ATTACKROUNDINFO hit{};
        round(&hit);
        CHECK(g_originals == 1);

        Recorder<ev::UnitDamageArgs> all, onWolf;
        all.Connect(ev::Event::OnUnitDamage);
        onWolf.Connect(ev::Event::OnUnitDamage, wolfHandle);

        // A melee round: two sub-damages summed, the critical bit, the attacker and target handles.
        hit.m_attacker = kHero;
        hit.m_target = kWolf;
        hit.m_damage = 120;
        hit.m_overkill = 5;
        hit.m_hitInfo = 0x200;
        hit.m_schoolMask[0] = 1;
        hit.m_schoolMask[1] = 4;
        hit.m_absorbed[0] = 3;
        hit.m_absorbed[1] = 4;
        hit.m_resisted[1] = 9;
        hit.m_blocked = 7;
        round(&hit);
        CHECK(all.count == 1 && onWolf.count == 1 && all.last.kind == ev::DamageKind::Melee);
        CHECK(all.last.amount == 120 && all.last.overkill == 5 && all.last.schoolMask == 1 && all.last.critical);
        CHECK(all.last.absorbed == 7 && all.last.resisted == 9 && all.last.blocked == 7);
        CHECK(all.last.sourceGuid == kHero && all.last.source.generation != 0 && Same(all.last.target, wolfHandle));

        // A miss carries nothing: no event.
        off::layout::ATTACKROUNDINFO miss{};
        miss.m_attacker = kHero;
        miss.m_target = kWolf;
        round(&miss);
        CHECK(all.count == 1);

        // Spell and periodic entries, the critical bit of hitInfo.
        spell(&kHero, &kWolf, 133, 300, 0, 4, 10, 0, 0, 0x2, 0, 0);
        CHECK(all.count == 2 && all.last.kind == ev::DamageKind::Spell && all.last.spellId == 133 && all.last.critical);
        CHECK(all.last.amount == 300 && all.last.absorbed == 10 && all.last.schoolMask == 4);
        spell(&kHero, &kWolf, 172, 40, 0, 32, 0, 0, 0, 0, 1, 0);
        CHECK(all.count == 3 && all.last.kind == ev::DamageKind::Periodic && !all.last.critical);

        split(&kHero, &kWolf, 6229, 15, 0, 2, 0, 0, 0, 0, 0);
        CHECK(all.count == 4 && all.last.kind == ev::DamageKind::Split && all.last.amount == 15);

        // A damage shield: the shield's owner is the source, the attacker it struck the target.
        shield(&kWolf, &kHero, 26364, 21, 0, 8);
        CHECK(all.count == 5 && all.last.kind == ev::DamageKind::Shield && all.last.targetGuid == kHero);
        CHECK(onWolf.count == 4);

        // The environment: no source, the school from the client's table (lava: fire).
        world(&kHero, 3, 500, 0, 0);
        CHECK(all.count == 6 && all.last.kind == ev::DamageKind::Environment && all.last.environment == 3);
        CHECK(all.last.sourceGuid == 0 && all.last.source.generation == 0 && all.last.schoolMask == 4);
        world(&kHero, 6, 1000, 0, 0);
        CHECK(all.count == 7 && all.last.schoolMask == 1);

        // A unit out of view is reported by GUID with an empty handle.
        const uint64_t stranger = 0xF130000009000009ull;
        spell(&kHero, &stranger, 133, 30, 0, 4, 0, 0, 0, 0, 0, 0);
        CHECK(all.count == 8 && all.last.target.generation == 0 && all.last.targetGuid == stranger);
        CHECK(g_originals == 10);
        g_source.resident.clear();
    }
}
