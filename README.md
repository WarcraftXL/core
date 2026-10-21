<div align="center">

# WarcraftXL core

**The runtime that loads WarcraftXL mods into World of Warcraft 3.3.5a**

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Client](https://img.shields.io/badge/client-3.3.5a%20(12340)-orange.svg)](#-installing)
[![ABI](https://img.shields.io/badge/ABI-0.5-blue.svg)](https://github.com/WarcraftXL/sdk)
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
    Mods/wxl.glue-unlock/{mod.json,bin/x86/glue-unlock.dll}
    Mods/wxl.named-archives/{mod.json,bin/x86/named-archives.dll}
```

### Build one

```sh
cmake -S . -B build/x86 -G "Visual Studio 18 2026" -A Win32 -DWXL_CORE_ARCH=win-x86
```

`WXL_CORE_TARGETS` selects which architectures the driver builds; `WXL_CORE_DIST` moves the
collection directory.

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
   `bin/x86/<name>.dll`.
5. Launch. The log is `Logs/WarcraftXL.log`, beside the executable.

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

## 🔌 What this runtime publishes

What a mod can reach **today**, by build:

| | x86 | x64 |
|---|---|---|
| `wxl.hook` (named points) | yes: the seams and the points of `src/hook/Points.def`, detourable | no: a seam is a call the client makes, not a function a detour can replace |
| `wxl.hook.raw` (raw addresses) | yes | never: no fixed addresses in a program we compile |
| `wxl.patch` (byte patches) | yes | never: nothing to patch in a program we compile |
| `wxl.object` (world objects) | yes | yes |
| `wxl.script` (the client's Lua) | yes: the client's own `lua_*` and `FrameScript_*` called at resolved addresses | yes: through the script rows of the binding |
| `wxl.storage` | yes: exists, size, read, open/read_at/close, mount_archive | the same |
| `wxl.data` | yes: a client database served from a mod's own file | the same |
| `wxl.gx` (the D3D9 device, model batches) | no | yes: the device and the batches `M2.DrawBatch` hands over |
| `wxl.m2file` (model files as the loader reads them) | no | yes: the file at the four `M2` loader seams, memory an M2 array can reach, the shadow pass's bone animation |
| `wxl.net`, `wxl.ui` | not yet; their events are raised, the service tables are not published | the same |
| events raised | the ones the seam table derives (below) | the same list |
| lifecycle stages | `EARLY`, `STORAGE`, `GRAPHICS`, `SCRIPT`, `READY`, `SHUTDOWN`, then the unload | the same |

### `wxl.script` and the two script contexts

The client runs one Lua state at a time and rebuilds it at every change of interface: the login
glue and the game each get a fresh context, and both are created and destroyed more than once in
a session (log in, log out, log in again, ReloadUI). `ScriptVmReady` is raised from
`FrameScript.Initialize` with `context` set to `WXL_SCRIPT_CONTEXT_GAME` when
`CGGameUI::m_loggingIn` or `CGGameUI::m_initialized` is set at that moment and to
`WXL_SCRIPT_CONTEXT_GLUE` otherwise; `ScriptVmClosing` is raised from `FrameScript.Destroy`
naming the same context. Two contexts the client creates and flushes before any interface loads
-- the one `InitializeClient` makes at process start, and the one `CGGameUI::ShutdownGame` makes
on logout -- are announced as glue.

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
| `World.Preload` | `World::Preload(const wxl_vec3* position, int32_t show_loading)` | `WorldLeave` before, `WorldEnter` after, only when `show_loading` is set: the far-sight, cinematic and game-object preloads reach the same function with it clear |
| `Event.DispatchUpdate` | `IEvtDispatchUpdate(void* context, uint32_t curr_time, float elapsed_sec)` (regparm: EDI) | `Update` before |
| `ObjectMgr.InitObject` | `InitObject(void* object, const void* create, uint32_t time)` (regparm: ESI, EAX, ECX) | `ObjectCreate` after |
| `ObjectMgr.ObjDelete` | `ObjDelete(void* object)` (regparm: EAX) | `ObjectDestroy` before |
| `ObjectMgr.DestroyRemovedObject` | `DestroyRemovedObject(void* object)` (regparm: ESI) | `ObjectDestroy` before, for an object still tracked |
| `Unit.OnDeath` | `CGUnit_C::OnDeath()` | `UnitDeath` after |
| `Unit.OnAuraAdded`, `Unit.OnAuraRemoved` | `CGUnit_C::OnAuraAdded/OnAuraRemoved(uint32_t index, uint8_t a3, void* aura, int32_t spell_id)` | `UnitAuraChanged` after |
| `Player.UpdateInventoryComponent` | `CGPlayer_C::UpdateInventoryComponent(int32_t slot, const void* old_item)` | `EquipmentChanged` after, when the item id moved |
| `GameUI.SendTarget`, `GameUI.ClearTarget` | `CGGameUI::SendTarget(uint64_t guid)`, `CGGameUI::ClearTarget(uint64_t guid, int32_t a2)` | `TargetChanged` after, once per change of `m_lockedTarget` |
| `MapArea.PrepareChunk` | `CMapArea::PrepareChunk(int32_t chunk_x, int32_t chunk_y)` | `TerrainChunkBuild` before |
| `FrameScript.Initialize` | `FrameScript_Initialize(int32_t profile)` | `ScriptVmReady` after, with the context CGGameUI's state implies and the new `lua_State*` |
| `FrameScript.Destroy` | `FrameScript_Destroy()` | `ScriptVmClosing` before, naming the context `Initialize` announced |
| `FrameScript.CreateEvents` | `FrameScript_CreateEvents(const char** names, uint32_t count)` | nothing: after, the installed table is handed to `wxl.script`, which reissues it with the declared events appended |
| `NetClient.ProcessMessage` (gated) | `NetClient::ProcessMessage(uint32_t time_received, CDataStore* message, int32_t)` | `MessageReceived` at the gate; suppressing it hides the message from the client entirely, which is how a mod serves an opcode the client does not know |
| `NetClient.Send` (gated) | `NetClient::Send(CDataStore* message)` | `MessageSent` at the gate; suppressing it stops the message being sent |
| `NetClient.HandleConnect` | `NetClient::HandleConnect()` | `Connected` after, once the client holds itself connected |
| `NetClient.HandleDisconnect` | `NetClient::HandleDisconnect()` | `Disconnected` before, while the connection is still the one being lost |
| `Input.WindowProc` (gated) | `OsWindowProc(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam)` | `Input` at the gate; writing `handled` answers 0 and the client's own procedure never sees the message |
| `Storage.OpenFile` (gated) | `SFile::OpenEx(SArchive*, const char* name, uint32_t flags, SFileImpl** result)` | `FileOpen` at the gate; a path written into `redirect` is what the client opens instead. `FileOpened` is not raised: it carries the result, and this function has a dozen returns |
| `Sound.PlaySoundKit` (gated) | `SI2::PlaySoundKit(int32_t kit, ...)` | `SoundPlay` at the gate; suppressing it keeps the sound from starting. Both of the client's overloads of this name end here, so a sound named by path arrives with the id its name resolved to |
| `Client.Destroy` | `WowClientDestroy()` | nothing, but it is where `WXL_STAGE_SHUTDOWN` and the unload run: the last moment at which a mod can be told anything |
| `ConsoleDevice.Initialize` | `ConsoleDeviceInitialize(const char* title, bool, bool)` | nothing, but its return is `WXL_STAGE_GRAPHICS`: the device is made after the first database is read and before the script context exists, which is the order the stages promise |
| `Scrn.OnPaint` | `OnPaint(const void* data, void* param)`, the paint handler | `FrameBegin` before, only when the device has a context and the window can be seen -- the test the paint makes itself |
| `Gx.ScenePresent` | `GxScenePresent()` | `FrameEnd` before, for the frame `Scrn.OnPaint` opened and only for it: the loading screen presents frames of its own, which raise nothing. The last draw list is flushed after it |
| `SimpleTop.OnLayerRender` | `CSimpleTop::OnLayerRender()` | `UiRenderEnd` after: every stratum of the interface is drawn, the world inside it |
| `WorldFrame.RenderWorld` | `CGWorldFrame::RenderWorld(void* world_frame)` | `WorldRenderBegin` before, `WorldRenderEnd` after. Run from inside the interface's batches, before anything of the interface over it; not reached where there is no world |
| `Gx.ReleaseD3dResources` | `CGxDeviceD3d::IReleaseD3dResources(int32_t)` | `SwapchainLost` before, with the size about to go: at both of the D3D9 device's resets, and when it is destroyed. A client on another device does not reach it |
| `Gx.NotifyOnDeviceRestored` | `CGxDevice::NotifyOnDeviceRestored()` | `SwapchainRestored` before, with the size the device came back at |
| `M2.DrawBatch` | `CM2SceneRender::DrawBatch()`, one plain batch of a model | nothing: after, the batch the device just drew is handed to the watchers of `wxl.gx`, with everything it was drawn with still bound |
| `M2.LoadSucceededCallback` | `CM2Shared::LoadSucceededCallback(void* param)`, a model file read whole | nothing: before, the unparsed bytes are handed to the watchers of `wxl.m2file`, which may rewrite and shorten them or refuse the file |
| `M2.Initialize` | `CM2Shared::Initialize()`, a parsed model made ready | nothing: before, the parsed model is handed to the watchers of `wxl.m2file`, ahead of its skin profile's choice and its textures |
| `M2.InitializeSkinProfile` | `CM2Shared::InitializeSkinProfile()`, a parsed skin profile sized for drawing | nothing: before, the model and its skin profile are handed to the watchers of `wxl.m2file` |
| `M2.FinishLoadingLowPrioritySequence` | `CM2Shared::FinishLoadingLowPrioritySequence(uint32_t sequence_index, uint8_t* buffer, uint32_t size)`, an external sequence file parsed against its model | nothing: before, the sequence's unparsed bytes are handed to the watchers of `wxl.m2file` |
| `Spell.Packet_Group_30` | `Packet_Group_30(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)`, the handler of `SMSG_SPELL_START` and `SMSG_SPELL_GO` | `SpellCastStart` or `SpellCastGo` before, by the opcode, read off the message before the client parses it: the caster, the object target, the cast time or the hit and miss counts |
| `Spell.Packet_SMSG_SPELL_FAILURE` | `Packet_SMSG_SPELL_FAILURE(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)` | `SpellCastInterrupted` before, for any caster in view, with the reason the server sent |
| `Spell.SpellFailed` | `Spell_C_SpellFailed(PendingSpellCast* cast, const SpellRec* spell, int32_t reason, int32_t misc1, int32_t misc2, bool reset_cooldown)` | `SpellCastFailed` before: the local player's cast refused by the server or by the client's own checks |
| `Spell.CastSpell` (gated) | `Spell_C_CastSpell(CGUnit_C* caster, int32_t spell_id, const CGItem_C* item, uint64_t target, PendingSpellCast* cast, bool confirmed)` | `SpellCastRequest` at the gate; refusing it answers false and nothing is sent |
| `Item.UseItem` (gated) | `CGItem_C::UseItem(const uint64_t* target, int32_t confirmed)` | `ItemUseRequest` at the gate; refusing it answers 0 and the item's spell is never asked for |
| `FrameScript.vSignalEvent` | `vFrameScript_SignalEvent(uint32_t index, const char* format, va_list args)` | `UiEvent` before, when a mod listens: the event's name, its format, and its arguments rendered as text, one entry per `%` |
| `FrameScript.HandleError` | `FrameScript_HandleError(lua_State* state)`, the error handler of every protected call | `ScriptError` before, with the error value as the VM left it |
| `Spell.Packet_MSG_CHANNEL_START` | `Packet_MSG_CHANNEL_START(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)` | `SpellChannelStart` before, for a channel of some length |
| `Spell.Packet_MSG_CHANNEL_UPDATE` | `Packet_MSG_CHANNEL_UPDATE(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)` | `SpellChannelUpdate` before, or `SpellChannelStop` when no time is left, with the channel's spell as the client holds it on the unit |
| `Spell.Packet_SMSG_UNIT_SPELLCAST_START` | `Packet_SMSG_UNIT_SPELLCAST_START(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)`, the handler of opcode 0x14D | `SpellCastResume` before: a cast already running when its caster came into view |
| `Spell.Packet_SMSG_SPELL_DELAYED` | `Packet_SMSG_SPELL_DELAYED(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)` | `SpellCastDelayed` before, with the cast the client holds on the unit |
| `Spell.Packet_SMSG_SPELL_COOLDOWN`, `Spell.Packet_SMSG_ITEM_COOLDOWN` | `Packet_SMSG_SPELL_COOLDOWN(...)`, `Packet_SMSG_ITEM_COOLDOWN(...)`, the four handler arguments | `SpellCooldown` before, once per spell of the message, or once for the item |
| `Spell.Packet_Group_31` | `Packet_Group_31(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)`, the handler of `SMSG_COOLDOWN_EVENT`, `SMSG_CLEAR_COOLDOWN` and `SMSG_MODIFY_COOLDOWN` | `SpellCooldownChanged` before, by the opcode |
| `Spell.CancelSpell` | `Spell_C_CancelSpell(PendingSpellCast* cast, bool stop_recast, bool notify_server, int32_t reason)`, where every cancel of the player's own cast ends | `SpellCastCancel` before, with the cast's caster, spell and count, the reason, and whether the server may be told |
| `Combat.Packet_Group_29` | `Packet_Group_29(void* param, uint32_t msg_id, uint32_t time, CDataStore* msg)`, the handler of `SMSG_ATTACKSTART`, `SMSG_ATTACKSTOP`, `SMSG_ATTACKERSTATEUPDATE` and `SMSG_ENVIRONMENTALDAMAGELOG` | `CombatAttackStart`, `CombatAttackStop`, `CombatDamage` or `CombatMiss` before, by the opcode and whether the round landed |
| `Combat.HandleSpellLogMiss` | `CombatLog::HandleSpellLogMiss(CDataStore* msg, int32_t time_offset)`, where `SMSG_SPELLLOGMISS` ends on its own or inside `SMSG_COMBAT_LOG_MULTIPLE` | `CombatMiss` before, once per target |
| `Combat.HandleSpellNonMeleeDamageLog` | `CombatLog::HandleSpellNonMeleeDamageLog(CDataStore* msg, int32_t time_offset)` | `CombatDamage` before |
| `Combat.HandleSpellHealLog` | `CombatLog::HandleSpellHealLog(CDataStore* msg, int32_t time_offset)` | `CombatHeal` before |
| `Combat.HandlePeriodicAuraLog` | `CombatLog::HandlePeriodicAuraLog(CDataStore* msg, int32_t time_offset)` | `CombatDamage` or `CombatHeal` before, per damage or heal entry, up to where the client stops |

The frame's slots -- `WorldRenderBegin`, `WorldRenderEnd`, `UiRenderEnd` -- carry the index of the
frame they are drawn in, and are not raised outside one. `FrameBegin` and `FrameEnd` come in pairs:
the client presents more often than it paints a frame, so the runtime keeps the pairing rather
than trusting the client to.

A seam is named after the function it attaches to, never after what it means; the name is part of
the ABI and is appended, never renamed or dropped.

## 🧩 The mods it ships

Two mods that do what a patcher used to do, so the patcher only has to bootstrap. Both are
32-bit only and both declare `wxl.patch` as required, so any other runtime refuses them cleanly.

| Mod | What it does |
|---|---|
| `wxl.glue-unlock` | bypasses the Lua/XML signature check, so custom interface code loads |
| `wxl.named-archives` | widens the client's `patch-?.MPQ` scan to `patch-*.MPQ`, so archives and folders with real names are mounted |

Both read memory before writing: if the bytes are already in the patched state they log
`already present` and do nothing.

## 📂 Layout

```
include/wxl/core/init.h          the 64-bit entry point, for the client to include
include/wxl/core/binding/        the client binding: the facts read out of the client, one .def per domain
include/wxl/seams/               the seam table (one .def per domain) and seams.h, their entry points
include/wxl/offsets/             site.hpp, layout.hpp and the generated tables
src/runtime/                     host bring-up, stages, frames, the per-architecture attachment
src/binding/                     the binding realised: memory reads (x86), the client's accessors (x64)
src/seams/                       the seam bodies, one file per domain (both); the x86 detours and thunks
src/objects/                     wxl.object over the binding
src/services/                    wxl.script, wxl.storage, wxl.data (both); wxl.hook, wxl.hook.raw, wxl.patch (x86)
src/hook/                        the detour engine and the named points        (x86)
src/patcher/                     PE import and entry trampoline                (x86)
src/common/                      the log
tests/seam_harness.cpp           every seam body against a fake client; --bench times the hot paths
mods/                            the mods above                                (x86)
deps/minhook/                    MinHook, BSD-2-Clause, unmodified             (x86)
tools/seams/                     check the seam table against the fork and against Wow.exe
tools/offsets/, tools/layouts/   regenerate the tables from the decompiled client
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
| `the client handed no <row> accessor; what reads it answers absent` (x64) | the client was built against an older `init.h` than this runtime; the row of `binding.def` it names answers its absent value (map `-1`, no position, ...) until the client passes one |

## 📄 License

GPLv3. See [LICENSE](LICENSE).

MinHook, under `deps/minhook/`, is BSD-2-Clause and keeps its own license file.

<div align="center">

[⬆ Back to top](#warcraftxl-core)

</div>
