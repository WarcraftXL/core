#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 WarcraftXL
#
# The engine headers (sdk/engine/include/wxl/engine/), generated from the client fork.
"""Writes the engine headers a mod includes to read and write the client's own structures.

    python core/tools/engine/generate_engine.py --client <fork> --sdk <sdk> [--check]
        [--binary Wow.orig.exe] [--layouts core/include/wxl/offsets/generated/layouts.def]

core/tools/engine/engine.txt lists the types, in groups; each group is one header. For every type
the fork's headers are compiled twice with clang-cl, for i686 (the stock Wow.exe 12340) and for
x86_64 (the recompiled client), with the flags of the fork's root CMakeLists.txt, and clang's
record layouts (-fdump-record-layouts) are read back: each member's offset, type and size on each
architecture. A header then declares, in namespace wxl::engine, a struct of the fork's name whose
members sit where the fork's do, one body per architecture, each followed by static_asserts of its
size and of every member's offset, and its layout hash: XXH64 of the canonical text of the layout
(every member's offset, kind and size, an embedded struct by its own hash), so a mod built against
one layout can tell, at load, that the runtime it runs on has another.

What a member becomes:
  - a scalar, a pointer to a scalar: itself; a pointer to a type of the list: that type; a pointer
    to anything else: void (same depth);
  - a struct of the list: itself; a template of wxl/engine/support.hpp (M2Array, M2Track, TSList,
    TSGrowableArray...): that template over the mapped arguments;
  - an enumeration or a typedef: the integer of its size (uintptr_t when it is pointer-sized);
  - anything else (a struct not in the list, an anonymous union): bytes of its size, named as the
    fork names the member, with the fork's type in a comment.

The x86 numbers are checked three ways before anything is written: against the fork's own
`/* NNNN */` offset comments, against core's measured layouts.def (--layouts), and, with
--binary, against the stock Wow.exe: a `witness` line of engine.txt names a function that reads
or writes the member, whose disassembly must hold the member's offset. --check writes nothing and
fails when a header differs from what the fork gives: the fork's build runs it (wxl-engine-check),
and client/WarcraftXL/EngineLayouts.cpp holds the fork's types to the x64 rows of layouts.def.

Python 3, standard library only, and clang-cl (LLVM's, any recent version) with Microsoft's CRT
and SDK headers (xwin on Linux: --xwin, default /opt/xwin; on Windows clang-cl finds Visual
Studio's own).
"""

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from xxh64 import xxh64  # noqa: E402


class Problem(Exception):
    pass


ARCHES = (("x86", "i686-pc-windows-msvc", 4), ("x64", "x86_64-pc-windows-msvc", 8))

# Scalars as the fork spells them: (C++ type of the engine headers, size, lowered kind).
SCALARS = {
    "bool": ("bool", 1, "u8"),
    "char": ("char", 1, "i8"),
    "signed char": ("int8_t", 1, "i8"),
    "unsigned char": ("uint8_t", 1, "u8"),
    "int8_t": ("int8_t", 1, "i8"),
    "uint8_t": ("uint8_t", 1, "u8"),
    "BYTE": ("uint8_t", 1, "u8"),
    "UCHAR": ("uint8_t", 1, "u8"),
    "short": ("int16_t", 2, "i16"),
    "unsigned short": ("uint16_t", 2, "u16"),
    "int16_t": ("int16_t", 2, "i16"),
    "uint16_t": ("uint16_t", 2, "u16"),
    "WORD": ("uint16_t", 2, "u16"),
    "UINT16": ("uint16_t", 2, "u16"),
    "wchar_t": ("wchar_t", 2, "u16"),
    "int": ("int32_t", 4, "i32"),
    "unsigned int": ("uint32_t", 4, "u32"),
    "long": ("int32_t", 4, "i32"),
    "unsigned long": ("uint32_t", 4, "u32"),
    "int32_t": ("int32_t", 4, "i32"),
    "uint32_t": ("uint32_t", 4, "u32"),
    "BOOL": ("int32_t", 4, "i32"),
    "DWORD": ("uint32_t", 4, "u32"),
    "UINT": ("uint32_t", 4, "u32"),
    "UINT32": ("uint32_t", 4, "u32"),
    "LONG": ("int32_t", 4, "i32"),
    "HRESULT": ("int32_t", 4, "i32"),
    "float": ("float", 4, "f32"),
    "double": ("double", 8, "f64"),
    "long long": ("int64_t", 8, "i64"),
    "unsigned long long": ("uint64_t", 8, "u64"),
    "__int64": ("int64_t", 8, "i64"),
    "unsigned __int64": ("uint64_t", 8, "u64"),
    "int64_t": ("int64_t", 8, "i64"),
    "uint64_t": ("uint64_t", 8, "u64"),
    "fixed16": None,  # a class of the fork, listed like any other
}
POINTER_SIZED = {"size_t": "size_t", "ptrdiff_t": "ptrdiff_t", "intptr_t": "intptr_t", "uintptr_t": "uintptr_t",
                 "SIZE_T": "size_t", "ULONG_PTR": "uintptr_t", "LONG_PTR": "intptr_t", "DWORD_PTR": "uintptr_t",
                 "UINT_PTR": "uintptr_t", "INT_PTR": "intptr_t"}

# Templates whose definitions wxl/engine/support.hpp writes by hand: name -> how many leading
# template arguments it keeps (the others are the fork's policies, which change no layout).
SUPPORT = {"M2Array": 1, "M2Track": 1, "M2PartTrack": 1, "M2SplineKey": 1, "TSLink": 1, "TSList": 1,
           "TSExplicitList": 2, "TSLinkedNode": 1, "TSBaseArray": 1, "TSFixedArray": 1, "TSGrowableArray": 1}
