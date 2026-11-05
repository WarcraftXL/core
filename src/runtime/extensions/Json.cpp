// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/extensions/Json.hpp"

#include <cstdint>
#include <cstdio>

namespace wxl::runtime::json
{
    namespace
    {
        constexpr int kMaxDepth = 64;

        constexpr uint32_t kHighSurrogate     = 0xD800;
        constexpr uint32_t kLowSurrogate      = 0xDC00;
        constexpr uint32_t kLastLowSurrogate  = 0xDFFF;
        constexpr uint32_t kFirstSupplementary = 0x10000;

        void AppendUtf8(std::string& out, uint32_t cp)
        {
            if (cp < 0x80)
            {
                out += char(cp);
                return;
            }
            if (cp < 0x800)
            {
                out += char(0xC0 | (cp >> 6));
                out += char(0x80 | (cp & 0x3F));
                return;
            }
            if (cp < 0x10000)
            {
                out += char(0xE0 | (cp >> 12));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
                return;
            }
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }

        class Parser
        {
        public:
            explicit Parser(const std::string& s) : m_s(s) {}

            bool Document(Value& out)
            {
                Space();
                if (!Parse(out, 0)) return false;
                Space();
                return m_pos == m_s.size() || Fail("text after the JSON value");
            }

            const std::string& Error() const { return m_error; }

        private:
            bool Fail(const char* what)
            {
                if (!m_error.empty()) return false;
                char buffer[96];
                std::snprintf(buffer, sizeof buffer, "%s at byte %u", what, static_cast<unsigned>(m_pos));
                m_error = buffer;
                return false;
            }

            bool More() const { return m_pos < m_s.size(); }
            bool At(char c) const { return More() && m_s[m_pos] == c; }

            bool Skip(char c)
            {
                if (!At(c)) return false;
                ++m_pos;
                return true;
            }

            void Space()
            {
                while (More() && (m_s[m_pos] == ' ' || m_s[m_pos] == '\t' || m_s[m_pos] == '\n' || m_s[m_pos] == '\r'))
                    ++m_pos;
            }

            bool Literal(const char* word)
            {
                for (const char* c = word; *c; ++c, ++m_pos)
                    if (!At(*c)) return Fail("invalid literal");
                return true;
            }

            bool Hex4(uint32_t& cp)
            {
                cp = 0;
                for (int i = 0; i < 4; ++i, ++m_pos)
                {
                    if (!More()) return Fail("truncated \\u escape");
                    const char c = m_s[m_pos];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= uint32_t(c - '0');
                    else if (c >= 'a' && c <= 'f') cp |= uint32_t(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp |= uint32_t(c - 'A' + 10);
                    else return Fail("invalid \\u escape");
                }
                return true;
            }

            /// \uXXXX, joined with a following low surrogate when it opens a pair.
            bool Unicode(std::string& out)
            {
                uint32_t cp;
                if (!Hex4(cp)) return false;
                const bool pair = cp >= kHighSurrogate && cp < kLowSurrogate && m_pos + 1 < m_s.size() &&
                                  m_s[m_pos] == '\\' && m_s[m_pos + 1] == 'u';
                if (pair)
                {
                    m_pos += 2;
                    uint32_t low;
                    if (!Hex4(low)) return false;
                    if (low < kLowSurrogate || low > kLastLowSurrogate) return Fail("invalid surrogate pair");
                    cp = kFirstSupplementary + ((cp - kHighSurrogate) << 10) + (low - kLowSurrogate);
                }
                AppendUtf8(out, cp);
                return true;
            }

            bool Escape(std::string& out)
            {
                switch (m_s[m_pos++])
                {
                case '"': out += '"'; return true;
                case '\\': out += '\\'; return true;
                case '/': out += '/'; return true;
                case 'b': out += '\b'; return true;
                case 'f': out += '\f'; return true;
                case 'n': out += '\n'; return true;
                case 'r': out += '\r'; return true;
                case 't': out += '\t'; return true;
                case 'u': return Unicode(out);
                default: return Fail("invalid escape");
                }
            }

            bool String(std::string& out)
            {
                ++m_pos;
                while (More())
                {
                    const char c = m_s[m_pos++];
                    if (c == '"') return true;
                    if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in a string");
                    if (c != '\\')
                    {
                        out += c;
                        continue;
                    }
                    if (!More()) break;
                    if (!Escape(out)) return false;
                }
                return Fail("unterminated string");
            }

            size_t Digits()
            {
                size_t n = 0;
                for (; More() && m_s[m_pos] >= '0' && m_s[m_pos] <= '9'; ++m_pos) ++n;
                return n;
            }

            bool Number()
            {
                Skip('-');
                if (!Digits()) return Fail("invalid number");
                if (Skip('.') && !Digits()) return Fail("invalid number");
                if (Skip('e') || Skip('E'))
                {
                    if (!Skip('+')) Skip('-');
                    if (!Digits()) return Fail("invalid number");
                }
                return true;
            }

            bool Object(Value& v, int depth)
            {
                v.kind = Value::Kind::Object;
                ++m_pos;
                Space();
                if (Skip('}')) return true;
                for (;;)
                {
                    Space();
                    if (!At('"')) return Fail("expected a member name");
                    std::string key;
                    if (!String(key)) return false;
                    Space();
                    if (!Skip(':')) return Fail("expected ':'");
                    Space();
                    auto member = std::make_unique<Value>();
                    if (!Parse(*member, depth + 1)) return false;
                    v.members[key] = std::move(member);
                    Space();
                    if (Skip(',')) continue;
                    if (Skip('}')) return true;
                    return Fail("expected ',' or '}'");
                }
            }

            bool Array(Value& v, int depth)
            {
                v.kind = Value::Kind::Array;
                ++m_pos;
                Space();
                if (Skip(']')) return true;
                for (;;)
                {
                    Space();
                    Value item;
                    if (!Parse(item, depth + 1)) return false;
                    Space();
                    if (Skip(',')) continue;
                    if (Skip(']')) return true;
                    return Fail("expected ',' or ']'");
                }
            }

            bool Parse(Value& v, int depth)
            {
                if (depth > kMaxDepth) return Fail("nesting too deep");
                if (!More()) return Fail("unexpected end");
                const char c = m_s[m_pos];
                switch (c)
                {
                case '{': return Object(v, depth);
                case '[': return Array(v, depth);
                case '"': v.kind = Value::Kind::String; return String(v.text);
                case 't': v.kind = Value::Kind::Bool; v.text = "true"; return Literal("true");
                case 'f': v.kind = Value::Kind::Bool; return Literal("false");
                case 'n': v.kind = Value::Kind::Null; return Literal("null");
                default: break;
                }
                if (c == '-' || (c >= '0' && c <= '9'))
                {
                    v.kind = Value::Kind::Number;
                    return Number();
                }
                return Fail("unexpected character");
            }

            const std::string& m_s;
            size_t             m_pos = 0;
            std::string        m_error;
        };
    }

    bool Parse(const std::string& text, Value& out, std::string& error)
    {
        Parser parser(text);
        if (parser.Document(out)) return true;
        error = parser.Error();
        return false;
    }

    const Value* Member(const Value& object, const char* name)
    {
        if (object.kind != Value::Kind::Object) return nullptr;
        const auto it = object.members.find(name);
        return it == object.members.end() ? nullptr : it->second.get();
    }
}
