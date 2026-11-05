// SPDX-License-Identifier: GPL-3.0-or-later
// Test extension: subscribes and detours, then declines to initialise.

#include "wxl/Extension.hpp"
#include "wxl/Hook.hpp"

namespace
{
    using TargetFn = int __cdecl(int);

    wxl::Hook<TargetFn> g_target;

    int __cdecl Detour(int x) { return g_target(x) + 1000; }

    class Declines final : public wxl::Extension
    {
        void OnUpdate(float, uint32_t) override { wxl::Log::Error("declined, yet called"); }
    };
    Declines g_declines;
}

extern "C" WXL_EXPORT const WXL_PluginInfo* WXL_CDECL WXL_Query(void)
{
    static const WXL_PluginInfo info = { sizeof(WXL_PluginInfo), WXL_API_VERSION, "wxl-test-declines", 1,
                                         WXL_CLIENT_BUILD, WXL_SDK_VERSION };
    return &info;
}

extern "C" WXL_EXPORT int WXL_CDECL WXL_Load(const WXL_Api* api)
{
    if (!wxl::Bind(api, "wxl-test-declines")) return 0;
    if (const auto* target = wxl::Api()->GetInterface("test.target", 1))
        g_target.Attach("declines", reinterpret_cast<uintptr_t>(target), &Detour);
    wxl::detail::Lifecycle<Declines>::Start(g_declines);
    return 0;
}
