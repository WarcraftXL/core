# offsets: derived address tables

`extract_offsets.py` reads the `client` fork (the C++ decompilation of Wow.exe 3.3.5a, build
12340) and turns its address tags into the X-macro tables under `core/offsets/`. The fork is the
single source of truth for addresses; nothing in `core/offsets/` is written by hand.

The tags it reads sit directly above the thing they describe:

```cpp
// VERIFY: 271a03c4 manual
// OFFSET: 0x0068FA60
void CGxDeviceD3d::IBufUnlock(CGxBuf* buf) {

// STATIC: 0x00C60930
static BLIT_FUNCTION s_blits[BlitFormats_Last][BlitFormats_Last][BlitAlphas_Last];

// OFFSET: [0x0077A2B0<SpellRec>, 0x0077A310<ItemDisplayInfoRec>, 0x0077A370]
template <class T>
void WowClientDB<T>::Load() {
```

## Outputs

| file | content |
| --- | --- |
| `functions.def` | `WXL_OFFSET_FUNCTION(name, address, bound, length, verify, targs, file, line)`, one row per function address; a template tag with N addresses gives N rows |
| `statics.def` | `WXL_OFFSET_STATIC(name, address, file, line)`, one row per static address |
| `unparsed.txt` | every `OFFSET:`/`STATIC:` tag the parser could not confidently name, with file, line, reason and the source line |
| `summary.txt` | counts per tag kind and per tree, headers versus sources, the reconciliation, VERIFY manual/auto counts, template histogram, the image's sections and the function extents (bound distribution, sentinels, how many lengths were settled and the cross-check), duplicate addresses, names with several addresses, INLINED tags |

Names are the qualified C++ names as written in the fork, with enclosing namespaces and classes
applied (`FMOD::DiskFile::DiskFile` for an in-class definition, `CmdLineGetString::buffer` for a
function-local static). Text columns are C string literals; paths are relative to the client
checkout with forward slashes. Rows are sorted by address, then name, file and line, so two runs
on the same input give byte-identical files and a run after an upstream pull gives a reviewable
diff.

## Function extents: `bound` and `length`

A hand-written constant is often not a function start but a site inside one: a return address,
a branch to patch, an immediate. Such a site is only expressible as `symbol + delta`, and the
delta is only checkable if the table knows how far the symbol extends. Two columns serve that,
and they do not mean the same thing.

`bound` is the distance from the function's address to the next tagged address in the same
section of the original image, functions and statics together, each distinct address counted
once (so an address carrying 94 folded names is one boundary, and all 94 rows share the bound).
It is an **upper bound** on the function's size, not its size: the decompilation does not cover
the whole binary, and untagged code may sit between the two addresses. A large bound means a
large untagged gap follows (`WinMain` has a 72 KB bound because the CRT follows it), not a large
function. `0` is a sentinel for the last tagged address of a section; the summary counts them.

`length` is the function's decoded size. `x86_sweep.py` decodes the bytes from the start
(length-only x86 decoding, stdlib) and stops at the `int3` padding the linker puts before the
next function, reading switch jump tables from the image instead of decoding them as code, and
using the ret/jmp structure to accept a function that ends exactly on a 16-byte boundary with
no padding when the next tagged address follows directly. `0` means the sweep did not settle the
end: a ret/jmp on a 16-byte boundary with code (not padding) after it, which the bytes alone
cannot split between the function's own exception funclet and an untagged neighbour, or an
opcode the decoder does not know. The summary says how many of each. When both are known,
`length <= bound` and the tool refuses to write otherwise.

The sweep is checked against the image: for every decoded length, the bytes up to the next
tagged address must be padding, or decode as a chain of whole padded functions that lands on
the successor. The summary reports the three outcomes; a chain that stops right at a decoded
end would mean the sweep cut a function (none does today).

The image is `client/game/base/Wow.exe`, found through `--image`, then `$WXL_CLIENT_ROOT`, then
the first ancestor directory of the tool that contains it. Its PE section headers give the
section ranges; the summary lists them with how many addresses fall in each. **Without the
image** the tool still runs: every function address is treated as one code section, the bound
is the distance to the next tagged function whatever its section, every length is `0`, and the
summary says so in capitals. Do not commit tables produced that way.

## Running it

```
python core/tools/offsets/extract_offsets.py <path to the client checkout> [--image <Wow.exe>]
```

Python 3, standard library only (`x86_sweep.py` next to the script is part of the tool). The
output directory defaults to `core/offsets/`; `--out <dir>` writes elsewhere (useful to compare
against the committed tables). The client checkout and the image are only read. The summary is
printed; `--quiet` silences it. A run takes about ten seconds.

## When to re-run

After every pull of the fork: new functions get tagged, addresses get corrected, names get
renamed. Re-run with the image available, read `summary.txt` (the reconciliation, the extent
cross-check, the unaligned addresses) and the diff of the two `.def` files, and commit the
generated files together with the tool version that produced them. A new tag inside a gap
shrinks the bounds of its neighbours; that is expected and is the table getting tighter.

## The invariant

Every `OFFSET:` and `STATIC:` tag ends up either as rows or as an entry in `unparsed.txt`, and
the tool refuses to write anything if the totals do not reconcile. A table that silently omitted
what it did not understand would be worse than no table, because nobody would ever find the hole.

## What to do with unparsed.txt

The file should be empty (header lines only). If an entry appears after a pull, the fork grew a
shape the parser does not know. Open the file and line, see what the declaration looks like, and
either teach `parse_function_signature` / `parse_static_declaration` the shape or fix the source
in the fork. Do not hand-edit the `.def` files around it: the next run would undo that.

## How core consumes the tables

The `.def` files are data, not interfaces. One translation unit in `core` defines
`WXL_OFFSET_FUNCTION` / `WXL_OFFSET_STATIC` to append to a lookup table (name to address and
address to name), includes the two files once, and exposes a lookup API; nothing else includes
them. They are 26k and 5k rows, so including them from a header would be paid in every
translation unit. That consumer is not part of this tool.

The one exception is `core/offsets/site.hpp`, which turns a mid-function site into a checked
constant at compile time:

```cpp
#include <wxl/offsets/site.hpp>   // however core/offsets/ is mounted on the include path

// The call site inside CGWorldFrame::HitTest where the result is written back.
constexpr uintptr_t kHitTestWriteback = WXL_SITE("CGWorldFrame::HitTest", 0x3C);

// A name carried by several addresses (an overload, a template instantiation, the two
// destructors of CSimpleTexture) is picked by its address.
constexpr uintptr_t kTexDeleteFlagTest = WXL_SITE_AT("CSimpleTexture::~CSimpleTexture", 0x00483010, 0x4D);
```

The build fails, with a message naming the symbol, when the name is not in `functions.def`,
when `WXL_SITE` is given a name that has several addresses, or when the delta reaches the
function's extent (`length`, or `bound` when the length is not settled). A symbol that vanishes
after a pull, or a delta that was never inside the function, therefore stops the build rather
than becoming a silently wrong address; a delta that lands in untagged code right after the
function is within the bound and is not caught. The name must be a string literal. The header
pulls the whole `functions.def` into a `constexpr` array (about 2.5 s of front-end time per
translation unit with MSVC, nothing in the object file), so include it only from the few files
that define sites. It is a C++20 header (`consteval`).

## Out of scope

Struct field layouts. Those come from compiling the fork's headers 32-bit and dumping `offsetof`,
a different mechanism: `core/tools/layouts/`, which writes `layouts.def` next to these tables.
