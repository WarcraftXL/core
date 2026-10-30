#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 WarcraftXL
#
# Derive the SDK's update-field table from the client fork.
"""
Write `sdk/include/wxl/update_fields.def`: every descriptor field of every kind of object the
client keeps -- object, item, container, unit, player, game object, dynamic object, corpse --
with its dword index, its size, the kind of value it holds, who the server sends it to, and
whether the client keeps the copy of it that lets a change be seen.

Everything comes from the fork, and each fact is read twice:

  the client's own field descriptors   ObjectMgrClient/Mirror.cpp: OBJECT_DISCRIPTORS,
                                       ITEM_DESCRIPTORS, ... -- each field's name, offset in
                                       dwords, count, value type and visibility flags, as the
                                       client's tables hold them
  the descriptor blocks                CGObjectData, CGItemData, ... CGPlayerLocalData,
                                       compiled 32-bit with clang-cl and measured by the
                                       compiler (offsetof/4): every field must start on a
                                       member, and a block's size must be what its descriptors
                                       add up to
  the dword counts                     s_objDwordCount and GetNumDwordBlocks
                                       (ObjectMgrClient.cpp): where each kind's block ends, and
                                       how far a player other than the active one reaches
  the mirror lists                     CGObject::s_savedFields, CGItem::s_mirrorFields,
                                       CGUnit::s_mirrorFieldIndices, ... (Object/*.cpp): the
                                       dwords the client copies before an update, which are the
                                       only ones a mirror handler may be set on

The code names (MaxHealth for UNIT_FIELD_MAXHEALTH) are the client's names with the kind's prefix
dropped and the words capitalised; the words the client runs together are spelled by
words.txt, beside this script, which a regeneration keeps.

With --binary, every one of those tables is also read out of a stock Wow.exe 12340 at the
address its STATIC tag gives, and compared record by record.

Usage:
    generate_update_fields.py [--client <fork>] [--out <update_fields.def>] [--binary Wow.exe]
                              [--clang-cl <path>] [--xwin <dir>] [--check]

--check writes nothing and fails when the file on disk differs from what the fork gives: the
fork's build runs it (client/WarcraftXL/CMakeLists.txt, wxl-fields-check).

Python 3, standard library only, and clang-cl (any LLVM with the MSVC target) with Microsoft's
headers, given by --xwin when they are not in the environment (an xwin splat on Linux; on
Windows, a Developer prompt's INCLUDE is enough and --xwin is not needed).
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))

# The kinds, in OBJECT_TYPE_ID order: name, the wxl_object_type it is, its parent, its
# descriptor array in Mirror.cpp, its descriptor block, the header that declares it, the mirror
# list(s) as (file, qualified array), and the prefixes its field names carry.
KINDS = [
    ('Object', 'WXL_OBJECT_UNKNOWN', None, 'OBJECT_DISCRIPTORS', 'CGObjectData', 'Object.h',
     [('Object.cpp', 'CGObject::s_savedFields')], ['OBJECT_FIELD_']),
    ('Item', 'WXL_OBJECT_ITEM', 'Object', 'ITEM_DESCRIPTORS', 'CGItemData', 'Item.h',
     [('Item.cpp', 'CGItem::s_mirrorFields')], ['ITEM_FIELD_', 'ITEM_']),
    ('Container', 'WXL_OBJECT_CONTAINER', 'Item', 'CONTAINER_DESCRIPTORS', 'CGContainerData',
     'Container.h', [('Container.cpp', 'CGContainer::s_mirrorFields')],
     ['CONTAINER_FIELD_', 'CONTAINER_']),
    ('Unit', 'WXL_OBJECT_UNIT', 'Object', 'UNIT_DESCRIPTORS', 'CGUnitData', 'Unit.h',
     [('Unit.cpp', 'CGUnit::s_mirrorFieldIndices')], ['UNIT_FIELD_', 'UNIT_']),
    ('Player', 'WXL_OBJECT_PLAYER', 'Unit', 'PLAYER_DESCRIPTORS', 'CGPlayerLocalData',
     'Player.h', [('Player.cpp', 'CGPlayer::s_mirrorFields'),
                  ('Player.cpp', 'CGPlayer::s_localMirrorFields')],
     ['PLAYER__FIELD_', 'PLAYER_FIELD_', 'PLAYER_']),
    ('GameObject', 'WXL_OBJECT_GAMEOBJECT', 'Object', 'GAMEOBJECT_DESCRIPTORS',
     'CGGameObjectData', 'GameObject.h', [('GameObject.cpp', 'CGGameObject::s_mirrorFieldIndex')],
     ['GAMEOBJECT_FIELD_', 'GAMEOBJECT_', 'OBJECT_FIELD_']),
    ('DynamicObject', 'WXL_OBJECT_DYNAMIC', 'Object', 'DYNAMICOBJECT_DESCRIPTORS',
     'CGDynamicObjectData', 'DynamicObject.h', [], ['DYNAMICOBJECT_FIELD_', 'DYNAMICOBJECT_']),
    ('Corpse', 'WXL_OBJECT_CORPSE', 'Object', 'CORPSE_DESCRIPTORS', 'CGCorpseData', 'Corpse.h',
     [('Corpse.cpp', 'CGCorpse::s_mirrorFields')], ['CORPSE_FIELD_', 'CORPSE_']),
]

# The client's descriptor value types (CGFieldDescriptor::m_type).
TYPE_INT, TYPE_SHORTS, TYPE_FLOAT, TYPE_LONG, TYPE_BYTES, TYPE_BYTES2 = 1, 2, 3, 4, 5, 6

SCALARS = {
    'uint8_t': (1, 'u'), 'int8_t': (1, 's'), 'uint16_t': (2, 'u'), 'int16_t': (2, 's'),
    'uint32_t': (4, 'u'), 'int32_t': (4, 's'), 'float': (4, 'f'), 'uint64_t': (8, 'u'),
    'WOWGUID': (8, 'g'), 'TypeMask': (4, 'u'), 'OBJECT_TYPE': (4, 'u'),
}

HEADER = """\
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The update fields: every descriptor field of every kind of object the 3.3.5a client keeps, by
// the client's own names. GENERATED by core/tools/fields/generate_update_fields.py from the
// client fork; do not edit. The fork's build checks that this file is what the fork gives.
//
// An object's descriptor block is an array of dwords the server fills through update packets:
// OBJECT_FIELD_GUID at dword 0, the kind's own fields after its parent's. A field is addressed by
// its dword index in that array, which is what wxl_object_api's read_fields, write_fields and
// watch_field take, and what an ObjectFieldChanged event names.
//
//   WXL_UPDATE_FIELD_KIND(kind, type, parent, first, end, others_end)
//       a kind of object and its block: `type` the wxl_object_type it is (WXL_OBJECT_UNKNOWN for
//       the base every object has), `parent` the kind whose fields come before its own (None for
//       Object), its own fields in dwords [first, end). `others_end` is where the block of an
//       object of that kind ends: `end`, except for Player, whose fields past PLAYER_FIELD_PAD_0
//       the client keeps for the active player only.
//
//   WXL_UPDATE_FIELD(kind, NAME, Name, index, size, value, flags, watch)
//       one field: the kind that owns it, the client's name (UNIT_FIELD_MAXHEALTH), a name for
//       code (MaxHealth: the kind's prefix dropped, the words joined, each spelled as
//       core/tools/fields/words.txt writes the ones the client runs together), its first
//       dword, its size in dwords, and:
//
//       value   what each element holds: u32 i32 f32 one dword; guid an object's guid in two
//               dwords, low first; u64 a 64-bit value in two dwords (a mask of titles); bytes
//               four bytes, low first; u16x2 i16x2 two 16-bit halves, low first. A field of
//               `size` dwords holds size / 2 elements of a two-dword value, size of the others.
//       flags   who the server sends the field to, as the client's descriptors carry it:
//               0x001 everyone who sees the object, 0x002 its owner's client only, 0x004 its
//               owner, 0x010 an item's owner, 0x020 special info, 0x040 the owner's party,
//               0x100 a value worked out for each viewer (dynamic flags). 0 for padding.
//       watch   whether a change of it can be seen, which the client decides by keeping a copy
//               of a dword before an update and comparing: any -- every element has that copy
//               on every object of the kind, so watch_field may watch it; local -- the copy is
//               kept for the active player only (the player's fields past PLAYER_FIELD_PAD_0);
//               none -- no copy is kept, so a change goes unseen; some -- some elements have
//               it and others not, as the WATCH rows below say.
//
//   WXL_UPDATE_FIELD_WATCH(kind, first, end, scope)
//       dwords [first, end) of the kind that the client keeps that copy of, on every object of
//       the kind (scope any) or on the active player (scope local). watch_field accepts the
//       dwords these rows cover and refuses the others.
//
// Rows are in index order within each kind, kinds in wxl_object_type order. Include with
// WXL_UPDATE_FIELD defined; WXL_UPDATE_FIELD_KIND and WXL_UPDATE_FIELD_WATCH default to nothing.

