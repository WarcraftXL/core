# Architecture

How WarcraftXL is put together, and which files a change touches. The code is the reference; this
page is the map.

## The three repositories

| Repository | License | What it is |
|---|---|---|
| `sdk/` | MIT | what a mod is written against: the C ABI (`include/wxl/*.h`), the C++ layer (`sdk.hpp`), the mod host every runtime links (`host/`), the shared half of `wxl.object` (`runtime/`) |
| `core/` | MIT | `WarcraftXL.dll`, the runtime, built for x86 and x64 from one source |
| `client/` | upstream's | the recompiled 3.3.5a client, with the calls into the runtime added in its source |

There are two runtimes and one body of logic:

| | x86 | x64 |
|---|---|---|
| loads into | the stock `Wow.exe` 3.3.5a (12340) | the recompiled client |
| starts at | an entry trampoline the patcher adds | the client calling `WarcraftXLInit` |
| reaches a client function by | a MinHook detour at an address resolved by name (seams, `wxl.hook`); a SafetyHook detour armed while a point has handlers (`wxl.point`) | a call the client's source makes; a `WXL_POINT_<n>` line testing one byte (`wxl.point`) |
| reads the client by | memory reads at offsets measured on the fork | accessors the client hands over at init |

Everything above that line is shared: the seam bodies, the services, the mod host.

## The path of an event

```
client function
  -> x86: detour (src/seams/AttachX86.cpp, Detour.hpp, RegparmX86.cpp)
     x64: call in the fork's source (client/.../*.cpp -> wxl::seam::<Domain>::<Name>::Before)
  -> seam body                        src/seams/<Domain>.cpp
  -> reads of the client              wxl::core::binding::<fact>   (src/binding/)
  -> ModHost::Raise                   sdk/host/src/mod_host.cpp
  -> EventBus::Emit                   sdk/host/src/event_bus.cpp, each handler under the crash guard
  -> the family thunk in the mod      sdk/include/wxl/sdk.hpp (Thunk<...>::Dispatch), which reads
                                      the args struct into the handler's parameters
  -> Script::On<Event>(...)           the mod's override
```

## The path of a point

```
client function, a row of include/wxl/points/points.txt
  -> x86: the SafetyHook detour armed at the first handler (src/points/PointsX86.cpp, DetourOf in
          DetourX86.hpp), one link of the address's chain in src/hook/Engine.cpp, in front of any seam
     x64: the WXL_POINT_<n>(...) line at the head of the function (client/WarcraftXL/Points.gen.h),
          which tests wxl::point::g_armed[n] and, set, calls wxl_core_point_enter
  -> points::Enter                    src/points/Points.cpp: the arguments by address, the answer's slot
  -> Before handlers, the Replace chain (call_original walks it down to the original), After
     handlers, each under the crash guard as its mod
  -> the original                     x86: the chain's next link, then the trampoline;
                                      x64: the function again, through a qualified call, bypassed once
```

A point the stock client inlined (a `deinlined` line of points.txt) has no address to detour.
On x86 it is reached through each function it was inlined into, its parents (a point of its own
or a function of Wow.exe that is no point), all put in place at its first handler:

```
parent's address in Wow.exe
  -> the parent's chain: its own point's detour (if it is a point) and any seam, in front
  -> the last link (priority INT_MAX), present while the de-inlined point has handlers: the
     parent recompiled from the fork (src/deinlined/<Parent>.gen.cpp, written by
     tools/extract/extract.py --deinlined), calling Wow.exe's other functions at their addresses
  -> the de-inlined function, a real one in that body: one byte tested
     (deinlined::Armed), and while set, deinlined::Enter -> points::Enter as above, the body
     of the function as the original
```

## The tables

Most of the code is generated from twelve tables. Each row is one fact; the table is the only
place it is written.

| Table | One row is | What the row generates |
|---|---|---|
| `core/include/wxl/seams/*.def` (`seams.def` includes them) | a client function the runtime hooks | `seams.h`: the declarations of the entry points its shape declares, `Before`, `After` or `Gate` (and, through `client/WarcraftXL/Seams.h`, what the fork calls); `AttachX86.cpp`: the x86 detour and its attach; `Table.cpp`: the seam list; `WXL_SEAM_RAISES` rows: the events the runtime announces as raisable |
| `core/include/wxl/core/binding/*.def` (`binding.def` includes them) | a fact read out of the client | `Binding.hpp`: `wxl::core::binding::<name>()`; `init.h`: a member of `wxl_core_init` and a line of `wxl_core_init_rows`; `BindingX86.cpp`: the x86 read (field rows); `client/WarcraftXL/Binding.cpp`: the x64 accessor (field rows); `BindingTable.cpp`: the x64 forwarder |
| `sdk/runtime/include/wxl/runtime/object_reads.def` | a getter of `wxl_object_api`: its kinds, who answers it (`field`, `client`, `cache`, `shared`), its C++ name and type, its doc, and (`WXL_OBJECT_SINCE`) the version that brought it | the `ObjectBinding` virtual of a `field`, `client` or `cache` row, the shared runtime's read of a `shared` row (`sdk/runtime/src/object_reads.cpp`), the service's wiring with its kind check, in core the read itself for a `field` row (`src/objects/Objects.hpp`); the C++ facades' members (`sdk/include/wxl/objects/generated/*.inc`) and the guides' tables of members, written by `sdk/tools/objects/gen_members.cpp` (`wxl-object-members`, checked by the sdk test `object-members`); the Lua method of the object userdata, refused to a mod whose abi is older; the editor types |
| `sdk/include/wxl/events.def` | an event of the contract, and (`WXL_EVENTS_SINCE`) the version that brought the events from an id on | the `wxl_event` enum, `EventArgs<>`, the `On<Event>` virtual of its script family, the trait that subscribes only overridden handlers, the event's entry in `wxl/reflect.hpp` with the version that brought it (`reflect::SinceOf`), which wxl.lua and the host refuse a subscription by |
| `sdk/include/wxl/enums.def` | a value of an enumeration an args member holds | `events.h`: the C enum; `sdk.hpp`: a `WXL_ARGS_ENUM` member's parameter type; `wxl/reflect.hpp`: the value's name, which `sdk/lua/` passes to a Lua handler as a string and its editor types list |
| `sdk/include/wxl/fields.def` | a member of an event's args struct | `events.h`: the struct itself; `sdk.hpp`: a parameter of the event's `On<Event>` and `wxl::Subscribe` handlers, in row order (a member that only measures another is carried by it); `wxl/reflect.hpp`: each event's members with their offset and kind, which a runtime other than C++ (a script VM) reads events through, `sdk/lua/` among them, which passes the same parameters to a Lua handler; `sdk/tests/reflect.cpp` checks the rows against the structs and the C++ parameters against the rows |
| `core/include/wxl/points/points.txt` | a point of `wxl.point`: an engine function mods attach to; a `deinlined` line names the point an inlined one is reached through on x86 | by `tools/points/generate_points.py`: `sdk/include/wxl/points.def` (the contract: the `wxl::points::<Point>` classes, the service's table), `include/wxl/points/x86.def` (the x86 detours, their sites, the de-inlined points with their parents, why a point has none), `sdk/include/wxl/deinlined.def` (the shared list of de-inlined points), `client/WarcraftXL/Points.gen.h` (the x64 macros); by `tools/points/insert_points.py`: the `WXL_POINT_<n>` lines in the fork; by `tools/extract/extract.py --deinlined`: each parent's recompiled body, `src/deinlined/<Parent>.gen.cpp` |
| `core/tools/engine/engine.txt` | an engine type mods read, in a group (one header per group), and `witness` lines (a function of Wow.exe that addresses a member) | by `tools/engine/generate_engine.py`, from the fork's headers measured by clang-cl for x86 and x64: `sdk/engine/include/wxl/engine/<group>.hpp` (the structs, one body per architecture, their `static_assert`s and layout hashes), `layouts.def` (sizes, hashes, member offsets: what `wxl.engine` answers from), `canon.def` (the hashed texts), `client/WarcraftXL/EngineLayouts.gen.cpp` (the fork's own types held to the x64 column) |
| `core/include/wxl/offsets/generated/` | an address or a layout of `Wow.exe` | the constexpr lookups behind `WXL_SITE`, `WXL_STATIC` and `WXL_FIELD`. Generated by `tools/offsets/` and `tools/layouts/`; never edited by hand |
| `sdk/include/wxl/update_fields.def` | a descriptor field of a kind of object: its client name, dword index, size, value, visibility, and whether the client sees it change | `wxl/objects/fields.hpp`: the `wxl::field::<Kind>::<Name>` constants and the table `wxl.object`'s field calls check ranges and watches against, which `sdk/lua/` names fields by (`obj:Field("Health")`, `wxl.field`). Generated by `core/tools/fields/` from the fork; never edited by hand |
| `core/include/wxl/core/client_dbs.def` | a global store of a client database: its name, record type, kind of store, file and row size | `client_dbs`, the binding row both sides realise from it (x86 by its `statics.def` address, x64 by the global itself), which `wxl.footprint` lists and `ClientDb.Initialize` raises `ClientDbLoaded` from. Generated by `tools/dbs/generate_client_dbs.py` from the fork; never edited by hand |
| `sdk/include/wxl/services/<service>.def` | a member of a service table (`wxl_<service>_api`), with the kinds it takes and answers, and whether a script and the C++ facade reach it generated, by a shim or by hand | `wxl/services/rows.hpp`: each member's C signature from its kinds, and the rows as data; `wxl/services/<service>.hpp`: the facade's plain methods (`StorageService::AddSearchPath`); `sdk/lua/`: the functions of the `wxl.<service>` modules, marshalled by kind, and their editor definitions; `sdk/tests/reflect.cpp` checks each row against its member and every member has one |

