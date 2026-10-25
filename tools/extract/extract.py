#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 WarcraftXL
#
# A function of the client fork as C++ a mod compiles for the stock client (x86).
"""Extract a function of the client fork as a starting template for the stock client (x86).

    python core/tools/extract/extract.py <name | 0xADDRESS> --client <fork>
        [--binary Wow.orig.exe] [--out file.cpp] [--namespace ns] [--runtime] [--check]

The fork is the 3.3.5a client decompiled: every function carries its address in Wow.exe 12340.
The tool reads the function's definition through clang's AST (the fork's own headers, compiled
32-bit with its flags) and writes it back as a .cpp a mod builds against the SDK's engine headers
(sdk/engine) for x86:

  - every call to a function the fork tags with an address becomes a call to that address in
    Wow.exe, with the stock function's convention (a member is thiscall: `self` in ECX, written
    as __fastcall with a dummy EDX, as the point detours do);
  - every global the fork tags (`// STATIC:`) becomes a reference to its address;
  - `this` becomes `self`, the first parameter of a member function;
  - a callee the 2008 compiler inlined (`// INLINED`) is extracted too, as a function of its own:
    that is how a function inlined in Wow.exe becomes a real one again (de-inlining);
  - the release client's empty functions (DebugLog) are empty here too; memory comes from Storm
    (SMemAlloc and its kin are stock calls like any other), with `__FILE__` written as the stock
    client spells it (".\\M2Shared.cpp"), which wxl.footprint keys Storm's blocks by;
  - the runtime's own lines in the fork (seam calls, WXL_POINT_ lines) are left out.

A function the fork has several of under one name (overloads, template instantiations) is told
apart by its parameter types; instantiations of one template the fork tags with a list of
addresses are told apart, with --binary, by the order of the calls in the stock function that
holds them (the extracted function, or its caller for an inlined one): each call of the template
in the source takes the next call of its candidates in Wow.exe. Without --binary, or when the
orders do not match, such a call is left as a `#error` naming its candidates.

What the output is not: compiled or checked by the tool. Engine types it names come from
wxl/engine (a type the headers lack is `void` behind a pointer); inline helpers of the fork's
headers (M2Array::operator[], Data()) come from wxl/engine/support.hpp; anything else the
function uses is listed at the top of the file as left to the modder.

--runtime writes what core compiles into the x86 runtime (core/src/deinlined/): the function
lives in namespace wxl::core::deinlined, and an extracted callee that is a point of points.txt
(a `deinlined` line) is entered through the point while it has handlers. --check writes nothing
and fails when --out differs from what the tool gives.

Python 3, standard library only, and clang-cl (LLVM's) with Microsoft's headers (xwin on Linux).
"""

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORE = HERE.parent.parent
sys.path.insert(0, str(HERE.parent / "points"))
sys.path.insert(0, str(HERE.parent / "engine"))

import points_table as table  # noqa: E402
from generate_engine import default_clang_cl, fork_defines  # noqa: E402


class Problem(Exception):
    pass


# --- The tables --------------------------------------------------------------------------------

STATIC_ROW = re.compile(r'WXL_OFFSET_STATIC\(\s*"([^"]*)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*"([^"]*)"\s*,\s*(\d+)\s*\)')


class Rows:
    """functions.def and statics.def by name."""

    def __init__(self, generated):
        self.functions = {}   # name -> [(address, length, file, line)]
        offsets = table.Offsets(generated)
        for address, rows in offsets.by_address.items():
            for name, bound, length, file, line in rows:
                self.functions.setdefault(name, []).append((address, length, file, line))
        self.statics = {}
        for path in (generated / "statics").rglob("*.def"):
            for m in STATIC_ROW.finditer(path.read_text(encoding="utf-8")):
                self.statics.setdefault(m.group(1), []).append((int(m.group(2), 16), m.group(3), int(m.group(4))))
        self.folded = {}
        for address, rows in offsets.by_address.items():
            self.folded[address] = sorted({r[0] for r in rows})

    def engine_names(self, sdk):
        path = sdk / "engine" / "include" / "wxl" / "engine" / "layouts.def"
        names = set()
        if path.is_file():
            names = set(re.findall(r"^WXL_ENGINE_TYPE\((\w+),", path.read_text(encoding="utf-8"), re.M))
        return names | {"M2Array", "M2Track", "M2TrackBase", "M2PartTrack", "M2SplineKey", "TSLink", "TSList",
                        "TSExplicitList", "TSLinkedNode", "TSBaseArray", "TSFixedArray", "TSGrowableArray"}


# --- clang's AST ---------------------------------------------------------------------------------

