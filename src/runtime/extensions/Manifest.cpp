// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/extensions/Manifest.hpp"

#include "wxl/abi/Version.h"

#include "runtime/extensions/Json.hpp"

#include <cstdio>

namespace wxl::runtime::manifest
{
    namespace
    {
        constexpr size_t kMaxFileSize = 1 << 20;
        constexpr char   kUtf8Bom[]   = "\xEF\xBB\xBF";
        constexpr size_t kMaxDigits   = 9; ///< a version part stays within 32 bits

        using json::Value;

        Manifest Invalid(std::string error)
        {
            Manifest m;
            m.status = Status::Invalid;
            m.error  = std::move(error);
            return m;
        }

        /// Reads the whole file; false with `m` set to the reason when it cannot.
        bool ReadAll(const std::string& path, std::string& text, Manifest& m)
        {
            FILE* file = std::fopen(path.c_str(), "rb");
            if (!file) return false;
            char   chunk[4096];
            size_t n;
            while ((n = std::fread(chunk, 1, sizeof chunk, file)) > 0)
            {
                text.append(chunk, n);
                if (text.size() <= kMaxFileSize) continue;
                std::fclose(file);
                m = Invalid("is larger than 1 MB");
                return false;
            }
            const bool failed = std::ferror(file) != 0;
            std::fclose(file);
            if (failed) m = Invalid("could not be read");
            return !failed;
        }
    }

    Version Core() { return Version{ WXL_SDK_VERSION_MAJOR, WXL_SDK_VERSION_MINOR }; }

    bool ParseVersion(const std::string& text, Version& out)
    {
        uint32_t parts[3] = {};
        size_t   count = 0, digits = 0;
        for (const char c : text)
        {
            if (c >= '0' && c <= '9')
            {
                if (digits == kMaxDigits) return false;
                parts[count] = parts[count] * 10 + uint32_t(c - '0');
                ++digits;
            }
            else if (c == '.' && digits && count < 2)
            {
                ++count;
                digits = 0;
            }
            else
            {
                return false;
            }
        }
        if (!digits || count < 1) return false;
        out = Version{ parts[0], parts[1] };
        return true;
    }

    bool Compatible(Version extension, Version core)
    {
        return extension.major == core.major && extension.minor <= core.minor;
    }

    Manifest Parse(const std::string& text)
    {
        Value       root;
        std::string error;
        if (!json::Parse(text, root, error)) return Invalid("is not valid JSON (" + error + ")");

        const Value* extension = json::Member(root, "extension");
        if (!extension || extension->kind != Value::Kind::Object) return Invalid("has no \"extension\" object");

        const Value* abi = json::Member(*extension, "abi");
        if (!abi || abi->kind != Value::Kind::String)
            return Invalid("has no \"extension.abi\" (the SDK version, \"major.minor\")");

        Manifest m;
        if (!ParseVersion(abi->text, m.abi))
            return Invalid("has \"extension.abi\": \"" + abi->text + "\", not \"major.minor\"");
        if (const Value* id = json::Member(*extension, "id"); id && id->kind == Value::Kind::String) m.id = id->text;
        if (const Value* v = json::Member(*extension, "version"); v && v->kind == Value::Kind::String) m.version = v->text;
        if (const Value* permissions = json::Member(root, "permissions"))
        {
            const Value* lowLevel = json::Member(*permissions, "low_level");
            if (lowLevel && lowLevel->kind != Value::Kind::Bool)
                return Invalid("has \"permissions.low_level\" that is not true or false");
            m.lowLevel = lowLevel && lowLevel->text == "true";
        }
        if (const Value* load = json::Member(*extension, "load"))
        {
            const bool isText = load->kind == Value::Kind::String;
            if (isText && load->text == "engine") m.stage = Stage::Engine;
            else if (isText && load->text == "before-archives") m.stage = Stage::BeforeArchives;
            else return Invalid("has \"extension.load\" that is not \"engine\" or \"before-archives\"");
        }
        m.status = Status::Ok;
        return m;
    }

    Manifest Read(const std::string& path)
    {
        std::string text;
        Manifest    failure;
        if (!ReadAll(path, text, failure)) return failure;
        if (text.compare(0, sizeof kUtf8Bom - 1, kUtf8Bom) == 0) text.erase(0, sizeof kUtf8Bom - 1);
        return Parse(text);
    }
}
