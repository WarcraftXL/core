// SPDX-License-Identifier: GPL-3.0-or-later
// Object lifetimes: added once, back from lazy cleanup as a new handle, removed with a live handle, then expired.

#include "tests/seams/Harness.hpp"

#include "wxl/offsets/free/G.hpp"

namespace harness
{
    namespace
    {
        void* g_updateObject = nullptr;

        void* __cdecl FakeGetUpdateObject(uint64_t, int32_t*) { return g_updateObject; }
    }

    void CheckLifetime()
    {
        Recorder<ev::ObjectLifeArgs> added, removed;
        added.Connect(ev::Event::OnObjectAdded);
        removed.Connect(ev::Event::OnObjectRemoved);

        FakeObject wolf(0xF130000001000001ull, 0x09);
        g_source.resident[0xF130000001000001ull] = &wolf;
        obj::Added(&wolf);
        obj::Added(&wolf);
        CHECK(added.count == 1 && added.last.guid == 0xF130000001000001ull && added.last.typeMask == 0x09);
        CHECK(added.last.pointer == &wolf && added.last.object.generation != 0);
        const WXL_Handle first = added.last.object;

        // The update path hands back resident objects all the time: nothing new, nothing raised.
        auto getUpdate = Detour("GetUpdateObject", &FakeGetUpdateObject);
        g_updateObject = &wolf;
        int32_t reenabled = 0;
        CHECK(getUpdate(0xF130000001000001ull, &reenabled) == &wolf && added.count == 1);

        // A handler about the object hears its removal while the handle still resolves.
        Recorder<ev::ObjectLifeArgs> aboutWolf;
        aboutWolf.Connect(ev::Event::OnObjectRemoved, first);
        obj::Removed(&wolf);
        CHECK(removed.count == 1 && Same(removed.last.object, first) && aboutWolf.count == 1);
        void* p = nullptr;
        CHECK(g_table.Resolve(first, 0xFFFFFFFFu, &p) == WXL_E_EXPIRED);
        CHECK(ev::Connected(aboutWolf.id) == WXL_E_EXPIRED); // ended with the handle

        // Freed later from the lazy-cleanup list: it was already removed, nothing more.
        obj::Removed(&wolf);
        CHECK(removed.count == 1);

        // Back in view with the same pointer: a new handle, announced from the update path.
        CHECK(getUpdate(0xF130000001000001ull, &reenabled) == &wolf);
        CHECK(added.count == 2 && !Same(added.last.object, first));
        obj::Removed(&wolf);
        CHECK(removed.count == 2 && g_table.Live() == 0);
        g_source.resident.clear();
    }
}
