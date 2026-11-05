// SPDX-License-Identifier: GPL-3.0-or-later
// Test extension with hand-written entry points: one claiming to be built against SDK <major>.9.

#include "wxl/abi/Plugin.h"

extern "C" WXL_EXPORT const WXL_PluginInfo* WXL_CDECL WXL_Query(void)
{
    static const WXL_PluginInfo info = { sizeof(WXL_PluginInfo), WXL_API_VERSION, "newer", 4,
                                         WXL_CLIENT_BUILD, WXL_SDK_VERSION_MAKE(WXL_SDK_VERSION_MAJOR, 9) };
    return &info;
}

extern "C" WXL_EXPORT int WXL_CDECL WXL_Load(const WXL_Api* api)
{
    if (api) api->Log(WXL_LOG_INFO, "newer", "loaded");
    return api != nullptr;
}
