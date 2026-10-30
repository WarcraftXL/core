#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 WarcraftXL
#
# The point list read into rows: points.txt joined with the fork's definitions and core's address
# table. Shared by generate_points.py and insert_points.py.
"""Read include/wxl/points/points.txt into Point rows.

Each line of points.txt names a function of the client fork. The row is completed from the fork
itself -- the definition's return type and parameters, the file and line where its body opens,
whether it is a member and whether that member is static -- and from core's generated
functions.def -- whether the address names exactly that one function on x86, and how long it is.
Everything a generated file says about a point comes from here, so the generators cannot disagree.
"""

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "seams"))

import check_seams as cxx  # noqa: E402  (strip_cxx, split_top_level, balanced, enclosing_functions)

# --- Lowering ----------------------------------------------------------------------------------
#
# A point's signature is reduced to sizes and classes before anything else sees it, so a rename or
# a retype upstream (int32_t to an enum of the same size) changes neither the contract nor a mod's
# layout hash. Every pointer is `ptr`, and so is a reference: both pass an address in Microsoft's
# two ABIs; a handler is handed the address and writes through it.

SCALARS = {
    # spelled in the fork              lowered   C type of the contract
    "void":                            ("void", "void"),
    "int32_t":                         ("i32", "int32_t"),
    "int":                             ("i32", "int32_t"),
    "uint32_t":                        ("u32", "uint32_t"),
    "unsigned int":                    ("u32", "uint32_t"),
    "unsigned":                        ("u32", "uint32_t"),
    "int16_t":                         ("i16", "int16_t"),
    "uint16_t":                        ("u16", "uint16_t"),
    "int8_t":                          ("i8", "int8_t"),
    "char":                            ("i8", "int8_t"),
    "uint8_t":                         ("u8", "uint8_t"),
    "bool":                            ("u8", "uint8_t"),
    "int64_t":                         ("i64", "int64_t"),
    "uint64_t":                        ("u64", "uint64_t"),
    "float":                           ("f32", "float"),
    "double":                          ("f64", "double"),
    # va_list is a char* in both of Microsoft's ABIs: a pointer, passed by value.
    "va_list":                         ("ptr", "void*"),
}

CONVENTIONS = ("cdecl", "thiscall", "stdcall")


class Problem(Exception):
    pass


def fnv1a32(text):
    h = 0x811C9DC5
    for byte in text.encode("utf-8"):
        h ^= byte
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def lower(spelled, what):
    """(lowered kind, contract C type) of a type as the fork spells it."""
    t = re.sub(r"\s+", " ", spelled.replace("const ", " ").replace(" const", " ")).strip()
    t = t.replace("struct ", "").replace("class ", "").strip()
    if "&&" in t:
        raise Problem("%s is an rvalue reference (%s); a point passes scalars, pointers and references"
                      % (what, spelled))
    if "*" in t or "&" in t:
        return "ptr", "void*"
    if t in SCALARS:
        return SCALARS[t]
    raise Problem("%s has type `%s`, which generate_points.py does not lower; add it to SCALARS "
                  "(by size and class) if it is a scalar, or pass it by pointer" % (what, spelled))


class Param:
    def __init__(self, spelled, name):
        self.spelled = spelled        # the fork's declaration of the type, e.g. `CDataStore*`
        self.name = name              # the fork's name of the parameter
        self.kind = None
        self.ctype = None

    @property
    def reference(self):
        """Whether the fork passes it by reference: the point carries its address."""
        return "&" in self.spelled


def split_name(name):
    """(qualified name, [parameter types] or None) of a points.txt name: `M2Init(uint8_t*,M2Data&)`
    names one overload of M2Init by its parameter types, as the fork spells them."""
    if "(" not in name:
        return name, None
    if not name.endswith(")"):
        raise Problem("`%s`: a name with parameter types ends with `)`" % name)
    base, inner = name[:-1].split("(", 1)
    return base, [normal_type(t) for t in inner.split(",")] if inner.strip() else []


def normal_type(spelled):
    """A parameter type compared across spellings: no spaces, no top-level const on a value."""
    t = re.sub(r"\s+", " ", spelled).strip()
    t = re.sub(r"\s*([*&])\s*", r"\1", t)
    t = t.replace("struct ", "").replace("class ", "")
    return t.replace(" ", "")


