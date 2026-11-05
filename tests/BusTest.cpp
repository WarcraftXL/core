// SPDX-License-Identifier: GPL-3.0-or-later
// wxl-bus-test.exe: order, connections made or ended mid-walk, releases, subjects, answers, owners.

#include "engine/events/Event.hpp"
#include "runtime/Owners.hpp"

#include <cstdio>
#include <string>

namespace
{
    int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

    namespace ev     = wxl::events;
    namespace owners = wxl::runtime::owners;

    std::string g_order;
    int         g_releases = 0;
    WXL_Connection g_self{};

    void __cdecl Mark(void* user, const void*) { g_order += static_cast<const char*>(user); }
    void __cdecl Release(void*) { ++g_releases; }
    void __cdecl EndSelf(void*, const void*)
    {
        g_order += "S";
        CHECK(ev::Disconnect(g_self) == WXL_OK);
        CHECK(g_releases == 1); // not during the walk
    }
    void __cdecl ConnectMore(void*, const void*)
    {
        WXL_Connection id{};
        ev::Connect(ev::Event::OnUpdate, WXL_Handle{}, WXL_PRIORITY_FIRST, &Mark, const_cast<char*>("n"), nullptr, &id);
    }
    void __cdecl Yes(void*, const void* args) { *static_cast<const ev::InputArgs*>(args)->handled = true; }
    void __cdecl YesThenFault(void*, const void* args)
    {
        *static_cast<const ev::InputArgs*>(args)->handled = true;
        *static_cast<volatile int*>(nullptr) = 1;
    }

    WXL_Connection Connect(ev::Event e, int32_t priority, ev::Handler fn, const char* tag, WXL_Handle subject = {},
                           WXL_ReleaseFn release = nullptr)
    {
        WXL_Connection id{};
        CHECK(ev::Connect(e, subject, priority, fn, const_cast<char*>(tag), release, &id) == WXL_OK);
        return id;
    }

    void Tick()
    {
        ev::UpdateArgs a{ 0.1f, 0 };
        ev::Emit<ev::Event::OnUpdate>(a);
    }

    // A handler that raises another event inside the walk, and one that connects in that inner walk.
    void __cdecl Nested(void*, const void*)
    {
        g_order += "(";
        const ev::WorldEnterArgs w{};
        ev::Emit<ev::Event::OnWorldEnter>(w);
        g_order += ")";
    }
    void __cdecl ConnectInner(void*, const void*)
    {
        g_order += "i";
        WXL_Connection id{};
        ev::Connect(ev::Event::OnWorldEnter, WXL_Handle{}, WXL_PRIORITY_FIRST, &Mark, const_cast<char*>("j"), nullptr, &id);
    }

    // A release that ends another connection: settled after it, released too.
    WXL_Connection g_other{};
    void __cdecl ReleaseOther(void*)
    {
        ++g_releases;
        CHECK(ev::Disconnect(g_other) == WXL_OK);
    }
}