#ifndef WXL_UPDATE_FIELD
#error "define WXL_UPDATE_FIELD(kind, NAME, Name, index, size, value, flags, watch) before including update_fields.def"
#endif

#ifndef WXL_UPDATE_FIELD_KIND
    #define WXL_UPDATE_FIELD_KIND(kind, type, parent, first, end, others_end)
    #define WXL_UPDATE_FIELD_KIND_DEFAULTED
#endif
#ifndef WXL_UPDATE_FIELD_WATCH
    #define WXL_UPDATE_FIELD_WATCH(kind, first, end, scope)
    #define WXL_UPDATE_FIELD_WATCH_DEFAULTED
#endif
"""

FOOTER = """
#ifdef WXL_UPDATE_FIELD_KIND_DEFAULTED
    #undef WXL_UPDATE_FIELD_KIND
    #undef WXL_UPDATE_FIELD_KIND_DEFAULTED
#endif
#ifdef WXL_UPDATE_FIELD_WATCH_DEFAULTED
    #undef WXL_UPDATE_FIELD_WATCH
    #undef WXL_UPDATE_FIELD_WATCH_DEFAULTED
#endif

// MSVC takes a file that expands to nothing outside its opening #ifndef X ... #endif for one
// guarded by X, and skips it when it is included again with X defined: this table is read
// several times in one translation unit, under shapes that may leave it empty. A directive
// after the rows keeps it from looking guarded.
#ifdef WXL_TABLE_END
#endif
"""


class Problem(Exception):
    pass


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def number(text):
    return int(text, 0)


# --- the fork's tables ----------------------------------------------------------------------

def static_before(source, position):
    """The address of the `// STATIC: 0x...` tag right above a definition."""
    tag = None
    for match in re.finditer(r'//\s*STATIC:\s*(0x[0-9A-Fa-f]+)', source[:position]):
        tag = match
    if not tag or source[tag.end():position].count('\n') > 2:
        raise Problem('no STATIC tag above the definition at offset %d' % position)
    return int(tag.group(1), 16)


def descriptor_arrays(mirror_cpp):
    source = read(mirror_cpp)
    arrays = {}
    for match in re.finditer(r'CGFieldDescriptor\s+(\w+)\s*\[(\d+)\]\s*=\s*\{(.*?)\n\};',
                             source, re.S):
        name, declared, body = match.group(1), int(match.group(2)), match.group(3)
        rows = []
        for row in re.finditer(r'\{\s*"(\w+)"\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\}',
                               body):
            rows.append((row.group(1), number(row.group(2)), number(row.group(3)),
                         number(row.group(4)), number(row.group(5))))
        if len(rows) != declared:
            raise Problem('%s declares %d descriptors and %d were read' % (name, declared, len(rows)))
        arrays[name] = (static_before(source, match.start()), rows)
    return arrays


def uint_array(source, qualified):
    """A `const uint32_t <qualified>[N] = { ... };` of numbers or enum names."""
    match = re.search(r'uint32_t\s+' + re.escape(qualified) + r'\s*\[(\w+)\]\s*=\s*\{(.*?)\};',
                      source, re.S)
    if not match:
        raise Problem('no definition of %s' % qualified)
    items = [item.strip() for item in match.group(2).replace('\n', ' ').split(',') if item.strip()]
    if match.group(1).isdigit() and len(items) != int(match.group(1)):
        raise Problem('%s declares %s entries and %d were read'
                      % (qualified, match.group(1), len(items)))
    return static_before(source, match.start()), items


def dword_counts(client):
    source = read(os.path.join(client, 'WoW/Source/ObjectMgrClient/ObjectMgrClient.cpp'))
    address, items = uint_array(source, 's_objDwordCount')
    counts = [number(item) for item in items]
    remote = re.search(r'(?:HEIR_TYPE_PLAYER|TYPEMASK_OBJECT \| TYPEMASK_UNIT \| TYPEMASK_PLAYER):\s*\{\s*return guid == '
                       r'ClntObjMgrGetActivePlayer\(\) \? (\w+) : (\w+);', source)
    if not remote:
        raise Problem('GetNumDwordBlocks: the player case was not found')
    return address, counts, number(remote.group(1)), number(remote.group(2))


# --- the descriptor blocks, measured --------------------------------------------------------

MEMBER_RE = re.compile(r'^\s*/\*\s*([0-9A-Fa-f]+)\s*\*/\s*([\w:]+)\s+(\w+)\s*((?:\[\s*\w+\s*\]\s*)*);')


def enumerator(object_dir, name):
    """The value of an enumerator the fork's object headers define, for an array bound named by it."""
    values = set()
    for root, _, files in os.walk(object_dir):
        for f in files:
            if f.endswith('.h'):
                for match in re.finditer(r'^\s*' + name + r'\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*,', read(os.path.join(root, f)), re.M):
                    values.add(number(match.group(1)))
    if len(values) != 1:
        raise Problem('the array bound %s is defined %d times under WoW/Source/Object' % (name, len(values)))
    return values.pop()


