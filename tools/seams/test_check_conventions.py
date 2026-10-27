#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 WarcraftXL
#
# Self-test for check_conventions.py: a stub binary per failure mode, the local backend's own
# parts, and every row of the real table against Wow.exe.
"""A checker nobody has seen fail is not known to work.

Three halves. The first serves a stub binary over a stub MCP -- a handful of synthetic functions
whose convention is known because this file wrote their instructions -- and asserts that the
tool reads each one right and says so when the row disagrees. The second takes the local backend
apart: the objdump parser on a listing of every line shape it has to read, the PE reader, the
reference scan and the stock check on small images this file writes, and the refusals of the
command line. The third points the tool at the real Wow.exe ($WXL_WOW_EXE, or --binary) and
compares its answer for every row against the conventions verified by hand against the
disassembly; it is skipped, not failed, when no binary is named.
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
CHECKER = HERE / "check_conventions.py"

sys.path.insert(0, str(HERE))

import check_conventions as checker

# --- A stub binary --------------------------------------------------------------------------
#
# One entry per function: its address, and its instructions as (size, text). The sizes are what
# the stub MCP prints as bytes and what the tool steps over, so they only have to be consistent.

def body(*instructions):
    return list(instructions)


CDECL_TWO = body((1, "push ebp"), (2, "mov ebp, esp"), (3, "mov eax, dword ptr [ebp + 8]"),
                 (3, "add eax, dword ptr [ebp + 0xc]"), (1, "pop ebp"), (1, "ret"))

STDCALL_TWO = body((1, "push ebp"), (2, "mov ebp, esp"), (3, "mov eax, dword ptr [ebp + 8]"),
                   (3, "add eax, dword ptr [ebp + 0xc]"), (1, "pop ebp"), (3, "ret 8"))

THISCALL_ONE = body((1, "push ebp"), (2, "mov ebp, esp"), (1, "push esi"), (2, "mov esi, ecx"),
                    (3, "mov eax, dword ptr [ebp + 8]"), (3, "mov dword ptr [esi + 4], eax"),
                    (1, "pop esi"), (1, "pop ebp"), (3, "ret 4"))

REGPARM_EDI = body((1, "push ebp"), (2, "mov ebp, esp"), (1, "push esi"),
                   (3, "lea esi, [edi + 0x10]"), (3, "mov eax, dword ptr [ebp + 8]"),
                   (1, "pop esi"), (1, "pop ebp"), (1, "ret"))

# A switch table right after the body: decoded linearly it reads registers the function never
# takes, which is what trimming at the table's address is for.
TABLE_SWITCH = body((3, "mov edx, dword ptr [esi + 0x14]"), (3, "cmp edx, 6"),
                    (2, "ja 0x401306"), (7, "jmp dword ptr [edx*4 + 0x401320]"),
                    (1, "push eax"), (1, "push ecx"), (2, "mov ecx, esi"),
                    (5, "call 0x409840"), (1, "ret"),
                    # From here on it is the table, decoded as whatever the bytes look like.
                    (2, "mov edi, edi"), (3, "add al, byte ptr [eax + 0x40]"),
                    (3, "add byte ptr [esi], dl"), (2, "inc eax"))

# Its caller: pushes two dwords and cleans them itself.
CALLER_CLEANS = body((1, "push ebp"), (2, "mov ebp, esp"), (2, "push 2"), (2, "push 1"),
                     (5, "call 0x401000"), (3, "add esp, 8"), (1, "pop ebp"), (1, "ret"))

# A caller MSVC folded the cleanup out of: it pushes and then adjusts much later.
CALLER_FOLDS = body((1, "push ebp"), (2, "mov ebp, esp"), (2, "push 2"), (2, "push 1"),
                    (5, "call 0x401000"), (2, "push 3"), (5, "call 0x409840"),
                    (3, "add esp, 0xc"), (1, "pop ebp"), (1, "ret"))

# An event handler that never reads the two parameters it is handed. The function alone reads
# no stack at all; only its caller says how many there are.
CDECL_IGNORES = body((1, "push ebp"), (2, "mov ebp, esp"), (5, "mov eax, 1"), (1, "pop ebp"),
                     (1, "ret"))

# Its caller pushes the two and cleans exactly those eight bytes: the evidence that excuses it.
# A call before it bounds the walk back over the pushes, as it does in real code.
CALLER_EXACT = body((1, "push ebp"), (2, "mov ebp, esp"), (5, "call 0x409840"), (2, "push 2"),
                    (2, "push 1"), (5, "call 0x401600"), (3, "add esp, 8"), (1, "pop ebp"),
                    (1, "ret"))

# The same evidence at the first call of its caller, where nothing bounds the walk and it runs
# on into the caller's own prologue: three pushes counted against eight bytes cleaned.
CDECL_IGNORES_FIRST = body((1, "push ebp"), (2, "mov ebp, esp"), (5, "mov eax, 1"), (1, "pop ebp"),
                           (1, "ret"))
CALLER_FIRST = body((1, "push ebp"), (2, "mov ebp, esp"), (2, "push 2"), (2, "push 1"),
                    (5, "call 0x401a00"), (3, "add esp, 8"), (1, "pop ebp"), (1, "ret"))

# The same handler reached only through a folded cleanup, which never shows the exact count.
CDECL_IGNORES_FOLDED = body((1, "push ebp"), (2, "mov ebp, esp"), (5, "mov eax, 1"),
                            (1, "pop ebp"), (1, "ret"))
CALLER_FOLDS_ONLY = body((1, "push ebp"), (2, "mov ebp, esp"), (2, "push 2"), (2, "push 1"),
                         (5, "call 0x401800"), (2, "push 3"), (5, "call 0x409840"),
                         (3, "add esp, 0xc"), (1, "pop ebp"), (1, "ret"))

# Instructions that write a register without reading it. Each saves the register it uses, as
# MSVC does, and sets it to a constant: -1, 0, the carry spread, or a product of something else.
# A rule that took any of them for a read would see an argument arriving in that register.
CDECL_ALL_ONES = body((1, "push ebp"), (2, "mov ebp, esp"), (3, "mov eax, dword ptr [ebp + 8]"),
                      (1, "push edi"), (3, "or edi, 0xffffffff"),
                      (2, "mov dword ptr [eax], edi"), (1, "pop edi"), (1, "pop ebp"), (1, "ret"))
CDECL_ZEROED = body((1, "push ebp"), (2, "mov ebp, esp"), (1, "push esi"), (1, "push ebx"),
                    (3, "and esi, 0"), (2, "sub ebx, ebx"), (2, "sbb edx, edx"),
                    (3, "mov eax, dword ptr [ebp + 8]"), (2, "mov dword ptr [eax], esi"),
                    (3, "mov dword ptr [eax + 4], ebx"), (3, "mov dword ptr [eax + 8], edx"),
                    (1, "pop ebx"), (1, "pop esi"), (1, "pop ebp"), (1, "ret"))
CDECL_SCALED = body((1, "push ebp"), (2, "mov ebp, esp"),
                    (4, "imul ecx, dword ptr [ebp + 8], 0x1c"), (2, "mov eax, ecx"),
                    (1, "pop ebp"), (1, "ret"))
# `or edi, 1` keeps the bits EDI arrived with: that one is a read.
REGPARM_OR_ONE = body((1, "push ebp"), (2, "mov ebp, esp"), (3, "or edi, 1"),
                      (2, "mov eax, edi"), (1, "pop ebp"), (1, "ret"))

# A handler reached only through a table of pointers: it reads none of its three, and nothing
# calls it directly. Its dispatcher pushes one, calls a helper that takes its argument in EAX,
# pushes two more, calls through a pointer and cleans the three.
CDECL_DISPATCHED = body((1, "push ebp"), (2, "mov ebp, esp"), (5, "mov eax, 1"), (1, "pop ebp"),
                        (1, "ret"))
REGPARM_HELPER = body((2, "mov eax, dword ptr [eax]"), (1, "ret"))
DISPATCHER = body((1, "push ebp"), (2, "mov ebp, esp"), (2, "test esi, esi"),
                  (2, "je 0x401f1e"), (2, "push 1"), (3, "mov eax, dword ptr [ebp + 8]"),
                  (5, "call 0x401e00"), (2, "push 2"), (2, "push 3"),
                  (3, "mov ecx, dword ptr [esi + 0x10]"), (2, "call ecx"), (3, "add esp, 0xc"),
                  (1, "pop ebp"), (1, "ret"))


def address_of(start, instructions, text):
    """The address of the first instruction of a stub function that reads `text`."""
    for size, line in instructions:
        if line == text:
            return start
        start += size
    raise KeyError(text)


DISPATCH_CALL = address_of(0x00401F00, DISPATCHER, "call ecx")
DISPATCH_PUSH = address_of(0x00401F00, DISPATCHER, "push 3")

# A function that leaves by a tail jump, so no `ret` says who cleans its two. Its one caller
# pushes them right after a call to a cdecl function of one, and cleans all three at once.
TAIL_TWO = body((1, "push ebp"), (2, "mov ebp, esp"), (3, "mov eax, dword ptr [ebp + 8]"),
                (3, "mov ecx, dword ptr [ebp + 0xc]"), (1, "pop ebp"), (5, "jmp 0x409840"))
CDECL_ONE = body((1, "push ebp"), (2, "mov ebp, esp"), (3, "mov eax, dword ptr [ebp + 8]"),
                 (1, "pop ebp"), (1, "ret"))
TAIL_CALLER = body((1, "push ebp"), (2, "mov ebp, esp"), (2, "push 5"), (5, "call 0x402100"),
                   (2, "push 1"), (2, "push 2"), (5, "call 0x402000"), (3, "add esp, 0xc"),
                   (1, "pop ebp"), (1, "ret"))
TAIL_CALL = address_of(0x00402200, TAIL_CALLER, "call 0x402000")
# MSVC's float argument: `push ecx` makes room, `fstp dword ptr [esp]` fills it; ecx is never
# read. Without the store, the push reads ecx.
FLOAT_SLOT = body((1, "push ebp"), (2, "mov ebp, esp"), (3, "mov eax, dword ptr [ebp + 8]"), (1, "push ecx"),
                  (3, "fld dword ptr [ebp + 0xc]"), (3, "fstp dword ptr [esp]"), (3, "add esp, 4"),
                  (1, "pop ebp"), (1, "ret"))
PUSHED_ECX = body((1, "push ebp"), (2, "mov ebp, esp"), (3, "mov eax, dword ptr [ebp + 8]"), (1, "push ecx"),
                  (3, "add esp, 4"), (1, "pop ebp"), (1, "ret"))

# A function that answers nothing: its one caller writes EAX before reading it. Another whose
# caller tests what it answers.
NO_ANSWER = body((1, "push ebp"), (2, "mov ebp, esp"), (5, "call 0x402100"), (1, "pop ebp"), (1, "ret"))
NO_ANSWER_CALLER = body((1, "push ebp"), (2, "mov ebp, esp"), (5, "call 0x402380"), (3, "mov esi, dword ptr [ebp + 8]"),
                        (2, "je 0x40240d"), (5, "mov eax, 1"), (1, "pop ebp"), (1, "ret"))
ANSWERS = body((1, "push ebp"), (2, "mov ebp, esp"), (5, "mov eax, 7"), (1, "pop ebp"), (1, "ret"))
ANSWER_CALLER = body((1, "push ebp"), (2, "mov ebp, esp"), (5, "call 0x402440"), (2, "test eax, eax"),
                     (1, "pop ebp"), (1, "ret"))

# A member that writes ECX on an early return and reads `this` through it only past the branch
# that skips the write, as CGxDevice::ITexWHDStartEnd (0x006A5F1F, 0x006A5FAE): read straight
# down, ECX looks written before it is read.
BRANCH_ECX = body((1, "push ebp"), (2, "mov ebp, esp"), (3, "mov eax, dword ptr [ebp + 8]"),
                  (2, "test eax, eax"), (2, "jne 0x4024d1"), (3, "mov ecx, dword ptr [ebp + 8]"),
                  (1, "pop ebp"), (3, "ret 4"),
                  # 0x004024D1
                  (6, "mov ecx, dword ptr [ecx + 0x350]"), (1, "pop ebp"), (3, "ret 4"))

PROGRAM = {
    0x00401000: CDECL_TWO,
    0x00401100: STDCALL_TWO,
    0x00401200: THISCALL_ONE,
    0x00401280: REGPARM_EDI,
    0x00401300: TABLE_SWITCH,
    0x00401400: CALLER_CLEANS,
    0x00401500: CALLER_FOLDS,
    0x00401600: CDECL_IGNORES,
    0x00401700: CALLER_EXACT,
    0x00401800: CDECL_IGNORES_FOLDED,
    0x00401900: CALLER_FOLDS_ONLY,
    0x00401A00: CDECL_IGNORES_FIRST,
    0x00401B00: CALLER_FIRST,
    0x00401C00: CDECL_ALL_ONES,
    0x00401C40: CDECL_ZEROED,
    0x00401C80: CDECL_SCALED,
    0x00401CC0: REGPARM_OR_ONE,
    0x00401D00: CDECL_DISPATCHED,
    0x00401E00: REGPARM_HELPER,
    0x00401F00: DISPATCHER,
    0x00402000: TAIL_TWO,
    0x00402100: CDECL_ONE,
    0x00402200: TAIL_CALLER,
    0x00402300: FLOAT_SLOT,
    0x00402340: PUSHED_ECX,
    0x00402380: NO_ANSWER,
    0x00402400: NO_ANSWER_CALLER,
    0x00402440: ANSWERS,
    0x00402480: ANSWER_CALLER,
    0x004024C0: BRANCH_ECX,
}

# (name, address, extent)
SYMBOLS = [("Thing::Plain", 0x00401000, 0x20), ("Thing::Owned", 0x00401100, 0x20),
           ("Thing::Member", 0x00401200, 0x20), ("Thing::Fast", 0x00401280, 0x20),
           ("Thing::Switch", 0x00401300, 0x30), ("Thing::CallsCleanly", 0x00401400, 0x20),
           ("Thing::CallsFolded", 0x00401500, 0x20), ("Thing::Ignores", 0x00401600, 0x20),
           ("Thing::CallsExactly", 0x00401700, 0x20), ("Thing::IgnoresFolded", 0x00401800, 0x20),
           ("Thing::CallsFoldedOnly", 0x00401900, 0x20), ("Thing::IgnoresFirst", 0x00401A00, 0x20),
           ("Thing::CallsFirst", 0x00401B00, 0x20), ("Thing::AllOnes", 0x00401C00, 0x20),
           ("Thing::Zeroed", 0x00401C40, 0x30), ("Thing::Scaled", 0x00401C80, 0x10),
           ("Thing::OrOne", 0x00401CC0, 0x10), ("Thing::Dispatched", 0x00401D00, 0x10),
           ("Thing::Helper", 0x00401E00, 0x10), ("Thing::Dispatcher", 0x00401F00, 0x20),
           ("Thing::Tail", 0x00402000, 0x10), ("Thing::One", 0x00402100, 0x10),
           ("Thing::TailCaller", 0x00402200, 0x20), ("Thing::FloatSlot", 0x00402300, 0x20),
           ("Thing::PushedEcx", 0x00402340, 0x20), ("Thing::NoAnswer", 0x00402380, 0x20),
           ("Thing::NoAnswerCaller", 0x00402400, 0x20), ("Thing::Answers", 0x00402440, 0x20),
           ("Thing::AnswerCaller", 0x00402480, 0x20), ("Thing::BranchEcx", 0x004024C0, 0x20)]


def flatten():
    """address -> (size, text), for every instruction of every stub function."""
    memory = {}
    for start, instructions in PROGRAM.items():
        address = start
        for size, text in instructions:
            memory[address] = (size, text)
            address += size
    return memory


MEMORY = flatten()
# The direct call sites, found by walking the stub functions; the tool reads them through
# ghidra_xrefs.
XREFS = {}
for _address, (_, _text) in sorted(MEMORY.items()):
    if _text.startswith("call 0x"):
        XREFS.setdefault(int(_text[5:], 16), []).append("%08x(UNCONDITIONAL_CALL)" % _address)


class Stub(BaseHTTPRequestHandler):
    """Answers ghidra_disasm and ghidra_xrefs out of PROGRAM, the way the real MCP does."""

    def log_message(self, *_):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        message = json.loads(self.rfile.read(length) or b"{}")
        name = message.get("params", {}).get("name")
        arguments = message.get("params", {}).get("arguments", {})
        if name == "ghidra_disasm":
            text = self.disasm(int(arguments["addr"], 16), int(arguments.get("count", 16)))
        elif name == "ghidra_xrefs":
            address = int(arguments["addr"], 16)
            text = "to 0x%08X (%d): %s\n\nfrom 0x%08X (0): -" % (
                address, len(XREFS.get(address, [])),
                ", ".join(XREFS.get(address, [])) or "-", address)
        else:
            text = ""
        payload = json.dumps({"jsonrpc": "2.0", "id": message.get("id"),
                              "result": {"content": [{"type": "text", "text": text}],
                                         "isError": False}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    @staticmethod
    def disasm(address, count):
        lines = []
        while len(lines) < count and address in MEMORY:
            size, text = MEMORY[address]
            lines.append("%08x  %s   %s" % (address, " ".join(["90"] * size), text))
            address += size
        return "\n".join(lines)


# --- The synthetic tree ---------------------------------------------------------------------

SEAMS_DEF = '''// A synthetic seam table.
#ifndef WXL_SEAM
#error "define WXL_SEAM(domain, name, symbol, ret, conv, params) before including seams.def"
#endif
#include "wxl/seams/demo.def"
'''

DEMO_DEF = '''// The demo domain.
#ifndef WXL_SEAM
#error "define WXL_SEAM(domain, name, symbol, ret, conv, params) before including demo.def"
#endif
WXL_SEAM(Demo, Plain, "Thing::Plain", void, cdecl, (int32_t a, int32_t b))
WXL_SEAM(Demo, Owned, "Thing::Owned", void, stdcall, (int32_t a, int32_t b))
WXL_SEAM(Demo, Member, "Thing::Member", void, thiscall, (void* self, int32_t a))
WXL_SEAM(Demo, BranchEcx, "Thing::BranchEcx", void, thiscall, (void* self, int32_t a))
WXL_SEAM(Demo, Fast, "Thing::Fast", void, regparm, (void* context, int32_t a))
WXL_SEAM(Demo, Switch, "Thing::Switch", void, regparm, (void* object, const void* create, uint32_t time))
WXL_SEAM(Demo, AllOnes, "Thing::AllOnes", void, cdecl, (int32_t* out))
WXL_SEAM(Demo, Zeroed, "Thing::Zeroed", void, cdecl, (int32_t* out))
WXL_SEAM(Demo, Scaled, "Thing::Scaled", int32_t, cdecl, (int32_t count))
'''

DISPATCHED = ('WXL_SEAM(Demo, Dispatched, "Thing::Dispatched", void, cdecl, '
              '(int32_t a, int32_t b, int32_t c))\n')
DISPATCHED_WITNESS = "WXL_SEAM_WITNESS(Demo, Dispatched, 0x%08X)\n" % DISPATCH_CALL
TAIL = 'WXL_SEAM(Demo, Tail, "Thing::Tail", void, cdecl, (int32_t a, int32_t b))\n'
TAIL_WITNESS = "WXL_SEAM_WITNESS(Demo, Tail, 0x%08X)\n" % TAIL_CALL

THUNKS = '''#include "seams/RegparmX86.hpp"

__declspec(naked) static void FastThunk()
{
    __asm {
        push ebp
        mov  ebp, esp
        push edi
        push dword ptr [ebp-04h]
        call dword ptr [g_fastBefore]
        add  esp, 04h
        mov  edi, dword ptr [ebp-04h]
        call dword ptr [g_fastOriginal]
        ret
    }
}

__declspec(naked) static void SwitchThunk()
{
    __asm {
        push ebp
        mov  ebp, esp
        push esi
        push eax
        push ecx
        push dword ptr [ebp-04h]
        call dword ptr [g_switchBefore]
        add  esp, 04h
        mov  esi, dword ptr [ebp-04h]
        mov  eax, dword ptr [ebp-08h]
        mov  ecx, dword ptr [ebp-0Ch]
        call dword ptr [g_switchOriginal]
        ret
    }
}

namespace
{
    const wxl::core::seams::regparm::Row kRows[] = {
        { "Demo.Fast", reinterpret_cast<void*>(&FastThunk), &g_fastOriginal },
        { "Demo.Switch", reinterpret_cast<void*>(&SwitchThunk), &g_switchOriginal },
    };
}
'''


def functions_def():
    lines = ["// A synthetic offsets table."]
    for name, address, extent in SYMBOLS:
        lines.append('WXL_OFFSET_FUNCTION("%s", 0x%08X, 0x%X, 0x%X, "manual", "", "stub.cpp", 1)'
                     % (name, address, extent, extent))
    return "\n".join(lines) + "\n"


def build(case, demo_def=DEMO_DEF, thunks=THUNKS):
    for sub in ("seams", "offsets", "src"):
        (case / sub).mkdir(parents=True, exist_ok=True)
    (case / "seams" / "seams.def").write_text(SEAMS_DEF, encoding="utf-8")
    (case / "seams" / "demo.def").write_text(demo_def, encoding="utf-8")
    (case / "offsets" / "functions.def").write_text(functions_def(), encoding="utf-8")
    (case / "src" / "RegparmX86.cpp").write_text(thunks, encoding="utf-8")


def run(case, extra=(), env=None):
    return subprocess.run(
        [sys.executable, "-I", str(CHECKER),
         "--seams", str(case / "seams"), "--offsets", str(case / "offsets"),
         "--thunks", str(case / "src" / "RegparmX86.cpp"), *extra],
        capture_output=True, text=True, env=env)


# (label, demo.def, RegparmX86.cpp, exit code, text expected, extra arguments)
CASES = [
    ("a correct table", DEMO_DEF, THUNKS, 0, None, ()),
    ("a regparm function declared cdecl",
     DEMO_DEF.replace('"Thing::Fast", void, regparm,', '"Thing::Fast", void, cdecl,'),
     THUNKS, 1, "declared cdecl, but 0x00401280 is regparm (EDI) + 1 stack dword(s)", ()),
    ("a thiscall function declared cdecl",
     DEMO_DEF.replace('"Thing::Member", void, thiscall, (void* self, int32_t a)',
                      '"Thing::Member", void, cdecl, (int32_t a)'),
     THUNKS, 1, "declared cdecl, but 0x00401200 is thiscall (ECX) + 1 stack dword(s)", ()),
    ("a member reading this only past a branch, declared stdcall",
     DEMO_DEF.replace('"Thing::BranchEcx", void, thiscall, (void* self, int32_t a)',
                      '"Thing::BranchEcx", void, stdcall, (int32_t a)'),
     THUNKS, 1, "declared stdcall, but 0x004024C0 is thiscall (ECX) + 1 stack dword(s)", ()),
    ("a stdcall function declared cdecl",
     DEMO_DEF.replace('"Thing::Owned", void, stdcall,', '"Thing::Owned", void, cdecl,'),
     THUNKS, 1, "declared cdecl, but 0x00401100 is stdcall, 2 stack dword(s)", ()),
    ("a cdecl function declared stdcall",
     DEMO_DEF.replace('"Thing::Plain", void, cdecl,', '"Thing::Plain", void, stdcall,'),
     THUNKS, 1, "declared stdcall, but 0x00401000 is cdecl, 2 stack dword(s)", ()),
    ("a parameter dropped",
     DEMO_DEF.replace('"Thing::Plain", void, cdecl, (int32_t a, int32_t b)',
                      '"Thing::Plain", void, cdecl, (int32_t a)'),
     THUNKS, 1, "the row's parameters fill 1 stack dword(s), but 0x00401000 reads 2", ()),
    ("a parameter invented",
     DEMO_DEF.replace('"Thing::Member", void, thiscall, (void* self, int32_t a)',
                      '"Thing::Member", void, thiscall, (void* self, int32_t a, int32_t b)'),
     THUNKS, 1, "the row's parameters fill 2 stack dword(s), but 0x00401200 reads 1", ()),
    ("a uint64 counted as one dword",
     DEMO_DEF.replace('"Thing::Plain", void, cdecl, (int32_t a, int32_t b)',
                      '"Thing::Plain", void, cdecl, (uint64_t guid)'),
     THUNKS, 0, None, ()),
    ("a regparm row with the wrong number of registers",
     DEMO_DEF.replace('"Thing::Switch", void, regparm, (void* object, const void* create, '
                      'uint32_t time)',
                      '"Thing::Switch", void, regparm, (void* object, const void* create)'),
     THUNKS, 1, "takes 3 in registers (ESI, EAX, ECX) and 0 on the stack", ()),
    ("a regparm row with no thunk", DEMO_DEF,
     THUNKS.replace('        { "Demo.Fast", reinterpret_cast<void*>(&FastThunk), '
                    '&g_fastOriginal },\n', ""),
     1, "declared regparm, but RegparmX86.cpp has no thunk for it", ()),
    ("a thunk for a row that is not regparm",
     DEMO_DEF.replace('"Thing::Fast", void, regparm,', '"Thing::Fast", void, cdecl,'),
     THUNKS, 1, "has a thunk for Demo.Fast, but the row declares it cdecl", ()),
    ("a thunk for a row nobody declares", DEMO_DEF,
     THUNKS.replace('{ "Demo.Fast",', '{ "Demo.Ghost",'),
     1, "has a thunk for Demo.Ghost, which no row declares", ()),
    ("a thunk restoring the wrong register", DEMO_DEF,
     THUNKS.replace("        mov  edi, dword ptr [ebp-04h]", "        mov  esi, dword ptr [ebp-04h]"),
     1, "its thunk restores ESI before calling the original, but 0x00401280 reads EDI", ()),
    ("a symbol the offsets table does not carry",
     DEMO_DEF.replace('"Thing::Plain"', '"Thing::Vanished"'),
     THUNKS, 1, "no function named Thing::Vanished in functions.def", ()),
    # A cdecl event handler that ignores its parameters reads none of them, but they are on the
    # stack: the caller pushed them and takes them back. The caller's count decides -- and only
    # an exact one does.
    ("a cdecl handler that ignores what its caller hands it",
     DEMO_DEF + 'WXL_SEAM(Demo, Ignores, "Thing::Ignores", void, cdecl, '
                '(const void* data, void* param))\n',
     THUNKS, 0, None, ()),
    ("the same, with no caller that cleans exactly what it declares",
     DEMO_DEF + 'WXL_SEAM(Demo, IgnoresFolded, "Thing::IgnoresFolded", void, cdecl, '
                '(const void* data, void* param))\n',
     THUNKS, 1, "the row's parameters fill 2 stack dword(s), but 0x00401800 reads 0", ()),
    ("a parameter invented on a handler that ignores its own",
     DEMO_DEF + 'WXL_SEAM(Demo, Ignores, "Thing::Ignores", void, cdecl, '
                '(const void* data, void* param, int32_t extra))\n',
     THUNKS, 1, "the row's parameters fill 3 stack dword(s), but 0x00401600 reads 0", ()),
    ("the same evidence at a caller's first call, which the walk over-counts",
     DEMO_DEF + 'WXL_SEAM(Demo, IgnoresFirst, "Thing::IgnoresFirst", void, cdecl, '
                '(const void* data, void* param))\n',
     THUNKS, 1, "pushes 3 and cleans 8", ()),
    # Writes that do not read: `or r, -1`, `and r, 0`, `sub r, r`, `sbb r, r` and the
    # three-operand `imul` leave the table above agreeing; `or r, 1` is still a read.
    ("`or r, 1` reads the register it sets",
     DEMO_DEF + 'WXL_SEAM(Demo, OrOne, "Thing::OrOne", int32_t, cdecl, ())\n',
     THUNKS, 1, "declared cdecl, but 0x00401CC0 is regparm (EDI) + 0 stack dword(s)", ()),
    ("a `push ecx` that only makes room for a float stored there is no read of ecx",
     DEMO_DEF + 'WXL_SEAM(Demo, FloatSlot, "Thing::FloatSlot", void, cdecl, (int32_t a, float b))\n',
     THUNKS, 0, "agree", ()),
    ("a `push ecx` whose slot nothing fills reads ecx",
     DEMO_DEF + 'WXL_SEAM(Demo, PushedEcx, "Thing::PushedEcx", void, cdecl, (int32_t a))\n',
     THUNKS, 1, "declared cdecl, but 0x00402340 is thiscall (ECX)", ()),
    # An answer the After is handed: the function must give one. CMapArea::PrepareChunk answers
    # nothing on the stock client, and an After reading EAX as the chunk read garbage.
    ("a row answering a pointer for a function no caller reads an answer of",
     DEMO_DEF + 'WXL_SEAM(Demo, NoAnswer, "Thing::NoAnswer", void*, cdecl, ())\n',
     THUNKS, 1, "no caller of 0x00402380 reads EAX after it (0x00402403): the function answers nothing", ()),
    ("the same function declared void",
     DEMO_DEF + 'WXL_SEAM(Demo, NoAnswer, "Thing::NoAnswer", void, cdecl, ())\n',
     THUNKS, 0, None, ()),
    ("a function whose caller tests its answer",
     DEMO_DEF + 'WXL_SEAM(Demo, Answers, "Thing::Answers", int32_t, cdecl, ())\n',
     THUNKS, 0, None, ()),
    ("a Before alone is handed no answer",
     DEMO_DEF + 'WXL_SEAM_BEFORE(Demo, NoAnswer, "Thing::NoAnswer", void*, cdecl, ())\n',
     THUNKS, 0, None, ()),
    # A witness: a call site the row declares, read like a found one.
    ("a handler reached only through a pointer, without a witness",
     DEMO_DEF + DISPATCHED, THUNKS, 1,
     "the row's parameters fill 3 stack dword(s), but 0x00401D00 reads 0", ()),
    ("the same, with its dispatcher's indirect call as its witness",
     DEMO_DEF + DISPATCHED + DISPATCHED_WITNESS, THUNKS, 0, "pushes all 3 and cleans them",
     ("-v",)),
    ("a witness that hands it fewer than the row declares",
     DEMO_DEF + DISPATCHED.replace("int32_t c)", "int32_t c, int32_t d)") + DISPATCHED_WITNESS,
     THUNKS, 1, "the row's parameters fill 4 stack dword(s), but its witness hands it 3", ()),
    ("a witness that is not a call",
     DEMO_DEF + DISPATCHED + "WXL_SEAM_WITNESS(Demo, Dispatched, 0x%08X)\n" % DISPATCH_PUSH,
     THUNKS, 1, "its witness at 0x%08X cannot be read: the instruction there is `push 3`"
     % DISPATCH_PUSH, ()),
    ("a witness for a row nobody declares",
     DEMO_DEF + "WXL_SEAM_WITNESS(Demo, Ghost, 0x%08X)\n" % DISPATCH_CALL,
     THUNKS, 1, "a witness for Demo.Ghost, which no row declares", ()),
    ("a witness on a row its caller does not clean for",
     DEMO_DEF + "WXL_SEAM_WITNESS(Demo, Owned, 0x00401404)\n",
     THUNKS, 1, "a witness proves a call its caller cleans for, but the row declares stdcall",
     ()),
    ("a function that leaves by a tail jump, without a witness",
     DEMO_DEF + TAIL, THUNKS, 0, "the binary does not settle the convention", ("-v",)),
    ("the same, with its caller as its witness, the cleanup folding the call before",
     DEMO_DEF + TAIL + TAIL_WITNESS, THUNKS, 0, "takes them back, so it is cdecl", ("-v",)),
    ("the same, declared stdcall",
     DEMO_DEF + TAIL.replace("cdecl", "stdcall") + TAIL_WITNESS, THUNKS, 1,
     "a witness proves a call its caller cleans for, but the row declares stdcall", ()),
]


# A recompiled parent body (src/deinlined/<Parent>.gen.cpp) and the point table's row of its
# parent: Thing::CallsCleanly, which calls Thing::Plain. The body names what it calls in its
# `namespace stock`, as extract.py writes it.
PARENT_X86_DEF = ('WXL_POINT_X86_PARENT(Thing_CallsCleanly, "Thing::CallsCleanly", 0x00401400, 32, cdecl, '
                  'void, ())\n')
BODY = '''namespace wxl::core::deinlined
{
    namespace stock
    {
        inline const auto Thing_Plain = reinterpret_cast<void(__cdecl*)(int32_t, int32_t)>(0x00401000); // Thing::Plain
    }
}
'''
GLOBAL_LINE = "        inline uint32_t& s_count = *reinterpret_cast<uint32_t*>(0x00D41258);\n"

# (label, body, exit code, text expected)
BODY_CASES = [
    ("a recompiled body calling what Wow.exe's own calls", BODY, 0, "1 recompiled body(ies) calling 1"),
    ("a recompiled body calling a function Wow.exe's own does not",
     BODY.replace("reinterpret_cast<void(__cdecl*)(int32_t, int32_t)>(0x00401000); // Thing::Plain",
                  "reinterpret_cast<void(__stdcall*)(int32_t, int32_t)>(0x00401100); // Thing::Owned"),
     1, "calls Thing::Owned at 0x00401100 (Thing_Plain), which Wow.exe's own body at 0x00401400 does not call"),
    ("the same body leaving out the call Wow.exe's own makes",
     BODY.replace("reinterpret_cast<void(__cdecl*)(int32_t, int32_t)>(0x00401000); // Thing::Plain",
                  "reinterpret_cast<void(__stdcall*)(int32_t, int32_t)>(0x00401100); // Thing::Owned"),
     1, "Wow.exe's own body at 0x00401400 calls 0x00401000, which the recompiled one does not call"),
    ("a recompiled body calling Wow.exe's function with another convention",
     BODY.replace("void(__cdecl*)", "void(__stdcall*)"), 1, "declared stdcall, but 0x00401000 is cdecl"),
    ("a recompiled body naming a global Wow.exe's own never reaches",
     BODY.replace("    }\n}", GLOBAL_LINE + "    }\n}", 1), 1,
     "names s_count at 0x00D41258, which Wow.exe's own body at 0x00401400 never reaches"),
]


# --- The local backend's parts ----------------------------------------------------------------

# Every line shape llvm-objdump 19 prints for Wow.exe: the headers, a short and a long encoding
# (the long one runs into the tab), a symbolised branch, a negative immediate, a scaled index, a
# prefix on its own line, a prefix before its mnemonic, and bytes it cannot decode.
OBJDUMP_LISTING = """
Wow.exe:\tfile format coff-i386