def ast(client, rel, name_filter, clang_cl, xwin, sdk):
    defines, std = fork_defines(client)
    command = [clang_cl, "--target=i686-pc-windows-msvc", "/std:" + std, "/Zs", "-Wno-everything",
               "/I", str(client), "/I", str(CORE / "include"), "/I", str(sdk / "include"), "/DNDEBUG"]
    if xwin:
        command += ["/vctoolsdir", os.path.join(xwin, "crt"), "/winsdkdir", os.path.join(xwin, "sdk")]
    command += ["/D" + d for d in defines["x86"]]
    command += ["-Xclang", "-ast-dump=json", "-Xclang", "-ast-dump-filter=" + name_filter, str(client / rel)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise Problem("clang could not read %s:\n%s" % (rel, (result.stderr or result.stdout)[-3000:]))
    import json
    decoder, text, at, out = json.JSONDecoder(), result.stdout, 0, []
    while True:
        start = text.find("{", at)
        if start < 0:
            break
        node, at = decoder.raw_decode(text, start)
        out.append(node)
    return out


def walk(node):
    yield node
    for child in node.get("inner", []) or []:
        yield from walk(child)


def begin(node):
    return node.get("range", {}).get("begin", {}).get("offset")


def end(node):
    e = node.get("range", {}).get("end", {})
    if e.get("offset") is None:
        return None
    return e["offset"] + e.get("tokLen", 0)


# --- Types ---------------------------------------------------------------------------------------

def convention_of(qual, member):
    """The x86 convention clang's type of a function says: an attribute, or the default."""
    m = re.search(r"__attribute__\(\((stdcall|fastcall|thiscall|cdecl)\)\)", qual)
    if m:
        return m.group(1)
    return "thiscall" if member else "cdecl"


def split_params(qual):
    """(return text, [parameter texts], variadic) of a function type as clang writes it."""
    qual = re.sub(r"__attribute__\(\([^)]*\)\)", "", qual).strip()
    qual = re.sub(r"\)\s*const\s*$", ")", qual)
    depth, open_at = 0, None
    for i in range(len(qual) - 1, -1, -1):
        if qual[i] == ")":
            depth += 1
        elif qual[i] == "(":
            depth -= 1
            if depth == 0:
                open_at = i
                break
    if open_at is None:
        raise Problem("not a function type: %s" % qual)
    ret, inner = qual[:open_at].strip(), qual[open_at + 1:qual.rfind(")")]
    params = [p.strip() for p in table.cxx.split_top_level(inner)] if inner.strip() and inner.strip() != "void" else []
    variadic = bool(params) and params[-1] == "..."
    if variadic:
        params = params[:-1]
    return ret, params, variadic


def engine_type(text, known):
    """A type of the fork's as the engine headers can name it: a pointer or reference to a type
    they lack is to void."""
    t = re.sub(r"\b(class|struct|enum|union)\s+", "", text).strip()
    t = re.sub(r"\s+([*&])", r"\1", t)
    words = [w for w in re.findall(r"[A-Za-z_]\w*", t)
             if w not in ("const", "volatile", "unsigned", "signed", "int", "char", "short", "long", "float",
                          "double", "bool", "void", "__int64")
             and not re.match(r"^u?int(8|16|32|64|ptr)_t$|^size_t$|^ptrdiff_t$|^va_list$", w)]
    if all(w in known for w in words):
        return t
    if t.endswith(("*", "&")):
        const = "const " if t.startswith("const ") else ""
        stars = len(t) - len(t.rstrip("*&"))
        return const + "void" + "*" * stars
    return None


def normal(text):
    return table.normal_type(re.sub(r"\b(class|struct|enum|union)\s+", "", text))


# --- The extraction ------------------------------------------------------------------------------

class Function:
    """A function of the fork being extracted: its definition's AST and source."""

    def __init__(self, name, rel, line, address, node, source):
        self.name = name                # qualified: CM2Shared::FinishLoading
        self.rel = rel
        self.line = line
        self.address = address          # None for an inlined one
        self.node = node
        self.source = source
        self.member = node["kind"] == "CXXMethodDecl" and node.get("storageClass") != "static"
        self.cls = name.rsplit("::", 1)[0] if self.member or node["kind"] == "CXXMethodDecl" else None
        self.ret, self.params, self.variadic = split_params(node["type"]["qualType"])
        self.param_names = [p.get("name", "a%d" % i) for i, p in enumerate(
            n for n in node.get("inner", []) if n.get("kind") == "ParmVarDecl")]
        self.local_ids = {n["id"] for n in walk(node) if n.get("kind") in ("VarDecl", "ParmVarDecl")}
        self.overload_ident = None      # the top function named by its types: a parent overload
        self.calls = []                 # (node, Target), in source order
        self.callees = []               # local Function objects this one calls

    @property
    def ident(self):
        if self.overload_ident:
            return self.overload_ident
        return re.sub(r"\W", "_", self.name.replace("::", "_"))


class Target:
    """Where a call goes."""

    def __init__(self, kind, name, ident, ret, params, variadic, member, address=None, candidates=None,
                 local=None, note="", conv="cdecl"):
        self.kind = kind                # stock, local, empty, kept, unresolved
        self.name = name
        self.ident = ident
        self.ret = ret
        self.params = params
        self.variadic = variadic
        self.member = member
        self.address = address
        self.candidates = candidates or []
        self.local = local
        self.note = note
        self.conv = conv              # cdecl, stdcall, fastcall or thiscall, as the fork declares it


class Extractor:
    def __init__(self, client, sdk, rows, clang_cl, xwin, points):
        self.client = client
        self.sdk = sdk
        self.rows = rows
        self.clang_cl = clang_cl
        self.xwin = xwin
        self.known = rows.engine_names(sdk)
        self.points = points
        self.asts = {}
        self.functions = {}     # (rel, line) -> Function
        self.stock = {}         # ident -> Target
        self.globals = {}       # name -> (address, type)
        self.left = []          # what the modder must provide
        self.order = []         # Functions, callees first
        self.row_names = {(r[0], r[2], r[3]): name for name, rs in rows.functions.items() for r in rs}
        self.empties = {}       # name -> Target of an empty function of the release client

    # -- finding definitions --

    def nodes(self, rel, last):
        key = (rel, last)
        if key not in self.asts:
            self.asts[key] = ast(self.client, rel, last, self.clang_cl, self.xwin, self.sdk)
        return self.asts[key]

    def definition(self, name, rel, line, address):
        key = (str(rel), line)
        if key in self.functions:
            return self.functions[key]
        raw = (self.client / rel).read_text(encoding="utf-8", errors="replace")
        text = table.cxx.strip_cxx(raw)
        found = table.definition_at(text, name, max(1, line - 2))
        if not found:
            raise Problem("no definition of %s at %s:%d" % (name, rel, line))
        name_line = text.count("\n", 0, found[0]) + 1
        last = name.split("::")[-1]
        chosen = None
        for top in self.nodes(str(rel), last):
            for node in walk(top):
                if node.get("kind") not in ("FunctionDecl", "CXXMethodDecl") or node.get("name") != last:
                    continue
                if not any(c.get("kind") == "CompoundStmt" for c in node.get("inner", [])):
                    continue
                loc = node.get("loc", {})
                if loc.get("line") == name_line or (loc.get("offset") is not None and abs(loc["offset"] - found[0]) <= len(name)):
                    chosen = node
                    break
            if chosen:
                break
        if chosen is None:
            raise Problem("clang's AST of %s holds no definition of %s at line %d" % (rel, name, name_line))
        # An out-of-class definition carries no `static`: the declaration in the class does.
        if chosen["kind"] == "CXXMethodDecl" and chosen.get("storageClass") != "static":
            for top in self.nodes(str(rel), last):
                for node in walk(top):
                    if (node.get("kind") == "CXXMethodDecl" and node.get("name") == last
                            and node.get("storageClass") == "static"
                            and node.get("type", {}).get("qualType") == chosen["type"]["qualType"]):
                        chosen = dict(chosen, storageClass="static")
                        break
        function = Function(name, str(rel), name_line, address, chosen, raw)
        self.functions[key] = function
        return function

    # -- resolving a call --

    def by_type(self, rows, ret_params):
        """The rows among `rows` whose definition takes the call's parameter types."""
        _, want, _ = ret_params
        want = [normal(p) for p in want]
        out = []
        for row in rows:
            address, length, file, line = row
            row_name = self.row_names.get((address, file, line), self.current_name)
            path = self.client / file
            if not path.is_file():
                continue
            text = table.cxx.strip_cxx(path.read_text(encoding="utf-8", errors="replace"))
            params = table.definition_params(text, row_name, max(1, line - 2))
            if params is None:
                continue
            if params == want:
                out.append((row, 0))
                continue
            # A template's parameters stand for anything: T& matches M2Array<M2Sequence>&.
            if len(params) != len(want):
                continue
            score, ok = 0, True
            for have, need in zip(params, want):
                if have == need:
                    score += len(have)
                    continue
                rx = re.sub(r"\b(T|U|V)\b", "(.+)", re.escape(have))
                if re.fullmatch(rx, need):
                    score += len(re.sub(r"\b(T|U|V)\b", "", have))
                else:
                    ok = False
                    break
            if ok:
                out.append((row, -1000 + score))
        return out

    def resolve(self, caller, callee_text, ref_name, qual, member_class, member):
        ret, params, variadic = split_params(qual)
        bare = re.sub(r"<.*>", "", callee_text).strip()
        names = []
        if member_class:
            names.append("%s::%s" % (member_class, ref_name))
        if "::" in bare:
            names.append(bare)
        if caller.cls:
            names.append("%s::%s" % (caller.cls, ref_name))
        names.append(ref_name)
        explicit = re.search(r"<(.*)>", callee_text)
        for name in names:
            rows = self.rows.functions.get(name, [])
            special = None
            if explicit:
                special = self.rows.functions.get("%s<%s>" % (name, re.sub(r"\s+", "", explicit.group(1))))
            if not rows and not special:
                continue
            self.current_name = name
            scored = self.by_type(rows, (ret, params, variadic)) if rows else []
            # An explicit specialization the fork names `M2Init<M2Sequence>` beats the template.
            for targ_name, targ_rows in self.rows.functions.items():
                if targ_name.startswith(name + "<") and targ_name.endswith(">"):
                    self.current_name = name
                    for row, score in self.by_type(targ_rows, (ret, params, variadic)):
                        if score == 0:
                            scored.append((row, 10 ** 6))
            if not scored:
                continue
            best = max(s for _, s in scored)
            top = [r for r, s in scored if s == best]
            ident = self.ident_for(name, top)
            target = Target("stock", name, ident, ret, params, variadic, member, conv=convention_of(qual, member))
            if len({r[0] for r in top}) == 1:
                target.address = top[0][0]
            else:
                target.candidates = sorted({r[0] for r in top})
            return target
        # Not tagged: inlined in Wow.exe, or an empty function of the release client.
        for name in names:
            base = name
            hits = table.find_inlined(self.client, base, [normal(p) for p in params])
            if len(hits) == 1:
                rel, line = hits[0]
                local = self.definition(base, rel, line, None)
                return Target("local", base, local.ident, ret, params, variadic, member, local=local)
        if self.empty_inline(ref_name):
            return Target("empty", ref_name, ref_name, ret, params, variadic, member,
                          note="an empty function in the release client")
        return Target("unresolved", names[0], ref_name, ret, params, variadic, member)

    def empty_inline(self, name):
        pattern = re.compile(r"inline\s+void\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{\s*\}")
        for path in (self.client / "Engine" / "Source" / "Storm" / "H").glob("*.h"):
            if pattern.search(path.read_text(encoding="utf-8", errors="replace")):
                return True
        return False

    def ident_for(self, name, rows):
        base = re.sub(r"\W", "_", name.replace("::", "_").replace("<", "_").replace(">", ""))
        base = re.sub(r"_+", "_", base).strip("_")
        if len({r[0] for r in rows}) == 1:
            others = self.rows.functions.get(name, [])
            if len(others) > 1:
                return "%s_%08X" % (base, rows[0][0])
        return base

    # -- reading one function --

    def scan(self, function):
        """Collects the calls and globals of a function, and extracts its inlined callees."""
        for node in self.own(function, function.node):
            kind = node.get("kind")
            if kind in ("CallExpr", "CXXMemberCallExpr"):
                target = self.call_target(function, node)
                if target is None:
                    continue
                function.calls.append((node, target))
                if target.kind == "local" and target.local is not function and target.local not in function.callees:
                    function.callees.append(target.local)
                    if not target.local.calls and target.local not in self.order:
                        self.scan(target.local)
                if target.kind == "stock" and target.ident not in self.stock:
                    self.stock[target.ident] = target
            elif kind == "DeclRefExpr":
                ref = node.get("referencedDecl", {})
                if ref.get("kind") == "VarDecl" and ref.get("id") not in function.local_ids:
                    rows = self.rows.statics.get(ref.get("name"), [])
                    if rows:
                        same = [r for r in rows if r[1] == function.rel] or rows
                        self.globals[ref["name"]] = (same[0][0], ref.get("type", {}).get("qualType", "uint32_t"))
                    else:
                        self.left.append("the global %s has no // STATIC: tag" % ref.get("name"))
        if function not in self.order:
            self.order.append(function)

    def own(self, function, node):
        """The nodes of the function's code, the runtime's seam calls left out."""
        b, e = begin(node), end(node)
        if node.get("kind") in ("IfStmt", "CallExpr", "ExprWithCleanups") and b is not None and e is not None \
                and "wxl::seam::" in function.source[b:e]:
            return
        yield node
        for child in node.get("inner", []) or []:
            yield from self.own(function, child)

    def call_target(self, function, node):
        inner = node.get("inner", [])
        if not inner:
            return None
        if node["kind"] == "CXXMemberCallExpr":
            member = inner[0]
            if member.get("kind") != "MemberExpr":
                return None
            obj = (member.get("inner") or [{}])[0]
            cls = obj.get("type", {}).get("qualType", "").replace("*", "").replace("const", "").strip()
            cls = re.sub(r"\b(class|struct)\s+", "", cls)
            # A bound member prints no type: the declaration's own is read from the class.
            qual = self.method_type(function, cls, member.get("name"), node)
            target = self.resolve(function, member.get("name"), member.get("name"), qual, cls, True)
            target.member = True
            return target
        callee = inner[0]
        while callee.get("kind") in ("ImplicitCastExpr", "ParenExpr") and callee.get("inner"):
            callee = callee["inner"][0]
        if callee.get("kind") != "DeclRefExpr":
            return None
        ref = callee.get("referencedDecl", {})
        if ref.get("kind") not in ("FunctionDecl", "CXXMethodDecl"):
            return None
        b, e = begin(callee), end(callee)
        text = function.source[b:e] if b is not None and e is not None else ref.get("name")
        # Explicit template arguments follow the name in the source.
        after = function.source[e:e + 200] if e is not None else ""
        explicit = re.match(r"\s*<([^()]*)>\s*\(", after)
        if explicit:
            text += "<%s>" % explicit.group(1)
        qual = ref.get("type", {}).get("qualType", "")
        if "wxl::" in text or (ref.get("name", "").startswith("Listening")):
            return None
        return self.resolve(function, text, ref.get("name"), qual, None, False)

    def method_type(self, caller, cls, name, call):
        """The declared type of the member function a call names: the overload taking as many
        arguments as the call passes."""
        count = len([a for a in call.get("inner", [])[1:]])
        for top in self.nodes(caller.rel, "%s::%s" % (cls, name)):
            for node in walk(top):
                if node.get("kind") != "CXXMethodDecl" or node.get("name") != name:
                    continue
                params = [n for n in node.get("inner", []) if n.get("kind") == "ParmVarDecl"]
                if len(params) == count:
                    return node["type"]["qualType"]
        raise Problem("the declaration of %s::%s (%d argument(s)) was not found" % (cls, name, count))


# --- Writing ------------------------------------------------------------------------------------

class Writer:
    def __init__(self, ex, top, runtime, namespace, resolved, binary=None):
        self.ex = ex
        self.binary = str(binary) if binary else None
        self.top = top
        self.runtime = runtime
        self.namespace = namespace
        self.resolved = resolved      # id(call node) -> address, for template instantiations

    def shim_signature(self, target):
        known = self.ex.known
        ret = engine_type(target.ret, known) or "void*"
        params = []
        for p in target.params:
            t = engine_type(p, known)
            if t is None:
                self.ex.left.append("%s takes a %s, which the engine headers lack" % (target.name, p))
                t = "void*"
            params.append(t)
        return ret, params

    def call_text(self, function, node, target, span):
        """The call rewritten: its callee and, for a member, its object as the first argument. The
        arguments are the source's own text, with what they hold rewritten in turn."""
        src = function.source
        inner = node["inner"]
        callee = inner[0]
        callee_end = end(callee)
        call_end = end(node)
        if callee_end is None or call_end is None:
            return None
        lparen = src.find("(", callee_end)
        if target.kind == "local" and not target.member:
            # A free inlined callee keeps its name: its de-inlined copy is defined under it.
            return None
        if target.kind == "empty":
            if target.name not in self.ex.empties:
                self.ex.empties[target.name] = target
            return None
        args = span(lparen + 1, call_end - 1).strip()
        if target.member:
            member = callee
            obj = (member.get("inner") or [None])[0]
            if obj is None or obj.get("isImplicit") or (obj.get("kind") == "CXXThisExpr"):
                obj_text = "self"
            else:
                obj_text = span(begin(obj), end(obj))
                if member.get("isArrow") is False:
                    obj_text = "&(%s)" % obj_text
        if target.kind == "stock":
            address = target.address or self.resolved.get(node["id"])
            if address is None:
                return "WXL_EXTRACT_UNRESOLVED(\"%s\" /* one of %s */)" % (
                    target.name, ", ".join("0x%08X" % a for a in target.candidates))
            ident = target.ident if target.address else "%s_%08X" % (target.ident, address)
            if not target.address:
                # One address can be several instantiations the linker folded (M2Loop's and
                # M2Material's arrays): each signature gets its own name for that address.
                base, n = ident, 1
                while ident in self.extra_shims and self.extra_shims[ident][0].params != target.params:
                    n += 1
                    ident = "%s_%d" % (base, n)
                self.extra_shims.setdefault(ident, (target, address))
            head = ([obj_text, "nullptr"] if target.conv == "thiscall" else [obj_text]) if target.member else []
            return "stock::%s(%s)" % (ident, ", ".join(head + ([args] if args else [])))
        if target.kind == "local":
            return "%s(%s)" % (target.local.ident, ", ".join([obj_text] + ([args] if args else [])))
        self.ex.left.append("%s is called and has no address, no // INLINED definition and no empty body"
                            % target.name)
        return None

    def render_function(self, function):
        src = function.source
        calls = {node["id"]: target for node, target in function.calls}
        edits = []

        def inside(node, lo, hi):
            b, e = begin(node), end(node)
            return b is not None and e is not None and lo <= b and e <= hi

        def collect(node, out, lo, hi):
            """The outermost rewrites of nodes lying within [lo, hi)."""
            kind = node.get("kind")
            nid = node.get("id")
            here = inside(node, lo, hi)
            if here and nid in calls:
                text = self.call_text(function, node, calls[nid], span)
                if text is not None:
                    out.append((begin(node), end(node), text))
                    return
            if here and kind == "CXXThisExpr":
                # An implicit `this` (m_flags for this->m_flags) spans the member's name: its
                # MemberExpr writes `self->` in front instead.
                if not node.get("isImplicit") and src[begin(node):end(node)] == "this":
                    out.append((begin(node), end(node), "self"))
                return
            if here and kind == "MemberExpr":
                obj = (node.get("inner") or [{}])[0]
                if obj.get("kind") == "CXXThisExpr" and (obj.get("isImplicit") or src[begin(obj):end(obj)] != "this"):
                    out.append((begin(node), begin(node), "self->"))
            if here and kind == "DeclRefExpr":
                ref = node.get("referencedDecl", {})
                if ref.get("kind") == "VarDecl" and ref.get("name") in self.ex.globals and ref.get("id") not in function.local_ids:
                    out.append((begin(node), end(node), "stock::%s" % ref["name"]))
                    return
            for child in node.get("inner", []) or []:
                collect(child, out, lo, hi)

        def span(lo, hi):
            """The source from lo to hi, every rewrite inside it applied."""
            out = []
            collect(body_node[0], out, lo, hi)
            out.sort(key=lambda x: (x[0], -x[1]))
            text, at = [], lo
            for s_, t, rep in out:
                if s_ < at:
                    continue
                text.append(src[at:s_])
                text.append(rep)
                at = t
            text.append(src[at:hi])
            return "".join(text)

        body = [c for c in function.node.get("inner", []) if c.get("kind") == "CompoundStmt"][0]
        body_node = [body]
        text = span(begin(body), end(body))
        return clean(text, function)

    def signature(self, function, name, point=None):
        known = self.ex.known
        params = []
        if function.member:
            params.append("%s* self" % function.cls)
            params.append("void* /* edx */")
        for t, n in zip(function.params, function.param_names):
            mapped = engine_type(t, known)
            if mapped is None:
                self.ex.left.append("%s takes a %s, which the engine headers lack" % (function.name, t))
                mapped = "void*"
            params.append(self.declare(mapped, n))
        ret = engine_type(function.ret, known) or "void"
        conv = "__fastcall" if function.member else "__cdecl"
        return "%s %s %s(%s)" % (ret, conv, name, ", ".join(params) or "void")

    @staticmethod
    def declare(type_text, name):
        m = re.match(r"^(.*?)(\[\d+\])$", type_text)
        if m:
            return "%s %s%s" % (m.group(1), name, m.group(2))
        return "%s %s" % (type_text, name)

    def write(self, command):
        ex, top = self.ex, self.top
        self.extra_shims = {}
        bodies = []
        for function in ex.order:
            text = self.render_function(function)
            text = self.stock_file_names(text)
            point = self.point_of(function) if self.runtime else None
            if function is top:
                name = top.ident
                doc = "// %s, Wow.exe 12340 0x%08X (%s)" % (
                    top.name, top.address or 0, "thiscall" if top.member else "cdecl")
            else:
                name = function.ident if function.cls else function.name.split("::")[-1]
                doc = "// %s, inlined in Wow.exe: de-inlined here (%s)" % (function.name, function.rel)
            if point:
                body_name = name + "_body"
                bodies.append("%s\n// The body; the function below enters the point %s around it.\n"
                              "static __declspec(noinline) %s\n%s\n" % (doc, point, self.signature(function, body_name), text))
                plain = ", ".join(function.param_names)
                contract = ", ".join(("&" if "&" in t else "") + n for t, n in zip(function.params, function.param_names))
                bodies.append("// %s on x86: the point's byte tested, and while it has handlers they run around\n"
                              "// the body (a reference passed as its address, as the point's contract has it).\n"
                              "static __declspec(noinline) %s\n{\n"
                              "    if (!Armed(points::k%s)) return %s(%s);\n"
                              "    return Enter<points::k%s>(&%s, %s);\n}\n"
                              % (function.name, self.signature(function, name), point, body_name, plain,
                                 point, body_name, contract))
            else:
                bodies.append("%s\n%s%s\n%s\n" % (doc, "" if function is top else "static __declspec(noinline) ",
                                                   self.signature(function, name), text))
        every = {ident: (target, target.address) for ident, target in ex.stock.items() if target.address is not None}
        every.update(self.extra_shims)
        shims = [self.shim(ident, target, address) for ident, (target, address) in sorted(every.items())]
        empties = []
        for name, target in sorted(ex.empties.items()):
            ret, params = self.shim_signature(target)
            empties.append("    // %s: empty in the release client (Wow.exe's calls go to a bare `ret`)." % name)
            empties.append("    static inline %s %s(%s) {}" % (ret, name, ", ".join(params + (["..."] if target.variadic else []))))
        globals_ = []
        for name, (address, qual) in sorted(ex.globals.items()):
            t = engine_type(qual, ex.known) or "uint32_t"
            globals_.append("        inline %s& %s = *reinterpret_cast<%s*>(0x%08X);" % (t, name, t, address))
        out = []
        if self.runtime:
            out += ["// SPDX-License-Identifier: MIT", "// Copyright (c) 2026 WarcraftXL", "//"]
        out.append("// GENERATED by core/tools/extract/extract.py %s" % command)
        out.append("// from the client fork: %s, %s." % (top.name, top.rel))
        if self.runtime:
            out.append("// Do not edit: change the fork or the tool and run it again (the fork's build runs --check).")
        else:
            out.append("// A starting point for x86 (the stock Wow.exe 12340): build it with the SDK's engine headers.")
        out.append("//")
        out.append("// Calls go to Wow.exe's own functions at their addresses (namespace stock), globals are")
        out.append("// Wow.exe's own, and what the 2008 compiler inlined is a function of its own here.")
        left = sorted(set(ex.left))
        if left:
            out.append("//")
            out.append("// Left to the modder:")
            out += ["//   - %s" % l for l in left]
        out.append("")
        if self.runtime:
            out.append('#include "deinlined/Deinlined.hpp"')
        out.append('#include "wxl/engine/engine.hpp"')
        out.append("")
        out.append("#if !WXL_ENGINE_X86")
        out.append("#error \"this is the stock client's code: build it for x86\"")
        out.append("#endif")
        out.append("")
        out.append("namespace %s" % self.namespace)
        out.append("{")
        out.append("    using namespace ::wxl::engine;")
        out.append("")
        out.append("    // Wow.exe 12340's functions and globals this code uses, at their addresses.")
        out.append("    namespace stock")
        out.append("    {")
        out += shims + globals_
        out.append("    }")
        out.append("")
        if empties:
            out += empties
            out.append("")
        for b in bodies:
            out += ["    " + l if l.strip() else "" for l in b.rstrip("\n").split("\n")]
            out.append("")
        out.append("}")
        if self.runtime:
            out.append("")
            out.append("WXL_DEINLINED_BODY(%s)" % top.ident)
        return "\n".join(out) + "\n"

    def shim(self, ident, target, address):
        """Wow.exe's function at its address, under a name: a pointer of the stock convention."""
        ret, params = self.shim_signature(target)
        if target.member and target.conv == "thiscall":
            # `this` in ECX and the callee pops: a __fastcall with a dummy EDX.
            cls = target.name.rsplit("::", 1)[0]
            cls_t = cls if cls in self.ex.known else "void"
            params = ["%s*" % cls_t, "void*"] + params
            conv = "__fastcall"
        elif target.member:
            cls = target.name.rsplit("::", 1)[0]
            params = ["%s*" % (cls if cls in self.ex.known else "void")] + params
            conv = "__" + target.conv
        else:
            conv = "__" + target.conv
        folded = self.ex.rows.folded.get(address, [])
        note = target.name + (" (folded with %d other(s))" % (len(folded) - 1) if len(folded) > 1 else "")
        pointer = "%s(%s*)(%s%s)" % (ret, conv, ", ".join(params),
                                     (", ..." if params else "...") if target.variadic else "")
        return "        inline const auto %s = reinterpret_cast<%s>(0x%08X); // %s" % (ident, pointer, address, note)

    def stock_file_names(self, text):
        """In the runtime's copy, a source file name Storm is handed is Wow.exe's own string, at
        its address: the blocks the body allocates are then the stock client's to the byte."""
        if not self.runtime or not self.binary:
            return text

        def swap(match):
            literal = match.group(1).encode("latin-1").decode("unicode_escape").encode("latin-1") + b"\0"
            address = string_address(self.binary, literal)
            if address is None:
                return match.group(0)
            return 'reinterpret_cast<const char*>(0x%08X) /* "%s" */' % (address, match.group(1))
        return re.sub(r'"(\.\\\\[A-Za-z0-9_]+\.cpp)"', swap, text)

    def point_of(self, function):
        if not self.ex.points:
            return None
        for p in self.ex.points:
            if not p.parents or p.base_name != function.name:
                continue
            if p.overload is not None and p.overload != [normal(t) for t in function.params]:
                continue
            return p.ident
        return None


_SECTIONS = {}


def string_address(binary, literal):
    """The address of a zero-ended string in Wow.exe's read-only data, or None."""
    if binary not in _SECTIONS:
        sys.path.insert(0, str(HERE.parent / "points"))
        from generate_points import read_pe
        _SECTIONS[binary] = read_pe(binary)
    for name, va, body in _SECTIONS[binary]:
        if name != ".rdata":
            continue
        at = body.find(literal)
        if at >= 0 and (at == 0 or body[at - 1] == 0):
            return va + at
    return None


def clean(text, function):
    """The body without the runtime's own lines, with __FILE__ as the stock client spells it."""
    lines = text.split("\n")
    out, skip = [], 0
    i = 0
    while i < len(lines):
        line = lines[i]
        if "WXL_POINT_" in line and "// wxl.point" in line:
            i += 1
            continue
        if "wxl::seam::" in line:
            stripped = line.strip()
            if stripped.startswith("if") and stripped.endswith("{"):
                depth = line.count("{") - line.count("}")
                i += 1
                while i < len(lines) and depth > 0:
                    depth += lines[i].count("{") - lines[i].count("}")
                    i += 1
                if out and not out[-1].strip() and i < len(lines) and not lines[i].strip():
                    i += 1
                continue
            i += 1
            continue
        out.append(line)
        i += 1
    text = "\n".join(out)
    text = re.sub(r"^\{\n[ \t]*\n", "{\n", text)
    base = Path(function.rel).name
    text = text.replace("__FILE__", '".\\\\%s"' % base)
    # Storm keys a block by its file; the line is the stock client's own and not known here.
    text = text.replace("__LINE__", "0")
    return text


# --- The stock function's call order -----------------------------------------------------------------

def binary_calls(binary, address, length):
    objdump = shutil.which("llvm-objdump") or shutil.which("llvm-objdump-19")
    if not objdump:
        raise Problem("--binary needs llvm-objdump")
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", "--start-address=0x%X" % address,
                          "--stop-address=0x%X" % (address + length), str(binary)], capture_output=True, text=True).stdout
    calls = []
    for m in re.finditer(r"^\s*([0-9a-f]+):\s*(calll?|jmpl?)\s+0x([0-9a-f]+)", out, re.M):
        calls.append(int(m.group(3), 16))
    return calls


