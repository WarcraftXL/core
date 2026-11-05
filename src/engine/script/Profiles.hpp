// SPDX-License-Identifier: GPL-3.0-or-later
// Named profiles over every registered settings set: WTF\WarcraftXL\profiles\<name>.cfg.

#pragma once

#include <string>
#include <vector>

namespace wxl::script::profiles
{
    /// A name a profile may take: 1..48 of letters, digits, space, '-', '_', '.' (no leading '.').
    bool ValidName(const char* name);

    /// Writes every registered set's keys, one [set] section each; false for a bad name or a file
    /// that could not be written.
    bool Save(const char* name);

    /// Applies a profile through each set's Set, in the file's order; unknown sets and keys, and keys
    /// a set refuses, are skipped. False for a bad name or a missing file.
    bool Load(const char* name);

    /// Deletes a profile's file. False for a bad name or a missing file.
    bool Delete(const char* name);

    /// The profiles on disk, sorted by name.
    std::vector<std::string> List();

    /// The profile last loaded or saved this session, "" for none.
    const char* Current();

    /// Loads WXL_PROFILE (environment, then WarcraftXL.cfg) once, at the first interface load: every
    /// set has registered by then.
    void LoadStartupOnce();

    /// Publishes the wxl.profiles service (wxl/abi/Profiles.h).
    void Publish();
}
