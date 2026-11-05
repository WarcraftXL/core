// SPDX-License-Identifier: GPL-3.0-or-later
// OnUnitDamage's args from what each combat-log entry function receives (handles left empty).

#pragma once

#include "wxl/Events.hpp"
#include "wxl/offsets/ATTACKROUNDINFO.hpp"

#include <cstdint>

namespace wxl::client::damage
{
    /// The amounts of a spell, periodic or split damage entry, as UnitCombatLogSpellDamage takes them.
    struct SpellEntry
    {
        uint64_t source;
        uint64_t target;
        int32_t  spellId;
        int32_t  damage;
        int32_t  overkill;
        uint32_t school;
        int32_t  absorbed;
        int32_t  resisted;
        int32_t  blocked;
        uint32_t hitInfo;
    };

    events::UnitDamageArgs FromAttackRound(const offsets::layout::ATTACKROUNDINFO& round);
    events::UnitDamageArgs FromSpell(const SpellEntry& entry, events::DamageKind kind);
    events::UnitDamageArgs FromShield(uint64_t source, uint64_t target, int32_t spellId, int32_t damage,
                                      int32_t overkill, uint32_t schoolMask);
    events::UnitDamageArgs FromEnvironment(uint64_t target, uint32_t type, int32_t damage, int32_t absorbed,
                                           int32_t resisted);

    /// Whether the entry carries any damage, taken, absorbed, resisted or blocked: a miss carries none.
    bool Carries(const events::UnitDamageArgs& args);
}
