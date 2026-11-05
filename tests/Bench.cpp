// SPDX-License-Identifier: GPL-3.0-or-later
// wxl-bench.exe: the cost of one dispatch through the bus, a hook chain, an interceptor, On, a signal, an override, a settings walk.

#include "engine/events/Event.hpp"
#include "engine/hook/Hook.hpp"
#include "runtime/Owners.hpp"
#include "runtime/extensions/Api.hpp"
#include "runtime/settings/Store.hpp"

#include "wxl/Extension.hpp"
#include "wxl/Intercept.hpp"
#include "wxl/Listener.hpp"
#include "wxl/Signal.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

// Every allocation and release of the program, counted while a timed loop runs.
namespace
{
    std::atomic<bool>     g_counting{ false };
    std::atomic<uint64_t> g_allocations{ 0 };
    std::atomic<uint64_t> g_releases{ 0 };

    void* Allocate(size_t size, size_t alignment = 0)
    {
        if (g_counting.load(std::memory_order_relaxed)) g_allocations.fetch_add(1, std::memory_order_relaxed);
        const size_t bytes = size ? size : 1;
        return alignment ? _aligned_malloc(bytes, alignment) : std::malloc(bytes);
    }

    void* AllocateOrThrow(size_t size, size_t alignment = 0)
    {
        if (void* p = Allocate(size, alignment)) return p;
        throw std::bad_alloc();
    }

    void Release(void* p, bool aligned = false)
    {
        if (!p) return;
        if (g_counting.load(std::memory_order_relaxed)) g_releases.fetch_add(1, std::memory_order_relaxed);
        if (aligned) _aligned_free(p);
        else std::free(p);
    }
}