A consumer defines the macro for each row shape it cares about and includes the table. The header
of each table lists its shapes and columns.

## Changing things

### A new seam

1. A row in `core/include/wxl/seams/<domain>.def` (a new domain is a new file, included from
   `seams.def`), and a `WXL_SEAM_RAISES` row per event its body raises. The shape is the sides
   the body needs: `WXL_SEAM_BEFORE`, `WXL_SEAM_AFTER`, both with `WXL_SEAM`, or `WXL_SEAM_GATE`. A body that
   needs what only the recompiled client has adds a `WXL_SEAM_X64_ONLY` row saying why: the x86
   runtime attaches no detour for it and does not announce its events. No row needs one today:
   rebuild on x86 what the fork records, as `M2.DrawBatch` does, before reaching for it.
2. Its body in `core/src/seams/<Domain>.cpp`: the entry points the shape declares.
3. If its `conv` is `regparm`, a thunk in `core/src/seams/RegparmX86.cpp`.
4. The calls in the fork, for the sides the row declares: `Before` at the top of the function,
   `After` before every return, `Gate` as a test at the top. A call that is busy and whose body only
   raises one event goes inside `if (wxl::seam::Listening(WXL_EV_...))`; a service of the runtime
   whose work rides on such a seam holds that event's flag up while it needs the calls
   (`listening::RuntimeWants`, `src/runtime/Listening.hpp`), or the client would skip them while no
   mod listens. If the file's library is new to the seams, add it to `WXL_SEAM_LIBRARIES` in
   `client/WarcraftXL/CMakeLists.txt`.
5. A check in `core/tests/seam_harness.cpp` that drives the body.
6. A row of hand-verified ground truth in `core/tools/seams/test_check_conventions.py`, and a
   `WXL_SEAM_WITNESS` row when the function's own code does not show what it takes (see
   `core/tools/seams/README.md`).
7. A line in the seam table of `core/README.md`.

### A new point

1. A line at the end of `core/include/wxl/points/points.txt` (ids are positions): the function's
   x86 address from its `// OFFSET:` tag, or `-` for one the fork tags `// INLINED`; its qualified
   name; `replace` or `observe`; `main` or `any`; `provisional`; a note a modder needs.
2. `python tools/points/generate_points.py --client <fork> --sdk <sdk>` reads the definition from
   the fork, lowers its signature and writes the three tables. A parameter it cannot lower (a
   reference, a struct by value, an unknown scalar) is refused by name.
3. `python tools/points/insert_points.py --client <fork> --write` writes the line at the head of
   the function and the include of `WarcraftXL/Points.h`. A library of the fork new to the points
   goes in `WXL_POINT_LIBRARIES` in `client/WarcraftXL/CMakeLists.txt`.
4. `python tools/points/generate_points.py --client <fork> --coverage --binary Wow.orig.exe`: a
   point whose callers the 2008 compiler inlined shows fewer calls in the binary than sites in the
   source; each such copy becomes a `site` line (where it starts, where its arguments are).
5. `check_conventions.py` checks the x86 row with the seams; add a row of hand-read ground truth
   to `tools/seams/test_check_conventions.py`.
6. Raise the ABI feature in `sdk/include/wxl/abi.h`, as for any append.

### A de-inlined point

A function the stock client inlined everywhere, which a mod should reach on x86 too.

1. Its line in points.txt with address `-` (step 1 of a new point), and a `deinlined <name>
   <parent>` line under it for each function of Wow.exe that holds an inlined copy: a point with
   an x86 detour, or a function the fork tags with an address (`name(types)` when the fork
   overloads it), which then needs no point of its own.
2. `generate_points.py` as for any point: the x86 rows are `WXL_POINT_X86_DEINLINED`, one
   `WXL_POINT_X86_DEINLINED_IN` per parent and one `WXL_POINT_X86_PARENT` per parent function, and
   the shared list `sdk/include/wxl/deinlined.def` gains a line per parent.
3. `python tools/extract/extract.py --deinlined --client <fork> --binary Wow.orig.exe` writes the
   parent's recompiled body, `src/deinlined/<Parent>.gen.cpp`, and the head that enters the point
   in the de-inlined function. A body the tool cannot complete (a callee with no address, no
   `// INLINED` definition and no empty body) is refused by name. Add the file to
   `WXL_CORE_DEINLINED` in `CMakeLists.txt`. `check_conventions.py` then holds what the body calls
   and names to Wow.exe's own parent.
4. `wxl-core-stock-tests` (ctest `stock`): run the parent and its recompiled body on inputs of the
   function's own against a stock Wow.exe laid at its base, the client's surroundings stubbed at
   their addresses; both must answer and leave the same.
5. Raise the ABI feature for a new point; a parent added to a point already listed appends
   nothing to the contract.

### A new engine type

1. Its fork spelling under a group of `tools/engine/engine.txt` (a new group names its headers);
   a type it embeds must be listed too, or the member is bytes.
2. `python tools/engine/generate_engine.py --client <fork> --sdk <sdk> --binary Wow.orig.exe`. It
   refuses to write when a `/* NNNN */` comment of the fork, a row of
   `include/wxl/offsets/generated/layouts.def` or a `witness` line disagrees with the x86 layout.
3. Its row appears in `layouts.def`, so `wxl.engine` answers it on both runtimes; adding a type
   changes no other type's hash. Raise the ABI feature.

### A new fact about the client

1. A row in `core/include/wxl/core/binding/<domain>.def`.
2. A field row (`WXL_BINDING_FIELD*`) is generated on both sides. If x86 does not know the field
   yet, add it to `core/tools/layouts/layouts.txt` and rerun `measure_layouts.py`.
3. A call row (`WXL_BINDING`) is written by hand twice: in `core/src/binding/BindingX86.cpp` and in
   `client/WarcraftXL/Binding.cpp`. Either side missing fails to link. A client function the x86
   side calls gets a row in `include/wxl/core/binding/calls_x86.def` with the convention it is
   called with, which `check_conventions.py` holds against `Wow.exe` as it holds the seams, and a
   line of hand-read ground truth in `tools/seams/test_check_conventions.py`.

### A native function handed to the client

The client checks every function it calls through a pointer it was handed (a C function its Lua
calls, a Lua hook, a console command's handler, a CVar's callback) with
`OsValidateFunctionPointer`, which ends the process unless the pointer is in its own `.text`. The
`Os.ValidateFunctionPointer` gate lets through the code `src/runtime/Code.cpp` registered: the
runtime's own image and every binary the mod host maps (the host opens them through
`code::HostLoader()`, which registers a binary's executable sections when it is mapped and drops
them before it is unmapped).

1. A binding that pushes a native function into the client (`lua_pushcclosure`, a console
   command, a CVar callback) pushes a function of `WarcraftXL.dll`, as `wxl.script` pushes its
   `NativeThunk`, never a mod's pointer, a trampoline or generated code.
2. The service asks `code::Handable(fn, what)` before the push and refuses the call when it
   answers false: a pointer outside the registered code would end the client at its first call.
3. Code made at run time (a JIT, a thunk) is not accepted: the registry holds the sections of
   mapped modules only, and the contract has no member for a mod to add a range.
4. A harness check that the pointer the binding pushes passes the gate (`CheckFrameScript` does it
   for `register_function`).

### A new object getter for mods

1. The fact, as above, when the client must answer it (a `field`, `client` or `cache` row); a
   `shared` row reads what the shared runtime already has (the descriptor block, other reads).
2. A member at the end of `wxl_object_api` in `sdk/include/wxl/services/object.h`, and
   `WXL_ABI_VERSION_FEATURE` raised in `abi.h`: something was appended.
3. A row in `sdk/runtime/include/wxl/runtime/object_reads.def`, under a `WXL_OBJECT_SINCE` row of
   that version. A `field` row whose `member` is the fact's name needs nothing more in core; a
   `client` or `cache` row gets its function in `core/src/objects/Reads.cpp`; a `shared` row in
   `sdk/runtime/src/object_reads.cpp`.
4. Run `wxl-object-members <sdk>`: it writes the C++ method into the class of the row's kind
   (`sdk/include/wxl/objects/generated/`) and the line of the guides' tables. Its Lua method is
   named after the member (`sdk/lua/src/ObjectNames.hpp`); the `lua-types` test fails a name that
   would be ambiguous.