def resolve_templates(ex, top, binary):
    """Pairs each call of an ambiguous instantiation with the stock function's calls, in order."""
    events = []

    def visit(function, seen):
        for node, target in function.calls:
            if target.kind == "local" and target.local not in seen:
                visit(target.local, seen | {target.local})
            elif target.kind == "stock":
                events.append((node, target))

    visit(top, {top})
    ambiguous = [(n, t) for n, t in events if t.address is None and t.candidates]
    if not ambiguous:
        return {}, []
    if not binary or top.address is None:
        return {}, ["%s: one of %s; give --binary to choose by the stock function's calls"
                    % (t.name, ", ".join("0x%08X" % a for a in t.candidates)) for _, t in ambiguous]
    rows = ex.rows.functions.get(top.name, [])
    length = next((r[1] for r in rows if r[0] == top.address), 0) or 0x1000
    union = set(a for _, t in ambiguous for a in t.candidates)
    stock = [a for a in binary_calls(binary, top.address, length) if a in union]
    resolved, at, problems = {}, 0, []
    for node, target in ambiguous:
        while at < len(stock) and stock[at] not in target.candidates:
            at += 1
        if at == len(stock):
            problems.append("%s: no call left in 0x%08X among %s" % (target.name, top.address,
                                                                     ", ".join("0x%08X" % a for a in target.candidates)))
            continue
        resolved[node["id"]] = stock[at]
        at += 1
    if at != len(stock) and not problems:
        problems.append("0x%08X calls %d more instantiation(s) than the source: the orders differ"
                        % (top.address, len(stock) - at))
    return resolved, problems


