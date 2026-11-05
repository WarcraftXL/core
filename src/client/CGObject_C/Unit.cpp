// SPDX-License-Identifier: GPL-3.0-or-later
// The object seams: OnObjectUpdate after an update message, OnObjectDestroy before a despawn, OnTargetChanged.

#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "wxl/offsets/free/Packet.hpp"
#include "wxl/offsets/free/Script.hpp"

namespace
{
    namespace ev  = wxl::events;
    namespace off = wxl::offsets;

    off::Packet_SMSG_DESTROY_OBJECTFn g_origObjectUpdate  = nullptr;
    off::Packet_SMSG_DESTROY_OBJECTFn g_origObjectDestroy = nullptr;
    off::Script_TargetUnitFn          g_origTargetUnit    = nullptr;

    /// One event per update message (a batch of created or updated objects), after it is parsed.
    int __cdecl ObjectUpdateDetour(void* context, int opcode, int message, void* packet)
    {
        const int result = g_origObjectUpdate(context, opcode, message, packet);
        const ev::ObjectUpdateArgs args{ packet, opcode };
        ev::Emit<ev::Event::OnObjectUpdate>(args);
        WLOG_INFO_ONCE("object: update stream active");
        return result;
    }

    /// Raised while the object is still resident; the packet holds its GUID.
    int __cdecl ObjectDestroyDetour(void* context, int opcode, int message, void* packet)
    {
        const ev::ObjectDestroyArgs args{ packet, opcode };
        ev::Emit<ev::Event::OnObjectDestroy>(args);
        WLOG_INFO_ONCE("object: destroy hook active");
        return g_origObjectDestroy(context, opcode, message, packet);
    }

    int __cdecl TargetUnitDetour(void* scriptState)
    {
        const int result = g_origTargetUnit(scriptState);
        const ev::TargetChangedArgs args{ scriptState };
        ev::Emit<ev::Event::OnTargetChanged>(args);
        WLOG_INFO_ONCE("target: hook live (first change)");
        return result;
    }

    bool InstallUnit()
    {
        wxl::runtime::hookpoints::Attach("Packet_SMSG_UPDATE_OBJECT", &ObjectUpdateDetour, &g_origObjectUpdate);
        wxl::runtime::hookpoints::Attach("Packet_SMSG_DESTROY_OBJECT", &ObjectDestroyDetour, &g_origObjectDestroy);
        wxl::runtime::hookpoints::Attach("Script_TargetUnit", &TargetUnitDetour, &g_origTargetUnit);
        return true;
    }
}

WXL_REGISTER_FEATURE("unit", true, InstallUnit)