SUPPORT_PLAIN = {"M2TrackBase"}  # plain classes support.hpp defines because a template derives from them

KEYWORDS = re.compile(r"\b(?:class|struct|union|enum)\s+")


# --- engine.txt ----------------------------------------------------------------------------------

class Type:
    def __init__(self, fork, name, group, origin):
        self.fork = fork        # as the fork spells it: CGxDevice::TextureTarget
        self.name = name        # in wxl::engine: CGxDevice_TextureTarget
        self.group = group
        self.origin = origin
        self.layout = {}        # arch -> Record
        self.hash = {}          # arch -> int
        self.canon = {}         # arch -> str
        self.members = {}       # arch -> [Member]


class Group:
    def __init__(self, stem, headers, note, origin):
        self.stem = stem
        self.headers = headers
        self.note = note
        self.origin = origin
        self.types = []


def read_spec(path):
    groups, types, witnesses, comment_ok, problems = [], {}, [], {}, []
    group = None
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].rstrip()
        note = raw.split("#", 1)[1].strip() if "#" in raw else ""
        origin = "%s:%d" % (path.name, number)
        if not line.strip():
            continue
        words = line.split()
        if words[0] == "group" and len(words) >= 3:
            headers = [h.strip() for h in " ".join(words[2:]).split(",") if h.strip()]
            group = Group(words[1], headers, note, origin)
            groups.append(group)
        elif words[0] == "witness" and len(words) == 3 and "." in words[1]:
            witnesses.append((words[1].split(".", 1)[0], words[1].split(".", 1)[1], int(words[2], 16), origin))
        elif words[0] == "comment-differs" and len(words) >= 2 and "." in words[1]:
            comment_ok[words[1]] = note or origin
        elif raw.startswith((" ", "\t")) and group is not None:
            fork = words[0]
            name = words[2] if len(words) == 3 and words[1] == "as" else re.sub(r"\W", "_", fork.replace("::", "_"))
            if len(words) not in (1, 3):
                problems.append("%s: a type line is `<fork type> [as <name>]`: %s" % (origin, raw.strip()))
                continue
            if name in types:
                problems.append("%s: %s is listed twice (first at %s)" % (origin, name, types[name].origin))
                continue
            t = Type(fork, name, group, origin)
            types[name] = t
            group.types.append(t)
        else:
            problems.append("%s: not understood: %s" % (origin, raw.strip()))
    if problems:
        raise Problem("\n".join(problems))
    return groups, types, witnesses, comment_ok


# --- The fork's flags ----------------------------------------------------------------------------

def fork_defines(client):
    """The definitions the fork's root CMakeLists.txt gives every target, per architecture."""
    text = (client / "CMakeLists.txt").read_text(encoding="utf-8")
    common = re.search(r"add_compile_definitions\(\s*\n((?:\s+\w+\s*\n)+)\s*\)", text)
    if not common:
        raise Problem("the fork's CMakeLists.txt has no add_compile_definitions( list ) block")
    defines = common.group(1).split()
    for needed in ("WOWDECOMP_ARCH_64=1", "WOWDECOMP_ARCH_32=1", "_USE_32BIT_TIME_T"):
        if needed not in text:
            raise Problem("the fork's CMakeLists.txt no longer defines %s: read what changed" % needed)
    std = re.search(r"set\(CMAKE_CXX_STANDARD (\d+)\)", text)
    return {"x86": defines + ["WOWDECOMP_ARCH_32=1", "_USE_32BIT_TIME_T"],
            "x64": defines + ["WOWDECOMP_ARCH_64=1"]}, "c++%s" % (std.group(1) if std else "17")


def default_clang_cl():
    for candidate in ("clang-cl", "/usr/lib/llvm-19/bin/clang-cl", "clang-cl-19"):
        found = shutil.which(candidate)
        if found:
            return found
    return None


def dump(client, arch, target, unit_text, clang_cl, xwin, defines, std):
    with tempfile.TemporaryDirectory() as temp:
        unit = os.path.join(temp, "engine_layouts.cpp")
        with open(unit, "w") as f:
            f.write(unit_text)
        command = [clang_cl, "--target=" + target, "/std:" + std, "/Zs", "-Wno-everything", "/I", str(client)]
        if xwin:
            command += ["/vctoolsdir", os.path.join(xwin, "crt"), "/winsdkdir", os.path.join(xwin, "sdk")]
        command += ["/D" + d for d in defines]
        command += ["-Xclang", "-fdump-record-layouts", unit]
        result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise Problem("compiling the fork's headers for %s failed:\n%s\n%s"
                      % (arch, " ".join(command), (result.stderr or result.stdout)[-4000:]))
    return result.stdout


# --- Record layouts --------------------------------------------------------------------------

class Entry:
    def __init__(self, offset, bits, depth, text):
        self.offset = offset
        self.bits = bits        # (first, last) of a bit-field, else None
        self.depth = depth
        self.text = text


class Record:
    def __init__(self, name, size, align, entries):
        self.name = name
        self.size = size
        self.align = align
        self.entries = entries


LAYOUT_LINE = re.compile(r"^\s*(\d+)(?::(\d+)-(\d+))?\s\|( +)(.*?)\s*$")


def bare(type_text):
    """A type as the dump writes it, without class/struct keywords and spaces."""
    return re.sub(r"\s+", "", KEYWORDS.sub("", type_text))