def ident_of(name):
    """The C identifier of a points.txt name (see Point.ident)."""
    base, overload = split_name(name)
    ident = re.sub(r"\W", "_", base.replace("::", "_"))
    if overload is not None:
        ident += "".join("_" + re.sub(r"\W", "", t) for t in overload) or "_void"
    return ident


class Point:
    """One line of points.txt, completed."""

    def __init__(self, index, address, name, policy, thread, stability, note, origin):
        self.index = index
        self.address = address        # int, or None for a function inlined on x86
        self.name = name              # qualified, as the fork writes it (with its parameter
                                      # types when the fork overloads it)
        self.base_name, self.overload = split_name(name)
        self.parents = []             # the functions whose recompiled x86 bodies hold this one's
                                      # copies (Point rows: a listed point, or a function of the
                                      # fork that is a parent only, whose index is None)
        self.parent_names = []        # (name, origin) of its deinlined lines, until load() resolves them
        self.policy = policy
        self.thread = thread
        self.stability = stability
        self.note = note
        self.origin = origin
        self.sites = []               # (address, [arg sources], origin)
        # From the fork:
        self.file = None              # Path, relative to the fork
        self.line = None              # the line of the definition's name
        self.returns = None           # spelled
        self.params = []              # [Param], `this` excluded
        self.member = False           # a non-static member function: `this` is argument 0
        self.self_type = None         # the class, for a member
        # From functions.def:
        self.length = 0
        self.x86 = False
        self.x86_why = ""             # why there is no x86 detour, for find()'s answer
        self.x64 = True

    @property
    def ident(self):
        """The C identifier of the point: CGCamera::UpdateCallback is CGCamera_UpdateCallback; an
        overload adds the bare names of its parameter types: M2Init(uint8_t*,uint32_t,M2Data&) is
        M2Init_uint8_t_uint32_t_M2Data."""
        return ident_of(self.name)

    @property
    def listed(self):
        """Whether it is a point of points.txt, rather than a function that is only a parent."""
        return self.index is not None

    @property
    def convention(self):
        return "thiscall" if self.member else "cdecl"

    @property
    def ret_kind(self):
        return lower(self.returns, "the return of %s" % self.name)

    def contract_params(self):
        """[(C type, name)] of the contract: `self` first for a member."""
        out = [("void*", "self")] if self.member else []
        for p in self.params:
            out.append((p.ctype, p.name))
        return out

    @property
    def lowered(self):
        kinds = (["ptr"] if self.member else []) + [p.kind for p in self.params]
        return "%s(%s)" % (self.ret_kind[0], ",".join(kinds))

    @property
    def layout_hash(self):
        return fnv1a32(self.lowered)

    @property
    def runtimes(self):
        if self.x86 and self.x64:
            return "both"
        return "x86" if self.x86 else "x64"


# --- points.txt --------------------------------------------------------------------------------

POINT_LINE = re.compile(r"^(0x[0-9A-Fa-f]{8}|-)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s*(?:#\s*(.*))?$")
SITE_LINE = re.compile(r"^site\s+(\S+)\s+(0x[0-9A-Fa-f]{8})\s+(.+?)\s*(?:#.*)?$")
DEINLINE_LINE = re.compile(r"^deinlined\s+(\S+)\s+(\S+)\s*(?:#.*)?$")
SOURCE = re.compile(r"^(?:e?(?:ax|bx|cx|dx|si|di|bp)|\[(?:esp|ebp)\s*[+-]\s*(?:0x[0-9A-Fa-f]+|\d+)\])$")


