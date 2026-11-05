// SPDX-License-Identifier: GPL-3.0-or-later
// The player's /wxl command over the settings store.

#include "runtime/settings/Store.hpp"

#include "wxl/abi/Plugin.h"

namespace wxl::runtime::settings
{
    bool Store::Split(std::string_view name, std::string& ext, std::string& key) const
    {
        size_t best = 0;
        for (const auto& [id, list] : m_exts)
        {
            if (list.empty() || id.size() <= best) continue;
            if (name == id)
            {
                best = id.size();
                ext  = id;
                key.clear();
            }
            else if (name.size() > id.size() + 1 && name.substr(0, id.size()) == id && name[id.size()] == '.')
            {
                best = id.size();
                ext  = id;
                key  = std::string(name.substr(id.size() + 1));
            }
        }
        return best > 0;
    }

    std::vector<std::string> Store::CommandList() const
    {
        std::vector<std::string> out;
        for (const auto& [id, list] : m_exts)
            if (!list.empty()) out.push_back(id + ": " + std::to_string(list.size()) + " setting(s); /wxl get " + id);
        if (out.empty()) out.push_back("No extension has settings.");
        return out;
    }

    std::vector<std::string> Store::CommandOne(std::string_view verb, std::string_view name, std::string_view value)
    {
        std::string ext, key;
        if (!Split(name, ext, key))
            return { "No extension with settings is named " + std::string(name) + "; /wxl get lists them." };
        if (verb == "get" && key.empty())
        {
            std::vector<std::string> out;
            for (const Setting& s : m_exts[ext]) out.push_back(ext + "." + s.key + " = " + s.value);
            return out;
        }
        const Setting* s = Find(ext, key);
        if (!s) return { ext + " has no setting " + key + "; /wxl get " + ext + " lists them." };
        if (verb == "get")
        {
            std::vector<std::string> out{ ext + "." + s->key + " = " + s->value + " (default " + s->fallback + RangeText(*s) + ")" };
            if (!s->doc.empty()) out.push_back(s->doc);
            return out;
        }
        if (value.empty()) return { "Give a value: /wxl set " + ext + "." + s->key + " <value>." };
        std::string why;
        if (Set(ext, key, value, &why) != WXL_OK) return { why };
        return { ext + "." + key + " = " + Find(ext, key)->value };
    }

    std::vector<std::string> Store::Command(std::string_view args)
    {
        std::string_view       rest  = Trim(args);
        const std::string_view verb  = NextWord(rest);
        const std::string_view name  = NextWord(rest);
        const std::string_view value = Trim(rest);
        if (verb == "get" && name.empty()) return CommandList();
        if ((verb == "get" || verb == "set") && !name.empty()) return CommandOne(verb, name, value);
        return { "/wxl get [<extension>[.<key>]] shows settings; /wxl set <extension>.<key> <value> changes one." };
    }
}