def parse_dump(text):
    records = {}
    for block in text.split("*** Dumping AST Record Layout")[1:]:
        lines = block.strip("\n").split("\n")
        head = LAYOUT_LINE.match(lines[0])
        if not head:
            continue
        name = head.group(5)
        size = re.search(r"\[sizeof=(\d+), align=(\d+)", block)
        if not size:
            continue
        entries = []
        for line in lines[1:]:
            m = LAYOUT_LINE.match(line)
            if not m:
                continue
            depth = (len(m.group(4)) - 1) // 2
            bits = (int(m.group(2)), int(m.group(3))) if m.group(2) is not None else None
            entries.append(Entry(int(m.group(1)), bits, depth, m.group(5)))
        records[bare(name)] = Record(name, int(size.group(1)), int(size.group(2)), entries)
    return records


MEMBER = re.compile(r"^(.*?)\s*([A-Za-z_]\w*)$")


class Member:
    """One member of a struct as the engine header declares it."""

    def __init__(self, offset, name, decl_type, suffix, size, kind, fork_type, bits=None, note=""):
        self.offset = offset
        self.name = name
        self.decl_type = decl_type   # C++ type text in the header
        self.suffix = suffix         # [N] dimensions
        self.size = size
        self.kind = kind             # canonical kind for the hash
        self.fork_type = fork_type
        self.bits = bits             # (first, last)
        self.note = note


# --- Mapping the fork's types --------------------------------------------------------------

class Mapper:
    def __init__(self, types, records, arch, ptr):
        self.types = types
        self.by_fork = {bare(t.fork): t for t in types.values()}
        self.records = records
        self.arch = arch
        self.ptr = ptr

    def record_of(self, text):
        return self.records.get(bare(text))

    def template(self, text):
        """(name, [args]) of a template instantiation as the dump writes it, or None."""
        t = re.sub(r"\s+", " ", KEYWORDS.sub("", text)).strip()
        m = re.match(r"^([A-Za-z_]\w*)\s*<(.*)>$", t)
        if not m:
            return None
        args, depth, cur = [], 0, ""
        for ch in m.group(2):
            if ch == "<":
                depth += 1
            elif ch == ">":
                depth -= 1
            if ch == "," and depth == 0:
                args.append(cur.strip())
                cur = ""
            else:
                cur += ch
        args.append(cur.strip())
        return m.group(1), args

    def value_type(self, text):
        """(C++ text, size, kind) of a non-pointer, non-array type the engine headers can name,
        or None."""
        t = re.sub(r"\b(const|volatile)\b", "", text).strip()
        b = bare(t)
        if b in self.by_fork:
            entry = self.by_fork[b]
            rec = self.record_of(t)
            return entry.name, (rec.size if rec else None), "rec:" + entry.name
        if b in SUPPORT_PLAIN:
            rec = self.record_of(t)
            return b, (rec.size if rec else None), "rec:" + b
        tmpl = self.template(t)
        if tmpl and tmpl[0] in SUPPORT:
            name, args = tmpl
            mapped = []
            for arg in args[:SUPPORT[name]]:
                if re.match(r"^-?\d+$", arg):
                    mapped.append(arg)
                    continue
                inner = self.value_type(arg)
                mapped.append(inner[0] if inner else ("void*" if arg.endswith("*") else "void"))
            text_out = "%s<%s>" % (name, ", ".join(mapped))
            rec = self.record_of(t)
            return text_out, (rec.size if rec else None), "tmpl:" + text_out.replace(" ", "")
        plain = re.sub(r"\s+", " ", t)
        if plain in SCALARS and SCALARS[plain]:
            ctype, size, kind = SCALARS[plain]
            return ctype, size, kind
        if plain in POINTER_SIZED:
            return POINTER_SIZED[plain], self.ptr, "uptr"
        return None

    def map(self, type_text, probed_size):
        """Member mapping: (decl type, suffix, size, kind, note); None decl type = bytes."""
        t = type_text.strip()
        dims = []
        while True:
            m = re.match(r"^(.*)\[(\d+)\]$", t)
            if not m:
                break
            dims.insert(0, int(m.group(2)))
            t = m.group(1).strip()
        count = 1
        for d in dims:
            count *= d
        suffix = "".join("[%d]" % d for d in dims)
        if "(" in t:
            # A function pointer (or a pointer to member): pointer-sized on both, a plain pointer.
            if probed_size is not None and probed_size != self.ptr * count:
                return None, "", probed_size, "bytes", "a pointer to member"
            return "void*", suffix, self.ptr * count, "ptr" + suffix, ""
        stars = 0
        while t.endswith("*") or t.endswith("* const") or t.endswith("*const"):
            t = re.sub(r"\*\s*(const)?$", "", t).strip()
            stars += 1
        if stars:
            inner = self.value_type(t)
            if inner and not inner[2].startswith("tmpl:"):
                pointee = inner[0]
            elif inner:
                pointee = inner[0]
            elif re.sub(r"\b(const|volatile)\b", "", t).strip() == "void":
                pointee = "void"
            else:
                pointee = "void"
            return pointee + "*" * stars, suffix, self.ptr * count, "ptr" + suffix, ""
        inner = self.value_type(t)
        if inner:
            size = inner[1] if inner[1] is not None else (probed_size // count if probed_size else None)
            if size is None:
                return None, "", probed_size, "bytes", ""
            return inner[0], suffix, size * count, inner[2] + suffix, ""
        # An enumeration or a typedef the table does not know: an integer of its size.
        if probed_size is not None and self.record_of(t) is None and not t.startswith(("union ", "struct ", "class ")):
            unit = probed_size // count
            if self.arch == "x64" and unit == 8 and getattr(self, "other_size", {}).get(type_text) == 4:
                return "uintptr_t", suffix, probed_size, "uptr" + suffix, ""
            names = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
            if unit in names:
                return names[unit], suffix, probed_size, "u%d%s" % (unit * 8, suffix), ""
        return None, "", probed_size, "bytes", ""


# --- Building the members of a type --------------------------------------------------------

def top_entries(record):
    """The depth-1 entries of a record (its own members, bases and vtable pointers)."""
    return [e for e in record.entries if e.depth == 1]


def build_members(t, arch, records, mapper, sizes, problems):
    record = records.get(bare(t.fork))
    if record is None:
        problems.append("%s: %s was not laid out for %s (no complete definition in the group's headers?)"
                        % (t.origin, t.fork, arch))
        return None, None
    members, base = [], None
    probe = sizes.get(t.fork, {})
    for e in top_entries(record):
        text = e.text
        if text.startswith("(") and "pointer)" in text:
            kind = "vbptr" if "vbtable" in text else "vfptr"
            members.append(Member(e.offset, "vbtable" if kind == "vbptr" else "vftable", "void* const*", "",
                                  mapper.ptr, kind, text))
            continue
        bm = re.match(r"^(.*?)\s+\((primary base|base|virtual base)\)(?:\s+\(empty\))?$", text)
        if bm:
            btext, how = bm.group(1), bm.group(2)
            brec = mapper.record_of(btext)
            bsize = brec.size if brec else 0
            if "(empty)" in text or bsize == 0:
                continue
            target = mapper.value_type(btext)
            if how == "primary base" and e.offset == 0 and target and target[2].startswith("rec:"):
                base = (target[0], bsize, target[2])
                continue
            if how == "virtual base":
                problems.append("%s: %s has a virtual base (%s), which the headers do not lay out" % (t.origin, t.fork, btext))
                return None, None
            name = "base_" + re.sub(r"\W", "_", bare(btext))
            if target:
                members.append(Member(e.offset, name, target[0], "", bsize, target[2], btext))
            else:
                members.append(Member(e.offset, name, None, "", bsize, "bytes", btext))
            continue
        mm = MEMBER.match(text)
        if not mm or text.endswith(")") or text.endswith(" "):
            # An anonymous union or struct: bytes, its size from its own layout.
            rec = mapper.record_of(text.strip())
            if rec is None:
                problems.append("%s: %s: an entry not understood at offset %d: %s" % (t.origin, t.fork, e.offset, text))
                return None, None
            members.append(Member(e.offset, "anon_%X" % e.offset, None, "", rec.size, "bytes", text.strip()))
            continue
        type_text, name = mm.group(1).strip(), mm.group(2)
        if e.bits:
            decl = mapper.value_type(type_text)
            if decl is None:
                probed = probe.get(name)
                width_type = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}.get(probed or 4, "uint32_t")
                decl = (width_type, probed or 4, "u%d" % ((probed or 4) * 8))
            members.append(Member(e.offset, name, decl[0], "", decl[1], decl[2], type_text, bits=e.bits))
            continue
        decl, suffix, size, kind, note = mapper.map(type_text, probe.get(name))
        if size is None:
            problems.append("%s: %s::%s (%s) has no size for %s" % (t.origin, t.fork, name, type_text, arch))
            return None, None
        members.append(Member(e.offset, name, decl, suffix, size, kind, type_text, note=note))
    return base, members


