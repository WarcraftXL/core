// SPDX-License-Identifier: GPL-3.0-or-later
// wxl-loader-test <built> <scratch>: manifests of every kind through loader::Open, then WXL_Load.

#include "runtime/extensions/Loader.hpp"
#include "runtime/extensions/Manifest.hpp"

#include "wxl/Events.hpp"
#include "wxl/abi/Plugin.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

    namespace mf = wxl::runtime::manifest;
    namespace ld = wxl::runtime::loader;

    std::string g_built, g_root;

    struct Sub { uint32_t event; WXL_EventFn fn; void* user; };
    std::vector<Sub>         g_subs;
    std::vector<std::string> g_log;

    void __cdecl FakeLog(int, const char* tag, const char* fmt, ...)
    {
        char line[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof line, fmt, args);
        va_end(args);
        g_log.push_back(std::string(tag ? tag : "?") + ": " + line);
    }
    int __cdecl FakeConnect(uint32_t event, WXL_Handle, int32_t, WXL_EventFn fn, void* user, WXL_ReleaseFn,
                            WXL_Connection* out)
    {
        g_subs.push_back({ event, fn, user });
        *out = WXL_Connection{ uint32_t(g_subs.size()), 1 };
        return WXL_OK;
    }
    void __cdecl FakeEmit(uint32_t event, const void* args)
    {
        for (const Sub& s : g_subs)
            if (s.event == event) s.fn(s.user, args);
    }

    bool Logged(const std::string& line)
    {
        for (const std::string& l : g_log)
            if (l == line) return true;
        return false;
    }

    std::string ReadFile(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        std::stringstream s;
        s << f.rdbuf();
        return s.str();
    }

    std::string Version(uint32_t major, uint32_t minor)
    {
        return std::to_string(major) + "." + std::to_string(minor);
    }

    std::string Abi(const std::string& abi)
    {
        return "{ \"manifest\": 1, \"extension\": { \"id\": \"x\", \"abi\": \"" + abi + "\" } }";
    }

    /// Stages Extensions\<name>\: a copy of built\<from>\wxl-test-<from>.dll as <name>.dll (unless
    /// from is empty) and wxl.json (unless manifest is null). Returns the folder.
    std::string Stage(const std::string& name, const std::string& from, const char* manifest)
    {
        const std::string dir = g_root + "\\Extensions\\" + name;
        CreateDirectoryA((g_root + "\\Extensions").c_str(), nullptr);
        CreateDirectoryA(dir.c_str(), nullptr);
        DeleteFileA((dir + "\\wxl.json").c_str());
        if (!from.empty())
            CHECK(CopyFileA((g_built + "\\" + from + "\\wxl-test-" + from + ".dll").c_str(),
                            (dir + "\\" + name + ".dll").c_str(), FALSE));
        if (manifest)
        {
            std::ofstream f(dir + "\\wxl.json", std::ios::binary);
            f << manifest;
        }
        return dir;
    }

    bool Loaded(const std::string& name)
    {
        return GetModuleHandleA((g_root + "\\Extensions\\" + name + "\\" + name + ".dll").c_str()) != nullptr;
    }

    /// Opens a staged extension; on refusal, checks the DLL is not loaded and the reason says `why`.
    bool Open(const std::string& name, ld::Opened& out, const char* why = nullptr)
    {
        std::string reason;
        const bool ok = ld::Open(name, g_root + "\\Extensions\\" + name, out, &reason);
        std::printf("%-14s %s %s\n", name.c_str(), ok ? "admitted" : "refused:", reason.c_str());
        if (!ok)
        {
            CHECK(!Loaded(name));
            if (why && reason.find(why) == std::string::npos)
            {
                std::printf("FAIL %s: reason lacks \"%s\"\n", name.c_str(), why);
                ++g_failures;
            }
        }
        return ok;
    }

    void Versions()
    {
        mf::Version v;
        CHECK(mf::ParseVersion("1.3", v) && v.major == 1 && v.minor == 3);
        CHECK(mf::ParseVersion("1.3.307", v) && v.major == 1 && v.minor == 3);
        CHECK(mf::ParseVersion("12.0", v) && v.major == 12 && v.minor == 0);
        CHECK(!mf::ParseVersion("1", v));
        CHECK(!mf::ParseVersion("1.", v));
        CHECK(!mf::ParseVersion(".3", v));
        CHECK(!mf::ParseVersion("1.3.0.0", v));
        CHECK(!mf::ParseVersion("v1.3", v));
        CHECK(!mf::ParseVersion("1.3-beta", v));
        CHECK(!mf::ParseVersion("", v));

        const mf::Version core{ 1, 3 };
        CHECK(mf::Compatible({ 1, 0 }, core));
        CHECK(mf::Compatible({ 1, 3 }, core));
        CHECK(!mf::Compatible({ 1, 4 }, core));
        CHECK(!mf::Compatible({ 0, 9 }, core));
        CHECK(!mf::Compatible({ 2, 0 }, core));

        // permissions.low_level: raw detours.
        const char* base = R"({ "extension": { "abi": "1.3" } )";
        CHECK(!mf::Parse(std::string(base) + "}").lowLevel);
        CHECK(mf::Parse(std::string(base) + R"(, "permissions": { "low_level": true } })").lowLevel);
        CHECK(!mf::Parse(std::string(base) + R"(, "permissions": { "low_level": false } })").lowLevel);
        CHECK(mf::Parse(std::string(base) + R"(, "permissions": { "low_level": 1 } })").status == mf::Status::Invalid);

        // extension.load: when the extension loads.
        CHECK(mf::Parse(R"({ "extension": { "abi": "1.3" } })").stage == mf::Stage::Engine);
        CHECK(mf::Parse(R"({ "extension": { "abi": "1.3", "load": "engine" } })").stage == mf::Stage::Engine);
        CHECK(mf::Parse(R"({ "extension": { "abi": "1.3", "load": "before-archives" } })").stage
              == mf::Stage::BeforeArchives);
        CHECK(mf::Parse(R"({ "extension": { "abi": "1.3", "load": "early" } })").status == mf::Status::Invalid);
        CHECK(mf::Parse(R"({ "extension": { "abi": "1.3", "load": true } })").status == mf::Status::Invalid);

        CHECK(mf::Core().major == WXL_SDK_VERSION_MAJOR && mf::Core().minor == WXL_SDK_VERSION_MINOR);
    }

    void Manifests()
    {
        mf::Manifest m = mf::Parse("{\"extension\":{\"id\":\"a\\u00e9\",\"abi\":\"1.1\",\"hooks\":[{\"target\":\"X\"}],\"n\":-1.5e3,\"t\":true,\"z\":null}}");
        CHECK(m.status == mf::Status::Ok && m.abi.major == 1 && m.abi.minor == 1 && m.id == "a\xC3\xA9");
        CHECK(mf::Parse("").status == mf::Status::Invalid);
        CHECK(mf::Parse("[]").status == mf::Status::Invalid);
        CHECK(mf::Parse("{\"extension\":{\"abi\":1.3}}").status == mf::Status::Invalid);
        CHECK(mf::Parse("{\"extension\":{\"abi\":\"1.3\"}} x").status == mf::Status::Invalid);
        CHECK(mf::Parse("{\"extension\":{\"abi\":\"1.3\",}}").status == mf::Status::Invalid);
        CHECK(mf::Read(g_root + "\\no-such-file.json").status == mf::Status::Absent);

        // The manifest wxl_add_extension wrote carries this SDK's version.
        m = mf::Read(g_built + "\\current\\wxl.json");
        CHECK(m.status == mf::Status::Ok && m.id == "wxl-test-current");
        CHECK(m.abi.major == WXL_SDK_VERSION_MAJOR && m.abi.minor == WXL_SDK_VERSION_MINOR);
    }

    void Admission()
    {
        const uint32_t major = WXL_SDK_VERSION_MAJOR, minor = WXL_SDK_VERSION_MINOR;
        ld::Opened ext;

        // Built against this SDK, with the manifest the build wrote: loaded, then driven.
        const std::string current = ReadFile(g_built + "\\current\\wxl.json");
        Stage("current", "current", current.c_str());
        CHECK(Open("current", ext));
        CHECK(ext.info && ext.info->sdkVersion == WXL_SDK_VERSION && ext.info->pluginVersion == (7u << 16 | 1u));
        CHECK(ext.version == "7.0.1");
        CHECK(ext.name && std::strcmp(ext.name, "wxl-test-current") == 0);
        WXL_Api api{};
        api.structSize = sizeof api;
        api.apiVersion = WXL_API_VERSION;
        api.Log        = &FakeLog;
        api.Connect    = &FakeConnect;
        api.Emit       = &FakeEmit;
        CHECK(ext.load && ext.load(&api));
        wxl::events::UpdateArgs update{ 0.5f, 42 };
        FakeEmit(uint32_t(wxl::events::Event::OnUpdate), &update);
        CHECK(Logged("wxl-test-current: tick 42"));

        // A manifest asking for more than this core serves: refused before LoadLibrary.
        const std::string ahead = Abi(Version(major, minor + 1));
        Stage("minor-ahead", "current", ahead.c_str());
        CHECK(!Open("minor-ahead", ext, ("says SDK " + Version(major, minor + 1)).c_str()));
        const std::string next = Abi(Version(major + 1, 0));
        Stage("major-ahead", "current", next.c_str());
        CHECK(!Open("major-ahead", ext, ("says SDK " + Version(major + 1, 0)).c_str()));
        if (major > 0)
        {
            const std::string prev = Abi(Version(major - 1, 9));
            Stage("major-behind", "current", prev.c_str());
            CHECK(!Open("major-behind", ext, "says SDK"));
        }

        // Unusable manifests: refused before LoadLibrary.
        Stage("bad-json", "current", "{ \"extension\": ");
        CHECK(!Open("bad-json", ext, "not valid JSON"));
        Stage("no-abi", "current", "{ \"extension\": { \"id\": \"no-abi\" } }");
        CHECK(!Open("no-abi", ext, "extension.abi"));
        Stage("bad-abi", "current", Abi("one.two").c_str());
        CHECK(!Open("bad-abi", ext, "not \"major.minor\""));

        // An older minor in the manifest: admitted; the DLL's own version still decides.
        Stage("older-minor", "current", Abi(Version(major, 0)).c_str());
        CHECK(Open("older-minor", ext));

        // No manifest: refused before LoadLibrary, whatever the DLL.
        Stage("no-manifest", "current", nullptr);
        CHECK(!Open("no-manifest", ext, "no wxl.json"));

        // Another API version: refused after WXL_Query, with that version named.
        Stage("old-api", "old-api", Abi(Version(major, minor)).c_str());
        CHECK(!Open("old-api", ext, "built against API v1"));

        // A DLL built against a newer minor than its manifest says: refused after WXL_Query and unloaded.
        Stage("newer-lying", "newer", Abi(Version(major, minor)).c_str());
        CHECK(!Open("newer-lying", ext, ("built against SDK " + Version(major, 9)).c_str()));

        // No DLL at all.
        Stage("empty", "", Abi(Version(major, minor)).c_str());
        CHECK(!Open("empty", ext, "could not be loaded"));
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3)
    {
        std::printf("usage: wxl-loader-test <built> <scratch>\n");
        return 2;
    }
    g_built = argv[1];
    g_root  = argv[2];
    for (std::string* s : { &g_built, &g_root })
        for (char& c : *s)
            if (c == '/') c = '\\';
    CreateDirectoryA(g_root.c_str(), nullptr);

    Versions();
    Manifests();
    Admission();

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
