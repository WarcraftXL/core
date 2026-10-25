# seams: the wiring check

Two tools read the same table. `check_seams.py` compares the seam table in
`core/include/wxl/seams/*.def` with the client fork's sources and fails when the two disagree.
`check_conventions.py` compares the same table with `Wow.exe` 3.3.5a (12340) and fails when a row
names a convention the binary does not use; it is documented at the bottom of this file.

## Why it exists

A seam is declared once and realised twice.

The **x86** realisation is generated: `core/src/seams/AttachX86.cpp` expands every row of the table
into a detour in the row's calling convention (`core/src/seams/Detour.hpp`: a `__cdecl` or
`__stdcall` function, or a `__fastcall` shim standing in for a `__thiscall` member) and attaches it
at `WXL_SITE(symbol, 0)`. Nothing can drift, because nothing is written by hand -- a row that loses
its symbol fails the build in `WXL_SITE` at compile time, and a row that lacks its `ret` or `conv`
column does not expand at all.

The **x64** realisation is a pair of calls sitting inside an upstream function, in a fork that keeps
pulling from upstream. A merge that drops one of those lines is silent: the build succeeds, the
runtime starts, the seam logs that it is live, and the event simply never fires again. No error, no
log, nothing. That is the one failure mode this design has, and this tool is what turns it into a
build failure.

## What it checks

For every row of the seam table:

| | |
| --- | --- |
| **present** | the fork calls `wxl::seam::<Domain>::<Name>::Before`, and `::After`, at least once each |
| **once in** | exactly one `Before`: a function is entered once. `After` may repeat, one per return path |
| **in place** | every call sits inside the function the row's `symbol` column names, not another one |
| **arity** | the call passes as many arguments as the row declares parameters; `After` of a row with a return type takes the result first, so one more |
| **arguments** | argument *n* carries the row's parameter *n* by name; a `thiscall` row's first parameter is carried by `this` |
| **ABI** | every row has its six columns; `ret` is not empty; `conv` is `cdecl`, `thiscall`, `stdcall` or `regparm`; a `thiscall` row's first parameter is a pointer |

and, in the other direction:

| | |
| --- | --- |
| **declared** | a `wxl::seam::` call in the fork whose `<Domain>.<Name>` no row declares is an error |

The argument rule compares names, not types: argument *n*'s expression must contain the row's *n*th
parameter name, ignoring case and underscores. `WarcraftXL::Vec3(position)` carries `position`;
`entity->m_guid` carries `guid`; `this` carries a `thiscall` row's first parameter whatever the
row names it. The result argument of a returning seam's `After` is not checked by name: it is
whatever the function is about to return. It exists to catch a swapped pair, which arity alone would not --
the call would still compile if the two happened to be convertible. It is deliberately a wiring
check and not a type check: the compiler does types, and does them better. Its known limit is that
an unrelated expression which happens to spell the parameter's name passes, so it narrows the hole
rather than closing it.

The tool reads C++ with a brace-and-scope scanner, not a compiler: it blanks comments, literals and
preprocessor directives, then tracks namespace, class and function scopes to say which function a
call is in. It reports a file it could not read as a problem rather than passing it.

## Running it

```sh
python core/tools/seams/check_seams.py            # from the core checkout's parent
python core/tools/seams/check_seams.py -v         # list every seam and where it is wired
python core/tools/seams/check_seams.py --client <path to the fork> --seams <path to the tables>
```

Python 3, standard library only. It exits 0 when everything is wired, 1 with one line per problem
otherwise, and 2 when it was pointed at something that is not a checkout. By default it finds both
inputs from its own location: the seam tables in the core checkout above it, the fork in `client`
beside that checkout.

## In the build

`client/WarcraftXL/CMakeLists.txt` declares a `wxl-seam-check` target that runs the tool, and
`WorldClient` depends on it, so a 64-bit build of the client runs the check and fails on a
dropped call site. It is declared there and not in the fork's root `CMakeLists.txt` on purpose:
`core/tools/layouts/measure_layouts.py` parses that root file and refuses a condition it does not
recognise, so nothing conditional may be added to it.

If Python is not on the machine, the target is not declared and the build says so once. The check
is then the manual command above.

## What it does not prove

That a seam's body raises the right event with the right payload. That is a different question and
`core/tests/seam_harness.cpp` answers it: it links the seam bodies themselves against a real
`ModHost` whose raisable events are the ones the table derives, subscribes to each, calls every
seam's `Before` and `After`, and checks what arrived. Built for both architectures as
`wxl-core-seam-harness`, and registered with ctest as `seams`.

