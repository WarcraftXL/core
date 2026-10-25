#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 WarcraftXL
#
# Checks extract.py against a small fork made up on the spot.
"""Self-test of the extraction tool: a fork of a few functions, a functions.def, a statics.def."""

import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import extract  # noqa: E402

CMAKE = """project(fork CXX)
set(CMAKE_CXX_STANDARD 17)
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_compile_definitions(WOWDECOMP_ARCH_64=1)
elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_compile_definitions(WOWDECOMP_ARCH_32=1)
    add_compile_definitions(_USE_32BIT_TIME_T)
endif ()
add_compile_definitions(
    WIN32
    NOMINMAX
)
"""

HEADER = """#include <cstdint>
#include <cstddef>

#define STORMAPI __stdcall

struct Data {
    uint32_t m_count;
};

class Thing {
    public:
    /* 0000 */ uint32_t m_flags;
    /* 0004 */ Data* m_data;

    int32_t Outer();
    int32_t Method(uint32_t value);
};

int32_t Helper(uint32_t value);
void* STORMAPI Alloc(size_t bytes, const char* file, int32_t line);
"""

STORM = """inline void Log(const char* format, ...) {
}
"""

SOURCE = """#include "Src/Thing.h"
#include "Engine/Source/Storm/H/storm.h"

// STATIC: 0x00A00000
static uint32_t s_seen = 0;

// OFFSET: 0x00401000
int32_t Helper(uint32_t value) {
    return static_cast<int32_t>(value);
}

// INLINED
int32_t Inner(Thing* thing) {
    s_seen++;
    return Helper(thing->m_flags) + thing->Method(2);
}

// OFFSET: 0x00401200
int32_t Thing::Method(uint32_t value) {
    return static_cast<int32_t>(value + this->m_flags);
}

// OFFSET: 0x00401300
void* STORMAPI Alloc(size_t bytes, const char* file, int32_t line) {
    return nullptr;
}

// OFFSET: 0x00401100
int32_t Thing::Outer() {
    Log("outer %d", m_flags);
    void* block = Alloc(16, __FILE__, __LINE__);
    return Inner(this) + Method(1) + (block ? 1 : 0);
}
"""

FUNCTIONS = """WXL_OFFSET_FUNCTION("Helper", 0x00401000, 0x100, 0x10, "manual", "", "Src/Thing.cpp", 8)
WXL_OFFSET_FUNCTION("Thing::Outer", 0x00401100, 0x100, 0x40, "manual", "", "Src/Thing.cpp", 30)
WXL_OFFSET_FUNCTION("Thing::Method", 0x00401200, 0x100, 0x20, "manual", "", "Src/Thing.cpp", 20)
WXL_OFFSET_FUNCTION("Alloc", 0x00401300, 0x100, 0x20, "manual", "", "Src/Thing.cpp", 25)
"""

STATICS = """WXL_OFFSET_STATIC("s_seen", 0x00A00000, "Src/Thing.cpp", 5)
"""


class Report:
    def __init__(self):
        self.total = self.failures = 0

    def __call__(self, what, ok, detail=""):
        self.total += 1
        if not ok:
            self.failures += 1
        print("%-4s %s%s" % ("ok" if ok else "FAIL", what, "" if ok else "\n     " + detail))


def main():
    report = Report()
    if not extract.default_clang_cl():
        print("test_extract: no clang-cl here; nothing checked")
        return 0
    root = Path(tempfile.mkdtemp(prefix="wxl-extract-"))
    try:
        fork = root / "fork"
        (fork / "Src").mkdir(parents=True)
        (fork / "Engine" / "Source" / "Storm" / "H").mkdir(parents=True)
        (fork / "CMakeLists.txt").write_text(CMAKE)
        (fork / "Src" / "Thing.h").write_text(HEADER)
        (fork / "Src" / "Thing.cpp").write_text(SOURCE)
        (fork / "Engine" / "Source" / "Storm" / "H" / "storm.h").write_text(STORM)
        offsets = root / "offsets"
        (offsets / "statics").mkdir(parents=True)
        (offsets / "functions.def").write_text(FUNCTIONS)
        (offsets / "statics" / "thing.def").write_text(STATICS)
        sdk = root / "sdk"
        (sdk / "engine" / "include" / "wxl" / "engine").mkdir(parents=True)
        (sdk / "engine" / "include" / "wxl" / "engine" / "layouts.def").write_text(
            "WXL_ENGINE_TYPE(Thing, Thing, 0x8, 0x0ULL, 0x10, 0x0ULL)\nWXL_ENGINE_TYPE(Data, Data, 0x4, 0x0ULL, 0x4, 0x0ULL)\n")
        out = root / "Outer.cpp"
        status = extract.main(["Thing::Outer", "--client", str(fork), "--sdk", str(sdk), "--offsets", str(offsets),
                               "--out", str(out), "--binary", ""])
        text = out.read_text() if out.is_file() else ""
        report("the function extracts", status == 0 and text, text)
        report("a member becomes a __fastcall function taking `self` (thiscall's ECX) and a dummy EDX",
               "int32_t __fastcall Thing_Outer(Thing* self, void* /* edx */)" in text, text)
        report("a stock function is called at its address with its convention",
               "inline const auto Helper = reinterpret_cast<int32_t(__cdecl*)(uint32_t)>(0x00401000);" in text, text)
        report("a stdcall one (STORMAPI) keeps __stdcall",
               "reinterpret_cast<void*(__stdcall*)(size_t, const char*, int32_t)>(0x00401300)" in text, text)
        report("a stock member is called with its object in ECX",
               "stock::Thing_Method(self, nullptr, 1)" in text and
               "inline const auto Thing_Method = reinterpret_cast<int32_t(__fastcall*)(Thing*, void*, uint32_t)>(0x00401200);"
               in text, text)
        report("an implicit member read goes through self", 'Log("outer %d", self->m_flags);' in text, text)
        report("an empty function of the release client is an empty local",
               "static inline void Log(const char*, ...) {}" in text, text)
        report("__FILE__ is the stock client's spelling", 'stock::Alloc(16, ".\\\\Thing.cpp", 0)' in text, text)
        report("an // INLINED callee is extracted as a function of its own (de-inlined)",
               "static __declspec(noinline) int32_t __cdecl Inner(Thing* thing)" in text and "Inner(self)" in text, text)
        report("inside it, the stock calls are rewritten too",
               "stock::Helper(thing->m_flags) + stock::Thing_Method(thing, nullptr, 2)" in text, text)
        report("a tagged global is Wow.exe's own", "stock::s_seen++" in text and
               "inline uint32_t& s_seen = *reinterpret_cast<uint32_t*>(0x00A00000);" in text, text)
        report("and the file is for x86 only", "#if !WXL_ENGINE_X86" in text)
        status = extract.main(["Thing::Outer", "--client", str(fork), "--sdk", str(sdk), "--offsets", str(offsets),
                               "--out", str(out), "--binary", "", "--check"])
        report("--check finds it current", status == 0)
        out.write_text(text.replace("stock::Helper", "Helper"))
        status = extract.main(["Thing::Outer", "--client", str(fork), "--sdk", str(sdk), "--offsets", str(offsets),
                               "--out", str(out), "--binary", "", "--check"])
        report("and fails once it differs", status == 1)
        status = extract.main(["0x00401100", "--client", str(fork), "--sdk", str(sdk), "--offsets", str(offsets),
                               "--binary", ""])
        report("an address names the same function", status == 0)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("\n%d case(s), %d failure(s)" % (report.total, report.failures))
    return 1 if report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
