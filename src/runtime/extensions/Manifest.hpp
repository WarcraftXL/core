// SPDX-License-Identifier: GPL-3.0-or-later
// An extension's wxl.json, read before its DLL loads: its SDK version, its load stage, its permissions.

#pragma once

#include <cstdint>
#include <string>

namespace wxl::runtime::manifest
{
    struct Version
    {
        uint32_t major = 0;
        uint32_t minor = 0;
    };

    /// The SDK version this core serves (the SDK it was built against).
    Version Core();

    /// Parses "major.minor" or "major.minor.patch"; the patch is ignored.
    bool ParseVersion(const std::string& text, Version& out);

    /// Same major, and a minor no greater than the core's.
    bool Compatible(Version extension, Version core);

    enum class Status
    {
        Absent,    ///< no wxl.json
        Ok,        ///< read; abi holds extension.abi
        Invalid,   ///< present but unusable; error says why
    };

    /// When an extension loads (extension.load).
    enum class Stage
    {
        Engine,         ///< "engine", the default: at engine initialisation, before the graphics device
        BeforeArchives, ///< "before-archives": before the client mounts its archives, ahead of the others
    };

    struct Manifest
    {
        Status      status = Status::Absent;
        Version     abi;
        std::string id;      ///< extension.id, empty when absent
        std::string version; ///< extension.version, empty when absent
        bool        lowLevel = false; ///< permissions.low_level: raw detours, patches
        Stage       stage = Stage::Engine;
        std::string error;
    };

    /// Reads a manifest file: a JSON object whose "extension" object carries "abi".
    Manifest Read(const std::string& path);

    /// Same as Read, from text already in memory.
    Manifest Parse(const std::string& text);
}