5. The fake service in `sdk/tests/fake_services.hpp` answers every row from 0.1.18 on by itself;
   `contract.cpp` and `lua_tests.cpp` check each such row through its C++ and Lua method.

### An action of the player (wxl.action)

A row in `sdk/include/wxl/services/action.def` and its member in `action.h`, the client call as a
binding row in `core/include/wxl/core/binding/action.def` (written in `BindingX86.cpp` with the
convention Wow.exe gives the function, checked through `src/binding/CallX86.hpp` by the harness,
and in the fork's `WarcraftXL/Binding.cpp`), and its body in `core/src/services/ActionService.cpp`,
which refuses a call off the main thread. The objects' own action methods (`Select`, `Interact`,
`CastAt`, `Use`, `Follow`, `Assist`) are written by hand in the facades and in `sdk/lua/src/Objects.cpp`.

### A new service member for mods

1. The member at the end of its `wxl_<svc>_api` in `sdk/include/wxl/services/<svc>.h`, and
   `WXL_ABI_VERSION_FEATURE` raised in `abi.h`.
2. Its row at the end of `sdk/include/wxl/services/<svc>.def`. The first member a feature appends
   to a table gets a `WXL_API_SINCE(svc, member, (major, minor, feature))` row right above it: a
   Lua mod whose manifest asks for an older `abi` is refused that member, naming the version to ask
   for. A new table states the version it came with in its `WXL_API_SERVICE` row, and the whole
   module is refused the same way. `sdk/tests/reflect.cpp` fails a row out of place or newer than
   `abi.h`. A C++ mod is not gated by it: it names a service from its own headers, so its built
   abi always covers it, and `requires`/`optional` with the service's version decide the rest.
3. The implementation in core (`src/services/`), and its facade method or Lua shim if the row
   says `by_hand` or `shim`.

### The update-field table

`sdk/include/wxl/update_fields.def` is written by `core/tools/fields/generate_update_fields.py`
from the fork: the client's field descriptors (`ObjectMgrClient/Mirror.cpp`), its descriptor
blocks (`CG*Data`, measured 32-bit by clang-cl), its dword counts and its mirror lists. After a
pull that touches them, run it and review the diff:

```sh
python core/tools/fields/generate_update_fields.py --client <fork> --out <sdk>/include/wxl/update_fields.def \
    --binary <stock Wow.exe>     # optional: compare every table with the binary's own
```

The code names (`MaxHealth` for `UNIT_FIELD_MAXHEALTH`) are the client's names with the kind's
prefix dropped and each word capitalised; `core/tools/fields/words.txt` spells the words the
client runs together, the way the getters name the same values, and the generator fails on a
line no field uses. A getter of `object_reads.def` and the field it reads agree in type and, where
an object has both under one name, in what they read: `sdk/tests/getter_fields.def` pairs them,
`sdk/tests/runtime.cpp` holds the types to each other and `sdk/tests/lua_tests.cpp` the names.

The fork's build runs it with `--check` (`wxl-fields-check`, beside `wxl-seam-check`) and fails
when the table is not what the fork gives; `client/WarcraftXL/Binding.cpp` also holds each
kind's block size to the struct the build compiles. `sdk/tests/reflect.cpp` checks the table
against itself and against the client's dword counts.

### A new event

1. A row in `sdk/include/wxl/events.def` with the next free id, its args struct as a block of
   rows in `fields.def` (or an existing struct), and `WXL_ABI_VERSION_FEATURE` raised in `abi.h`:
   an appended event is a new feature, so a mod that subscribes to it asks for that feature in its
   `mod.json` and an older runtime refuses it. The minor is a stage of the platform and the major
   a change of meaning (`sdk/include/wxl/abi.h`); neither moves for an event. The struct, its
   reflection and the parameters of its C++ and Lua handlers follow from the rows.
2. The first event a feature appends gets a `WXL_EVENTS_SINCE(major, minor, feature, id)` row
   right above it, with the version `abi.h` was raised to; the events after it, to the next such
   row, are counted to that version. wxl.lua refuses a Lua mod's subscription to an event newer than
   its manifest's `abi`, naming the version to ask for, and the host one from a binary built
   against headers older than the event. `sdk/tests/reflect.cpp` fails when a row is out of place
   or newer than `abi.h`; it cannot tell a forgotten one, which counts the new events to the
   previous version.
3. The seam that raises it, as above.

### A new mod

Start from `sdk/template/` and follow `sdk/docs/your-first-mod.md`. A Lua mod is a folder with an
`init.lua`, run by `wxl.lua` (`sdk/lua/`): see `sdk/examples/lua-hello/`. `wxl.lua` ships with both
runtimes: `core/cmake/wxl_lua.cmake` builds it from the SDK checkout for the tree's architecture
into `Mods/wxl.lua/` (core's `dist/`, the client's `bin/Win/`).

### Memory accounting

`wxl.footprint` (`src/services/FootprintService.cpp`) answers where the client's memory goes. Its
hot half is three seams on Storm's allocator (`seams/storm.def`): while a mod has started
accounting they key every block to the source file the client named, in tables of their own
pages (a row per file, counted without a lock; 64 shards of blocks, each behind its own lock),
and otherwise return after one load. Nothing there raises an event or allocates through Storm.
Its cold half is the picture `snapshot()` takes on the main thread: the counts, the binding rows
of `binding/memory.def` (the device's textures and buffers, described by `wxl/core/gpu.hpp` on
both sides; the sound and script heaps; each archive's tables; each client database), the
reports of mods and a walk of the address space. `MemoryPressure` is looked for from
`Gx.ScenePresent`, about once a second while a mod holds a threshold. Lua mods report through
wxl.lua, which names each one's heap and reports at its own snapshot.

### The camera and the pointer

`wxl.camera` (`src/services/CameraService.cpp`) keeps the views mods set, newest first. The
`Camera.UpdateCallback` seam is on the one function that places the world camera each frame,
called from inside the world pass (`CGWorldFrame::RenderWorld`, then `OnWorldUpdate`), right
before the projection and the sound listener are set from the camera: its After raises
`CameraUpdate` and the service writes the first view over the client's (`binding/camera.def`:
position, facing rows built as `CSimpleCamera::SetFacing` builds them and taken into a parent's
frame on a vehicle or a transport, `m_fov`). The client's camera runs underneath; only its field
of view outlives a frame, and the seam's Before puts it back before the client's own update.
A view's roll turns the second and third facing rows around the first
(`include/wxl/core/camera_facing.hpp`, shared by both bindings): the client builds its view
matrix from `Forward()` and `Up()` (`CSimpleCamera::SetGxProjectionAndView`, 0x00607CB0), and its
pick (`CGWorldFrame::HitTestPoint`) casts through that matrix, so the world and the pick turn
together; the interface has its own projection. `Roll()` measures the up row against the level
one for the same forward.

`wxl.input` (`src/services/InputService.cpp`) holds the pointer for the mods that capture it:
at every present (`Gx.ScenePresent`) it is clipped to one pixel of the window, the centre or the
point of the last mod to capture or move it (`capture_cursor_at`), and the device stops drawing it
(`binding/input.def`), while the game's own pointer state stays on, so its picking and clicks act
there. The window procedure's gate raises `MouseMotion` from
raw input, which the service registers for the window at the first capture or listener, and
`MouseWheel`, which a handler keeps from the client.

### The mouse look kept out, heads, Dear ImGui

The game's mouse look starts when a key binding's Lua action sets a look bit with
`CGInputControl::SetControlBit` (0x1 the right button, 0x2 the left) and ends at
`UnsetControlBit`, which does the click's default action when the hold was short. While a mod
blocks it (`wxl.input`), the two gates of `seams/inputcontrol.def` keep the look bits from the
game, note when each button went down, and at the release do the default action the press noted
(`CGWorldFrame::PerformDefaultAction`) when it came within 0.8 s: the game never holds a button.
Whatever path the click comes by, `PerformDefaultAction` is where the game acts on it; its gate
(`seams/worldframe.def`) raises `WorldClickRequest` with what the press noted under the pointer,
and a mod that refuses it keeps the game from selecting, talking, looting or walking.

The interface's signature is one function on both clients, `FrameXML_CheckSignature`, which the
glue, the game and the add-on list each call for a .toc. Its gate (`seams/framexml.def`) has the
client judge the file first, through the binding (`interface_check_signature`), whose call reaches
the same gate again and is let through: on x86 the binding calls the client's address, which is
the detour; on x64 the fork's function, whose first line is the gate. It then raises
`InterfaceSignature` with the verdict and answers it, or signed for a file a mod accepted. The
callers hold what they load to the digest the check wrote, which a missing or corrupt `.sig` leaves
unwritten; `interface_signature_digest` makes it then as `CalculateSignatureMD5` does (on x86 by
its own steps, since that function takes an argument in ESI).

`wxl.pose` (`src/services/PoseService.cpp`) keeps the heads mods turned; after each
`CGUnit_C::PreAnimate` (`Unit.PreAnimate`), where the game sets its own twist, the head moves on
towards its target and is written as the head key bone's procedural transform.

