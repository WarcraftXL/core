// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/storage/ArchiveName.hpp"

namespace wxl::storage
{
    namespace
    {
        constexpr unsigned char kFirstPrintable = 0x20;
        constexpr unsigned char kDelete         = 0x7F;

        char Fold(char c)
        {
            if (c == '/') return '\\';
            return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
        }

        constexpr std::string_view kSkipped[] = { ".pub", ".url", ".tex", ".wav", ".mp3", ".ogg" };
    }

    bool EndsWithCI(std::string_view name, std::string_view suffix)
    {
        if (suffix.size() > name.size()) return false;
        name = name.substr(name.size() - suffix.size());
        for (size_t i = 0; i < suffix.size(); ++i)
            if (Fold(name[i]) != Fold(suffix[i])) return false;
        return true;
    }

    bool CopyArchiveName(const char* name, NameBuffer& out)
    {
        if (!name) return false;
        for (size_t i = 0; i < kMaxArchiveName; ++i)
        {
            const auto c = static_cast<unsigned char>(name[i]);
            out[i]       = char(c);
            if (c == '\0') return i > 0;
            if (c < kFirstPrintable || c >= kDelete) return false;
        }
        return false;
    }

    bool ShouldIntercept(std::string_view name)
    {
        for (std::string_view suffix : kSkipped)
            if (EndsWithCI(name, suffix)) return false;
        return true;
    }
}
