// SPDX-License-Identifier: GPL-3.0-or-later
// wxl-settings-test.exe: the store over in-memory files, its ini lines, values and the /wxl command.

#include "runtime/settings/Ini.hpp"
#include "runtime/settings/Store.hpp"

#include <cstdio>
#include <map>
#include <string>

namespace
{
    int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

    namespace st = wxl::runtime::settings;

    std::map<std::string, std::string> g_files;
    std::vector<std::string>           g_log, g_changes;

    st::Store MakeStore()
    {
        return st::Store(st::Store::Hooks{
            [](std::string_view ext, std::string& text) {
                auto it = g_files.find(std::string(ext));
                if (it == g_files.end()) return false;
                text = it->second;
                return true;
            },
            [](std::string_view ext, const std::string& text) {
                g_files[std::string(ext)] = text;
                return true;
            },
            [](std::string_view ext) { return std::string(ext) + ".ini"; },
            [](std::string_view, int, const std::string& line) { g_log.push_back(line); },
            [](const std::string& ext, const std::string& key, const std::string& value) {
                g_changes.push_back(ext + "." + key + "=" + value);
            } });
    }

    WXL_SettingDecl Decl(const char* key, uint32_t kind, const char* value, const char* doc, double min = 0, double max = -1)
    {
        WXL_SettingDecl d{};
        d.structSize = sizeof d;
        d.section    = "Greeter";
        d.key        = key;
        d.kind       = kind;
        d.value      = value;
        d.doc        = doc;
        d.min        = min;
        d.max        = max;
        d.ui         = "";
        return d;
    }

    bool Has(const std::vector<std::string>& lines, const std::string& text)
    {
        for (const std::string& l : lines)
            if (l.find(text) != std::string::npos) return true;
        return false;
    }
}

int main()
{
    const WXL_SettingDecl decls[] = {
        Decl("Text", WXL_SETTING_KIND_TEXT, "hello", "What it says."),
        Decl("Delay", WXL_SETTING_KIND_NUMBER, "2", "Seconds before it speaks.", 0, 30),
        Decl("Times", WXL_SETTING_KIND_INTEGER, "1", "How many times.", 1, 10),
        Decl("Enabled", WXL_SETTING_KIND_BOOL, "true", "Whether it speaks."),
    };

    {
        st::Store store = MakeStore();
        CHECK(store.Declare("greeter", decls, 4) == WXL_OK);
        const std::string& file = g_files["greeter"];
        CHECK(file.find("[Greeter]") != std::string::npos);
        CHECK(file.find("; Seconds before it speaks. Range: from 0 to 30. Default: 2.") != std::string::npos);
        CHECK(file.find("Delay = 2") != std::string::npos && file.find("Enabled = true") != std::string::npos);

        std::string v;
        CHECK(store.Get("greeter", "Times", v) == WXL_OK && v == "1");
        CHECK(store.Set("greeter", "Times", "11") == WXL_E_INVALID_ARGUMENT);
        CHECK(store.Set("greeter", "Times", "4") == WXL_OK && g_changes.back() == "greeter.Times=4");
        CHECK(g_files["greeter"].find("Times = 4") != std::string::npos);
        CHECK(store.Set("greeter", "Enabled", "off") == WXL_OK && g_changes.back() == "greeter.Enabled=false");
        CHECK(store.Set("greeter", "Nope", "1") == WXL_E_NOT_FOUND);
        const size_t changes = g_changes.size();
        CHECK(store.Set("greeter", "Times", "4") == WXL_OK && g_changes.size() == changes); // no change
    }

    // A second session reads the file; a value the player broke keeps the default, with a warning.
    {
        std::string& file = g_files["greeter"];
        file.replace(file.find("Delay = 2"), 9, "Delay = soon   ; edited");
        file += "\n; a comment of the player's\n";
        st::Store store = MakeStore();
        CHECK(store.Declare("greeter", decls, 4) == WXL_OK);
        std::string v;
        CHECK(store.Get("greeter", "Times", v) == WXL_OK && v == "4");
        CHECK(store.Get("greeter", "Delay", v) == WXL_OK && v == "2");
        CHECK(Has(g_log, "Delay = soon is not a number; 2 is kept"));
        CHECK(g_files["greeter"].find("; a comment of the player's") != std::string::npos);

        // The command.
        auto lines = store.Command("get");
        CHECK(lines.size() == 1 && Has(lines, "greeter: 4 setting(s)"));
        lines = store.Command("get greeter.Delay");
        CHECK(Has(lines, "greeter.Delay = 2 (default 2, from 0 to 30)") && Has(lines, "Seconds before it speaks."));
        lines = store.Command("set greeter.Delay 3.5");
        CHECK(Has(lines, "greeter.Delay = 3.5"));
        lines = store.Command("set greeter.Delay 99");
        CHECK(Has(lines, "is out of range"));
        lines = store.Command("get nobody");
        CHECK(Has(lines, "No extension with settings is named nobody"));
        lines = store.Command("");
        CHECK(Has(lines, "/wxl get"));
    }

    // A declaration the store refuses.
    {
        st::Store store = MakeStore();
        WXL_SettingDecl bad = Decl("bad key", WXL_SETTING_KIND_BOOL, "true", "x");
        CHECK(store.Declare("other", &bad, 1) == WXL_E_INVALID_ARGUMENT);
        WXL_SettingDecl outOfRange = Decl("Level", WXL_SETTING_KIND_INTEGER, "50", "x", 0, 10);
        CHECK(store.Declare("other", &outOfRange, 1) == WXL_E_INVALID_ARGUMENT);
    }

    // Values and ini lines on their own.
    {
        st::Setting flag;
        flag.kind = st::Kind::Bool;
        std::string out, why;
        CHECK(st::Normalize(flag, " Yes ", out, why) && out == "true");
        CHECK(!st::Normalize(flag, "maybe", out, why) && why == "is not true or false");
        st::Setting ratio;
        ratio.kind = st::Kind::Number;
        CHECK(st::Normalize(ratio, "0.1", out, why) && out == "0.1");
        CHECK(!st::Normalize(ratio, "1e999", out, why));
        st::Setting text;
        CHECK(!st::Normalize(text, "a\nb", out, why) && why == "holds a line break");

        std::string ini = "[A]\nx = 1 ; kept\n\n[B]\ny = 2\n";
        CHECK(st::IniGet(ini, "A", "x", out) && out == "1");
        CHECK(!st::IniGet(ini, "B", "x", out));
        st::Setting z;
        z.section = "A";
        z.key     = "z";
        z.value   = "3";
        st::IniSet(ini, z, false);
        CHECK(ini == "[A]\nx = 1 ; kept\nz = 3\n\n[B]\ny = 2\n");
        z.value = "4";
        st::IniSet(ini, z, false);
        CHECK(ini.find("z = 4\n") != std::string::npos && ini.find("z = 3") == std::string::npos);
        z.section = "C";
        st::IniSet(ini, z, false);
        CHECK(ini.size() > 12 && ini.substr(ini.size() - 12) == "\n\n[C]\nz = 4\n");
    }

    std::printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
