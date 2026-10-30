#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 WarcraftXL
#
# Writes, or checks, the WXL_POINT_<n>(...) line at the head of every point's function in the fork.
"""Write or check the recompiled client's point lines.

A point on the recompiled client is one line, the first statement of its function:

    int32_t NetClient::ProcessMessage(uint32_t timeReceived, CDataStore* msg, int32_t unused_a4) {
        WXL_POINT_4(this, timeReceived, msg, unused_a4); // wxl.point

and the include of WarcraftXL/Points.h in its file. The lines are committed, so they are visible and
greppable; a merge from upstream that drops one, moves one or renames a parameter is caught by
--check, which the fork's build runs (wxl-point-check), and repaired by --write.

--check fails on: a point's line missing, not first in its function, or not what the table
gives; a line twice; a WXL_POINT_<n> line for no point; a file without the include.
"""

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import points_table as table  # noqa: E402

CORE = HERE.parent.parent
INCLUDE = '#include "WarcraftXL/Points.h"'
LINE = re.compile(r"^[ \t]*WXL_POINT_(\d+)\s*\(.*$", re.M)
TAG = "// wxl.point"


def source_of(client, point):
    """The file the x64 build compiles for a point: its Win64 twin for a Win32 file."""
    parts = list(point.file.parts)
    if "Win32" in parts:
        twin = Path(*[("Win64" if p == "Win32" else p) for p in parts])
        if (client / twin).is_file():
            return twin
    return point.file


def expected_line(point, indent):
    args = (["this"] if point.member else []) + [p.name for p in point.params]
    return "%sWXL_POINT_%d(%s); %s" % (indent, point.index, ", ".join(args), TAG)


def body_line(text, point):
    """(index of the line holding the body's '{', the indentation of the body) in `text`."""
    stripped = table.cxx.strip_cxx(text)
    # An overload is told from its namesakes by where the fork defines it.
    start = max(1, point.line - 2) if point.overload is not None else 1
    found = table.definition_at(stripped, point.base_name, start)
    if not found:
        return None, None
    brace = found[1]
    index = text.count("\n", 0, brace)
    lines = text.split("\n")
    indent = "    "
    for line in lines[index + 1:index + 6]:
        if line.strip():
            indent = line[:len(line) - len(line.lstrip())] or indent
            break
    return index, indent


