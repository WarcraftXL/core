#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 WarcraftXL
#
# Derive core's address tables from the OFFSET:/STATIC: tags of the client fork.
"""
Scan a `client` checkout (the C++ decompilation of Wow.exe 3.3.5a build 12340) for the
comment tags that give every reconstructed function and static its address in the original
binary, and write them out as X-macro tables for `core`.

    // VERIFY: 271a03c4 manual
    // OFFSET: 0x0068FA60
    void CGxDeviceD3d::IBufUnlock(CGxBuf* buf) {

    // STATIC: 0x00C60930
    static BLIT_FUNCTION s_blits[BlitFormats_Last][BlitFormats_Last][BlitAlphas_Last];

Outputs (in the output directory):
    functions.def   one row per function address, with its extent (bound and length)
    statics.def     one row per static address
    unparsed.txt    every OFFSET:/STATIC: tag the tool could not confidently name
    summary.txt     counts, coverage and anomalies

Invariant: every OFFSET:/STATIC: tag ends up either as rows or as an unparsed entry, and the
tool refuses to write anything if the totals do not reconcile.

Function extents come from the original image (Wow.exe): its PE section headers bucket the
addresses, so a function's `bound` is the distance to the next tagged address in the same
section (an upper bound: untagged code may lie in between), and a linear sweep of its bytes
(x86_sweep.py) gives its `length` where the int3 padding between functions makes the end
unambiguous.

Python 3, standard library only.
"""

import argparse
import collections
import os
import re
import struct
import sys

sys.dont_write_bytecode = True      # no __pycache__ next to the tool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import x86_sweep  # noqa: E402

SOURCE_EXTENSIONS = ('.cpp', '.c', '.h', '.hpp', '.inl', '.cc', '.cxx')
HEADER_EXTENSIONS = ('.h', '.hpp', '.inl')
SKIP_DIRS = {'.git', '.svn', '.hg'}

TAG_RE = re.compile(r'^\s*//\s*(OFFSET|STATIC|VERIFY|INLINED|NOTE):\s*(.*?)\s*$')
HEX_RE = re.compile(r'^0x[0-9A-Fa-f]{1,8}$')
VERIFY_RE = re.compile(r'^([0-9A-Fa-f]{8})\s+(auto|manual)$')

# Lines after the tag that may precede the declaration proper.
TEMPLATE_PREFIX_RE = re.compile(r'^\s*template\s*<')
ACCESS_LABEL_RE = re.compile(r'^\s*(?:public|private|protected)\s*:')

# A name whose last segment is one of these is not a function or variable name.
NOT_A_NAME = {
    'if', 'for', 'while', 'switch', 'return', 'sizeof', 'catch', 'defined', 'new', 'delete',
    'throw', 'typeid', 'alignof', 'decltype', 'static_assert', 'else', 'do', 'case', 'goto',
    '__try', '__except', '__finally', '__leave', 'try', 'using', 'typedef',
    'void', 'int', 'char', 'short', 'long', 'float', 'double', 'bool', 'unsigned', 'signed',
    'const', 'volatile', 'static', 'extern', 'inline', 'register', 'struct', 'class', 'union',
    'enum', 'auto', 'template', 'typename', 'operator', 'namespace', 'virtual', 'explicit',
    'friend', 'mutable', 'constexpr', 'wchar_t', 'size_t', '__int64', '__declspec',
    '__cdecl', '__stdcall', '__fastcall', '__thiscall', '__forceinline', '__inline',
}
CONTROL_HEAD = ('if', 'for', 'while', 'switch', 'else', 'do', 'try', 'catch', 'return',
                '__try', '__except', '__finally', 'case', 'default')

# What may follow a function's parameter list before its body or the end of the declaration.
PARAM_TAIL_RE = re.compile(
    r'^\s*(?:(?:const|volatile|noexcept|override|final|&&|&'
    r'|throw\s*\([^)]*\)|__attribute__\s*\(\(.*?\)\)|=\s*0|=\s*default|=\s*delete)\s*)*'
    r'(?::(?!:).*)?$', re.S)


def fail(message):
    sys.stderr.write('extract_offsets: ' + message + '\n')
    sys.exit(2)


# ----------------------------------------------------------------------------------------------
# Masking: a same-length view of the file where comments, string and character literals and
# preprocessor lines are blanked, so brace and parenthesis scanning sees code only.
# ----------------------------------------------------------------------------------------------

def mask_source(text):
    out = []
    i = 0
    n = len(text)
    at_line_start = True
    while i < n:
        c = text[i]
        if at_line_start:
            j = i
            while j < n and text[j] in ' \t':
                j += 1
            if j < n and text[j] == '#':
                # Preprocessor line, with its continuation lines.
                k = j
                while k < n:
                    if text[k] == '\n':
                        if k > 0 and text[k - 1] == '\\':
                            k += 1
                            continue
                        break
                    k += 1
                out.append(blank(text[i:k]))
                i = k
                continue
            at_line_start = False
        if c == '\n':
            out.append('\n')
            i += 1
            at_line_start = True
        elif text.startswith('//', i):
            k = text.find('\n', i)
            if k < 0:
                k = n
            out.append(' ' * (k - i))
            i = k
        elif text.startswith('/*', i):
            k = text.find('*/', i + 2)
            k = n if k < 0 else k + 2
            out.append(blank(text[i:k]))
            i = k
        elif c == '"' or c == "'":
            # A literal never spans a line: an unterminated one (an apostrophe in prose
            # inside a macro argument, say) ends at the newline, which stays.
            k = i + 1
            while k < n and text[k] != c and text[k] != '\n':
                if text[k] == '\\' and k + 1 < n and text[k + 1] != '\n':
                    k += 1
                k += 1
            if k < n and text[k] == c:
                k += 1
                out.append(c + ' ' * (k - i - 2) + c)
            else:
                out.append(c + ' ' * (k - i - 1))
            i = k
        else:
            out.append(c)
            i += 1
    masked = ''.join(out)
    if len(masked) != n or masked.count('\n') != text.count('\n'):
        fail('internal: the masked view does not line up with the source')
    return masked


def blank(s):
    return ''.join('\n' if ch == '\n' else ' ' for ch in s)


# ----------------------------------------------------------------------------------------------
# Name extraction helpers.
# ----------------------------------------------------------------------------------------------

def top_level_groups(text, open_ch='(', close_ch=')'):
    """Spans (start, end) of the top-level open_ch...close_ch groups of text; end is inclusive."""
    groups = []
    depth = 0
    start = -1
    for i, ch in enumerate(text):
        if ch == open_ch:
            if depth == 0:
                start = i
            depth += 1
        elif ch == close_ch:
            if depth > 0:
                depth -= 1
                if depth == 0:
                    groups.append((start, i))
    return groups