void* operator new(size_t n) { return AllocateOrThrow(n); }
void* operator new[](size_t n) { return AllocateOrThrow(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return Allocate(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return Allocate(n); }
void* operator new(size_t n, std::align_val_t a) { return AllocateOrThrow(n, size_t(a)); }
void* operator new[](size_t n, std::align_val_t a) { return AllocateOrThrow(n, size_t(a)); }
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return Allocate(n, size_t(a)); }
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return Allocate(n, size_t(a)); }
void operator delete(void* p) noexcept { Release(p); }
void operator delete[](void* p) noexcept { Release(p); }
void operator delete(void* p, size_t) noexcept { Release(p); }
void operator delete[](void* p, size_t) noexcept { Release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Release(p); }
void operator delete(void* p, std::align_val_t) noexcept { Release(p, true); }
void operator delete[](void* p, std::align_val_t) noexcept { Release(p, true); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { Release(p, true); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { Release(p, true); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { Release(p, true); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { Release(p, true); }

namespace
{
    namespace ev     = wxl::events;
    namespace owners = wxl::runtime::owners;

    constexpr uint32_t kSizes[]       = { 1, 8, 64 };
    constexpr uint32_t kMaxLinks      = 64;
    constexpr uint32_t kHandlerCalls  = 1u << 20; ///< handler calls per measure: the loop runs this / N times
    constexpr uint32_t kMinIterations = 1u << 12;
    constexpr uint32_t kWarmup        = 256;

    int      g_failures = 0;
    uint64_t g_hits     = 0;
    owners::Id g_ext    = owners::kCore;

    double NowNs()
    {
        static const double nsPerTick = [] {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            return 1e9 / double(frequency.QuadPart);
        }();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return double(now.QuadPart) * nsPerTick;
    }

    /// Times `call` with N handlers behind it; fails the run when it allocates, or misses a handler.
    template <class F>
    void Measure(const char* what, uint32_t n, F&& call)
    {
        const uint32_t iterations = std::max(kHandlerCalls / n, kMinIterations);
        for (uint32_t i = 0; i < kWarmup; ++i) call();

        const uint64_t hits = g_hits;
        g_allocations       = 0;
        g_releases          = 0;
        g_counting          = true;
        const double start  = NowNs();
        for (uint32_t i = 0; i < iterations; ++i) call();
        const double elapsed = NowNs() - start;
        g_counting           = false;

        const double perCall = double(g_allocations + g_releases) / iterations;
        const bool   missed  = g_hits - hits != uint64_t(iterations) * n;
        std::printf("%-28s %3u %10.1f %10.2f %12.3f%s\n", what, n, elapsed / iterations, elapsed / iterations / n,
                    perCall, missed ? "  MISSED HANDLERS" : "");
        if (perCall > 0 || missed) ++g_failures;
    }

    // --- the bus -------------------------------------------------------------------------------------

    void __cdecl Hit(void*, const void*) { ++g_hits; }

    template <ev::Event E>
    void Emit()
    {
        static const ev::Args<E> args{};
        ev::EmitRaw(E, &args);
    }

    template <ev::Event E>
    void BenchBus(const char* what, owners::Id owner)
    {
        uint32_t connected = 0;
        for (uint32_t n : kSizes)
        {
            owners::Run(owner, "connect", [&] {
                for (; connected < n; ++connected)
                {
                    WXL_Connection id{};
                    ev::Connect(E, WXL_Handle{}, WXL_PRIORITY_DEFAULT, &Hit, nullptr, nullptr, &id);
                }
            });
            Measure(what, n, &Emit<E>);
        }
    }

    // --- a raw hook chain ----------------------------------------------------------------------------

    using IntFn = int(__cdecl*)(int);

    volatile int g_salt = 0;

    /// Long enough for the 5-byte jump, and not inlined.
    extern "C" __declspec(noinline) int __cdecl HookTarget(int x)
    {
        int r = x * 2;
        if (g_salt) r += g_salt * 3 + (r >> 1);
        return r;
    }

    extern "C" __declspec(noinline) int __cdecl InterceptTarget(int x)
    {
        int r = x * 3;
        if (g_salt) r += g_salt * 5 + (r >> 2);
        return r;
    }

    IntFn volatile g_hookCall      = &HookTarget;
    IntFn volatile g_interceptCall = &InterceptTarget;

    IntFn g_next[kMaxLinks] = {};

    template <uint32_t I>
    int __cdecl Link(int x)
    {
        ++g_hits;
        return g_next[I](x);
    }

    template <uint32_t... I>
    constexpr std::array<IntFn, sizeof...(I)> MakeLinks(std::integer_sequence<uint32_t, I...>)
    {
        return { &Link<I>... };
    }
    constexpr auto kLinks = MakeLinks(std::make_integer_sequence<uint32_t, kMaxLinks>{});

    void BenchHookChain()
    {
        uint32_t linked = 0;
        for (uint32_t n : kSizes)
        {
            for (; linked < n; ++linked)
                wxl::hook::Install("bench link", reinterpret_cast<void*>(&HookTarget), reinterpret_cast<void*>(kLinks[linked]),
                                   reinterpret_cast<void**>(&g_next[linked]), int(linked));
            wxl::hook::EnableAll();
            Measure("hook chain (core links)", n, [] { g_hookCall(1); });
        }
    }

    // --- an interceptor, as an extension writes it ---------------------------------------------------

    void BenchInterceptor()
    {
        auto&                          interceptor = wxl::Intercept<IntFn, &InterceptTarget>();
        std::vector<wxl::Interception> held;
        held.reserve(kMaxLinks);
        for (uint32_t n : kSizes)
        {
            while (held.size() < n)
            {
                if (held.size() % 2 == 0) held.push_back(interceptor.Before([](auto&) { ++g_hits; }));
                else held.push_back(interceptor.After([](auto&) { ++g_hits; }));
            }
            Measure("interceptor (Before/After)", n, [] { g_interceptCall(1); });
        }
    }

    // --- handlers, signals and overrides on the bus ----------------------------------------------------

    void BenchOn()
    {
        uint32_t connected = 0;
        for (uint32_t n : kSizes)
        {
            owners::Run(g_ext, "connect", [&] {
                for (; connected < n; ++connected) wxl::On<wxl::ItemSlotChange>([](const wxl::ItemSlotChangeArgs&) { ++g_hits; });
            });
            Measure("On<Tag> (extension)", n, &Emit<ev::Event::OnItemSlotChange>);
        }
    }

    void BenchSignals()
    {
        constexpr ev::Event kEvent = ev::Event::OnDoodadSpawn;
        std::vector<wxl::Connection> held;
        held.reserve(kMaxLinks);
        for (uint32_t n : kSizes)
        {
            owners::Run(g_ext, "connect", [&] {
                while (held.size() < n)
                    held.push_back(wxl::SignalOf<wxl::DoodadSpawn>().Connect([](const ev::Args<kEvent>&) { ++g_hits; }).Value());
            });
            Measure("wxl::Signal (extension)", n, &Emit<kEvent>);
        }
    }

    struct Ticker : wxl::Extension
    {
        void OnUpdate(float, uint32_t) override { ++g_hits; }
    };

    void BenchOverrides()
    {
        std::vector<std::unique_ptr<Ticker>> extensions;
        for (uint32_t n : kSizes)
        {
            owners::Run(g_ext, "start", [&] {
                while (extensions.size() < n)
                {
                    extensions.push_back(std::make_unique<Ticker>());
                    wxl::detail::Lifecycle<Ticker>::Start(*extensions.back());
                }
            });
            Measure("Extension::OnUpdate", n, &Emit<ev::Event::OnUpdate>);
        }
    }

    // --- a log line, as an extension formats it ---------------------------------------------------------

    void BenchLog()
    {
        owners::Run(g_ext, "log", [] {
            Measure("Log::Info (extension)", 1, [] {
                ++g_hits;
                wxl::Log::Info("{} {:08X} {:.2f}", g_hits, 0xBEEFu, 0.5);
            });
        });
    }

    // --- the settings walk the overlay's Settings panel makes each frame ------------------------------

    void __cdecl HitSetting(void*, const char*, const WXL_SettingDecl*) { ++g_hits; }

    void BenchSettingsVisit()
    {
        namespace settings = wxl::runtime::settings;
        settings::Store store(settings::Store::Hooks{ nullptr, [](std::string_view, const std::string&) { return true; } });
        std::vector<std::string>     keys;
        std::vector<WXL_SettingDecl> decls;
        for (uint32_t i = 0; i < kMaxLinks; ++i) keys.push_back("Key" + std::to_string(i));
        for (uint32_t n : kSizes)
        {
            while (decls.size() < n)
                decls.push_back(WXL_SettingDecl{ sizeof(WXL_SettingDecl), "Bench", keys[decls.size()].c_str(),
                                                 WXL_SETTING_KIND_INTEGER, "1", "", 0, 100, 1, "" });
            store.Declare("bench", decls.data(), n);
            Measure("settings Visit", n, [&] { store.Visit(&HitSetting, nullptr); });
        }
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wxl::hook::Init();
    g_ext = owners::Add("bench");
    owners::Run(g_ext, "bind", [] { wxl::Bind(&wxl::runtime::extensions::Api(), "bench"); });

    std::printf("%-28s %3s %10s %10s %12s\n", "case", "N", "ns/call", "ns/handler", "allocs/call");
    BenchBus<ev::Event::OnSoundPlay>("bus (core handlers)", owners::kCore);
    BenchBus<ev::Event::OnBlpLoad>("bus (extension handlers)", g_ext);
    BenchHookChain();
    BenchInterceptor();
    BenchOn();
    BenchSignals();
    BenchOverrides();
    BenchLog();
    BenchSettingsVisit();

    if (!owners::Running(g_ext)) ++g_failures;
    std::printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