def canonical(t, arch, base, members, hashes):
    """The text the layout hash is taken of: one line per member, an embedded type by its hash."""
    lines = ["%s %s size=%d" % (t.name, arch, t.layout[arch].size)]
    if base:
        lines.append("base %s %016x" % (base[0], hashes.get(base[0], 0)))
    for m in members:
        kind = m.kind
        ref = kind.split(":", 1)[1].split("[")[0] if kind.startswith("rec:") else None
        if ref and ref in hashes:
            kind += "#%016x" % hashes[ref]
        bits = ":%d-%d" % m.bits if m.bits else ""
        lines.append("%d%s %s %s %d" % (m.offset, bits, m.name, kind, m.size))
    return "\n".join(lines) + "\n"


# --- Checks of the x86 layout ----------------------------------------------------------------

COMMENT_MEMBER = re.compile(r"/\*\s*([0-9A-Fa-f]{4,})\s*\*/\s*([^;]*?)\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*(?::\s*\d+\s*)?;")


def class_body(text, name):
    """The body of the definition of class or struct `name` (the last name of a qualified one)."""
    short = name.split("::")[-1]
    for m in re.finditer(r"\b(?:class|struct)\s+" + re.escape(short) + r"\b[^;{]*\{", text):
        depth, i = 0, m.end() - 1
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    return text[m.end():i]
            i += 1
    return None


def check_comments(client, groups, comment_ok, problems):
    """Every `/* NNNN */` comment of a listed type agrees with the x86 offset."""
    checked = 0
    for group in groups:
        texts = [(client / h).read_text(encoding="utf-8", errors="replace") for h in group.headers]
        for t in group.types:
            if "x86" not in t.members or t.members["x86"] is None:
                continue
            body = None
            for text in texts:
                body = class_body(text, t.fork)
                if body:
                    break
            if body is None:
                continue
            # The class's own members only: nested types and inline bodies dropped.
            flat, depth = [], 0
            for ch in body:
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                elif depth == 0:
                    flat.append(ch)
            body = "".join(flat)
            offsets ={m.name: m.offset for m in t.members["x86"]}
            for m in COMMENT_MEMBER.finditer(body):
                name = m.group(3)
                if name not in offsets:
                    continue
                checked += 1
                said = int(m.group(1), 16)
                if said != offsets[name] and "%s.%s" % (t.name, name) not in comment_ok:
                    problems.append("%s: %s::%s: the fork's comment says 0x%X, clang lays it out at 0x%X on x86 "
                                    "(fix the fork, or add `comment-differs %s.%s # why` to engine.txt)"
                                    % (t.origin, t.fork, name, said, offsets[name], t.name, name))
    return checked