def find_top_level(text, chars):
    """Index of the first character of `chars` outside (), [] and {}; -1 if none."""
    depth = 0
    for i, ch in enumerate(text):
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        elif depth == 0 and ch in chars:
            return i
    return -1


def strip_trailing_brackets(text):
    """Remove trailing [...] groups (array declarators)."""
    text = text.rstrip()
    while text.endswith(']'):
        depth = 0
        i = len(text) - 1
        while i >= 0:
            if text[i] == ']':
                depth += 1
            elif text[i] == '[':
                depth -= 1
                if depth == 0:
                    break
            i -= 1
        if i < 0:
            return None
        text = text[:i].rstrip()
    return text


def qualified_suffix(text):
    """
    The longest suffix of text that is a qualified identifier: segments `~?ident<...>?` joined by
    `::`. Returns (name, rest) or (None, text). The destructor tilde is only accepted on the last
    segment.
    """
    text = text.rstrip()
    i = len(text)
    first = True
    while True:
        j = i
        if j > 0 and text[j - 1] == '>':
            depth = 0
            k = j - 1
            while k >= 0:
                if text[k] == '>':
                    depth += 1
                elif text[k] == '<':
                    depth -= 1
                    if depth == 0:
                        break
                k -= 1
            if k < 0:
                return None, text
            j = k
            while j > 0 and text[j - 1] in ' \t':
                j -= 1
        ident_end = j
        while j > 0 and (text[j - 1].isalnum() or text[j - 1] == '_'):
            j -= 1
        if j == ident_end or text[j].isdigit():
            return None, text
        if first and j > 0 and text[j - 1] == '~':
            j -= 1
        first = False
        i = j
        if i >= 2 and text[i - 2:i] == '::':
            i -= 2
            continue
        break
    name = text[i:]
    if i > 0 and text[i - 1] in '.>':  # member access, not a definition
        return None, text
    return name, text[:i]


def last_segment(name):
    seg = name.rsplit('::', 1)[-1]
    seg = seg.split('<', 1)[0]
    return seg.lstrip('~')


def normalise_ws(text):
    return re.sub(r'\s+', ' ', text).strip()


def parse_function_signature(sig):
    """
    sig is the declaration text before its `{` or `;`, with templates and comments removed.
    Returns (name, None) or (None, reason).
    """
    groups = top_level_groups(sig)
    if not groups:
        return None, 'no parameter list'
    params = None
    for start, end in groups:
        if PARAM_TAIL_RE.match(sig[end + 1:]):
            params = (start, end)
            break
    if params is None:
        return None, 'no parameter list followed by a body'
    pretext = sig[:params[0]].rstrip()

    # Operators, first because `operator()` ends with a parenthesis of its own.
    m = None
    for m in re.finditer(r'\boperator\b', pretext):
        pass
    if m is None and pretext.endswith(')'):
        # Parenthesised name: LUALIB_API int (luaL_foo) (lua_State *L)
        inner = top_level_groups(pretext)
        if inner and inner[-1][1] == len(pretext) - 1:
            content = pretext[inner[-1][0] + 1:inner[-1][1]].strip()
            name, rest = qualified_suffix(content)
            if name is not None and rest.strip() == '':
                return name, None
        return None, 'name is not an identifier'
    if m is not None:
        tail = pretext[m.end():]
        qualifier_text = pretext[:m.start()].rstrip()
        if qualifier_text.endswith('::'):
            qualifier, _ = qualified_suffix(qualifier_text[:-2])
            if qualifier is None:
                return None, 'operator qualifier'
            qualifier += '::'
        else:
            qualifier = ''
        tail_ws = normalise_ws(tail)
        if tail_ws == '':
            return None, 'operator without a symbol'
        if re.match(r'^[A-Za-z_]', tail_ws):
            # Conversion operator or new/delete: keep single spaces, tidy `new []`.
            tail_ws = re.sub(r'\s*\[\s*\]', '[]', tail_ws)
            tail_ws = re.sub(r'\s*([*&])\s*', r'\1', tail_ws)
            return qualifier + 'operator ' + tail_ws, None
        return qualifier + 'operator' + re.sub(r'\s+', '', tail_ws), None

    name, rest = qualified_suffix(pretext)
    if name is None:
        return None, 'name is not an identifier'
    if last_segment(name) in NOT_A_NAME:
        return None, 'name is a keyword'
    if rest.rstrip().endswith('::'):
        return None, 'dangling scope operator'
    return name, None


def parse_static_declaration(decl):
    """decl is the text before the terminating `;`, `=` or `{`. Returns (name, None) or (None, reason)."""
    text = decl.rstrip()
    if text.endswith(')'):
        groups = top_level_groups(text)
        if not groups or groups[-1][1] != len(text) - 1:
            return None, 'unbalanced parentheses'
        last = groups[-1]
        pre = text[:last[0]].rstrip()
        if pre.endswith(')'):
            # Function pointer: static R (*s_name[N])(args)
            inner_groups = top_level_groups(pre)
            if not inner_groups or inner_groups[-1][1] != len(pre) - 1:
                return None, 'unbalanced parentheses'
            content = pre[inner_groups[-1][0] + 1:inner_groups[-1][1]]
            # Drop the calling convention (keyword or macro such as WINAPI), a pointer-to-member's
            # `Class::`, and the stars.
            content = re.sub(r'^\s*(?:[A-Za-z_]\w*\s*(?:::\s*)?)*\*+\s*', '', content)
            content = strip_trailing_brackets(content)
            if content is None:
                return None, 'array declarator'
            name, rest = qualified_suffix(content)
            if name is None or rest.strip() != '':
                return None, 'function pointer name'
            return name, None
        # Constructor-initialised static: static T s_name(args)
        text = pre
    text = strip_trailing_brackets(text)
    if text is None:
        return None, 'array declarator'
    name, rest = qualified_suffix(text)
    if name is None:
        return None, 'name is not an identifier'
    if last_segment(name) in NOT_A_NAME:
        return None, 'name is a keyword'
    if rest.strip() == '' and '::' not in name:
        return None, 'no type before the name'
    return name, None


# ----------------------------------------------------------------------------------------------
# Scope tracking: which namespaces, classes and functions enclose a given position.
# ----------------------------------------------------------------------------------------------

NAMESPACE_HEAD_RE = re.compile(r'^(?:inline\s+)?namespace(?:\s+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*))?$')
CLASS_HEAD_RE = re.compile(
    r'^(?:template\s*<.*>\s*)?(?:class|struct|union)\s+'
    r'(?:__declspec\s*\([^)]*\)\s+|[A-Z_][A-Z0-9_]*\s+)*'
    r'([A-Za-z_]\w*(?:<[^{}]*>)?)\s*(?:final\s*)?(?::(?!:)[^{}]*)?$', re.S)
