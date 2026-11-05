// SPDX-License-Identifier: GPL-3.0-or-later
// wxl-guard-test.exe: extensions that fault, throw or decline are disabled and their hooks taken back, resident
// hooks excepted.

#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Hook.hpp"
#include "runtime/extensions/Loader.hpp"
#include "runtime/Owners.hpp"

#include "wxl/abi/Plugin.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

    namespace owners = wxl::runtime::owners;
    namespace ev     = wxl::events;

    // The function the extensions detour. Not inlined, and long enough for a 5-byte jump.
    volatile int g_salt = 0;
    extern "C" __declspec(noinline) int __cdecl Target(int x)
    {
        int r = x * 2;
        if (g_salt) r += g_salt * 3 + (r >> 1);
        return r;
    }
    int (__cdecl* volatile g_call)(int) = &Target;

    // The resident extension's target.
    extern "C" __declspec(noinline) int __cdecl Second(int x)
    {
        int r = x * 2;
        if (g_salt) r += g_salt * 5 + (r >> 2);
        return r;
    }
    int (__cdecl* volatile g_second)(int) = &Second;

    // A core link behind the extensions' (priority 10): the chain must survive their removal.
    int (__cdecl* g_coreNext)(int) = nullptr;
    int __cdecl CoreDetour(int x) { return g_coreNext(x) + 1; }

    // A core subscriber after the extensions: still called on the emit an extension faults in.
    uint32_t g_coreTicks = 0;
    void __cdecl CoreTick(void*, const void*) { ++g_coreTicks; }

    struct Published { std::string name; uint32_t version; void* iface; };
    std::vector<Published> g_services;
    std::vector<std::string> g_log;

    void __cdecl ApiLog(int, const char* tag, const char* fmt, ...)
    {
        char line[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof line, fmt, args);
        va_end(args);
        g_log.push_back(std::string(tag ? tag : "?") + ": " + line);
    }
    int __cdecl ApiConnect(uint32_t event, WXL_Handle subject, int32_t priority, WXL_EventFn fn, void* user,
                           WXL_ReleaseFn release, WXL_Connection* out)
    {
        return ev::Connect(ev::Event(event), subject, priority, fn, user, release, out);
    }
    int __cdecl ApiDisconnect(WXL_Connection c) { return ev::Disconnect(c); }
    int __cdecl ApiConnected(WXL_Connection c) { return ev::Connected(c); }
    void __cdecl ApiEmit(uint32_t event, const void* args) { ev::EmitRaw(ev::Event(event), args); }
    int __cdecl ApiHookAttach(const char* name, uintptr_t target, void* detour, void** original, int priority)
    {
        return wxl::hook::Install(name, target, detour, original, priority) ? 1 : 0;
    }
    int __cdecl ApiHookAttachEx(const char* name, uintptr_t target, void* detour, void** original, int priority,
                                uint32_t flags)
    {
        return wxl::hook::Install(name, target, detour, original, priority, (flags & WXL_HOOK_RESIDENT) != 0) ? 1 : 0;
    }
    int __cdecl ApiHookAttachByName(const char*, void*, void**, int) { return 0; }
    void __cdecl ApiPublish(const char* name, uint32_t version, void* iface) { g_services.push_back({ name, version, iface }); }
    void* __cdecl ApiGetInterface(const char* name, uint32_t version)
    {
        for (const Published& s : g_services)
            if (s.version == version && s.name == name) return s.iface;
        return nullptr;
    }

    WXL_Api MakeApi()
    {
        WXL_Api api{};
        api.structSize       = sizeof api;
        api.apiVersion       = WXL_API_VERSION;
        api.Log              = &ApiLog;
        api.Connect          = &ApiConnect;
        api.Disconnect       = &ApiDisconnect;
        api.Connected        = &ApiConnected;
        api.Emit             = &ApiEmit;
        api.HookAttach       = &ApiHookAttach;
        api.HookAttachByName = &ApiHookAttachByName;
        api.PublishInterface = &ApiPublish;
        api.GetInterface     = &ApiGetInterface;
        api.HookAttachEx     = &ApiHookAttachEx;
        return api;
    }

    /// Loads one extension as the core does: guarded WXL_Load, disabled when it faults or declines.
    owners::Id Load(const std::string& built, const char* folder, const WXL_Api* api, bool* loaded)
    {
        wxl::runtime::loader::Opened ext;
        *loaded = false;
        if (!wxl::runtime::loader::Open(folder, built + "\\" + folder, ext)) return owners::kCore;
        const owners::Id id = owners::Add(ext.name);
        int ok = 0;
        if (!owners::Run(id, "WXL_Load", [&] { ok = ext.load(api); })) return id;
        if (!ok) owners::Disable(id, "it declined to initialise (WXL_Load returned 0)");
        *loaded = ok != 0;
        return id;
    }

    /// Whether the core's log file holds `text`.
    bool LogHolds(const std::string& path, const char* text)
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::string all;
        char buffer[4096];
        for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) all.append(buffer, n);
        std::fclose(f);
        return all.find(text) != std::string::npos;
    }

    void Tick()
    {
        ev::UpdateArgs update{ 0.016f, 1 };
        ev::Emit<ev::Event::OnUpdate>(update);
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2)
    {
        std::printf("usage: wxl-guard-test <built extensions folder>\n");
        return 2;
    }
    const std::string built = argv[1];
    wxl::log::EnableConsole("core: ");
    const std::string logPath = built + "\\guard.log";
    wxl::log::Open(logPath.c_str());

    owners::OnDisable("subscriptions", &ev::DropOwner);
    owners::OnDisable("hooks", &wxl::hook::DropOwner, &wxl::hook::Resident);
    CHECK(wxl::hook::Init());
    CHECK(wxl::hook::Install("core", reinterpret_cast<void*>(&Target), reinterpret_cast<void*>(&CoreDetour),
                             reinterpret_cast<void**>(&g_coreNext), 10));
    ApiPublish("test.target", 1, reinterpret_cast<void*>(&Target));
    ApiPublish("test.second", 1, reinterpret_cast<void*>(&Second));

    const WXL_Api api = MakeApi();
    bool loaded = false;
    const owners::Id faulty = Load(built, "wxl-test-faulty", &api, &loaded);
    CHECK(faulty != owners::kCore && loaded && owners::Running(faulty));
    const owners::Id throws = Load(built, "wxl-test-throws", &api, &loaded);
    CHECK(throws != owners::kCore && loaded && owners::Running(throws));
    const owners::Id declines = Load(built, "wxl-test-declines", &api, &loaded);
    CHECK(declines != owners::kCore && !loaded && !owners::Running(declines));
    const owners::Id resident = Load(built, "wxl-test-resident", &api, &loaded);
    CHECK(resident != owners::kCore && loaded && owners::Running(resident));
    CHECK(wxl::hook::Resident(resident) == 1);
    CHECK(owners::Current() == owners::kCore);

    ev::Subscribe(ev::Event::OnUpdate, &CoreTick, nullptr);
    CHECK(wxl::hook::EnableAll());

    // The declined extension's detour was taken back before the chain went live.
    CHECK(g_call(5) == 10 + 1 + 100);
    CHECK(g_second(5) == 10 + 1000 + 20000);

    const auto* ticks = static_cast<const uint32_t*>(ApiGetInterface("test.faulty.ticks", 1));
    CHECK(ticks != nullptr);
    if (!ticks) return 1;

    // Tick 1: the throwing and the resident extensions are disabled; the faulty one and the core go on.
    Tick();
    CHECK(!owners::Running(throws));
    CHECK(!owners::Running(resident));
    // Its plain detour is taken back, its resident one still runs, and the log says it was kept.
    CHECK(g_second(5) == 10 + 1000);
    CHECK(wxl::hook::Resident(resident) == 1 && wxl::hook::DropOwner(resident) == 0);
    wxl::log::Close("guard: the log is read"); // the file sink is not shared while open
    CHECK(LogHolds(logPath, "kept: 1 resident hooks"));
    // Its DLL stays: an unload asked for leaves it mapped, and the resident detour still answers.
    HMODULE module = GetModuleHandleA("wxl-test-resident.dll");
    CHECK(module != nullptr);
    if (module) FreeLibrary(module);
    CHECK(GetModuleHandleA("wxl-test-resident.dll") != nullptr);
    CHECK(g_second(5) == 10 + 1000);
    CHECK(owners::Running(faulty) && *ticks == 1 && g_coreTicks == 1);
    Tick();
    CHECK(*ticks == 2 && g_coreTicks == 2);

    // Tick 3: the faulty extension reads through null. The core's handler, after it, still runs.
    Tick();
    CHECK(!owners::Running(faulty));
    CHECK(*ticks == 3 && g_coreTicks == 3);
    CHECK(owners::Current() == owners::kCore);

    // Taken back: no more ticks for it, and its detour (the head of the chain) is gone.
    Tick();
    CHECK(*ticks == 3 && g_coreTicks == 4);
    CHECK(g_call(5) == 10 + 1);
    CHECK(ev::Any(ev::Event::OnUpdate));
    CHECK(ev::DropOwner(owners::kCore) == 1 && !ev::Any(ev::Event::OnUpdate));

    // Nothing of the declined one ever ran.
    for (const std::string& line : g_log) CHECK(line.find("declined, yet called") == std::string::npos);

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