# --- Main ----------------------------------------------------------------------------------------

def locate(target, rows, client):
    """(qualified name, overload, file, line, address) of what the command line names."""
    if re.match(r"^(0x)?[0-9A-Fa-f]{6,8}$", target):
        address = int(target, 16)
        hits = [(name, r) for name, rs in rows.functions.items() for r in rs if r[0] == address]
        if not hits:
            raise Problem("no function of the fork is tagged 0x%08X" % address)
        if len({h[0] for h in hits}) > 1:
            raise Problem("0x%08X is %s: the linker folded them; name the one to extract"
                          % (address, ", ".join(sorted({h[0] for h in hits}))))
        name, row = hits[0]
        return re.sub(r"<.*>$", "", name), None, row[2], row[3], address
    base, overload = table.split_name(target)
    found = rows.functions.get(base, [])
    if len(found) == 1 and overload is None:
        r = found[0]
        return base, None, r[2], r[3], r[0]
    if len(found) > 1 and overload is None:
        raise Problem("%s names %d functions (%s): give the address, or the parameter types as "
                      "%s(type,type)" % (base, len(found), ", ".join("0x%08X" % r[0] for r in found), base))
    for r in found:
        text = table.cxx.strip_cxx((client / r[2]).read_text(encoding="utf-8", errors="replace"))
        if table.definition_params(text, base, max(1, r[3] - 2)) == overload:
            return base, overload, r[2], r[3], r[0]
    hits = table.find_inlined(client, base, overload)
    if len(hits) == 1:
        return base, overload, str(hits[0][0]), hits[0][1], None
    raise Problem("%s is neither tagged with an address nor a single // INLINED definition of the fork" % target)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("function", nargs="?", help="a qualified name (CM2Shared::FinishLoading), an overload "
                                                    "(M2Init(uint8_t*,uint32_t,M2Data&)) or an x86 address (0x0083CF00)")
    parser.add_argument("--deinlined", action="store_true",
                        help="the runtime's recompiled body of every parent of a `deinlined` line of points.txt, "
                             "into core/src/deinlined/<Parent>.gen.cpp (with --check: fail when one differs)")
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, default=CORE.parent / "sdk")
    parser.add_argument("--offsets", type=Path, default=CORE / "include" / "wxl" / "offsets" / "generated")
    parser.add_argument("--list", type=Path, default=CORE / "include" / "wxl" / "points" / "points.txt")
    parser.add_argument("--binary", type=Path, default=os.environ.get("WXL_WOW_EXE"))
    parser.add_argument("--out", type=Path)
    parser.add_argument("--namespace", default="extracted")
    parser.add_argument("--runtime", action="store_true", help="core's de-inlined body (core/src/deinlined/)")
    parser.add_argument("--check", action="store_true", help="with --out: fail when the file differs")
    parser.add_argument("--clang-cl", default=default_clang_cl())
    parser.add_argument("--xwin", default="/opt/xwin" if os.path.isdir("/opt/xwin") else None)
    args = parser.parse_args(argv)
    if not args.clang_cl:
        print("extract: no clang-cl; name one with --clang-cl", file=sys.stderr)
        return 2
    if args.deinlined:
        points, problems = table.load(args.list.resolve(), args.client.resolve(), args.offsets.resolve())
        if problems:
            print("extract: points.txt: %s" % "; ".join(problems), file=sys.stderr)
            return 1
        parents = list({q.name: q for p in points for q in p.parents}.values())
        status = 0
        for parent in parents:
            out = CORE / "src" / "deinlined" / ("%s.gen.cpp" % parent.ident)
            forward = [parent.name, "--client", str(args.client), "--sdk", str(args.sdk), "--offsets", str(args.offsets),
                       "--list", str(args.list), "--runtime", "--out", str(out), "--clang-cl", args.clang_cl]
            if args.binary:
                forward += ["--binary", str(args.binary)]
            if args.xwin:
                forward += ["--xwin", args.xwin]
            if args.check:
                forward.append("--check")
            status = main(forward) or status
        return status
    if not args.function:
        parser.error("name a function, or give --deinlined")
    client = args.client.resolve()
    try:
        rows = Rows(args.offsets.resolve())
        points = None
        if args.runtime:
            points, problems = table.load(args.list.resolve(), client, args.offsets.resolve())
            if problems:
                raise Problem("points.txt: " + "; ".join(problems))
        name, overload, rel, line, address = locate(args.function, rows, client)
        ex = Extractor(client, args.sdk.resolve(), rows, args.clang_cl, args.xwin, points)
        top = ex.definition(name, Path(rel), line, address)
        if overload is not None:
            top.overload_ident = table.ident_of(args.function)
        ex.scan(top)
        resolved, problems = resolve_templates(ex, top, args.binary)
        ex.left += problems
        namespace = "wxl::core::deinlined" if args.runtime else args.namespace
        shown = " ".join([args.function] + (["--runtime"] if args.runtime else []))
        text = Writer(ex, top, args.runtime, namespace, resolved, args.binary).write(shown)
    except (Problem, table.Problem) as trouble:
        print("extract: %s" % trouble, file=sys.stderr)
        return 1
    if args.runtime and ex.left:
        print("extract: the runtime's copy must be complete; left: %s" % "; ".join(sorted(set(ex.left))), file=sys.stderr)
        return 1
    if not args.out:
        sys.stdout.write(text)
        return 0
    old = args.out.read_text(encoding="utf-8") if args.out.is_file() else ""
    if args.check:
        if old != text:
            sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), text.splitlines(True), str(args.out), "extracted"))
            print("extract --check: %s is not what the fork gives: run extract.py again" % args.out, file=sys.stderr)
            return 1
        print("extract --check: %s is current" % args.out)
        return 0
    if old != text:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8", newline="\n")
    print("extract: %s -> %s (%d function(s), %d stock call(s), %d global(s))"
          % (top.name, args.out, len(ex.order), len(ex.stock), len(ex.globals)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