ANON_AGGREGATE_RE = re.compile(r'^(?:typedef\s+)?(?:class|struct|union)\s*$')
EXTERN_C_RE = re.compile(r'^extern\s*""\s*$')


class Scope:
    __slots__ = ('kind', 'name')

    def __init__(self, kind, name):
        self.kind = kind    # 'namespace' | 'class' | 'function' | 'other'
        self.name = name    # qualified name for namespace/class/function, else None


def scope_prefix(stack):
    """The qualification a declaration at this point receives from its enclosing scopes."""
    for scope in reversed(stack):
        if scope.kind == 'function':
            return scope.name
    parts = [s.name for s in stack if s.kind in ('namespace', 'class')]
    return '::'.join(parts)


def classify_head(head, stack):
    head = normalise_ws(head)
    head = ACCESS_LABEL_RE.sub('', head).strip()
    while True:
        stripped = ACCESS_LABEL_RE.sub('', head).strip()
        if stripped == head:
            break
        head = stripped
    if head == '':
        return Scope('other', None)
    m = NAMESPACE_HEAD_RE.match(head)
    if m:
        return Scope('namespace', m.group(1)) if m.group(1) else Scope('other', None)
    if EXTERN_C_RE.match(head) or ANON_AGGREGATE_RE.match(head):
        return Scope('other', None)
    if head.startswith('enum'):
        return Scope('other', None)
    m = CLASS_HEAD_RE.match(head)
    if m and find_top_level(head, '=') < 0:
        return Scope('class', m.group(1))
    first_word = re.match(r'[A-Za-z_]\w*', head)
    if first_word and first_word.group(0) in CONTROL_HEAD:
        return Scope('other', None)
    if find_top_level(head, '=') >= 0 or '(' not in head:
        return Scope('other', None)
    sig = strip_template_prefix(head)
    name, _ = parse_function_signature(sig)
    if name is None:
        return Scope('other', None)
    prefix = scope_prefix(stack)
    if prefix:
        name = prefix + '::' + name
    return Scope('function', name)


def strip_template_prefix(text):
    text = text.lstrip()
    while text.startswith('template'):
        rest = text[len('template'):].lstrip()
        if not rest.startswith('<'):
            break
        depth = 0
        i = 0
        while i < len(rest):
            if rest[i] == '<':
                depth += 1
            elif rest[i] == '>':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        text = rest[i + 1:].lstrip()
    return text


# ----------------------------------------------------------------------------------------------
# Tags.
# ----------------------------------------------------------------------------------------------

class Tag:
    __slots__ = ('kind', 'text', 'file', 'line', 'raw', 'verify', 'scope')

    def __init__(self, kind, text, file, line, raw):
        self.kind = kind
        self.text = text
        self.file = file
        self.line = line        # 1-based
        self.raw = raw
        self.verify = None
        self.scope = ''


def split_top_level_commas(text):
    items = []
    depth = 0
    cur = []
    for ch in text:
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        if ch == ',' and depth == 0:
            items.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
    items.append(''.join(cur))
    return items


def parse_addresses(text):
    """[(address, template_args)] or None when the address text is not understood."""
    text = text.strip()
    m = re.match(r'^(0x[0-9A-Fa-f]{1,8})\s*(?:<(.*)>)?$', text, re.S)
    if m:
        return [(int(m.group(1), 16), normalise_ws(m.group(2) or ''))]
    if text.startswith('[') and text.endswith(']'):
        result = []
        for item in split_top_level_commas(text[1:-1]):
            item = item.strip()
            m = re.match(r'^(0x[0-9A-Fa-f]{1,8})\s*(?:<(.*)>)?$', item, re.S)
            if not m:
                return None
            result.append((int(m.group(1), 16), normalise_ws(m.group(2) or '')))
        return result if result else None
    return None


def collect_declaration(lines, masked_lines, start, terminators, max_lines=400):
    """
    Join the masked lines from `start` until the first top-level character of `terminators`
    (`{` or `;` for a function, plus `=` for a static so an aggregate initialiser is not entered).
    Returns (text_before_terminator, terminator, line_index_of_first_code) or (None, reason, _).
    """
    text = []
    depth = 0
    first = None
    i = start
    while i < len(lines) and i < start + max_lines:
        line = masked_lines[i]
        if first is None and line.strip() != '':
            first = i
        for ch in line:
            if ch in '([':
                depth += 1
            elif ch in ')]':
                depth -= 1
            elif depth == 0 and ch in terminators:
                return ''.join(text), ch, first
            text.append(ch)
        text.append('\n')
        i += 1
    if first is None:
        return None, 'nothing follows the tag', first
    return None, 'no body or terminator within %d lines' % max_lines, first


def c_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


# ----------------------------------------------------------------------------------------------
# The original image: PE sections, and function extents derived from them.
# ----------------------------------------------------------------------------------------------

IMAGE_RELATIVE = os.path.join('client', 'game', 'base', 'Wow.exe')


class Section:
    __slots__ = ('name', 'start', 'end', 'raw_offset', 'raw_size', 'code')

    def __init__(self, name, start, end, raw_offset, raw_size, code):
        self.name = name
        self.start = start          # virtual address of the first byte
        self.end = end              # virtual address past the last byte (VirtualSize)
        self.raw_offset = raw_offset
        self.raw_size = raw_size
        self.code = code            # IMAGE_SCN_CNT_CODE


class Image:
    """Wow.exe: its section table and a reader for its bytes by virtual address."""

    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.data = f.read()
        d = self.data
        if len(d) < 0x40 or d[:2] != b'MZ':
            fail('not a PE image: %s' % path)
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            fail('not a PE image: %s' % path)
        machine, count, _, _, _, opt_size, _ = struct.unpack_from('<HHIIIHH', d, pe + 4)
        if machine != 0x14C:
            fail('not a 32-bit x86 image: %s' % path)
        magic = struct.unpack_from('<H', d, pe + 24)[0]
        if magic != 0x10B:
            fail('not a PE32 image: %s' % path)
        self.base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
        self.sections = []
        off = pe + 24 + opt_size
        for i in range(count):
            hdr = d[off + i * 40:off + i * 40 + 40]
            name = hdr[:8].rstrip(b'\0').decode('ascii', errors='replace')
            vsize, vaddr, rsize, roff = struct.unpack_from('<IIII', hdr, 8)
            flags = struct.unpack_from('<I', hdr, 36)[0]
            self.sections.append(Section(name, self.base + vaddr, self.base + vaddr + vsize,
                                         roff, rsize, bool(flags & 0x20)))

    def section_of(self, va):
        for s in self.sections:
            if s.start <= va < s.end:
                return s
        return None

    def read(self, va, n):
        """Bytes [va, va+n) of the mapped image; bytes past a section's raw data read as zero."""
        s = self.section_of(va)
        if s is None:
            return b''
        n = min(n, s.end - va)
        raw = va - s.start
        avail = max(0, min(n, s.raw_size - raw))
        out = self.data[s.raw_offset + raw:s.raw_offset + raw + avail] if avail else b''
        return out + b'\0' * (n - len(out))