int main()
{
    // Priority, then connection order.
    const WXL_Connection b = Connect(ev::Event::OnUpdate, 0, &Mark, "b", {}, &Release);
    Connect(ev::Event::OnUpdate, WXL_PRIORITY_LAST, &Mark, "z");
    Connect(ev::Event::OnUpdate, WXL_PRIORITY_FIRST, &Mark, "a");
    Connect(ev::Event::OnUpdate, 0, &Mark, "c");
    Tick();
    CHECK(g_order == "abcz");

    // Ended: not called again, released once, Connected says so.
    CHECK(ev::Connected(b) == WXL_OK);
    CHECK(ev::Disconnect(b) == WXL_OK && g_releases == 1);
    CHECK(ev::Disconnect(b) == WXL_E_EXPIRED && ev::Connected(b) == WXL_E_EXPIRED);
    g_order.clear();
    Tick();
    CHECK(g_order == "acz");

    // Ended inside its own handler: released after the walk. Connected during a walk: from the next.
    g_self = Connect(ev::Event::OnUpdate, 1, &EndSelf, "", {}, &Release);
    Connect(ev::Event::OnUpdate, 2, &ConnectMore, "");
    g_order.clear();
    Tick();
    CHECK(g_order == "acSz" && g_releases == 2);
    g_order.clear();
    Tick();
    CHECK(g_order == "ancz" || (std::printf("order %s\n", g_order.c_str()), false));

    // A subject: only events about it; its connections end when it expires, with their releases.
    const WXL_Handle wolf{ 7, 1 }, boar{ 8, 1 };
    Connect(ev::Event::OnTargetChanged, 0, &Mark, "w", wolf, &Release);
    Connect(ev::Event::OnTargetChanged, 0, &Mark, "*");
    ev::TargetChangedArgs t{ nullptr };
    g_order.clear();
    ev::EmitFor(ev::Event::OnTargetChanged, boar, &t);
    ev::EmitFor(ev::Event::OnTargetChanged, wolf, &t);
    ev::EmitRaw(ev::Event::OnTargetChanged, &t);
    CHECK(g_order == "*w**" || (std::printf("order %s\n", g_order.c_str()), false));
    CHECK(ev::DropSubject(wolf) == 1 && g_releases == 3);
    g_order.clear();
    ev::EmitFor(ev::Event::OnTargetChanged, wolf, &t);
    CHECK(g_order == "*");

    // About an object or not, one order: priority, then connection order, across both.
    {
        const WXL_Handle fox{ 9, 1 };
        Connect(ev::Event::OnObjectDestroy, 5, &Mark, "3", fox);
        Connect(ev::Event::OnObjectDestroy, 0, &Mark, "2");
        Connect(ev::Event::OnObjectDestroy, 0, &Mark, "x", WXL_Handle{ 10, 1 });
        Connect(ev::Event::OnObjectDestroy, -5, &Mark, "1", fox);
        Connect(ev::Event::OnObjectDestroy, 5, &Mark, "4");
        Connect(ev::Event::OnObjectDestroy, 0, &Mark, "b", fox);
        const int m = 0;
        g_order.clear();
        ev::EmitFor(ev::Event::OnObjectDestroy, fox, &m);
        CHECK(g_order == "12b34" || (std::printf("order %s\n", g_order.c_str()), false));
        g_order.clear();
        ev::EmitRaw(ev::Event::OnObjectDestroy, &m);
        CHECK(g_order == "24");
        CHECK(ev::DropSubject(fox) == 3);
        g_order.clear();
        ev::EmitFor(ev::Event::OnObjectDestroy, fox, &m);
        CHECK(g_order == "24");
    }

    // A walk inside a walk: what the inner one connects joins once both have ended.
    {
        Connect(ev::Event::OnSoundPlay, 0, &Nested, "");
        Connect(ev::Event::OnWorldEnter, 0, &ConnectInner, "");
        const int l = 0;
        g_order.clear();
        ev::EmitRaw(ev::Event::OnSoundPlay, &l);
        CHECK(g_order == "(i)" || (std::printf("order %s\n", g_order.c_str()), false));
        g_order.clear();
        ev::EmitRaw(ev::Event::OnSoundPlay, &l);
        CHECK(g_order == "(ji)" || (std::printf("order %s\n", g_order.c_str()), false));
    }

    // A release that ends another connection.
    {
        const int before = g_releases;
        g_other = Connect(ev::Event::OnDoodadSpawn, 0, &Mark, "", {}, &Release);
        const WXL_Connection first = Connect(ev::Event::OnDoodadSpawn, 0, &Mark, "", {}, &ReleaseOther);
        CHECK(ev::Disconnect(first) == WXL_OK && g_releases == before + 2);
        CHECK(ev::Connected(g_other) == WXL_E_EXPIRED && !ev::Any(ev::Event::OnDoodadSpawn));
    }

    // With no connection about an object, an expiry has nothing to drop.
    CHECK(ev::DropSubject(WXL_Handle{ 11, 1 }) == 0);

    // Answers: an extension that wrote and then faulted leaves the answer as it found it.
    const owners::Id ext = owners::Add("faulty");
    owners::Run(ext, "test", [] { Connect(ev::Event::OnInput, WXL_PRIORITY_LAST, &YesThenFault, ""); });
    bool handled = false;
    ev::InputArgs in{ 1, 0, 0, &handled };
    ev::Emit<ev::Event::OnInput>(in);
    CHECK(!handled && !owners::Running(ext));
    Connect(ev::Event::OnInput, 0, &Yes, "");
    ev::Emit<ev::Event::OnInput>(in);
    CHECK(handled);

    // An owner's connections are dropped without their releases.
    const owners::Id other = owners::Add("other");
    owners::Run(other, "test", [] { Connect(ev::Event::OnUpdate, 0, &Mark, "o", {}, &Release); });
    const int releases = g_releases;
    CHECK(ev::DropOwner(other) == 1 && g_releases == releases);

    std::printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
