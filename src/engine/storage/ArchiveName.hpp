// SPDX-License-Identifier: GPL-3.0-or-later
// The file names the client's archive opens receive: validated copies and suffix tests.

#pragma once

#include <cstddef>
#include <string_view>

namespace wxl::storage
{
    /// The longest name copied out of an open; a longer one is not a path the client asks for.
    constexpr size_t kMaxArchiveName = 512;

    /// A name buffer for CopyArchiveName, terminator included.
    using NameBuffer = char[kMaxArchiveName + 1];

    /// Whether `name` ends with `suffix`, ignoring case and taking '/' for '\\'.
    bool EndsWithCI(std::string_view name, std::string_view suffix);

    /// Copies `name` when it looks like a path: printable ASCII, not empty, terminated within the bound.
    /// The client sometimes passes a sentinel whose bytes are not a path.
    bool CopyArchiveName(const char* name, NameBuffer& out);

    /// Whether providers and transforms are offered the name: not the existence probes (.pub, .url),
    /// the texture catalog (.tex) or audio, which the client reads through a path a served file never
    /// completes.
    bool ShouldIntercept(std::string_view name);
}