def find_image(explicit):
    """The image path: --image, then $WXL_CLIENT_ROOT, then the ancestors of this script."""
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    root = os.environ.get('WXL_CLIENT_ROOT')
    if root and os.path.isfile(os.path.join(root, IMAGE_RELATIVE)):
        return os.path.join(root, IMAGE_RELATIVE)
    here = os.path.dirname(os.path.abspath(__file__))
    while True:
        candidate = os.path.join(here, IMAGE_RELATIVE)
        if os.path.isfile(candidate):
            return candidate
        parent = os.path.dirname(here)
        if parent == here:
            return None
        here = parent


class Extent:
    __slots__ = ('bound', 'length', 'status', 'section')

    def __init__(self, bound, length, status, section):
        self.bound = bound          # distance to the next tagged address in the section; 0 = none
        self.length = length        # decoded length; 0 = not determined
        self.status = status        # x86_sweep status, or 'no image'
        self.section = section      # section name, or None


def compute_extents(image, fn_addresses, st_addresses):
    """
    Extent per distinct function address. Boundaries are the distinct tagged addresses (of both
    kinds) of the same section, so a folded address counts once and a code address never takes
    its bound from a data address. Without an image every function address is one bucket and
    no length is decoded.
    """
    extents = {}
    if image is None:
        ordered = sorted(fn_addresses)
        for a, nxt in zip(ordered, ordered[1:] + [None]):
            extents[a] = Extent(nxt - a if nxt is not None else 0, 0, 'no image', None)
        return extents
    buckets = collections.defaultdict(set)
    for a in fn_addresses | st_addresses:
        s = image.section_of(a)
        buckets[s.name if s else None].add(a)
    for name, addresses in buckets.items():
        ordered = sorted(addresses)
        section = next((s for s in image.sections if s.name == name), None)
        for a, nxt in zip(ordered, ordered[1:] + [None]):
            if a not in fn_addresses:
                continue
            bound = nxt - a if nxt is not None else 0
            length, status = 0, 'no section'
            if section is not None and section.code:
                limit = nxt if nxt is not None else section.end
                end, status = x86_sweep.function_end(image.read, a, limit)
                if end is not None:
                    length = end - a
            extents[a] = Extent(bound, length, status, name)
    return extents


def gap_is_whole_functions(image, start, limit):
    """
    Whether [start, limit) is int3 padding plus a chain of functions the sweep can delimit,
    the last of which ends at limit. Returns (True, limit) or (False, where the chain stops).
    """
    pos = start
    align = x86_sweep.ALIGN
    chained = 0
    while pos < limit:
        if image.read(pos, 1) == b'\xcc':
            pos += 1
            continue
        if pos % align:
            return False, pos, chained
        end, status = x86_sweep.function_end(image.read, pos, limit)
        if end is None:
            return False, pos, chained
        pos = end
        chained += 1
    return True, limit, chained


def padding_start_before(image, va):
    """The start of the int3 run that ends at va, or va when the byte before it is not int3."""
    s = image.section_of(va - 1)
    if s is None:
        return va
    p = va
    while p - 1 >= s.start and image.read(p - 1, 1) == b'\xcc':
        p -= 1
    return p


# ----------------------------------------------------------------------------------------------
# Main pass.
# ----------------------------------------------------------------------------------------------

class Row:
    __slots__ = ('name', 'address', 'file', 'line', 'targs', 'verify', 'kind')

    def __init__(self, kind, name, address, file, line, targs, verify):
        self.kind = kind
        self.name = name
        self.address = address
        self.file = file
        self.line = line
        self.targs = targs
        self.verify = verify


class Unparsed:
    __slots__ = ('tag', 'reason', 'source')

    def __init__(self, tag, reason, source):
        self.tag = tag
        self.reason = reason
        self.source = source


def scan_file(root, rel, results):
    path = os.path.join(root, rel)
    with open(path, 'rb') as f:
        data = f.read()
    text = data.decode('utf-8-sig', errors='replace').replace('\r\n', '\n').replace('\r', '\n')
    lines = text.split('\n')
    tags = []
    for idx, line in enumerate(lines):
        m = TAG_RE.match(line)
        if m:
            tags.append(Tag(m.group(1), m.group(2), rel, idx + 1, line.rstrip()))
    if not tags:
        return
    masked = mask_source(text)
    masked_lines = masked.split('\n')

    # Line start offsets, to map tag lines to positions in the masked text.
    line_start = [0]
    for line in lines[:-1]:
        line_start.append(line_start[-1] + len(line) + 1)

    # Sweep the masked text once, snapshotting the scope stack at each tag line.
    stack = []
    head_start = 0
    paren_depth = 0
    tag_i = 0
    tag_positions = [line_start[t.line - 1] for t in tags]
    i = 0
    n = len(masked)
    while i < n:
        while tag_i < len(tags) and tag_positions[tag_i] <= i:
            tags[tag_i].scope = scope_prefix(stack)
            tag_i += 1
        ch = masked[i]
        if ch == '(':
            paren_depth += 1
        elif ch == ')':
            paren_depth = max(0, paren_depth - 1)
        elif ch == '{':
            stack.append(classify_head(masked[head_start:i], stack))
            head_start = i + 1
            paren_depth = 0
        elif ch == '}':
            if stack:
                stack.pop()
            head_start = i + 1
            paren_depth = 0
        elif ch == ';' and paren_depth == 0:
            head_start = i + 1
        i += 1
    while tag_i < len(tags):
        tags[tag_i].scope = scope_prefix(stack)
        tag_i += 1

    # VERIFY attaches to the OFFSET tag on the very next line.
    by_line = {t.line: t for t in tags}
    for t in tags:
        if t.kind == 'OFFSET':
            prev = by_line.get(t.line - 1)
            if prev is not None and prev.kind == 'VERIFY':
                m = VERIFY_RE.match(prev.text)
                t.verify = m.group(2) if m else 'unknown'
                results['verify_attached'].add((prev.file, prev.line))

    for t in tags:
        results['tags'].append(t)
        if t.kind not in ('OFFSET', 'STATIC'):
            continue
        addresses = parse_addresses(t.text)
        if addresses is None:
            results['unparsed'].append(Unparsed(t, 'address text not understood', t.raw))
            continue
        decl, term, first = collect_declaration(
            lines, masked_lines, t.line, '{;' if t.kind == 'OFFSET' else '{;=')
        source_line = lines[first].rstrip() if first is not None else ''
        if decl is None:
            results['unparsed'].append(Unparsed(t, term, source_line))
            continue
        stripped = strip_template_prefix(decl)
        # The row points at the line that carries the name: past blank lines and `template <...>`.
        first = t.line + decl[:len(decl) - len(stripped)].count('\n')
        decl = stripped
        if t.kind == 'OFFSET':
            name, reason = parse_function_signature(decl)
        else:
            name, reason = parse_static_declaration(decl)
        if name is None:
            results['unparsed'].append(Unparsed(t, reason, source_line))
            continue
        if t.scope:
            name = t.scope + '::' + name
        results['parsed'] += 1
        for address, targs in addresses:
            results['rows'].append(Row(t.kind, name, address, rel, first + 1, targs, t.verify))


