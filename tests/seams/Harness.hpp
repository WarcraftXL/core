// SPDX-License-Identifier: GPL-3.0-or-later
// The seams harness: the core's seam files over fake hook points, a fake object manager and the real bus.

#pragma once

#include "engine/events/Event.hpp"
#include "runtime/objects/Objects.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace harness
{
    extern int g_failures;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++harness::g_failures; } } while (0)

    namespace ev  = wxl::events;
    namespace obj = wxl::runtime::objects;

    /// A client object as the seams read it: the header pointer at CGObject_C::m_obj, GUID and type in it.
    struct FakeObject
    {
        explicit FakeObject(uint64_t guid, uint32_t typeMask);
        alignas(8) uint8_t bytes[0xD0] = {};
        alignas(8) uint8_t header[0x18] = {};
    };

    /// The fake object manager behind the objects table the seams use.
    class FakeSource final : public obj::Source
    {
    public:
        std::map<uint64_t, void*> resident;

        bool InWorld() override { return true; }
        void* Find(uint64_t guid) override;
        uint64_t GuidOf(void* object) override;
        uint32_t TypeMaskOf(void* object) override;
    };

    extern FakeSource g_source;
    extern obj::Table g_table;

    /// What a seam's installer gave each hook point: its detour, and where its original is kept.
    struct Attached { void* detour = nullptr; void** original = nullptr; };
    extern std::map<std::string, Attached> g_points;

    /// Keeps a feature installer the seam files registered.
    void RegisterInstaller(bool (*install)());
    /// Runs every feature the seam files registered; each attaches to g_points.
    void InstallFeatures();

    /// The detour of a hook point, with `original` put behind it.
    template <class Fn>
    Fn Detour(const char* point, Fn original)
    {
        Attached& a = g_points[point];
        CHECK(a.detour != nullptr);
        if (a.original) *a.original = reinterpret_cast<void*>(original);
        return reinterpret_cast<Fn>(a.detour);
    }

    /// Records the args of each emission of one event while it lives.
    template <class Args>
    struct Recorder
    {
        int            count = 0;
        Args           last{};
        WXL_Connection id{};

        Recorder() = default;
        Recorder(const Recorder&) = delete;
        ~Recorder() { ev::Disconnect(id); }

        void Connect(ev::Event e, WXL_Handle subject = {})
        {
            CHECK(ev::Connect(e, subject, WXL_PRIORITY_DEFAULT, &Recorder::On, this, nullptr, &id) == WXL_OK);
        }
        static void __cdecl On(void* user, const void* args)
        {
            auto* self = static_cast<Recorder*>(user);
            ++self->count;
            self->last = *static_cast<const Args*>(args);
        }
    };

    inline bool Same(WXL_Handle a, WXL_Handle b) { return a.index == b.index && a.generation == b.generation; }

    void CheckLifetime();
    void CheckThunks();
    void CheckDamage();
    void CheckUnitAndChat();
    void CheckWorld();
}