def class_members(text, name, object_dir):
    """The data members of `class|struct name { ... };`, in declaration order."""
    match = re.search(r'^(?:class|struct)\s+' + name + r'\b[^;{]*\{(.*?)^\};', text, re.S | re.M)
    if not match:
        return None
    base = re.search(r'^(?:class|struct)\s+' + name + r'\s*:\s*public\s+(\w+)', text, re.M)
    members = []
    for line in match.group(1).split('\n'):
        member = MEMBER_RE.match(line)
        if member:
            dims = [int(d) if d.isdigit() else enumerator(object_dir, d)
                    for d in re.findall(r'\w+', member.group(4))]
            members.append((member.group(3), member.group(2), dims, int(member.group(1), 16)))
    return (base.group(1) if base else None), members


def find_class(client, name, hint):
    """The members of a class of the fork, looked for in its header first."""
    object_dir = os.path.join(client, 'WoW/Source/Object')
    candidates = [os.path.join(object_dir, hint)] if hint else []
    for root, _, files in os.walk(object_dir):
        candidates += [os.path.join(root, f) for f in sorted(files) if f.endswith('.h')]
    for path in candidates:
        found = class_members(read(path), name, object_dir)
        if found:
            return path, found
    raise Problem('no declaration of %s under WoW/Source/Object' % name)