`wxl.imgui` (`src/services/ImGuiService.cpp`) owns one Dear ImGui and one ImPlot context, built
from the SDK's copy (`sdk/deps/imgui`, `sdk/deps/implot`) with its Win32 and Direct3D 9
backends. At every present of a painted frame, while a mod listens to `ImGuiFrame`, it opens a
frame, raises the event, and draws with the game's device inside its scene; the device's state
is saved and restored by the backend and the game told to send its own again. The window
procedure's gate hands every message to ImGui first and stops the ones it wants. Mods compile the
same copy (`wxl::imgui`) and point it at these contexts and this allocator (`ImGuiService::Bind`).

### Highlights

`wxl.highlight` (`src/services/HighlightService.cpp`) keeps the highlights mods set, one per mod
and object, newest drawn where two mods mark one object. The service is the same on both
runtimes; what draws is the runtime's renderer. On the stock client it is
`src/gx/d3d9/Highlight.cpp`, over the `Device` interface of `src/gx/d3d9/Device.hpp` (the game's
`IDirect3DDevice9`, or the harness's recording one):

- `WorldFrame.RenderWorld` Before opens the pass; at its first `M2.DrawBatch` (the camera is placed
  by then) the objects' top models are found, and every opaque batch of theirs drawn to the
  screen is kept: its numbers (`gx_model_draw`), what the device had bound, its first texture and
  alpha test and the vertex program's constants, read from the engine's own copy
  (`src/binding/HighlightDrawX86.hpp`): the game makes its device pure, so Direct3D cannot be asked.
- `WorldFrame.RenderWorld` After, before `WorldRenderEnd`, draws them again with the game's vertex
  program into two masks against the scene's depth, never writing it (multisampled like that
  depth, then resolved), dropping the texels the game's alpha test drops, blurs the second at half
  size, and finds the outline and adds the glow in one full-screen pass, with widths scaled from
  1080 lines to the render's height. Then the device is put back as the engine believes it: the
  vertex registers the batches overwrote get the engine's values again (the passes' own register,
  c250, is above the game's), the declaration, index buffer and streams the engine's copy holds
  are bound again (the engine binds its index buffer again only when it changes its own, so a draw
  of the interface after the pass would otherwise use a model's), the states the engine never sets
  go back to their value, and `gx_invalidate_state` has it send the rest.
- `Gx.ReleaseD3dResources` takes its targets and programs; they are made again at the next draw.
- While a highlight is set the service holds up `ModelBatchDrawn`'s flag, so that the x64 client
  calls `M2.DrawBatch` with no mod listening to the event. Each world pass is counted (highlights,
  top models found, model batches drawn, the highlighted models' ones, kept, drawn into the masks,
  composed onto the scene): at `Gx.ScenePresent` one line under the `highlight` tag, at debug
  level, about once a second while highlights are set, and every reason something was left out
  (no model batch reached the service, no record of a draw, the fixed function, a refused pass)
  logged once. `drawn` counts the batches of a pass that reached the scene, 0 when it did not.

Its programs are HLSL (`src/gx/d3d9/shaders/highlight.hlsl`), compiled when the runtime is built
by `wxl-core-d3d9-shaders` (`tools/shaders/compile_d3d9.cpp`, the system's d3dcompiler_47: under
Wine from Linux) and held to `shaders.txt`: the profile's version, the constant registers each may
read (above the game's, so the game's are never touched), the samplers; the device's caps are
checked against the composite's length before anything is drawn.

On the recompiled client's Vulkan renderer the same passes are `src/gx/vk/render/Highlight.cpp`,
made of `wxl.render`'s own pieces (below): at `M2.DrawBatch` the renderer's record of the batch's
draw (`draw::Batch`: its vertex program, layout, buffers, viewport and push constants) is kept, a
batch drawn by the fixed function left out as on D3D9; after the world the batches are drawn again
with a batch program (`highlight_mask.frag`: the game's alpha test on its first texture, the
colour and the glow into two masks) against the scene's depth, the masks of the scene's samples
resolved when the pass ends, blurred across then down at half size (`highlight_blur.frag`) and
composed onto the scene (`highlight_composite.frag`, blended ONE, ONE_MINUS_SRC_ALPHA), each program
the HLSL one line for line; nothing of the engine's state is touched, so nothing is put back. The
native tests hold the picture to the Direct3D 9 algorithm computed on the CPU. On the recompiled
client under D3D9, `supported` answers false with the reason.

### The game's selection circles

`wxl.selectioncircle` (`src/services/SelectionCircleService.cpp`) keeps the looks mods give the
circles the game projects on the ground: hidden, or with a texture, colour or scale of their own,
per kind and reaction (a cell each) or per object. Every circle is drawn by one of four members,
between the device's `RsPush` and `RsPop`: `CGUnit_C::RenderTargetSelection` and
`CGCorpse_C::RenderTargetSelection` (called by `CGWorldFrame::ClearTrackedObjects` for the target
`CGObject_C::PreAnimate` noted), `CGUnit_C::RenderPetTargetSelection` and `RenderAutoTrackCursor`.
Each binds the static `CGObject_C::s_selectionTexture` (`SetCircleRenderStates`) and calls
`ProjectTex::Render2d` with a box and a colour of its own. The four drawers' seams open and close a
circle: Before chooses its look (an object's own, then its kind's and reaction's, the last set
among mods; the reaction is read only when some look is set) and puts a mod's texture in the
static, After puts the game's back. `ProjectTex.Render2d`'s gate, which every ground projection
passes, does nothing outside a circle; inside one it skips a hidden circle, or points the box and
colour arguments at the service's own, scaled around their centre and recoloured with the game's
alpha kept as a factor, so the game's own fades still play. Textures are made with the client's
`CreateTexture` once per path and closed with their last look. With no look set, a circle costs
the drawer's two calls and one load at the gate.

### Console variables

`wxl.cvar` (`src/services/CVarService.cpp`) keeps the variables mods and the interface register by
name (without case, as the client's hash compares them), each with its default, flags, help,
range and holder, and makes each one of the client's own CVars: `CVar::Register` with the
runtime's callback, then the flags written as the client's bits (archive 0x1 for any save, the
category 0x10 or 0x20 for the account's or the character's, latch 0x2, protected 0x8, hidden 0x40,
read-only to the console and `SetCVar` 0x100, registered by code 0x80000000). The client reads
Config.wtf once, in `CVar::Initialize`, early in `InitializeGlobal`; a `SET` line of a name nobody
registered makes an archived variable of that value with no default, which a later `Register`
adopts, value kept. A registration made before waits: `ConsoleVar.Initialize`'s Before makes it,
so the file's lines set it like a variable of the client's (the latch bit only after, or the line
would go to the latched value). The client deletes its account variables when an account logs in
and its character variables when the player leaves the world, and registers its own again at the
next world; `ConsoleVar.DeleteAccountCVars` and `DeleteCharacterCVars`' After make the mods' again
at once, so they exist at every moment and take the saved values the account data brings. Every
set the client makes calls the callback first (`CVar::Set`, `Reset`, `Default`, the console's
`set`, inlined on the stock client): it refuses a value outside a range, and a set of a read-only
variable that is not the service's own. Nothing is cached by pointer: every call looks the name up.
The interface's `WxlRegisterCVar` is a function `wxl.script` registers at each context, owned by
no mod. Main thread only.

### The ground

`wxl.terrain` (`src/services/TerrainService.cpp`, the grid's arithmetic in the SDK's
`wxl/terrain/grid.hpp`) reads and reshapes the map's ground. A loaded chunk (`CMapChunk`) points
into its tile's file buffer for its 145 heights (MCVT, relative to `m_topLeftCoords.z`) and normals
(MCNR, three signed bytes), and every reader takes them live: the drawing fills its vertex buffers
from them (`CMapRenderChunk::RenderPrepBufs`, when the buffer's `m_hasData` is 0), the collision
facets of the movement and the camera (`CMap::GetFacets` -> `GetChunkFacets` ->
`CMapChunk::Intersect(rect, box, facets)`), the ray tests of the camera, the clicks and
`World::Intersect` (`CMap::VectorIntersectSubChunkList` -> `CMapChunk::Intersect(subX, subY, ray)`)
and the height query (`CMap::GetHeightTerrain`). The stock client is the same code. An edit
writes the heights and the normals there (binding `terrain_chunk_view`), then has the client make
again what it derived from them (`terrain_chunk_rebuild`: `CMapChunk::CreateBounds`, the render
chunk's bounds by `CMapRenderChunk::AddBatch` over the same chunks, its pooled vertex buffer marked
empty, so the device's own lock and unlock fill it on D3D9 and the Vulkan renderer alike, and the
ground clutter freed with `CDetailDoodad::FreeInst`, grown again by `CMapChunk::RenderPrep`). The
clutter is freed outside the world's drawing only (between `WorldFrame.RenderWorld`'s Before and
After it waits for the After): the frame being drawn may hold it in its sort list. The movement
keeps copies of the ground's triangles in `s_queryBox`/`s_moveFacets` (Movement.cpp) and reuses
them while a move stays inside the box (`CMovement_C::ValidateTestVsFacetQuery`): every edit empties
the box (`terrain_forget_collision`). A unit standing still runs no collision at all, so a player
standing on terrain that went down is made to fall as the client does when its transport goes
(`CMovement_C::OnFallLocal`).

Edits are kept by chunk (column and row on the map) and vertex, each with its owner (the mod's
token and a layer it names; wxl.lua gives each Lua mod one) and a serial: the newest shows. An
outer vertex on a chunk's edge is written in every chunk holding it (up to four). A chunk's
original heights and normals are taken the first time the service writes into it and kept until
the map unloads: a reset writes them back, and a chunk the client builds again
(`MapArea.PrepareChunk` After: from the same buffer, which still holds the edits, or from the disk
after its tile was freed) gets its edits again, with the normals of its vertices and of its
neighbours' edges, before `TerrainChunkLoaded`. A vertex's normal is the file's while no vertex
sharing a triangle with it is edited, and otherwise the area-weighted sum of its triangles' normals
over whatever chunks are loaded. `vertices_in` walks the map's global grid inside a circle
(`wxl::terrain::ForVerticesInCircle`), naming an edge vertex once, in the chunk whose cells it
starts, and reading its heights from any loaded chunk holding it. `MapChunk.Purge` raises `TerrainChunkUnloaded`; `Map.Unload`
drops every edit and raises `TerrainEditsCleared`. Main thread only. No new call into Wow.exe for
the reads; the x86 binding calls `CreateBounds`, `AddBatch`, `FreeInst` and `OnFallLocal`
(`calls_x86.def`) and writes `s_queryBox` at its address.

### Textures from SVG and PNG

`wxl.texture` (`src/services/TextureService.cpp`) makes textures the client loads as its own files.
A request names a source (a mod's folder, read off the disk; any other name read through
`wxl.storage`, so archives, served bytes and providers answer, with the disk beside the client as a
fallback for `Interface\AddOns\`), a size, restyled groups and a tint. The source's bytes are
hashed (FNV-1a) with the rest of the request into the texture's name,
`Interface\WXL\<origin>\<stem>_<w>x<h>_<hash>.blp`: a name already known is answered at once. A new one
is drawn by `src/imaging/` (nanosvg for SVG, its parsed shapes restyled by id before they are
rasterized; stb_image for PNG, resampled with a Mitchell filter widened by the ratio; everything
premultiplied in floats), padded to power-of-two edges (`GxTexCreate` refuses any other 2D texture
and anything under 8), given its mips and written as an uncompressed BLP2 (colour encoding 3, alpha
8, preferred ARGB8888: `PumpBlpTextureAsync` points the device's mips straight into the file),
straight alpha with transparent pixels coloured from their neighbours. The bytes reach the client
through one provider of the runtime's own (mod 0, priority 1000, `src/services/Assets.cpp`), which
answers only names under `Interface\WXL\`: every loader on every thread reads them as a loose file,
on both clients and under D3D9 and Vulkan alike. Names and their recipes (the source's bytes when
under 1 MB) are kept up to 2048, the textures' bytes up to 64 MB; past the budget the least used
bytes are let go and drawn again at the client's next open, past the count the least used name is
forgotten. The interface's `WxlTexture` is a Lua wrapper run with no taint at each
`FrameScript.CreateEvents` that starts a context, before the mods hear of it: it flattens the
groups table (the binding reads no table) and calls a native function it keeps as an upvalue,
taking it out of the globals. No new call into `Wow.exe`.

### The profiler

WarcraftXL.dll carries the process's one Tracy client (`deps/tracy`, v0.14.1), compiled with
`WXL_PROFILE` (the build's choice, default on; off removes it, the service and the host's zones,
and the launch header says the `[profile]` section is ignored). Whether it runs is the player's
choice: `WarcraftXL.ini`'s `[profile]` `enabled` (default 0: no thread, no port), `port` (8086)
and `listen` (`localhost`, 127.0.0.1 only, or `any`), with `WXL_PROFILE=0|1` in the environment
winning over `enabled`; `settings::ResolveProfile` turns them into the choice the header reports
and `profile::Start` takes. Tracy reads its port and its localhost-only switch at run time from
`TRACY_PORT`, `TRACY_ONLY_LOCALHOST` and `TRACY_ONLY_IPV4` (through `GetEnvironmentVariableA`, at
`___tracy_startup_profiler` and when it opens its socket), which `profile::Start` sets from the
choice, so the vendored source is untouched; localhost is IPv4 alone, since an IPv6 loopback
socket takes no viewer connecting to 127.0.0.1. The address Tracy announces itself to is
compile-time only (the whole network), so it is built with `TRACY_NO_BROADCAST`: a viewer types the
address. A port another process holds is warned before Tracy tries it once and goes quiet. On
demand: nothing is sent until a viewer connects, and it is started on a thread of its own from
`Core::Start`, never at load, since its clock calibration sleeps and its start must not run under
the loader lock. No crash handler (the host's guard owns faults), no system tracing, sampling or
call stacks.
`src/profile/Profile.cpp` is the only file that calls Tracy; three things go through it:

- **The host's zones.** `ModHost::SetProfiler` hands the event bus a `wxl::host::Profiler`
  (`sdk/host/include/wxl/host/profile.hpp`): each dispatch that reaches a handler is a zone named
  `On<Event>`, each handler a zone inside it named `<mod id>: On<Event>` in a colour per mod.
  The bus opens them only while its profiling flag is on, which `Gx.ScenePresent` sets at every
  present from whether a viewer is connected: with nobody watching, a dispatch pays one load.
  Zones open after the bus's own check that someone listens, so a seam behind `Listening` and an
  event nobody handles open none. `wxl.lua` adds a zone per Lua mod inside its own handler.
- **`wxl.profile`** (`sdk/include/wxl/services/profile.h`, `services/profile.def`): a mod's own
  zones at locations the runtime keeps for the process's life, text, plots, messages and frame
  sets. A zone is the connection it was opened under, so one left from an earlier viewer is
  dropped by the profiler's own check.
- **The client's zones** (x64): `WarcraftXLZoneBegin`/`End`, exported, behind the one-line macro
  `WXL_CORE_ZONE` of `include/wxl/core/profile.h`, which the fork uses through
  `client/WarcraftXL/Profile.h` in its main loop's hot spots (world update and render, models,
  object updates, the interface, script events).

Every present ends a frame (`Gx.ScenePresent`, both architectures, the loading screen included);
while a viewer watches and accounting runs, it also plots `storm bytes`, `storm blocks` and
`accounting tables`. `wxl-core-seam-harness --profile-live` (127.0.0.1) or `--profile-live-any`
waits for a viewer and sends a hundred frames of every kind of zone, which checks the profiler end to end on one architecture without a
client; `--bench` measures a zone with no viewer.

### The Vulkan renderer

The renderer is the runtime's (`src/gx/vk/`, namespace `wxl::vk`, MIT), one source for x86 and
x64. Its modules are static libraries of `cmake/wxl_vk.cmake`, each seeing only the ones above it:

| Module | Owns |
|---|---|
| `device` | volk (`deps/volk`, the loader found at run time), the instance, the physical device chosen among those that meet the Vulkan 1.3 floor (`device/Floor.hpp`: the features and extensions as two tables, the refusal of a device by the first fact it lacks, the choice by type, version and memory, `[gx] gpu`), the logical device with its graphics queue, its loss |
| `resources` | VMA (`deps/vma`) over volk's entry points, buffers and images (levels, cube faces, a view's swizzle), the driver's budget; the frame's rings (`Ring.hpp`: staging and each draw's data, a list of mapped chunks per frame in flight); the bindless table (`Bindless.hpp`: one descriptor set of every texture's image and of the samplers made at their first use, written as textures are made, never per draw) |
| `shaders` | a SPIR-V writer over SPIRV-Headers (`shaders/Spirv.hpp`) and the present pass's shaders made with it at run time (`shaders/Own.cpp`); the translator of the client's DX9 programs (`shaders/Dx9.cpp` decodes vs_1_1 to vs_3_0 and ps_1_1 to ps_3_0, `shaders/Translate.cpp` writes SPIR-V 1.5 against the interface every program shares, `shaders/Interface.hpp`: vertex inputs at the engine's attributes, fourteen varyings at fixed locations, 128 bytes of push constants, the engine's registers read through buffer addresses); the translations kept (`shaders/Cache.cpp`: in memory and in `<client>\WarcraftXL\cache\vk\shaders.pack`, keyed by an xxHash3 of the record, the translator's version and its options, a worker translating a shader's permutations ahead); the GLSL of the renderer's own programs (`shaders/glsl/`) |
| `pipelines` | the programs of the engine's draws: the renderer's own, compiled from `shaders/glsl/` when it is built by glslang (`deps/glslang`, built for the build machine by `cmake/wxl_glsl.cmake`; nothing compiles at run time: D3D9's fixed function with its lighting and user clip plane, the client's interface programs `UI` and `Desaturate`), and the client's translated ones, added as the engine creates them, each keeping its id and its record's hash; the pipelines, found by a 64-bit key of small ids (programs, vertex layout, blend mode, colour mask, topology class, fill, the target's formats and samples, and whether its samples sit on the pixel's centre: `GxRs_Multisample 0`, D3D9's MULTISAMPLEANTIALIAS off, through VK_EXT_sample_locations where the device has them, the draw left antialiased and counted where it has not), everything else dynamic, through a `VkPipelineCache` kept in `<client>\WarcraftXL\cache\vk\pipelines.bin`. A missing one is linked at once from four parts where the device has graphics pipeline libraries (the parts that compile a program made on worker threads as soon as the program is added, the whole pipeline compiled on a worker then taking the linked one's place), compiled on a worker otherwise (`[gx] async_pipelines`, on by default: a draw into the scene is left out until it is ready, one into a texture waits), or compiled on the spot; the pipelines drawn are kept in `pipelines.keys` (the programs by their records' hashes) and compiled ahead at the next launch as their programs come |
| `frame` | the timeline (submissions as serials of one timeline semaphore, waits that always end, 2 s), a one-shot submitter, the frames in flight (`[gx] frames_in_flight`, 1-3) |
| `present` | surfaces (Win32's; a headless one for the tests), the swapchain and the rules it is made by (present mode, format, extent, image count, every surface state read as a rebuild or a loss), the present pass (the scene stretched to the image shown, the gamma ramp; the software cursor is the engine's own draw into the scene, as on D3D9, so screenshots hold it), the frame limiter, and `Display`: a frame from its first command to the image shown, the scene (colour and depth) at the engine's size and its multisample count (resolved for the image shown, a copy, a readback), a readback of the scene in the middle of a frame (the frame so far submitted and waited for, then the frame goes on) |
| `draw` | the engine's draws in Vulkan (`draw/Draw.hpp`): its texture formats (`draw/Formats.hpp`, D3D9's bytes read through a format and a swizzle, nothing converted), its textures (an image, bindless slots for its 2D and cube views, uploads by lock and unlock, views as a render target), its pools (a stream pool's mapped buffers renamed when the engine discards it; a static or dynamic pool's CPU copy, copied to the device before the first draw that reads what changed), and `Renderer`: the client's programs (translated once per record), the engine's vertex and pixel registers (copied into the frame when they change, as many rows as the bound programs read), the render target (the scene, or a texture with its own depth or the renderer's), clears, copies from the scene (TexCopy, TexStretch), occlusion queries (counted precisely, read without a wait), and a draw packet recorded into the target, the uploads in the frame's upload pass when nothing drew with the resource yet this frame, between the draws otherwise |
| `engine` | `gxfront/Engine.cpp`: one draw of the engine read out of its `CGxDevice` (the SDK's engine layouts, and the small records of `include/wxl/core/gx_state.h`) into a packet, as the D3D9 device's `IStateSync` reads the same state (sixteen texture stages, the lights, the fog, the user clip plane, a texture target's viewport the right way up); what a pointer of the engine names asked of a resolver (the live client's, or a capture's); the draws it leaves out, by reason |
| `imgui` | Dear ImGui's Vulkan backend (the SDK's `deps/imgui/backends/imgui_impl_vulkan`) over the renderer's device, dynamic rendering into the scene at its sample count (as into D3D9's back buffer, under the cursor), the font atlas uploaded in the frame |
| `render` | `wxl.render` beside the engine's draws (`render/Render.hpp`): images (screen-sized ones made again with the scene, a multisampled one resolved into its one-sample image when a pass ends, a bindless slot each), buffers (device memory read by address and as 32-bit indices, written through the frame's upload pass after the frames before it are done reading), pipelines of a mod's SPIR-V on the engine's layout (graphics with no vertex input, full-screen behind the runtime's triangle, batch behind an engine vertex program and its layout, a variant per target and engine program), passes (a rendering of their own between the engine's: `Display::BeginOwnRendering`), the engine's draws kept as batches (`Renderer::KeepBatches`) and drawn again, the scene's colour and depth copied for sampling, GPU zones by timestamps; and `wxl.highlight`'s passes made of them (`render/Highlight.cpp`) |
| `capture` | the reader of a frame capture (`include/wxl/core/gx_capture.h`), which the client records under D3D9 (`gxCapture`), and its replay through the renderer (`capture/Replay.cpp`) compared with the D3D9 picture it holds |

**Past the stock limits (x64, Vulkan only).** The device's caps carry two words of the
recompiled client's own in `CGxCaps`' unused ones (`wxl/core/gx_vk.h`, `WXL_GX_VK_CAP_*`; D3D9
leaves them zero, the layout stays stock): a bone palette of 256 bones and index pools of 32-bit
indices. The client's three bone uploads (`CM2SceneRender::DrawBatch`, `DrawBatchDoodad`,
`CM2Model::RenderBatchShadowMap`) then hand their bones to `GxBonePaletteSet` instead of writing
c31 and up; `CM2Shared::Initialize` takes the 256-bone skin profile and the doodads' instancing
budget is the palette's. The backend's `bone_palette` row keeps the palette (`Renderer::BonePalette`),
copied into each frame's ring as the registers are; a translated vertex program that reads bones
(a0-relative c31..c33, `ProgramInfo::palette`) reads them there, its address at `Push::data` (or
at `Push::pixel` beside the renderer's own pixel program, `kFlagPaletteAtPixel`) and its bone count
in the flags, and from the registers when no palette was set (a capture of D3D9). A pool of
`GxPoolTarget_Index32` (2) is bound with 32-bit indices, its offsets read four bytes an index; the
client's skins past 16 bits use one, and `GxDrawIndexRange`/`GxDraw32` keep their vertex range in
32 bits. Textures load up to `min(device, 16384)` with a decode scratch that grows to the image
and a larger read budget. `wxl.render` adds compute pipelines (`kCompute`, their own layout of the
bindless set and 128 bytes of push constants) dispatched between passes, and mods' palettes in the
frame's ring.

None knows the runtime or the log: they hand their lines to a sink (`gx/vk/Log.hpp`) and build
natively for Linux in `tests/vk/` from the same rules; only `engine` reads the engine, through its
layouts. `gxfront` (`src/gx/vk/gxfront/Front.cpp`, Windows) is the Vulkan device behind one engine
`CGxDevice`: the caps it fills, the gamma ramp, the pointer, the format's vsync and window mode, the
engine's pools, textures, render targets, shaders (their records kept, so a device made again after
a loss makes their programs again), registers and queries on the device, its copies from the back
buffer, and each draw read by `engine` and recorded by `draw`. The runtime's side is `src/gx/`:

- **Which API.** `WarcraftXL.ini`'s `[gx] api` (`settings::GxApi`), applied by `gx::SelectApi`:
  `vulkan` takes a D3D9 or D3D9Ex request to Vulkan, `d3d9` a Vulkan one to D3D9; without the line
  the client's request stands. Numbers are the client's `EGxApi`, Vulkan appended as 6.
- **The recompiled client.** Its `Gx/CGxDeviceVk` keeps the engine's own logic of each device call,
  as the D3D9 device does (the window and its procedure, the stream pools' offsets, the textures'
  callbacks, the lost device's return), and calls through the table of
  `include/wxl/core/gx_vk.h` (`WarcraftXLGxVulkan`) for the Vulkan work: `select_api` from
  `ConsoleDeviceInitialize`; `device_create` on the window it made, which the adapter calls only
  when the table's `device_layout_hash` is the `CGxDevice` layout of its own engine headers;
  `device_set_window` when the format makes the window again; the frame (`scene_begin`,
  `scene_clear`, `draw`, `scene_present`); the window (`window_size`, `cursor_set`); the pools
  (`pool_memory`, `pool_discard`, `pool_written`) and textures (`tex_create`, `tex_lock`,
  `tex_unlock`: the adapter latches each level and copies it with the engine's own `Blit`, as the
  D3D9 device does); the shaders (`shader_create` with the record, `shader_prewarm` with every
  permutation the engine loads, `shader_constants` from the adapter's `IShaderConstantsFlush`); the
  render target (`target_set` after `DeviceSetRenderTarget`, the target made first); the copies from
  the back buffer (`tex_copy`, `tex_stretch`); the occlusion queries (`query_create` ..
  `query_result`, the handle in `CGxQuery::m_apiSpecific`); the back buffer read back
  (`read_pixels`, the adapter's `DeviceReadPixels`: screenshots and the portraits' alpha check);
  the multisample counts the device has (`sample_counts`, which filter the formats the video
  options offer); `device_restore` after a loss. When the device cannot be made (no Vulkan 1.3 device, a
  window it cannot present to), `ConsoleDeviceInitialize` creates D3D9 with the same format and the
  log has one line saying why.
- **The stock client** has no Vulkan adapter: `ConsoleDevice.Initialize` After makes the device
  (`gx::DeviceReady`), once per launch, when `[gx] api = vulkan`, checks it with a frame and
  releases it; the client keeps D3D9.

On the recompiled client the device renders: each frame's scene is cleared as the engine asks, the
engine's world and interface are drawn into it (and into the textures it draws into: the minimap,
portraits, the full-screen effects), then Dear ImGui (`wxl.imgui`, `ImGuiFrame`), and the present
pass shows it. A draw is left out, and counted by reason, when it binds a shader whose record the
translator refused, when its target is a depth texture alone, when a buffer it reads is unknown,
or while its pipeline compiles on a worker; `describe` (the release line, and the client's crash
report after what it holds already) says how many of each, how many draws were the world's
(`WorldFrame.RenderWorld`'s Before to its After, `gx::WorldPass`), the samples per pixel, the
pipelines made and how (linked from parts, compiled on workers, prewarmed from the last launch,
replaced by their whole compile, compiled on the main thread, and the stalls of 4 ms or more with
the longest), the programs translated (and how many came from the cache or ahead of their bind),
the target renderings, copies, resolves, readbacks and queries. Fullscreen is a borderless
window over the monitor, minimised when it loses the focus; a resize rebuilds the swapchain and the
scene, not the device. `gxVSync` 0 is MAILBOX (else IMMEDIATE), 1 FIFO, 2-4 FIFO and a frame
limiter in fullscreen. The gamma ramp applies in fullscreen, as D3D9's. A loss (or the console
command `gxVkSimulateLoss`) goes through the engine's own lost-device path: `m_context` 0, the
device made again at the next scene, `SwapchainLost` before and the engine's restore callbacks
(`SwapchainRestored`) after; three losses within a minute close the client with a message. With
the profiler on, each frame is a GPU zone of a "Vulkan" context, each `RenderPoint` and each mod's
`gpu_zone_begin` another.

The platform contract under Vulkan (`src/services/RenderService.cpp`): `wxl.gx` answers the API the
device runs on; `M2.DrawBatch` hands `ModelBatchDrawn` the renderer's record of the batch (`batch`)
and the viewport it drew with, the client's `CGxDeviceVk::Draw` keeping the draw's numbers in
D3D9's terms as the D3D9 device does; `RenderPoint` is raised after the opaque world
(`WorldScene.RenderDetailDoodads`), the liquids (`WorldScene.RenderLiquidPass`), the world
(`WorldFrame.RenderWorld` After, after the highlights) and the interface (`Gx.ScenePresent`
Before), on every device, inside a frame; `wxl.render` takes passes and draws only inside one, and
closes what a mod left open; its images, buffers and pipelines are handles owned by the mod that
made them, given back at its unload, kept across a device loss (`Render::Release`, made again at
their next use) and forgotten with the renderer; a mod's replacement of the client's shaders is a
program of its SPIR-V in the place of the matching records' own (`Front::ReplaceShader`), with what
the record's program reads; `wxl.vk` hands the device's objects and, inside a point, the frame's
command buffer with the runtime's rendering ended. On the stock client, which the renderer does not
draw yet, `wxl.render` and `wxl.vk` answer that they do not drive its device, and the points are
raised for a mod drawing with the D3D9 device. Without `[gx] api = vulkan` (or
`gxApi Vulkan`) nothing of Vulkan is loaded.

### The log and the crash report

The log is one file per launch, `Logs\WarcraftXL.log` next to the executable, written by
`src/common/Log.cpp` straight to the OS (no user-space buffer; warnings and errors flushed to the
disk). It stays UTF-8 text whatever a mod logs: the host's `Logger` (`sdk/host/include/wxl/host/log.hpp`)
writes every byte of a line that is not part of a UTF-8 sequence, and every control character but
a tab or a line break, as `\xNN` before the line reaches the sink. `src/runtime/Session.cpp` owns a launch's story, from `DllMain` (kernel32 and ntdll only):

- **Settings.** `WarcraftXL.ini` next to the executable (`src/runtime/Settings.cpp`), section
  `[log]`: `level`, `keep` (earlier launches kept, 30 by default) and `level.<tag>` for one tag's
  own level. `WXL_LOG_LEVEL` wins over the file's `level`. The log's gate is the lowest of all
  levels, which the host's `log_minimum` takes; the file sink then filters by tag. Section
  `[profile]`: `enabled`, `port`, `listen` (above). Section `[gx]`: `api`, `validation`, `gpu`,
  `frames_in_flight`, `present_filter`, `async_pipelines` (the Vulkan renderer, above). A key or value that does not read is warned in
  the header with its line number. `session::CurrentSettings` hands the file to `Core::Start`.
- **Files.** `log::OpenSession` renames the previous `WarcraftXL.log` after its first line's
  `launch YYYY-MM-DD HH:MM:SS` (or its last write) to `WarcraftXL-<time>.log` and prunes the oldest
  past `keep`; a file held by another process sends the lines to `WarcraftXL-pid<pid>.log`.
- **Header**, tag `session`, written whatever the levels: the launch time, the runtime and abi, the
  commits of core, sdk and client (`cmake/build_ids.cmake`, run at every build into
  `generated/wxl_build_ids.h`; the client's when core is built inside the client's tree), the
  executable with its size, PE stamp, large-address-aware flag and, on x86, the FNV-1a of its
  `.text` against the stock 12340's, Windows (`RtlGetVersion`), Wine and its host
  (`wine_get_version`, `wine_get_build_id`, `wine_get_host_version`) and a few of its environment
  variables, the CPU, the memory and the address space, the command line, the settings, the
  profiler (`profiler: off (...)`, or on with its port and `listen`, and what decided it), the
  graphics API `[gx]` asks for (`graphics: ...`) and what
  happened to the previous log. `session::ReportMods` follows `LoadAll`: every folder, loaded
  (version, manifest abi, built abi, binary) or not (why).
- **End.** `session end: <how>, after <uptime>`, once: at `Client.Destroy` after the unload, at
  `DLL_PROCESS_DETACH` otherwise, or after a crash report. A launch whose log lacks it was killed.
- **Crash report** (`src/runtime/Crash.cpp`, tag `crash`). An unhandled-exception filter, put
  in front at `DllMain` and taken back at every stage (the client sets Storm's in `CommonMain`,
  which then runs after the report): the code and address as `module+offset`
  (`wxl::host::DescribeFault`, `DescribeAddress`), the thread, uptime and stage, the registers,
  the stack (x64 by the unwind tables; x86 by frame pointers, then a scan of the stack for
  addresses in module code when the chain is short), the mod whose code the thread was in, and
  the last 16 events raised to a handler (`ModHost::Recent`, a lock-free ring of 32 written only
  when the event dispatches). A fault inside a mod's handler or entry point never gets there: the
  guard catches it and the host logs one line with the mod, the handler and the fault.
- **The client's own errors** (`crash::ClientError`, tag `fatal`), from the `Storm.SErrDisplayError`
  seam, before the client's dialog and `Errors\` file. A fatal error (`SErrDisplayAppFatal`, a failed
  assertion, an allocation that cannot be served) is no exception, so the filter never sees it:
  the report says the error's number, code, message and source, names the module and offset of an
  "Invalid function pointer", then the stack, the running mod and the last events as a crash's
  does, and ends the session with `fatal error (#<n> 0x<code>: <message>)`. A recoverable error is
  one warning, tag `client`. After a crash report it writes nothing: Storm's filter displays the
  same fault.

`tools/read_logs.py <client folder>` reads the last launches back: header, mods, stages, the end,
the crash or the fatal error, warnings and errors grouped by wording, the marks of the `wxl.probe`
example (`sdk/examples/probe/`, `/mark <text>`) and what happened between them, the events seen and
never seen, and the newest files of the client's `Errors\`; for a launch with no clean end, the
first lines of the `Errors\` file written during it (the message and the `Current Addon` lines).

## Rules that hold everywhere

- **The boundary is C.** No C++ type, vtable or exception crosses between a mod and a runtime. Every
  struct starts with `struct_size` and grows only at its end.
- **Ids are appended.** Event ids, stage numbers and seam names are never renumbered or renamed.
- **Stages run in the order `mod.h` lists them**, on the main thread, each at most once. A stage the
  runtime cannot recognise is never reported.
- **Threads.** The host is built, loads, reports stages and unloads on the main thread. Events may be
  raised on other threads (`FileOpen`, `MessageSent`); the bus takes no lock to dispatch and frees
  nothing a dispatch may still read. A mod that faults off the main thread stops receiving events
  at once and is disabled at the main thread's next raise.
- **An unattached point costs nothing.** On x86 its function is not detoured at all; on x64 it is
  one byte tested at the head of the function. Nothing detours the recompiled client.
- **A faulting mod is disabled, not unloaded.** Its handlers are dropped, its later entry points
  skipped, its binary left mapped. A mod that fails where the guard cannot see -- a C++ exception
  reaching a stage of a `WXL_MOD` mod -- disables itself the same way, through `wxl_host.disable`.
- **What a mod holds goes with it.** Its open files, its search folders, its asset providers, served
  names and transforms, its field watches, its
  memory reports and pressure threshold, its camera view, its hold on the pointer, its block of the mouse look, its heads, its highlights, its `wxl.render` images, buffers, pipelines and shader replacements, its looks of the selection circles, its edits of the ground, its hooks and its patches are given back when it is disabled or unloaded (`OnModDisabled` in
  `src/runtime/Core.cpp`, the shutdown in `src/seams/Client.cpp`). The host sees every Lua mod as
  `wxl.lua`, so `sdk/lua/` keeps which Lua mod holds what and gives it back when that mod closes,
  reloads or is disabled.
- **An event can concern some mods only.** `ModHost::RaiseTo` raises it to the subscriptions
  whose owner a predicate accepts: `ObjectFieldChanged` reaches the mods whose watch covers the
  field (`ObjectService::Delivers`), and `wxl.lua` passes it on to the Lua mods that watch it.
- **An event nobody listens to costs nothing.** `ModHost::Listening(event)` is one atomic load:
  a seam body asks it before it reads the client for an event's args. The host tells the runtime
  when an event gains its first handler or loses its last (`SetListenHandler`), and the runtime
  keeps one byte per event, `wxl_core_listening` (`wxl/core/init.h`), which the x64 client tests
  through `wxl::seam::Listening` before calling a busy seam that raises that event: the busy ones
  (`Texture.Create`, `Storage.OpenEx`, the doodad and update seams, `M2.DrawBatch`) cost a load and
  a branch there while nobody needs them. The byte is the mods' word or the runtime's own
  (`src/runtime/Listening.cpp`): `wxl.highlight` holds up `ModelBatchDrawn`'s while a highlight is
  set, since it keeps its batches at `M2.DrawBatch`. On x86 the detour still runs; the body returns
  at once.
- **Client and runtime come from one tree.** `WarcraftXLInit` compares the client's binding rows with
  its own and refuses a client built from a different table.

## Files a mod serves

The client opens every file through `SFile::OpenEx`, which resolves the name with
`System_SFile2::FindFile`, then reads a plain file through `Blizzard::File`, whose every call is a
verb of the client's file stack (`System_File::Stacked::s_manager`, mirrored in
`include/wxl/core/filestack.hpp`). The `Storage.FindFile` gate answers a name a mod's source wins
(a provider, served bytes) with a path of the runtime's own, `|wxl|<id>|<name>`, and the runtime
writes its verbs into the top node of the stack once (`src/services/Assets.cpp`): its streams are
answered by the runtime, any other goes on to the verb the node held. A kind of file some mod
transforms is opened the same way, read whole through the client's own resolution (the thread
marked so that no gate answers twice) and converted before the client reads it. A folder of the
search path is still a name rewritten for the client's disk lookup. Nothing is done while no
folder, source or transform is registered: the gate's test is two loads.

## Checks

| Command | What it proves |
|---|---|
| `wxl-core-d3d9-shaders`, at every x86 build | the runtime's own Direct3D 9 programs compile and read only the registers and samplers `src/gx/d3d9/shaders/shaders.txt` allows them |
| `ctest` in `core/build/win-x64` and `core/build/win-x86` | every seam body against a fake client; on x86, the generated detours and the register thunks; `wxl.point`'s dispatch, and on x86 its detours armed and disarmed on stand-ins; the log's files, levels and settings (`wxl-core-log-tests`) |
| `ctest` in `sdk/build` | the contract, the field table against the args structs, the mod host, the shared `wxl.object`, `wxl.lua` against the host and its editor definitions against the tables, and that the docs quote the examples as they are |
| `python core/tools/seams/check_seams.py` | every seam is called from the fork, once, in the right function, with the right arguments. Runs in the fork's build |
| `python core/tools/points/insert_points.py --check` | every x64 point is the first line of its function, once, with the table's arguments; no line of no point. Runs in the fork's build (`wxl-point-check`), with `generate_points.py --check`: the three point tables are what `points.txt` and the fork give |
| `python core/tools/points/test_points.py` | the two point tools against a small fork made on the spot, overloads, references and de-inlined lines included |
| `python core/tools/engine/generate_engine.py --check` | the SDK's engine headers are what the fork gives, their x86 offsets checked against the fork's comments, `layouts.def` and (`--binary`) Wow.exe. Runs in the fork's build (`wxl-engine-check`); `client/WarcraftXL/EngineLayouts.gen.cpp` holds the x64 layouts at compile time |
| `python core/tools/extract/extract.py --deinlined --check` | `src/deinlined/` is what the fork gives. Runs in the fork's build when `WXL_WOW_EXE` names a stock Wow.exe (`wxl-deinlined-check`) |
| `python core/tools/extract/test_extract.py` | the extraction tool against a small fork made on the spot: conventions, members, globals, de-inlined callees, empty functions |
| `ctest` `stock` in `core/build/win-x86` (`WXL_WOW_EXE`) | the recompiled bodies of `src/deinlined/` against a stock Wow.exe's own code, on model files of its own: same answers, same bytes, the points fired |
| `python core/tools/seams/check_conventions.py` | every `conv` column agrees with `Wow.exe`, the x86 points' and their parents' (`include/wxl/points/x86.def`), the x86 binding's calls (`include/wxl/core/binding/calls_x86.def`) and the stock functions the recompiled bodies of `src/deinlined/` call included; each body calls the functions and names the globals Wow.exe's own parent does. Reads a stock `Wow.exe` 12340 (`--binary` or `$WXL_WOW_EXE`) with llvm-objdump |
| `python core/tools/fields/generate_update_fields.py --check` | the SDK's update-field table is what the fork gives; with `--binary`, what `Wow.exe`'s own tables hold. Runs in the fork's build |
| `ctest` `vk` in `core/build/win-x64` and `core/build/win-x86`, and natively `core/tests/vk/` | the Vulkan renderer's lower modules on the machine's driver (Wine's winevulkan included): the floor and the choice among devices, a device made and refused, VMA, the timeline and a wait that runs out, an image cleared by dynamic rendering and read back; the swapchain's rules, the limiter, the graveyard, the own SPIR-V; the present pass read back (stretch, gamma), the frame loop with 1-3 frames in flight and a resized scene, a lost device, Dear ImGui drawn offscreen, a real swapchain on a headless surface where the driver has one (lavapipe natively); the capture reader; the engine's draws read out of a made-up device and recorded (formats, blend modes, alpha test, scissor, culling, depth, stages, the interface's programs, pools and textures written again inside a frame, mips, cubes; client programs translated with registers that change between draws, a texture target, a copy from the scene, an occlusion query, fixed-function lighting, fog and clip plane; the back buffer read in the middle of a frame, a multisampled scene resolved for the screen, a copy and a readback, point sizes clamped as D3D9's; pipelines compiled on workers with draws left out meanwhile, linked from parts and replaced by their whole compile, compiled ahead from the last launch's list; the software cursor in a screenshot texel for texel, one sample and multisampled, an overlay under it; `GxRs_Multisample 0` by D3D9's pixel-centre rule where the device has sample locations, else drawn antialiased and counted) and compared with D3D9's; the translator against a reference interpreter of D3D9's instruction set (every arithmetic instruction, modifiers, masks, relative addressing, loops, branches, texture reads, texkill, alpha test, fog, clip), its refusals, its cache and pack; captures made up in the client's format replayed and compared with their pictures; natively, every program held to `spirv-val`, and `wxl-vk-corpus` over a whole extracted shader corpus |
| `wxl-core-seam-harness --profile-live`, with Tracy's `tracy-capture -a 127.0.0.1` | the profiler's zones, plots, messages and frames reach a viewer, from a harness under Wine |
| `sdk/tests/ingame/` in a client (`wxl-ingame` assembles it as a Mods folder) | in the real game, on each runtime: the test mods' functions called from the client's Lua, chat and CVar events, hot reload and handler order, a broken and repaired manifest, casts, a cooldown, an item use, an equipment change, `/reload` and a logout; `wxl.selftest` logs PASS or FAIL per check |
| `python core/tools/dbs/generate_client_dbs.py --check` | `include/wxl/core/client_dbs.def` lists every client database the fork declares, with its file and row size |

The `ctest` suites are Windows executables; from Linux they build with clang-cl and run under Wine
("Run the tests from Linux" in `core/README.md` and `sdk/README.md`).

## Words

| Word | Means |
|---|---|
| seam | a client function the runtime hooks, named `<Domain>.<Function>` |
| point | an engine function mods attach Before, Replace or After handlers to, named as the fork names it (`CGCamera::UpdateCallback`) |
| site | an inlined copy of a point's function on x86, reached by a mid-function hook: Before handlers only |
| de-inlined point | a point Wow.exe inlined, real on x86 in its parents' recompiled bodies, which take the parents' places while the point has handlers |
| engine header | a struct of `wxl::engine` (sdk/engine), the client's own type laid out for x86 and x64, with its layout hash |
| gate | a seam that can stop the call or rewrite its arguments |
| binding | the facts the runtime reads out of the client (`wxl::core::binding`) |
| `ObjectBinding` | the reads `wxl.object` asks a runtime for (`sdk/runtime`) |
| attachment | how one architecture reaches the client (`src/runtime/Attachment*.cpp`) |
| family | a script class grouping events: `WorldScript`, `RenderScript`, ... |
| stage | a step of the boot reported to every mod |
| handle | a mod's reference to a client object for one visibility episode: from its `ObjectCreate` to its `ObjectDestroy`, then stale; an object that comes back gets a new one, and its guid is what lasts |
