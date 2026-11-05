// SPDX-License-Identifier: GPL-3.0-or-later
// The DLL's entry: the log, the hook engine, the extension loader, then the features by phase.

#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/storage/StorageHook.hpp"
#include "runtime/extensions/Extensions.hpp"
#include "wxl/abi/Common.h"
#include "wxl/game/Gx.hpp"

#include <windows.h>

/// What the patcher adds to the client's imports, so the loader maps this DLL.
extern "C" WXL_EXPORT void WarcraftXL() {}

namespace
{
    constexpr int   kDeviceWaitTicks = 600; ///< 60 s
    constexpr DWORD kDeviceWaitMs    = 100;

    /// Waits for the graphics device (a backend without D3D9 marks its own), then installs the Normal
    /// features: they may read render state.
    DWORD WINAPI MainThread(LPVOID)
    {
        wxl::storage::Install();
        for (int i = 0; i < kDeviceWaitTicks && !wxl::game::gx::RawDevice() && !wxl::game::gx::DeviceUpWithoutD3d9(); ++i)
            Sleep(kDeviceWaitMs);

        wxl::hook::InstallRegisteredFeatures(wxl::hook::Phase::Normal);
        wxl::hook::EnableAll();
        WLOG_INFO("wxl-core ready");
        return 0;
    }
}

/// Under the loader lock nothing may be loaded: the extensions load later, from the client's own
/// startup (see InstallLoader). The Boot features (the Lua service, the device seam) are enabled now,
/// before the client's code reaches them.
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH) wxl::log::Close("wxl-core detaching");
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);
    wxl::log::Open("Logs\\wxl-core.log");
    WLOG_INFO("wxl-core starting (build %s %s)", __DATE__, __TIME__);

    wxl::hook::Init();
    wxl::runtime::extensions::InstallLoader();
    wxl::hook::InstallRegisteredFeatures(wxl::hook::Phase::Boot);
    wxl::hook::EnableAll();
    CloseHandle(CreateThread(nullptr, 0, &MainThread, nullptr, 0, nullptr));
    return TRUE;
}