def default_clang_cl():
    for candidate in ('clang-cl', '/usr/lib/llvm-19/bin/clang-cl', '/usr/bin/clang-cl'):
        found = shutil.which(candidate)
        if found:
            return found
    return None


def measure(client, classes, clang_cl, xwin):
    """sizeof and the member offsets of every class, as clang-cl lays them out for Win32."""
    headers = sorted({path for path, _ in classes.values()})
    lines = ['#include <cstddef>']
    lines += ['#include "%s"' % os.path.relpath(h, client).replace(os.sep, '/') for h in headers]
    lines.append('static_assert(sizeof(void*) == 4, "the blocks are measured 32-bit");')
    lines.append('unsigned wxl_measured[] = {')
    lines += ['    sizeof(%s),' % name for name in sorted(classes)]
    lines.append('};')
    with tempfile.TemporaryDirectory() as temp:
        unit = os.path.join(temp, 'measure.cpp')
        with open(unit, 'w') as f:
            f.write('\n'.join(lines) + '\n')
        command = [clang_cl, '--target=i686-pc-windows-msvc', '/std:c++17', '/Zs', '-Wno-everything',
                   '/I', client, '-Xclang', '-fdump-record-layouts-simple', unit]
        if xwin:
            command[2:2] = ['/vctoolsdir', os.path.join(xwin, 'crt'),
                            '/winsdkdir', os.path.join(xwin, 'sdk')]
        result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise Problem('measuring the descriptor blocks failed:\n%s\n%s'
                      % (' '.join(command), (result.stderr or result.stdout)[:3000]))
    layouts = {}
    for block in re.finditer(r'Type: (?:class|struct) (\w+)\s*\n\s*Layout: <ASTRecordLayout(.*?FieldOffsets: \[[0-9, ]*\]>)',
                             result.stdout, re.S):
        size = re.search(r'Size:(\d+)', block.group(2))
        offsets = re.search(r'FieldOffsets: \[([0-9, ]*)\]', block.group(2))
        if not size or offsets is None:
            raise Problem('the layout of %s was not understood' % block.group(1))
        layouts[block.group(1)] = (int(size.group(1)) // 8,
                                   [int(v) // 8 for v in offsets.group(1).split(',') if v.strip()])
    return layouts


def leaves(client, name, hint, layouts, classes, base=0, path=''):
    """Every scalar of a class, flattened: (byte offset, size, sign, designator, type)."""
    if name not in classes:
        classes[name] = find_class(client, name, hint)
    _, (parent, members) = classes[name]
    out = []
    if parent:
        out += leaves(client, parent, hint, layouts, classes, base, path)
    if name not in layouts:
        raise Problem('the compiler did not lay out %s' % name)
    size, offsets = layouts[name]
    if len(offsets) != len(members):
        raise Problem('%s: the compiler lays out %d members, the header declares %d'
                      % (name, len(offsets), len(members)))
    for (member, type_, dims, commented), offset in zip(members, offsets):
        if commented != offset:
            raise Problem('%s::%s is commented at 0x%X and laid out at 0x%X'
                          % (name, member, commented, offset))
        count = 1
        for d in dims:
            count *= d
        if type_ in SCALARS:
            width, sign = SCALARS[type_]
            for i in range(count):
                suffix = '[%d]' % i if dims else ''
                out.append((base + offset + i * width, width, sign, path + member + suffix, type_))
        else:
            if type_ not in layouts:
                raise Problem('%s::%s is a %s the compiler did not lay out' % (name, member, type_))
            stride = layouts[type_][0]
            for i in range(count):
                suffix = '[%d]' % i if dims else ''
                out += leaves(client, type_, None, layouts, classes, base + offset + i * stride,
                              path + member + suffix + '.')
    return out


# --- the table ------------------------------------------------------------------------------

def read_words(path):
    """words.txt: the client words the code names spell as several, CLIENT WORD -> Spelling."""
    words = {}
    for number_, line in enumerate(read(path).split('\n'), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 2 or not re.match(r'^[A-Z]+$', parts[0]):
            raise Problem('%s:%d: a line is a client word in capitals and its spelling' % (path, number_))
        if not re.match(r'^[A-Z][A-Za-z]*$', parts[1]):
            raise Problem('%s:%d: %r is not a spelling a code name can take' % (path, number_, parts[1]))
        if parts[0] in words:
            raise Problem('%s:%d: %s is spelled twice' % (path, number_, parts[0]))
        words[parts[0]] = parts[1]
    return words


def spell(part, words, used):
    """One word of a client name as a code name writes it: words.txt's spelling, or capitalised."""
    letters = part.rstrip('0123456789')
    if letters in words:
        used.add(letters)
        return words[letters] + part[len(letters):]
    return part[0].upper() + part[1:].lower()


def ident(name, prefixes, words, used):
    for prefix in prefixes:
        if name.startswith(prefix) and len(name) > len(prefix):
            name = name[len(prefix):]
            break
    parts = [p for p in name.split('_') if p]
    out = ''
    for i, part in enumerate(parts):
        if part.isdigit() and i and parts[i - 1].isdigit():
            out += '_'
        out += spell(part, words, used)
    return out


def value_kind(kind, name, desc_type, count, scalars):
    """The value of one field from its descriptor type and the members it covers."""
    signs = {s[2] for s in scalars}
    types = {s[4] for s in scalars}
    if desc_type == TYPE_LONG:
        if count % 2:
            raise Problem('%s %s: a 64-bit field of an odd %d dwords' % (kind, name, count))
        if types <= {'WOWGUID'}:
            return 'guid'
        if types <= {'uint64_t'}:
            return 'u64'
        return 'guid'
    if desc_type == TYPE_FLOAT:
        if signs != {'f'}:
            raise Problem('%s %s: a float field over %s' % (kind, name, sorted(types)))
        return 'f32'
    if desc_type == TYPE_INT:
        if signs == {'s'} and all(s[1] == 4 for s in scalars):
            return 'i32'
        if 'f' in signs:
            raise Problem('%s %s: an integer field over %s' % (kind, name, sorted(types)))
        return 'u32'
    if desc_type == TYPE_SHORTS:
        halves = [s for s in scalars if s[1] == 2]
        if halves and all(s[2] == 's' for s in halves):
            return 'i16x2'
        return 'u16x2'
    if desc_type in (TYPE_BYTES, TYPE_BYTES2):
        return 'bytes'
    raise Problem('%s %s: descriptor type %d is not known' % (kind, name, desc_type))


def mirrored(client, kind_spec, descriptor_names):
    """The kind-relative dwords the client copies before an update: (any, local)."""
    lists = []
    for file, qualified in kind_spec[6]:
        address, items = uint_array(read(os.path.join(client, 'WoW/Source/Object', file)), qualified)
        values = []
        for item in items:
            values.append(descriptor_names[item] if item in descriptor_names else number(item))
        lists.append((qualified, address, values))
    return lists


def build(client, clang_cl, xwin, words):
    used = set()
    mirror_cpp = os.path.join(client, 'WoW/Source/ObjectMgrClient/Mirror.cpp')
    arrays = descriptor_arrays(mirror_cpp)
    counts_address, counts, active_end, others_end = dword_counts(client)
    if len(counts) != len(KINDS):
        raise Problem('s_objDwordCount has %d entries for %d kinds' % (len(counts), len(KINDS)))

    classes = {}
    for spec in KINDS:
        classes[spec[4]] = find_class(client, spec[4], spec[5])
    classes['CGPlayerData'] = find_class(client, 'CGPlayerData', 'Player.h')
    # Nested element types are found by name as the walk meets them; declare them up front so
    # the measuring unit includes their headers.
    for name in ('PlayerQuestLogEntry', 'PlayerSkillInfo', 'ItemEnchantment', 'VisibleItem_C'):
        classes[name] = find_class(client, name, None)
    layouts = measure(client, classes, clang_cl, xwin)

    names_by_kind = {}
    kinds = []
    fields = []
    watches = []
    evidence = {'descriptors': {}, 'counts': (counts_address, counts), 'mirrors': []}
    for type_id, spec in enumerate(KINDS):
        kind, wxl_type, parent, array, block, header, _, prefixes = spec
        address, descriptors = arrays[array]
        evidence['descriptors'][array] = (address, descriptors)
        first = counts[KINDS.index(next(k for k in KINDS if k[0] == parent))] if parent else 0
        end = counts[type_id]
        block_size, _ = layouts[block]
        if block_size != (end - first) * 4:
            raise Problem('%s: %s is %d dwords, s_objDwordCount gives %d'
                          % (kind, block, block_size // 4, end - first))
        scalars = leaves(client, block, header, layouts, classes)
        at = {}
        for scalar in scalars:
            at.setdefault(scalar[0], []).append(scalar)
        ends = sorted({s[0] for s in scalars})

        kind_others = end
        if kind == 'Player':
            if active_end != end:
                raise Problem('GetNumDwordBlocks gives %d dwords for the active player, '
                              's_objDwordCount %d' % (active_end, end))
            remote_size = layouts['CGPlayerData'][0]
            if first + remote_size // 4 != others_end:
                raise Problem('CGPlayerData ends at dword 0x%X, GetNumDwordBlocks at 0x%X'
                              % (first + remote_size // 4, others_end))
            kind_others = others_end
        kinds.append((kind, wxl_type, parent or 'None', first, end, kind_others))

        descriptor_names = {name: off for name, off, _, _, _ in descriptors}
        lists = mirrored(client, spec, descriptor_names)
        evidence['mirrors'] += [(q, a, v) for q, a, v in lists]
        any_set, local_set = set(), set()
        if lists:
            any_set = {v for v in lists[0][2] if v < end - first}
        if len(lists) > 1:
            local_set = {v for v in lists[1][2] if v < end - first}
        for scope, dwords in (('any', any_set), ('local', local_set)):
            run = None
            for d in sorted(dwords) + [None]:
                if run and d == run[1]:
                    run[1] += 1
                    continue
                if run:
                    watches.append((kind, first + run[0], first + run[1], scope))
                run = [d, d + 1] if d is not None else None

        cursor = 0
        idents = {}
        for name, off, count, desc_type, flags in descriptors:
            if off != cursor:
                raise Problem('%s %s starts at dword %d, the previous field ended at %d'
                              % (kind, name, off, cursor))
            cursor = off + count
            if (off * 4) not in at:
                raise Problem('%s %s: no member of %s starts at byte 0x%X'
                              % (kind, name, block, off * 4))
            covered = [s for s in scalars if off * 4 <= s[0] < (off + count) * 4]
            value = value_kind(kind, name, desc_type, count, covered)
            element = 2 if value in ('guid', 'u64') else 1
            classes_seen = set()
            for e in range(off, off + count, element):
                dwords = set(range(e, e + element))
                if dwords <= any_set:
                    classes_seen.add('any')
                elif dwords <= local_set:
                    classes_seen.add('local')
                else:
                    classes_seen.add('none')
            watch = classes_seen.pop() if len(classes_seen) == 1 else 'some'
            fields.append([kind, name, None, first + off, count, value, flags, watch, prefixes])
            names_by_kind.setdefault(kind, []).append(len(fields) - 1)
            idents[name] = ident(name, prefixes, words, used)
        if cursor != end - first:
            raise Problem('%s: the descriptors cover %d dwords, the block holds %d'
                          % (kind, cursor, end - first))

        # A code name two fields of the kind would share keeps more of its client name:
        # PLAYER_BYTES is Bytes and PLAYER_FIELD_BYTES FieldBytes.
        taken = {}
        for name, code in idents.items():
            taken.setdefault(code, []).append(name)
        for code, owners in taken.items():
            if len(owners) > 1:
                for owner in owners:
                    shorter = [p for p in prefixes if owner.startswith(p)]
                    idents[owner] = ident(owner, [kind.upper() + '_'] if shorter else [], words, used)
        final = list(idents.values())
        if len(set(final)) != len(final):
            raise Problem('%s: two fields share a code name: %s'
                          % (kind, sorted({c for c in final if final.count(c) > 1})))
        for i in names_by_kind[kind]:
            fields[i][2] = idents[fields[i][1]]
            if not re.match(r'^[A-Z][A-Za-z0-9_]*$', fields[i][2]):
                raise Problem('%s %s: the code name %r is not an identifier'
                              % (kind, fields[i][1], fields[i][2]))
    unused = sorted(set(words) - used)
    if unused:
        raise Problem('words.txt spells %s, which no field name holds; remove the line' % ', '.join(unused))
    return kinds, fields, watches, evidence


def render(kinds, fields, watches):
    lines = [HEADER]
    for kind, wxl_type, parent, first, end, others in kinds:
        own = [f for f in fields if f[0] == kind]
        lines.append('')
        lines.append('// --- %s: dwords 0x%04X..0x%04X, %d fields %s'
                     % (kind, first, end - 1, len(own), '-' * max(0, 60 - len(kind))))
        lines.append('WXL_UPDATE_FIELD_KIND(%s, %s, %s, 0x%04X, 0x%04X, 0x%04X)'
                     % (kind, wxl_type, parent, first, end, others))
        for f in own:
            lines.append('WXL_UPDATE_FIELD(%s, %s, %s, 0x%04X, %d, %s, 0x%03X, %s)'
                         % (f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]))
        for w in [w for w in watches if w[0] == kind]:
            lines.append('WXL_UPDATE_FIELD_WATCH(%s, 0x%04X, 0x%04X, %s)' % w)
    lines.append(FOOTER)
    return '\n'.join(lines)


# --- the binary -----------------------------------------------------------------------------

class Image:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        pe = struct.unpack_from('<I', self.data, 0x3C)[0]
        if self.data[pe:pe + 4] != b'PE\0\0':
            raise Problem('%s is not a PE image' % path)
        sections = struct.unpack_from('<H', self.data, pe + 6)[0]
        optional = struct.unpack_from('<H', self.data, pe + 20)[0]
        self.base = struct.unpack_from('<I', self.data, pe + 24 + 28)[0]
        stamp = struct.unpack_from('<I', self.data, pe + 8)[0]
        if stamp != 0x4C2452FE or self.base != 0x400000:
            raise Problem('%s is not Wow.exe 3.3.5a build 12340' % path)
        self.sections = []
        table = pe + 24 + optional
        for i in range(sections):
            entry = table + 40 * i
            vsize, va, rsize, raw = struct.unpack_from('<IIII', self.data, entry + 8)
            self.sections.append((va, max(vsize, rsize), raw, rsize))

    def at(self, va, size):
        rva = va - self.base
        for start, length, raw, rsize in self.sections:
            if start <= rva < start + length:
                if rva - start + size > rsize:
                    raise Problem('0x%08X is not backed by the file' % va)
                return self.data[raw + rva - start: raw + rva - start + size]
        raise Problem('0x%08X is in no section' % va)

    def text(self, va):
        out = b''
        while True:
            c = self.at(va + len(out), 1)
            if c == b'\0':
                return out.decode('ascii', 'replace')
            out += c


def check_binary(path, evidence):
    image = Image(path)
    checked = 0
    for array, (address, rows) in evidence['descriptors'].items():
        for i, (name, off, count, type_, flags) in enumerate(rows):
            ptr, b_off, b_count, b_type, b_flags = struct.unpack('<IIIII', image.at(address + 20 * i, 20))
            got = (image.text(ptr), b_off, b_count, b_type, b_flags)
            if got != (name, off, count, type_, flags):
                raise Problem('%s[%d] at 0x%08X: the fork has %s, Wow.exe %s'
                              % (array, i, address + 20 * i, (name, off, count, type_, flags), got))
            checked += 1
    address, counts = evidence['counts']
    got = list(struct.unpack('<%dI' % len(counts), image.at(address, 4 * len(counts))))
    if got != counts:
        raise Problem('s_objDwordCount at 0x%08X: the fork has %s, Wow.exe %s' % (address, counts, got))
    for qualified, address, values in evidence['mirrors']:
        got = list(struct.unpack('<%dI' % len(values), image.at(address, 4 * len(values))))
        if got != values:
            raise Problem('%s at 0x%08X differs from the fork' % (qualified, address))
    return checked, len(evidence['mirrors'])


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    core = os.path.dirname(os.path.dirname(HERE))
    parser.add_argument('--client', default=os.path.join(os.path.dirname(core), 'client'))
    parser.add_argument('--out', default=os.path.join(os.path.dirname(core), 'sdk', 'include',
                                                      'wxl', 'update_fields.def'))
    parser.add_argument('--binary', default=os.environ.get('WXL_WOW_EXE'))
    parser.add_argument('--clang-cl', default=default_clang_cl())
    parser.add_argument('--xwin', default=os.environ.get('WXL_XWIN') or
                        ('/opt/xwin' if os.path.isdir('/opt/xwin') else None))
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--quiet', action='store_true')
    args = parser.parse_args()

    if not os.path.isfile(os.path.join(args.client, 'WoW/Source/ObjectMgrClient/Mirror.cpp')):
        print('generate_update_fields: %s is not a client checkout' % args.client, file=sys.stderr)
        return 2
    if not args.clang_cl:
        print('generate_update_fields: no clang-cl to measure the descriptor blocks with; '
              'name one with --clang-cl', file=sys.stderr)
        return 2
    try:
        words = read_words(os.path.join(HERE, 'words.txt'))
        kinds, fields, watches, evidence = build(args.client, args.clang_cl, args.xwin, words)
        text = render(kinds, fields, watches)
        binary = check_binary(args.binary, evidence) if args.binary else None
    except Problem as problem:
        print('generate_update_fields: %s' % problem, file=sys.stderr)
        return 1

    summary = ', '.join('%s %d' % (k[0], sum(1 for f in fields if f[0] == k[0])) for k in kinds)
    if args.check:
        current = read(args.out) if os.path.isfile(args.out) else ''
        if current.replace('\r\n', '\n') != text:
            print('generate_update_fields: %s is not what the fork gives; run '
                  'core/tools/fields/generate_update_fields.py and review the diff' % args.out,
                  file=sys.stderr)
            return 1
    else:
        with open(args.out, 'w', newline='\n') as f:
            f.write(text)
    if not args.quiet:
        print('%s %s: %d fields (%s)' % ('checked' if args.check else 'wrote', args.out,
                                         len(fields), summary))
        for kind, _, _, first, end, others in kinds:
            print('  %-13s dwords 0x%04X..0x%04X%s' % (kind, first, end,
                  '' if others == end else ' (0x%04X for a player other than the active one)' % others))
        if binary:
            print('  Wow.exe agrees: %d descriptors, s_objDwordCount, %d mirror lists' % binary)
    return 0


if __name__ == '__main__':
    sys.exit(main())