def walk(root):
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for fn in sorted(filenames):
            if fn.lower().endswith(SOURCE_EXTENSIONS):
                rel = os.path.relpath(os.path.join(dirpath, fn), root).replace('\\', '/')
                files.append(rel)
    files.sort()
    return files


def is_header(rel):
    return rel.lower().endswith(HEADER_EXTENSIONS)


def tree_of(rel):
    parts = rel.split('/')
    return '/'.join(parts[:2]) if len(parts) > 2 else parts[0]


DEF_HEADER = """\
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// {title}
//
// GENERATED by core/tools/offsets/extract_offsets.py from the client fork's {tag} tags.
// Do not edit: re-run the tool after pulling upstream and review the diff.
//
// One row per address, as an X-macro: {macro}{columns}.
// Every text column is a C string literal; the address is Wow.exe (build 12340) virtual address.
// Rows are sorted by address, then name, then file and line, so the file diffs cleanly.
//
// This table is data, not an interface. Include it from ONE translation unit only, with
// {macro} defined to append to a lookup table (name -> address, address -> name). Never
// include it from a header: it is {count} rows.
//
{notes}
#ifndef {macro}
#error "define {macro}{columns} before including {filename}"
#endif

"""

FUNCTION_NOTES = """\
// Columns:
//   name     the qualified C++ name as written in the fork (enclosing namespaces and classes
//            applied; explicit specialisations keep their <args>; operators as `operator+=`,
//            `operator()`, `operator uint32_t`).
//   address  one instantiation's address. A template tag lists several addresses: one row each.
//   bound    an UPPER BOUND on the function's size: the distance from its address to the next
//            tagged address in the same section of Wow.exe (functions and statics, each
//            distinct address once). The decompilation does not cover the whole binary, so
//            untagged code may sit between the two; a large bound means a large untagged gap,
//            not a large function. 0 is a sentinel: the address is the last tagged one of its
//            section. Rows sharing an address (folded bodies) share the bound.
//   length   the function's decoded size, from a linear sweep of its bytes that stops at the
//            int3 padding the linker puts before the next function (see x86_sweep.py). Always
//            <= bound when both are known. 0 means the sweep could not settle the end: the
//            function ends exactly on a 16-byte boundary with code after it, which the bytes
//            alone cannot split between it and its (untagged) successor, or an opcode the
//            decoder does not know. A site inside the function is valid at delta < length,
//            or < bound when length is 0.
//   verify   "manual", "auto" or "none": the VERIFY: tag above the OFFSET: tag, i.e. whether the
//            decompiled body was checked by hand against the original. A confidence signal.
//   targs    the instantiation's template arguments when the tag gives them ("" otherwise);
//            instantiations tagged without arguments differ by address only.
//   file     the source file, relative to the client checkout, forward slashes.
//   line     the 1-based line of the definition (the line after the tag block).
"""

STATIC_NOTES = """\
// Columns:
//   name     the qualified C++ name (enclosing namespaces and classes applied). A function-local
//            static is qualified by its function: `CmdLineGetString::buffer`.
//   address  the static's address.
//   file     the source file, relative to the client checkout, forward slashes.
//   line     the 1-based line of the declaration (the line after the tag).
"""


def row_key(r):
    return (r.address, r.name, r.file, r.line, r.targs)


SHARD_HEADER = """// SPDX-License-Identifier: MIT
// GENERATED by tools/offsets/extract_offsets.py -- do not edit.
// Rows for {module}. Included by ../../{aggregate}; see it for the column contract.

#ifndef {macro}
    #error "include {aggregate}, not this shard"
#endif

"""


def module_of(path):
    """The shard a row belongs to: the fork's own module, so an upstream pull
    touches only the shards whose module changed."""
    parts = path.replace(os.sep, '/').split('/')
    if len(parts) >= 3 and parts[0] in ('Engine', 'WoW') and parts[1] == 'Source':
        # A source file sitting directly in Source/ has no module of its own.
        third = '_root' if '.' in parts[2] else parts[2]
        return (parts[0] + '/' + third).lower()
    if len(parts) >= 2:
        # A source file sitting directly in the tree root has no module of its own.
        second = '_root' if '.' in parts[1] else parts[1]
        return (parts[0] + '/' + second).lower()
    return parts[0].lower()


def write_sharded(out_dir, stem, rows, macro, render, header_kwargs):
    """One file per module, plus an aggregate that includes them in order."""
    buckets = {}
    for r in rows:
        buckets.setdefault(module_of(r.file), []).append(r)

    for module, group in buckets.items():
        shard = os.path.join(out_dir, stem, *module.split('/'))
        shard += '.def'
        os.makedirs(os.path.dirname(shard), exist_ok=True)
        with open(shard, 'w', encoding='utf-8', newline='\n') as f:
            f.write(SHARD_HEADER.format(module=module, macro=macro,
                                        aggregate=stem + '.def'))
            for r in sorted(group, key=row_key):
                f.write(render(r))

    aggregate = os.path.join(out_dir, stem + '.def')
    with open(aggregate, 'w', encoding='utf-8', newline='\n') as f:
        f.write(DEF_HEADER.format(count=len(rows), filename=stem + '.def',
                                  **header_kwargs))
        f.write('// %d shards, one per module of the client source tree.\n\n' % len(buckets))
        for module in sorted(buckets):
            f.write('#include "%s/%s.def"\n' % (stem, module))
    return len(buckets)


