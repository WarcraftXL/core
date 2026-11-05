// SPDX-License-Identifier: GPL-3.0-or-later
// The register-convention thunks: the original sees its registers, the handler the object, the caller its registers.

#include "tests/seams/Harness.hpp"

#include "client/CGObject_C/LifetimeThunks.hpp"
#include "wxl/abi/Common.h"

namespace harness
{
    namespace
    {
        namespace lifetime = wxl::client::lifetime;

        // What the fake originals saw, and what the caller found after the thunk.
        uint32_t g_seenEsi = 0, g_seenEax = 0, g_seenEcx = 0;
        uint32_t g_afterEax = 0, g_afterEcx = 0, g_afterEbx = 0, g_afterEsi = 0, g_afterEdi = 0;
        constexpr uint32_t kOriginalEax = 0xDEAD0001u;
        constexpr uint32_t kOriginalEcx = 0xBEEF0002u;

        WXL_NAKED void FakeInitObject()
        {
            __asm
            {
                mov g_seenEsi, esi
                mov g_seenEax, eax
                mov g_seenEcx, ecx
                mov eax, 0xDEAD0001
                mov ecx, 0xBEEF0002
                ret
            }
        }

        WXL_NAKED void FakeObjDelete()
        {
            __asm
            {
                mov g_seenEax, eax
                ret
            }
        }

        WXL_NAKED void FakeDestroyRemovedObject()
        {
            __asm
            {
                mov g_seenEsi, esi
                ret
            }
        }

        /// Calls a thunk with ESI, EAX and ECX loaded, EBX and EDI as sentinels, and records them after.
        __declspec(noinline) void CallThunk(void (*thunk)(), uint32_t esiValue, uint32_t eaxValue, uint32_t ecxValue)
        {
            __asm
            {
                push ebx
                push esi
                push edi
                mov edx, thunk
                mov esi, esiValue
                mov eax, eaxValue
                mov ecx, ecxValue
                mov ebx, 0x5EB05EB0
                mov edi, 0x5ED05ED0
                call edx
                mov g_afterEax, eax
                mov g_afterEcx, ecx
                mov g_afterEbx, ebx
                mov g_afterEsi, esi
                mov g_afterEdi, edi
                pop edi
                pop esi
                pop ebx
            }
        }

        uint32_t Bits(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
    }

    void CheckThunks()
    {
        CHECK(g_points["InitObject"].detour == reinterpret_cast<void*>(&lifetime::InitObjectAfter));
        *g_points["InitObject"].original           = reinterpret_cast<void*>(&FakeInitObject);
        *g_points["ObjDelete"].original            = reinterpret_cast<void*>(&FakeObjDelete);
        *g_points["DestroyRemovedObject"].original = reinterpret_cast<void*>(&FakeDestroyRemovedObject);

        Recorder<ev::ObjectLifeArgs> added, removed;
        added.Connect(ev::Event::OnObjectAdded);
        removed.Connect(ev::Event::OnObjectRemoved);
        FakeObject boar(0xF130000002000002ull, 0x09);
        g_source.resident[0xF130000002000002ull] = &boar;

        // After: the original runs with ESI/EAX/ECX as given, the handler gets ESI, the caller keeps
        // what the original left in EAX/ECX and its own EBX/ESI/EDI.
        CallThunk(&lifetime::InitObjectAfter, Bits(&boar), 0x1111u, 0x2222u);
        CHECK(g_seenEsi == Bits(&boar) && g_seenEax == 0x1111u && g_seenEcx == 0x2222u);
        CHECK(added.count == 1 && added.last.pointer == &boar);
        CHECK(g_afterEax == kOriginalEax && g_afterEcx == kOriginalEcx);
        CHECK(g_afterEbx == 0x5EB05EB0u && g_afterEsi == Bits(&boar) && g_afterEdi == 0x5ED05ED0u);

        // Before, object in EAX: the handler runs first, the original still gets EAX.
        g_seenEax = 0;
        CallThunk(&lifetime::ObjDeleteBefore, 0x3333u, Bits(&boar), 0x4444u);
        CHECK(removed.count == 1 && removed.last.pointer == &boar && g_seenEax == Bits(&boar));
        CHECK(g_afterEbx == 0x5EB05EB0u && g_afterEsi == 0x3333u && g_afterEcx == 0x4444u);

        // Before, object in ESI: an object already removed raises nothing.
        g_seenEsi = 0;
        CallThunk(&lifetime::DestroyRemovedObjectBefore, Bits(&boar), 0x5555u, 0x6666u);
        CHECK(removed.count == 1 && g_seenEsi == Bits(&boar) && g_afterEax == 0x5555u);
        g_source.resident.clear();
    }
}
