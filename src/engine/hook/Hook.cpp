// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/hook/Hook.hpp"
#include "common/Log.hpp"
#include "runtime/Owners.hpp"

#include <MinHook.h>
#include <windows.h>

#include <cstring>
#include <deque>
#include <iterator>
#include <vector>

namespace wxl::hook
{
    namespace
    {
        /// One party on an address. `original` is the party's own variable; the chain rewrites it
        /// whenever its shape changes.
        struct Link
        {
            const char* name;
            void*       detour;
            void**      original;
            int         priority;
            uint32_t    owner;
            bool        resident;
        };

        /// What MinHook jumps to: `jmp [target]`, so the head of a live chain changes with one aligned store.
        struct Thunk
        {
            uint8_t        code[8];
            void* volatile target;
            uint32_t       pad;
        };
        static_assert(sizeof(Thunk) == 16, "Thunk");

        constexpr size_t  kPageSize      = 4096;
        constexpr size_t  kThunksPerPage = kPageSize / sizeof(Thunk);
        constexpr uint8_t kJmpIndirect[] = { 0xFF, 0x25 }; ///< jmp dword ptr [abs32]
        constexpr uint8_t kInt3          = 0xCC;

        Thunk* NewThunk()
        {
            static Thunk* page = nullptr;
            static size_t used = 0;
            if (!page || used == kThunksPerPage)
            {
                page = static_cast<Thunk*>(VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
                used = 0;
                if (!page) return nullptr;
            }
            Thunk*         t    = &page[used++];
            const uint32_t slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&t->target));
            std::memcpy(&t->code[0], kJmpIndirect, sizeof kJmpIndirect);
            std::memcpy(&t->code[2], &slot, sizeof slot);
            t->code[6] = kInt3;
            t->code[7] = kInt3;
            return t;
        }

        /// Every party on one address, head first. MinHook only ever sees the thunk.
        struct Chain
        {
            void*             target;
            std::vector<Link> links;
            void*             trampoline; ///< valid once patched
            Thunk*            thunk;      ///< made at patch time
            bool              patched;
        };

        /// A deque: a Chain* stays valid while chains are added.
        std::deque<Chain>& Chains()
        {
            static std::deque<Chain> chains;
            return chains;
        }

        Chain* FindChain(void* target)
        {
            for (Chain& chain : Chains())
                if (chain.target == target) return &chain;
            return nullptr;
        }

        Chain& ChainFor(void* target)
        {
            if (Chain* existing = FindChain(target)) return *existing;
            Chains().push_back(Chain{target, {}, nullptr, nullptr, false});
            return Chains().back();
        }

        /// Points every party at its successor, the last at the function, the head at the first. One
        /// aligned store each, successors first: a call in flight reads the old link or the new one.
        void Relink(Chain& chain)
        {
            const size_t count = chain.links.size();
            for (size_t i = 0; i + 1 < count; ++i)
                *chain.links[i].original = chain.links[i + 1].detour;

            if (!chain.patched) return;
            if (count) *chain.links[count - 1].original = chain.trampoline;
            chain.thunk->target = count ? chain.links.front().detour : chain.trampoline;
        }

        /// Hands the thunk to MinHook and closes the chain on the returned trampoline.
        bool Patch(Chain& chain)
        {
            if (chain.patched) return true;

            chain.thunk = NewThunk();
            if (!chain.thunk)
            {
                WLOG_ERROR("hook: no memory for the head of @0x%p", chain.target);
                return false;
            }
            const MH_STATUS status = MH_CreateHook(chain.target, chain.thunk, &chain.trampoline);
            if (status != MH_OK)
            {
                for (const Link& link : chain.links)
                    WLOG_ERROR("hook: create '%s' @0x%p failed (%s)", link.name, chain.target,
                               MH_StatusToString(status));
                return false;
            }

            chain.patched = true;
            Relink(chain);
            return true;
        }
    }

    bool Init()
    {
        MH_STATUS status = MH_Initialize();
        if (status != MH_OK)
        {
            WLOG_ERROR("hook: MH_Initialize failed (%s)", MH_StatusToString(status));
            return false;
        }
        return true;
    }

    bool Install(const char* name, void* target, void* detour, void** original, int priority, bool resident)
    {
        // A resident detour outlives its owner: the module holding it may never be unloaded.
        HMODULE module = nullptr;
        if (resident && !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                            static_cast<LPCSTR>(detour), &module))
        {
            WLOG_ERROR("hook: '%s' @0x%p: no module holds its detour, so it cannot be resident", name, target);
            return false;
        }
        Chain& chain = ChainFor(target);

        auto at = chain.links.end();
        while (at != chain.links.begin() && std::prev(at)->priority > priority) --at;

        chain.links.insert(at, Link{name, detour, original, priority, runtime::owners::Current(), resident});
        Relink(chain);

        WLOG_DEBUG("hook: installed '%s' @0x%p (%zu on this address)", name, target,
                   chain.links.size());
        return true;
    }

    uint32_t DropOwner(uint32_t owner)
    {
        uint32_t dropped = 0;
        for (Chain& chain : Chains())
        {
            const size_t before = chain.links.size();
            // A dropped link keeps its own original: a call in flight through it goes on.
            for (auto it = chain.links.begin(); it != chain.links.end();)
                it = it->owner == owner && !it->resident ? chain.links.erase(it) : std::next(it);
            if (chain.links.size() == before) continue;
            dropped += uint32_t(before - chain.links.size());
            Relink(chain);
        }
        return dropped;
    }

    uint32_t Resident(uint32_t owner)
    {
        uint32_t kept = 0;
        for (const Chain& chain : Chains())
            for (const Link& link : chain.links) kept += link.owner == owner && link.resident;
        return kept;
    }

    bool EnableAll()
    {
        bool patched = true;
        for (Chain& chain : Chains())
        {
            if (chain.links.empty()) continue;
            if (!Patch(chain)) patched = false;
        }

        MH_STATUS status = MH_EnableHook(MH_ALL_HOOKS);
        if (status != MH_OK)
        {
            WLOG_ERROR("hook: enable all failed (%s)", MH_StatusToString(status));
            return false;
        }
        return patched;
    }
}