def write_functions(out_dir, rows, extents):
    def render(r):
        e = extents[r.address]
        return 'WXL_OFFSET_FUNCTION(%s, 0x%08X, 0x%X, 0x%X, %s, %s, %s, %d)\n' % (
            c_string(r.name), r.address, e.bound, e.length, c_string(r.verify or 'none'),
            c_string(r.targs), c_string(r.file), r.line)

    return write_sharded(out_dir, 'functions', rows, 'WXL_OFFSET_FUNCTION', render, dict(
        title='Function addresses of Wow.exe 3.3.5a (12340), derived from the client fork.',
        tag='OFFSET:', macro='WXL_OFFSET_FUNCTION',
        columns='(name, address, bound, length, verify, targs, file, line)',
        notes=FUNCTION_NOTES))


def write_statics(out_dir, rows):
    def render(r):
        return 'WXL_OFFSET_STATIC(%s, 0x%08X, %s, %d)\n' % (
            c_string(r.name), r.address, c_string(r.file), r.line)

    return write_sharded(out_dir, 'statics', rows, 'WXL_OFFSET_STATIC', render, dict(
        title='Static and global addresses of Wow.exe 3.3.5a (12340), derived from the client fork.',
        tag='STATIC:', macro='WXL_OFFSET_STATIC',
        columns='(name, address, file, line)',
        notes=STATIC_NOTES))


def write_unparsed(path, unparsed):
    unparsed = sorted(unparsed, key=lambda u: (u.tag.file, u.tag.line))
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# OFFSET:/STATIC: tags the tool could not confidently name: %d\n' % len(unparsed))
        f.write('# Each entry: file:line  KIND  reason, then the tag line and the first source line after it.\n')
        f.write('# These addresses are NOT in functions.def / statics.def. Fix the parser or the source.\n\n')
        for u in unparsed:
            f.write('%s:%d  %s  %s\n' % (u.tag.file, u.tag.line, u.tag.kind, u.reason))
            f.write('    %s\n' % u.tag.raw.strip())
            if u.source.strip():
                f.write('    %s\n' % u.source.strip())
            f.write('\n')


def pct(part, whole):
    return 100.0 * part / whole if whole else 0.0


BOUND_BUCKETS = ((16, 'under 16'), (64, '16 to 63'), (256, '64 to 255'), (1024, '256 to 1023'),
                 (4096, '1024 to 4095'), (None, '4096 and over'))


def bound_bucket(b):
    for limit, label in BOUND_BUCKETS:
        if limit is None or b < limit:
            return label


def write_extent_summary(w, image, extents, fn_rows, st_rows):
    fn_addresses = {r.address for r in fn_rows}
    st_addresses = {r.address for r in st_rows}
    names = collections.defaultdict(set)
    for r in fn_rows:
        names[r.address].add(r.name)

    def label(a):
        nm = sorted(names[a])
        return nm[0] + (' (+%d more)' % (len(nm) - 1) if len(nm) > 1 else '')

    w('Image and sections (what the function extents are measured against)')
    w('--------------------------------------------------------------------')
    if image is None:
        w('  IMAGE NOT FOUND: %s was not located (--image, $WXL_CLIENT_ROOT, or an ancestor of the tool).' % IMAGE_RELATIVE)
        w('  FALLBACK HEURISTIC IN EFFECT: every function address is treated as one code section, the')
        w('  bound is the distance to the next tagged function address whatever its section, and no')
        w('  length is decoded (every length column is 0). Re-run with the image to get real extents.')
    else:
        w('  image : %s, %d bytes, base 0x%08X, %d sections' % (
            os.path.basename(image.path), len(image.data), image.base, len(image.sections)))
        w('  %-8s %-21s %-5s %10s %8s' % ('section', 'virtual range', 'kind', 'functions', 'statics'))
        for s in image.sections:
            fc = sum(1 for a in fn_addresses if s.start <= a < s.end)
            sc = sum(1 for a in st_addresses if s.start <= a < s.end)
            w('  %-8s 0x%08X-0x%08X %-5s %10d %8d' % (
                s.name, s.start, s.end, 'code' if s.code else 'data', fc, sc))
        out_fn = sorted(a for a in fn_addresses if image.section_of(a) is None)
        out_st = sorted(a for a in st_addresses if image.section_of(a) is None)
        w('  outside every section: functions %d, statics %d' % (len(out_fn), len(out_st)))
        for a in out_fn[:20]:
            w('    0x%08X  %s' % (a, label(a)))
        for a in out_st[:20]:
            w('    0x%08X  (static)' % a)
        extra = [s.name for s in image.sections if s.name not in ('.text', '.rdata', '.data', '.rsrc', '.reloc', '.tls', '.idata', '.zdata')]
        if extra:
            w('  note: the image carries sections a stock 12340 Wow.exe does not (%s); only the' % ', '.join(extra))
            w('  section ranges are used, and the bytes of .text are read as they are in this file.')
    w('')

    w('Function extents (bound: next tagged address in the section; length: decoded end)')
    w('-----------------------------------------------------------------------------------')
    distinct = sorted(fn_addresses)
    bounds = [extents[a].bound for a in distinct]
    sentinels = [a for a in distinct if extents[a].bound == 0]
    w('  distinct function addresses %d (rows %d; rows sharing an address share its extent)' % (len(distinct), len(fn_rows)))
    w('  sentinels (bound 0: last tagged address of its section): %d' % len(sentinels))
    for a in sentinels:
        e = extents[a]
        w('    0x%08X  %s  in %s%s' % (a, label(a), e.section or '?',
                                        ('  length 0x%X (decoded up to the section end)' % e.length) if e.length else ''))
    w('  bound distribution (bytes), sentinels excluded:')
    hist = collections.Counter(bound_bucket(b) for b in bounds if b)
    for _, lbl in BOUND_BUCKETS:
        w('    %-14s %6d' % (lbl, hist.get(lbl, 0)))
    unaligned = [a for a in distinct if a % x86_sweep.ALIGN]
    w('  function addresses not %d-byte aligned: %d (the linker aligns every function start; such a' % (x86_sweep.ALIGN, len(unaligned)))
    w('  tag points into a body and should be fixed upstream; meanwhile it cuts the previous bound short)')
    for a in unaligned:
        w('    0x%08X  %s' % (a, label(a)))
    w('  largest bounds (a big untagged gap follows each; the length shows the function itself):')
    for a in sorted((a for a in distinct if extents[a].bound), key=lambda a: (-extents[a].bound, a))[:20]:
        e = extents[a]
        w('    0x%08X  bound 0x%06X  length 0x%05X  %s' % (a, e.bound, e.length, label(a)))
    w('')

    w('  length (0 = not determined):')
    status = collections.Counter(extents[a].status for a in distinct)
    determined = sum(1 for a in distinct if extents[a].length)
    w('    determined %d, not determined %d' % (determined, len(distinct) - determined))
    for st, text in (('padding', 'ends before int3 padding'),
                     ('limit', 'runs up to the next tagged address, no padding between'),
                     ('aligned', 'NOT determined: ret/jmp on a 16-byte boundary, code follows (its own funclet or an untagged neighbour)'),
                     ('undecodable', 'NOT determined: opcode unknown to the decoder'),
                     ('empty', 'NOT determined: the start byte is padding'),
                     ('no section', 'NOT determined: address outside a code section'),
                     ('no image', 'NOT determined: no image')):
        if status.get(st):
            w('    %6d  %s' % (status[st], text))
    if image is not None:
        over = [a for a in distinct if extents[a].length and extents[a].bound and extents[a].length > extents[a].bound]
        w('    length greater than bound: %d (must be 0)' % len(over))
        # Cross-check: between a decoded end and the next tagged address there must be nothing
        # but int3 padding and whole untagged functions, i.e. the gap must itself decode as a
        # chain of padded functions that lands exactly on the successor. A chain that breaks
        # means the sweep ended too early (it cut the function) or the decoder is wrong.
        direct = chained = 0
        broken = []
        for a, nxt in zip(distinct, distinct[1:]):
            e = extents[a]
            if not e.length or e.section != '.text':
                continue
            if e.length == e.bound:
                direct += 1
                continue
            end = a + e.length
            if padding_start_before(image, nxt) == end:
                direct += 1
                continue
            ok, stop, count = gap_is_whole_functions(image, end, nxt)
            if ok:
                chained += 1
            else:
                broken.append((a, e, stop, count))
        w('    cross-check of every decoded length against the bytes up to the tagged successor:')
        w('      %6d  the successor (or the padding before it) follows the function directly' % direct)
        w('      %6d  untagged functions follow: the gap decodes as a chain of padded functions' % chained)
        w('      %6d  the gap does not decode as whole functions all the way to the successor. A chain' % len(broken))
        w('              that stops after 0 functions right at the decoded end would mean the sweep cut the')
        w('              function; one that stops later means the gap holds something the sweep cannot')
        w('              delimit (an untagged function ending on alignment, data, CRT or library code).')
        for a, e, stop, count in broken[:40]:
            w('      0x%08X  length 0x%X (%s)  bound 0x%X  chain of %d stops at 0x%08X  %s' % (
                a, e.length, e.status, e.bound, count, stop, label(a)))
        for st in ('aligned', 'undecodable', 'empty'):
            sample = [a for a in distinct if extents[a].status == st][:10]
            if sample:
                w('    first %s %d: %s' % (st, len(sample), ', '.join('0x%08X' % a for a in sample)))
    w('')


