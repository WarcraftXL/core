// SPDX-License-Identifier: GPL-3.0-or-later
// Test extension: a resident and a plain detour on the test's second target; its first tick reads through null.

#include "wxl/wxl.hpp"

using namespace wxl;

namespace
{
    using TargetFn = int __cdecl(int);

    Hook<TargetFn> g_resident;
    Hook<TargetFn> g_plain;
    int* volatile  g_null = nullptr;

    int __cdecl ResidentDetour(int x) { return g_resident(x) + 1000; }
    int __cdecl PlainDetour(int x) { return g_plain(x) + 20000; }
}

class Resident : public Extension
{
    void OnStart() override
    {
        const void* target = Api()->GetInterface("test.second", 1);
        if (!target) return;
        g_resident.AttachResident("resident", reinterpret_cast<uintptr_t>(target), &ResidentDetour);
        g_plain.Attach("plain", reinterpret_cast<uintptr_t>(target), &PlainDetour);
    }

    void OnUpdate(float, uint32_t) override { *g_null = 1; }
};

Export<Resident> extension;
