// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/settings/Ini.hpp"

#include <string_view>
#include <vector>

namespace wxl::runtime::settings
{
    namespace
    {
        struct Line
        {
            size_t           begin, end; ///< the line, without its line break
            std::string_view text;
        };

        std::vector<Line> Lines(const std::string& text)
        {
            std::vector<Line> lines;
            size_t            at = 0;
            while (at < text.size())
            {
                size_t       end  = text.find('\n', at);
                const size_t next = end == std::string::npos ? text.size() : end + 1;
                if (end == std::string::npos) end = text.size();
                if (end > at && text[end - 1] == '\r') --end;
                lines.push_back(Line{ at, end, std::string_view(text).substr(at, end - at) });
                at = next;
            }
            return lines;
        }

        bool SectionOf(std::string_view line, std::string& section)
        {
            line = Trim(line);
            if (line.size() < 2 || line.front() != '[' || line.back() != ']') return false;
            section = std::string(Trim(line.substr(1, line.size() - 2)));
            return true;
        }

        /// key = value on a line; false for a comment, a section, anything else.
        bool KeyOf(std::string_view line, std::string& key, std::string& value)
        {
            line = Trim(line);
            if (line.empty() || line.front() == ';' || line.front() == '#' || line.front() == '[') return false;
            const size_t equals = line.find('=');
            if (equals == std::string_view::npos) return false;
            key                = std::string(Trim(line.substr(0, equals)));
            std::string_view v = line.substr(equals + 1);
            for (size_t i = 1; i < v.size(); ++i)
                if ((v[i] == ';' || v[i] == '#') && (v[i - 1] == ' ' || v[i - 1] == '\t'))
                {
                    v = v.substr(0, i);
                    break;
                }
            value = std::string(Trim(v));
            return true;
        }

        /// "; <doc> Range: ... Default: ...", one comment line per line of the doc.
        std::string Description(const Setting& s)
        {
            std::string       doc   = s.doc;
            const std::string range = RangeText(s);
            if (!range.empty()) doc += (doc.empty() ? "Range" : " Range") + std::string(":") + range.substr(1) + ".";
            doc += (doc.empty() ? "" : " ") + std::string("Default: ") + (s.fallback.empty() ? "(empty)" : s.fallback) + ".";

            std::string block;
            size_t      at = 0;
            while (at <= doc.size())
            {
                const size_t end = doc.find('\n', at);
                block += "; " + doc.substr(at, end == std::string::npos ? std::string::npos : end - at) + "\n";
                if (end == std::string::npos) break;
                at = end + 1;
            }
            return block;
        }
    }

    bool IniGet(const std::string& text, const std::string& section, const std::string& key, std::string& value)
    {
        std::string current, k, v;
        for (const Line& line : Lines(text))
        {
            if (SectionOf(line.text, current)) continue;
            if (current == section && KeyOf(line.text, k, v) && k == key)
            {
                value = v;
                return true;
            }
        }
        return false;
    }

    void IniSet(std::string& text, const Setting& s, bool describe)
    {
        const std::string entry = s.key + " = " + s.value;
        std::string       current, k, v;
        size_t            sectionEnd = std::string::npos;
        for (const Line& line : Lines(text))
        {
            if (SectionOf(line.text, current))
            {
                if (current == s.section) sectionEnd = line.end;
                continue;
            }
            if (current != s.section) continue;
            if (KeyOf(line.text, k, v) && k == s.key)
            {
                text.replace(line.begin, line.end - line.begin, entry);
                return;
            }
            if (!Trim(line.text).empty()) sectionEnd = line.end;
        }

        const std::string block = (describe ? Description(s) : std::string()) + entry + "\n";
        if (sectionEnd == std::string::npos)
        {
            if (!text.empty() && text.back() != '\n') text += "\n";
            text += "\n[" + s.section + "]\n" + block;
            return;
        }
        const size_t insert = text.find('\n', sectionEnd);
        if (insert == std::string::npos) text += "\n" + block.substr(0, block.size() - 1);
        else text.insert(insert + 1, block);
    }
}
