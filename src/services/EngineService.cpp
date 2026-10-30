// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.engine service: each engine type's layout hash, from the SDK's table of them
// (wxl/engine/layouts.def), the column of this runtime's architecture. On x64 the recompiled
// client's build holds its own types to that column (WarcraftXL/EngineLayouts.gen.cpp), so the
// answer is the client's; on x86 the client is Wow.exe 12340, which never changes.

#include "services/EngineService.hpp"

#include <cstring>

namespace
{
    struct Layout
    {
        const char* fork;  ///< CGxDevice::TextureTarget
        const char* name;  ///< CGxDevice_TextureTarget
        uint64_t    hash;
    };

#if defined(_M_IX86)
#define WXL_ENGINE_TYPE(Name, fork, size_x86, hash_x86, size_x64, hash_x64) {#fork, #Name, hash_x86},
#else
#define WXL_ENGINE_TYPE(Name, fork, size_x86, hash_x86, size_x64, hash_x64) {#fork, #Name, hash_x64},
#endif
    const Layout kLayouts[] = {
#include "wxl/engine/layouts.def"
    };
#undef WXL_ENGINE_TYPE

    wxl_result WXL_CALL LayoutOf(const char* name, uint64_t* out_hash)
    {
        if (!name || !out_hash) return WXL_E_INVALID;
        for (const Layout& layout : kLayouts)
            if (std::strcmp(layout.fork, name) == 0 || std::strcmp(layout.name, name) == 0)
            {
                *out_hash = layout.hash;
                return WXL_OK;
            }
        return WXL_E_NOT_FOUND;
    }

    const wxl_engine_api kApi = {sizeof(wxl_engine_api), &LayoutOf};
}

namespace wxl::core::services
{
    const wxl_engine_api& EngineApi() { return kApi; }
}
