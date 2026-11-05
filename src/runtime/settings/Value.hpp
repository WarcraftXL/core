// SPDX-License-Identifier: GPL-3.0-or-later
// One declared setting and the text forms of its value: checked, normalized, described.

#pragma once

#include "wxl/abi/Settings.h"

#include <string>
#include <string_view>

namespace wxl::runtime::settings
{
    enum class Kind : uint32_t
    {
        Bool    = WXL_SETTING_KIND_BOOL,
        Integer = WXL_SETTING_KIND_INTEGER,
        Number  = WXL_SETTING_KIND_NUMBER,
        Text    = WXL_SETTING_KIND_TEXT,
    };

    struct Setting
    {
        std::string section;
        std::string key;
        Kind        kind = Kind::Text;
        std::string value;    ///< normalized: "true", "42", "0.5", the text
        std::string fallback; ///< the default, normalized
        std::string doc;
        std::string ui;
        double      min  = 0;
        double      max  = -1; ///< max < min: no range
        double      step = 0;

        bool Ranged() const { return max >= min && kind != Kind::Bool && kind != Kind::Text; }
    };

    /// Turns `in` into the setting's text form, or says in `why` why it does not fit.
    bool Normalize(const Setting& s, std::string_view in, std::string& out, std::string& why);

    /// The shortest "%g" text that reads back as `v`.
    std::string FormatNumber(double v);

    /// ", from <min> to <max>" for a ranged setting, "" otherwise.
    std::string RangeText(const Setting& s);

    /// Letters, digits and '_', not starting with a digit.
    bool IsIdentifier(std::string_view s);

    std::string_view Trim(std::string_view s);

    /// The next word of `rest` (blank-separated); `rest` moves past it.
    std::string_view NextWord(std::string_view& rest);
}