def check(points, client):
    problems = []
    by_file = {}
    for p in points:
        if p.x64:
            by_file.setdefault(source_of(client, p), []).append(p)
    wanted = {p.index for p in points if p.x64}
    seen = {}
    for path in sorted(client.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp") or not path.is_file():
            continue
        rel = path.relative_to(client)
        if rel.parts[0] in ("ThirdParty", "build", "bin", "out") or rel.as_posix().startswith("WarcraftXL/"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "WXL_POINT_" not in text:
            continue
        for m in LINE.finditer(text):
            n = int(m.group(1))
            where = "%s:%d" % (rel.as_posix(), text.count("\n", 0, m.start()) + 1)
            if n not in wanted:
                problems.append("%s: WXL_POINT_%d is no point of points.txt on x64" % (where, n))
            seen.setdefault(n, []).append(where)
    for path, ps in by_file.items():
        full = client / path
        if not full.is_file():
            problems.append("%s: the file of %s is missing" % (path.as_posix(), ", ".join(p.name for p in ps)))
            continue
        text = full.read_text(encoding="utf-8", errors="replace")
        if INCLUDE not in text:
            problems.append("%s: carries %s and does not %s" % (path.as_posix(), ", ".join(p.name for p in ps), INCLUDE))
        lines = text.split("\n")
        for p in ps:
            index, indent = body_line(text, p)
            if index is None:
                problems.append("%s: the definition of %s is not found" % (path.as_posix(), p.name))
                continue
            first = next((l for l in lines[index + 1:] if l.strip()), "")
            want = expected_line(p, indent)
            where = "%s:%d" % (path.as_posix(), index + 2)
            if first.rstrip() != want:
                if "WXL_POINT_%d(" % p.index in first:
                    problems.append("%s: %s's line is `%s`, the table gives `%s`" % (where, p.name, first.strip(), want.strip()))
                else:
                    problems.append("%s: %s does not start with its point line `%s`" % (where, p.name, want.strip()))
        for p in ps:
            if len(seen.get(p.index, [])) > 1:
                problems.append("WXL_POINT_%d (%s) is written %d times: %s"
                                % (p.index, p.name, len(seen[p.index]), ", ".join(seen[p.index])))
    return problems


def write(points, client):
    by_file = {}
    for p in points:
        if p.x64:
            by_file.setdefault(source_of(client, p), []).append(p)
    changed = []
    # Stray lines of points no longer listed go first, wherever they are.
    wanted_files = set(by_file)
    for path in sorted(client.rglob("*.cpp")):
        rel = path.relative_to(client)
        if rel in wanted_files or rel.parts[0] in ("ThirdParty", "build", "bin", "out"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if TAG not in text:
            continue
        kept = [l for l in text.split("\n") if not (LINE.match(l) and l.rstrip().endswith(TAG))]
        path.write_text("\n".join(kept), encoding="utf-8", newline="")
        changed.append(rel)
    for path, ps in by_file.items():
        full = client / path
        original = full.read_text(encoding="utf-8", errors="replace")
        newline = "\r\n" if "\r\n" in original else "\n"
        text = original.replace("\r\n", "\n")
        lines = [l for l in text.split("\n") if not (LINE.match(l) and l.rstrip().endswith(TAG))]
        text = "\n".join(lines)
        for p in sorted(ps, key=lambda q: -body_line(text, q)[0] if body_line(text, q)[0] is not None else 0):
            index, indent = body_line(text, p)
            if index is None:
                raise SystemExit("insert_points: the definition of %s is not found in %s" % (p.name, path))
            lines = text.split("\n")
            lines.insert(index + 1, expected_line(p, indent))
            text = "\n".join(lines)
        if INCLUDE not in text:
            lines = text.split("\n")
            last = max((i for i, l in enumerate(lines[:200]) if l.startswith("#include")), default=-1)
            lines.insert(last + 1, INCLUDE)
            text = "\n".join(lines)
        text = text.replace("\n", newline)
        if text != original:
            full.write_text(text, encoding="utf-8", newline="")
            changed.append(path)
    return changed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", type=Path, default=CORE / "include" / "wxl" / "points" / "points.txt")
    parser.add_argument("--client", type=Path, required=True, help="the client fork's checkout")
    parser.add_argument("--offsets", type=Path, default=CORE / "include" / "wxl" / "offsets" / "generated")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="fail when a line is missing or wrong")
    mode.add_argument("--write", action="store_true", help="write the lines and the includes")
    args = parser.parse_args(argv)

    client = args.client.resolve()
    try:
        points, problems = table.load(args.list.resolve(), client, args.offsets.resolve())
    except table.Problem as trouble:
        print("insert_points: %s" % trouble, file=sys.stderr)
        return 2
    if problems:
        print("insert_points: %d problem(s) in the point list" % len(problems), file=sys.stderr)
        for problem in problems:
            print("  " + problem, file=sys.stderr)
        return 1

    if args.write:
        for path in write(points, client):
            print("insert_points: wrote %s" % path.as_posix())
    problems = check(points, client)
    if problems:
        print("insert_points: %d problem(s) with the fork's point lines%s" % (
            len(problems), "" if args.write else "; core/tools/points/insert_points.py --write repairs them"),
            file=sys.stderr)
        for problem in problems:
            print("  " + problem, file=sys.stderr)
        return 1
    print("insert_points: %d point line(s) in place" % sum(p.x64 for p in points))
    return 0


if __name__ == "__main__":
    sys.exit(main())
