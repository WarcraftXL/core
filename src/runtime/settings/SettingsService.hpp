// SPDX-License-Identifier: GPL-3.0-or-later
// The wxl.settings service: each extension's ini in its folder, OnSettingChanged, /wxl in the chat.

#pragma once

#include <string>

namespace wxl::runtime::settings
{
    class Store;

    /// The store every extension's settings are in.
    Store& Shared();

    /// Publishes the service; before the extensions load.
    void Publish();

    /// `/wxl` in the chat; once the script seams are up.
    void InstallInterface();

    /// Registers (or refreshes) the extension's settings as a WXL_SettingsSet, for the profiles and
    /// the client's Lua.
    void PublishSet(const std::string& ext);
}
