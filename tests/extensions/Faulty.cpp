// SPDX-License-Identifier: GPL-3.0-or-later
// Test extension: detours the test's target and counts ticks; the third tick reads through null.

#include "wxl/wxl.hpp"

using namespace wxl;

namespace
{
    using TargetFn = int __cdecl(int);

    /// The tick counter the test reads, published as a service.
    struct Ticks
    {
        uint32_t count = 0;
    };

    Hook<TargetFn> g_target;
    Ticks          g_ticks;
    int* volatile  g_null = nullptr;

    int __cdecl Detour(int x) { return g_target(x) + 100; }
}

template <>
struct wxl::ServiceTraits<Ticks>
{
    using Table                           = Ticks;
    static constexpr const char* kName    = "test.faulty.ticks";
    static constexpr uint32_t    kVersion = 1;
};

class Faulty : public Extension
{
    void OnStart() override
    {
        if (const void* target = Api()->GetInterface("test.target", 1))
            g_target.Attach("faulty", reinterpret_cast<uintptr_t>(target), &Detour);
        Publish<Ticks>(&g_ticks);
    }

    void OnUpdate(float, uint32_t) override
    {
        if (++g_ticks.count == 3) *g_null = 1;
    }
};

Export<Faulty> extension;