Between them: `WXL_SITE` proves the x86 address resolves, this tool proves the x64 calls are
where the table says, and the harness proves the body in the middle does what the row promises.
None of the three can prove a detour behaves correctly in a running client.

## Self-test

`test_check_seams.py` builds small synthetic trees in a temporary directory -- a correct one, and
one per failure mode -- and asserts the tool's exit code and the problem it names for each. It
needs nothing but Python:

```sh
python core/tools/seams/test_check_seams.py
```

# conventions: the binary check

`check_seams.py` cannot see a row whose `conv` column is wrong, because it never looks at
`Wow.exe`. A row that says `cdecl` about a function taking its context in EDI produces a detour
that clobbers EDI; the original then dereferences whatever was left there and the client dies on
the first frame. `check_conventions.py` closes that: it reads the function out of the binary and
compares what the instructions do with what the row claims. x86 only -- the x64 realisation is
direct source calls with no convention of its own.

Besides the seams, it reads two more tables of the same shape: the x86 points
(`include/wxl/points/x86.def`, generated) and the client functions the x86 binding calls by hand
(`include/wxl/core/binding/calls_x86.def`, `WXL_BINDING_CALL_X86(domain, name, symbol, ret, conv,
params)`), whose rows declare the convention `BindingX86.cpp` calls them with. `--points` and
`--calls` name others; by default both are read when `--seams` is core's own.

## What it decides, per row

It resolves the row's `symbol` column against `include/wxl/offsets/generated/functions.def`, the
way `WXL_SEAM_SITE` does, including the `"<name>@0x<address>"` form, then reads the function:

| | |
| --- | --- |
| **register arguments** | a register read before it is written is one the function was handed. `eax`, `ecx` and `edx` count only before the first `call`, after which they hold whatever a callee left; `ebx`, `esi` and `edi` count anywhere, except where a `push` is matched by a `pop` and is therefore a save. An instruction whose result does not depend on the register's old value writes it without reading it: `xor r, r`, `sub r, r`, `sbb r, r`, `and r, 0`, `or r, -1`, and the three-operand `imul r, src, imm` |
| **thiscall** | `ecx` is the only register argument |
| **stack arguments** | the widest `[ebp+N]` the function reaches, or `[esp+N]` tracked against the entry stack pointer when it has no frame, and the amount its `ret` pops, whichever is larger |
| **who cleans** | `ret N` means the function cleans N bytes, so `stdcall` or `thiscall`; `ret` with arguments on the stack means the caller cleans, so `cdecl` |
| **the call sites** | every direct `call` to the function, then the enclosing function decoded from its entry: the pushes before the call and an `add esp, N` after it |
| **the witnesses** | the call sites the row declares, read the same way (below) |
| **the thunks** | a `regparm` row has a thunk in `src/seams/RegparmX86.cpp`, a thunk belongs to a `regparm` row, and the registers the thunk restores before calling the original are the ones the function reads |

A row's declared parameters are measured in 4-byte slots, so a `uint64_t` counts twice and a
`thiscall` row's first parameter is `this` in ECX rather than a slot. Every verdict is printed
with the addresses it was read from, so the claim can be checked rather than believed.

Two things it is careful about, because both mislead by hand:

* **MSVC elides `add esp, N`.** `World.Preload` is cleaned explicitly at `0x0074B64A` and not at
  all at `0x0071375A`, where the compiler folded the cleanup into a later adjustment. No cleanup
  at one site proves nothing, so the tool reads further sites until one cleans, and a cleanup
  *larger* than the call needs is treated as a fold rather than a disagreement. The verdict rests
  on the function's own `ret`; the call sites corroborate it.
* **A linear decode trusts wherever it is pointed.** Every decode starts on an address known to
  be an instruction boundary -- a function entry, never a byte before a call -- and stops at the
  switch table a `jmp dword ptr [reg*4 + 0x...]` points into, which otherwise decodes as
  plausible instructions reading registers the function never takes.

## Witnesses

Some functions do not say what they take. A handler reached only through a table of pointers may
read none of the arguments its dispatcher pushes, and nothing calls it directly; a function that
leaves by a tail `jmp` has no `ret` to say who cleans. For those, the row names a call site that
shows it, in the seam table right after the row:

```cpp
WXL_SEAM_BEFORE(Player, OnUpdateMoney, "OnUpdateMoney", int32_t, cdecl, (uint64_t guid, uint32_t offset, uint32_t count, const void* value, void* param))
WXL_SEAM_WITNESS(Player, OnUpdateMoney, 0x004D5254)    // CallMirrorFunctions: call ecx; add esp, 0x18
```

