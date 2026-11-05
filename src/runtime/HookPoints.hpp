// SPDX-License-Identifier: GPL-3.0-or-later
// The named hook points of HookPoints.def: the core and extensions attach to them by name.

#pragma once

#include "engine/hook/Hook.hpp"

namespace wxl::runtime::hookpoints
{
    /// Installs a detour on a hook point (WXL_Api::HookAttachByName); 0 for an unknown name.
    int AttachByName(const char* pointName, void* detour, void** original, int priority);

    /// The same, with the detour and its original of one function type: a mismatch does not compile.
    template <class Fn>
    inline bool Attach(const char* pointName, Fn* detour, Fn** original, int priority = wxl::hook::kDefaultPriority)
    {
        return AttachByName(pointName, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), priority) != 0;
    }
}
