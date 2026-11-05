// SPDX-License-Identifier: GPL-3.0-or-later
// wxl-net-test.exe: the opcodes past the client's table, their owners, and the messages' split and frame.

#include "runtime/net/Messages.hpp"

#include "wxl/abi/Plugin.h"

#include <cstdio>
#include <cstring>

namespace
{
    int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

    namespace net = wxl::runtime::net;

    void __cdecl HandlerA(void*, uint32_t, uint32_t, const uint8_t*, uint32_t) {}
    void __cdecl HandlerB(void*, uint32_t, uint32_t, const uint8_t*, uint32_t) {}
}

int main()
{
    net::Handlers handlers;
    int           userA = 0, userB = 0;
    net::Handler  found;

    // The range: past the client's table, at most two bytes.
    CHECK(handlers.First() == 0x51F);
    CHECK(handlers.Set(1, 0x51E, &HandlerA, &userA) == WXL_E_INVALID_ARGUMENT);
    CHECK(handlers.Set(1, 0x10000, &HandlerA, &userA) == WXL_E_INVALID_ARGUMENT);
    CHECK(handlers.Set(1, 0x521, nullptr, &userA) == WXL_E_INVALID_ARGUMENT);
    CHECK(handlers.Set(1, 0x521, &HandlerA, &userA) == WXL_OK);
    CHECK(handlers.Set(1, 0xFFFF, &HandlerA, &userA) == WXL_OK);
    CHECK(handlers.Count() == 2);

    // One owner per opcode; the owner's own is replaced.
    CHECK(handlers.Set(2, 0x521, &HandlerB, &userB) == WXL_E_DENIED);
    CHECK(handlers.Find(0x521, found) && found.owner == 1 && found.fn == &HandlerA && found.user == &userA);
    CHECK(handlers.Set(1, 0x521, &HandlerB, &userB) == WXL_OK);
    CHECK(handlers.Find(0x521, found) && found.fn == &HandlerB && found.user == &userB);
    CHECK(!handlers.Find(0x522, found));
    CHECK(!handlers.Find(0x100, found));

    // Clear is the owner's; a disable takes back all of an owner's opcodes.
    CHECK(handlers.Clear(2, 0x521) == WXL_E_NOT_FOUND);
    CHECK(handlers.Set(2, 0x522, &HandlerB, &userB) == WXL_OK);
    CHECK(handlers.DropOwner(1) == 2);
    CHECK(!handlers.Find(0x521, found) && handlers.Count() == 1);
    CHECK(handlers.Clear(2, 0x522) == WXL_OK && handlers.Count() == 0);

    // A client that moved its bound: opcodes under it are the client's.
    net::Handlers patched(0x600);
    CHECK(patched.Set(1, 0x521, &HandlerA, nullptr) == WXL_E_INVALID_ARGUMENT);
    CHECK(patched.Set(1, 0x600, &HandlerA, nullptr) == WXL_OK);
    patched.SetFirst(0x700);
    CHECK(!patched.Find(0x600, found));

    // The split: opcode then body, from the store's read position; too short is no message.
    const uint8_t stream[] = { 0x21, 0x05, 0x00, 0x00, 0x80, 0x3F, 0xAA };
    net::Message  message;
    CHECK(net::Split(stream, 0, 0, sizeof stream, message));
    CHECK(message.opcode == 0x521 && message.body == stream + 2 && message.size == 5);
    CHECK(net::Split(stream + 2, 2, 2, sizeof stream, message) && message.opcode == 0);
    CHECK(net::Split(stream, 0, 5, sizeof stream, message) && message.size == 0);
    CHECK(!net::Split(stream, 0, 6, sizeof stream, message));
    CHECK(!net::Split(stream, 0, 8, sizeof stream, message));
    CHECK(!net::Split(stream, 4, 2, sizeof stream, message));
    CHECK(!net::Split(nullptr, 0, 0, 4, message));

    // The frame: four opcode bytes, then the body.
    const uint8_t body[] = { 1, 2, 3 };
    const auto    frame  = net::Frame(0x522, body, sizeof body);
    const uint8_t want[] = { 0x22, 0x05, 0x00, 0x00, 1, 2, 3 };
    CHECK(frame.size() == sizeof want && std::memcmp(frame.data(), want, sizeof want) == 0);
    CHECK(net::Frame(0x523, nullptr, 0).size() == 4);

    if (g_failures) return 1;
    std::printf("net: ok\n");
    return 0;
}
