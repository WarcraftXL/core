// SPDX-License-Identifier: GPL-3.0-or-later
// The object lifetime seams: an object joins the object manager or leaves it, its handle made or expired.

#include "client/CGObject_C/LifetimeThunks.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "runtime/objects/Objects.hpp"
#include "wxl/offsets/free/G.hpp"

namespace
{
    namespace off      = wxl::offsets;
    namespace lifetime = wxl::client::lifetime;
    namespace objects  = wxl::runtime::objects;

    off::GetUpdateObjectFn g_origGetUpdateObject = nullptr;

    void __cdecl OnAdded(void* object) { objects::Added(object); }
    void __cdecl OnRemoved(void* object) { objects::Removed(object); }

    /// An object back from the lazy-cleanup list returns here with the pointer it had.
    void* __cdecl GetUpdateObjectDetour(uint64_t guid, int32_t* reenabled)
    {
        void* object = g_origGetUpdateObject(guid, reenabled);
        if (object) objects::Added(object);
        return object;
    }

    bool Attach(const char* point, void (*thunk)(), void** original)
    {
        return wxl::runtime::hookpoints::AttachByName(point, reinterpret_cast<void*>(thunk), original,
                                                      wxl::hook::kDefaultPriority) != 0;
    }

    bool InstallLifetime()
    {
        lifetime::g_thunks.onAdded   = &OnAdded;
        lifetime::g_thunks.onRemoved = &OnRemoved;
        const bool ok = Attach("InitObject", &lifetime::InitObjectAfter, &lifetime::g_thunks.initObject)
                     && Attach("ObjDelete", &lifetime::ObjDeleteBefore, &lifetime::g_thunks.objDelete)
                     && Attach("DestroyRemovedObject", &lifetime::DestroyRemovedObjectBefore,
                               &lifetime::g_thunks.destroyRemovedObject);
        return wxl::runtime::hookpoints::Attach("GetUpdateObject", &GetUpdateObjectDetour, &g_origGetUpdateObject) && ok;
    }
}

WXL_REGISTER_FEATURE("object-lifetime", true, InstallLifetime)
