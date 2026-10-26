// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// WarcraftXL.ini: a small ini reader, kernel32 only, so that it can run before the log opens.

#include "runtime/Settings.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace
{
    std::string_view Trim(std::string_view text)
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
            text.remove_suffix(1);
        return text;
    }

    void Problem(wxl::core::settings::Settings& out, unsigned line, const char* what, std::string_view text)
    {
        if (out.problem_count >= 8) return;
        std::snprintf(out.problems[out.problem_count++], sizeof out.problems[0], "line %u: %s '%.*s'", line, what,
                      static_cast<int>(text.size() > 60 ? 60 : text.size()), text.data());
    }

    bool SameWord(std::string_view a, const char* b)
    {
        const size_t length = std::strlen(b);
        if (a.size() != length) return false;
        for (size_t i = 0; i < length; ++i)
            if ((a[i] | 0x20) != b[i]) return false;
        return true;
    }
}

namespace wxl::core::settings
{
    void Parse(const char* text, size_t size, Settings& out)
    {
        out.found = true;
        std::string_view rest(text, size);
        // A UTF-8 byte order mark, which Notepad writes.
        if (rest.size() >= 3 && static_cast<unsigned char>(rest[0]) == 0xEF &&
            static_cast<unsigned char>(rest[1]) == 0xBB && static_cast<unsigned char>(rest[2]) == 0xBF)
            rest.remove_prefix(3);

        enum class Section
        {
            other,
            log,
            profile,
            gx,
        };
        Section  section = Section::other;
        unsigned number = 0;
        while (!rest.empty())
        {
            const size_t     end  = rest.find('\n');
            std::string_view line = rest.substr(0, end);
            rest                  = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
            ++number;

            const size_t comment = line.find_first_of(";#");
            if (comment != std::string_view::npos) line = line.substr(0, comment);
            line = Trim(line);
            if (line.empty()) continue;

            if (line.front() == '[')
            {
                if (line.back() != ']')
                {
                    Problem(out, number, "a section without its ']'", line);
                    section = Section::other;
                    continue;
                }
                const std::string_view name = Trim(line.substr(1, line.size() - 2));
                section = SameWord(name, "log")       ? Section::log
                          : SameWord(name, "profile") ? Section::profile
                          : SameWord(name, "gx")      ? Section::gx
                                                      : Section::other;
                if (section == Section::profile) out.has_profile = true;
                continue;
            }
            if (section == Section::other) continue;

            const size_t equals = line.find('=');
            if (equals == std::string_view::npos)
            {
                Problem(out, number, "not a 'key = value' line", line);
                continue;
            }
            const std::string_view key   = Trim(line.substr(0, equals));
            const std::string_view value = Trim(line.substr(equals + 1));

            if (section == Section::gx)
            {
                if (SameWord(key, "api"))
                {
                    if (SameWord(value, "vulkan")) out.gx_api = GxApi::vulkan;
                    else if (SameWord(value, "d3d9")) out.gx_api = GxApi::d3d9;
                    else Problem(out, number, "not vulkan or d3d9:", value);
                }
                else if (SameWord(key, "validation"))
                {
                    if (value == "0" || value == "1") out.gx_validation = value == "1";
                    else Problem(out, number, "not 0 or 1:", value);
                }
                else if (SameWord(key, "gpu"))
                {
                    if (!value.empty() && value.size() < sizeof out.gx_gpu)
                    {
                        std::memcpy(out.gx_gpu, value.data(), value.size());
                        out.gx_gpu[value.size()] = '\0';
                    }
                    else Problem(out, number, "not a device index or a part of its name (1 to 63 characters):", value);
                }
                else
                {
                    Problem(out, number, "an unknown key of [gx] (api, validation, gpu):", key);
                }
                continue;
            }

            if (section == Section::profile)
            {
                if (SameWord(key, "enabled"))
                {
                    if (value == "0" || value == "1")
                    {
                        out.has_profile_enabled = true;
                        out.profile_enabled     = value == "1";
                    }
                    else Problem(out, number, "not 0 or 1:", value);
                }
                else if (SameWord(key, "port"))
                {
                    unsigned port   = 0;
                    bool     digits = !value.empty() && value.size() <= 5;
                    for (char c : value) digits = digits && c >= '0' && c <= '9';
                    if (digits)
                        for (char c : value) port = port * 10 + static_cast<unsigned>(c - '0');
                    if (digits && port >= 1 && port <= 65535) out.profile_port = static_cast<uint16_t>(port);
                    else Problem(out, number, "not a port from 1 to 65535:", value);
                }
                else if (SameWord(key, "listen"))
                {
                    if (SameWord(value, "localhost")) out.profile_any = false;
                    else if (SameWord(value, "any")) out.profile_any = true;
                    else Problem(out, number, "not localhost or any:", value);
                }
                else
                {
                    Problem(out, number, "an unknown key of [profile] (enabled, port, listen):", key);
                }
                continue;
            }

            if (SameWord(key, "level"))
            {
                if (wxl::log::ParseLevel(value, out.level)) out.has_level = true;
                else Problem(out, number, "not a level (trace, debug, info, warn, error, off):", value);
            }
            else if (SameWord(key, "keep"))
            {
                unsigned keep  = 0;
                bool     digits = !value.empty() && value.size() <= 4;
                for (char c : value) digits = digits && c >= '0' && c <= '9';
                for (char c : value) keep = keep * 10 + static_cast<unsigned>(c - '0');
                if (digits) out.keep = keep;
                else Problem(out, number, "not a count from 0 to 9999:", value);
            }
            else if (key.size() > 6 && SameWord(key.substr(0, 6), "level."))
            {
                const std::string_view tag = key.substr(6);
                wxl::log::Level        level;
                if (!wxl::log::ParseLevel(value, level))
                    Problem(out, number, "not a level (trace, debug, info, warn, error, off):", value);
                else if (tag.size() >= sizeof out.tags[0].name || out.tag_count >= 32)
                    Problem(out, number, "too long a tag, or more than 32 of them:", tag);
                else
                {
                    Settings::Tag& entry = out.tags[out.tag_count++];
                    std::memcpy(entry.name, tag.data(), tag.size());
                    entry.name[tag.size()] = '\0';
                    entry.level            = level;
                }
            }
            else
            {
                Problem(out, number, "an unknown key of [log] (level, keep, level.<tag>):", key);
            }
        }
    }

