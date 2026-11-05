// SPDX-License-Identifier: GPL-3.0-or-later
// Detours for the object manager's register-convention lifetime functions: InitObject, ObjDelete, DestroyRemovedObject.

#pragma once

namespace wxl::client::lifetime
{
    /// What the thunks call with the object; set before the hooks are attached.
    using ObjectFn = void(__cdecl*)(void* object);

    struct Thunks
    {
        ObjectFn onAdded   = nullptr; ///< after InitObject built the object
        ObjectFn onRemoved = nullptr; ///< before ObjDelete or DestroyRemovedObject lets it go

        void* initObject           = nullptr; ///< each one's next link, kept by the hook chain
        void* objDelete            = nullptr;
        void* destroyRemovedObject = nullptr;
    };

    /// The thunks' state: one per process.
    extern Thunks g_thunks;

    /// InitObject (object in ESI, create in EAX, time in ECX): runs it, then onAdded(object).
    void InitObjectAfter();
    /// ObjDelete (object in EAX): onRemoved(object), then runs it.
    void ObjDeleteBefore();
    /// DestroyRemovedObject (object in ESI): onRemoved(object), then runs it.
    void DestroyRemovedObjectBefore();
}