def write_summary(path, root, files, results, out_dir, image, extents):
    tags = results['tags']
    rows = results['rows']
    unparsed = results['unparsed']
    by_kind = collections.Counter(t.kind for t in tags)
    offset_tags = [t for t in tags if t.kind == 'OFFSET']
    static_tags = [t for t in tags if t.kind == 'STATIC']
    fn_rows = [r for r in rows if r.kind == 'OFFSET']
    st_rows = [r for r in rows if r.kind == 'STATIC']
    un_offset = [u for u in unparsed if u.tag.kind == 'OFFSET']
    un_static = [u for u in unparsed if u.tag.kind == 'STATIC']
    lines = []
    w = lines.append

    w('Offset extraction summary')
    w('=========================')
    # The folder name only: the file is committed and must not carry a machine's absolute path.
    w('client checkout : %s' % os.path.basename(root.rstrip('/\\')))
    w('source files    : %d scanned, %d carry tags' % (len(files), len({t.file for t in tags})))
    w('')
    w('Tags')
    w('----')
    for kind in ('OFFSET', 'STATIC', 'VERIFY', 'INLINED', 'NOTE'):
        w('  %-8s %6d' % (kind + ':', by_kind.get(kind, 0)))
    w('')
    w('Per tree (top two path components)')
    trees = collections.defaultdict(collections.Counter)
    for t in tags:
        trees[tree_of(t.file)][t.kind] += 1
    for tree in sorted(trees):
        c = trees[tree]
        w('  %-22s OFFSET %6d  STATIC %5d  VERIFY %6d  INLINED %3d  NOTE %3d' % (
            tree, c['OFFSET'], c['STATIC'], c['VERIFY'], c['INLINED'], c['NOTE']))
    w('')
    w('Headers versus sources')
    for kind, tl in (('OFFSET', offset_tags), ('STATIC', static_tags)):
        h = sum(1 for t in tl if is_header(t.file))
        w('  %-8s headers %5d   sources %6d' % (kind + ':', h, len(tl) - h))
    w('')
    w('Reconciliation (the tool fails if these do not add up)')
    w('------------------------------------------------------')
    off_parsed = len(offset_tags) - len(un_offset)
    st_parsed = len(static_tags) - len(un_static)
    w('  OFFSET tags %6d = parsed %6d + unparsed %4d   -> functions.def rows %6d  coverage %6.2f%%' % (
        len(offset_tags), off_parsed, len(un_offset), len(fn_rows), pct(off_parsed, len(offset_tags))))
    w('  STATIC tags %6d = parsed %6d + unparsed %4d   -> statics.def rows   %6d  coverage %6.2f%%' % (
        len(static_tags), st_parsed, len(un_static), len(st_rows), pct(st_parsed, len(static_tags))))
    w('  (a template tag with N addresses yields N rows; the parsed/unparsed split counts tags)')
    w('')
    w('Template tags (bracketed OFFSET:), by number of addresses')
    hist = collections.Counter()
    targ_tags = 0
    for t in offset_tags:
        a = parse_addresses(t.text)
        if a is not None and t.text.strip().startswith('['):
            hist[len(a)] += 1
            if any(ta for _, ta in a):
                targ_tags += 1
    w('  tags %d, with template arguments in the tag %d' % (sum(hist.values()), targ_tags))
    w('  ' + ', '.join('%d addresses: %d' % (k, hist[k]) for k in sorted(hist)))
    w('')
    w('VERIFY (confidence that the decompiled body matches the original)')
    w('-----------------------------------------------------------------')
    vc = collections.Counter((t.verify or 'none') for t in offset_tags)
    for k in ('manual', 'auto', 'none', 'unknown'):
        if vc.get(k):
            w('  OFFSET tags with VERIFY %-8s %6d' % (k + ':', vc[k]))
    orphans = [t for t in tags if t.kind == 'VERIFY' and (t.file, t.line) not in results['verify_attached']]
    w('  VERIFY tags not directly above an OFFSET tag: %d' % len(orphans))
    for t in orphans:
        w('    %s:%d  %s' % (t.file, t.line, t.raw.strip()))
    w('')
    w('Unparsed residue, by reason')
    w('---------------------------')
    rc = collections.Counter((u.tag.kind, u.reason) for u in unparsed)
    for (kind, reason), n in sorted(rc.items(), key=lambda kv: (-kv[1], kv[0])):
        w('  %5d  %-7s %s' % (n, kind, reason))
    w('  (see unparsed.txt)')
    w('')

    write_extent_summary(w, image, extents, fn_rows, st_rows)

    w('Addresses carrying more than one row (thunks, Win32/Win64 twins, shared bodies)')
    w('------------------------------------------------------------------------------')
    for label, rl in (('functions', fn_rows), ('statics', st_rows)):
        by_addr = collections.defaultdict(list)
        for r in rl:
            by_addr[r.address].append(r)
        dup = {a: v for a, v in by_addr.items() if len(v) > 1}
        same_name = sum(1 for v in dup.values() if len({r.name for r in v}) == 1)
        w('  %s: %d addresses with several rows (%d of them one name tagged in several files, %d with different names)' % (
            label, len(dup), same_name, len(dup) - same_name))
        for a in sorted(dup):
            names = sorted({r.name + (('<' + r.targs + '>') if r.targs else '') for r in dup[a]})
            locs = sorted({r.file for r in dup[a]})
            w('    0x%08X  %s' % (a, ' | '.join(names)))
            if len(names) == 1:
                w('              in %s' % ', '.join(locs))
    w('')

    w('Names carrying more than one address (overloads, template instantiations, same name in two scopes)')
    w('--------------------------------------------------------------------------------------------------')
    for label, rl in (('functions', fn_rows), ('statics', st_rows)):
        by_name = collections.defaultdict(set)
        for r in rl:
            by_name[r.name].add(r.address)
        multi = {nm: v for nm, v in by_name.items() if len(v) > 1}
        w('  %s: %d names with several addresses' % (label, len(multi)))
        for nm in sorted(multi):
            w('    %-60s %s' % (nm, ' '.join('0x%08X' % a for a in sorted(multi[nm]))))
    w('')

    inl = [t for t in tags if t.kind == 'INLINED']
    w('INLINED tags (no address: the body is inlined in the original; ranges are call-site spans)')
    w('------------------------------------------------------------------------------------------')
    for t in inl:
        w('  %s:%d  %s' % (t.file, t.line, t.text))
    w('')
    notes = [t for t in tags if t.kind == 'NOTE']
    w('NOTE: lines (ordinary comments in third-party code, not address tags)')
    for t in notes:
        w('  %s:%d  %s' % (t.file, t.line, t.text))
    w('')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('client', help='path to the client checkout (read-only input)')
    ap.add_argument('--out', default=None,
                    help='output directory (default: ../../offsets relative to this script)')
    ap.add_argument('--reports', default=None,
                    help='where to write summary.txt and unparsed.txt (default: core/reports)')
    ap.add_argument('--image', default=None,
                    help='the original Wow.exe, for sections and function extents (default: '
                         '$WXL_CLIENT_ROOT/%s, else the first ancestor of this tool that has it)' % IMAGE_RELATIVE.replace('\\', '/'))
    ap.add_argument('--quiet', action='store_true', help='do not print the summary')
    args = ap.parse_args()

    root = os.path.abspath(args.client)
    if not os.path.isdir(root):
        fail('not a directory: %s' % root)
    image_path = find_image(args.image)
    if args.image and image_path is None:
        fail('image not found: %s' % args.image)
    image = Image(image_path) if image_path else None
    core = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    out_dir = args.out or os.path.join(core, 'include', 'wxl', 'offsets', 'generated')
    report_dir = args.reports or os.path.join(core, 'reports')
    out_dir = os.path.abspath(out_dir)
    report_dir = os.path.abspath(report_dir)

    results = {
        'tags': [], 'rows': [], 'unparsed': [], 'parsed': 0, 'verify_attached': set(),
    }
    files = walk(root)
    for rel in files:
        scan_file(root, rel, results)

    # Reconcile before writing anything.
    tags = results['tags']
    for kind in ('OFFSET', 'STATIC'):
        total = sum(1 for t in tags if t.kind == kind)
        unparsed = sum(1 for u in results['unparsed'] if u.tag.kind == kind)
        parsed_addresses = 0
        for t in tags:
            if t.kind == kind and not any(u.tag is t for u in results['unparsed']):
                parsed_addresses += len(parse_addresses(t.text))
        rows = sum(1 for r in results['rows'] if r.kind == kind)
        if parsed_addresses != rows:
            fail('%s: %d addresses in parsed tags but %d rows' % (kind, parsed_addresses, rows))
        parsed_tags = total - unparsed
        if parsed_tags < 0:
            fail('%s: more unparsed tags than tags' % kind)
    if results['parsed'] + len(results['unparsed']) != sum(1 for t in tags if t.kind in ('OFFSET', 'STATIC')):
        fail('parsed + unparsed tags != OFFSET + STATIC tags')
    # Every row must point at a real tag of its own kind, in a file we read.
    seen = {(t.file, t.kind) for t in tags}
    for r in results['rows']:
        if (r.file, r.kind) not in seen:
            fail('row without a tag: %s %s' % (r.file, r.name))

    fn_rows = [r for r in results['rows'] if r.kind == 'OFFSET']
    st_rows = [r for r in results['rows'] if r.kind == 'STATIC']
    extents = compute_extents(image, {r.address for r in fn_rows}, {r.address for r in st_rows})
    for r in fn_rows:
        e = extents.get(r.address)
        if e is None:
            fail('no extent for 0x%08X' % r.address)
        if e.length and e.bound and e.length > e.bound:
            fail('length 0x%X exceeds bound 0x%X at 0x%08X' % (e.length, e.bound, r.address))

    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(report_dir, exist_ok=True)
    fn_shards = write_functions(out_dir, fn_rows, extents)
    st_shards = write_statics(out_dir, st_rows)
    write_unparsed(os.path.join(report_dir, 'unparsed.txt'), results['unparsed'])
    lines = write_summary(os.path.join(report_dir, 'summary.txt'), root, files, results, out_dir,
                          image, extents)
    lines.append('  shards: %d for functions, %d for statics' % (fn_shards, st_shards))
    if not args.quiet:
        stop = next((i for i, l in enumerate(lines) if l.startswith('Addresses carrying')), len(lines))
        print('\n'.join(lines[:stop]))
        print('written to %s' % out_dir)


if __name__ == '__main__':
    main()
