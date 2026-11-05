// SPDX-License-Identifier: GPL-3.0-or-later
// An extension's ini text: a value read by section and key, a value written on its own line.

#pragma once

#include "runtime/settings/Value.hpp"

#include <string>

namespace wxl::runtime::settings
{
    /// The value of `key` in `[section]`; a comment after it (" ; ..." or " # ...") is not part of it.
    bool IniGet(const std::string& text, const std::string& section, const std::string& key, std::string& value);

    /// Writes the setting's value on its line, else at the end of its section (made when missing), with its
    /// description when `describe`; every other line stays as the player wrote it.
    void IniSet(std::string& text, const Setting& s, bool describe);
}