LAYOUT_TYPE_ROW = re.compile(r'WXL_LAYOUT_TYPE\((\w+), "([^"]*)", (0x[0-9A-F]+)')
LAYOUT_FIELD_ROW = re.compile(r'WXL_LAYOUT_FIELD\((\w+), "([^"]*)", (0x[0-9A-F]+)')


def nested_offsets(record, records):
    """designator -> offset of every member of a record, nested ones as a.b."""
    out = {}
    stack = []
    for e in record.entries:
        mm = MEMBER.match(e.text)
        if not mm or e.text.startswith("("):
            name = None
        else:
            name = mm.group(2)
        while stack and stack[-1][0] >= e.depth:
            stack.pop()
        if re.search(r"\((primary base|base|virtual base)\)", e.text):
            stack.append((e.depth, None))
            continue
        if name is None:
            continue
        path = ".".join([s[1] for s in stack if s[1]] + [name])
        out.setdefault(path, e.offset)
        stack.append((e.depth, name))
    return out


def check_layouts_def(path, types, records, problems):
    """core's measured layouts.def and the x86 dump agree on every row of a listed type."""
    text = path.read_text(encoding="utf-8")
    ids = {m.group(1): m.group(2) for m in LAYOUT_TYPE_ROW.finditer(text)}
    by_fork = {bare(t.fork): t for t in types.values()}
    checked = 0
    for m in LAYOUT_TYPE_ROW.finditer(text):
        t = by_fork.get(bare(m.group(2)))
        if t and "x86" in t.layout and t.layout["x86"].size != int(m.group(3), 16):
            problems.append("layouts.def: sizeof(%s) is %s there, 0x%X here" % (t.fork, m.group(3), t.layout["x86"].size))
        checked += 1 if t else 0
    cache = {}
    for m in LAYOUT_FIELD_ROW.finditer(text):
        fork = ids.get(m.group(1))
        t = by_fork.get(bare(fork or ""))
        if not t or "x86" not in t.layout:
            continue
        if t.name not in cache:
            cache[t.name] = nested_offsets(t.layout["x86"], records)
        designator = m.group(2)
        if "[" in designator or designator not in cache[t.name]:
            continue
        checked += 1
        if cache[t.name][designator] != int(m.group(3), 16):
            problems.append("layouts.def: %s.%s is at %s there, 0x%X here"
                            % (t.fork, designator, m.group(3), cache[t.name][designator]))
    return checked


def function_bounds(core, address):
    rows = re.compile(r'WXL_OFFSET_FUNCTION\("[^"]*", (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+|\d+), (0x[0-9A-Fa-f]+|\d+)')
    gen = core / "include" / "wxl" / "offsets" / "generated" / "functions"
    for path in gen.rglob("*.def"):
        for m in rows.finditer(path.read_text(encoding="utf-8")):
            if int(m.group(1), 16) == address:
                return int(m.group(2), 0)
    return None


def check_witnesses(binary, core, witnesses, types, problems):
    """Each witness function's disassembly addresses the member at its x86 offset."""
    objdump = shutil.which("llvm-objdump") or shutil.which("llvm-objdump-19")
    if not objdump:
        problems.append("--binary needs llvm-objdump")
        return 0
    checked = 0
    for tname, member, address, origin in witnesses:
        t = types.get(tname)
        if not t or not t.members.get("x86"):
            problems.append("%s: witness of %s, which is not a listed type" % (origin, tname))
            continue
        found = [m for m in t.members["x86"] if m.name == member]
        if not found:
            problems.append("%s: %s has no member %s" % (origin, tname, member))
            continue
        bound = function_bounds(core, address) or 0x400
        result = subprocess.run([objdump, "-d", "--no-show-raw-insn", "-M", "intel",
                                 "--start-address=0x%X" % address, "--stop-address=0x%X" % (address + bound),
                                 str(binary)], capture_output=True, text=True)
        offset = found[0].offset
        want = re.compile(r"[+]\s*0x%x\]" % offset if offset else r"\[e[a-z]{2}\]", re.I)
        if not want.search(result.stdout):
            problems.append("%s: no instruction of 0x%08X addresses %s::%s at +0x%X in Wow.exe"
                            % (origin, address, t.fork, member, offset))
        checked += 1
    return checked


# --- Writing ---------------------------------------------------------------------------------

LICENSE_HEAD = """\
// Engine structures of World of Warcraft 3.3.5a (12340), as the client fork declares them, laid
// out for the stock Wow.exe (x86) and the recompiled client (x64). See sdk/engine/NOTICE.md.
//
// GENERATED by core/tools/engine/generate_engine.py from core/tools/engine/engine.txt and the fork's
// headers. Do not edit: change engine.txt or the fork and run the tool again.
"""


def order_types(group, types, deps):
    """The group's types with every embedded type of the group before the type embedding it."""
    done, out = set(), []

    def visit(t, trail):
        if t.name in done:
            return
        if t.name in trail:
            raise Problem("%s: %s embeds itself through %s" % (t.origin, t.name, " -> ".join(trail)))
        for d in sorted(deps.get(t.name, ())):
            dt = types[d]
            if dt.group is group:
                visit(dt, trail + [t.name])
        done.add(t.name)
        out.append(t)

    for t in group.types:
        visit(t, [])
    return out


