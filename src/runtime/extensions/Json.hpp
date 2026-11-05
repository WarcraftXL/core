// SPDX-License-Identifier: GPL-3.0-or-later
// A strict JSON reader for manifests: objects, strings and booleans are kept, the rest only checked.

#pragma once

#include <map>
#include <memory>
#include <string>

namespace wxl::runtime::json
{
    struct Value
    {
        enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
        std::string                                   text; ///< a string's text; "true" for true
        std::map<std::string, std::unique_ptr<Value>> members;
    };

    /// Parses a whole document; false with `error` ("<what> at byte <n>") when it is not JSON.
    bool Parse(const std::string& text, Value& out, std::string& error);

    /// An object's member, or null.
    const Value* Member(const Value& object, const char* name);
}
