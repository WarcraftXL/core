// SPDX-License-Identifier: GPL-3.0-or-later
// The registry of extension script functions, and the two seams that put them into every context.

#pragma once

#include "wxl/abi/Lua.h"

namespace wxl::script
{
    /// Hooks the callback check and the interface load; false (and nothing registered) when either fails.
    bool InstallSeams();

    /// Adds a global script function to the live context and every later one. The name (63 characters
    /// at most) is copied; false for a null argument, a long name or a full registry.
    bool AddFunction(const char* name, WXL_LuaFunction fn);

    /// Runs `source` (static text) after every interface load, and now on a live context.
    bool AddChunk(const char* name, const char* source);
}