def read_list(path):
    points, problems = [], []
    by_name = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        origin = "%s:%d" % (path.name, number)
        deinlined = DEINLINE_LINE.match(line)
        if deinlined:
            point, parent = by_name.get(deinlined.group(1)), deinlined.group(2)
            if point is None:
                problems.append("%s: deinlined names %s, which no line above names" % (origin, deinlined.group(1)))
                continue
            if point.address is not None:
                problems.append("%s: %s has an x86 address; deinlined is for a function the x86 compiler "
                                "inlined (address `-`)" % (origin, point.name))
                continue
            if parent in (n for n, _ in point.parent_names):
                problems.append("%s: %s is deinlined in %s twice" % (origin, point.name, parent))
                continue
            point.parent_names.append((parent, origin))
            continue
        site = SITE_LINE.match(line)
        if site:
            point = by_name.get(site.group(1))
            if point is None:
                problems.append("%s: a site of %s, which no line above names" % (origin, site.group(1)))
                continue
            args = site.group(3).replace(" ", "").split(",") if "," in site.group(3) else site.group(3).split()
            bad = [a for a in args if not SOURCE.match(a)]
            if bad:
                problems.append("%s: %s is not a register or a stack slot ([esp+8], [ebp-0x10])"
                                % (origin, bad[0]))
                continue
            point.sites.append((int(site.group(2), 16), args, origin))
            continue
        match = POINT_LINE.match(line)
        if not match:
            problems.append("%s: not `address name policy thread stability [# note]`: %s" % (origin, line))
            continue
        address, name, policy, thread, stability, note = match.groups()
        if policy not in ("replace", "observe"):
            problems.append("%s: policy is `%s`, not replace or observe" % (origin, policy))
        if thread not in ("main", "any"):
            problems.append("%s: thread is `%s`, not main or any" % (origin, thread))
        if stability not in ("stable", "provisional"):
            problems.append("%s: stability is `%s`, not stable or provisional" % (origin, stability))
        if name in by_name:
            problems.append("%s: %s is listed twice (first at %s)" % (origin, name, by_name[name].origin))
            continue
        point = Point(len(points), None if address == "-" else int(address, 16), name, policy,
                      thread, stability, (note or "").strip(), origin)
        by_name[name] = point
        points.append(point)
    return points, problems


# --- functions.def -----------------------------------------------------------------------------

OFFSET_ROW = re.compile(r'WXL_OFFSET_FUNCTION\(\s*"([^"]*)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*'
                        r'(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*"([^"]*)"\s*,\s*'
                        r'"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(\d+)\s*\)')
OFFSET_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


class Offsets:
    def __init__(self, generated):
        self.by_address = {}          # address -> [(name, bound, length, file, line)]
        self.by_name = {}             # name -> [address]
        todo, seen = [generated / "functions.def"], set()
        while todo:
            path = todo.pop()
            if path in seen or not path.is_file():
                continue
            seen.add(path)
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in OFFSET_INCLUDE.finditer(text):
                todo.append(generated / match.group(1))
            for m in OFFSET_ROW.finditer(text):
                self.by_address.setdefault(int(m.group(2), 16), []).append(
                    (m.group(1), int(m.group(3), 0), int(m.group(4), 0), m.group(7), int(m.group(8))))
                self.by_name.setdefault(m.group(1), []).append(int(m.group(2), 16))
        if not self.by_address:
            raise Problem("%s holds no WXL_OFFSET_FUNCTION row" % (generated / "functions.def"))


# --- The fork ----------------------------------------------------------------------------------

def definition_at(text, name, near_line):
    """(offset of the name, offset of the body's '{', signature text) of `name`'s definition
    starting at or after line `near_line`, in text with comments blanked."""
    lines = text.split("\n")
    start = sum(len(l) + 1 for l in lines[:max(0, near_line - 1)])
    pattern = re.compile(r"(?<![\w:])" + r"\s*::\s*".join(map(re.escape, name.split("::"))) + r"\s*\(")
    for match in pattern.finditer(text, start):
        open_at = match.end() - 1
        close = cxx.balanced(text, open_at)
        if close < 0:
            continue
        brace = text.find("{", close)
        if brace < 0 or text[close:brace].strip(" \t\r\n") not in ("", "const"):
            continue
        line_start = text.rfind("\n", 0, match.start()) + 1
        return match.start(), brace, text[line_start:match.start()], text[open_at + 1:close - 1]
    return None


def definition_params(text, name, near_line):
    """[normalised parameter types] of the definition of `name` at or after `near_line`."""
    found = definition_at(text, name, near_line)
    if not found:
        return None
    inner = found[3]
    decls = [] if inner.strip() in ("", "void") else cxx.split_top_level(inner)
    out = []
    for decl in decls:
        decl = decl.split("=")[0].strip()
        m = re.match(r"^(.*?[\s*&])([A-Za-z_]\w*)$", decl)
        out.append(normal_type(m.group(1) if m else decl))
    return out


