#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 WarcraftXL
#
# Checks every seam row's declared calling convention against Wow.exe 3.3.5a (build 12340).
"""Verify the x86 half of the seam table against the client binary it attaches to.

check_seams.py compares the table with the C++ that implements it. Nothing compares the table
with Wow.exe, and a row that names the wrong convention produces a detour that clobbers a
register the original reads, or cleans a stack the caller also cleans. This reads the function's
prologue and its call sites out of the binary, derives the convention and the argument count
from the instructions, and fails when the row disagrees.

The instructions come from Wow.exe itself, decoded by llvm-objdump (the default), or from a
ghidra MCP. The x64 side is direct source calls with no convention of its own, so only x86 is
checked. See README.md next to it.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
from bisect import bisect_right
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_seams as table

# --- The seam table, with the symbol column kept whole -----------------------------------------
#
# check_seams.Seam drops the "@0x..." of a symbol and keeps parameter names rather than their
# declarations. Resolving an address needs the first and sizing the stack needs the second, so the
# rows are read again here with check_seams' own scanner.

WIDE_TYPES = ("uint64_t", "int64_t", "unsigned long long", "long long", "double", "uint64",
              "int64", "__int64", "size64_t")

WITNESS_ROW = re.compile(r"\bWXL_SEAM_WITNESS\s*\(\s*(\w+)\s*,\s*(\w+)\s*,\s*([^)]*?)\s*\)")


class Row:
    """One WXL_SEAM row, as the ABI sees it."""

    def __init__(self, domain, name, spec, returns, convention, params, origin, gated):
        self.domain = domain
        self.name = name
        self.spec = spec              # the symbol column, "name" or "name@0x<address>"
        self.returns = returns
        self.convention = convention
        self.params = params          # the parameter declarations, as written
        self.origin = origin
        self.gated = gated
        self.witnesses = []           # (call site address, origin) of its WXL_SEAM_WITNESS rows

    @property
    def point(self):
        return "%s.%s" % (self.domain, self.name)

    @property
    def symbol(self):
        return self.spec.split("@")[0]

    @property
    def dwords(self):
        """How many 4-byte slots the declared parameters occupy, `this` included."""
        total = 0
        for decl in self.params:
            total += 2 if self.is_wide(decl) else 1
        return total

    @staticmethod
    def is_wide(decl):
        if "*" in decl or "&" in decl:
            return False
        return any(word in decl for word in WIDE_TYPES)

    @property
    def expected_stack(self):
        """The stack dwords the row implies, or None when the row does not pin them down."""
        if self.convention == "thiscall":
            return max(0, self.dwords - 1)
        if self.convention == "regparm":
            return None               # the split between registers and stack is the binary's
        return self.dwords


def read_rows(seams_dir, problems):
    """Every row of the table, in include order, with its symbol column and its witnesses."""
    root = seams_dir / "seams.def"
    if not root.is_file():
        problems.append("the seam table is missing: %s" % root)
        return []

    files = [root]
    root_text = table.strip_cxx(root.read_text(encoding="utf-8", errors="replace"),
                                keep_strings=True, keep_directives=True)
    for match in table.INCLUDE_DEF.finditer(root_text):
        files.append(seams_dir / match.group(1))

    rows, witnesses = [], []
    for path in files:
        if not path.is_file():
            problems.append("seams.def includes %s, which does not exist" % path.name)
            continue
        text = table.strip_cxx(path.read_text(encoding="utf-8", errors="replace"),
                               keep_strings=True)
        for match in table.SEAM_ROW.finditer(text):
            gated = match.group(1) == "_GATE"
            open_at = match.end() - 1
            close = table.balanced(text, open_at)
            if close < 0:
                continue
            columns = table.split_top_level(text[open_at + 1:close - 1])
            if len(columns) != 6:
                continue
            domain, name, spec, returns, convention, params = columns
            origin = "%s:%d" % (path.name, text.count("\n", 0, match.start()) + 1)
            inner = params.strip()[1:-1].strip()
            decls = [] if not inner else table.split_top_level(inner)
            rows.append(Row(domain, name, spec.strip().strip('"'), returns, convention,
                            decls, origin, gated))
        for match in WITNESS_ROW.finditer(text):
            origin = "%s:%d" % (path.name, text.count("\n", 0, match.start()) + 1)
            witnesses.append(("%s.%s" % (match.group(1), match.group(2)), match.group(3), origin))

    by_point = {row.point: row for row in rows}
    for point, address, origin in witnesses:
        row = by_point.get(point)
        if row is None:
            problems.append("%s: a witness for %s, which no row declares" % (origin, point))
        elif not re.fullmatch(r"0x[0-9A-Fa-f]{1,8}", address):
            problems.append("%s: the witness of %s is `%s`, not an address written 0x..."
                            % (origin, point, address))
        else:
            row.witnesses.append((int(address, 16), origin))
    return rows


POINT_ROW = re.compile(r"\bWXL_POINT_X86\s*\(")


def read_point_rows(path, problems):
    """Every WXL_POINT_X86 row of the generated point table, as a row named Point.<Ident>."""
    if not path.is_file():
        problems.append("the point table is missing: %s" % path)
        return []
    text = table.strip_cxx(path.read_text(encoding="utf-8", errors="replace"), keep_strings=True)
    rows = []
    for match in POINT_ROW.finditer(text):
        open_at = match.end() - 1
        close = table.balanced(text, open_at)
        if close < 0:
            continue
        columns = table.split_top_level(text[open_at + 1:close - 1])
        if len(columns) != 7:
            problems.append("%s: a WXL_POINT_X86 row without its seven columns" % path.name)
            continue
        ident, name, address, _length, convention, returns, params = columns
        origin = "%s:%d" % (path.name, text.count("\n", 0, match.start()) + 1)
        inner = params.strip()[1:-1].strip()
        decls = [] if not inner else table.split_top_level(inner)
        rows.append(Row("Point", ident, "%s@%s" % (name.strip().strip('"'), address.strip()), returns,
                        convention, decls, origin, False))
    return rows


PARENT_ROW = re.compile(r"\bWXL_POINT_X86_PARENT\s*\(")


def read_parent_rows(path, problems):
    """Every WXL_POINT_X86_PARENT row of the point table, as a row named Parent.<Ident>: a function
    whose body the runtime replaces with its recompilation (src/deinlined/). A parent that is a
    point of its own is its Point row already and is left out here."""
    if not path.is_file():
        return []
    text = table.strip_cxx(path.read_text(encoding="utf-8", errors="replace"), keep_strings=True)
    points = set()
    for match in POINT_ROW.finditer(text):
        close = table.balanced(text, match.end() - 1)
        if close > 0:
            points.add(table.split_top_level(text[match.end():close - 1])[1].strip().strip('"'))
    rows = []
    for match in PARENT_ROW.finditer(text):
        open_at = match.end() - 1
        close = table.balanced(text, open_at)
        if close < 0:
            continue
        columns = table.split_top_level(text[open_at + 1:close - 1])
        if len(columns) != 7:
            problems.append("%s: a WXL_POINT_X86_PARENT row without its seven columns" % path.name)
            continue
        ident, name, address, _length, convention, returns, params = columns
        name = name.strip().strip('"')
        origin = "%s:%d" % (path.name, text.count("\n", 0, match.start()) + 1)
        inner = params.strip()[1:-1].strip()
        decls = [] if not inner else table.split_top_level(inner)
        row = Row("Parent", ident.strip(), "%s@%s" % (name.split("(")[0], address.strip()), returns,
                  convention.strip(), decls, origin, False)
        row.listed = name in points
        rows.append(row)
    return rows


# --- The recompiled bodies (src/deinlined/<Parent>.gen.cpp) -----------------------------------
#
# extract.py writes each parent's body with Wow.exe's functions and globals it uses at their
# addresses, in its `namespace stock`. Every function there is a row checked like the seams'
# (named Deinlined.<shim>, once per address), and the body as a whole is held to Wow.exe's own:
# what it calls and reads, Wow.exe's parent calls and reads (check_bodies).

SHIM = re.compile(r"^\s*inline const auto (\w+) = reinterpret_cast<(.+?)\((__\w+)\*\)\((.*)\)>"
                  r"\((0x[0-9A-Fa-f]{8})\); // (\S+)", re.M)
GLOBAL = re.compile(r"^\s*inline (.+?)& (\w+) = \*reinterpret_cast<.+?\*>\((0x[0-9A-Fa-f]{8})\);", re.M)
STRING = re.compile(r"reinterpret_cast<const char\*>\((0x[0-9A-Fa-f]{8})\)")


def split_types(text):
    """A parameter list split at its top-level commas, template brackets kept whole."""
    out, depth, start = [], 0, 0
    for i, c in enumerate(text):
        if c in "<(":
            depth += 1
        elif c in ">)":
            depth -= 1
        elif c == "," and depth == 0:
            out.append(text[start:i].strip())
            start = i + 1
    if text[start:].strip():
        out.append(text[start:].strip())
    return out


class Body:
    """One recompiled parent body: the parent row, and the stock functions, globals and strings
    it names."""

    def __init__(self, parent, path):
        self.parent = parent          # its Parent or Point row
        self.path = path
        self.shims = []               # (ident, address, name, conv, ret, [param types])
        self.data = []                # (name, address): globals and strings


def read_bodies(directory, parents, problems):
    """The body of each parent row, read from <directory>/<Ident>.gen.cpp."""
    bodies = []
    for parent in parents:
        path = directory / ("%s.gen.cpp" % parent.name)
        if not path.is_file():
            problems.append("%s (%s): its recompiled body %s is missing" % (parent.point, parent.origin, path.name))
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        body = Body(parent, path)
        for m in SHIM.finditer(text):
            body.shims.append((m.group(1), int(m.group(5), 16), m.group(6), m.group(3)[2:], m.group(2).strip(),
                               split_types(m.group(4))))
        for m in GLOBAL.finditer(text):
            body.data.append((m.group(2), int(m.group(3), 16)))
        for m in STRING.finditer(text):
            body.data.append(("the string at %s" % m.group(1), int(m.group(1), 16)))
        bodies.append(body)
    return bodies


def body_rows(bodies, functions):
    """A row per stock function the bodies call, once per address: its convention as the body
    calls it (a thiscall member is written __fastcall with a dummy EDX)."""
    rows, seen = [], set()
    for body in bodies:
        origin = body.path.name
        for ident, address, name, conv, ret, params in body.shims:
            if address in seen:
                continue
            seen.add(address)
            if conv == "fastcall" and len(params) >= 2 and params[1] == "void*" and "::" in name:
                conv, params = "thiscall", [params[0]] + params[2:]
            names = sorted(n for n, at in functions.by_name.items() if address in at)
            symbol = name if name in names else (names[0] if names else name)
            rows.append(Row("Deinlined", re.sub(r"_\d+$", "", ident) if ident.endswith("_2") else ident,
                            "%s@0x%08X" % (symbol, address), ret, conv, params, origin, False))
    return rows


def bare_return(binary, address):
    """Whether the function at `address` is a lone `ret`: an empty function of the release
    client (DebugLog), which a recompiled body leaves out."""
    insns = disassemble(binary, address, address + 4)
    return bool(insns) and insns[0].mn == "ret"


def check_bodies(binary, functions, bodies, problems):
    """Each recompiled body against Wow.exe's own parent: every function it calls is one the stock
    parent calls, every function the stock parent calls is one it calls (or an empty one), and
    every global and string it names is one the stock parent reads or writes."""
    for body in bodies:
        parent = body.parent
        resolved, trouble = functions.resolve(parent.spec)
        if resolved is None:
            problems.append("%s (%s): its symbol `%s` does not resolve: %s"
                            % (parent.point, parent.origin, parent.spec, trouble))
            continue
        address, extent = resolved
        insns = trim_tables(disassemble(binary, address, address + extent), address, address + extent)
        calls = {branch_target(i) for i in insns if i.mn == "call"} - {None}
        named = set()
        for insn in insns:
            named.update(int(h, 16) for h in re.findall(r"0x[0-9a-f]{6,8}", insn.ops))
        mine = {at for _, at, _, _, _, _ in body.shims}
        where = "%s (%s)" % (body.path.name, parent.point)
        for ident, at, name, _, _, _ in body.shims:
            if at not in calls:
                problems.append("%s: calls %s at 0x%08X (%s), which Wow.exe's own body at 0x%08X does not call"
                                % (where, name, at, ident, address))
        for at in sorted(calls - mine):
            if not bare_return(binary, at):
                problems.append("%s: Wow.exe's own body at 0x%08X calls 0x%08X, which the recompiled one "
                                "does not call (and which is not an empty function)" % (where, address, at))
        for name, at in body.data:
            if at not in named:
                problems.append("%s: names %s at 0x%08X, which Wow.exe's own body at 0x%08X never reaches"
                                % (where, name, at, address))


CALL_ROW = re.compile(r"\bWXL_BINDING_CALL_X86\s*\(")


def read_call_rows(path, problems):
    """Every WXL_BINDING_CALL_X86 row: a client function the x86 binding calls by hand, read as a
    row named <domain>.<name>."""
    if not path.is_file():
        problems.append("the binding's call table is missing: %s" % path)
        return []
    text = table.strip_cxx(path.read_text(encoding="utf-8", errors="replace"), keep_strings=True)
    rows = []
    for match in CALL_ROW.finditer(text):
        open_at = match.end() - 1
        close = table.balanced(text, open_at)
        if close < 0:
            continue
        columns = table.split_top_level(text[open_at + 1:close - 1])
        if len(columns) != 6:
            problems.append("%s: a WXL_BINDING_CALL_X86 row without its six columns" % path.name)
            continue
        domain, name, spec, returns, convention, params = columns
        origin = "%s:%d" % (path.name, text.count("\n", 0, match.start()) + 1)
        inner = params.strip()[1:-1].strip()
        decls = [] if not inner else table.split_top_level(inner)
        rows.append(Row(domain.strip(), name.strip(), spec.strip().strip('"'), returns, convention.strip(),
                        decls, origin, False))
    return rows


# --- The offsets table ------------------------------------------------------------------------

OFFSET_ROW = re.compile(r'WXL_OFFSET_FUNCTION\(\s*"([^"]*)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*'
                        r'(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,')
OFFSET_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


class Functions:
    """name -> address, and address -> the function that contains it."""

    def __init__(self):
        self.by_name = {}             # name -> {address: extent}
        self._starts = []             # sorted distinct addresses
        self._extent = {}             # address -> extent (length when known, else bound)

    @classmethod
    def load(cls, generated):
        """Every WXL_OFFSET_FUNCTION row of functions.def and the files it includes."""
        found = cls()
        todo, seen = [generated / "functions.def"], set()
        while todo:
            path = todo.pop()
            if path in seen or not path.is_file():
                continue
            seen.add(path)
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in OFFSET_INCLUDE.finditer(text):
                todo.append(generated / match.group(1))
            for match in OFFSET_ROW.finditer(text):
                name = match.group(1)
                address = int(match.group(2), 16)
                bound = int(match.group(3), 0)
                length = int(match.group(4), 0)
                extent = length or bound
                found.by_name.setdefault(name, {})[address] = extent
                # Rows can share an address (folded bodies); the widest extent wins.
                found._extent[address] = max(found._extent.get(address, 0), extent)
        found._starts = sorted(found._extent)
        return found

    def resolve(self, spec):
        """The (address, extent) a seam's symbol column names, as WXL_SEAM_SITE resolves it."""
        name, _, at = spec.partition("@")
        hits = self.by_name.get(name, {})
        if at:
            want = int(at, 16)
            hits = {a: e for a, e in hits.items() if a == want}
            if not hits:
                return None, "no function named %s sits at %s" % (name, at)
        if not hits:
            return None, "no function named %s in functions.def" % name
        if len(hits) > 1:
            return None, ("%s has %d addresses (%s); write it as \"<name>@0x...\""
                          % (name, len(hits), ", ".join("0x%08X" % a for a in sorted(hits))))
        address = next(iter(hits))
        extent = hits[address]
        if not extent:
            return None, "%s has no known extent (sentinel row)" % name
        return (address, extent), None

    def containing(self, address):
        """The start of the tagged function `address` falls inside, or None."""
        index = bisect_right(self._starts, address) - 1
        if index < 0:
            return None
        start = self._starts[index]
        if address >= start + self._extent[start]:
            return None
        return start, self._extent[start]

    def starting_at(self, address):
        """The extent of the tagged function that begins at `address`, or None."""
        return self._extent.get(address) or None


