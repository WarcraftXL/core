// SPDX-License-Identifier: GPL-3.0-or-later
// OnUnitDeath, OnUnitNameChanged and OnChatMessage from their seams.

#include "tests/seams/Harness.hpp"

#include "wxl/offsets/CGChat.hpp"
#include "wxl/offsets/CGGameUI.hpp"
#include "wxl/offsets/CGUnit_C.hpp"

#include <cstring>

namespace harness
{
    namespace
    {
        namespace off = wxl::offsets;

        int         g_originals = 0;
        const char* g_recorded  = nullptr;

        void __fastcall FakeOnDeath(void*, void*) { ++g_originals; }
        void __cdecl FakeNameUpdate(const uint64_t*) { ++g_originals; }
        void __cdecl FakeRecordChat(const char* text, int32_t, const char*, uint64_t, uint32_t, uint32_t, const char*,
                                    uint32_t, const void*)
        {
            g_recorded = text;
        }

        constexpr uint64_t kWolf = 0xF130000001000001ull;
        constexpr uint64_t kHero = 0x42ull;
    }

    void CheckUnitAndChat()
    {
        FakeObject wolf(kWolf, 0x09);
        g_source.resident[kWolf] = &wolf;

        auto onDeath = Detour("CGUnit_C::OnDeath", static_cast<off::CGUnit_C::OnDeathFn>(&FakeOnDeath));
        Recorder<ev::UnitDeathArgs> deaths;
        deaths.Connect(ev::Event::OnUnitDeath);
        onDeath(&wolf, nullptr);
        CHECK(g_originals == 1 && deaths.count == 1 && deaths.last.guid == kWolf && deaths.last.unit.generation != 0);

        // A name update for a unit out of view raises nothing (the client only refreshes its frames).
        auto nameUpdate = Detour("CGGameUI::UnitNameUpdate", static_cast<off::CGGameUI::UnitNameUpdateFn>(&FakeNameUpdate));
        Recorder<ev::UnitNameArgs> names;
        names.Connect(ev::Event::OnUnitNameChanged);
        const uint64_t away = 0xF130000009000009ull;
        nameUpdate(&away);
        CHECK(g_originals == 2 && names.count == 0);

        // A chat line: raised before the client records it, strings as given, null names as "".
        auto recordChat = Detour("CGChat::RecordChat", static_cast<off::CGChat::RecordChatFn>(&FakeRecordChat));
        Recorder<ev::ChatMessageArgs> lines;
        lines.Connect(ev::Event::OnChatMessage);
        recordChat("hello", 1, "Hero", kHero, 0, 0, nullptr, 77, nullptr);
        CHECK(g_recorded && std::strcmp(g_recorded, "hello") == 0 && lines.count == 1);
        CHECK(lines.last.type == 1 && std::strcmp(lines.last.senderName, "Hero") == 0 && lines.last.lineId == 77);
        CHECK(lines.last.senderGuid == kHero && lines.last.sender.generation == 0 && *lines.last.channelName == '\0');

        FakeObject hero(kHero, 0x19);
        g_source.resident[kHero] = &hero;
        recordChat("in view", 17, nullptr, kHero, 0, 22, "Trade", 78, nullptr);
        CHECK(lines.count == 2 && lines.last.sender.generation != 0 && *lines.last.senderName == '\0');
        CHECK(std::strcmp(lines.last.channelName, "Trade") == 0 && lines.last.zoneChannelId == 22);
        g_source.resident.clear();
    }
}