def find_inlined(client, name, overload=None):
    """(file, line) of the `// INLINED` definition of a function the x86 compiler inlined; with
    `overload`, of the one whose parameter types are those."""
    tail = name.split("::")[-1]
    hits = []
    for path in sorted(client.rglob("*.cpp")):
        rel = path.relative_to(client)
        if rel.parts[0] in ("ThirdParty", "build", "bin", "out"):
            continue
        raw = path.read_text(encoding="utf-8", errors="replace")
        if "// INLINED" not in raw or tail not in raw:
            continue
        lines = raw.split("\n")
        for i, line in enumerate(lines):
            if line.strip() != "// INLINED":
                continue
            following = "\n".join(lines[i + 1:i + 4])
            if re.search(r"(?<![\w:])" + re.escape(name) + r"\s*\(", following):
                if overload is not None:
                    params = definition_params(cxx.strip_cxx(raw), name, i + 1)
                    if params != overload:
                        continue
                hits.append((rel, i + 2))
    return hits


def is_static_member(client, cls, method, cache):
    """Whether `method` is declared static in the definition of class `cls`, or None when the class
    is not found (a namespace)."""
    if cls not in cache:
        found = []
        pattern = re.compile(r"\b(?:class|struct)\s+" + re.escape(cls) + r"\b[^;{]*\{")
        for path in client.rglob("*.h"):
            rel = path.relative_to(client)
            if rel.parts[0] in ("ThirdParty", "build", "bin", "out"):
                continue
            raw = path.read_text(encoding="utf-8", errors="replace")
            if cls not in raw:
                continue
            text = cxx.strip_cxx(raw)
            for m in pattern.finditer(text):
                depth, i = 0, m.end() - 1
                while i < len(text):
                    if text[i] == "{":
                        depth += 1
                    elif text[i] == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    i += 1
                found.append(text[m.end():i])
        cache[cls] = found
    bodies = cache[cls]
    if not bodies:
        return None
    decl = re.compile(r"(^|[;{}:\n])([^;{}]*?)\b" + re.escape(method) + r"\s*\(", re.M)
    for body in bodies:
        for m in decl.finditer(body):
            prefix = m.group(2)
            if "static" in prefix.split():
                return True
            if prefix.strip() or m.group(1) in ";{}:\n":
                return False
    return None


def complete(point, client, offsets, cache, problems):
    """Fills a point from functions.def and the fork; a problem stops that point only."""
    try:
        if point.address is not None:
            rows = offsets.by_address.get(point.address)
            if not rows:
                raise Problem("no OFFSET tag of the fork is at 0x%08X" % point.address)
            names = sorted({r[0] for r in rows})
            if point.base_name not in names:
                raise Problem("the fork tags 0x%08X as %s, not %s" % (point.address, ", ".join(names),
                                                                       point.base_name))
            row = next(r for r in rows if r[0] == point.base_name)
            point.length = row[2]
            point.file, point.line = Path(row[3]), row[4]
            if len(names) > 1:
                point.x86_why = ("its x86 body is shared with %d other function(s) the linker folded "
                                 "into one (%s)" % (len(names) - 1, ", ".join(n for n in names if n != point.base_name)))
            elif 0 < point.length < 5:
                point.x86_why = "its x86 body is %d byte(s), too short for a detour" % point.length
            elif point.length == 0:
                point.x86_why = "its x86 length is unknown (functions.def length 0)"
            else:
                point.x86 = True
        else:
            hits = find_inlined(client, point.base_name, point.overload)
            if len(hits) != 1:
                raise Problem("no single `// INLINED` definition of %s in the fork (%d found); an "
                              "address of `-` is for a function the fork tags so%s"
                              % (point.name, len(hits), "" if point.overload is not None or len(hits) < 2 else
                                 "; name the overload by its parameter types, `%s(type,type)`" % point.name))
            point.file, point.line = hits[0]
            point.x86_why = "the x86 compiler inlined it into its callers: it has no address"

        raw = (client / point.file).read_text(encoding="utf-8", errors="replace")
        text = cxx.strip_cxx(raw)
        found = definition_at(text, point.base_name, point.line - 2)
        if not found:
            raise Problem("its definition was not found at %s:%d" % (point.file.as_posix(), point.line))
        _, brace, head, inner = found
        point.body_offset = brace
        head = head.strip()
        for word in ("static", "inline", "virtual", "WXL_CALL", "__cdecl", "__stdcall", "__thiscall"):
            head = re.sub(r"\b%s\b" % word, " ", head)
        point.returns = re.sub(r"\s+", " ", head).strip()
        if not point.returns:
            raise Problem("its definition has no return type (a constructor?)")
        decls = [] if inner.strip() in ("", "void") else cxx.split_top_level(inner)
        for i, decl in enumerate(decls):
            if decl == "...":
                raise Problem("it is variadic; a point cannot carry `...`")
            if "=" in decl:
                decl = decl.split("=")[0].strip()
            m = re.match(r"^(.*?[\s*&])([A-Za-z_]\w*)$", decl)
            if not m or m.group(2) in ("int", "float", "double", "void", "char"):
                raise Problem("parameter %d (`%s`) has no name; name it in the fork" % (i + 1, decl))
            param = Param(m.group(1).strip(), m.group(2))
            param.kind, param.ctype = lower(param.spelled, "parameter %d of %s" % (i + 1, point.name))
            point.params.append(param)
        if point.overload is not None and [normal_type(p.spelled) for p in point.params] != point.overload:
            raise Problem("its definition at %s:%d takes (%s), not the (%s) its name says"
                          % (point.file.as_posix(), point.line, ", ".join(p.spelled for p in point.params),
                             ", ".join(point.overload)))
        point.ret_kind  # checks the return lowers

        parts = point.base_name.split("::")
        if len(parts) > 1:
            static = is_static_member(client, parts[-2], parts[-1], cache)
            if static is False:
                point.member = True
                point.self_type = "::".join(parts[:-1])
        if point.member and point.ret_kind[0] in ("i64", "u64", "f32", "f64") and point.x86:
            # thiscall returns of those come back in EDX:EAX or ST0: the fastcall shim carries them.
            pass
        if point.policy == "replace" and not point.x86 and not point.x64:
            raise Problem("it exists on no runtime")
    except Problem as trouble:
        problems.append("%s: %s: %s" % (point.origin, point.name, trouble))
        return False
    return True


