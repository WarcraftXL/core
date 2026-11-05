// SPDX-License-Identifier: GPL-3.0-or-later
// wxl-patch-test.exe: expected bytes checked, another owner's range refused, all given back newest first.

#include "engine/patch/Patch.hpp"
#include "runtime/Owners.hpp"

#include "wxl/abi/Plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace
{
    int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

    namespace patch  = wxl::patch;
    namespace owners = wxl::runtime::owners;

    uint8_t                    g_data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    extern const uint8_t       g_rodata[4];
    const uint8_t              g_rodata[4] = { 0x0F, 0x85, 0x11, 0x01 };
    const volatile uint8_t*    g_ro = g_rodata;

    uintptr_t At(const void* p, size_t offset = 0) { return reinterpret_cast<uintptr_t>(p) + offset; }

    bool Is(const volatile uint8_t* p, std::initializer_list<uint8_t> bytes)
    {
        size_t i = 0;
        for (uint8_t b : bytes)
            if (p[i++] != b) return false;
        return true;
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    owners::OnDisable("patches", &patch::DropOwner);
    const owners::Id a = owners::Add("a");
    const owners::Id b = owners::Add("b");

    const uint8_t two[2] = { 0xAA, 0xBB }, wrong[2] = { 9, 9 }, expected[2] = { 3, 4 };

    // Checked, then written.
    CHECK(patch::Write("a1", At(g_data, 2), wrong, two, 2, a) == WXL_E_MISMATCH);
    CHECK(Is(g_data, { 1, 2, 3, 4, 5, 6, 7, 8 }));
    CHECK(patch::Write("a1", At(g_data, 2), expected, two, 2, a) == WXL_OK);
    CHECK(Is(g_data, { 1, 2, 0xAA, 0xBB, 5, 6, 7, 8 }));

    // Read-only data: the protection is lifted for the write and put back.
    const uint8_t jmp[4] = { 0xE9, 0x12, 0x01, 0x00 }, jne[4] = { 0x0F, 0x85, 0x11, 0x01 };
    CHECK(patch::Write("a2", At(g_rodata), jne, jmp, 4, a) == WXL_OK);
    CHECK(Is(g_ro, { 0xE9, 0x12, 0x01, 0x00 }));

    // Another owner may not touch a range already changed; the same owner may, on top of itself.
    const uint8_t one = 0xCC;
    CHECK(patch::Write("b1", At(g_data, 3), nullptr, &one, 1, b) == WXL_E_MISMATCH);
    CHECK(patch::Write("b2", At(g_data, 6), nullptr, &one, 1, b) == WXL_OK);
    CHECK(patch::Write("a3", At(g_data, 3), nullptr, &one, 1, a) == WXL_OK);
    CHECK(Is(g_data, { 1, 2, 0xAA, 0xCC, 5, 6, 0xCC, 8 }));

    // Bad arguments.
    CHECK(patch::Write("x", 0, nullptr, &one, 1, a) == WXL_E_INVALID_ARGUMENT);
    CHECK(patch::Write("x", At(g_data), nullptr, nullptr, 1, a) == WXL_E_INVALID_ARGUMENT);
    CHECK(patch::Write("x", At(g_data), nullptr, &one, 0, a) == WXL_E_INVALID_ARGUMENT);
    CHECK(patch::Write("x", 0x10, nullptr, &one, 1, a) == WXL_E_INVALID_ARGUMENT);

    // Disabling a gives back its bytes, newest first; b's stay.
    owners::Disable(a, "test");
    CHECK(Is(g_data, { 1, 2, 3, 4, 5, 6, 0xCC, 8 }));
    CHECK(Is(g_ro, { 0x0F, 0x85, 0x11, 0x01 }));
    CHECK(patch::DropOwner(a) == 0);
    CHECK(patch::DropOwner(b) == 1);
    CHECK(Is(g_data, { 1, 2, 3, 4, 5, 6, 7, 8 }));

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
