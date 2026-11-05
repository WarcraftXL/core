// SPDX-License-Identifier: GPL-3.0-or-later
// OnWorldEnter after the active player's world-entry initialisation, OnWorldLeave before a world preload.

#include "tests/seams/Harness.hpp"

#include "wxl/offsets/CGPlayer_C.hpp"
#include "wxl/offsets/World.hpp"
#include "wxl/offsets/free/S.hpp"

#include <windows.h>

namespace harness
{
    namespace
    {
        namespace off = wxl::offsets;

        int g_entered = 0, g_preloaded = 0;
        int g_eventsWhenEntered = -1;
        Recorder<ev::WorldEnterArgs>* g_enters = nullptr;

        void __fastcall FakePostInitActivePlayer(void*, void*)
        {
            ++g_entered;
            g_eventsWhenEntered = g_enters ? g_enters->count : -1;
        }
        void __cdecl FakePreload(const void*, int32_t) { ++g_preloaded; }

        /// The client's map id global, made in this process at its own address.
        bool SetMapId(int32_t map)
        {
            const uintptr_t page = off::s_mapId & ~uintptr_t(0xFFF);
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(reinterpret_cast<void*>(page), &info, sizeof info);
            if (info.State != MEM_COMMIT &&
                !VirtualAlloc(reinterpret_cast<void*>(page), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return false;
            *reinterpret_cast<int32_t*>(off::s_mapId) = map;
            return true;
        }
    }

    void CheckWorld()
    {
        CHECK(SetMapId(571));
        Recorder<ev::WorldEnterArgs> enters;
        Recorder<ev::WorldLeaveArgs> leaves;
        enters.Connect(ev::Event::OnWorldEnter);
        leaves.Connect(ev::Event::OnWorldLeave);
        g_enters = &enters;

        // The login or a loading screen: raised once the client has set the player in the world.
        auto enterWorld = Detour("CGPlayer_C::PostInitActivePlayer",
                                 static_cast<off::CGPlayer_C::PostInitActivePlayerFn>(&FakePostInitActivePlayer));
        FakeObject player(0x42ull, 0x19);
        enterWorld(&player, nullptr);
        CHECK(g_entered == 1 && g_eventsWhenEntered == 0 && enters.count == 1 && enters.last.mapId == 571);

        // A preload (a cinematic, a transport) raises the leave only: it is not a world entry.
        auto preload = Detour("World::Preload", static_cast<off::World::PreloadFn>(&FakePreload));
        preload(nullptr, 0);
        CHECK(g_preloaded == 1 && leaves.count == 1 && leaves.last.mapId == 571 && enters.count == 1);
        g_enters = nullptr;
    }
}