The address is the `call` instruction itself, direct or through a pointer, inside a function the
offsets table tags. The tool decodes that function from its entry and reads the witness like any
call site, with two allowances a found one does not get, because a person has pointed at it:

* the pushes are counted across a direct call to a function that takes its arguments in
  registers and reads nothing on the stack (`0x004D523E` above, which takes EAX);
* a cleanup larger than the pushes is accounted for when the call just before them is to a
  function its caller cleans for and that reads exactly the difference.

A witness proves a call its caller cleans for, so it belongs to a `cdecl` row. It must push
exactly the row's stack dwords and take them back; then it settles a convention the function's
own code leaves open, and it stands for the parameters a handler never reads. A witness that
cannot be read, does not add up, or hands the function another count fails the row: it is
evidence, never an exemption.

## The binary

The default backend reads `Wow.exe` from disk and decodes it with `llvm-objdump` (any recent
LLVM; `--objdump` names one that is not on PATH). The direct calls to a function are found by
scanning `.text` for `E8`/`E9 rel32` that land on it. Name the executable with `--binary`, or
once with `WXL_WOW_EXE`:

```sh
export WXL_WOW_EXE=/path/to/WoW-3.3.5a/Wow.exe
```

It must be the stock client, 3.3.5a build 12340 (7 704 216 bytes, as the 3.3.5a installer and
patches leave it). The tool checks the PE timestamp (`0x4C2452FE`), the image base, the bounds of
`.text`, and the SHA-256 of `.text` (`0dce76d7...`), and refuses anything else: an executable
whose code was patched -- a byte patch, a crack, a loader that rewrites `.text` -- answers for the
patch, not for the client the seams attach to. A copy patched by `wxl-core-patcher` is accepted:
the patcher adds sections and edits headers but leaves `.text` as it was, and only `.text` is
read. The `Wow.exe.orig` the patcher writes next to it is the stock one, too.

A ghidra MCP can answer instead (`--backend mcp`): it asks the ghidra module of
https://mcp.kiji.dev with the bearer token in `KIJI_MCP_TOKEN` (a key granted that module), or
another endpoint given with `--mcp`. Its answers can be recorded with `--cache FILE` and replayed
with `--offline`; the local backend needs neither.

## Running it

```sh
python core/tools/seams/check_conventions.py             # from the core checkout
python core/tools/seams/check_conventions.py -v          # what the binary says about every row
python core/tools/seams/check_conventions.py --json      # the same as data
python core/tools/seams/check_conventions.py --point Event.DispatchUpdate
python core/tools/seams/check_conventions.py --binary Wow.exe
python core/tools/seams/check_conventions.py --backend mcp --cache mcp.json   # then --offline
```

Python 3, standard library only, and llvm-objdump. It exits 0 when every row agrees, 1 with one
line per disagreement otherwise, and 2 when it cannot start -- a missing offsets table, no
binary, a binary that is not the stock 12340, no llvm-objdump, or an MCP that does not answer,
which it names rather than crashing on. A full run of the table takes about a second.

## What it does not prove

That the registers of a `regparm` row carry the parameters in the order the row lists them. The
tool checks that the set of registers matches the thunk and that the counts add up; which one is
`object` and which is `create` is read from the callers, and `RegparmX86.cpp` records that above
each thunk with the addresses it was read from. It also cannot settle a function that nothing
calls directly and that takes nothing on the stack: with no arguments, `cdecl` and `stdcall` are
the same machine code, and the tool says so instead of guessing. Reading a witness's push count
across a call relies on that call taking nothing on the stack because its target reads nothing
there; the witness is declared by hand so that a person has looked.

## Self-test

`test_check_conventions.py` has three halves. It serves a stub binary over a stub MCP --
synthetic functions whose convention is known because the test wrote their instructions -- and
asserts the tool's exit code and the problem it names, once per rule. It takes the local backend
apart: the objdump parser on every line shape it reads, the instruction rules, the PE reader, the
reference scan and the stock check on small images it writes, and the refusals of the command
line. Then it points the tool at the real `Wow.exe` and compares every row against the
conventions verified by hand against the disassembly, skipping that half when no binary is named:

```sh
python core/tools/seams/test_check_conventions.py                  # with $WXL_WOW_EXE set
python core/tools/seams/test_check_conventions.py --binary Wow.exe
```