def body_lines(t, arch, base, members):
    lines = []
    head = "struct %s" % t.name + (" : %s" % base[0] if base else "")
    lines.append("%s" % head)
    lines.append("{")
    cursor = base[1] if base else 0
    bit_unit = None
    width = max([len(m.decl_type or "uint8_t") for m in members] + [8])
    for m in members:
        if m.bits and bit_unit and bit_unit[1] == m.size and bit_unit[0] <= m.offset < bit_unit[0] + bit_unit[1]:
            pass
        elif m.offset < cursor:
            raise Problem("%s: %s::%s at %d overlaps the member before it (%d) on %s" % (t.origin, t.fork, m.name, m.offset, cursor, arch))
        else:
            if m.offset > cursor:
                lines.append("    %-*s _pad_%X[0x%X];" % (width, "uint8_t", cursor, m.offset - cursor))
            cursor = m.offset + m.size
            bit_unit = (m.offset, m.size) if m.bits else None
        comment = "0x%03X" % m.offset
        if m.decl_type is None:
            comment += "  %s" % m.fork_type
            lines.append("    %-*s %s[0x%X]; // %s" % (width, "uint8_t", m.name, m.size, comment))
        elif m.bits:
            lines.append("    %-*s %s : %d; // %s bits %d-%d" % (width, m.decl_type, m.name, m.bits[1] - m.bits[0] + 1, comment, m.bits[0], m.bits[1]))
        else:
            if m.decl_type not in ("void* const*",) and bare(m.fork_type) != bare(m.decl_type + m.suffix):
                comment += "  %s" % re.sub(r"\s+", " ", KEYWORDS.sub("", m.fork_type)).strip()
            lines.append("    %-*s %s%s; // %s" % (width, m.decl_type, m.name, m.suffix, comment))
    size = t.layout[arch].size
    if cursor < size:
        lines.append("    %-*s _pad_%X[0x%X];" % (width, "uint8_t", cursor, size - cursor))
    lines.append("")
    lines.append("    static constexpr const char* kName       = \"%s\";" % t.fork)
    lines.append("    static constexpr uint64_t    kLayoutHash = 0x%016XULL;" % t.hash[arch])
    lines.append("};")
    lines.append("static_assert(sizeof(%s) == 0x%X, \"sizeof(%s) on %s\");" % (t.name, size, t.name, arch))
    for m in members:
        if m.bits or m.kind in ("vfptr", "vbptr"):
            continue
        lines.append("static_assert(offsetof(%s, %s) == 0x%X);" % (t.name, m.name, m.offset))
    return lines


def write_group(group, types, deps, groups_of):
    out = [LICENSE_HEAD.rstrip("\n"), "//"]
    out.append("// %s" % (group.note or group.stem))
    out.append("// From: %s" % ", ".join(group.headers))
    out.append("")
    out.append("#pragma once")
    out.append("")
    out.append("#include \"wxl/engine/support.hpp\"")
    needed = sorted({groups_of[d] for t in group.types for d in deps.get(t.name, ()) if groups_of[d] != group.stem})
    for stem in needed:
        out.append("#include \"wxl/engine/%s.hpp\"" % stem)
    out.append("")
    out.append("WXL_ENGINE_BEGIN")
    out.append("")
    pointees = sorted({p for t in group.types for p in deps.get("*" + t.name, ())})
    if pointees:
        out.append("// Pointed to, defined in their own group (or below).")
        out.append("struct %s;" % ";\nstruct ".join(pointees))
        out.append("")
    ordered = order_types(group, types, deps)
    for arch, guard in (("x86", "#if WXL_ENGINE_X86"), ("x64", "#else // x64")):
        out.append(guard)
        out.append("")
        for t in ordered:
            base, members = t.members[arch + "_base"], t.members[arch]
            out += body_lines(t, arch, base, members)
            out.append("")
    out.append("#endif")
    out.append("")
    out.append("WXL_ENGINE_END")
    return "\n".join(out) + "\n"


def write_table(groups, types):
    out = [LICENSE_HEAD.rstrip("\n"), "//"]
    out.append("""\
// Every engine type, as data:
//
//   WXL_ENGINE_TYPE(Name, fork, x86 size, x86 layout hash, x64 size, x64 layout hash)
//       Name is the struct of wxl::engine, fork the type as the fork spells it (#fork names it)
//   WXL_ENGINE_FIELD(Name, member, x86 offset, x64 offset)   a named member (bit-fields left out)
//
// A runtime answers a type's hash from the row of its own architecture (wxl.engine's layout), the
// recompiled client's build holds its own types to the x64 columns (WarcraftXL/EngineLayouts.cpp),
// and the sdk test `engine` hashes the canonical texts again with the xxHash library.

#ifndef WXL_ENGINE_TYPE
#error "define WXL_ENGINE_TYPE(Name, fork, size_x86, hash_x86, size_x64, hash_x64) before including layouts.def"
#endif
#ifndef WXL_ENGINE_FIELD
#define WXL_ENGINE_FIELD(Name, member, offset_x86, offset_x64)
#define WXL_ENGINE_FIELD_DEFAULTED
#endif
""")
    for group in groups:
        out.append("// %s" % group.stem)
        for t in group.types:
            out.append("WXL_ENGINE_TYPE(%s, %s, 0x%X, 0x%016XULL, 0x%X, 0x%016XULL)"
                       % (t.name, t.fork, t.layout["x86"].size, t.hash["x86"], t.layout["x64"].size, t.hash["x64"]))
            x64 = {m.name: m for m in t.members["x64"]}
            for m in t.members["x86"]:
                if m.bits or m.kind in ("vfptr", "vbptr") or m.name not in x64 or m.name.startswith(("base_", "anon_")):
                    continue
                out.append("WXL_ENGINE_FIELD(%s, %s, 0x%X, 0x%X)" % (t.name, m.name, m.offset, x64[m.name].offset))
    out.append("")
    out.append("#ifdef WXL_ENGINE_FIELD_DEFAULTED")
    out.append("#undef WXL_ENGINE_FIELD")
    out.append("#undef WXL_ENGINE_FIELD_DEFAULTED")
    out.append("#endif")
    return "\n".join(out) + "\n"


