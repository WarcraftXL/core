// SPDX-License-Identifier: GPL-3.0-or-later
// Test extension with hand-written entry points: the info block of API v1, five fields long.

#include "wxl/abi/Plugin.h"

namespace
{
    struct InfoV1
    {
        uint32_t    structSize;
        uint32_t    apiVersion;
        const char* name;
        uint32_t    pluginVersion;
        uint32_t    clientBuild;
    };
}

extern "C" WXL_EXPORT const WXL_PluginInfo* WXL_CDECL WXL_Query(void)
{
    static const InfoV1 info = { sizeof(InfoV1), 1, "old-api", 3, WXL_CLIENT_BUILD };
    return reinterpret_cast<const WXL_PluginInfo*>(&info);
}

extern "C" WXL_EXPORT int WXL_CDECL WXL_Load(const WXL_Api*)
{
    return 1;
}
