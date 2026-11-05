// SPDX-License-Identifier: GPL-3.0-or-later
// Opens one extension: its manifest is checked before the DLL is loaded, its WXL_Query after.

#pragma once

#include "wxl/abi/Plugin.h"

#include <windows.h>

#include <string>

namespace wxl::runtime::loader
{
    struct Opened
    {
        HMODULE               module = nullptr;
        WXL_LoadFn            load   = nullptr;
        const WXL_PluginInfo* info   = nullptr;
        const char*           name   = nullptr;   ///< info->name, or the folder name
        std::string           version;            ///< extension.version of its manifest, empty when absent
        bool                  lowLevel = false;   ///< its manifest grants permissions.low_level
    };

    /// Reads <dir>\wxl.json, then loads <dir>\<folder>.dll and checks its WXL_Query; WXL_Load is not
    /// called. A missing or unusable manifest refuses it before LoadLibrary; a refusal is logged and
    /// returned in `reason`, and leaves nothing loaded.
    bool Open(const std::string& folder, const std::string& dir, Opened& out, std::string* reason = nullptr);
}