def write_client(groups):
    out = ["// SPDX-License-Identifier: MIT", "// Copyright (c) 2026 WarcraftXL", "//",
           "// GENERATED by core/tools/engine/generate_engine.py. Do not edit: run the tool again.", "//"]
    out.append("""\
// The fork's own engine types held to the SDK's engine headers (wxl/engine/layouts.def): a size or
// an offset that moves in this source without the headers being generated again stops the build
// here, before a mod built against the old headers reads the wrong bytes.""")
    out.append("")
    out.append("#include <cstddef>")
    out.append("")
    seen = []
    for g in groups:
        for h in g.headers:
            if h not in seen:
                seen.append(h)
                out.append("#include \"%s\"" % h)
    out.append("")
    out.append("#if defined(_M_X64)")
    out.append("#define WXL_ENGINE_SIZE(x86, x64) x64")
    out.append("#else")
    out.append("#define WXL_ENGINE_SIZE(x86, x64) x86")
    out.append("#endif")
    out.append("#define WXL_ENGINE_TYPE(Name, fork, size_x86, hash_x86, size_x64, hash_x64)                  \\")
    out.append("    using wxl_engine_fork_##Name = fork;                                                    \\")
    out.append("    static_assert(sizeof(fork) == WXL_ENGINE_SIZE(size_x86, size_x64),                       \\")
    out.append("                  \"sizeof(\" #fork \") moved: run core/tools/engine/generate_engine.py\");")
    out.append("#define WXL_ENGINE_FIELD(Name, member, offset_x86, offset_x64)                              \\")
    out.append("    static_assert(offsetof(wxl_engine_fork_##Name, member) == WXL_ENGINE_SIZE(offset_x86, offset_x64), \\")
    out.append("                  #Name \"::\" #member \" moved: run core/tools/engine/generate_engine.py\");")
    out.append("#if defined(__clang__)")
    out.append("#pragma clang diagnostic ignored \"-Winvalid-offsetof\"")
    out.append("#endif")
    out.append("#include \"wxl/engine/layouts.def\"")
    return "\n".join(out) + "\n"


def write_canon(groups):
    out = [LICENSE_HEAD.rstrip("\n"), "//"]
    out.append("// The canonical text of every layout, whose XXH64 (seed 0) is the type's layout hash:")
    out.append("//   WXL_ENGINE_CANON(Name, arch, text)")
    out.append("")
    for group in groups:
        for t in group.types:
            for arch in ("x86", "x64"):
                text = t.canon[arch].replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n")
                out.append("WXL_ENGINE_CANON(%s, %s, \"%s\")" % (t.name, arch, text))
    return "\n".join(out) + "\n"


def write_all(groups):
    out = [LICENSE_HEAD.rstrip("\n"), "//", "// Every group of engine types.", "", "#pragma once", ""]
    for group in groups:
        out.append("#include \"wxl/engine/%s.hpp\"" % group.stem)
    return "\n".join(out) + "\n"


# --- Main ------------------------------------------------------------------------------------

