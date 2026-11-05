// SPDX-License-Identifier: GPL-3.0-or-later
// Settings sets reachable from Lua by name: the WXL_Settings_* script functions (wxl/abi/Lua.h).

#pragma once

#include "wxl/abi/Lua.h"

namespace wxl::script
{
    /// Registers the WXL_Settings_* functions. After InstallSeams.
    bool InstallSettingsFunctions();

    /// Adds a settings set, or replaces the one of the same name; it must outlive the process. False
    /// for a bad set or a full table.
    bool AddSettings(const WXL_SettingsSet* set);

    /// The sets registered, in registration order.
    uint32_t SettingsCount();
    const WXL_SettingsSet* SettingsAt(uint32_t index);
}
