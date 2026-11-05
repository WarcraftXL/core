// SPDX-License-Identifier: GPL-3.0-or-later
// The doodad seam: OnDoodadSpawn with each doodad the client places from a map tile.

#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "wxl/offsets/CMap.hpp"

namespace
{
    namespace ev  = wxl::events;
    namespace off = wxl::offsets;

    off::CMap::CreateDoodadDef_7BECD0Fn g_origDoodadSpawn = nullptr;

    void* __cdecl DoodadSpawnDetour(const char* modelName, void* placement, const void* tileOrigin)
    {
        void* doodad = g_origDoodadSpawn(modelName, placement, tileOrigin);
        const ev::DoodadSpawnArgs args{ doodad };
        ev::Emit<ev::Event::OnDoodadSpawn>(args);
        return doodad;
    }

    bool InstallDoodadSpawn()
    {
        wxl::runtime::hookpoints::Attach("CMap::CreateDoodadDef_7BECD0", &DoodadSpawnDetour, &g_origDoodadSpawn);
        return true;
    }
}

WXL_REGISTER_FEATURE("doodad-spawn", true, InstallDoodadSpawn)
