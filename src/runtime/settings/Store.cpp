// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/settings/Store.hpp"

#include "runtime/settings/Ini.hpp"

#include "wxl/abi/Plugin.h"

namespace wxl::runtime::settings
{
    bool Store::CheckDecl(std::string_view ext, const WXL_SettingDecl& d, const std::vector<Setting>& declared) const
    {
        if (d.structSize < sizeof(WXL_SettingDecl) || !d.key || !d.section || !d.value)
        {
            Log(ext, WXL_LOG_ERROR, "a setting declaration is incomplete (key, section and value are required)");
            return false;
        }
        if (!IsIdentifier(d.key) || !IsIdentifier(d.section))
        {
            Log(ext, WXL_LOG_ERROR, std::string("setting '") + d.key + "' in [" + d.section +
                                        "]: a key and a section are C identifiers (letters, digits, _)");
            return false;
        }
        if (d.kind > WXL_SETTING_KIND_TEXT)
        {
            Log(ext, WXL_LOG_ERROR, std::string("setting ") + d.key + ": not a kind of setting");
            return false;
        }
        for (const Setting& s : declared)
            if (s.key == d.key)
            {
                Log(ext, WXL_LOG_ERROR, std::string("setting ") + d.key + " is declared twice");
                return false;
            }
        return true;
    }

    int Store::Declare(std::string_view ext, const WXL_SettingDecl* decls, uint32_t count)
    {
        if (!decls && count) return WXL_E_INVALID_ARGUMENT;
        std::vector<Setting> declared;
        for (uint32_t i = 0; i < count; ++i)
        {
            const WXL_SettingDecl& d = decls[i];
            if (!CheckDecl(ext, d, declared)) return WXL_E_INVALID_ARGUMENT;
            Setting s;
            s.section = d.section;
            s.key     = d.key;
            s.kind    = Kind(d.kind);
            s.doc     = d.doc ? d.doc : "";
            s.ui      = d.ui ? d.ui : "";
            s.min     = d.min;
            s.max     = d.max;
            s.step    = d.step;
            std::string why;
            if (!Normalize(s, d.value, s.fallback, why))
            {
                Log(ext, WXL_LOG_ERROR, "setting " + s.key + ": its default '" + d.value + "' " + why);
                return WXL_E_INVALID_ARGUMENT;
            }
            s.value = s.fallback;
            declared.push_back(std::move(s));
        }
        m_exts[std::string(ext)] = std::move(declared);
        ++m_shape;
        Load(ext);
        return WXL_OK;
    }

    void Store::Visit(WXL_SettingVisitFn fn, void* user) const
    {
        const uint32_t shape = m_shape;
        for (const auto& [ext, list] : m_exts)
            for (size_t i = 0; i < list.size(); ++i)
            {
                const Setting&        s = list[i];
                const WXL_SettingDecl d{ sizeof(WXL_SettingDecl), s.section.c_str(), s.key.c_str(), uint32_t(s.kind),
                                         s.value.c_str(), s.doc.c_str(), s.min, s.max, s.step, s.ui.c_str() };
                fn(user, ext.c_str(), &d);
                if (m_shape != shape) return;
            }
    }

    void Store::Load(std::string_view ext)
    {
        std::vector<Setting>&       list = m_exts[std::string(ext)];
        std::string                 text;
        const bool                  had = m_hooks.read && m_hooks.read(ext, text);
        std::vector<const Setting*> missing;
        for (Setting& s : list)
        {
            std::string raw;
            if (!had || !IniGet(text, s.section, s.key, raw))
            {
                missing.push_back(&s);
                continue;
            }
            std::string normal, why;
            if (Normalize(s, raw, normal, why)) s.value = normal;
            else Log(ext, WXL_LOG_WARN, FileName(ext) + ": " + s.key + " = " + raw + " " + why + "; " + s.fallback + " is kept");
        }
        if (had && missing.empty()) return;
        if (!had) text = Header(ext);
        for (const Setting* s : missing) IniSet(text, *s, true);
        if (!m_hooks.write || !m_hooks.write(ext, text))
            Log(ext, WXL_LOG_WARN, FileName(ext) + " cannot be written; the settings are kept in memory");
        else if (!had)
            Log(ext, WXL_LOG_INFO, FileName(ext) + " written with the defaults");
    }

    const Setting* Store::Find(std::string_view ext, std::string_view key) const
    {
        const auto it = m_exts.find(std::string(ext));
        if (it == m_exts.end()) return nullptr;
        for (const Setting& s : it->second)
            if (s.key == key) return &s;
        return nullptr;
    }

    int Store::Get(std::string_view ext, std::string_view key, std::string& out) const
    {
        const Setting* s = Find(ext, key);
        if (!s) return WXL_E_NOT_FOUND;
        out = s->value;
        return WXL_OK;
    }

    int Store::Set(std::string_view ext, std::string_view key, std::string_view value, std::string* why)
    {
        auto* s = const_cast<Setting*>(Find(ext, key));
        if (!s)
        {
            if (why) *why = std::string(ext) + " has no setting " + std::string(key);
            return WXL_E_NOT_FOUND;
        }
        std::string normal, reason;
        if (!Normalize(*s, value, normal, reason))
        {
            const std::string line = std::string(ext) + "." + s->key + ": '" + std::string(value) + "' " + reason;
            if (why) *why = line;
            else Log(ext, WXL_LOG_WARN, line);
            return WXL_E_INVALID_ARGUMENT;
        }
        if (normal == s->value) return WXL_OK;
        s->value = normal;
        if (m_hooks.read && m_hooks.write)
        {
            std::string text;
            if (!m_hooks.read(ext, text)) text = Header(ext);
            IniSet(text, *s, false);
            if (!m_hooks.write(ext, text)) Log(ext, WXL_LOG_WARN, FileName(ext) + " cannot be written");
        }
        // Copies: a listener may change the store.
        const std::string e(ext), k = s->key, v = s->value;
        if (m_hooks.changed) m_hooks.changed(e, k, v);
        return WXL_OK;
    }

    void Store::Log(std::string_view ext, int level, const std::string& line) const
    {
        if (m_hooks.log) m_hooks.log(ext, level, line);
    }

    std::string Store::FileName(std::string_view ext) const
    {
        return m_hooks.fileName ? m_hooks.fileName(ext) : std::string(ext) + ".ini";
    }

    std::string Store::Header(std::string_view ext) const
    {
        const std::string name(ext);
        return "; Settings of " + name +
               ", written with their defaults by WarcraftXL. Edit them here while the game is\n"
               "; closed, or in game with /wxl set " + name + ".<key> <value>; the rest of this file is left as you write it.\n";
    }
}
