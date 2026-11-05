// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/Config.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace
{
    using Entries = std::unordered_map<std::string, std::string>;

    constexpr size_t kMaxLine      = 512;
    constexpr size_t kMaxBoolValue = 16;

    bool IsBlank(char c) { return c == ' ' || c == '\t'; }

    /// KEY=value lines; '#' or ';' starts a comment; blanks around both are dropped; the first of a
    /// repeated key wins.
    void ParseLine(char* line, Entries& entries)
    {
        while (IsBlank(*line)) ++line;
        if (*line == '#' || *line == ';' || *line == '\0') return;
        char* equals = std::strchr(line, '=');
        if (!equals) return;
        char* keyEnd = equals;
        while (keyEnd > line && IsBlank(keyEnd[-1])) --keyEnd;
        char* value = equals + 1;
        while (IsBlank(*value)) ++value;
        char* valueEnd = value + std::strlen(value);
        while (valueEnd > value && (IsBlank(valueEnd[-1]) || valueEnd[-1] == '\n' || valueEnd[-1] == '\r')) --valueEnd;
        if (keyEnd > line) entries.emplace(std::string(line, keyEnd), std::string(value, valueEnd));
    }

    Entries ParseFile(const char* path)
    {
        Entries entries;
        FILE*   file = nullptr;
        if (fopen_s(&file, path, "rb") != 0 || !file) return entries;
        char line[kMaxLine];
        while (std::fgets(line, sizeof line, file)) ParseLine(line, entries);
        std::fclose(file);
        return entries;
    }

    /// WarcraftXL.cfg in the working directory, else one level up; read once.
    const Entries& CfgFile()
    {
        static const Entries entries = [] {
            Entries found = ParseFile("WarcraftXL.cfg");
            return found.empty() ? ParseFile("..\\WarcraftXL.cfg") : found;
        }();
        return entries;
    }
}

namespace wxl::config
{
    bool Raw(const char* name, char* buffer, size_t capacity)
    {
        if (!name) return false;
        const DWORD length = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(capacity));
        if (length > 0 && length < capacity) return true;

        const auto it = CfgFile().find(name);
        if (it == CfgFile().end() || it->second.empty() || it->second.size() + 1 > capacity) return false;
        std::memcpy(buffer, it->second.c_str(), it->second.size() + 1);
        return true;
    }

    bool Env(const char* name, bool fallback)
    {
        char value[kMaxBoolValue] = {};
        if (!Raw(name, value, sizeof value)) return fallback;
        const char c = value[0];
        return !(c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F');
    }
}