Disassembly of section .text:

00401000 <.text>:
  806200: 55                           \tpush\tebp
  806201: 8b ec                        \tmov\tebp, esp
  806210: e8 8b 1b 0b 00               \tcall\t0x8b7da0 <.text+0x4b6da0>
  8062ac: f7 85 50 fd ff ff 04 04 00 00\ttest\tdword ptr [ebp - 0x2b0], 0x404
  42597c: c7 84 85 ac fe ff ff 00 00 00 00     \tmov\tdword ptr [ebp + 4*eax - 0x154], 0x0
  8062b8: 83 cf ff                     \tor\tedi, -0x1
  40472e: f0                           \tlock
  40472f: 0f b1 0e                     \tcmpxchg\tdword ptr [esi], ecx
  40bbdf: f3 ab                        \trep\t\tstosd\tdword ptr es:[edi], eax
  40cc3e: 8f e4 8b                     \t<unknown>
  690220: c2 04 00                     \tret\t0x4
"""

OBJDUMP_EXPECTED = [
    (0x806200, 1, "push", "ebp"),
    (0x806201, 2, "mov", "ebp, esp"),
    (0x806210, 5, "call", "0x8b7da0"),
    (0x8062AC, 10, "test", "dword ptr [ebp - 0x2b0], 0x404"),
    (0x42597C, 11, "mov", "dword ptr [ebp + eax*4 - 0x154], 0x0"),
    (0x8062B8, 3, "or", "edi, -0x1"),
    (0x40472E, 4, "cmpxchg", "dword ptr [esi], ecx"),
    (0x40BBDF, 2, "stosd", "dword ptr es:[edi], eax"),
    (0x40CC3E, 3, "(bad)", ""),
    (0x690220, 3, "ret", "0x4"),
]

# A small image of real machine code at 0x00401000: a cdecl function of two, a caller that
# pushes two and cleans them, a jump to the function, and a stray E8 that targets nowhere.
SYNTHETIC_CODE = bytes.fromhex(
    "55 8b ec 8b 45 08 03 45 0c 5d c3"          # 0x401000 push ebp; ...; pop ebp; ret
    "cc cc cc cc cc"                            # 0x40100B padding
    "55 8b ec 6a 02 6a 01 e8 e4 ff ff ff"       # 0x401010 push 2; push 1; call 0x401000
    "83 c4 08 5d c3"                            # 0x40101C add esp, 8; pop ebp; ret
    "e9 da ff ff ff"                            # 0x401021 jmp 0x401000
    "e8 00 00 00 40")                           # 0x401026 bytes that decode as a far call


def write_pe(path, code, timestamp=0x12345678, text_size=None):
    """A PE32 image with one .text section holding `code` at 0x00401000."""
    text_size = text_size or len(code)
    raw_size = (text_size + 0x1FF) & ~0x1FF
    header = bytearray(0x200)
    header[0:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, 0x40)
    header[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", header, 0x44, 0x14C, 1, timestamp, 0, 0, 0xE0, 0x102)
    optional = 0x58
    struct.pack_into("<H", header, optional, 0x10B)
    struct.pack_into("<I", header, optional + 16, 0x1000)                 # entry point
    struct.pack_into("<I", header, optional + 28, 0x00400000)             # image base
    struct.pack_into("<II", header, optional + 32, 0x1000, 0x200)         # alignments
    struct.pack_into("<I", header, optional + 56, 0x1000 + ((text_size + 0xFFF) & ~0xFFF))
    struct.pack_into("<I", header, optional + 60, 0x200)                  # headers
    struct.pack_into("<I", header, optional + 92, 16)
    section = optional + 0xE0
    header[section:section + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", header, section + 8, text_size, 0x1000, raw_size, 0x200)
    struct.pack_into("<I", header, section + 36, 0x60000020)
    body = code + b"\xcc" * (raw_size - len(code))
    Path(path).write_bytes(bytes(header) + body)


def local_cases(temp, report):
    """The objdump parser, the PE reader, the reference scan, the stock check, the refusals."""
    got = [(i.addr, i.size, i.mn, i.ops) for i in checker.parse_objdump(OBJDUMP_LISTING)]
    report("the objdump parser reads every line shape", got == OBJDUMP_EXPECTED,
                      "\n".join(map(str, OBJDUMP_EXPECTED)), "\n".join(map(str, got)))

    # The instruction rules, on objdump's own spelling of them.
    for label, line, reads in [
            ("`or r, -1` writes without reading", "or\tedi, -0x1", set()),
            ("`or r, 0xffffffff` writes without reading", "or\tedi, 0xffffffff", set()),
            ("`or r8, 0xff` writes without reading", "or\tal, 0xff", set()),
            ("`and r, 0` writes without reading", "and\tesi, 0x0", set()),
            ("`sub r, r` writes without reading", "sub\tebx, ebx", set()),
            ("`imul r, m, imm` reads only its source", "imul\tecx, dword ptr [edx + 0x8], 0x1c",
             {"edx"}),
            ("`or r, 1` reads", "or\tedi, 0x1", {"edi"}),
            ("`and r, 0xff` reads", "and\tesi, 0xff", {"esi"})]:
        listing = "  401000: 90 90 90\t%s\n  401003: c3\tret\n" % line
        facts = checker.read_callee(checker.parse_objdump(listing))
        report(label, set(facts.reg_args) == reads, sorted(reads),
                          str(facts.reg_args))

    objdump = checker.find_objdump(None)
    image = Path(temp) / "synthetic.exe"
    write_pe(image, SYNTHETIC_CODE)

    try:
        checker.LocalBinary(image, objdump or "llvm-objdump")
        ok, said = False, "it was accepted"
    except checker.Unusable as trouble:
        said = str(trouble)
        ok = "is not Wow.exe 3.3.5a build 12340" in said
    report("an image that is not 12340 is refused", ok, "a refusal naming the build",
                      said)

    stock_like = Path(temp) / "patched.exe"
    write_pe(stock_like, SYNTHETIC_CODE, checker.STOCK["timestamp"], checker.STOCK["text"][1])
    try:
        checker.LocalBinary(stock_like, objdump or "llvm-objdump")
        ok, said = False, "it was accepted"
    except checker.Unusable as trouble:
        said = str(trouble)
        ok = "its .text is not the stock one" in said
    report("12340's headers over other code are refused", ok,
                      "a refusal naming .text", said)

    unpatched = Path(temp) / "unpatched.exe"
    write_pe(unpatched, SYNTHETIC_CODE)
    binary = checker.LocalBinary(unpatched, objdump or "llvm-objdump", stock=None)
    refs = binary.xrefs(0x00401000)
    report("the reference scan finds the call and the jump, and only them",
                      refs == [(0x00401017, "call"), (0x00401021, "jump")],
                      "[(0x401017, call), (0x401021, jump)]", str(refs))

    if objdump is None:
        print("skip the decoding half of the local backend: no llvm-objdump on PATH")
        return
    insns = binary.disasm(0x00401000, 0x0040100B)
    facts = checker.read_callee(insns)
    report("a function decoded by objdump reads as cdecl, 2 stack dword(s)",
           facts.convention == "cdecl" and facts.stack_dwords == 2,
           "cdecl 2", "%s %d (%s)" % (facts.convention, facts.stack_dwords, insns))
    functions = checker.Functions()
    functions._extent = {0x00401000: 0x10, 0x00401010: 0x11}
    functions._starts = sorted(functions._extent)
    site = checker.read_site(binary, functions, 0x00401000, 0x00401017)
    # Its caller's first call: the walk back runs into the prologue and counts its `push ebp`.
    report("its caller, decoded by objdump, pushes 3 counting its prologue and cleans 8",
           (site.note, site.pushes, site.cleanup) == (None, 3, 8),
           "(None, 3, 8)", str((site.note, site.pushes, site.cleanup)))

    # The command line refuses rather than guesses.
    case = Path(temp) / "refusals"
    build(case)
    env = {k: v for k, v in os.environ.items() if k != "WXL_WOW_EXE"}
    for label, extra, wanted in [
            ("no binary named", (), "no Wow.exe was named"),
            ("a binary that is not 12340", ("--binary", str(image)),
             "is not Wow.exe 3.3.5a build 12340"),
            ("a binary that does not exist", ("--binary", str(Path(temp) / "absent.exe")),
             "cannot read"),
            ("--cache without the MCP", ("--binary", str(image), "--cache", "x.json"),
             "they need --backend mcp")]:
        result = run(case, extra, env)
        output = result.stdout + result.stderr
        report("the local backend refuses %s" % label,
               result.returncode == 2 and wanted in output, "exit 2, with %r" % wanted, output)


# --- The real client ------------------------------------------------------------------------

# What the real binary must say, verified by hand against the disassembly.
# point: (address, convention, registers, stack dwords)
GROUND_TRUTH = {
    "World.Preload":                   ("0x00781500", "cdecl", [], 2),
    # CGGameUI::EnterWorld and LeaveWorld open with `push ebp; mov ebp,esp`, read no argument
    # and no register before the m_inWorld test (0x00528019, 0x00528C39), and end in bare `ret`s.
    "World.EnterWorld":                ("0x00528010", "cdecl", [], 0),
    "World.LeaveWorld":                ("0x00528C30", "cdecl", [], 0),
    "Event.DispatchUpdate":            ("0x0047DB70", "regparm", ["edi"], 2),
    "ObjectMgr.InitObject":            ("0x004D3FF0", "regparm", ["esi", "eax", "ecx"], 0),
    "ObjectMgr.ObjDelete":             ("0x004D6FC0", "regparm", ["eax"], 0),
    "ObjectMgr.DestroyRemovedObject":  ("0x004D4090", "regparm", ["esi"], 0),
    "ObjectMgr.GetUpdateObject":       ("0x004D6AE0", "cdecl", [], 3),
    # The shared side of the object manager: neither reads anything on the stack. InitializeShared
    # has no frame and ends in `ret` at 0x004D4B20, called with nothing pushed at 0x004055D4;
    # DestroyShared reads only [ebp-N] and ends in `ret` at 0x004D5D6A, called at 0x0040665F.
    "ObjectMgr.InitializeShared":      ("0x004D4AC0", "cdecl", [], 0),
    "ObjectMgr.DestroyShared":         ("0x004D5C80", "cdecl", [], 0),
    "Unit.OnDeath":                    ("0x00729220", "thiscall", ["ecx"], 0),
    "Unit.OnAuraAdded":                ("0x00727760", "thiscall", ["ecx"], 4),
    "Unit.OnAuraRemoved":              ("0x00722090", "thiscall", ["ecx"], 4),
    "Player.UpdateInventoryComponent": ("0x006E1E10", "thiscall", ["ecx"], 2),
    "GameUI.SendTarget":               ("0x00518DC0", "cdecl", [], 2),
    "GameUI.ClearTarget":              ("0x005241B0", "cdecl", [], 3),
    "MapArea.PrepareChunk":            ("0x007D6B30", "thiscall", ["ecx"], 2),
    # CMapChunk::Purge moves ECX to EDI (0x007C3377), reads no stack and ends in a bare `ret`
    # (0x007C35CE).
    "MapChunk.Purge":                  ("0x007C3370", "thiscall", ["ecx"], 0),
    "FrameScript.Initialize":          ("0x00819BB0", "cdecl", [], 1),
    "FrameScript.Destroy":             ("0x0081A9A0", "cdecl", [], 0),
    "FrameScript.CreateEvents":        ("0x0081B5F0", "cdecl", [], 2),
    "NetClient.ProcessMessage":        ("0x00631FE0", "thiscall", ["ecx"], 3),
    "NetClient.Send":                  ("0x00632B50", "thiscall", ["ecx"], 1),
    "NetClient.HandleConnect":         ("0x00632510", "thiscall", ["ecx"], 0),
    "NetClient.HandleDisconnect":      ("0x00632540", "thiscall", ["ecx"], 0),
    "Input.WindowProc":                ("0x0086A210", "cdecl", [], 4),
    "Storage.OpenFile":                ("0x00424B50", "stdcall", [], 4),
    "Client.Destroy":                  ("0x00402910", "cdecl", [], 0),
    "Sound.PlaySoundKit":              ("0x004C6A40", "cdecl", [], 8),
    # The Render family, read by hand before it went in. OnPaint reads none of its two: they are
    # pushed and cleaned by its caller (0x004A8BA0 `push 0; push 0; call; add esp, 8`), which is
    # why its row declares them and the binary reports none. GxScenePresent reads no [ebp+N].
    # IReleaseD3dResources takes `this` in ECX and one at [ebp+8], and ends in `ret 4`.
    # NotifyOnDeviceRestored and OnLayerRender read ECX before writing it and nothing on the
    # stack. RenderWorld reads its one at 0x004FB02C and ends in a bare `ret` at 0x004FB078.
    "Scrn.OnPaint":                    ("0x004A8720", "cdecl", [], 0),
    "Gx.ScenePresent":                 ("0x00682A00", "cdecl", [], 0),
    "Gx.ReleaseD3dResources":          ("0x00690150", "thiscall", ["ecx"], 1),
    "Gx.NotifyOnDeviceRestored":       ("0x006843B0", "thiscall", ["ecx"], 0),
    "SimpleTop.OnLayerRender":         ("0x00495410", "thiscall", ["ecx"], 0),
    "WorldFrame.RenderWorld":          ("0x004FAF90", "cdecl", [], 1),
    # The world renderer's points (V6). RenderDetailDoodads opens a frame of its own and reads no
    # [ebp+N]; RenderLiquidPass has no frame, reads nothing on the stack and ends in a tail `jmp`
    # to CMap::RenderWaterRipples (0x00790AA7). Both are class statics reached through thunks.
    "WorldScene.RenderDetailDoodads":  ("0x007984A0", "cdecl", [], 0),
    "WorldScene.RenderLiquidPass":     ("0x00790A80", "cdecl", [], 0),
    # Reads its title alone; the two flags are pushed and cleaned by its one caller
    # (0x00406B28 `push 1; push 1; push eax; call; add esp, 0xc`).
    "ConsoleDevice.Initialize":        ("0x0076AB80", "cdecl", [], 1),
    # The Spell, Item and UI rows. The two packet handlers are reached only through the handler
    # table and read their message at [ebp+0x14]; Spell_C_SpellFailed reads its cast at [ebp+8]
    # and its last argument at [ebp+0x1C]; Spell_C_CastSpell its caster at [ebp+8], the guid
    # two dwords, `confirmed` at [ebp+0x20]. UseItem takes `this` in ECX (0x00708C2A) and two
    # on the stack. vFrameScript_SignalEvent reads its index at [ebp+8] and the va_list at
    # [ebp+0x10]; FrameScript_HandleError is a lua_CFunction, the state at [ebp+8].
    "Spell.Packet_Group_30":           ("0x0080FEE0", "cdecl", [], 4),
    "Spell.Packet_SMSG_SPELL_FAILURE": ("0x00809C70", "cdecl", [], 4),
    "Spell.SpellFailed":               ("0x00808200", "cdecl", [], 6),
    "Spell.CastSpell":                 ("0x0080CCE0", "cdecl", [], 7),
    "Item.UseItem":                    ("0x00708C20", "thiscall", ["ecx"], 2),
    "FrameScript.vSignalEvent":        ("0x0081AC90", "cdecl", [], 3),
    "FrameScript.HandleError":         ("0x00819730", "cdecl", [], 1),
    # The channel, resume, delay and cooldown handlers, and the melee one, are reached only
    # through the handler table (Spell_C__SystemInitialize 0x008101BA-0x008102A5,
    # UnitCombatClientInitialize 0x00756BE3-) and read their message at [ebp+0x14]; each ends
    # in a bare `ret`. Spell_C_CancelSpell reads its cast at [ebp+8], its three flags up to
    # [ebp+0x14], ends in `ret` at 0x00806389, and its caller 0x008063C8 pushes 4 and cleans
    # 0x10; the `or edi, -1` at 0x008062B8 writes EDI after the `push edi` at 0x00806245.
    "Spell.Packet_MSG_CHANNEL_START":  ("0x00801C90", "cdecl", [], 4),
    "Spell.Packet_MSG_CHANNEL_UPDATE": ("0x00801DB0", "cdecl", [], 4),
    "Spell.Packet_SMSG_UNIT_SPELLCAST_START": ("0x008020C0", "cdecl", [], 4),
    "Spell.Packet_SMSG_SPELL_DELAYED": ("0x00801B80", "cdecl", [], 4),
    "Spell.Packet_SMSG_SPELL_COOLDOWN": ("0x00806DD0", "cdecl", [], 4),
    "Spell.Packet_SMSG_ITEM_COOLDOWN": ("0x00807060", "cdecl", [], 4),
    "Spell.Packet_Group_31":           ("0x00804010", "cdecl", [], 4),
    "Spell.CancelSpell":               ("0x00806200", "cdecl", [], 4),
    "Combat.Packet_Group_29":          ("0x00756800", "cdecl", [], 4),
    # The combat log statics take their message in a register and the time offset alone on the
    # stack, cleaned by the caller. SpellLogMiss: `mov edi, [ebp+0x14]; push 0; call` at
    # 0x006D3204, `mov edi, esi; call` at 0x006D419B, EDI read at 0x006D30A0. NonMeleeDamageLog
    # and HealLog: `mov eax, [ebp+0x14]; push 0; call` at 0x006D3C13 and 0x006D3DD3, `mov eax,
    # esi` at 0x006D41CC and 0x006D4191, `mov esi, eax` at 0x006D384B and 0x006D3C3B.
    # PeriodicAuraLog takes both on the stack: `push edi; push esi; call; add esp, 8` at 0x006D41BF.
    "Combat.HandleSpellLogMiss":       ("0x006D3090", "regparm", ["edi"], 1),
    "Combat.HandleSpellNonMeleeDamageLog": ("0x006D3840", "regparm", ["eax"], 1),
    "Combat.HandleSpellHealLog":       ("0x006D3C30", "regparm", ["eax"], 1),
    "Combat.HandlePeriodicAuraLog":    ("0x007531F0", "cdecl", [], 2),
    # SpellLogEnergizeLog is the same kind of static: `mov eax, [ebp+0x14]; push 0; call; add
    # esp, 4` at 0x006D3EF3, `push edi; mov eax, esi; call` at 0x006D4186, `mov esi, eax` at
    # 0x006D3DFB.
    "Combat.HandleSpellLogEnergizeLog": ("0x006D3DF0", "regparm", ["eax"], 1),
    # The zone, mouseover and click rows. NewZoneFeedback reads its seven at [ebp+8..0x20] and
    # leaves by a tail `jmp` to CGSpellBook__UpdateUsable at 0x005206D9, so no `ret` settles who
    # cleans; its one caller (0x0078F1DD, the row's witness) pushes the seven and cleans them
    # with the argument of the call before it (`add esp, 0x20`). HandleObjectTrackChange reads guid at [ebp+8] and
    # prev_guid at [ebp+0x10]; HandleSpriteTrack (0x0051FB75) pushes four and cleans 0x10. The
    # click events are pointers to the world frame's locals (0x004F7971, 0x004F799D): the
    # terrain one's button at +0x14, the sprite one's at +8.
    "GameUI.NewZoneFeedback":          ("0x005204C0", "cdecl", [], 7),
    "GameUI.HandleObjectTrackChange":  ("0x0051F790", "cdecl", [], 4),
    "GameUI.HandleTerrainClick":       ("0x00527830", "cdecl", [], 1),
    "GameUI.HandleSpriteClick":        ("0x00527870", "cdecl", [], 1),
    # CVar::InternalSet reads ECX at 0x007667B5 and ends in `ret 0x14`. SendMovementUpdate reads
    # ECX at 0x0071F0C8 and ends in `ret 0x1C`; CallMoveEventHandlers (0x006EB2ED-0x006EB309)
    # pushes seven around the call that computes the opcode.
    "ConsoleVar.InternalSet":          ("0x007667B0", "thiscall", ["ecx"], 5),
    # CVar::Initialize reads its file name at [ebp+8] (0x00768343) and ends in a bare `ret`
    # (0x00768401); DeleteCharacterCVars and DeleteAccountCVars read no stack and end in a bare
    # `ret` (0x00767393, 0x007673E3).
    "ConsoleVar.Initialize":           ("0x00768340", "cdecl", [], 1),
    "ConsoleVar.DeleteCharacterCVars": ("0x00767350", "cdecl", [], 0),
    "ConsoleVar.DeleteAccountCVars":   ("0x007673A0", "cdecl", [], 0),
    # FrameXML_CheckSignature reads its .toc at [ebp+8] (0x008165EA), its extra file at [ebp+0xC]
    # and its digest at [ebp+0x14] (0x008166BE) and ends in bare `ret`s (0x00816647, 0x00816757);
    # DeleteInterfaceFiles reads no stack and ends in a bare `ret` (0x005F4E2F).
    "FrameXML.CheckSignature":         ("0x008165E0", "cdecl", [], 4),
    "AddOns.DeleteInterfaceFiles":     ("0x005F4D90", "cdecl", [], 0),
    "Unit.SendMovementUpdate":         ("0x0071F0C0", "thiscall", ["ecx"], 7),
    # OnUpdateMoney is a mirror handler and reads none of its arguments; CallMirrorFunctions
    # (0x004D5238-0x004D5256) pushes the six dwords of MIRRORHANDLERFUNC, calls through
    # [esi+0x10] at 0x004D5254 (the row's witness) and cleans 0x18. Packet_Group_9 reads msg at [ebp+0x14] and msg_id at
    # [ebp+0xC] and ends in a bare `ret`.
    "Player.OnUpdateMoney":            ("0x006DA070", "cdecl", [], 0),
    "Player.Packet_Group_9":           ("0x006E2E90", "cdecl", [], 4),
    # UpdateCurrentScreen copies its one at 0x004D82FF into 0x00B6A9E0 and ends in `ret` or tail
    # jumps. The RealmConnection handlers end in `ret 0xC`; Login::RealmListResult, reached only
    # through the vtables of GruntLogin (0x00A94928) and BattlenetLogin (0x00A94768), in `ret 4`.
    "Glue.UpdateCurrentScreen":        ("0x004D82C0", "cdecl", [], 1),
    "RealmConnection.HandleAuthResponse": ("0x00464640", "thiscall", ["ecx"], 3),
    "RealmConnection.HandleCharEnum":  ("0x00464C10", "thiscall", ["ecx"], 3),
    "RealmConnection.RequestCharacterLogin": ("0x00464AB0", "thiscall", ["ecx"], 3),
    "Login.RealmListResult":           ("0x00465F10", "thiscall", ["ecx"], 1),
    # ChatMessageReceived: OnMessageChat (0x0050EBA6) pushes 0 and msg and cleans 8.
    # AddChatMessage: ChatMessageReceived pushes fifteen dwords and cleans 0x3C after the call at
    # 0x0050C3AA. QueueChatText: AddChatMessage pushes seventeen dwords at 0x0050AD82 and cleans
    # 0x44. ~PENDINGCHAT: `mov esi,ecx` (0x00501711) before any read, a bare `ret`, and DeleteNode
    # (0x00503114) sets ECX and pushes nothing. RecordChat: AddChatMessage pushes ten dwords at
    # 0x0050A8A5 and cleans them with SendChatEvent's in one `add esp,0x50` (0x0050A8DA), and
    # its body reads [ebp+8]. The script functions are lua_CFunctions; LOG_XPGAIN reads its
    # message at [ebp+0x14].
    "Chat.ChatMessageReceived":        ("0x0050BE70", "cdecl", [], 2),
    "Chat.AddChatMessage":             ("0x00509DD0", "cdecl", [], 15),
    "Chat.QueueChatText":              ("0x00502D00", "cdecl", [], 17),
    "Chat.DestroyPendingChat":         ("0x00501710", "thiscall", ["ecx"], 0),
    "Chat.RecordChat":                 ("0x004FDD80", "cdecl", [], 10),
    "Chat.Script_SendChatMessage":     ("0x0050D170", "cdecl", [], 1),
    "Chat.Script_SendAddonMessage":    ("0x00500560", "cdecl", [], 1),
    "Chat.Packet_SMSG_LOG_XPGAIN":     ("0x0050C720", "cdecl", [], 4),
    # Storm's allocator: SMemAlloc reads [ebp+8..0x14] and ends in `ret 0x10` (0x0076E586,
    # 0x0076E597); operator new (0x00401010) pushes four and cleans none. SMemFree reads its
    # block at [ebp+8] and ends in `ret 0x10` (0x0076E5C1). SMemReAlloc reads flags at
    # [ebp+0x18] (0x0076E5E5) and every return is `ret 0x14`.
    "Storm.SMemAlloc":                 ("0x0076E540", "stdcall", [], 4),
    "Storm.SMemFree":                  ("0x0076E5A0", "stdcall", [], 4),
    "Storm.SMemReAlloc":               ("0x0076E5E0", "stdcall", [], 5),
    # SErrDisplayError reads [ebp+0xC] at 0x00771D13 and every return is `ret 0x1C` (0x00772653);
    # SErrDisplayAppFatalCustomV pushes seven at 0x00772A51-63 and cleans none.
    "Storm.SErrDisplayError":          ("0x00771D10", "stdcall", [], 7),
    # OsValidateFunctionPointer reads its pointer at [ebp+8] (0x0086B5C9) and ends in a bare
    # `ret` (0x0086B5F4); luaD_precall pushes EDI at 0x00856513 and folds the cleanup into a
    # later one, as every caller does, so no call site witnesses it.
    "Os.ValidateFunctionPointer":      ("0x0086B5A0", "cdecl", [], 1),
    # SFile::OpenArchive ends in `ret 0x10` (0x004219E2, 0x00421A02), SFile::CloseArchive in
    # `ret 4` (0x00421742). ClientDBInitialize reads nothing on the stack and ends in a bare
    # `ret` at 0x00634E58.
    "Storage.OpenArchive":             ("0x00421950", "stdcall", [], 4),
    "Storage.CloseArchive":            ("0x00421720", "stdcall", [], 1),
    "ClientDb.Initialize":             ("0x00634E00", "cdecl", [], 0),
    # The observed half of SFile::OpenEx, the same function as Storage.OpenFile: `ret 0x10` at
    # 0x00424C3C and 0x00424CCB.
    "Storage.OpenEx":                  ("0x00424B50", "stdcall", [], 4),
    # System_SFile2::FindFile reads [ebp+8] (0x004247C0) up to [ebp+0x1C] (0x004247A9) and ends
    # in a bare `ret` (0x00424B0A); SFile::FileExistsEx pushes six and `add esp, 0x18` at 0x00424B38.
    "Storage.FindFile":                ("0x00424780", "cdecl", [], 6),
    # The observed half of System_SFile2::FindFile, the same function: as above.
    "Storage.FindFileResult":          ("0x00424780", "cdecl", [], 6),
    # The asset pipeline's events. CMapArea::AsyncCallback reads [ebp+8] (0x007D7024), a bare
    # `ret` (0x007D7044); AsyncTextureSuccessCallback the same (0x004B7E84, 0x004B7F0D); both are
    # called by AsyncFileReadPollHandler through ECX at 0x004B9B9E, `add esp, 4`.
    "MapArea.AsyncCallback":           ("0x007D7020", "cdecl", [], 1),
    "Texture.AsyncTextureSuccessCallback": ("0x004B7E80", "cdecl", [], 1),
    # CMapObjGroup::Create: `mov esi, ecx` (0x007D82E8), bare `ret` (0x007D8560, 0x007D856E).
    "MapObj.CreateGroup":              ("0x007D82E0", "thiscall", ["ecx"], 0),
    # CM2Shared::FinishLoadingSkinProfile: `mov edi, ecx` (0x00838494), [ebp+8] read, `ret 4`
    # (0x008384A3).
    "M2Shared.FinishLoadingSkinProfile": ("0x00838490", "thiscall", ["ecx"], 1),
    # The model loader's later stages: CM2Shared::Initialize `mov esi, ecx` (0x0083CC88), plain
    # `ret` (0x0083CD29); InitializeSkinProfile `mov ebx, ecx` (0x00837A47), `ret` (0x00837AFE);
    # FinishLoadingLowPrioritySequence reads [ebp+8], [ebp+0xc] and [ebp+0x10], `ret 0xc` (0x0083CA73).
    "M2Shared.Initialize":             ("0x0083CC80", "thiscall", ["ecx"], 0),
    "M2Shared.InitializeSkinProfile":  ("0x00837A40", "thiscall", ["ecx"], 0),
    "M2Shared.FinishLoadingLowPrioritySequence": ("0x0083C6E0", "thiscall", ["ecx"], 3),
    # The model renderer and animator: DrawBatch `ret` with `this` in ECX; AnimateMT and
    # AnimateMTSimple `mov esi, ecx` and `ret 0x14` (0x00830968, 0x0082E545): five dwords, the two
    # floats among them.
    "M2.DrawBatch":                    ("0x008203B0", "thiscall", ["ecx"], 0),
    "M2.AnimateMT":                    ("0x0082F0F0", "thiscall", ["ecx"], 5),
    "M2.AnimateMTSimple":              ("0x0082E140", "thiscall", ["ecx"], 5),
    # The character appearance: AddItem(slot, rec, visual) `ret 0xc` (0x004F26C2), RemoveItem(slot)
    # `ret 4` (0x004EE53C); FreeComponent and RemoveVisuals are class statics reading [ebp+8] and
    # ending in a plain `ret` (0x004F16E7, 0x004EB060).
    "Component.AddItem":               ("0x004F2640", "thiscall", ["ecx"], 3),
    "Component.RemoveItem":            ("0x004EE460", "thiscall", ["ecx"], 1),
    "Component.FreeComponent":         ("0x004F16C0", "cdecl", [], 1),
    "Component.RemoveVisuals":         ("0x004EAF70", "cdecl", [], 1),
    # CBLPFile::Open: [ebp+8] (0x006AFF16) and `mov esi, ecx` (0x006AFF1B), `ret 8` (0x006AFF3A).
    # CBLPFile::Source: `mov ebx, ecx` (0x006AE904), [ebp+8] (0x006AE924), `ret 4` (0x006AE955).
    "BLPFile.Open":                    ("0x006AFF10", "thiscall", ["ecx"], 2),
    "BLPFile.Source":                  ("0x006AE900", "thiscall", ["ecx"], 1),
    # CGxDevice::IShaderLoad reads ECX (0x0068497E) and [ebp+8..0x14], `ret 0x14` (0x00684A9F).
    "Gx.IShaderLoad":                  ("0x00684970", "thiscall", ["ecx"], 5),
    # SE2::CreateWaveData reads [ebp+8] (0x0087EE83) to the floats at [ebp+0x38], bare `ret`
    # (0x0087F195, 0x0087F376); Sound_LoadScriptSoundFile pushes sixteen, `add esp, 0x40` at
    # 0x0087F808.
    "Sound.CreateWaveData":            ("0x0087EE60", "cdecl", [], 16),
    # TextureCreate (the file-name overload) reads [ebp+8..0x14] and ends in a bare `ret`
    # (0x004B97A4, 0x004B98EC); 0x0040A075 pushes four and cleans 0x10.
    "Texture.Create":                  ("0x004B9760", "cdecl", [], 4),
    # CM2Shared::LoadSucceededCallback reads its model at [ebp+8] and tail jumps into
    # FinishLoading (0x0083D35E); AsyncFileReadPollHandler pushes the user arg and calls through
    # ECX at 0x004B9B9E (the witness), then `add esp, 4`.
    "M2Shared.LoadSucceededCallback":  ("0x0083D340", "cdecl", [], 1),
    # CMapObj::Load: `mov edi, ecx` at 0x007D7479, nothing read on the stack, bare `ret` at
    # 0x007D7700.
    "MapObj.Load":                     ("0x007D7470", "thiscall", ["ecx"], 0),
    # CMap::AllocDoodadDef reads nothing and ends in a bare `ret` (0x007C021F). The three
    # CreateDoodadDef overloads read three ([ebp+8..0x10]), four (the facing a float at
    # [ebp+0x10]) and six dwords (the doodad set a word at [ebp+0x1C]) and each ends in a bare
    # `ret` (0x007BEF31, 0x007BECC9, 0x007BF11F).
    "Map.AllocDoodadDef":              ("0x007C01F0", "cdecl", [], 0),
    # CMap::Unload reads no stack and ends in a bare `ret` (0x007C39F9).
    "Map.Unload":                      ("0x007C3830", "cdecl", [], 0),
    "Map.CreateDoodadDef":             ("0x007BECD0", "cdecl", [], 3),
    "Map.CreateDoodadDefAt":           ("0x007BEB40", "cdecl", [], 4),
    "Map.CreateDoodadDefInMapObj":     ("0x007BEF40", "cdecl", [], 6),
    # FillInPartialObjectData reads [ebp+8..0x1C] (the bools as bytes at 0x004D544E, 0x004D54D1)
    # and ends in a bare `ret`; CreateObject pushes six at 0x004D6C5B-63. Packet_SMSG_UPDATE_OBJECT
    # reads time and msg at [ebp+0x10], [ebp+0x14], ends in a bare `ret` (0x004D74C9), and
    # SMSG_COMPRESSED_UPDATE_OBJECT's handler pushes four and cleans 0x10 at 0x004D7597.
    "ObjectMgr.FillInPartialObjectData": ("0x004D53C0", "cdecl", [], 6),
    "ObjectMgr.Packet_SMSG_UPDATE_OBJECT": ("0x004D73A0", "cdecl", [], 4),
    # The points of wxl.point (include/wxl/points/x86.def), read by hand: UpdateCallback is called
    # once, at 0x004FA7BC, with two dwords pushed and none cleaned, and ends in a bare `ret`;
    # CreateShared reads ECX at 0x0081C3A4 and returns `ret 8`; RenderWorld is a render-batch
    # callback (its address pushed, never called directly) reading one dword, bare `ret`;
    # vFrameScript_SignalEvent is called at 0x005326E8 with three pushed and twelve cleaned.
    "Point.CGCamera_UpdateCallback":   ("0x00607B00", "cdecl", [], 2),
    # The camera seam on the same function, read by hand as its point is: called once, at
    # 0x004FA7BC, two dwords pushed and none cleaned there (the caller's frame pops them), a bare
    # `ret`.
    "Camera.UpdateCallback":           ("0x00607B00", "cdecl", [], 2),
    # The control bits, read by hand: SetControlBit reads ECX into ESI and [ebp+8] (0x005FA177,
    # 0x005FA17B) and ends `ret 8`; UnsetControlBit ends `ret 0xC` (0x005FA46C, 0x005FA6A6).
    # CGUnit_C::PreAnimate, a virtual, reads ECX and its frame, one `ret 4` (0x0073E40D).
    "InputControl.SetControlBit":      ("0x005FA170", "thiscall", ["ecx"], 2),
    "InputControl.UnsetControlBit":    ("0x005FA450", "thiscall", ["ecx"], 3),
    "Unit.PreAnimate":                 ("0x0073DAB0", "thiscall", ["ecx"], 1),
    # CGWorldFrame::PerformDefaultAction reads ECX into ESI (0x004F7887) and its button at
    # [ebp+8] (0x004F78C7), and ends `ret 4` on every path (0x004F78E7, 0x004F7A38).
    "WorldFrame.PerformDefaultAction": ("0x004F7880", "thiscall", ["ecx"], 1),
    # The circles' drawers, members called through the vtable or by CGUnit_C::Render with no
    # stack argument: each moves ECX to ESI first (0x00725991, 0x00725BF7, 0x007275C7,
    # 0x007062F7) and ends in one bare `ret` (0x00725BD6, 0x00725DE6, 0x00727759, 0x00706429).
    "Unit.RenderTargetSelection":      ("0x00725980", "thiscall", ["ecx"], 0),
    "Unit.RenderPetTargetSelection":   ("0x00725BF0", "thiscall", ["ecx"], 0),
    "Unit.RenderAutoTrackCursor":      ("0x007275C0", "thiscall", ["ecx"], 0),
    "Corpse.RenderTargetSelection":    ("0x007062F0", "thiscall", ["ecx"], 0),
    # ProjectTex::Render2d reads seven dwords from [ebp+8] and ends in a bare `ret` (0x007E447B);
    # CGUnit_C::RenderTargetSelection pushes seven and cleans 0x1c (0x00725A8F).
    "ProjectTex.Render2d":             ("0x007E4370", "cdecl", [], 7),
    "Point.CM2Cache_CreateShared":     ("0x0081C390", "thiscall", ["ecx"], 2),
    "Point.CGWorldFrame_RenderWorld":  ("0x004FAF90", "cdecl", [], 1),
    "Point.vFrameScript_SignalEvent":  ("0x0081AC90", "cdecl", [], 3),
    "Point.NetClient_ProcessMessage":  ("0x00631FE0", "thiscall", ["ecx"], 3),
    # FinishLoading is reached by LoadSucceededCallback's tail jump with ECX set (0x0083D35A), moves
    # ECX at 0x0083CF01 and ends in bare `ret`s (0x0083CF0F, 0x0083D23D, 0x0083D25F, 0x0083D2C1).
    "Point.CM2Shared_FinishLoading":   ("0x0083CF00", "thiscall", ["ecx"], 0),
    # M2Init's second parent, which the runtime recompiles but is no point: reads ECX at
    # 0x0083C6EB and [ebp+8], [ebp+0xc] (0x0083C6E4, 0x0083C6E7), `ret 0xc` at 0x0083CA73 and
    # 0x0083CA8A; its caller 0x0083CA90 pushes three before each call (0x0083CAB2, 0x0083CADD).
    "Parent.CM2Shared_FinishLoadingLowPrioritySequence_uint32_t_uint8_t_uint32_t":
                                       ("0x0083C6E0", "thiscall", ["ecx"], 3),
    # What the recompiled bodies call (src/deinlined/), each read by hand. CM2Shared::Initialize
    # reads ECX (0x0083CC88) and ends in bare `ret`s (0x0083CD29, 0x0083CD4F); SMemAlloc reads
    # four stack slots and ends `ret 0x10` (0x0076E586, 0x0076E597), STORMAPI. The M2Init<T>
    # array readers read their frame up to [ebp+0x14] (the array; 0x00835AED, 0x00835C7A) and
    # end in bare `ret`s (0x00835AFE, 0x00835C90), and their callers in
    # FinishLoading and FinishLoadingLowPrioritySequence push four and clean 0x10 (`add esp,
    # 0x10`, e.g. 0x0083C75A after the call at 0x0083C755).
    "Deinlined.CM2Shared_Initialize":  ("0x0083CC80", "thiscall", ["ecx"], 0),
    "Deinlined.SMemAlloc":             ("0x0076E540", "stdcall", [], 4),
    "Deinlined.M2Init_00835AE0":       ("0x00835AE0", "cdecl", [], 4),
    "Deinlined.M2Init_00835B80":       ("0x00835B80", "cdecl", [], 4),
    "Deinlined.M2Init_00835BD0":       ("0x00835BD0", "cdecl", [], 4),
    "Deinlined.M2Init_00835C20":       ("0x00835C20", "cdecl", [], 4),
    "Deinlined.M2Init_00835C70":       ("0x00835C70", "cdecl", [], 4),
    "Deinlined.M2Init_00835DF0":       ("0x00835DF0", "cdecl", [], 4),
    "Deinlined.M2Init_00836B60":       ("0x00836B60", "cdecl", [], 4),
    "Deinlined.M2Init_00836E40":       ("0x00836E40", "cdecl", [], 4),
    "Deinlined.M2Init_00837EE0":       ("0x00837EE0", "cdecl", [], 4),
    "Deinlined.M2Init_008382A0":       ("0x008382A0", "cdecl", [], 4),
    "Deinlined.M2Init_008385A0":       ("0x008385A0", "cdecl", [], 4),
    "Deinlined.M2Init_00838B10":       ("0x00838B10", "cdecl", [], 4),
    "Deinlined.M2Init_00839080":       ("0x00839080", "cdecl", [], 4),
    "Deinlined.M2Init_00839270":       ("0x00839270", "cdecl", [], 4),
    "Deinlined.M2Init_00839EF0":       ("0x00839EF0", "cdecl", [], 4),
    "Deinlined.M2Init_0083A460":       ("0x0083A460", "cdecl", [], 4),
    "Deinlined.M2Init_0083AF90":       ("0x0083AF90", "cdecl", [], 4),
    # The x86 binding's own calls for wxl.m2's models of the world (calls_x86.def), each read by
    # hand: `ret 8` at 0x008251C5, `ret 4` at 0x00825F36, `ret 0x1c` at 0x00832ADA, `ret 0xc` at
    # 0x0082D11A, `ret 4` at 0x00823F59, `ret 8` at 0x00824F19, `ret 4` at 0x004F5E70 and
    # 0x004F5EF7 (the hidden return pointer), `ret 4` at 0x00827A3C and 0x00824270, `ret 4` at
    # 0x00827437, `ret 8` at 0x0083140C (hidden pointer and id), `ret 4` at 0x004C3339,
    # 0x004C3379, 0x004C33B9 and 0x004C1C3D; bare `ret`s at 0x00782342, 0x007804F4, 0x00782731.
    "M2Call.SetLoadedCallback":        ("0x008251B0", "thiscall", ["ecx"], 2),
    "M2Call.HasSequence":              ("0x00825EE0", "thiscall", ["ecx"], 1),
    "M2Call.SetBoneSequence":          ("0x00832AB0", "thiscall", ["ecx"], 7),
    "M2Call.GetSequenceInfo":          ("0x0082CED0", "thiscall", ["ecx"], 3),
    "M2Call.SetAnimating":             ("0x00823F10", "thiscall", ["ecx"], 1),
    "M2Call.IsLoaded":                 ("0x00824F00", "thiscall", ["ecx"], 2),
    "M2Call.GetBoundingBox":           ("0x004F5E20", "thiscall", ["ecx"], 1),
    "M2Call.GetBoundingSphere":        ("0x004F5E80", "thiscall", ["ecx"], 1),
    "M2Call.SetEmittersEnabled":       ("0x008279F0", "thiscall", ["ecx"], 1),
    "M2Call.SetRibbonsEnabled":        ("0x00824230", "thiscall", ["ecx"], 1),
    "M2Call.HasAttachment":            ("0x008273D0", "thiscall", ["ecx"], 1),
    "M2Call.GetAttachmentPosition":    ("0x00831330", "thiscall", ["ecx"], 2),
    "M2Call.RotateAroundX":            ("0x004C3300", "thiscall", ["ecx"], 1),
    "M2Call.RotateAroundY":            ("0x004C3340", "thiscall", ["ecx"], 1),
    "M2Call.RotateAroundZ":            ("0x004C3380", "thiscall", ["ecx"], 1),
    "M2Call.Scale":                    ("0x004C1BF0", "thiscall", ["ecx"], 1),
    "M2Call.ObjectCreateStatic":       ("0x00782000", "cdecl", [], 5),
    "M2Call.ObjectUpdate":             ("0x00780240", "cdecl", [], 7),
    "M2Call.ObjectDestroy":            ("0x007826E0", "cdecl", [], 1),
    # The object reads' calls (calls_x86.def), read by hand: CanAttack moves ECX to EBX
    # (0x00729744) and ends `ret 4` (0x00729768); CanInteractNow moves ECX to ESI (0x00729625)
    # and ends `ret 4` (0x0072968F, 0x00729738); CanHighlight moves ECX to ESI (0x0070F581) and
    # ends in bare `ret`s (0x0070F61B, 0x0070F627).
    "ObjectCall.CanAttack":            ("0x00729740", "thiscall", ["ecx"], 1),
    "ObjectCall.CanInteractNow":       ("0x00729620", "thiscall", ["ecx"], 1),
    "ObjectCall.CanHighlight":         ("0x0070F580", "thiscall", ["ecx"], 0),
    # The circle textures' calls (calls_x86.def), read by hand: CreateTexture reads its name and
    # status at [ebp+8] and [ebp+0xc] (0x004E8D50) and ends in a bare `ret` (0x004E8D68);
    # GetGlobalStatusObj is `mov eax, imm32; ret` (0x0047C095); HandleClose reads its handle at
    # [ebp+8] (0x0047BF33) and ends in bare `ret`s.
    "TextureCall.CreateTexture":       ("0x004E8D30", "cdecl", [], 2),
    "TextureCall.GetGlobalStatusObj":  ("0x0047C090", "cdecl", [], 0),
    "TextureCall.HandleClose":         ("0x0047BF30", "cdecl", [], 1),
    # wxl.terrain's calls (calls_x86.def), read by hand: CreateBounds writes through ECX
    # (0x007C5244) and ends in a bare `ret` (0x007C543A); AddBatch moves ECX to ESI (0x007B7AFF),
    # reads [ebp+8] to [ebp+0x14] and ends `ret 0x10` (0x007B7BC9); FreeInst reads [ebp+8]
    # (0x007B396B) and ends in a bare `ret` (0x007B3986); OnFallLocal reads [ebp+8] (0x006ECCF3),
    # moves ECX to ESI (0x006ECCFA) and ends `ret 4` (0x006ECD46).
    "TerrainCall.CreateBounds":        ("0x007C5220", "thiscall", ["ecx"], 0),
    "TerrainCall.AddBatch":            ("0x007B7AF0", "thiscall", ["ecx"], 4),
    "TerrainCall.FreeInst":            ("0x007B3960", "cdecl", [], 1),
    "TerrainCall.OnFallLocal":         ("0x006ECCF0", "thiscall", ["ecx"], 1),
    # wxl.animation's calls (calls_x86.def), read by hand: PlayBaseAnimation moves ECX to EBX
    # (0x007385CA) and ends `ret 8` (0x0073880C); UpdateBaseAnimation moves ECX to ESI
    # (0x0073AC34) and ends `ret 8` (0x0073ACF9); GetCurrentTorsoAnimId moves ECX to ESI
    # (0x00717261) and ends in bare `ret`s (0x007172A4, 0x007172A9); SetKeyBoneSequenceSpeed
    # moves ECX to ESI (0x00735CCA), reads [ebp+8] to [ebp+0x14] and ends `ret 0x10` (0x00735DC6);
    # CM2Model::GetBoneSequenceId moves ECX to ESI (0x008267E4) and ends `ret 4` (0x00826853).
    "AnimCall.PlayBaseAnimation":      ("0x007385C0", "thiscall", ["ecx"], 2),
    "AnimCall.UpdateBaseAnimation":    ("0x0073AC30", "thiscall", ["ecx"], 2),
    "AnimCall.GetCurrentTorsoAnimId":  ("0x00717260", "thiscall", ["ecx"], 0),
    "AnimCall.SetKeyBoneSequenceSpeed": ("0x00735CC0", "thiscall", ["ecx"], 4),
    "AnimCall.GetBoneSequenceId":      ("0x008267E0", "thiscall", ["ecx"], 1),
    # wxl.cvar's calls (calls_x86.def), read by hand: Lookup reads its name at [ebp+8]
    # (0x00767443) and ends in bare `ret`s (0x00767456); Register reads up to its ninth dword
    # at [ebp+0x28] (0x00768068) and ends in bare `ret`s (0x00768085, 0x007681E0); Set moves ECX
    # to ESI (0x007668CE) and ends `ret 0x14` (0x00766915, 0x00766933).
    "CVarCall.Lookup":                 ("0x00767440", "cdecl", [], 1),
    "CVarCall.Register":               ("0x00767FC0", "cdecl", [], 9),
    "CVarCall.Set":                    ("0x007668C0", "thiscall", ["ecx"], 5),
    # The interface signature's calls (calls_x86.def), read by hand: CheckSignature as its seam
    # row; MD5Init reads its context at [ebp+8] (0x00779343) and ends in a bare `ret`
    # (0x0077936A); ProcessFileForMakeSign reads up to [ebp+0x10] (0x008162B6) and ends in a bare
    # `ret` (0x008164C4); MD5Update reads its length at [ebp+0x10] (0x00779A3E), MD5Final its
    # context at [ebp+0xC] (0x00779AE7), both ending in bare `ret`s (0x00779B96 for MD5Final).
    "InterfaceCall.CheckSignature":         ("0x008165E0", "cdecl", [], 4),
    "InterfaceCall.MD5Init":                ("0x00779340", "cdecl", [], 1),
    "InterfaceCall.ProcessFileForMakeSign": ("0x008162B0", "cdecl", [], 3),
    "InterfaceCall.MD5Update":              ("0x00779A30", "cdecl", [], 3),
    "InterfaceCall.MD5Final":               ("0x00779AE0", "cdecl", [], 2),
    # The gameplay rows, read by hand. The packet handlers are reached only through NetClient's
    # handler table, which pushes four and cleans 0x10 at 0x0063202C; each reads its message at
    # [ebp+0x14] and ends in bare `ret`s. UNINVITE, DESTROYED and READY_CHECK_FINISHED read
    # nothing and are witnessed there. The mirror handlers are called by CallMirrorFunctions
    # (six dwords pushed, 0x18 cleaned at 0x004D5254, their witness) and their own code shows
    # only what each reads: the guid (two dwords), the offset (three), the value (five). The aura log statics: OnAuraUpdate pushes six (0x0072FAB3, add esp,0x18) and four
    # (0x0072FB34, add esp,0x10). CGLootInfo::SetObject reads [ebp+8..0x10], ClearSlot a byte
    # at [ebp+8]; UpdateActivePlayer reads nothing and UpdateGhostStatus starts `mov esi,ecx`
    # (0x006DF717), both ending in bare `ret`s.
    "Combat.UnitCombatLogAuraDose":    ("0x00752860", "cdecl", [], 6),
    "Combat.UnitCombatLogAuraRefreshed": ("0x00752BA0", "cdecl", [], 4),
    "Unit.MountDisplayIDUpdateHandler": ("0x007419C0", "cdecl", [], 2),
    "Loot.SetObject":                  ("0x005896B0", "cdecl", [], 3),
    "Loot.ClearSlot":                  ("0x00589920", "cdecl", [], 1),
    "Player.PH_LOOT_OPCODES":          ("0x006D84F0", "cdecl", [], 4),
    "Player.OnUpdateQuest":            ("0x006DF370", "cdecl", [], 5),
    "Player.Packet_Group_12":          ("0x006D7F10", "cdecl", [], 4),
    "Player.Packet_SMSG_GROUP_INVITE": ("0x006DF1A0", "cdecl", [], 4),
    "Player.Packet_SMSG_GROUP_LIST":   ("0x006D8870", "cdecl", [], 4),
    "Player.Packet_SMSG_GROUP_UNINVITE": ("0x006CBE10", "cdecl", [], 0),
    "Player.Packet_SMSG_GROUP_DESTROYED": ("0x006CBE40", "cdecl", [], 0),
    "Player.Packet_SMSG_RESURRECT_REQUEST": ("0x006DBD00", "cdecl", [], 4),
    "Player.Packet_SMSG_EMOTE":        ("0x006CFE70", "cdecl", [], 4),
    "Player.UpdateGhostStatus":        ("0x006DF710", "thiscall", ["ecx"], 0),
    "GameUI.UpdateActivePlayer":       ("0x00520F70", "cdecl", [], 0),
    "Chat.OnTextEmote":                ("0x00504070", "cdecl", [], 4),
    "Item.OnUpdateDurability":         ("0x00707AE0", "cdecl", [], 5),
    "Item.OnUpdateStackCount":         ("0x00707720", "cdecl", [], 2),
    "Container.InvUpdateHandler":      ("0x005D9B40", "cdecl", [], 3),
    "Container.UpdateInvContents":     ("0x005D9790", "cdecl", [], 5),
    "Container.UpdateContainerContents": ("0x005D9600", "cdecl", [], 5),
    "Raid.Packet_MSG_RAID_READY_CHECK": ("0x00574D50", "cdecl", [], 4),
    "Raid.Packet_MSG_RAID_READY_CHECK_CONFIRM": ("0x005744F0", "cdecl", [], 4),
    "Raid.Packet_MSG_RAID_READY_CHECK_FINISHED": ("0x00572560", "cdecl", [], 0),
    "Duel.Packet_SMSG_DUEL_REQUESTED": ("0x005CFCD0", "cdecl", [], 4),
    "Duel.Packet_SMSG_DUEL_COUNTDOWN": ("0x005CFA50", "cdecl", [], 4),
    "Duel.Packet_SMSG_DUEL_WINNER":    ("0x005CFB20", "cdecl", [], 4),
    "Duel.Packet_SMSG_DUEL_COMPLETE":  ("0x005CFA90", "cdecl", [], 4),
    "Trade.Packet_SMSG_TRADE_STATUS":  ("0x007044A0", "cdecl", [], 4),
    "Reputation.Packet_SMSG_SET_FACTION_STANDING": ("0x005D20A0", "cdecl", [], 4),
    "Achievement.Packet_SMSG_ACHIEVEMENT_EARNED": ("0x005B3020", "cdecl", [], 4),
    # The Vulkan adapter's calls into the stock device (GxCall) and the two D3D9 vtables' slots
    # (Slot.<name>.<api>, include/wxl/gx/slots.def). Members a vtable calls take `this` in ECX
    # whether they read it or hand it on to what they call (CapsWindowSize 0x006A5A00 calls
    # DeviceCurWindow with ECX untouched; XformSetView 0x006A9E00 is `push ebp; mov ebp,esp; pop
    # ebp; jmp 0x00689050`, read through to CGxDevice's, `ret 4`); members that never touch `this`
    # (CGxDevice::BufData 0x00683130, TexDestroy 0x00687980) are called as stdcall. The factories
    # are cdecl with nothing on the stack and end in `ret` (0x00689F13, 0x0068C243).
    "GxCall.DeviceCreate":                   ("0x00682CB0", "thiscall", ["ecx"], 2),
    "GxCall.DeviceDestroy":                  ("0x00682CF0", "thiscall", ["ecx"], 0),
    "GxCall.DeviceSetFormat":                ("0x006840F0", "thiscall", ["ecx"], 1),
    "GxCall.DeviceSetBaseMipLevel":          ("0x00682D00", "thiscall", ["ecx"], 1),
    "GxCall.DeviceSetGamma":                 ("0x00684170", "thiscall", ["ecx"], 1),
    "GxCall.DeviceSetGammaRamp":             ("0x00684190", "thiscall", ["ecx"], 1),
    "GxCall.DeviceOverride":                 ("0x00684FE0", "thiscall", ["ecx"], 2),
    "GxCall.DeviceSetRenderTarget":          ("0x006842A0", "thiscall", ["ecx"], 3),
    "GxCall.DeviceSetDefWindow":             ("0x00684360", "thiscall", ["ecx"], 1),
    "GxCall.ClampRectToWindow":              ("0x00683CE0", "thiscall", ["ecx"], 1),
    "GxCall.ScenePresent":                   ("0x00682E50", "thiscall", ["ecx"], 0),
    "GxCall.DeviceScreenShot":               ("0x006841D0", "thiscall", ["ecx"], 0),
    "GxCall.WaitForFPSCap":                  ("0x006836D0", "thiscall", ["ecx"], 0),
    "GxCall.XformSetView":                   ("0x00689050", "thiscall", ["ecx"], 1),
    "GxCall.QueryCreate":                    ("0x006877C0", "thiscall", ["ecx"], 2),
    "GxCall.ShaderConstantsClear":           ("0x006833A0", "cdecl", [], 0),
    "GxCall.IRsSync":                        ("0x00685B50", "thiscall", ["ecx"], 1),
    "GxCall.ITexMarkAsUpdated":              ("0x00684900", "stdcall", [], 1),
    "GxCall.ITexWHDStartEnd":                ("0x006A5EF0", "thiscall", ["ecx"], 5),
    "GxCall.TexMarkForUpdate":               ("0x006848A0", "thiscall", ["ecx"], 3),
    "GxCall.TexDestroy":                     ("0x00687980", "stdcall", [], 1),
    "GxCall.PoolDestroy":                    ("0x00688340", "thiscall", ["ecx"], 1),
    "GxCall.BufLock":                        ("0x00683150", "thiscall", ["ecx"], 1),
    "GxCall.BufUnlock":                      ("0x00683180", "thiscall", ["ecx"], 2),
    "GxCall.BufData":                        ("0x00683130", "stdcall", [], 4),
    "GxCall.ICursorCreate":                  ("0x00684AD0", "thiscall", ["ecx"], 1),
    "GxCall.ICursorDestroy":                 ("0x006835E0", "thiscall", ["ecx"], 0),
    "GxCall.ICursorDraw":                    ("0x00687A90", "thiscall", ["ecx"], 0),
    "GxCall.CursorSetVisible":               ("0x00683640", "thiscall", ["ecx"], 1),
    "GxCall.CursorUnlock":                   ("0x00684B50", "thiscall", ["ecx"], 2),
    "GxCall.QueryDestroy":                   ("0x006879F0", "stdcall", [], 1),
    "GxCall.ShaderCreate":                   ("0x006897C0", "thiscall", ["ecx"], 5),
    "GxCall.ShaderDestroy":                  ("0x00687820", "thiscall", ["ecx"], 1),
    "GxCall.LogFormat":                      ("0x00684D10", "stdcall", [], 1),
    "GxCall.LogCaps":                        ("0x00684C40", "stdcall", [], 1),
    "GxCall.AdjustWindowRectToWorkArea":     ("0x00684D70", "cdecl", [], 3),
    "GxCall.MatrixStackIdentity":            ("0x0057C340", "thiscall", ["ecx"], 0),
    "GxCall.EmergencyLock":                  ("0x00685E90", "thiscall", ["ecx"], 1),
    "GxCall.EmergencyUnlock":                ("0x00682C40", "thiscall", ["ecx"], 0),
    "GxCall.PoolDiscard":                    ("0x00688260", "thiscall", ["ecx"], 0),
    "GxCall.PixelsSetCount":                 ("0x00616CA0", "thiscall", ["ecx"], 1),
    "GxCall.Blit":                           ("0x006AE7C0", "cdecl", [], 8),
    "GxCall.GetBlitFormat":                  ("0x006817B0", "cdecl", [], 1),
    "GxCall.GxLog":                          ("0x006817E0", "cdecl", [], 2),
    "GxCall.PostClose":                      ("0x0047D760", "cdecl", [], 0),
    "GxCall.CommandRegister":                ("0x00769100", "cdecl", [], 4),
    "GxCall.ConsoleWrite":                   ("0x00765270", "cdecl", [], 2),
    "GxCall.TexUserFunc":                    ("0x00683610", "cdecl", [], 8),
    "Slot.Factory.d3d9":                     ("0x00689EF0", "cdecl", [], 0),
    "Slot.Factory.d3d9ex":                   ("0x0068C220", "cdecl", [], 0),
    "Slot.ITexMarkAsUpdated.d3d9":           ("0x006A3070", "thiscall", ["ecx"], 1),
    "Slot.ITexMarkAsUpdated.d3d9ex":         ("0x006A7100", "thiscall", ["ecx"], 1),
    "Slot.IRsSendToHw.d3d9":                 ("0x006A4C30", "thiscall", ["ecx"], 1),
    "Slot.IRsSendToHw.d3d9ex":               ("0x006A8BD0", "thiscall", ["ecx"], 1),
    "Slot.ICursorCreate.d3d9":               ("0x0068E900", "thiscall", ["ecx"], 1),
    "Slot.ICursorCreate.d3d9ex":             ("0x0068E900", "thiscall", ["ecx"], 1),
    "Slot.ICursorDestroy.d3d9":              ("0x006A00C0", "thiscall", ["ecx"], 0),
    "Slot.ICursorDestroy.d3d9ex":            ("0x006A00C0", "thiscall", ["ecx"], 0),
    "Slot.ICursorDraw.d3d9":                 ("0x0068E810", "thiscall", ["ecx"], 0),
    "Slot.ICursorDraw.d3d9ex":               ("0x0069FFD0", "thiscall", ["ecx"], 0),
    "Slot.NotifyOnDeviceRestored.d3d9":      ("0x006843B0", "thiscall", ["ecx"], 0),
    "Slot.NotifyOnDeviceRestored.d3d9ex":    ("0x006843B0", "thiscall", ["ecx"], 0),
    "Slot.NotifyOnTextureRecreation.d3d9":   ("0x006843E0", "thiscall", ["ecx"], 0),
    "Slot.NotifyOnTextureRecreation.d3d9ex": ("0x006843E0", "thiscall", ["ecx"], 0),
    "Slot.NotifyOnStereoChanged.d3d9":       ("0x00684410", "thiscall", ["ecx"], 0),
    "Slot.NotifyOnStereoChanged.d3d9ex":     ("0x00684410", "thiscall", ["ecx"], 0),
    "Slot.Destructor.d3d9":                  ("0x0068FE80", "thiscall", ["ecx"], 1),
    "Slot.DeviceCreateOnWindow.d3d9":        ("0x00690830", "thiscall", ["ecx"], 2),
    "Slot.DeviceCreateOnWindow.d3d9ex":      ("0x006A23A0", "thiscall", ["ecx"], 2),
    "Slot.DeviceCreate.d3d9":                ("0x00690750", "thiscall", ["ecx"], 2),
    "Slot.DeviceCreate.d3d9ex":              ("0x006A22C0", "thiscall", ["ecx"], 2),
    "Slot.DeviceDestroy.d3d9":               ("0x006905F0", "thiscall", ["ecx"], 0),
    "Slot.DeviceDestroy.d3d9ex":             ("0x006A2160", "thiscall", ["ecx"], 0),
    "Slot.DeviceEvictResources.d3d9":        ("0x0068E450", "thiscall", ["ecx"], 0),
    "Slot.DeviceEvictResources.d3d9ex":      ("0x0069FDC0", "thiscall", ["ecx"], 0),
    "Slot.DeviceSetFormat.d3d9":             ("0x006904D0", "thiscall", ["ecx"], 1),
    "Slot.DeviceSetFormat.d3d9ex":           ("0x006A2040", "thiscall", ["ecx"], 1),
    "Slot.DeviceSetBaseMipLevel.d3d9":       ("0x0068E4A0", "thiscall", ["ecx"], 1),
    "Slot.DeviceSetBaseMipLevel.d3d9ex":     ("0x0069FE60", "thiscall", ["ecx"], 1),
    "Slot.DeviceSetGammaRamp.d3d9":          ("0x0069FE80", "thiscall", ["ecx"], 1),
    "Slot.DeviceSetGammaRamp.d3d9ex":        ("0x0069FE80", "thiscall", ["ecx"], 1),
    "Slot.DeviceSetGamma.d3d9":              ("0x0068E4C0", "thiscall", ["ecx"], 1),
    "Slot.DeviceSetGamma.d3d9ex":            ("0x0068E4C0", "thiscall", ["ecx"], 1),
    "Slot.DeviceWindow.d3d9":                ("0x0069FED0", "thiscall", ["ecx"], 0),
    "Slot.DeviceWindow.d3d9ex":              ("0x0069FED0", "thiscall", ["ecx"], 0),
    "Slot.DeviceTakeScreenShot.d3d9":        ("0x00682D30", "thiscall", ["ecx"], 0),
    "Slot.DeviceTakeScreenShot.d3d9ex":      ("0x00682D30", "thiscall", ["ecx"], 0),
    "Slot.DeviceReadScreenShot.d3d9":        ("0x00684260", "thiscall", ["ecx"], 3),
    "Slot.DeviceReadScreenShot.d3d9ex":      ("0x00684260", "thiscall", ["ecx"], 3),
    "Slot.DeviceReadPixels.d3d9":            ("0x0068FED0", "thiscall", ["ecx"], 2),
    "Slot.DeviceReadPixels.d3d9ex":          ("0x006A16D0", "thiscall", ["ecx"], 2),
    "Slot.DeviceReadDepths.d3d9":            ("0x006A1950", "thiscall", [], 2),
    "Slot.DeviceReadDepths.d3d9ex":          ("0x006A1950", "thiscall", [], 2),
    "Slot.DeviceWM.d3d9":                    ("0x00690230", "thiscall", ["ecx"], 3),
    "Slot.DeviceWM.d3d9ex":                  ("0x006A1DC0", "thiscall", ["ecx"], 3),
    "Slot.DeviceSetRenderTarget.d3d9":       ("0x0068F770", "thiscall", ["ecx"], 3),
    "Slot.DeviceSetRenderTarget.d3d9ex":     ("0x006A13A0", "thiscall", ["ecx"], 3),
    "Slot.DeviceSaveDepthSurface.d3d9":      ("0x0068E510", "thiscall", ["ecx"], 0),
    "Slot.DeviceSaveDepthSurface.d3d9ex":    ("0x0069FEE0", "thiscall", ["ecx"], 0),
    "Slot.DeviceRestoreDepthSurface.d3d9":   ("0x0068E540", "thiscall", ["ecx"], 0),
    "Slot.DeviceRestoreDepthSurface.d3d9ex": ("0x0069FF10", "thiscall", ["ecx"], 0),
    "Slot.DeviceResolveDepthBuffer.d3d9":    ("0x0068F900", "thiscall", ["ecx"], 1),
    "Slot.DeviceResolveDepthBuffer.d3d9ex":  ("0x006A1530", "thiscall", ["ecx"], 1),
    "Slot.DeviceCopyTex.d3d9":               ("0x0068F950", "thiscall", ["ecx"], 4),
    "Slot.DeviceCopyTex.d3d9ex":             ("0x006A1580", "thiscall", ["ecx"], 4),
    "Slot.DeviceOverride.d3d9":              ("0x0069FF40", "thiscall", ["ecx"], 2),
    "Slot.DeviceOverride.d3d9ex":            ("0x0069FF40", "thiscall", ["ecx"], 2),
    "Slot.AddDeviceRestoredCallback.d3d9":   ("0x006853B0", "thiscall", ["ecx"], 1),
    "Slot.AddDeviceRestoredCallback.d3d9ex": ("0x006853B0", "thiscall", ["ecx"], 1),
    "Slot.RemoveDeviceRestoredCallback.d3d9": ("0x006853D0", "thiscall", ["ecx"], 1),
    "Slot.RemoveDeviceRestoredCallback.d3d9ex": ("0x006853D0", "thiscall", ["ecx"], 1),
    "Slot.AddTextureRecreationCallback.d3d9": ("0x00685460", "thiscall", ["ecx"], 1),
    "Slot.AddTextureRecreationCallback.d3d9ex": ("0x00685460", "thiscall", ["ecx"], 1),
    "Slot.RemoveTextureRecreationCallback.d3d9": ("0x00685480", "thiscall", ["ecx"], 1),
    "Slot.RemoveTextureRecreationCallback.d3d9ex": ("0x00685480", "thiscall", ["ecx"], 1),
    "Slot.AddStereoChangedCallback.d3d9":    ("0x00685510", "thiscall", ["ecx"], 1),
    "Slot.AddStereoChangedCallback.d3d9ex":  ("0x00685510", "thiscall", ["ecx"], 1),
    "Slot.RemoveStereoChangedCallback.d3d9": ("0x00685530", "thiscall", ["ecx"], 1),
    "Slot.RemoveStereoChangedCallback.d3d9ex": ("0x00685530", "thiscall", ["ecx"], 1),
    "Slot.CapsWindowSize.d3d9":              ("0x006A5A00", "thiscall", [], 1),
    "Slot.CapsWindowSize.d3d9ex":            ("0x006A5A00", "thiscall", [], 1),
    "Slot.CapsWindowSizeInScreenCoords.d3d9": ("0x006A9920", "thiscall", ["ecx"], 1),
    "Slot.CapsWindowSizeInScreenCoords.d3d9ex": ("0x006A9920", "thiscall", ["ecx"], 1),
    "Slot.LogCrashInfo.d3d9":                ("0x0068E570", "thiscall", ["ecx"], 2),
    "Slot.LogCrashInfo.d3d9ex":              ("0x0068E570", "thiscall", ["ecx"], 2),
    "Slot.ScenePresent.d3d9":                ("0x006A3450", "thiscall", ["ecx"], 0),
    "Slot.ScenePresent.d3d9ex":              ("0x006A7610", "thiscall", ["ecx"], 0),
    "Slot.SceneClear.d3d9":                  ("0x006A74B0", "thiscall", ["ecx"], 2),
    "Slot.SceneClear.d3d9ex":                ("0x006A74B0", "thiscall", ["ecx"], 2),
    "Slot.XformSetProjection.d3d9":          ("0x006A9B40", "thiscall", ["ecx"], 1),
    "Slot.XformSetProjection.d3d9ex":        ("0x006A9B40", "thiscall", ["ecx"], 1),
    "Slot.XformSetView.d3d9":                ("0x006A9E00", "thiscall", ["ecx"], 1),
    "Slot.XformSetView.d3d9ex":              ("0x006A9E00", "thiscall", ["ecx"], 1),
    "Slot.Draw.d3d9":                        ("0x006A3620", "thiscall", ["ecx"], 2),
    "Slot.Draw.d3d9ex":                      ("0x006A77C0", "thiscall", ["ecx"], 2),
    "Slot.PrimBegin.d3d9":                   ("0x006855C0", "thiscall", ["ecx"], 1),
    "Slot.PrimBegin.d3d9ex":                 ("0x006855C0", "thiscall", ["ecx"], 1),
    "Slot.PrimDrawElements.d3d9":            ("0x006845B0", "thiscall", ["ecx"], 0),
    "Slot.PrimDrawElements.d3d9ex":          ("0x006845B0", "thiscall", ["ecx"], 0),
    "Slot.PrimVertex.d3d9":                  ("0x00685640", "thiscall", ["ecx"], 1),
    "Slot.PrimVertex.d3d9ex":                ("0x00685640", "thiscall", ["ecx"], 1),
    "Slot.PrimTexCoord.d3d9":                ("0x00682FA0", "thiscall", ["ecx"], 2),
    "Slot.PrimTexCoord.d3d9ex":              ("0x00682FA0", "thiscall", ["ecx"], 2),
    "Slot.PrimNormal.d3d9":                  ("0x00682F70", "thiscall", ["ecx"], 1),
    "Slot.PrimNormal.d3d9ex":                ("0x00682F70", "thiscall", ["ecx"], 1),
    "Slot.PrimColor.d3d9":                   ("0x00684590", "thiscall", ["ecx"], 1),
    "Slot.PrimColor.d3d9ex":                 ("0x00684590", "thiscall", ["ecx"], 1),
    "Slot.MasterEnableSet.d3d9":             ("0x00685EB0", "thiscall", ["ecx"], 2),
    "Slot.MasterEnableSet.d3d9ex":           ("0x00685EB0", "thiscall", ["ecx"], 2),
    "Slot.PoolSizeSet.d3d9":                 ("0x0069FF80", "thiscall", ["ecx"], 2),
    "Slot.PoolSizeSet.d3d9ex":               ("0x0069FF80", "thiscall", ["ecx"], 2),
    "Slot.PoolDestroy.d3d9":                 ("0x0068E720", "thiscall", ["ecx"], 1),
    "Slot.PoolDestroy.d3d9ex":               ("0x0068E720", "thiscall", ["ecx"], 1),
    "Slot.BufLock.d3d9":                     ("0x0068FCE0", "thiscall", ["ecx"], 1),
    "Slot.BufLock.d3d9ex":                   ("0x0068FCE0", "thiscall", ["ecx"], 1),
    "Slot.BufUnlock.d3d9":                   ("0x0068FAE0", "thiscall", ["ecx"], 2),
    "Slot.BufUnlock.d3d9ex":                 ("0x0068FAE0", "thiscall", ["ecx"], 2),
    "Slot.BufData.d3d9":                     ("0x0068FD00", "thiscall", ["ecx"], 4),
    "Slot.BufData.d3d9ex":                   ("0x0068FD00", "thiscall", ["ecx"], 4),
    "Slot.TexCreate.d3d9":                   ("0x00685C60", "thiscall", ["ecx"], 11),
    "Slot.TexCreate.d3d9ex":                 ("0x00685C60", "thiscall", ["ecx"], 11),
    "Slot.TexDestroy.d3d9":                  ("0x006A2BB0", "thiscall", ["ecx"], 1),
    "Slot.TexDestroy.d3d9ex":                ("0x006A6B20", "thiscall", ["ecx"], 1),
    "Slot.TexCopy.d3d9":                     ("0x006A30D0", "thiscall", ["ecx"], 6),
    "Slot.TexCopy.d3d9ex":                   ("0x006A7160", "thiscall", ["ecx"], 6),
    "Slot.TexStretch.d3d9":                  ("0x006A31E0", "thiscall", ["ecx"], 5),
    "Slot.TexStretch.d3d9ex":                ("0x006A7270", "thiscall", ["ecx"], 5),
    "Slot.TexSetCacheSize.d3d9ex":           ("0x006A60B0", "thiscall", ["ecx"], 1),
    "Slot.QueryCreate.d3d9":                 ("0x0068E9C0", "thiscall", ["ecx"], 2),
    "Slot.QueryCreate.d3d9ex":               ("0x006A0140", "thiscall", ["ecx"], 2),
    "Slot.QueryDestroy.d3d9":                ("0x006A0190", "thiscall", ["ecx"], 1),
    "Slot.QueryDestroy.d3d9ex":              ("0x006A0190", "thiscall", ["ecx"], 1),
    "Slot.QueryBegin.d3d9":                  ("0x0068EA10", "thiscall", ["ecx"], 1),
    "Slot.QueryBegin.d3d9ex":                ("0x006A01C0", "thiscall", ["ecx"], 1),
    "Slot.QueryEnd.d3d9":                    ("0x006A0240", "thiscall", ["ecx"], 1),
    "Slot.QueryEnd.d3d9ex":                  ("0x006A0240", "thiscall", ["ecx"], 1),
    "Slot.QueryGetParam.d3d9":               ("0x0068EA90", "thiscall", ["ecx"], 3),
    "Slot.QueryGetParam.d3d9ex":             ("0x006A0280", "thiscall", ["ecx"], 3),
    "Slot.QueryGetData.d3d9":                ("0x006A0310", "thiscall", ["ecx"], 2),
    "Slot.QueryGetData.d3d9ex":              ("0x006A0310", "thiscall", ["ecx"], 2),
    "Slot.ShaderCreate.d3d9":                ("0x006AA130", "thiscall", ["ecx"], 5),
    "Slot.ShaderCreate.d3d9ex":              ("0x006AA130", "thiscall", ["ecx"], 5),
    "Slot.ShaderDestroy.d3d9":               ("0x006AA190", "thiscall", ["ecx"], 1),
    "Slot.ShaderDestroy.d3d9ex":             ("0x006AA190", "thiscall", ["ecx"], 1),
    "Slot.ShaderConstantsSet.d3d9":          ("0x006833E0", "thiscall", [], 4),
    "Slot.ShaderConstantsSet.d3d9ex":        ("0x006833E0", "thiscall", [], 4),
    "Slot.IShaderReload.d3d9":               ("0x006A5D50", "thiscall", ["ecx"], 3),
    "Slot.IShaderReload.d3d9ex":             ("0x006A5D50", "thiscall", ["ecx"], 3),
    "Slot.IShaderCreate.d3d9":               ("0x006A5E10", "thiscall", [], 1),
    "Slot.IShaderCreate.d3d9ex":             ("0x006A5E10", "thiscall", [], 1),
    "Slot.CursorSetVisible.d3d9":            ("0x0068E750", "thiscall", ["ecx"], 1),
    "Slot.CursorSetVisible.d3d9ex":          ("0x0068E750", "thiscall", ["ecx"], 1),
    "Slot.CursorLock.d3d9":                  ("0x00683650", "thiscall", ["ecx"], 0),
    "Slot.CursorLock.d3d9ex":                ("0x00683650", "thiscall", ["ecx"], 0),
    "Slot.CursorUnlock.d3d9":                ("0x0068E7E0", "thiscall", ["ecx"], 2),
    "Slot.CursorUnlock.d3d9ex":              ("0x0068E7E0", "thiscall", ["ecx"], 2),
    "Slot.StereoSetConvergence.d3d9":        ("0x0068E980", "thiscall", ["ecx"], 1),
    "Slot.StereoSetConvergence.d3d9ex":      ("0x0068E980", "thiscall", ["ecx"], 1),
    "Slot.StereoGetConvergence.d3d9":        ("0x006A0110", "thiscall", ["ecx"], 0),
    "Slot.StereoGetConvergence.d3d9ex":      ("0x006A0110", "thiscall", ["ecx"], 0),
    "Slot.StereoSetSeparation.d3d9":         ("0x0068E9A0", "thiscall", ["ecx"], 1),
    "Slot.StereoSetSeparation.d3d9ex":       ("0x0068E9A0", "thiscall", ["ecx"], 1),
    "Slot.StereoGetSeparation.d3d9":         ("0x006A0120", "thiscall", ["ecx"], 0),
    "Slot.StereoGetSeparation.d3d9ex":       ("0x006A0120", "thiscall", ["ecx"], 0),
    "Slot.StereoEnabled.d3d9":               ("0x006A0130", "thiscall", ["ecx"], 0),
    "Slot.StereoEnabled.d3d9ex":             ("0x006A0130", "thiscall", ["ecx"], 0),

}


class Tally:
    """Prints each case's verdict and counts them."""

    def __init__(self):
        self.total = 0
        self.failures = 0

    def __call__(self, label, ok, expected, output):
        self.total += 1
        print("%-4s %s" % ("ok" if ok else "FAIL", label))
        if ok:
            return
        self.failures += 1
        print("     expected %s" % expected)
        for line in output.strip().splitlines()[:12]:
            print("     | " + line)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--binary", default=os.environ.get("WXL_WOW_EXE"),
                        help="the stock Wow.exe for the real half; default $WXL_WOW_EXE")
    args = parser.parse_args()
    report = Tally()

    server = HTTPServer(("127.0.0.1", 0), Stub)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/mcp" % server.server_address[1]
    stub = ("--backend", "mcp", "--mcp", url)

    with tempfile.TemporaryDirectory() as temp:
        for index, (label, demo_def, thunks, want_code, want_text, extra) in enumerate(CASES):
            case = Path(temp) / ("case%02d" % index)
            build(case, demo_def, thunks)
            result = run(case, stub + tuple(extra))
            output = result.stdout + result.stderr
            ok = result.returncode == want_code and (not want_text or want_text in output)
            report(label, ok, "exit %d%s" % (want_code, ", with %r" % want_text
                                             if want_text else ""), output)

        for index, (label, text, want_code, want_text) in enumerate(BODY_CASES):
            case = Path(temp) / ("body%02d" % index)
            build(case)
            (case / "deinlined").mkdir()
            (case / "x86.def").write_text(PARENT_X86_DEF, encoding="utf-8")
            (case / "deinlined" / "Thing_CallsCleanly.gen.cpp").write_text(text, encoding="utf-8")
            result = run(case, stub + ("--points", str(case / "x86.def"), "--deinlined", str(case / "deinlined")))
            output = result.stdout + result.stderr
            report(label, result.returncode == want_code and want_text in output,
                   "exit %d, with %r" % (want_code, want_text), output)

        # The tool must say so and leave, not crash, when the MCP is not there.
        case = Path(temp) / "unreachable"
        build(case)
        result = run(case, ("--backend", "mcp", "--mcp", "http://127.0.0.1:1/mcp"))
        output = result.stdout + result.stderr
        report("the MCP unreachable",
               result.returncode == 2 and "the ghidra MCP at http://127.0.0.1:1/mcp did not "
                                          "answer" in output,
               "exit 2, with the MCP named", output)
        server.shutdown()

        local_cases(temp, report)

    # --- the real client ---
    if not args.binary:
        print("\nskip the real rows: name a stock Wow.exe with --binary or $WXL_WOW_EXE")
        print("\n%d case(s), %d failure(s)" % (report.total, report.failures))
        return 1 if report.failures else 0

    started = time.monotonic()
    result = subprocess.run([sys.executable, "-I", str(CHECKER), "--json",
                             "--binary", args.binary], capture_output=True, text=True)
    wall = time.monotonic() - started
    try:
        answer = json.loads(result.stdout)
    except ValueError:
        print("FAIL the real rows: the tool printed no JSON")
        print((result.stdout + result.stderr)[:2000])
        return 1
    report("the real table has no disagreement", not answer["problems"],
           "no problem", "\n".join(answer["problems"]))

    rows = {row["point"]: row for row in answer["rows"]}
    for point, (address, convention, registers, stack) in GROUND_TRUTH.items():
        row = rows.get(point)
        got = None if row is None else (row["address"], row["convention"],
                                        row["registers"], row["stack_dwords"])
        report("%s reads as %s %s %d" % (point, convention, ",".join(registers) or "-", stack),
               got == (address, convention, registers, stack),
               "%s" % ((address, convention, registers, stack),), str(got))
    for point in sorted(set(rows) - set(GROUND_TRUTH)):
        report("%s is a row the ground truth does not cover" % point, False,
               "a line for it in GROUND_TRUTH", "")

    # The slot table refuses a kept slot whose D3D9 function reaches past CGxDevice's members, and a
    # row whose pops are not what the vtable's function pops.
    slots = HERE.parent.parent / "include" / "wxl" / "gx" / "slots.def"
    with tempfile.TemporaryDirectory() as temp:
        text = slots.read_text(encoding="utf-8")
        text = text.replace("WXL_GX_SLOT(17, DeviceWindow, void*, (void* self), 0, front",
                            "WXL_GX_SLOT(17, DeviceWindow, void*, (void* self), 0, keep")
        text = text.replace("WXL_GX_SLOT(39, SceneClear, void, (void* self, uint32_t a0, uint32_t a1), 8,",
                            "WXL_GX_SLOT(39, SceneClear, void, (void* self, uint32_t a0), 4,")
        bad = Path(temp) / "slots.def"
        bad.write_text(text, encoding="utf-8")
        result = subprocess.run([sys.executable, "-I", str(CHECKER), "--binary", args.binary, "--slots", str(bad),
                                 "--point", "Slot.DeviceWindow", "--point", "Slot.SceneClear"],
                                capture_output=True, text=True)
        output = result.stdout + result.stderr
        report("a kept slot whose D3D9 function reads a D3D9 member is refused",
               result.returncode == 1 and "DeviceWindow reaches past CGxDevice's members at 0x0069FED0" in output,
               "exit 1 naming 0x0069FED0", output[:600])
        report("a slot row popping less than the vtable's function is refused",
               "0x006A74B0 pops 8 byte(s), the row 4" in output, "the pops named", output[:600])

    print("\n%d case(s), %d failure(s); the %d real rows in %.1fs (%.1fs with the start)"
          % (report.total, report.failures, len(rows), answer["seconds"], wall))
    return 1 if report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