# --- Instructions -----------------------------------------------------------------------------

class Insn:
    __slots__ = ("addr", "size", "mn", "ops")

    def __init__(self, addr, size, mn, ops):
        self.addr = addr
        self.size = size
        self.mn = mn
        self.ops = ops

    def __repr__(self):
        return "%08x %s %s" % (self.addr, self.mn, self.ops)

    @property
    def text(self):
        return ("%s %s" % (self.mn, self.ops)).strip()

    @property
    def operands(self):
        return [part.strip() for part in self.ops.split(",")] if self.ops.strip() else []


GHIDRA_LINE = re.compile(r"^([0-9A-Fa-f]{6,8})\s+((?:[0-9A-Fa-f]{2}\s)+)\s*(\S+)\s*(.*?)\s*$")


def parse_ghidra(text):
    """The instructions of a ghidra_disasm answer, in address order."""
    out = []
    for line in text.splitlines():
        match = GHIDRA_LINE.match(line.rstrip())
        if not match:
            continue
        size = len(match.group(2).split())
        out.append(Insn(int(match.group(1), 16), size, match.group(3).lower(),
                        match.group(4).lower()))
    return out


# An llvm-objdump line: the address, the encoding, then a tab and the instruction. A long
# encoding runs into the tab without the padding a short one gets.
OBJDUMP_LINE = re.compile(r"^\s*([0-9a-fA-F]+):\s((?:[0-9a-fA-F]{2} ?)+?)\s*(?:\t(.*))?$")
OBJDUMP_SYMBOL = re.compile(r"\s*<[^<>]*>$")
OBJDUMP_SCALE = re.compile(r"\b(\d+)\*(e?[a-z]{2})\b")
PREFIXES = {"lock", "rep", "repe", "repz", "repne", "repnz", "data16", "addr32", "cs", "ds", "es",
            "fs", "gs", "ss", "notrack", "bnd", "xacquire", "xrelease"}