    void Load(const wchar_t* path, Settings& out)
    {
        out = Settings{};
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        // A settings file is a few lines; what is past this is not read.
        char  text[16384];
        DWORD read = 0;
        const BOOL ok = ReadFile(file, text, sizeof text, &read, nullptr);
        CloseHandle(file);
        if (!ok) return;
        Parse(text, read, out);
    }

    ProfileChoice ResolveProfile(const Settings& settings, const char* env)
    {
        ProfileChoice choice;
        choice.enabled = settings.profile_enabled;
        choice.port    = settings.profile_port;
        choice.any     = settings.profile_any;
        if (env && *env)
        {
            if (std::strcmp(env, "0") == 0 || std::strcmp(env, "1") == 0)
            {
                choice.enabled  = env[0] == '1';
                choice.from_env = true;
            }
            else choice.ignored = env;
        }
        return choice;
    }

    ProfileChoice ResolveProfile(const Settings& settings)
    {
        // The variable's text lives as long as the process: it is handed back in the choice.
        static char value[64];
        const DWORD length = GetEnvironmentVariableA("WXL_PROFILE", value, sizeof value);
        if (length == 0) return ResolveProfile(settings, nullptr);
        if (length >= sizeof value) std::strcpy(value, "(too long)");
        return ResolveProfile(settings, value);
    }

    void ApplyToLog(const Settings& settings)
    {
        wxl::log::Level fromEnv;
        if (wxl::log::LevelFromEnvironment(fromEnv)) wxl::log::SetMinLevel(fromEnv);
        else if (settings.has_level) wxl::log::SetMinLevel(settings.level);
        for (unsigned i = 0; i < settings.tag_count; ++i)
            wxl::log::SetTagLevel(settings.tags[i].name, settings.tags[i].level);
    }
}
