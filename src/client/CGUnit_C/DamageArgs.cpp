// SPDX-License-Identifier: GPL-3.0-or-later

#include "client/CGUnit_C/DamageArgs.hpp"

#include <iterator>

namespace wxl::client::damage
{
    namespace
    {
        constexpr uint32_t kSpellHitCritical = 0x2;   ///< hitInfo bit of the spell and periodic entries
        constexpr uint32_t kRoundCritical    = 0x200; ///< ATTACKROUNDINFO::m_hitInfo bit
        constexpr uint32_t kRoundSchools     = 2;     ///< a melee round carries up to two sub-damages
    }

    events::UnitDamageArgs FromAttackRound(const offsets::layout::ATTACKROUNDINFO& round)
    {
        events::UnitDamageArgs args{};
        args.sourceGuid = round.m_attacker;
        args.targetGuid = round.m_target;
        args.kind       = events::DamageKind::Melee;
        args.amount     = round.m_damage;
        args.overkill   = round.m_overkill;
        args.schoolMask = round.m_schoolMask[0];
        // The client sums the sub-damages up to the first without a school.
        for (uint32_t i = 0; i < kRoundSchools && round.m_schoolMask[i]; ++i)
        {
            args.absorbed += static_cast<int32_t>(round.m_absorbed[i]);
            args.resisted += static_cast<int32_t>(round.m_resisted[i]);
        }
        args.blocked  = static_cast<int32_t>(round.m_blocked);
        args.critical = (round.m_hitInfo & kRoundCritical) != 0;
        return args;
    }

    events::UnitDamageArgs FromSpell(const SpellEntry& entry, events::DamageKind kind)
    {
        events::UnitDamageArgs args{};
        args.sourceGuid = entry.source;
        args.targetGuid = entry.target;
        args.kind       = kind;
        args.spellId    = entry.spellId;
        args.amount     = entry.damage;
        args.overkill   = entry.overkill;
        args.schoolMask = entry.school;
        args.absorbed   = entry.absorbed;
        args.resisted   = entry.resisted;
        args.blocked    = entry.blocked;
        args.critical   = (entry.hitInfo & kSpellHitCritical) != 0;
        return args;
    }

    events::UnitDamageArgs FromShield(uint64_t source, uint64_t target, int32_t spellId, int32_t damage,
                                      int32_t overkill, uint32_t schoolMask)
    {
        events::UnitDamageArgs args{};
        args.sourceGuid = source;
        args.targetGuid = target;
        args.kind       = events::DamageKind::Shield;
        args.spellId    = spellId;
        args.amount     = damage;
        args.overkill   = overkill;
        args.schoolMask = schoolMask;
        return args;
    }

    events::UnitDamageArgs FromEnvironment(uint64_t target, uint32_t type, int32_t damage, int32_t absorbed,
                                           int32_t resisted)
    {
        // The school bit of each environment type: the client's s_environmentalDamageSchools (0x00A2D394).
        constexpr uint32_t kSchools[] = { 0, 0, 0, 2, 3, 2 };
        events::UnitDamageArgs args{};
        args.targetGuid  = target;
        args.kind        = events::DamageKind::Environment;
        args.amount      = damage;
        args.schoolMask  = type < std::size(kSchools) ? 1u << kSchools[type] : 1u;
        args.absorbed    = absorbed;
        args.resisted    = resisted;
        args.environment = type;
        return args;
    }

    bool Carries(const events::UnitDamageArgs& args)
    {
        return args.amount || args.absorbed || args.resisted || args.blocked;
    }
}