def parse_objdump(text):
    """The instructions of an `llvm-objdump -d --x86-asm-syntax=intel` listing, in address order.

    The operands are put in the shape the analysis reads: lower case, the trailing `<symbol>` of a
    branch target dropped, a scaled index written `reg*N`. A prefix printed on a line of its own
    (`lock`) is folded into the instruction it prefixes, and a prefix printed before the mnemonic
    (`rep stosd`) is dropped, so the mnemonic is the instruction's. An encoding objdump cannot
    decode reads as `(bad)`. A line holding only bytes continues the instruction above it.
    """
    out, pending = [], None           # pending: (address, size) of a prefix on its own line
    for line in text.splitlines():
        match = OBJDUMP_LINE.match(line.rstrip("\r\n"))
        if not match:
            continue
        addr = int(match.group(1), 16)
        size = len(match.group(2).split())
        rest = (match.group(3) or "").strip()
        if not rest:
            if out and out[-1].addr + out[-1].size == addr:
                out[-1].size += size
            continue
        if rest.startswith("<unknown>"):
            mn, ops = "(bad)", ""
        else:
            mn, _, ops = rest.partition("\t") if "\t" in rest else rest.partition(" ")
            mn, ops = mn.strip().lower(), ops.strip()
            while mn in PREFIXES and ops:
                head, _, tail = ops.replace("\t", " ").partition(" ")
                mn, ops = head.strip().lower(), tail.strip()
            if mn in PREFIXES:
                pending = (addr, size) if pending is None else (pending[0], pending[1] + size)
                continue
        ops = OBJDUMP_SCALE.sub(r"\2*\1", OBJDUMP_SYMBOL.sub("", ops).lower())
        ops = re.sub(r"\s+", " ", ops)
        if pending is not None and pending[0] + pending[1] == addr:
            addr, size = pending[0], pending[1] + size
        pending = None
        out.append(Insn(addr, size, mn, ops))
    return out


# --- The binary -------------------------------------------------------------------------------
#
# The analysis asks two things of the binary: the instructions from an address known to be an
# instruction boundary up to an end, and the direct calls and jumps that reach an address. Two
# backends answer them: Wow.exe read from disk and decoded by llvm-objdump, and a ghidra MCP.

class Unusable(Exception):
    """The binary cannot be read; nothing can be checked against it."""


class Binary:
    """The questions the analysis asks of Wow.exe."""

    hint = ""                         # what to do when it is unusable

    def disasm(self, start, end):
        """Every instruction from `start` up to `end`, decoded linearly from `start`."""
        raise NotImplementedError

    def xrefs(self, address):
        """(from, kind) of every direct reference to `address`; kind is call, jump or data."""
        raise NotImplementedError

    def describe(self):
        """What answered, for the summary line."""
        raise NotImplementedError

    def save(self):
        pass


# The stock client: Wow.exe 3.3.5a build 12340, as Blizzard shipped it. Only .text is read, so
# only .text has to be the stock one; wxl-core-patcher adds sections and edits headers, and an
# image it patched reads the same.
STOCK = {
    "timestamp": 0x4C2452FE,
    "image_base": 0x00400000,
    "text": (0x00401000, 0x005DD3B3),           # virtual address, virtual size
    "text_sha256": "0dce76d774b2595802e97d898fb026f1911da388cf5990cf144ec410137ee785",
}

IMAGE_SCN_CNT_CODE = 0x00000020


class PeSection:
    __slots__ = ("name", "va", "size", "raw_offset", "raw_size", "flags")

    def __init__(self, name, va, size, raw_offset, raw_size, flags):
        self.name = name
        self.va = va                  # virtual address of its first byte, image base included
        self.size = size              # VirtualSize
        self.raw_offset = raw_offset
        self.raw_size = raw_size
        self.flags = flags

    def holds(self, address):
        return self.va <= address < self.va + self.size


class PeImage:
    """A PE32 image's headers and its sections' bytes, read with the standard library."""

    def __init__(self, path):
        self.path = Path(path)
        try:
            data = self.path.read_bytes()
        except OSError as trouble:
            raise Unusable("cannot read %s (%s)" % (path, trouble))
        self.data = data
        if len(data) < 0x40 or data[:2] != b"MZ":
            raise Unusable("%s is not a PE image (no MZ header)" % path)
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
            raise Unusable("%s is not a PE image (no PE signature)" % path)
        self.machine, count, self.timestamp, _, _, optional_size, _ = \
            struct.unpack_from("<HHIIIHH", data, pe + 4)
        optional = pe + 24
        if optional + optional_size > len(data):
            raise Unusable("%s is truncated inside its headers" % path)
        self.magic = struct.unpack_from("<H", data, optional)[0]
        if self.machine != 0x14C or self.magic != 0x10B:
            raise Unusable("%s is not a 32-bit x86 PE image (machine 0x%X, magic 0x%X)"
                           % (path, self.machine, self.magic))
        self.image_base = struct.unpack_from("<I", data, optional + 28)[0]
        self.sections = []
        table_at = optional + optional_size
        if table_at + 40 * count > len(data):
            raise Unusable("%s is truncated inside its section table" % path)
        for index in range(count):
            raw = data[table_at + 40 * index:table_at + 40 * index + 40]
            name = raw[:8].rstrip(b"\0").decode("ascii", errors="replace")
            size, va, raw_size, raw_offset = struct.unpack_from("<IIII", raw, 8)
            flags = struct.unpack_from("<I", raw, 36)[0]
            self.sections.append(PeSection(name, self.image_base + va, size, raw_offset,
                                           raw_size, flags))

    def section(self, name):
        return next((s for s in self.sections if s.name == name), None)

    def contents(self, section):
        """The section as mapped: its VirtualSize bytes, zero past what the file holds."""
        held = min(section.size, section.raw_size)
        body = self.data[section.raw_offset:section.raw_offset + held]
        return body + b"\0" * (section.size - len(body))


def verify_stock(image, stock=STOCK):
    """The image's .text, when the image is Wow.exe 12340 and its .text is the stock one."""
    path = image.path
    if image.timestamp != stock["timestamp"] or image.image_base != stock["image_base"]:
        raise Unusable("%s is not Wow.exe 3.3.5a build 12340: its PE timestamp is 0x%08X and its "
                       "image base 0x%08X, where 12340's are 0x%08X and 0x%08X"
                       % (path, image.timestamp, image.image_base, stock["timestamp"],
                          stock["image_base"]))
    text = image.section(".text")
    want_va, want_size = stock["text"]
    if text is None or text.va != want_va or text.size != want_size:
        raise Unusable("%s carries the 12340 headers but not its .text section (0x%08X, 0x%X "
                       "bytes)" % (path, want_va, want_size))
    body = image.contents(text)
    digest = hashlib.sha256(body).hexdigest()
    if digest != stock["text_sha256"]:
        raise Unusable("%s is build 12340, but its .text is not the stock one (sha256 %s, stock "
                       "%s): an executable whose code was patched answers for the patch, not for "
                       "the client the seams attach to" % (path, digest, stock["text_sha256"]))
    return text, body