def function_parent(name, origin, client, offsets, cache, problems):
    """A parent that is no point of the list: a function of the fork with an address, completed as
    a point would be (index None), or None after a problem."""
    try:
        base, overload = split_name(name)
    except Problem as trouble:
        problems.append("%s: %s" % (origin, trouble))
        return None
    addresses = sorted(set(offsets.by_name.get(base, [])))
    if overload is not None and len(addresses) > 1:
        matching = []
        for address in addresses:
            row = next(r for r in offsets.by_address[address] if r[0] == base)
            text = cxx.strip_cxx((client / row[3]).read_text(encoding="utf-8", errors="replace"))
            if definition_params(text, base, max(1, row[4] - 2)) == overload:
                matching.append(address)
        addresses = matching
    if len(addresses) != 1:
        problems.append("%s: the parent %s is neither a point above nor a single function the fork tags "
                        "with an address (%d found)%s"
                        % (origin, name, len(addresses), "; name the overload by its parameter types, "
                           "`%s(type,type)`" % name if len(addresses) > 1 and overload is None else ""))
        return None
    parent = Point(None, addresses[0], name, "replace", "main", "provisional", "", origin)
    if not complete(parent, client, offsets, cache, problems):
        return None
    return parent


def load(points_txt, client, generated):
    points, problems = read_list(points_txt)
    offsets = Offsets(generated)
    cache = {}
    for point in points:
        complete(point, client, offsets, cache, problems)
    by_name = {p.name: p for p in points}
    functions = {}
    for point in points:
        for name, origin in point.parent_names:
            parent = by_name.get(name)
            if parent is None:
                if name not in functions:
                    functions[name] = function_parent(name, origin, client, offsets, cache, problems)
                parent = functions[name]
                if parent is None:
                    continue
            if not parent.x86:
                problems.append("%s: %s is deinlined in %s, which has no x86 body to replace (%s)"
                                % (origin, point.name, parent.name, parent.x86_why or "?"))
                continue
            point.parents.append(parent)
        if point.parents:
            point.x86 = True
            point.x86_why = ""
    for point in points:
        for address, args, origin in point.sites:
            if not point.x86 and point.address is not None:
                problems.append("%s: a site of %s, which has no x86 detour" % (origin, point.name))
            n = len(point.params) + (1 if point.member else 0)
            if len(args) != n:
                problems.append("%s: %d argument source(s) for %s, which takes %d" % (origin, len(args), point.name, n))
    return points, problems
