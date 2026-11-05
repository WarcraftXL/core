// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/settings/Value.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wxl::runtime::settings
{
    namespace
    {
        constexpr int kShortestPrecision = 6;
        constexpr int kExactPrecision    = 17;

        bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

        std::string Lower(std::string_view text)
        {
            std::string lower;
            for (char c : text) lower += c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
            return lower;
        }

        bool NormalizeBool(const std::string& text, std::string& out, std::string& why)
        {
            const std::string lower = Lower(text);
            if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") out = "true";
            else if (lower == "false" || lower == "0" || lower == "no" || lower == "off") out = "false";
            else
            {
                why = "is not true or false";
                return false;
            }
            return true;
        }

        bool OutOfRange(const Setting& s, double v, std::string& why)
        {
            if (!s.Ranged() || (v >= s.min && v <= s.max)) return false;
            why = "is out of range" + RangeText(s);
            return true;
        }

        bool NormalizeInteger(const Setting& s, const std::string& text, std::string& out, std::string& why)
        {
            errno               = 0;
            char*           end = nullptr;
            const long long v   = std::strtoll(text.c_str(), &end, 10);
            if (text.empty() || *end != '\0' || errno == ERANGE)
            {
                why = "is not a whole number";
                return false;
            }
            if (OutOfRange(s, static_cast<double>(v), why)) return false;
            out = std::to_string(v);
            return true;
        }

        bool NormalizeNumber(const Setting& s, const std::string& text, std::string& out, std::string& why)
        {
            errno            = 0;
            char*        end = nullptr;
            const double v   = std::strtod(text.c_str(), &end);
            if (text.empty() || *end != '\0' || errno == ERANGE || !std::isfinite(v))
            {
                why = "is not a number";
                return false;
            }
            if (OutOfRange(s, v, why)) return false;
            out = FormatNumber(v);
            return true;
        }
    }

    bool Normalize(const Setting& s, std::string_view in, std::string& out, std::string& why)
    {
        const std::string text(Trim(in));
        switch (s.kind)
        {
        case Kind::Bool: return NormalizeBool(text, out, why);
        case Kind::Integer: return NormalizeInteger(s, text, out, why);
        case Kind::Number: return NormalizeNumber(s, text, out, why);
        case Kind::Text:
            if (text.find_first_of("\r\n") != std::string::npos)
            {
                why = "holds a line break";
                return false;
            }
            out = text;
            return true;
        }
        why = "is of no kind";
        return false;
    }

    std::string FormatNumber(double v)
    {
        char buffer[32];
        for (int precision = kShortestPrecision; precision <= kExactPrecision; ++precision)
        {
            std::snprintf(buffer, sizeof buffer, "%.*g", precision, v);
            if (std::strtod(buffer, nullptr) == v) break;
        }
        return buffer;
    }

    std::string RangeText(const Setting& s)
    {
        if (!s.Ranged()) return "";
        if (s.kind == Kind::Integer)
            return ", from " + std::to_string(static_cast<long long>(s.min)) + " to " +
                   std::to_string(static_cast<long long>(s.max));
        return ", from " + FormatNumber(s.min) + " to " + FormatNumber(s.max);
    }

    bool IsIdentifier(std::string_view s)
    {
        if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;
        for (char c : s)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
        return true;
    }

    std::string_view Trim(std::string_view s)
    {
        while (!s.empty() && IsBlank(s.front())) s.remove_prefix(1);
        while (!s.empty() && IsBlank(s.back())) s.remove_suffix(1);
        return s;
    }

    std::string_view NextWord(std::string_view& rest)
    {
        rest                         = Trim(rest);
        const size_t           end   = rest.find_first_of(" \t");
        const std::string_view word  = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end);
        return word;
    }
}
