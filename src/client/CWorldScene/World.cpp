// SPDX-License-Identifier: GPL-3.0-or-later
// The world seams: OnWorldLeave before a world preload, OnWorldEnter once the active player is in the world,
// OnUpdate once per frame.

#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "wxl/offsets/CGPlayer_C.hpp"
#include "wxl/offsets/FrameTime.hpp"
#include "wxl/offsets/World.hpp"
#include "wxl/offsets/free/S.hpp"

#include <cstdint>

namespace
{
    namespace ev  = wxl::events;
    namespace off = wxl::offsets;

    off::World::PreloadFn                     g_origPreload     = nullptr;
    off::CGPlayer_C::PostInitActivePlayerFn   g_origEnterWorld  = nullptr;
    off::FrameTime::UpdateFn                  g_origFrameTime   = nullptr;

    uint32_t CurrentMapId() { return static_cast<uint32_t>(*reinterpret_cast<int32_t*>(off::s_mapId)); }

    void __cdecl PreloadDetour(const void* position, int32_t showLoading)
    {
        const ev::WorldLeaveArgs leave{ CurrentMapId() };
        ev::Emit<ev::Event::OnWorldLeave>(leave);
        g_origPreload(position, showLoading);
    }

    /// The client runs it for the active player at every world entry: the login and each loading screen.
    /// World::Preload is not on that path for a plain login or teleport (cinematics and transports only).
    void __fastcall EnterWorldDetour(void* player, void* edx)
    {
        g_origEnterWorld(player, edx);
        const ev::WorldEnterArgs enter{ CurrentMapId() };
        ev::Emit<ev::Event::OnWorldEnter>(enter);
    }

    /// The frame delta and time are forwarded from the arguments, not read back from the globals.
    void __cdecl FrameTimeDetour(float deltaSeconds, uint32_t frameTimeMs)
    {
        g_origFrameTime(deltaSeconds, frameTimeMs);
        const ev::UpdateArgs args{ deltaSeconds, frameTimeMs };
        ev::Emit<ev::Event::OnUpdate>(args);
    }

    bool InstallWorld()
    {
        wxl::runtime::hookpoints::Attach("World::Preload", &PreloadDetour, &g_origPreload);
        wxl::runtime::hookpoints::Attach("CGPlayer_C::PostInitActivePlayer", &EnterWorldDetour, &g_origEnterWorld);
        wxl::runtime::hookpoints::Attach("FrameTime::Update", &FrameTimeDetour, &g_origFrameTime);
        return true;
    }
}

WXL_REGISTER_FEATURE("world", true, InstallWorld)
