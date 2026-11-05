// SPDX-License-Identifier: GPL-3.0-or-later
// wxl-objects-test.exe: handles, generations, tracking and expiry over a fake object manager.

#include "runtime/objects/Objects.hpp"

#include <cstdio>
#include <map>

namespace
{
    int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

    namespace obj = wxl::runtime::objects;

    struct Fake { uint64_t guid; uint32_t typeMask; };

    class FakeSource final : public obj::Source
    {
    public:
        bool                      inWorld = true;
        std::map<uint64_t, Fake*> resident;

        bool InWorld() override { return inWorld; }
        void* Find(uint64_t guid) override
        {
            auto it = resident.find(guid);
            return it == resident.end() ? nullptr : it->second;
        }
        uint64_t GuidOf(void* object) override { return static_cast<Fake*>(object)->guid; }
        uint32_t TypeMaskOf(void* object) override { return static_cast<Fake*>(object)->typeMask; }
    };

    bool Same(WXL_Handle a, WXL_Handle b) { return a.index == b.index && a.generation == b.generation; }
}

int main()
{
    FakeSource source;
    obj::Table table(source);

    Fake wolf{ 0xF130000001000001ull, 0x09 };
    Fake hero{ 0x42ull, 0x19 };
    source.resident[wolf.guid] = &wolf;
    source.resident[hero.guid] = &hero;

    WXL_Handle w{}, h{}, again{};
    CHECK(table.FromGuid(wolf.guid, &w) == WXL_OK && w.generation != 0);
    CHECK(table.FromGuid(hero.guid, &h) == WXL_OK && !Same(w, h));
    CHECK(table.FromGuid(wolf.guid, &again) == WXL_OK && Same(w, again));
    CHECK(table.FromPointer(&hero, &again) == WXL_OK && Same(h, again));
    CHECK(table.Live() == 2);

    void* p = nullptr;
    uint64_t guid = 0;
    CHECK(table.Resolve(w, 0xFFFFFFFFu, &p) == WXL_OK && p == &wolf);
    CHECK(table.Resolve(w, 0x10, &p) == WXL_E_WRONG_KIND && p == nullptr);
    CHECK(table.Guid(h, &guid) == WXL_OK && guid == hero.guid);
    CHECK(table.Resolve(WXL_Handle{}, 0xFFFFFFFFu, &p) == WXL_E_NULL_HANDLE);
    CHECK(table.Resolve(WXL_Handle{ 99, 1 }, 0xFFFFFFFFu, &p) == WXL_E_EXPIRED);
    CHECK(table.FromGuid(0x77, &again) == WXL_E_NOT_FOUND && again.generation == 0);

    // A pointer that is not the resident object of its GUID is refused.
    Fake ghost{ wolf.guid, 0x09 };
    CHECK(table.FromPointer(&ghost, &again) == WXL_E_NOT_FOUND);

    // The wolf leaves: expired for good, even once it is back.
    source.resident.erase(wolf.guid);
    CHECK(table.Resolve(w, 0xFFFFFFFFu, &p) == WXL_E_EXPIRED);
    CHECK(table.Guid(w, &guid) == WXL_E_EXPIRED);
    CHECK(table.Live() == 1);
    source.resident[wolf.guid] = &wolf;
    CHECK(table.Resolve(w, 0xFFFFFFFFu, &p) == WXL_E_EXPIRED);
    CHECK(table.FromGuid(wolf.guid, &again) == WXL_OK && !Same(w, again));
    CHECK(again.index == w.index && again.generation > w.generation); // the slot, reused

    // Another object under the same GUID: the old handle expires.
    const WXL_Handle w2 = again;
    Fake respawn{ wolf.guid, 0x09 };
    source.resident[wolf.guid] = &respawn;
    CHECK(table.Resolve(w2, 0xFFFFFFFFu, &p) == WXL_E_EXPIRED);
    CHECK(table.FromGuid(wolf.guid, &again) == WXL_OK && !Same(w2, again));
    CHECK(table.Resolve(again, 0xFFFFFFFFu, &p) == WXL_OK && p == &respawn);

    // Tracked from the object manager's lifetime points: added once, then forgotten exactly.
    Fake boar{ 0xF130000002000002ull, 0x09 };
    source.resident[boar.guid] = &boar;
    bool fresh = false;
    const WXL_Handle b = table.Track(&boar, &fresh);
    CHECK(fresh && b.generation != 0);
    CHECK(Same(table.Track(&boar, &fresh), b) && !fresh);
    CHECK(Same(table.Find(&boar), b) && table.Find(&ghost).generation == 0);
    CHECK(table.FromGuid(boar.guid, &again) == WXL_OK && Same(again, b));
    table.Forget(b);
    CHECK(table.Resolve(b, 0xFFFFFFFFu, &p) == WXL_E_EXPIRED && table.Find(&boar).generation == 0);
    table.Forget(b); // twice is harmless
    CHECK(table.Track(&boar, &fresh).generation != 0 && fresh && !Same(table.Find(&boar), b));

    // A tracked GUID taken by another object: the old handle expires, the new one is fresh.
    Fake reborn{ boar.guid, 0x09 };
    const WXL_Handle old = table.Find(&boar);
    CHECK(table.Track(&reborn, &fresh).generation != 0 && fresh);
    CHECK(table.Resolve(old, 0xFFFFFFFFu, &p) == WXL_E_EXPIRED);

    // Out of the world nothing resolves, and nothing expires by that alone.
    const WXL_Handle last = again = table.Find(&reborn);
    source.resident[boar.guid] = &reborn;
    source.inWorld = false;
    CHECK(table.Resolve(last, 0xFFFFFFFFu, &p) == WXL_E_NOT_IN_WORLD);
    CHECK(table.FromGuid(wolf.guid, &again) == WXL_E_NOT_IN_WORLD);
    source.inWorld = true;
    CHECK(table.Resolve(last, 0xFFFFFFFFu, &p) == WXL_OK && p == &reborn);

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
