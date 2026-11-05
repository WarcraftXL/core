// SPDX-License-Identifier: GPL-3.0-or-later
// Every extension's declared settings and values, kept in its ini; main thread.

#pragma once

#include "runtime/settings/Value.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wxl::runtime::settings
{
    /// The settings store. Files and the change report are given to it, so a test runs it in memory.
    class Store
    {
    public:
        /// Where the files are, and who hears of a change. `read` answers false when there is no file yet.
        struct Hooks
        {
            std::function<bool(std::string_view ext, std::string& text)>       read;
            std::function<bool(std::string_view ext, const std::string& text)> write;
            std::function<std::string(std::string_view ext)>                   fileName;
            std::function<void(std::string_view ext, int level, const std::string& line)> log;
            std::function<void(const std::string& ext, const std::string& key, const std::string& value)> changed;
        };

        explicit Store(Hooks hooks) : m_hooks(std::move(hooks)) {}

        /// Declares an extension's settings (replacing what it had declared), then reads its file; the
        /// first time, the file is written with the defaults and their descriptions.
        int Declare(std::string_view ext, const WXL_SettingDecl* decls, uint32_t count);

        int Get(std::string_view ext, std::string_view key, std::string& out) const;

        /// Changes a setting, saves its line, reports it. `why` says what was refused (else logged).
        int Set(std::string_view ext, std::string_view key, std::string_view value, std::string* why = nullptr);

        /// `/wxl get [<ext>[.<key>]]` or `/wxl set <ext>.<key> <value>`: the lines to show the player.
        std::vector<std::string> Command(std::string_view args);

        /// The extensions with settings, and theirs.
        const std::map<std::string, std::vector<Setting>>& All() const { return m_exts; }
        const Setting* Find(std::string_view ext, std::string_view key) const;
        /// Calls fn for every setting, in place and without allocating: Set only changes values, and a
        /// Declare from fn ends the walk.
        void Visit(WXL_SettingVisitFn fn, void* user) const;

    private:
        void Load(std::string_view ext);
        bool CheckDecl(std::string_view ext, const WXL_SettingDecl& d, const std::vector<Setting>& declared) const;
        void Log(std::string_view ext, int level, const std::string& line) const;
        std::string FileName(std::string_view ext) const;
        std::string Header(std::string_view ext) const;
        /// "<ext>.<key>" or "<ext>" into its parts, the longest extension name that matches.
        bool Split(std::string_view name, std::string& ext, std::string& key) const;
        std::vector<std::string> CommandList() const;
        std::vector<std::string> CommandOne(std::string_view verb, std::string_view name, std::string_view value);

        Hooks                                       m_hooks;
        std::map<std::string, std::vector<Setting>> m_exts;
        uint32_t                                    m_shape = 0;
    };
}
