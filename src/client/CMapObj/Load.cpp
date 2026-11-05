// SPDX-License-Identifier: GPL-3.0-or-later
// The map-object seams: OnWmoRootLoad and OnWmoGroupLoad while their buffers still hold the bytes read.

#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "wxl/offsets/CMapObj.hpp"
#include "wxl/offsets/CMapObjGroup.hpp"

namespace
{
    namespace ev  = wxl::events;
    namespace off = wxl::offsets;

    off::CMapObj::PostloadCallbackFn g_origRootLoaded = nullptr;
    off::CMapObjGroup::CreateFn      g_origGroupRead  = nullptr;

    /// The only caller of the root's chunk walker: raised before it runs, on the raw bytes.
    void __cdecl RootLoadedDetour(void* root)
    {
        const ev::WmoRootLoadArgs args{ root };
        ev::Emit<ev::Event::OnWmoRootLoad>(args);
        g_origRootLoaded(root);
    }

    /// The group reader joins the synchronous and asynchronous loads, so every group passes here.
    void __fastcall GroupReadDetour(void* group, void* edx)
    {
        const ev::WmoGroupLoadArgs args{ group };
        ev::Emit<ev::Event::OnWmoGroupLoad>(args);
        g_origGroupRead(group, edx);
    }

    bool InstallWmoLoad()
    {
        wxl::runtime::hookpoints::Attach("CMapObj::PostloadCallback", &RootLoadedDetour, &g_origRootLoaded);
        wxl::runtime::hookpoints::Attach("CMapObjGroup::Create", &GroupReadDetour, &g_origGroupRead);
        return true;
    }
}

WXL_REGISTER_FEATURE("wmo-load", true, InstallWmoLoad)