class LocalBinary(Binary):
    """Wow.exe read from disk: llvm-objdump decodes, a scan of .text finds the references."""

    hint = ("pass --binary (or set WXL_WOW_EXE) to a stock Wow.exe 3.3.5a build 12340, and "
            "--objdump to llvm-objdump if it is not on PATH")

    def __init__(self, path, objdump, stock=STOCK):
        self.image = PeImage(path)
        if stock is None:
            self.text = next((s for s in self.image.sections if s.flags & IMAGE_SCN_CNT_CODE),
                             None)
            if self.text is None:
                raise Unusable("%s has no code section" % path)
            self.body = self.image.contents(self.text)
        else:
            self.text, self.body = verify_stock(self.image, stock)
        self.objdump = objdump
        self.runs = 0
        self._decoded = {}
        self._references = None

    def disasm(self, start, end):
        key = (start, end)
        if key in self._decoded:
            return self._decoded[key]
        if not self.text.holds(start):
            raise Unusable("0x%08X is outside %s's code (%s, 0x%08X-0x%08X)"
                           % (start, self.image.path.name, self.text.name, self.text.va,
                              self.text.va + self.text.size))
        # objdump decodes the instruction that straddles --stop-address whole; asking a little
        # further and keeping what starts before `end` gives the same list either way.
        stop = min(end + 16, self.text.va + self.text.size)
        command = [self.objdump, "-d", "--x86-asm-syntax=intel", "--print-imm-hex",
                   "--section=" + self.text.name, "--start-address=0x%x" % start,
                   "--stop-address=0x%x" % stop, str(self.image.path)]
        try:
            done = subprocess.run(command, capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.SubprocessError) as trouble:
            raise Unusable("%s did not run (%s)" % (self.objdump, trouble))
        self.runs += 1
        if done.returncode != 0:
            raise Unusable("%s failed on 0x%08X-0x%08X: %s"
                           % (self.objdump, start, stop, done.stderr.strip()[:400]))
        insns = [i for i in parse_objdump(done.stdout) if start <= i.addr < end]
        if not insns and end > start:
            raise Unusable("%s decoded nothing at 0x%08X" % (self.objdump, start))
        self._decoded[key] = insns
        return insns

    def xrefs(self, address):
        if self._references is None:
            self._references = self._scan()
        return list(self._references.get(address, ()))

    def _scan(self):
        """Every `call rel32` (E8) and `jmp rel32` (E9) in the code section, by target.

        The scan reads bytes, not instructions: an E8 inside another instruction's encoding would
        be listed too. read_site decodes each caller from its entry and says so when a listed
        address is not an instruction boundary.
        """
        found, body, base = {}, self.body, self.text.va
        last = len(body) - 5
        for match in re.finditer(rb"[\xe8\xe9]", body):
            at = match.start()
            if at > last:
                break
            rel = struct.unpack_from("<i", body, at + 1)[0]
            target = (base + at + 5 + rel) & 0xFFFFFFFF
            if self.text.holds(target):
                kind = "call" if body[at] == 0xE8 else "jump"
                found.setdefault(target, []).append((base + at, kind))
        return found

    def describe(self):
        return "%d objdump run(s) over %s" % (self.runs, self.image.path.name)


