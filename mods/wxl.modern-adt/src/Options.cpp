// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// modern-adt.ini (see Options.hpp).

#include "Options.hpp"

#include <cctype>

namespace wxl::modern_adt
{
    namespace
    {
        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
            return s;
        }

        bool Same(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
            return true;
        }
    }

    Options ParseOptions(std::string_view text)
    {
        Options          out;
        std::string_view section;
        unsigned         number = 0;
        while (!text.empty())
        {
            const size_t     end  = text.find('\n');
            std::string_view line = text.substr(0, end);
            text                  = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
            ++number;
            if (const size_t comment = line.find_first_of(";#"); comment != std::string_view::npos) line = line.substr(0, comment);
            line = Trim(line);
            if (line.empty()) continue;
            const auto problem = [&](const char* what) {
                out.problems.push_back("modern-adt.ini line " + std::to_string(number) + ": " + what + ": " + std::string(line));
            };
            if (line.front() == '[')
            {
                if (line.back() != ']') problem("a section without its ']'");
                section = Trim(line.substr(1, line.size() - 2));
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string_view::npos)
            {
                problem("not key = value");
                continue;
            }
            const std::string_view key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
            if (Same(section, "texture_scale"))
            {
                if (Same(key, "enabled"))
                {
                    if (value == "0" || value == "1") out.texture_scale = value == "1";
                    else problem("not 0 or 1");
                }
                else if (Same(key, "source"))
                {
                    if (Same(value, "auto")) out.source = adt::Prefer::automatic;
                    else if (Same(value, "mtxp")) out.source = adt::Prefer::mtxp;
                    else if (Same(value, "mtxf")) out.source = adt::Prefer::mtxf;
                    else problem("not auto, mtxp or mtxf");
                }
                else
                    problem("an unknown key of [texture_scale] (enabled, source)");
            }
            else
                problem("an unknown section (texture_scale)");
        }
        return out;
    }
}
