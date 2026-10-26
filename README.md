<div align="center">

# WarcraftXL core

**The runtime that loads WarcraftXL mods into World of Warcraft 3.3.5a**

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Client](https://img.shields.io/badge/client-3.3.5a%20(12340)-orange.svg)](#-installing)
[![ABI](https://img.shields.io/badge/ABI-0.1.33-blue.svg)](https://github.com/WarcraftXL/sdk)
[![Targets](https://img.shields.io/badge/targets-x86%20%7C%20x64-lightgrey.svg)](#-overview)

</div>

---

> [!IMPORTANT]
> The 32-bit build replaces the DLL of any earlier WarcraftXL framework installed beside the
> client: the file name, the export and the patch section are the same, so only one can be present.
> Move the old one aside before copying this one over.

## 🚀 Overview

One codebase, built twice, so a mod written against the [SDK](https://github.com/WarcraftXL/sdk)
runs on both the client you own and the client we compile.

| | `bin/x86/WarcraftXL.dll` | `bin/x64/WarcraftXL.dll` |
|---|---|---|
| loads into | the stock `Wow.exe` 3.3.5a, build 12340 | [`WarcraftXL/client`](https://github.com/WarcraftXL/client), the recompiled client |
| gets in by | an import and an entry trampoline the patcher adds | the client calling `WarcraftXLInit` |
| attaches to the game by | a detour at a resolved address | the client's own source calling in |

Above that line the two are the same logic: the mod host, manifests and admission, the event bus,
the lifecycle, the log.

### Key features

- **Loads before the client does.** The patcher points the executable's entry at a 19-byte
  trampoline, so mods load and `WXL_STAGE_EARLY` runs on the main thread before the client's first
  instruction — early enough to change what the client reads while starting.
- **Chained detours.** Several mods may hook the same function; `original` walks the chain and any
  link can detach, including the first, while calls are in flight.
- **Byte patches that refuse to be wrong.** `wxl.patch` takes the bytes it expects to find and
  declines, with a log line, when memory says otherwise. Every write is journalled and reversible.
- **Addresses that fail at compile time.** Nothing is hardcoded: sites resolve by name against
  tables generated from the decompiled client, so a renamed function breaks the build instead of
  the game.
- **Seams declared once.** Every function the runtime attaches to is one row in
  `include/wxl/seams/`, whose body is written once: the x86 build generates a detour around the
  function from the row, the x64 build exports the entry points for the recompiled client to
  call from inside it. A row is observing (`Before`, `After`, or only one of them) or gated
  (`Gate`, which can stop the call and rewrite its arguments).
- **An unbuffered log.** One write per line, so a crash keeps everything up to it.

How the pieces fit, and which files a new seam, fact or event touches:
[ARCHITECTURE.md](ARCHITECTURE.md).

## 📋 Table of contents

- [Overview](#-overview)
- [Building](#-building)
- [Installing](#-installing)
- [What this runtime publishes](#-what-this-runtime-publishes)
- [The mods it ships](#-the-mods-it-ships)
- [Layout](#-layout)
- [Troubleshooting](#-troubleshooting)
- [Contributors](#-contributors)
- [License](#-license)

## ⚡ Building

### Prerequisites

- Visual Studio 2022 or newer, with the x86 **and** x64 toolsets
- CMake 3.21 or newer
- A checkout of [`WarcraftXL/sdk`](https://github.com/WarcraftXL/sdk) beside this one, or
  `-DWXL_SDK_DIR=<path>`

### Build both architectures

```sh
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Release
```

No `-A`: the top level is a driver that configures the real build once per architecture and
collects the result in `build/dist/`:

```
build/dist/
    bin/x86/WarcraftXL.dll
    bin/x86/wxl-core-patcher.exe
    bin/x64/WarcraftXL.dll
    Mods/wxl.glue-unlock/{mod.json,bin/x86/glue-unlock.dll,bin/x64/glue-unlock.dll}
    Mods/wxl.named-archives/{mod.json,bin/x86/named-archives.dll}
    Mods/wxl.lua/{mod.json,LICENSE-LuaJIT.txt,bin/x86/wxl-lua.dll,bin/x64/wxl-lua.dll}
```

`Mods/wxl.lua/` is the runtime of the Lua mods, built from the SDK's `lua/` for each architecture,
so that a Lua mod runs with nothing else to build. The first configure downloads LuaJIT, which it
embeds, at a fixed commit and builds it with the Visual Studio in use. `-DWXL_CORE_LUA=OFF` leaves
it out.

### Build one

```sh
cmake -S . -B build/x86 -G "Visual Studio 18 2026" -A Win32 -DWXL_CORE_ARCH=win-x86
```

`WXL_CORE_TARGETS` selects which architectures the driver builds; `WXL_CORE_DIST` moves the
collection directory.

### Run the tests from Linux

The seam harness is a Windows executable. From Linux it builds with clang-cl and runs under Wine,
one architecture per tree. Give the driver a toolchain file that drives clang-cl and lld-link
against the MSVC CRT and the Windows SDK (which [xwin](https://github.com/Jake-Shadle/xwin)
downloads), and an emulator for ctest to run the harness with, both as environment variables so
that the per-architecture tree the driver configures gets them too (CMake 3.28 or newer):

```sh
export CMAKE_TOOLCHAIN_FILE=<toolchain for i686-pc-windows-msvc>
export CMAKE_CROSSCOMPILING_EMULATOR=<path to the wrapper below>
cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release -DWXL_CORE_TARGETS=win-x86
cmake --build build-x86
ctest --test-dir build-x86/win-x86 --output-on-failure
```

`wxl.lua` cannot build LuaJIT there (it uses LuaJIT's `msvcbuild.bat`): give it a static LuaJIT
built for the architecture, `-DWXL_LUAJIT_LIBRARY=<dir>/lua51.lib -DWXL_LUAJIT_INCLUDE_DIR=<dir>`
for one architecture, or `WXL_LUAJIT_LIBRARY_X86`, `WXL_LUAJIT_INCLUDE_DIR_X86` and their `_X64`
twins for both. The SDK's README, "Run the tests from Linux", builds one.

The harness lives in the architecture's own tree, `win-x86` or `win-x64` under the driver's. The
wrapper runs its arguments under Wine; 32-bit Wine is needed for x86:

```sh
#!/bin/sh
export WINEDEBUG="${WINEDEBUG:--all}"
exec wine "$@"
```

The SDK's suites run the same way; its README says how to give them LuaJIT.

ctest's `vk` runs the Vulkan renderer's lower modules on the machine's driver: the device floor
and choice, VMA, a submission and a bounded wait, an image cleared and read back, the present pass,
and the engine's draws (every texture format, the blend modes, the alpha test, the scissor, culling,
depth, fixed function's stages, lighting, fog and clip plane, the interface's programs, pools and
textures written again inside a frame, mips, cubes, a texture target, a copy from the scene, an
occlusion query) read back and compared with what D3D9 shows; the translator of the client's DX9
programs against a reference interpreter of D3D9's instruction set; and frame captures made up in
the client's format drawn again and compared with their D3D9 pictures. Under Wine it
runs through winevulkan on the host's driver (Mesa's lavapipe on a machine without a GPU, 32-bit
lavapipe for x86). Wine with no display behind it lists `VK_KHR_win32_surface` but refuses it at
`vkCreateInstance`, so the test then goes on headless and says so. A machine without a Vulkan 1.3
device skips it. The same program builds natively for Linux, from the same module rules, where it
also holds every program the renderer makes to `spirv-val` (SPIRV-Tools, built there only), and
`wxl-vk-replay <capture folder>` draws a real capture again and writes what it drew, the D3D9
picture and their difference beside it, and `wxl-vk-corpus <folder>` translates every record of a
shader corpus extracted from the client's archives (`<folder>/<Vertex|Pixel>/<profile>/<name>.bls`),
holds each to `spirv-val`, and with `--device` makes each a module and a pipeline on the driver
(ctest runs it when `WXL_VK_CORPUS` names the folder):

```sh
cmake -S tests/vk -B build-vk -G Ninja && cmake --build build-vk && ctest --test-dir build-vk
```

## 📦 Installing

### Into the stock client (32-bit)

1. Close the game.
2. Copy `build/dist/bin/x86/WarcraftXL.dll` next to `Wow.exe`.
3. Patch the executable, once:
   ```sh
   wxl-core-patcher.exe Wow.exe
   ```
   It writes `Wow.exe.orig` beside it and is idempotent: run it again and it says so, byte for
   byte. Pass a second path to write elsewhere and leave the input alone.
4. Copy the mods you want into `Mods/`, each as its own folder with a `mod.json` and
   `bin/x86/<name>.dll`, and `build/dist/Mods/wxl.lua/` with them for Lua mods, which are folders
   with an `init.lua`.
5. Launch. The log is `Logs/WarcraftXL.log`, beside the executable; earlier launches are kept
   beside it, and `WarcraftXL.ini` sets its level (the SDK's `docs/the-log.md`) and turns the
   Tracy profiler on, off by default (`[profile]`, the SDK's `docs/profiling.md`).
   `python3 tools/read_logs.py <game folder>` summarises the last launches.
6. Optionally, Vulkan: `[gx] api = vulkan` in `WarcraftXL.ini` makes the Vulkan device beside the
   client's own D3D9 one once it is up, and logs which GPU it took and what it offers, or why the
   machine has none (no loader, a driver below Vulkan 1.3, a missing feature). The stock client is
   not drawn through Vulkan yet: the device is released and the client renders through D3D9 as
   before. `validation = 1` turns Vulkan's validation layer on where it is installed; `gpu = <index
   or a part of the name>` picks the device on a machine with several; `frames_in_flight = 1..3`
   (2), `present_filter = linear|nearest` (linear) and `async_pipelines = 0|1` (1: a draw whose
   pipeline is still compiling is left out rather than waited for) serve the recompiled client.

A client already patched by an earlier WarcraftXL only needs the trampoline added; the patcher
detects that and says `upgraded`. Without the trampoline the runtime still works, on its own
thread, and says so in the log.

### Into the recompiled client (64-bit)

The client builds this directory as a subdirectory of its own tree (`-DWXL_CORE_DIR` when it is
not beside it), links the DLL and calls `WarcraftXLInit` from
[`include/wxl/core/init.h`](include/wxl/core/init.h), handing over the accessors of
`wxl_core_init`: one per row of [`include/wxl/core/binding.def`](include/wxl/core/binding.def), the
client binding, which `client/WarcraftXL/Binding.cpp` defines against the client's own classes.
Its functions call the seams' entry points from
[`include/wxl/seams/seams.h`](include/wxl/seams/seams.h). Mods are read from `Mods/` beside the
executable, with `bin/x64/` binaries.

Its Vulkan device, `CGxDeviceVk`, is an adapter over this runtime's renderer, reached through the
table of [`include/wxl/core/gx_vk.h`](include/wxl/core/gx_vk.h). It is chosen by `gxApi Vulkan`,
`-vulkan`, or `WarcraftXL.ini`'s `[gx] api = vulkan`, which also takes a D3D9Ex request
(`[gx] api = d3d9` takes any Vulkan request back to D3D9). The client then renders through Vulkan:
its window, the swapchain, the frame's clear, the world and the game's interface, the textures the
engine draws into (the minimap, portraits, full-screen effects), Dear ImGui and the pointer. The
client's shaders are its own records translated from DX9 bytecode to SPIR-V on the player's
machine, once each (kept in `WarcraftXL\cache\vk\`, with the pipelines compiled); a record the
translator refuses leaves its draws out, counted. The pipelines are compiled on worker threads
(linked at once from parts where the driver has graphics pipeline libraries); until one is ready,
a draw into the frame that needs it is left out rather than waited for, which
`[gx] async_pipelines = 0` turns into a wait. The pipelines drawn are kept beside the cache and
compiled ahead at the next launch. `gxMultisample` gives a multisampled frame, resolved for the
screen, the copies of the full-screen effects and the readbacks; the screenshot key and the
portraits read the frame back as D3D9 does. A machine or a window Vulkan cannot serve keeps
D3D9, with one line in the log. D3D9 stays the
default. The console commands `gxVkSimulateLoss` (the device's loss and return, under Vulkan) and
`gxCapture` (the next frame of the D3D9 device recorded to `WarcraftXL\captures\` in the format of
[`include/wxl/core/gx_capture.h`](include/wxl/core/gx_capture.h)) are for testing the renderer.

## 🔌 What this runtime publishes

What a mod can reach **today**, by build:

| | x86 | x64 |
|---|---|---|
| `wxl.point` (engine points) | yes: each point of `include/wxl/points/points.txt` with an x86 body, a SafetyHook detour armed at its first handler and removed at its last, its first bytes checked by Zydis; Before, Replace and After. A point Wow.exe inlined (a `deinlined` line) runs in the recompiled bodies of the functions it was inlined into (`src/deinlined/`), which take those functions' places while the point has handlers | yes: each point, a `WXL_POINT_<n>` line at the head of its function in the fork, one byte tested while unattached; the `// INLINED` ones too |
| `wxl.hook` (named points) | yes: the seams and the points of `src/hook/Points.def`, detourable | no: a seam is a call the client makes, not a function a detour can replace |
| `wxl.hook.raw` (raw addresses) | yes | never: no fixed addresses in a program we compile |
| `wxl.patch` (byte patches) | yes | never: nothing to patch in a program we compile |
| `wxl.object` (world objects) | yes, with every descriptor field by index: read, write in the client, watch through the client's mirror handlers, each watch its mod's and its changes raised to that mod only, the handler taken back with the last watch | the same |
| `wxl.script` (the client's Lua) | yes: the client's own `lua_*` and `FrameScript_*` called at resolved addresses | yes: through the script rows of the binding |
| `wxl.storage` | yes, every member: exists, size, read, open/read_at/close, mount_archive; add_search_path and remove_search_path, whose folders (inside the client's folder) the client opens its files from, by a path it reads from the disk as it is, each its mod's and taken out when the mod is disabled or unloaded; redirect and unredirect, a table of names the open gate answers on every thread before `FileOpen`, each its mod's and taken back when the mod is disabled or unloaded; find, across every mounted archive's listfile and those folders | the same |
| `wxl.data` | yes: a client database served from a mod's own file | the same |
| `wxl.profile` | yes: zones, plots, messages and frame sets on the Tracy profiler's timeline, beside the runtime's own zones for every event and mod handler (`WXL_PROFILE`) | the same, and zones in the client's main loop |
| `wxl.footprint` | yes: Storm's allocations by source file while a mod counts them (detours of `SMemAlloc`, `SMemFree`, `SMemReAlloc`), the device's textures and buffers by pool, the sound and script heaps, each archive's tables, each client database, the address space; `MemoryPressure` | the same, from seam calls in the client's allocator |
| `wxl.m2file` (parsed model files) | yes: arrays read and pointed as addresses, memory anywhere, the shadow pass's bone animation; the loader's stages as `ModelParsed`, `SkinProfileParsed`, `ModelSequenceLoad` | the same, arrays as offsets from themselves and memory within 1 GB of them |
| `wxl.m2` (models) | yes: an object's model, `ItemDisplayInfo` rows, race prefixes, models made, hung (to follow the parent's bones or on one key bone too), shown, textured, kept on the interface's copies; `AppearanceChanged`; models of the world placed as the client places its spell effects (a static map entity, `CMap::ObjectCreateStatic`), animated, faded, the ground under a point; bone watchers after `M2.AnimateMT` | the same, through the binding |
| `wxl.gx` (the D3D9 device) | yes: the device and a state invalidation; `ModelBatchDrawn` after every model batch, its numbers rebuilt from the skin section and the device's buffers and viewport | the same, the numbers as the device recorded them |
| `wxl.engine` (engine layouts) | yes: each type of the SDK's engine headers, its x86 layout hash | yes: its x64 hash, which the client's build holds its own types to |
| `wxl.camera` (the world camera) | yes: its position, facing rows (through the parent's rotation on a vehicle or a transport), field of view and pivot read at measured offsets; a mod's view written at the end of `CGCamera::UpdateCallback`, after `CameraUpdate`, its roll turning the facing's up row (which the view matrix and the pick are built from) | the same, through the binding |
| `wxl.input` (the pointer) | yes: held at the centre of the window, or at the point a mod chose, by `ClipCursor`, hidden through the device's `CursorSetVisible` while the game's own pointer state is left on; `MouseMotion` from raw input, `MouseWheel` from `WM_MOUSEWHEEL` at the window procedure's gate | the same |
| `wxl.world` (the world's geometry) | yes: `World::Intersect` | the same |
| `wxl.input`'s block of the mouse look | yes: gates on `CGInputControl::SetControlBit` and `UnsetControlBit` keep the look bits from the game, and a short click is done through `CGWorldFrame::PerformDefaultAction` | the same, gates in the fork |
| `wxl.pose` (heads) | yes: after `CGUnit_C::PreAnimate`, the head key bone's procedural transform (`CM2Model::SetBoneFlags`, `SetBoneProceduralTransform`) | the same |
| `wxl.imgui` (Dear ImGui, ImPlot) | yes: one context, drawn at the present with the D3D9 device (the SDK's copy and its Win32 and DX9 backends), the window's messages fed at the window procedure's gate | the same |
| `wxl.highlight` (outline and glow) | yes: the batches of a highlighted object kept at `M2.DrawBatch` (bindings, vertex program, first texture and alpha test, and constants read from the engine's copy, the device being pure), drawn again after the world into two masks against the scene's depth with the game's alpha test, the device's bindings and registers put back as the engine believes them, the glow blurred at half size and the outline found in a full-screen pass, with programs compiled at build time (`src/gx/d3d9/`) | not yet: `supported` false, its reason the Vulkan renderer's render points to come |
| `wxl.selectioncircle` (the game's circles) | yes: around each drawer of a circle (the target's, a corpse's, the pet's target's, the move marker's) a mod's texture is put in `CGObject_C::s_selectionTexture`, and `ProjectTex::Render2d`'s gate skips a hidden circle or points the projection at a scaled box and another colour; textures made with the client's `CreateTexture` | the same, the calls in the fork |
| `wxl.texture` (SVG and PNG made into textures) | yes: an SVG rasterized (nanosvg) or a PNG decoded (stb_image) at the size asked, its groups restyled, written as an uncompressed BLP2 with its mips and served under `Interface\WXL\...` by a provider of the runtime's own, which every loader reads through the client's file stack; `WxlTexture` in both script contexts of the interface | the same |
| `wxl.cvar` (console variables) | yes: made with the client's own `CVar::Register`, their flags written as the client's bits, a callback of the runtime's (`OsValidateFunctionPointer` lets it through) holding ranges and read-only variables; registrations made before `CVar::Initialize` reads Config.wtf are made at its start, the account's and the character's made again after `CVar::DeleteAccountCVars` and `DeleteCharacterCVars`; `WxlRegisterCVar` in both script contexts of the interface | the same, the calls in the fork |
| `wxl.net`, `wxl.ui` | not yet; their events are raised, the service tables are not published | the same |
| events raised | the ones the seam table derives (below) | the same list |
| lifecycle stages | `EARLY`, `STORAGE`, `GRAPHICS`, `SCRIPT`, `READY`, `SHUTDOWN`, then the unload | the same |

### `wxl.script` and the two script contexts

The client runs one Lua state at a time and rebuilds it at every change of interface: the login
glue and the game each get a fresh context, and both are created and destroyed more than once in
a session (log in, log out, log in again, ReloadUI). A context is noted when
`FrameScript.Initialize` makes it, as `WXL_SCRIPT_CONTEXT_GAME` when `CGGameUI::m_loggingIn` or
`CGGameUI::m_initialized` is set at that moment and `WXL_SCRIPT_CONTEXT_GLUE` otherwise, and
announced with `ScriptVmReady` at its first `FrameScript.CreateEvents`: the interface installing
its event table, after the client registered its own functions and before its frames load.
`ScriptVmClosing` is raised from `FrameScript.Destroy` naming the same context. Two contexts the
client creates and flushes before any interface loads -- the one `InitializeClient` makes at
process start, and the one `CGGameUI::ShutdownGame` makes on logout -- install no event table, and
raise neither event: one glue screen is one `ScriptVmReady`.

**Nothing registered survives a context.** The Lua state is closed with it, which takes every
global and every event listener. The service forgets what it was told at `ScriptVmClosing` and
refuses `register_function`, `register_event`, `fire_event` and `run` with `WXL_E_FAILED` until
the next `ScriptVmReady`. A mod registers its functions and declares its events from a
`ScriptVmReady` handler, every time, and keeps nothing it got from the previous context.

Every call from Lua into a registered function runs under the crash guard (below): a function
that faults answers the script with an error naming the fault, and its mod is disabled.
`register_event` works by installing the client's own event table again with the declared names
appended behind it, so each has an index `fire_event` can signal through `FrameScript_SignalEvent`;
on the stock client that is the same `FrameScript_CreateEvents` call the client makes, reissued.

### The crash guard

Every call from the runtime into a mod -- `wxl_mod_query`, `wxl_mod_load`, `wxl_mod_stage`,
`wxl_mod_unload`, every event handler and every script function -- runs under a structured
exception handler (`sdk/host/guard.cpp`, MSVC only; a plain call elsewhere). A mod whose code
faults is logged with its id, what it was doing and the fault, and **disabled**: its event
subscriptions are dropped (the rest of the dispatch in progress included), its remaining stages
and its unload are skipped, the script functions it registered are taken back, and its binary is
left mapped rather than unloaded. One that faults in `wxl_mod_query` or `wxl_mod_load` is refused
instead. The client goes on.

A C++ exception is not a fault and never reaches the guard: the SDK's C++ layer catches it inside
the mod and logs it (the SDK's `docs/building-a-mod.md`, "What a throw costs"). One that leaves a
stage or the unload of a `WXL_MOD` mod makes the mod call `wxl_host.disable` with what it threw,
and the runtime disables it exactly as above.

What the guard does not do: it does not unwind the faulting frames (destructors do not run,
locks stay held), it does not withdraw the services the mod published (another mod may hold the
pointer, and calls into them will now reach code that faulted once), and it does not repair
anything the mod wrote before it faulted, in its own state or in the client's. Disabling a mod
is a judgement that the rest of the process is more likely intact than not; it is not a proof.

A mod that requires a service this runtime does not publish is refused before it loads, with the
service and version named in the log. One that merely asks gets nothing and carries on.

`attach` is refused on x64 because a seam there is two calls the client makes from inside a
function it compiled, not a function a detour can replace: there is no `original` to hand the mod
and nothing it could suppress. A mod that wants to sit at a seam subscribes to the events the seam
raises; one that needs a detour requires `wxl.hook` or `wxl.hook.raw` and is refused on x64 at the door, before it loads.

### The seams

One table per domain under [`include/wxl/seams/`](include/wxl/seams/), one row per function.
A row is one of four shapes:

| Shape | Entry points | What it can do |
|---|---|---|
| `WXL_SEAM_BEFORE` | `Before` | watch the call start |
| `WXL_SEAM_AFTER` | `After` | watch the call finish |
| `WXL_SEAM` | `Before`, `After` | both |
| `WXL_SEAM_GATE` | `Gate` | watch the call start, stop it and answer in its place, and rewrite what the function is given |

A row declares only the sides its body uses: on the recompiled client each side is a call in the
function's source, and an `After` is one at every return.

A gated row is used only where a promise of the contract needs it -- suppressing a message,
redirecting a file -- because it costs the recompiled client a real decision point in its own
source. Everything else observes.

| Point | Function | Raises |
|---|---|---|
| `World.EnterWorld` | `CGGameUI::EnterWorld()`, the player in the world (PLAYER_ENTERING_WORLD): after a login and a teleport to another map | `WorldEnter` after, with the map and the player's position, once per entry; not for a `/reload` |
| `World.LeaveWorld` | `CGGameUI::LeaveWorld()`, the player out of it (PLAYER_LEAVING_WORLD): a logout, a teleport to another map while the old map is loaded | `WorldLeave` before, with the map entered and where the world was loaded around; not for a `/reload` |
| `World.Preload` | `World::Preload(const wxl_vec3* position, int32_t show_loading)`, the loading screen of a teleport far across the same map | `WorldLeave` before, `WorldEnter` after, only when `show_loading` is set and the player is in the world: the far-sight, cinematic and game-object preloads reach the same function with it clear |
| `Event.DispatchUpdate` | `IEvtDispatchUpdate(void* context, uint32_t curr_time, float elapsed_sec)` (regparm: EDI) | `Update` before |
| `ObjectMgr.InitObject` | `InitObject(void* object, const void* create, uint32_t time)` (regparm: ESI, EAX, ECX) | `ObjectCreate` after |
| `ObjectMgr.GetUpdateObject` | `GetUpdateObject(uint64_t guid, int32_t* reenabled)`, the lookup every update of an object goes through | `ObjectCreate` after, with a new handle, for an object the client brought back from its lazy-cleanup set (it never passes `InitObject` again) |
| `ObjectMgr.ObjDelete` | `ObjDelete(void* object)` (regparm: EAX) | `ObjectDestroy` before |
| `ObjectMgr.DestroyRemovedObject` | `DestroyRemovedObject(void* object)` (regparm: ESI) | `ObjectDestroy` before, for an object still tracked |
| `ObjectMgr.InitializeShared` | `ClntObjMgrInitializeShared()`, as the client enters a world | after: sets the mirror handler of every watched field on its kind, which raises `ObjectFieldChanged` when an update changes it |
| `ObjectMgr.DestroyShared` | `ClntObjMgrDestroyShared()`, as the client leaves it and drops every kind's mirror handlers | nothing; notes the handlers gone until the next `InitializeShared` |
| `Unit.OnDeath` | `CGUnit_C::OnDeath()` | `UnitDeath` after |
| `Unit.OnAuraAdded`, `Unit.OnAuraRemoved` | `CGUnit_C::OnAuraAdded/OnAuraRemoved(uint32_t index, uint8_t a3, void* aura, int32_t spell_id)` | `UnitAuraChanged` after |
| `Player.UpdateInventoryComponent` | `CGPlayer_C::UpdateInventoryComponent(int32_t slot, const void* old_item)` | `EquipmentChanged` after, when the item id moved |
| `GameUI.SendTarget`, `GameUI.ClearTarget` | `CGGameUI::SendTarget(uint64_t guid)`, `CGGameUI::ClearTarget(uint64_t guid, int32_t a2)` | `TargetChanged` after, once per change of `m_lockedTarget` |
| `MapArea.PrepareChunk` | `CMapArea::PrepareChunk(int32_t chunk_x, int32_t chunk_y)` | `TerrainChunkBuild` before |
| `FrameScript.Initialize` | `FrameScript_Initialize(int32_t profile)` | nothing: after, notes the new context and which one CGGameUI's state implies |
| `FrameScript.Destroy` | `FrameScript_Destroy()` | `ScriptVmClosing` before, naming the context `CreateEvents` announced; nothing for a context never announced |
| `FrameScript.CreateEvents` | `FrameScript_CreateEvents(const char** names, uint32_t count)` | `ScriptVmReady` after, the first time in a context, with the new `lua_State*`; the installed table is handed to `wxl.script`, which reissues it with the declared events appended |
| `NetClient.ProcessMessage` (gated) | `NetClient::ProcessMessage(uint32_t time_received, CDataStore* message, int32_t)` | `MessageReceived` at the gate; suppressing it hides the message from the client entirely, which is how a mod serves an opcode the client does not know |
| `NetClient.Send` (gated) | `NetClient::Send(CDataStore* message)` | `MessageSent` at the gate; suppressing it stops the message being sent |
| `NetClient.HandleConnect` | `NetClient::HandleConnect()` | `Connected` after, once the client holds itself connected |
| `NetClient.HandleDisconnect` | `NetClient::HandleDisconnect()` | `Disconnected` before, while the connection is still the one being lost |
| `Input.WindowProc` (gated) | `OsWindowProc(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam)` | `Input` at the gate; writing `handled` answers 0 and the client's own procedure never sees the message. A message left to the client then raises `MouseMotion` (`WM_INPUT`, a raw mouse report) or `MouseWheel` (`WM_MOUSEWHEEL`, which a handler keeps from the client the same way) |
| `Storage.OpenFile` (gated) | `SFile::OpenEx(SArchive*, const char* name, uint32_t flags, SFileImpl** result)` | `FileOpen` at the gate, `redirect` holding what a mod's `wxl.storage` redirect answers; a path written into `redirect` is what the client opens instead, then a folder of the search path that wins the name |
| `Storage.OpenEx` | the same `SFile::OpenEx`, observed: on x86 a second link of the same chain, nearer the function than the gate | `FileOpened` after, at every return, with the name opened, whether a file came back and its size; nothing is read while nobody listens |
| `Storage.FindFile` (gated) | `System_SFile2::FindFile(name, path, size, flags, type, archive)`, where every name the client opens or looks for is resolved | nothing: a folder of the search path, a provider or served bytes that win the name answer it, and a kind some mod transforms is opened through the runtime's file layer |
| `Storage.FindFileResult` | the same function, observed after it: where the name was found, or that it was found nowhere | `FileMissing`, when no layer has the name the client opened or looked for |
| `Sound.PlaySoundKit` (gated) | `SI2::PlaySoundKit(int32_t kit, ...)` | `SoundPlay` at the gate; suppressing it keeps the sound from starting. Both of the client's overloads of this name end here, so a sound named by path arrives with the id its name resolved to |
| `Client.Destroy` | `WowClientDestroy()` | nothing, but it is where `WXL_STAGE_SHUTDOWN` and the unload run: the last moment at which a mod can be told anything |
| `ConsoleDevice.Initialize` | `ConsoleDeviceInitialize(const char* title, bool, bool)` | nothing, but its return is `WXL_STAGE_GRAPHICS`: the device is made after the first database is read and before the script context exists, which is the order the stages promise |
| `Scrn.OnPaint` | `OnPaint(const void* data, void* param)`, the paint handler | `FrameBegin` before, only when the device has a context and the window can be seen -- the test the paint makes itself |
| `Gx.ScenePresent` | `GxScenePresent()` | `FrameEnd` before, for the frame `Scrn.OnPaint` opened and only for it: the loading screen presents frames of its own, which raise nothing. The last draw list is flushed after it. About once a second while a mod watches the address space, `MemoryPressure` before, when its largest free block just fell under the threshold |
| `SimpleTop.OnLayerRender` | `CSimpleTop::OnLayerRender()` | `UiRenderEnd` after: every stratum of the interface is drawn, the world inside it |
| `InputControl.SetControlBit` (gated) | `CGInputControl::SetControlBit(uint32_t bit, uint32_t time)` | while a mod blocks the mouse look, its look bits are kept from the game and the press noted |
| `InputControl.UnsetControlBit` (gated) | `CGInputControl::UnsetControlBit(uint32_t bit, uint32_t time, int32_t keep_smoothing)` | the release of a look bit kept out, with the default action of a short click |
| `Unit.PreAnimate` | `CGUnit_C::PreAnimate(void* frame)` | After applies the head turns of `wxl.pose` |
| `Unit.RenderTargetSelection`, `Unit.RenderPetTargetSelection`, `Unit.RenderAutoTrackCursor`, `Corpse.RenderTargetSelection` | `CGUnit_C::RenderTargetSelection()`, `RenderPetTargetSelection()`, `RenderAutoTrackCursor()`, `CGCorpse_C::RenderTargetSelection()`: the circles under the target, the pet's target and the move marker | nothing: before, `wxl.selectioncircle` picks the look of the circle that comes and puts a mod's texture in the game's place; after, the game's goes back |
| `ProjectTex.Render2d` (gated) | `ProjectTex::Render2d(box, color, matrix, fade, a5, a6, polygon_offset)`, a texture projected onto the ground | nothing: inside a circle, one a mod hid is not drawn, and one it restyled is drawn with a scaled box and its colour; elsewhere (a spell's cursor, a trajectory) one load |
| `Camera.UpdateCallback` | `CGCamera::UpdateCallback(const void* data, void* param)` | Before puts back the field of view `wxl.camera` changed; After raises `CameraUpdate` for the world camera, then writes the view a mod set |
| `WorldFrame.RenderWorld` | `CGWorldFrame::RenderWorld(void* world_frame)` | `WorldRenderBegin` before, `WorldRenderEnd` after. Run from inside the interface's batches, before anything of the interface over it; not reached where there is no world. Before, `wxl.highlight` starts keeping its objects' batches; after, before `WorldRenderEnd`, it draws them |
| `WorldFrame.PerformDefaultAction` (gated) | `CGWorldFrame::PerformDefaultAction(MOUSEBUTTON button)` | `WorldClickRequest` with what the click met (a name plate, a model, the ground, a map object, nothing) and whether a spell waits for its target; a mod that refuses it keeps the game from acting on the click |
| `Gx.ReleaseD3dResources` | `CGxDeviceD3d::IReleaseD3dResources(int32_t)` | `SwapchainLost` before, with the size about to go: at both of the D3D9 device's resets, and when it is destroyed. A client on another device does not reach it. Dear ImGui's and `wxl.highlight`'s device objects go first |
| `Gx.NotifyOnDeviceRestored` | `CGxDevice::NotifyOnDeviceRestored()` | `SwapchainRestored` before, with the size the device came back at |
| `M2.DrawBatch` | `CM2SceneRender::DrawBatch()`, one plain batch of a model | `ModelBatchDrawn` after, with the numbers of the device's draw and everything it was drawn with still bound. The stock client keeps no record of its draw: x86 rebuilds the numbers from the skin section the renderer still points at and the device's buffers and viewport. One flag is read while nobody listens; two more while nothing is highlighted, else a batch of a highlighted object is kept for `wxl.highlight` |
| `M2.AnimateMT`, `M2.AnimateMTSimple` | `CM2Model::AnimateMT(...)` and `AnimateMTSimple(...)`, a model's bones posed for the frame, on the animate threads | nothing: after, a model `wxl.m2` hung to follow its parent's bones, or on one key bone, is posed from the parent's skeleton, then the bone watchers get the pose; one load while none is |
| `M2Shared.Initialize` | `CM2Shared::Initialize()`, a parsed model made ready | `ModelParsed` before, ahead of its skin profile's choice and its textures |
| `M2Shared.InitializeSkinProfile` | `CM2Shared::InitializeSkinProfile()`, a parsed skin profile sized for drawing | `SkinProfileParsed` before, with the model's header and the skin's |
| `M2Shared.FinishLoadingLowPrioritySequence` | `CM2Shared::FinishLoadingLowPrioritySequence(uint32_t sequence_index, uint8_t* buffer, uint32_t size)`, an external sequence file parsed against its model | `ModelSequenceLoad` before, with the sequence's unparsed bytes |
| `Component.AddItem` | `CCharacterComponent::AddItem(slot, display, visual)`, an item display put on a character appearance | `AppearanceChanged` (worn) after, once the client's own models for the slot hang on the body |
| `Component.RemoveItem` | `CCharacterComponent::RemoveItem(slot)` | `AppearanceChanged` (removed) before, while the display is still on the slot |
| `Component.FreeComponent` | `CCharacterComponent::FreeComponent(component)`, an appearance given back | `AppearanceChanged` (freed) before |
| `Component.RemoveVisuals` | `CCharacterComponent::RemoveVisuals(model)`, which strips the attachments of the copies the interface draws | nothing: the models `wxl.m2` hung on the stripped points, and copies of them, are hung back after |
| `Spell.Packet_Group_30` | `Packet_Group_30(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)`, the handler of `SMSG_SPELL_START` and `SMSG_SPELL_GO` | `SpellCastStart` or `SpellCastGo` before, by the opcode, read off the message before the client parses it: the caster, the object target, the source and destination points with their transports, the cast time or the hit and miss counts |
| `Spell.Packet_SMSG_SPELL_FAILURE` | `Packet_SMSG_SPELL_FAILURE(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)` | `SpellCastInterrupted` before, for any caster in view, with the reason the server sent |
| `Spell.SpellFailed` | `Spell_C_SpellFailed(PendingSpellCast* cast, const SpellRec* spell, int32_t reason, int32_t misc1, int32_t misc2, bool reset_cooldown)` | `SpellCastFailed` before: the local player's cast refused by the server or by the client's own checks |
| `Spell.CastSpell` (gated) | `Spell_C_CastSpell(CGUnit_C* caster, int32_t spell_id, const CGItem_C* item, uint64_t target, PendingSpellCast* cast, bool confirmed)` | `SpellCastRequest` at the gate; refusing it answers false and nothing is sent |
| `Item.UseItem` (gated) | `CGItem_C::UseItem(const uint64_t* target, int32_t confirmed)` | `ItemUseRequest` at the gate; refusing it answers 0 and the item's spell is never asked for |
| `FrameScript.vSignalEvent` | `vFrameScript_SignalEvent(uint32_t index, const char* format, va_list args)` | `UiEvent` before, when a mod listens: the event's name, its format, and its arguments rendered as text, one entry per `%`; `ChatAddonMessage` before `CHAT_MSG_ADDON` fired for an addon message of the server |
| `FrameScript.HandleError` | `FrameScript_HandleError(lua_State* state)`, the error handler of every protected call | `ScriptError` before, with the error value as the VM left it |
| `Spell.Packet_MSG_CHANNEL_START` | `Packet_MSG_CHANNEL_START(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)` | `SpellChannelStart` before, for a channel of some length |
| `Spell.Packet_MSG_CHANNEL_UPDATE` | `Packet_MSG_CHANNEL_UPDATE(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)` | `SpellChannelUpdate` before, or `SpellChannelStop` when no time is left, with the channel's spell as the client holds it on the unit |
| `Spell.Packet_SMSG_UNIT_SPELLCAST_START` | `Packet_SMSG_UNIT_SPELLCAST_START(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)`, the handler of opcode 0x14D | `SpellCastResume` before: a cast already running when its caster came into view |
| `Spell.Packet_SMSG_SPELL_DELAYED` | `Packet_SMSG_SPELL_DELAYED(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)` | `SpellCastDelayed` before, with the cast the client holds on the unit |
| `Spell.Packet_SMSG_SPELL_COOLDOWN`, `Spell.Packet_SMSG_ITEM_COOLDOWN` | `Packet_SMSG_SPELL_COOLDOWN(...)`, `Packet_SMSG_ITEM_COOLDOWN(...)`, the four handler arguments | `SpellCooldown` before, once per spell of the message, or once for the item |
| `Spell.Packet_Group_31` | `Packet_Group_31(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)`, the handler of `SMSG_COOLDOWN_EVENT`, `SMSG_CLEAR_COOLDOWN` and `SMSG_MODIFY_COOLDOWN` | `SpellCooldownChanged` before, by the opcode |
| `Spell.CancelSpell` | `Spell_C_CancelSpell(PendingSpellCast* cast, bool stop_recast, bool notify_server, int32_t reason)`, where every cancel of the player's own cast ends | `SpellCastCancel` before, with the cast's caster, spell and count, the reason, and whether the server may be told |
| `Combat.Packet_Group_29` | `Packet_Group_29(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)`, the handler of `SMSG_ATTACKSTART`, `SMSG_ATTACKSTOP`, `SMSG_ATTACKERSTATEUPDATE` and `SMSG_ENVIRONMENTALDAMAGELOG` | `CombatAttackStart`, `CombatAttackStop`, `CombatDamage` or `CombatMiss` before, by the opcode and whether the round landed |
| `Combat.HandleSpellLogMiss` | `CombatLog::HandleSpellLogMiss(CDataStore* msg, int32_t time_offset)` (regparm: EDI), where `SMSG_SPELLLOGMISS` ends on its own or inside `SMSG_COMBAT_LOG_MULTIPLE` | `CombatMiss` before, once per target |
| `Combat.HandleSpellNonMeleeDamageLog` | `CombatLog::HandleSpellNonMeleeDamageLog(CDataStore* msg, int32_t time_offset)` (regparm: EAX) | `CombatDamage` before |
| `Combat.HandleSpellHealLog` | `CombatLog::HandleSpellHealLog(CDataStore* msg, int32_t time_offset)` (regparm: EAX) | `CombatHeal` before |
| `Combat.HandlePeriodicAuraLog` | `CombatLog::HandlePeriodicAuraLog(CDataStore* msg, int32_t time_offset)` | `CombatDamage`, `CombatHeal` or `CombatEnergize` before, per entry, up to where the client stops |
| `Combat.HandleSpellLogEnergizeLog` | `CombatLog::HandleSpellLogEnergizeLog(CDataStore* msg, int32_t time_offset)` (regparm: EAX), where `SMSG_SPELLENERGIZELOG` ends on its own or inside `SMSG_COMBAT_LOG_MULTIPLE` | `CombatEnergize` before |
| `GameUI.NewZoneFeedback` | `CGGameUI::NewZoneFeedback(int32_t area_id, int32_t subzone_id, int32_t wmo_group_id, const char* zone_text, const char* subzone_text, const char* real_zone_text, int32_t inside)`, the one place the client learns the player's zone | `ZoneChanged` before, when the ids or the texts differ from what was last announced |
| `GameUI.HandleObjectTrackChange` | `CGGameUI::HandleObjectTrackChange(uint64_t guid, uint64_t prev_guid)` | `MouseoverChanged` before, once per change of the object under the mouse |
| `GameUI.HandleTerrainClick`, `GameUI.HandleSpriteClick` | `CGGameUI::HandleTerrainClick(const CTerrainClickEvent*)`, `CGGameUI::HandleSpriteClick(const CSpriteClickEvent*)`, the two ends of a click the world frame resolved | `WorldClick` before: the hit point and the game object for the terrain, the object and its position for a model |
| `ConsoleVar.InternalSet` | `CVar::InternalSet(const char* value, bool set_value, bool set_reset, bool set_default, bool set_needs_save)`, where every set of a console variable ends | `CVarChanged` before, when the call will change the value |
| `ConsoleVar.Initialize` | `CVar::Initialize(const char* file_name)`, which reads Config.wtf | nothing raised; before: `wxl.cvar`'s waiting registrations made; after: their latches set |
| `ConsoleVar.DeleteCharacterCVars` | `CVar::DeleteCharacterCVars()`, when the player leaves the world | nothing raised; after: the mods' character variables made again |
| `ConsoleVar.DeleteAccountCVars` | `CVar::DeleteAccountCVars()`, when an account logs in | nothing raised; after: the mods' account variables made again |
| `Unit.SendMovementUpdate` | `CGUnit_C::SendMovementUpdate(uint32_t time, uint32_t msg_id, float value, uint32_t index, uint64_t transport, uint8_t seat)` (the six-argument overload, at 0x0071F0C0), where every movement message of the local player is built and sent | `PlayerMoveStart` before for a start, a jump, an ascent or a descent, `PlayerMoveStop` for a stop or a landing |
| `Player.OnUpdateMoney` | `OnUpdateMoney(uint64_t guid, uint32_t offset, uint32_t count, const void* value, void* param)`, the mirror handler of `PLAYER_FIELD_COINAGE` | `PlayerMoneyChanged` before, with the previous value the mirror hands over and the one the field holds |
| `Player.Packet_Group_9` | `Packet_Group_9(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)`, the handler of `SMSG_LEVEL_UP_INFO` among the local player's other messages | `PlayerLevelUp` before, for that opcode |
| `Chat.Packet_SMSG_LOG_XPGAIN` | `Packet_SMSG_LOG_XPGAIN(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)` | `PlayerXpGain` before |
| `Glue.UpdateCurrentScreen` | `CGlueMgr::UpdateCurrentScreen(const char* screen)`, which every glue screen change goes through | `GlueScreenChanged` before, with the screen being left |
| `RealmConnection.HandleAuthResponse` | `RealmConnection::HandleAuthResponse(uint32_t msg_id, uint32_t time, CDataStore* msg)` | `GlueAuthResult` before |
| `RealmConnection.HandleCharEnum` | `RealmConnection::HandleCharEnum(uint32_t msg_id, uint32_t time, CDataStore* msg)` | `GlueCharacterList` before, then `GlueCharacter` per entry; none of them for a list cut short |
| `RealmConnection.RequestCharacterLogin` | `RealmConnection::RequestCharacterLogin(uint64_t guid, int32_t a2)`, the one request to enter the world | `GlueEnterWorld` before |
| `Login.RealmListResult` | `Login::RealmListResult(CDataStore* msg)`, the realm list as the Grunt logon link validated it or as the Battle.net login built it, read in the layout the login's server type names; on the logon connection's thread | `GlueRealmList` before, then `GlueRealm` per entry |
| `Chat.ChatMessageReceived` | `CGChat::ChatMessageReceived(CDataStore* msg, bool is_gm)`, where `SMSG_MESSAGECHAT` and `SMSG_GM_MESSAGECHAT` both end, and where the client drops what it will not show | nothing itself: Before reads the message, After forgets it |
| `Chat.AddChatMessage` | `CGChat::AddChatMessage(const char* text, int32_t type, ...)`, every line added to the chat | nothing itself: Before tells which received message the call adds, the one `ChatMessageReceived` read or one the client held, by the text it is handed |
| `Chat.QueueChatText` | `CGChat::QueueChatText(...)`, where the client holds a line until it knows a name or the world has loaded | nothing itself: After keeps the message for the held copy |
| `Chat.DestroyPendingChat` | `PENDINGCHAT::~PENDINGCHAT()`, a held line freed, added again or given up on | nothing itself: Before forgets what was kept for it |
| `Chat.RecordChat` | `CGChat::RecordChat(const char* text, int32_t type, ...)`, the commit of a line past every filter, right before the chat frames get it | `ChatMessage` before, for a line of a received message: its final type, the name it shows, its line id, its text as shown |
| `Chat.Script_SendChatMessage`, `Chat.Script_SendAddonMessage` (gated) | `Script_SendChatMessage(lua_State*)`, `Script_SendAddonMessage(lua_State*)`, the script functions every message from the interface goes through | `ChatSend` at the gate, with the script's arguments; suppressing it answers no result and nothing is sent |
| `Storage.OpenArchive` | `SFile::OpenArchive(const char* path, int32_t priority, uint32_t flags, SArchive** archive)`, every archive the client mounts | `ArchiveMounted` after, for an archive it opened, with its priority, its table sizes and what they hold |
| `Storage.CloseArchive` | `SFile::CloseArchive(SArchive* archive)` | `ArchiveUnmounted` before, while the archive is still whole |
| `ClientDb.Initialize` | `ClientDBInitialize()`, which loads every client database the client keeps | `ClientDbLoaded` after, once per database it left loaded, in the order the client declares them |
| `Texture.Create` | `TextureCreate(const char* file_name, CGxTexFlags, CStatus*, uint32_t flags)` (the file-name overload, at 0x004B9760), every texture asked for by name | `TextureLoad` before, cached or not; the x64 client skips the call while nobody listens |
| `M2Shared.LoadSucceededCallback` | `CM2Shared::LoadSucceededCallback(void* param)`, the end of a model file's read, before `FinishLoading` parses it | `ModelLoad` before, with the path and the bytes; bytes a handler answers are copied into a buffer of the client's allocator and parsed instead |
| `MapObj.Load` | `CMapObj::Load()`, which walks a map object's root once it is read | `MapObjectLoad` before, with the path and the root's bytes |
| `MapObj.CreateGroup` | `CMapObjGroup::Create()`, which walks a map object's group once its file is read | `MapObjectGroupLoad` before, with the root's name, the group's number and its bytes |
| `MapArea.AsyncCallback` | `CMapArea::AsyncCallback(void* param)`, the end of a terrain tile's read, before its chunks are walked | `TerrainTileLoad` before, with the tile's name, its place on the grid and its bytes |
| `M2Shared.FinishLoadingSkinProfile` | `CM2Shared::FinishLoadingSkinProfile(uint32_t size)`, the end of a skin profile's read, before it is parsed | `SkinProfileLoad` before, with the model's path and the skin's bytes |
| `Texture.AsyncTextureSuccessCallback` | `AsyncTextureSuccessCallback(CTexture*)`, the end of a texture file's read | nothing itself: notes the texture's name for `BLPFile.Source` |
| `BLPFile.Open` | `CBLPFile::Open(name, flags)`, a texture file read whole by name | nothing itself: notes the name for `BLPFile.Source` |
| `BLPFile.Source` | `CBLPFile::Source(const void* data)`, a texture file's bytes taken to decode | `TextureDecode` before, with the name its caller noted and what the BLP2 header says |
| `Gx.IShaderLoad` | `CGxDevice::IShaderLoad(shaders, target, path, name, permutations)`, a shader program loaded by name | `ShaderLoad` before, with the stage, the folder, the name and the permutations |
| `Sound.CreateWaveData` | `SE2::CreateWaveData(...)`, the sound data a sound plays from, made from a file | `SoundFileLoad` before, with the file, whether it streams and the channel's priority; never while the game's sound is off (`Sound_EnableAllSound` 0), when the client loads no sound |
| `Map.AllocDoodadDef` | `CMap::AllocDoodadDef()` | nothing itself: after, notes the def it handed out, which tells a placement from a def found in the map's hash |
| `Map.CreateDoodadDef`, `Map.CreateDoodadDefAt`, `Map.CreateDoodadDefInMapObj` | the three `CMap::CreateDoodadDef` overloads: a tile's doodad, one the game places at a position, one of a map object's doodad set | `DoodadSpawn` after, for a def the call allocated, with the model's path and the position |
| `ObjectMgr.FillInPartialObjectData` | `FillInPartialObjectData(CGObject_C*, WOWGUID, CDataStore*, bool, bool)`, where an update block's fields are written | nothing itself: after, notes a tracked object, once per packet |
| `ObjectMgr.Packet_SMSG_UPDATE_OBJECT` | `Packet_SMSG_UPDATE_OBJECT(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)`, the compressed form's too | `ObjectUpdate` after, once per object the packet's field writes noted and still tracked, in the packet's order |
| `Storm.SMemAlloc` | `SMemAlloc(size_t bytes, const char* filename, int32_t line, uint32_t flags)`, Storm's allocator, `new` included | nothing: after, the block is counted under its file while a mod has started `wxl.footprint`'s accounting |
| `Storm.SMemFree` | `SMemFree(void* ptr, const char* filename, int32_t line, uint32_t flags)` | nothing: before, the block leaves the counts |
| `Storm.SMemReAlloc` | `SMemReAlloc(void* ptr, size_t bytes, const char* filename, int32_t line, uint32_t flags)` | nothing: before, the block leaves the counts; after, what the call left comes back, under the file that moved it |
| `Storm.SErrDisplayError` | `SErrDisplayError(uint32_t errorcode, const char* filename, int32_t linenumber, const char* description, int32_t recoverable, uint32_t exitcode, uint32_t unknown)`, Storm's error display: every fatal error, assertion and the exception filter's dialog | nothing: before, the error goes to `WarcraftXL.log`, flushed; a fatal one with its stack, the running mod, the last events and the session's end line |
| `Os.ValidateFunctionPointer` (gated) | `OsValidateFunctionPointer(const void* ptr)`, the check of every C function the client's Lua calls, Lua hook, console command handler and CVar callback | nothing: the gate takes the call for a pointer in the runtime's image or a binary the mod host loaded; any other goes on to the client's check, fatal outside its `.text` |
| `FrameXML.CheckSignature` (gated) | `FrameXML_CheckSignature(const char* toc, const char* extra, const uint8_t* modulus, uint8_t* digest)`, the client's verdict on GlueXML.toc, FrameXML.toc and each add-on's .toc | `InterfaceSignature` with the .toc, the file hashed with it and the client's verdict, while a mod listens: the gate has the client judge, then answers its verdict, or signed for a file a mod accepted (the digest made as the client makes it where the verdict left it unwritten) |
| `AddOns.DeleteInterfaceFiles` (gated) | `DeleteInterfaceFiles()`, at the login screens' start: loose `Interface\GlueXML`, `Interface\FrameXML` and `Interface\AddOns\Blizzard*` folders renamed to `.old` | `InterfaceCleanupRequest`; a mod that refuses it keeps the folders |
| `Combat.UnitCombatLogAuraDose`, `Combat.UnitCombatLogAuraRefreshed` | `UnitCombatLogAuraDose(...)`, `UnitCombatLogAuraRefreshed(...)`, the combat log of an aura that stays in its slot | `UnitAuraRefreshed`, with the slot and the count before |
| `Unit.MountDisplayIDUpdateHandler` | the mirror handler of `UNIT_FIELD_MOUNTDISPLAYID` | `UnitMountChanged` |
| `Loot.SetObject` | `CGLootInfo::SetObject(CGObject_C*, int32_t coins, LOOT_ACQUIRE)`, the loot window put on an object or on nothing | `LootClosed` for the window it closes, `LootOpened` and a `LootSlot` per item for the new one |
| `Loot.ClearSlot` | `CGLootInfo::ClearSlot(uint8_t slot)` | `LootSlotCleared` |
| `Player.PH_LOOT_OPCODES` | the handler of the loot messages | `LootMoney`, `LootRollStart`, `LootRoll`, `LootRollWon`, `LootAllPassed` |
| `Player.Packet_Group_9` | (also) `SMSG_ITEM_PUSH_RESULT`, `SMSG_INVENTORY_CHANGE_FAILURE` | `ItemPushed`, `InventoryError` |
| `Container.InvUpdateHandler`, `Container.UpdateInvContents`, `Container.UpdateContainerContents` | the mirror handlers of the player's bag slots and of each bag's slots | `InventorySlotChanged` |
| `Item.OnUpdateDurability`, `Item.OnUpdateStackCount` | the mirror handlers of an item's durability and stack count | `ItemDurabilityChanged`, `ItemStackChanged` |
| `Player.OnUpdateQuest` | the mirror handler of a quest log entry | `QuestAccepted`, `QuestRemoved`, `QuestObjectiveChanged`, `QuestCompleted`, `QuestFailed` |
| `Player.Packet_Group_12` | the quest giver's messages | `QuestGiverGreeting`, `QuestGiverDetail`, `QuestGiverProgress`, `QuestGiverReward`, `QuestTurnedIn` |
| `Player.Packet_SMSG_GROUP_INVITE` (gated) | the invitation's handler | `GroupInvite`; an answered one is accepted or declined as the dialog does, which never shows |
| `Player.Packet_SMSG_GROUP_LIST`, `_UNINVITE`, `_DESTROYED` | the group list's handlers | `GroupUpdated`, `GroupMember`, `GroupLootMethodChanged`, `GroupLeft` |
| `Raid.Packet_MSG_RAID_READY_CHECK`, `_CONFIRM`, `_FINISHED` | the ready check's handlers | `ReadyCheck`, `ReadyCheckResponse`, `ReadyCheckFinished` |
| `Duel.Packet_SMSG_DUEL_REQUESTED` (gated), `_COUNTDOWN`, `_WINNER`, `_COMPLETE` | the duel's handlers | `DuelRequested` (answerable), `DuelCountdown`, `DuelWon`, `DuelFinished` |
| `Trade.Packet_SMSG_TRADE_STATUS` | the trade status handler | `TradeStatus` |
| `Player.Packet_SMSG_EMOTE`, `Chat.OnTextEmote` | the emote handlers | `Emote`, `TextEmote` |
| `Player.Packet_SMSG_RESURRECT_REQUEST` (gated) | the resurrection offer's handler | `ResurrectRequest` (answerable) |
| `GameUI.UpdateActivePlayer`, `Player.UpdateGhostStatus` | the player's state of life told to the interface, its ghost flag | `PlayerDied`, `PlayerReleased`, `PlayerResurrected` |
| `Reputation.Packet_SMSG_SET_FACTION_STANDING` | the standings' handler | `ReputationChanged` |
| `Achievement.Packet_SMSG_ACHIEVEMENT_EARNED` | the achievement's handler | `AchievementEarned` |
| `FrameScript.vSignalEvent` | (also) the interface events that open and close an NPC's window | `WindowOpened`, `WindowClosed` |

The frame's slots -- `WorldRenderBegin`, `WorldRenderEnd`, `UiRenderEnd` -- carry the index of the
frame they are drawn in, and are not raised outside one. `FrameBegin` and `FrameEnd` come in pairs:
the client presents more often than it paints a frame, so the runtime keeps the pairing rather
than trusting the client to.

A seam is named after the function it attaches to, never after what it means; the name is part of
the ABI and is appended, never renamed or dropped.

## 🧩 The mods it ships

Two mods that do what a patcher used to do, so the patcher only has to bootstrap.

| Mod | Clients | What it does |
|---|---|---|
| `wxl.glue-unlock` | x86, x64 | answers `InterfaceSignature` and `InterfaceCleanupRequest`: a changed GlueXML, FrameXML or Blizzard add-on is taken as signed and loads (an add-on without a `.sig` stays insecure, as the client judges it), and loose interface folders stay on disk; removed, the client checks as it always has |
| `wxl.named-archives` | x86 | widens the client's `patch-?.MPQ` scan to `patch-*.MPQ`, so archives and folders with real names are mounted; it requires `wxl.patch`, so another runtime refuses it cleanly |

`wxl.named-archives` reads memory before writing: if the bytes are already in the patched state it
logs `already present` and does nothing. `wxl.glue-unlock` writes nothing into the client: it
logs each file it took as signed (debug) and a count when the session ends.

## 📂 Layout

```
include/wxl/core/init.h          the 64-bit entry point, for the client to include
include/wxl/core/binding/        the client binding: the facts read out of the client, one .def per domain
include/wxl/seams/               the seam table (one .def per domain) and seams.h, their entry points
include/wxl/offsets/             site.hpp, layout.hpp and the generated tables
include/wxl/points/              points.txt, the list of wxl.point, and x86.def generated from it
include/wxl/core/points.h        how the recompiled client runs a point (x64)
src/runtime/                     host bring-up, stages, frames, the per-architecture attachment
src/binding/                     the binding realised: memory reads (x86), the client's accessors (x64)
src/seams/                       the seam bodies, one file per domain (both); the x86 detours and thunks
src/objects/                     wxl.object over the binding
src/services/                    wxl.script, wxl.storage, wxl.data (both); wxl.hook, wxl.hook.raw, wxl.patch (x86)
src/profile/                     the Tracy profiler and wxl.profile (both)
src/gx/vk/                       the Vulkan renderer, wxl::vk: device, resources, shaders, pipelines,
                                 frame, present, draw, imgui; no runtime, also built natively for Linux (both)
src/gx/vk/shaders/               the SPIR-V writer, the translator of the client's DX9 programs (Dx9, Translate,
                                 Interface) and its cache (Cache)
src/gx/vk/shaders/glsl/          the renderer's own programs, GLSL compiled to SPIR-V when it is built
src/gx/vk/gxfront/               the engine's draws read through the engine layouts (Engine.cpp, portable),
                                 and the Vulkan device behind one engine CGxDevice (Front.cpp, Windows)
src/gx/capture/                  a frame capture (include/wxl/core/gx_capture.h) read and drawn again
src/gx/                          the renderer in the runtime: [gx] api, the table of include/wxl/core/gx_vk.h
                                 for the recompiled client's CGxDeviceVk, the device on the stock client
src/points/                      wxl.point: handlers and dispatch (both), the detours (x86), the client's bytes (x64)
src/deinlined/                   functions of the fork recompiled for x86, in which the de-inlined points are
                                 real; generated by tools/extract/extract.py --deinlined   (x86)
src/hook/                        the detour engine and the named points        (x86)
src/patcher/                     PE import and entry trampoline                (x86)
src/common/                      the log
tests/seam_harness.cpp           every seam body against a fake client; --bench times the hot paths
tests/stock_tests.cpp            src/deinlined/ against a stock Wow.exe's own code, laid at its base    (x86)
tests/vk/                        the renderer's lower modules on the machine's Vulkan driver; its own
                                 CMakeLists.txt builds them natively for Linux, with spirv-val,
                                 wxl-vk-replay and wxl-vk-corpus
mods/                            the mods above                                (x86)
deps/minhook/                    MinHook, BSD-2-Clause, unmodified             (x86)
deps/safetyhook/                 SafetyHook v0.7.0, BSL-1.0, its sources; its near allocator also tries a
                                 free region's first reservable address           (x86)
deps/zydis/                      Zydis v4.1.0 and Zycore, MIT, the decoder     (x86)
tools/points/                    generate_points.py, insert_points.py, their self-test
deps/tracy/                      Tracy's client 0.14.1, BSD-3-Clause, unmodified
deps/vulkan-headers/             Vulkan-Headers vulkan-sdk-1.4.357.0, Apache-2.0 OR MIT, the C headers
deps/volk/                       volk vulkan-sdk-1.4.357.0, MIT, unmodified: the loader, at run time
deps/vma/                        Vulkan Memory Allocator v3.4.0, MIT, unmodified
deps/spirv-headers/              SPIRV-Headers vulkan-sdk-1.4.357.0, Khronos (MIT-style), its headers and grammars
deps/glslang/                    glslang vulkan-sdk-1.4.357.0, BSD-3-Clause and others, built for the build
                                 machine only: it compiles the renderer's GLSL, nothing of it ships
deps/spirv-tools/                SPIRV-Tools vulkan-sdk-1.4.357.0, Apache-2.0, built by the tests only (spirv-val)
deps/nanosvg/                    nanosvg 239e102 (2026-07-10), zlib, unmodified: SVG parsed and rasterized (wxl.texture)
deps/stb/                        stb_image 2.30 (stb 003501f), MIT or public domain, unmodified: PNG decoded (wxl.texture)
src/imaging/                     wxl.texture's pictures: SVG groups restyled, the resampler, the mips, the BLP2 writer
tests/imaging_tests.cpp          src/imaging/ pixel by pixel
tools/seams/                     check the seam table against the fork and against Wow.exe
tools/offsets/, tools/layouts/   regenerate the tables from the decompiled client
tools/fields/                    regenerate the SDK's update-field table from the decompiled client
tools/engine/                    the SDK's engine headers from the decompiled client (engine.txt lists the types)
tools/extract/                   a fork function as C++ for the stock client: a modder's template, or
                                 the runtime's de-inlined bodies
```

## 🔧 Troubleshooting

| Symptom | Cause |
|---|---|
| The game does not start, `0xc0000142` | `WarcraftXL.dll` is missing beside the executable, or is the wrong architecture |
| No log at all | the executable carries no WarcraftXL import: run the patcher |
| `the executable was patched by an older patcher` | it has the import but no trampoline: run the patcher again |
| A mod is missing from the log | wrong folder shape; it needs `Mods/<id>/mod.json` and `Mods/<id>/bin/<arch>/<name>.dll` |
| `refused … requires service …` | the mod needs something this build does not publish; see the table above |
| `not Wow.exe build 12340` | the addresses do not apply to this executable; named points and seams stay off |
| `attach 'World.Preload': refused` (x64) | a seam cannot be detoured on the recompiled client; subscribe to its events |
| `the client and this runtime were built from different binding tables` (x64) | the client and `WarcraftXL.dll` come from different trees; the line names the first row of `binding.def` that differs. Rebuild both from one tree |
| `seam <Domain.Name> not attached: x64 only: ...` (x86) | expected: the seam's body needs what only the recompiled client keeps (a `WXL_SEAM_X64_ONLY` row; none today); its events are not announced there and a subscription to them is refused |
| `<point>: <side> on <point> refused: ...` (tag `point`) | the line says why: the point is not on this runtime (`x64 only: ...`), it is observe only, the mod was built against another signature of it (rebuild it), or on x86 its first bytes cannot take a detour |
| `WarcraftXLPoints: ... different point tables` (x64) | the client and `WarcraftXL.dll` come from different point lists; rebuild both from one tree. No point can be attached until then |
| `the client handed no <row> accessor; what reads it answers absent` (x64) | the client was built against an older `init.h` than this runtime; the row of `binding.def` it names answers its absent value (map `-1`, no position, ...) until the client passes one |

## 🤝 Contributors

Tyrallis brought the model services (`wxl.m2file`, `wxl.m2`, `wxl.gx`) and the character
appearance and model seams they stand on (pull request #1 of sdk, core and client), then
`wxl.m2`'s models of the world, its bone watchers and the cast's ground point (pull requests #4).

## 📄 License

MIT. See [LICENSE](LICENSE).

MinHook, under `deps/minhook/`, is BSD-2-Clause and keeps its own license file. Tracy's client,
under `deps/tracy/`, is BSD-3-Clause and keeps its own license file. SafetyHook, under
`deps/safetyhook/`, is BSL-1.0, and Zydis with Zycore, under `deps/zydis/`, are MIT; each keeps its
own license file. nanosvg, under `deps/nanosvg/`, is under the zlib license, and stb_image, under
`deps/stb/`, is MIT or public domain; each keeps its license file.

<div align="center">

[⬆ Back to top](#warcraftxl-core)

</div>