def build(client, core, spec, clang_cl, xwin, layouts_def, binary):
    groups, types, witnesses, comment_ok = read_spec(spec)
    defines, std = fork_defines(client)
    headers = []
    for g in groups:
        for h in g.headers:
            if not (client / h).is_file():
                raise Problem("%s: %s is not a file of the fork" % (g.origin, h))
            if h not in headers:
                headers.append(h)
    problems = []
    unit = ["#include <cstddef>"] + ['#include "%s"' % h for h in headers]
    unit.append("unsigned wxl_engine_sizes[] = {")
    unit += ["    sizeof(%s)," % t.fork for t in types.values()]
    unit.append("};")
    base_unit = "\n".join(unit) + "\n"
    records_by_arch = {}
    for arch, target, ptr in ARCHES:
        records_by_arch[arch] = parse_dump(dump(client, arch, target, base_unit, clang_cl, xwin, defines[arch], std))
    # Sizes of the members whose type is not a scalar, pointer or listed struct: a probe struct
    # per type, laid out by the same compiler.
    probes = ["// probes"]
    for i, t in enumerate(types.values()):
        rec = records_by_arch["x64"].get(bare(t.fork))
        if rec is None:
            continue
        fields = []
        for e in top_entries(rec):
            mm = MEMBER.match(e.text)
            if e.bits or not mm or e.text.startswith("(") or re.search(r"\((primary base|base|virtual base)\)", e.text):
                continue
            fields.append(mm.group(2))
        probes.append("struct wxl_probe_%d { %s };" % (i, " ".join("char %s[sizeof(%s::%s)];" % (f, t.fork, f) for f in fields)
                                                      or "char none;"))
        probes.append("wxl_probe_%d wxl_probe_v%d;" % (i, i))
    sizes_by_arch = {}
    for arch, target, ptr in ARCHES:
        recs = parse_dump(dump(client, arch, target, base_unit + "\n".join(probes) + "\n", clang_cl, xwin, defines[arch], std))
        sizes = {}
        for i, t in enumerate(types.values()):
            rec = recs.get("wxl_probe_%d" % i)
            if rec is None:
                continue
            sizes[t.fork] = {}
            for e in top_entries(rec):
                m = re.match(r"^char\[(\d+)\]\s+(\w+)$", e.text)
                if m:
                    sizes[t.fork][m.group(2)] = int(m.group(1))
        sizes_by_arch[arch] = sizes

    deps = {}
    for arch, target, ptr in ARCHES:
        records = records_by_arch[arch]
        mapper = Mapper(types, records, arch, ptr)
        for t in types.values():
            rec = records.get(bare(t.fork))
            if rec:
                t.layout[arch] = rec
            base, members = build_members(t, arch, records, mapper, sizes_by_arch[arch], problems)
            t.members[arch] = members
            t.members[arch + "_base"] = base
            if members is None:
                continue
            for m in members + ([Member(0, "", base[0], "", base[1], base[2], "")] if base else []):
                if m.kind.startswith("rec:"):
                    deps.setdefault(t.name, set()).add(m.kind[4:].split("[")[0].split("#")[0])
                for word in re.findall(r"\b[A-Za-z_]\w*\b", m.decl_type or ""):
                    if word in types and word != t.name and (m.kind.startswith(("ptr", "tmpl:"))):
                        deps.setdefault("*" + t.name, set()).add(word)
                if m.kind.startswith("tmpl:"):
                    for word in re.findall(r"\b[A-Za-z_]\w*\b", m.decl_type):
                        if word in types and word != t.name:
                            deps.setdefault("*" + t.name, set()).add(word)
    if problems:
        raise Problem("\n".join(problems))
    deps = {k: {d for d in v if d in types or d in SUPPORT_PLAIN} for k, v in deps.items()}
    for t in types.values():
        deps[t.name] = {d for d in deps.get(t.name, ()) if d in types}

    # Hashes, embedded types first.
    for arch, _, _ in ARCHES:
        hashes, pending = {}, list(types.values())
        guard = 0
        while pending:
            guard += 1
            if guard > len(types) + 2:
                raise Problem("the embedding of %s does not end" % ", ".join(t.name for t in pending))
            rest = []
            for t in pending:
                if any(d not in hashes for d in deps.get(t.name, ())):
                    rest.append(t)
                    continue
                t.canon[arch] = canonical(t, arch, t.members[arch + "_base"], t.members[arch], hashes)
                t.hash[arch] = xxh64(t.canon[arch].encode("utf-8"))
                hashes[t.name] = t.hash[arch]
            pending = rest

    checks = {"comments": check_comments(client, groups, comment_ok, problems)}
    if layouts_def:
        checks["layouts.def"] = check_layouts_def(layouts_def, types, records_by_arch["x86"], problems)
    if binary:
        checks["witnesses"] = check_witnesses(binary, core, witnesses, types, problems)
    if problems:
        raise Problem("\n".join(problems))

    groups_of = {t.name: t.group.stem for t in types.values()}
    files = {"%s.hpp" % g.stem: write_group(g, types, deps, groups_of) for g in groups}
    files["layouts.def"] = write_table(groups, types)
    files["canon.def"] = write_canon(groups)
    files["engine.hpp"] = write_all(groups)
    return files, checks, types, groups


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--client", required=True, type=Path, help="the client fork's checkout")
    parser.add_argument("--sdk", required=True, type=Path, help="the sdk checkout the headers go to")
    parser.add_argument("--core", type=Path, default=HERE.parent.parent, help="core's checkout (functions.def)")
    parser.add_argument("--spec", type=Path, default=HERE / "engine.txt")
    parser.add_argument("--clang-cl", default=default_clang_cl())
    parser.add_argument("--xwin", default="/opt/xwin" if os.path.isdir("/opt/xwin") else None)
    parser.add_argument("--layouts", type=Path, help="core's layouts.def, to compare the x86 numbers with")
    parser.add_argument("--binary", type=Path, help="the stock Wow.exe 12340, to check the witness lines")
    parser.add_argument("--check", action="store_true", help="write nothing; fail when a header differs")
    args = parser.parse_args()
    if not args.clang_cl:
        print("generate_engine: no clang-cl; name one with --clang-cl", file=sys.stderr)
        return 2
    layouts = args.layouts or (args.core / "include/wxl/offsets/generated/layouts.def")
    try:
        files, checks, types, groups_of_build = build(args.client, args.core, args.spec, args.clang_cl, args.xwin,
                                     layouts if layouts.is_file() else None, args.binary)
    except Problem as trouble:
        print("generate_engine: %s" % trouble, file=sys.stderr)
        return 1
    out = args.sdk / "engine" / "include" / "wxl" / "engine"
    paths = {out / name: text for name, text in files.items()}
    paths[args.client / "WarcraftXL" / "EngineLayouts.gen.cpp"] = write_client(groups_of_build)
    differs = []
    for path, text in sorted(paths.items()):
        name = path.name
        old = path.read_text(encoding="utf-8") if path.is_file() else ""
        if old == text:
            continue
        differs.append(name)
        if args.check:
            sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), text.splitlines(True),
                                                       str(path), "generated", n=1))
        else:
            out.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
    summary = "%d types in %d headers; x86 checked against %s" % (
        len(types), len(groups_of_build), ", ".join("%s (%d)" % (k, v) for k, v in checks.items()))
    if args.check:
        if differs:
            print("generate_engine --check: %s differ(s) from what the fork gives: run generate_engine.py"
                  % ", ".join(differs), file=sys.stderr)
            return 1
        print("generate_engine --check: %s, up to date" % summary)
    else:
        print("generate_engine: %s; %s" % (summary, ("wrote " + ", ".join(differs)) if differs else "nothing changed"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