class McpBinary(Binary):
    """A ghidra MCP over its HTTP transport, with an optional on-disk cache of its answers."""

    hint = "set KIJI_MCP_TOKEN to a key granted the ghidra module, or point --mcp elsewhere"

    def __init__(self, url, cache_path=None, offline=False, token=None):
        self.url = url
        self.token = token
        self.offline = offline
        self.cache_path = cache_path
        self.cache = {}
        self.calls = 0
        if cache_path and Path(cache_path).is_file():
            self.cache = json.loads(Path(cache_path).read_text(encoding="utf-8"))

    def call(self, tool, arguments):
        key = "%s %s" % (tool, json.dumps(arguments, sort_keys=True))
        if key in self.cache:
            return self.cache[key]
        if self.offline:
            raise Unusable("the cache has no answer for `%s %s` and --offline forbids asking"
                           % (tool, arguments))
        body = {"jsonrpc": "2.0", "id": self.calls + 1, "method": "tools/call",
                "params": {"name": tool, "arguments": arguments}}
        headers = {"Content-Type": "application/json",
                   "Accept": "application/json, text/event-stream"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        request = urllib.request.Request(self.url, data=json.dumps(body).encode("utf-8"),
                                         headers=headers)
        try:
            raw = urllib.request.urlopen(request, timeout=120).read().decode("utf-8")
        except (urllib.error.URLError, OSError) as trouble:
            raise Unusable("the ghidra MCP at %s did not answer (%s)" % (self.url, trouble))
        self.calls += 1
        # The transport may answer as an event stream; the payload is the one data: line.
        if raw.lstrip().startswith("event:") or raw.lstrip().startswith("data:"):
            raw = "".join(line[5:].strip() for line in raw.splitlines()
                          if line.startswith("data:"))
        try:
            message = json.loads(raw)
        except ValueError:
            raise Unusable("the ghidra MCP at %s answered something that is not JSON-RPC"
                           % self.url)
        if "error" in message:
            raise Unusable("the ghidra MCP at %s refused `%s`: %s"
                           % (self.url, tool, message["error"]))
        result = message.get("result") or {}
        if result.get("isError"):
            raise Unusable("the ghidra MCP's %s tool failed: %s" % (tool, result))
        text = "".join(part.get("text", "") for part in result.get("content", []))
        self.cache[key] = text
        return text

    def disasm(self, start, end, budget=40):
        """ghidra_disasm counts instructions rather than bytes, so requests are chained until one
        reaches `end`, each resuming on the boundary the previous one ended on."""
        insns, addr = [], start
        while addr < end and budget > 0:
            budget -= 1
            want = max(16, min(2000, (end - addr) // 2 + 8))
            chunk = [i for i in parse_ghidra(self.call("ghidra_disasm",
                                                       {"addr": "0x%08X" % addr, "count": want}))
                     if i.addr >= addr]
            if not chunk:
                break
            insns.extend(i for i in chunk if i.addr < end)
            resume = chunk[-1].addr + chunk[-1].size
            if resume <= addr:
                break
            addr = resume
        return insns

    def xrefs(self, address):
        text = self.call("ghidra_xrefs", {"addr": "0x%08X" % address})
        head = text.split("from ")[0]             # the references to it, not from it
        out = []
        for match in XREF_ENTRY.finditer(head):
            kind = match.group(2)
            out.append((int(match.group(1), 16),
                        "call" if "CALL" in kind else "jump" if "JUMP" in kind else "data"))
        return out

    def describe(self):
        return "%d MCP call(s)" % self.calls

    def save(self):
        if self.cache_path:
            Path(self.cache_path).write_text(json.dumps(self.cache, indent=1, sort_keys=True),
                                             encoding="utf-8")


XREF_ENTRY = re.compile(r"\b([0-9A-Fa-f]{6,8})\(([A-Z_]+)\)")


# --- Reading disassembly --------------------------------------------------------------------

PARENT = {"al": "eax", "ah": "eax", "ax": "eax", "bl": "ebx", "bh": "ebx", "bx": "ebx",
          "cl": "ecx", "ch": "ecx", "cx": "ecx", "dl": "edx", "dh": "edx", "dx": "edx",
          "si": "esi", "di": "edi", "bp": "ebp", "sp": "esp",
          "eax": "eax", "ebx": "ebx", "ecx": "ecx", "edx": "edx",
          "esi": "esi", "edi": "edi", "ebp": "ebp", "esp": "esp"}
WIDTH = {"al": 8, "ah": 8, "bl": 8, "bh": 8, "cl": 8, "ch": 8, "dl": 8, "dh": 8,
         "ax": 16, "bx": 16, "cx": 16, "dx": 16, "si": 16, "di": 16, "bp": 16, "sp": 16}

REGISTER = re.compile(r"\b(e?[abcd]x|[abcd][lh]|e?(?:si|di|bp|sp))\b")
DISPLACEMENT = re.compile(r"\b(ebp|esp)\s*([+-])\s*(0x[0-9A-Fa-f]+|\d+)")

# The mnemonics whose first operand is written and not read, so a register there is not an
# argument arriving in it.
WRITE_FIRST = {"mov", "movzx", "movsx", "movabs", "lea", "pop", "xchg"}
# The mnemonics whose first operand is read and written both.
READ_WRITE_FIRST = {"add", "adc", "sub", "sbb", "and", "or", "xor", "shl", "shr", "sar", "rol",
                    "ror", "rcl", "rcr", "inc", "dec", "neg", "not", "imul", "mul", "idiv",
                    "div", "xadd", "bts", "btr", "btc", "shld", "shrd"}
IMMEDIATE = re.compile(r"^(-?0x[0-9A-Fa-f]+|-?\d+)$")


def writes_without_reading(insn, ops):
    """Whether the instruction sets its first operand, a register, to a value that does not
    depend on what the register held: `xor r, r`, `sub r, r` and `sbb r, r` (0, or the carry
    spread), `and r, 0`, `or r, -1`, and the three-operand `imul r, src, imm`."""
    if len(ops) < 2 or ops[0] not in PARENT:
        return False
    if insn.mn in ("xor", "sub", "sbb") and len(ops) == 2 and ops[0] == ops[1]:
        return True
    if insn.mn == "imul" and len(ops) == 3:
        return True
    if insn.mn in ("and", "or") and len(ops) == 2 and IMMEDIATE.match(ops[1]):
        mask = (1 << WIDTH.get(ops[0], 32)) - 1
        value = int(ops[1], 0) & mask
        return value == (0 if insn.mn == "and" else mask)
    return False


def disassemble(binary, start, end):
    """Every instruction from `start` up to `end`, decoded linearly from an instruction boundary.

    A linear decode trusts wherever it is pointed, so a wrong start yields plausible garbage.
    `start` must be a function entry or a call target.
    """
    return binary.disasm(start, end)


def registers(operand):
    """The 32-bit registers an operand names, and whether it reaches memory."""
    memory = "[" in operand
    return {PARENT[m.group(1)] for m in REGISTER.finditer(operand) if m.group(1) in PARENT}, memory


HEX_IN_MEMORY = re.compile(r"\[[^]]*?(0x[0-9A-Fa-f]{6,8})[^]]*\]")
BRANCH_TARGET = re.compile(r"^(?:0x)?([0-9a-f]{1,8})(?:\s|$)")


def branch_target(insn):
    """The address a direct call or jump goes to, or None for an indirect one."""
    match = BRANCH_TARGET.match(insn.ops.strip())
    return int(match.group(1), 16) if match else None


def trim_tables(insns, start, end):
    """The instructions up to the first switch table the function's own code points into.

    A `jmp dword ptr [reg*4 + 0x...]` is followed by its table, and a linear decode turns those
    addresses into plausible instructions that read registers the function never takes. Anything
    at or after the lowest such address inside the function is data.
    """
    limit = end
    for insn in insns:
        if insn.addr >= limit:
            break
        if insn.mn != "jmp" and "*" not in insn.ops:
            continue
        for match in HEX_IN_MEMORY.finditer(insn.ops):
            target = int(match.group(1), 16)
            if start < target < limit:
                limit = target
    return [i for i in insns if i.addr < limit]


# --- What the callee says ---------------------------------------------------------------------

VOLATILE = ("eax", "ecx", "edx")
PRESERVED = ("ebx", "esi", "edi")


class Callee:
    """What the function's own instructions say about how it was called."""

    def __init__(self):
        self.frame = False            # a push ebp / mov ebp, esp frame
        self.reg_args = []            # registers read before written, in first-read order
        self.reg_at = {}              # register -> the address it was first read at
        self.slots = {}               # stack dword index -> the address it was first read at
        self.rets = []                # (address, bytes the ret pops)
        self.notes = []
        self.unsettled = None         # why who cleans is undetermined, when it is

    @property
    def stack_dwords(self):
        """How many stack dwords the function reaches, counting a gap it skipped over."""
        widest = (max(self.slots) + 1) if self.slots else 0
        popped = max((n for _, n in self.rets), default=0) // 4
        return max(widest, popped)

    @property
    def cleans(self):
        """The bytes the function pops on return, or None when its returns disagree."""
        if not self.rets:
            return None
        amounts = {n for _, n in self.rets}
        if len(amounts) > 1:
            return None
        return amounts.pop()

    @property
    def convention(self):
        """The convention the instructions imply, or None when they do not settle it."""
        if self.reg_args and self.reg_args != ["ecx"]:
            return "regparm"
        if self.reg_args == ["ecx"]:
            return "thiscall"
        if self.cleans is None:
            # A function that ends in a tail call has no `ret` to read. With nothing on the
            # stack there is nothing to clean, and cdecl and stdcall are the same code.
            return "cdecl" if self.stack_dwords == 0 else None
        if self.cleans > 0:
            return "stdcall"
        return "cdecl"


def fills_pushed_slot(insns, position):
    """Whether the dword a `push reg` at `position` puts on the stack is only room for a value
    stored there afterwards: MSVC passes a float argument as `push ecx` then `fstp dword ptr
    [esp]`, whatever ecx held. True when a store to [esp] comes before anything else moves the
    stack or reads it."""
    for insn in insns[position + 1:]:
        ops = insn.operands
        target = ops[0].replace(" ", "") if ops else ""
        if insn.mn in ("fstp", "fst", "mov") and target.endswith("[esp]"):
            return True
        if insn.mn in ("push", "pop", "call", "ret", "retn") or "esp" in insn.ops or insn.mn.startswith("j"):
            return False
    return False


def read_callee(insns):
    """Register arguments, stack arguments and stack cleanup, from the function's own code."""
    facts = Callee()
    if not insns:
        facts.notes.append("nothing decoded at the function's address")
        return facts

    # A register this function pushes and later pops is one it is saving, not one it was handed.
    saved = {reg for i in insns if i.mn == "pop" for reg in registers(i.ops)[0]} & set(PRESERVED)

    written, seen_call, esp = set(), False, 0
    prologue = True
    for position, insn in enumerate(insns):
        ops = insn.operands
        first = ops[0] if ops else ""

        if prologue:
            # The entry run: the frame, the saves, and MSVC's `push ecx` for four bytes of frame.
            if insn.mn == "push" and first in ("ebp",) and not facts.frame:
                esp += 4
                continue
            if insn.mn == "mov" and first == "ebp" and len(ops) > 1 and ops[1] == "esp":
                facts.frame = True
                continue
            if insn.mn == "push" and first in PRESERVED:
                esp += 4
                continue
            if insn.mn == "push" and first in ("ecx", "eax") and facts.frame:
                esp += 4
                continue
            if insn.mn == "sub" and first == "esp" and len(ops) > 1 and IMMEDIATE.match(ops[1]):
                esp += int(ops[1], 0)
                continue
            prologue = False

        if insn.mn == "push":
            esp += 4
        elif insn.mn == "pop":
            esp -= 4
        elif insn.mn in ("sub", "add") and first == "esp" and len(ops) > 1 \
                and IMMEDIATE.match(ops[1]):
            esp += int(ops[1], 0) * (1 if insn.mn == "sub" else -1)

        if insn.mn in ("ret", "retn", "retf"):
            amount = int(insn.ops, 0) if IMMEDIATE.match(insn.ops.strip()) else 0
            facts.rets.append((insn.addr, amount))
            continue

        if insn.mn == "nop" or (insn.mn == "mov" and len(ops) == 2 and ops[0] == ops[1]):
            continue              # `mov edi, edi` is the padding MSVC aligns a function with

        # Reads and writes, operand by operand. A memory operand reads the registers that
        # address it whatever the mnemonic does with the value.
        reads, writes = set(), set()
        for index, operand in enumerate(ops):
            found, memory = registers(operand)
            if memory:
                reads |= found
            elif index == 0 and insn.mn in WRITE_FIRST:
                writes |= found
            elif index == 0 and insn.mn in READ_WRITE_FIRST:
                reads |= found
                writes |= found
            elif index == 0 and insn.mn.startswith("set"):
                writes |= found
            elif index == 0 and insn.mn.startswith("cmov"):
                reads |= found
                writes |= found
            else:
                reads |= found
        if insn.mn == "lea" and len(ops) > 1:
            # lea computes an address: its bracket registers are read, its first operand written.
            reads |= registers(ops[1])[0]
            writes |= registers(ops[0])[0]
            reads -= writes
        if writes_without_reading(insn, ops):
            # The first operand's old value does not reach the result; a memory source of
            # `imul r, [m], imm` is still read.
            reads = registers(ops[1])[0] if len(ops) == 3 else set()
            writes = registers(ops[0])[0]
        if insn.mn == "push" and first in saved:
            reads -= {first}          # a save, however late the compiler placed it
        if insn.mn == "push" and first in VOLATILE and fills_pushed_slot(insns, position):
            reads -= {first}          # four bytes of argument space, the value stored after

        for reg in reads - written - {"esp", "ebp"}:
            # After a call, eax, ecx and edx hold whatever the callee left: reading one there
            # says nothing about how this function was entered.
            if reg in VOLATILE and seen_call:
                continue
            if reg not in facts.reg_args:
                facts.reg_args.append(reg)
                facts.reg_at[reg] = insn.addr
        written |= writes

        # Stack arguments. [ebp+N] is only meaningful once the frame is up; without one the
        # displacement is measured against the entry stack pointer.
        for match in DISPLACEMENT.finditer(insn.ops):
            base, sign, value = match.group(1), match.group(2), int(match.group(3), 0)
            if sign == "-":
                continue
            if base == "ebp" and facts.frame:
                entry = value - 4
            elif base == "esp" and not facts.frame:
                entry = value - esp
            else:
                continue
            if entry < 4 or entry > 4 * 64:
                continue
            index = (entry - 4) // 4
            facts.slots.setdefault(index, insn.addr)

        if insn.mn == "call":
            seen_call = True

    if not facts.rets and facts.stack_dwords:
        facts.unsettled = ("no `ret` was decoded inside its extent, so who cleans its %d stack "
                           "dword(s) is undetermined" % facts.stack_dwords)
    elif not facts.rets:
        facts.notes.append("it ends without a `ret` (a tail call), but it takes nothing on the "
                           "stack, so cdecl and stdcall are the same code here")
    elif len({n for _, n in facts.rets}) > 1:
        facts.notes.append("its returns pop different amounts (%s), so the cleanup is undetermined"
                           % ", ".join("0x%08X pops %d" % r for r in facts.rets))
    if not facts.frame and facts.slots:
        facts.notes.append("it has no ebp frame; the stack slots were read through esp and may be "
                           "off if a branch moved it")
    return facts


def callee_at(binary, functions, address):
    """What the tagged function starting at `address` says about itself, or None."""
    extent = functions.starting_at(address)
    if extent is None:
        return None
    return read_callee(trim_tables(disassemble(binary, address, address + extent),
                                   address, address + extent))


# --- What the callers say ---------------------------------------------------------------------

BOUNDARY = ("call", "jmp", "ret", "retn", "leave", "int3")


class Site:
    def __init__(self, addr, pushes, cleanup, note, witness=False):
        self.addr = addr
        self.pushes = pushes          # pushes counted immediately before the call
        self.cleanup = cleanup        # bytes the caller pops after the call, or None
        self.note = note
        self.witness = witness        # declared by a WXL_SEAM_WITNESS row
        self.through = []             # calls the push count was read across (witnesses only)
        self.folded = 0               # bytes of the cleanup that are an earlier call's
        self.folded_from = None       # that earlier call

    @property
    def takes(self):
        """The dwords this call provably pushes and takes back, or None when the count of pushes
        and the cleanup do not add up."""
        if self.note or self.cleanup is None or self.pushes is None:
            return None
        return self.pushes if self.cleanup - self.folded == 4 * self.pushes else None

    def describe(self):
        if self.note:
            return "%s 0x%08X unread (%s)" % (self.kind, self.addr, self.note)
        parts = ["%s 0x%08X pushes %d" % (self.kind, self.addr, self.pushes)]
        if self.through:
            parts.append(" across the call(s) at %s"
                         % ", ".join("0x%08X" % a for a in self.through))
        if self.cleanup:
            parts.append(" and cleans %d" % self.cleanup)
            if self.folded:
                parts.append(" (%d of them the call at 0x%08X's)" % (self.folded,
                                                                     self.folded_from))
        else:
            parts.append(" and cleans nothing")
        return "".join(parts)

    @property
    def kind(self):
        return "witness" if self.witness else "caller"


def callers_of(binary, address):
    """The addresses that call `address` directly; jumps and data references are not calls."""
    return [at for at, kind in binary.xrefs(address) if kind == "call"]


def passes_registers_only(binary, functions, insn):
    """Whether `insn` is a direct call to a tagged function that takes its arguments in registers
    and reads nothing on the stack, so pushes before it are not its own."""
    target = branch_target(insn)
    facts = callee_at(binary, functions, target) if target is not None else None
    return (facts is not None and facts.convention in ("regparm", "thiscall")
            and facts.stack_dwords == 0)


def read_site(binary, functions, target, call_addr, witness=False):
    """One call site: the pushes before the call and the cleanup after it, or why neither.

    The call's own address is an instruction boundary, but the instructions before it are not
    reachable by decoding backwards. The enclosing function is decoded from its entry instead, so
    every instruction before the call is the one the processor sees.

    A witness is a call site a row declares; it may be an indirect call. Its push count is read
    across a direct call to a function that takes nothing on the stack, and a cleanup larger than
    its pushes is accounted for when the call just before them is to a function its caller
    cleans for, reading exactly the difference.
    """
    where = functions.containing(call_addr)
    if where is None:
        return Site(call_addr, None, None, "no tagged function contains it", witness)
    start, extent = where
    insns = trim_tables(disassemble(binary, start, start + extent), start, start + extent)
    at = next((n for n, i in enumerate(insns) if i.addr == call_addr), None)
    if at is None:
        return Site(call_addr, None, None,
                    "decoding 0x%08X from 0x%08X does not land on it" % (call_addr, start),
                    witness)
    if insns[at].mn == "jmp":
        return Site(call_addr, None, None,
                    "a tail call: the frame is the caller's, so no cleanup follows it", witness)
    if insns[at].mn != "call":
        return Site(call_addr, None, None, "the instruction there is `%s`" % insns[at].text,
                    witness)
    direct = branch_target(insns[at])
    if direct is not None and direct != target:
        return Site(call_addr, None, None,
                    "the call there goes to `%s`, not the seam" % insns[at].ops, witness)

    # Cleanup: `add esp, N`, or the `pop ecx` the compiler writes for one or two dwords. An
    # instruction that leaves esp alone may sit between, but a branch or a call ends the search.
    cleanup, step = None, at + 1
    popped = 0
    while step < len(insns) and step <= at + 4:
        insn = insns[step]
        if insn.mn == "add" and insn.operands[:1] == ["esp"] and len(insn.operands) > 1 \
                and IMMEDIATE.match(insn.operands[1]):
            cleanup = popped + int(insn.operands[1], 0)
            break
        if insn.mn == "pop" and insn.ops in ("ecx", "edx") and step == at + 1 + popped // 4:
            popped += 4
            cleanup = popped
            step += 1
            continue
        if insn.mn in BOUNDARY or insn.mn.startswith("j") or "esp" in insn.ops:
            break
        step += 1

    # Pushes: counted backwards to the previous boundary, so an interleaved `mov` does not hide
    # one. This is corroboration -- the callee's own `ret` is what settles who cleans.
    pushes, step, bound, through = 0, at - 1, None, []
    while step >= 0 and step >= at - 24:
        insn = insns[step]
        if insn.mn == "push":
            pushes += 1
        elif witness and insn.mn == "call" and passes_registers_only(binary, functions, insn):
            through.append(insn.addr)
        elif insn.mn in BOUNDARY or insn.mn.startswith("j") or \
                (insn.mn in ("add", "sub") and insn.operands[:1] == ["esp"]):
            bound = insn
            break
        step -= 1
    site = Site(call_addr, pushes, cleanup, None, witness)
    site.through = sorted(through)

    excess = (cleanup or 0) - 4 * pushes
    if witness and excess > 0 and bound is not None and bound.mn == "call":
        earlier = branch_target(bound)
        facts = callee_at(binary, functions, earlier) if earlier is not None else None
        if facts is not None and facts.cleans == 0 and facts.convention in ("cdecl", "regparm") \
                and 4 * facts.stack_dwords == excess:
            site.folded, site.folded_from = excess, bound.addr
    return site


# --- The regparm thunks -------------------------------------------------------------------------

THUNK_ROW = re.compile(r'\{\s*"([\w.]+)"\s*,\s*reinterpret_cast<void\*>\(&(\w+)\)')
THUNK_BODY = re.compile(r"__declspec\(naked\)\s+static\s+void\s+(\w+)\s*\(\s*\)\s*\{(.*?)\n\}",
                        re.S)
RESTORE = re.compile(r"^\s*mov\s+(eax|ebx|ecx|edx|esi|edi)\s*,\s*dword ptr \[ebp[+-]", re.M)
ORIGINAL_CALL = re.compile(r"call\s+dword ptr \[\s*g_\w*[Oo]riginal\s*\]")


def read_thunks(path, problems):
    """Each hand-written regparm thunk: the seam it stands for and the registers it restores."""
    if not path.is_file():
        problems.append("the regparm thunks are missing: %s" % path)
        return {}
    text = path.read_text(encoding="utf-8", errors="replace")
    bodies = {name: body for name, body in THUNK_BODY.findall(text)}
    thunks = {}
    for point, thunk in THUNK_ROW.findall(text):
        body = bodies.get(thunk)
        if body is None:
            problems.append("%s: the table names the thunk %s, which the file does not define"
                            % (path.name, thunk))
            continue
        # The registers put back in the instructions just before the original is called are the
        # ones the original reads them from.
        match = ORIGINAL_CALL.search(body)
        before = body[:match.start()] if match else body
        tail = before.rsplit("add  esp", 1)[-1]
        thunks[point] = [m.group(1) for m in RESTORE.finditer(tail)]
    return thunks


# --- The check ----------------------------------------------------------------------------------

class Finding:
    """One row's verdict: what the binary says, and whether the row agrees."""

    def __init__(self, row):
        self.row = row
        self.address = None
        self.callee = None
        self.convention = None        # the callee's own verdict, or the one a witness settled
        self.sites = []
        self.witnesses = []
        self.problems = []
        self.notes = []

    def as_dict(self):
        """The row's verdict as data, for a caller that wants to compare rather than read."""
        callee = self.callee

        def site(s):
            return {"address": "0x%08X" % s.addr, "pushes": s.pushes, "cleanup": s.cleanup,
                    "folded": s.folded, "through": ["0x%08X" % a for a in s.through],
                    "note": s.note}

        return {
            "point": self.row.point,
            "origin": self.row.origin,
            "symbol": self.row.spec,
            "address": "0x%08X" % self.address if self.address else None,
            "declared": self.row.convention,
            "declared_dwords": self.row.dwords,
            "convention": self.convention,
            "registers": list(callee.reg_args) if callee else [],
            "stack_dwords": callee.stack_dwords if callee else None,
            "cleans": callee.cleans if callee else None,
            "callers": [site(s) for s in self.sites],
            "witnesses": [site(s) for s in self.witnesses],
            "notes": list(self.notes),
            "problems": list(self.problems),
        }

    @property
    def observed(self):
        if self.callee is None:
            return "unreadable"
        convention = self.convention or "undetermined"
        if convention == "regparm":
            return "regparm (%s) + %d stack dword(s)" % (
                ", ".join(r.upper() for r in self.callee.reg_args), self.callee.stack_dwords)
        if convention == "thiscall":
            return "thiscall (ECX) + %d stack dword(s)" % self.callee.stack_dwords
        return "%s, %d stack dword(s)" % (convention, self.callee.stack_dwords)


def check_row(binary, functions, row, max_callers):
    """One row against the binary: resolve, read the prologue, read the call sites, compare."""
    finding = Finding(row)
    resolved, trouble = functions.resolve(row.spec)
    if resolved is None:
        finding.problems.append("%s (%s): its symbol `%s` does not resolve: %s"
                                % (row.point, row.origin, row.spec, trouble))
        return finding
    address, extent = resolved
    finding.address = address

    insns = trim_tables(disassemble(binary, address, address + extent), address,
                        address + extent)
    callee = read_callee(insns)
    finding.callee = callee
    finding.notes.extend(callee.notes)

    # Call sites, until one of them cleans the stack explicitly. MSVC folds the cleanup of a call
    # into a later adjustment, so a site without one proves nothing and the next is tried.
    for caller in callers_of(binary, address)[:max_callers]:
        site = read_site(binary, functions, address, caller)
        finding.sites.append(site)
        if site.cleanup:
            break

    expected = row.expected_stack
    observed = callee.convention

    # --- the witnesses ---
    # A witness is a call site the row declares, for a function reached only through a pointer
    # or one whose own code does not say who cleans. It is evidence for a caller-cleaned call,
    # and it is read like any other site: it must push exactly the row's stack dwords and take
    # them back, or the row is wrong.
    proven = []
    for at, origin in row.witnesses:
        site = read_site(binary, functions, address, at, witness=True)
        finding.witnesses.append(site)
        if row.convention != "cdecl":
            finding.problems.append(
                "%s (%s): a witness proves a call its caller cleans for, but the row declares %s"
                % (row.point, origin, row.convention))
        elif site.note:
            finding.problems.append("%s (%s): its witness at 0x%08X cannot be read: %s"
                                    % (row.point, origin, at, site.note))
        elif site.takes is None:
            finding.problems.append(
                "%s (%s): its witness does not add up: %s"
                % (row.point, origin, site.describe()))
        elif site.takes != expected:
            finding.problems.append(
                "%s (%s): the row's parameters fill %d stack dword(s), but its witness hands it "
                "%d: %s" % (row.point, origin, expected, site.takes, site.describe()))
        else:
            proven.append(site)

    if observed is None and callee.unsettled and proven:
        observed = "cdecl"
        finding.notes.append("its own code does not say who cleans; its witness at 0x%08X pushes "
                             "%d and takes them back, so it is cdecl"
                             % (proven[0].addr, proven[0].pushes))
    elif callee.unsettled:
        finding.notes.append(callee.unsettled)
    finding.convention = observed

    cleaning = [s for s in finding.sites + finding.witnesses if s.cleanup and not s.note]

    # --- the convention ---
    if observed is None:
        finding.notes.append("the binary does not settle the convention; the row says %s"
                             % row.convention)
    elif observed != row.convention:
        # cdecl and stdcall part company only over who cleans, and with no stack to clean they
        # are the same machine code.
        same = (callee.stack_dwords == 0 and {observed, row.convention} <= {"cdecl", "stdcall"})
        if not same:
            finding.problems.append(
                "%s (%s): declared %s, but 0x%08X is %s%s"
                % (row.point, row.origin, row.convention, address, finding.observed,
                   evidence(callee, finding.sites + finding.witnesses)))

    # --- the argument count ---
    if expected is not None and observed is not None and callee.stack_dwords != expected:
        # A cdecl function that never reads its trailing parameters is still handed them: its
        # caller pushes them and its caller takes them back, so a detour declared with them
        # reads only what is really on the stack. What the callee reads is then a lower bound,
        # and the count that decides is the caller's. That is accepted on evidence alone -- a
        # call site that pushes exactly the declared count and cleans exactly that many bytes,
        # or a witness that does.
        #
        # Both numbers are needed, because each can be wrong, and never in the same place.
        # The push count runs high at a caller's first call, where no earlier call bounds the
        # walk and it counts the caller's own prologue; but there, nothing earlier exists to be
        # folded, so the cleanup is exact and the two disagree. The cleanup runs high when MSVC
        # folds an earlier call's into it; but an earlier call is exactly what bounds the walk,
        # so the push count is exact and the two disagree again. Agreement is therefore real,
        # and a row declaring more than its caller pushes is still a parameter invented.
        unread = next((s for s in cleaning
                       if observed == "cdecl" and callee.stack_dwords < expected
                       and s.takes == expected), None)
        if unread:
            finding.notes.append(
                "0x%08X reads %d of its %d stack parameter(s); its %s at 0x%08X pushes all %d "
                "and cleans them, so the row's count stands"
                % (address, callee.stack_dwords, expected, unread.kind, unread.addr, expected))
        else:
            finding.problems.append(
                "%s (%s): the row's parameters fill %d stack dword(s), but 0x%08X reads %d%s"
                % (row.point, row.origin, expected, address, callee.stack_dwords,
                   evidence(callee, finding.sites + finding.witnesses)))
    if row.convention == "regparm" and observed == "regparm":
        total = len(callee.reg_args) + callee.stack_dwords
        if total != row.dwords:
            finding.problems.append(
                "%s (%s): the row declares %d parameter dword(s), but 0x%08X takes %d in "
                "registers (%s) and %d on the stack%s"
                % (row.point, row.origin, row.dwords, address, len(callee.reg_args),
                   ", ".join(r.upper() for r in callee.reg_args), callee.stack_dwords,
                   evidence(callee, finding.sites)))

    # --- who cleans ---
    if cleaning and observed in ("stdcall", "thiscall") and callee.stack_dwords:
        finding.problems.append(
            "%s (%s): 0x%08X cleans its own %d byte(s), but its %s at 0x%08X cleans %d too"
            % (row.point, row.origin, address, callee.cleans or 0, cleaning[0].kind,
               cleaning[0].addr, cleaning[0].cleanup))
    if cleaning:
        # A cleanup larger than this call needs is MSVC folding an earlier one into it, which
        # says nothing; one smaller than it needs does not add up and is worth a look.
        wanted = callee.stack_dwords * 4
        if cleaning[0].cleanup < wanted:
            finding.notes.append("its %s at 0x%08X cleans %d byte(s) where the function reads "
                                 "%d" % (cleaning[0].kind, cleaning[0].addr, cleaning[0].cleanup,
                                         wanted))
    elif observed == "cdecl" and callee.stack_dwords and finding.sites:
        finding.notes.append("no call site among %s cleans the stack explicitly, so cdecl rests "
                             "on the function's own `ret`"
                             % ", ".join("0x%08X" % s.addr for s in finding.sites))
    if not finding.sites and not finding.witnesses:
        finding.notes.append("no call site: nothing calls 0x%08X directly, so only the "
                             "function's own code was read" % address)
    return finding


def evidence(callee, sites):
    """The addresses a verdict was read from, so the claim can be checked rather than believed."""
    parts = []
    for reg in callee.reg_args:
        parts.append("%s first read at 0x%08X" % (reg.upper(), callee.reg_at[reg]))
    if callee.slots:
        parts.append("stack slots read at %s"
                     % ", ".join("0x%08X" % callee.slots[i] for i in sorted(callee.slots)))
    for addr, amount in callee.rets[:2]:
        parts.append("`ret %d` at 0x%08X" % (amount, addr))
    for site in sites:
        parts.append(site.describe())
    return " [" + "; ".join(parts) + "]" if parts else ""


def check_thunks(rows, thunks, findings, problems, whole_table=True):
    """A regparm row needs a thunk, a thunk needs a regparm row, and both need the same registers.

    `whole_table` is false when --point narrowed the rows: a thunk whose row was filtered out is
    then not an orphan, so that half of the check is left to a full run.
    """
    declared = {row.point: row for row in rows}
    for point, row in sorted(declared.items()):
        if row.convention == "regparm" and point not in thunks:
            problems.append("%s (%s): declared regparm, but RegparmX86.cpp has no thunk for it; "
                            "the generated detour would be attached instead"
                            % (point, row.origin))
    for point in sorted(thunks):
        row = declared.get(point)
        if row is None:
            if whole_table:
                problems.append("RegparmX86.cpp has a thunk for %s, which no row declares" % point)
        elif row.convention != "regparm":
            problems.append("RegparmX86.cpp has a thunk for %s, but the row declares it %s; a "
                            "thunk is only attached to a regparm row" % (point, row.convention))
    for finding in findings:
        wanted = thunks.get(finding.row.point)
        if wanted is None or finding.callee is None or finding.convention != "regparm":
            continue
        if set(wanted) != set(finding.callee.reg_args):
            problems.append(
                "%s (%s): its thunk restores %s before calling the original, but 0x%08X reads "
                "%s%s" % (finding.row.point, finding.row.origin,
                          ", ".join(r.upper() for r in wanted) or "nothing", finding.address,
                          ", ".join(r.upper() for r in finding.callee.reg_args),
                          evidence(finding.callee, finding.sites)))


def find_objdump(explicit):
    """The llvm-objdump to run: --objdump, else the first on PATH."""
    if explicit:
        return explicit
    for name in ["llvm-objdump"] + ["llvm-objdump-%d" % v for v in range(30, 13, -1)]:
        found = shutil.which(name)
        if found:
            return found
    return None


def open_binary(args):
    """The backend the arguments name, or a reason it cannot be opened."""
    if args.backend == "mcp":
        return McpBinary(args.mcp, args.cache, args.offline, os.environ.get("KIJI_MCP_TOKEN"))
    if args.cache or args.offline:
        raise Unusable("--cache and --offline replay MCP answers; they need --backend mcp")
    path = args.binary or os.environ.get("WXL_WOW_EXE")
    if not path:
        raise Unusable("no Wow.exe was named")
    objdump = find_objdump(args.objdump)
    if objdump is None:
        raise Unusable("no llvm-objdump on PATH")
    return LocalBinary(path, objdump)


def main(argv=None):
    here = Path(__file__).resolve().parent
    core = here.parent.parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seams", type=Path, default=core / "include" / "wxl" / "seams",
                        help="the directory holding seams.def and the domain tables")
    parser.add_argument("--offsets", type=Path,
                        default=core / "include" / "wxl" / "offsets" / "generated",
                        help="the directory holding the generated functions.def")
    parser.add_argument("--points", type=Path, default=None,
                        help="the generated x86 point table, whose rows are checked like the seams'; "
                             "default core's own, read when --seams is core's own too")
    parser.add_argument("--calls", type=Path, default=None,
                        help="the x86 binding's table of the client functions it calls, whose rows are "
                             "checked like the seams'; default core's own, read when --seams is core's own")
    parser.add_argument("--deinlined", type=Path, default=None,
                        help="the runtime's recompiled parent bodies, whose stock calls are checked like the "
                             "seams' and whose calls and globals are held to Wow.exe's own parents; default "
                             "core's own src/deinlined, read when --seams is core's own")
    parser.add_argument("--thunks", type=Path, default=core / "src" / "seams" / "RegparmX86.cpp",
                        help="the hand-written regparm thunks")
    parser.add_argument("--backend", choices=("local", "mcp"), default="local",
                        help="read Wow.exe from disk (local, the default) or ask a ghidra MCP")
    parser.add_argument("--binary", default=None,
                        help="the stock Wow.exe 3.3.5a build 12340; default $WXL_WOW_EXE")
    parser.add_argument("--objdump", default=None,
                        help="the llvm-objdump to decode with; default the first on PATH")
    parser.add_argument("--mcp", default="https://mcp.kiji.dev/ghidra/mcp",
                        help="with --backend mcp: the ghidra MCP endpoint; its bearer token is "
                             "read from $KIJI_MCP_TOKEN")
    parser.add_argument("--cache", type=Path, default=None,
                        help="with --backend mcp: a file to read and write the MCP's answers "
                             "through")
    parser.add_argument("--offline", action="store_true",
                        help="with --backend mcp: answer only out of --cache")
    parser.add_argument("--max-callers", type=int, default=8,
                        help="how many call sites to read before giving up on finding a cleanup")
    parser.add_argument("--point", action="append", default=None,
                        help="check only this Domain.Name (repeatable)")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="print what the binary says about every row")
    parser.add_argument("--json", action="store_true",
                        help="print what the binary says about every row as JSON, on stdout")
    args = parser.parse_args(argv)

    problems = []
    rows = read_rows(args.seams.resolve(), problems)
    points = args.points
    if points is None and args.seams.resolve() == (core / "include" / "wxl" / "seams").resolve():
        points = core / "include" / "wxl" / "points" / "x86.def"
    point_rows = read_point_rows(points.resolve(), problems) if points else []
    rows += point_rows
    parent_rows = read_parent_rows(points.resolve(), problems) if points else []
    rows += [row for row in parent_rows if not row.listed]
    deinlined = args.deinlined
    if deinlined is None and args.seams.resolve() == (core / "include" / "wxl" / "seams").resolve():
        deinlined = core / "src" / "deinlined"
    bodies = []
    if deinlined and parent_rows:
        by_name = {row.name: row for row in point_rows}
        parents = [by_name.get(row.name, row) for row in parent_rows]
        bodies = read_bodies(deinlined.resolve(), parents, problems)
    calls = args.calls
    if calls is None and args.seams.resolve() == (core / "include" / "wxl" / "seams").resolve():
        calls = core / "include" / "wxl" / "core" / "binding" / "calls_x86.def"
    rows += read_call_rows(calls.resolve(), problems) if calls else []
    if not rows:
        print("check_conventions: no seam row was read from %s" % args.seams, file=sys.stderr)
        for problem in problems:
            print("  " + problem, file=sys.stderr)
        return 2
    if args.point:
        wanted = set(args.point)
        rows = [row for row in rows if row.point in wanted]
        missing = wanted - {row.point for row in rows}
        if missing:
            print("check_conventions: no such row: %s" % ", ".join(sorted(missing)),
                  file=sys.stderr)
            return 2

    if not (args.offsets / "functions.def").is_file():
        print("check_conventions: --offsets (%s) holds no functions.def" % args.offsets,
              file=sys.stderr)
        return 2
    functions = Functions.load(args.offsets.resolve())
    shim_rows = body_rows(bodies, functions)
    rows += shim_rows

    thunks = read_thunks(args.thunks.resolve(), problems)

    binary = None
    started = time.monotonic()
    findings = []
    try:
        binary = open_binary(args)
        for row in rows:
            findings.append(check_row(binary, functions, row, args.max_callers))
        if bodies and not args.point:
            check_bodies(binary, functions, bodies, problems)
    except Unusable as trouble:
        hint = binary.hint if binary else (McpBinary.hint if args.backend == "mcp"
                                           else LocalBinary.hint)
        print("check_conventions: the binary is not usable: %s" % trouble, file=sys.stderr)
        print("  " + hint, file=sys.stderr)
        if binary:
            binary.save()
        return 2
    elapsed = time.monotonic() - started
    binary.save()

    check_thunks(rows, thunks, findings, problems, whole_table=not args.point)
    for finding in findings:
        problems.extend(finding.problems)

    if args.json:
        print(json.dumps({"rows": [f.as_dict() for f in findings],
                          "problems": problems, "seconds": round(elapsed, 3)}, indent=1))
    elif args.verbose:
        for finding in findings:
            print("%-32s %-12s %-10s %s"
                  % (finding.row.point,
                     "0x%08X" % finding.address if finding.address else "-",
                     finding.row.convention, finding.observed))
            for note in finding.notes:
                print("%34s %s" % ("", note))
            if finding.callee:
                print("%34s %s" % ("", evidence(finding.callee, finding.sites
                                                + finding.witnesses).strip(" []")))

    notes = sum(len(f.notes) for f in findings)
    if problems:
        print("check_conventions: %d disagreement(s) between the seam table and Wow.exe 12340"
              % len(problems), file=sys.stderr)
        for problem in problems:
            print("  " + problem, file=sys.stderr)
        return 1

    if not args.json:
        print("check_conventions: %d row(s) agree with Wow.exe 12340 (%d of them points, %d recompiled body(ies) "
              "calling %d stock function(s) as Wow.exe's own do, %d regparm thunk(s), %d witness(es), %s, %d "
              "undetermined point(s), %.1fs)"
              % (len(rows), len(point_rows), len(bodies), len(shim_rows), len(thunks),
                 sum(len(r.witnesses) for r in rows), binary.describe(), notes, elapsed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
