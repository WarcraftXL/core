// SPDX-License-Identifier: GPL-3.0-or-later
// Detour chains over MinHook: several parties on one function, each calling the next.

#pragma once

#include <cstdint>

/// The detours on one address, chained (each one's original is the next detour) behind a `jmp [head]` thunk,
/// so a link is added or taken out with one store; each link records its owner for DropOwner.
namespace wxl::hook
{
    /// Chain position: lower runs first, closer to the caller. Equal values keep install order.
    constexpr int kDefaultPriority = 0;

    bool Init();

    /// Registers a detour in its target's chain; `*original` is kept pointing at the next link. The
    /// address is patched at EnableAll, so a bad one is reported there. A resident link stays when its owner is
    /// dropped, and the module holding its detour is pinned in the process.
    bool Install(const char* name, void* target, void* detour, void** original, int priority = kDefaultPriority,
                 bool resident = false);

    inline bool Install(const char* name, uintptr_t target, void* detour, void** original,
                        int priority = kDefaultPriority, bool resident = false)
    {
        return Install(name, reinterpret_cast<void*>(target), detour, original, priority, resident);
    }

    /// Patches every chain registered since the last call and enables them all.
    bool EnableAll();

    /// Takes an extension's links out of their chains, its resident ones excepted; the others and the
    /// function are called as if they had never been there. Answers how many.
    uint32_t DropOwner(uint32_t owner);

    /// How many resident links an extension holds.
    uint32_t Resident(uint32_t owner);
}
