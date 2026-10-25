// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The x86 realisation of the client binding: memory reads at measured offsets, calls at resolved addresses.

#include "binding/Binding.hpp"
#include "binding/BindingX86.hpp"
#include "binding/CallX86.hpp"
#include "binding/ModelDrawX86.hpp"

#include "common/Log.hpp"
#include "runtime/Core.hpp"
#include "runtime/Image.hpp"

#include "wxl/events.h"
#include "wxl/core/gpu.hpp"
#include "wxl/core/storm.hpp"
#include "wxl/offsets/layout.hpp"
#include "wxl/offsets/site.hpp"
#include "wxl/services/script.h"

#include <windows.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    DWORD g_mainThread = 0;

    bool IsClient() { return wxl::core::image::IsClient(); }

    bool OnMainThread()
    {
        if (GetCurrentThreadId() == g_mainThread) return true;
        WLOG_WARN(wxl::core::kTag, "a binding row that reads the object manager was called off "
                                   "the main thread; it answers absent");
        return false;
    }

    /// A value of T at a byte offset from a base, read without alignment assumptions.
    template <class T>
    T Read(const void* base, std::size_t offset)
    {
        T value;
        std::memcpy(&value, static_cast<const char*>(base) + offset, sizeof value);
        return value;
    }

    /// The block a pointer member points at, or null when the member is null.
    const void* Via(const void* object, std::size_t offset)
    {
        return object ? Read<const void*>(object, offset) : nullptr;
    }

    // The client's own getters, called as the __thiscall members they are: `this` in ECX, a dummy
    // EDX, the stack arguments after. A C3Vector return goes through the hidden pointer the
    // caller passes as the first stack argument, which the shim passes explicitly.
    using PositionFn = wxl_vec3*(__fastcall*)(const void* self, void* edx, wxl_vec3* out);
    using FacingFn   = float(__fastcall*)(const void* self, void* edx);
    using NameFn     = const char*(__fastcall*)(const void* self, void* edx);
    using PlayerFn   = void*(__cdecl*)();

    wxl_bool CallPosition(std::uintptr_t site, const void* object, wxl_vec3* out)
    {
        reinterpret_cast<PositionFn>(site)(object, nullptr, out);
        return WXL_TRUE;
    }

    wxl_bool CallFacing(std::uintptr_t site, const void* object, float* out)
    {
        *out = reinterpret_cast<FacingFn>(site)(object, nullptr);
        return WXL_TRUE;
    }

    const char* CallName(std::uintptr_t site, const void* object)
    {
        return reinterpret_cast<NameFn>(site)(object, nullptr);
    }

    constexpr std::size_t kCorpsePosition = WXL_FIELD("CGCorpse_C", "m_passenger") + WXL_FIELD("CPassenger", "m_position");

    // The client's Lua 5.1 and the FrameScript layer, called as the C functions they are. The
    // pseudo-indices and type codes are Lua 5.1's public API, the same in every build of it.
    constexpr int32_t kLuaGlobalsIndex = -10002;              // LUA_GLOBALSINDEX
    constexpr int32_t kLuaUpvalue1     = kLuaGlobalsIndex - 1; // lua_upvalueindex(1)
    constexpr int32_t kLuaTBoolean     = 1;
    constexpr int32_t kLuaTNumber      = 3;
    constexpr int32_t kLuaTString      = 4;

    using LuaGetTop        = int32_t(__cdecl*)(void* L);
    using LuaSetTop        = void(__cdecl*)(void* L, int32_t index);
    using LuaType          = int32_t(__cdecl*)(void* L, int32_t index);
    using LuaToNumber      = double(__cdecl*)(void* L, int32_t index);
    using LuaToLString     = const char*(__cdecl*)(void* L, int32_t index, std::size_t* length);
    using LuaToBoolean     = int32_t(__cdecl*)(void* L, int32_t index);
    using LuaPushNumber    = void(__cdecl*)(void* L, double value);
    using LuaPushLString   = void(__cdecl*)(void* L, const char* text, std::size_t length);
    using LuaPushBoolean   = void(__cdecl*)(void* L, int32_t value);
    using LuaPushNil       = void(__cdecl*)(void* L);
    using LuaPushCClosure  = void(__cdecl*)(void* L, wxl_script_native_fn fn, int32_t upvalues);
    using LuaGetField      = void(__cdecl*)(void* L, int32_t index, const char* key);
    using LuaSetField      = void(__cdecl*)(void* L, int32_t index, const char* key);
    using LuaError         = int32_t(__cdecl*)(void* L);
    using ScriptExecute    = void(__cdecl*)(const char* source, const char* name, const char* taint);
    using ScriptCreateEvts = void(__cdecl*)(const char** names, uint32_t count);
    using ScriptSignal     = void(__cdecl*)(uint32_t index, void* L, int32_t arg_count);

    // The client's file layer. STORMAPI is __stdcall, and every one of these has external
    // linkage, so the convention they are declared with is the convention they carry.
    using FileExists   = int32_t(__stdcall*)(const char* name);
    using FileOpen     = int32_t(__stdcall*)(const char* name, void** file);
    using FileSize     = uint32_t(__stdcall*)(void* file, uint32_t* high);
    using FileRead     = int32_t(__stdcall*)(void* file, void* buffer, uint32_t count,
                                             uint32_t* read, void* overlapped, uint32_t unused);
    using FileSeek     = int32_t(__stdcall*)(void* file, int32_t distance, int32_t* high,
                                             uint32_t method);
    using FileClose    = int32_t(__stdcall*)(void* file);
    using ArchiveOpen  = int32_t(__stdcall*)(const char* path, int32_t priority, uint32_t flags,
                                             void** archive);

    // The graphics device's queries. Each is a free function with external linkage and no
    // argument -- or one, for the size -- that reads g_theGxDevicePtr and asks the device, so
    // the convention they are declared with, cdecl, is the one they carry.
    using GxFlag       = int32_t(__cdecl*)();
    using GxWindowSize = void(__cdecl*)(float* rect); ///< a CRect: minY, minX, maxY, maxX

    /// The ARCHIVEREC* of every mounted archive, in the client's order.
    std::vector<void*> ArchiveRecords()
    {
        std::vector<void*> records;
        // System_MPQGlobals::GetArchiveList answers the std::set of mounted archives. Its layout
        // is MSVC 9's, as FindArchiveByName (0x00453FC0) and the set iterator's ++ (0x00429F90)
        // read it: the head node at +4; a node's left, parent and right at 0, 4 and 8, its value
        // (the ARCHIVEREC*) at 0xC, its is-nil flag at 0x11. The head is the nil node.
        using GetList = const uint8_t*(__cdecl*)();
        const uint8_t* set =
            reinterpret_cast<GetList>(WXL_SITE("System_MPQGlobals::GetArchiveList", 0))();
        if (!set) return records;
        const auto link = [](const uint8_t* node, size_t at) {
            return *reinterpret_cast<const uint8_t* const*>(node + at);
        };
        const auto nil = [](const uint8_t* node) { return node[0x11] != 0; };

        const uint8_t* head = link(set, 4);
        const uint8_t* node = link(head, 0);
        while (node && !nil(node) && records.size() < 4096)
        {
            records.push_back(*reinterpret_cast<void* const*>(node + 0xC));
            if (!nil(link(node, 8)))
            {
                node = link(node, 8);
                while (!nil(link(node, 0))) node = link(node, 0);
            }
            else
            {
                const uint8_t* parent = link(node, 4);
                while (!nil(parent) && node == link(parent, 8))
                {
                    node   = parent;
                    parent = link(parent, 4);
                }
                node = parent;
            }
        }
        return records;
    }

    /// The device the client draws with, or null before it is made and after it is torn down.
    const void* GxDevice() { return *reinterpret_cast<void* const*>(WXL_STATIC("g_theGxDevicePtr")); }

    LuaGetTop       LuaGetTopAt() { return reinterpret_cast<LuaGetTop>(WXL_SITE("lua_gettop", 0)); }
    LuaSetTop       LuaSetTopAt() { return reinterpret_cast<LuaSetTop>(WXL_SITE("lua_settop", 0)); }
    LuaType         LuaTypeAt() { return reinterpret_cast<LuaType>(WXL_SITE("lua_type", 0)); }
    LuaToNumber     LuaToNumberAt() { return reinterpret_cast<LuaToNumber>(WXL_SITE("lua_tonumber", 0)); }
    LuaToLString    LuaToLStringAt() { return reinterpret_cast<LuaToLString>(WXL_SITE("lua_tolstring", 0)); }
    LuaToBoolean    LuaToBooleanAt() { return reinterpret_cast<LuaToBoolean>(WXL_SITE("lua_toboolean", 0)); }
    LuaPushNumber   LuaPushNumberAt() { return reinterpret_cast<LuaPushNumber>(WXL_SITE("lua_pushnumber", 0)); }
    LuaPushLString  LuaPushLStringAt() { return reinterpret_cast<LuaPushLString>(WXL_SITE("lua_pushlstring", 0)); }
    LuaPushBoolean  LuaPushBooleanAt() { return reinterpret_cast<LuaPushBoolean>(WXL_SITE("lua_pushboolean", 0)); }
    LuaPushNil      LuaPushNilAt() { return reinterpret_cast<LuaPushNil>(WXL_SITE("lua_pushnil", 0)); }
    LuaPushCClosure LuaPushCClosureAt() { return reinterpret_cast<LuaPushCClosure>(WXL_SITE("lua_pushcclosure", 0)); }
    LuaGetField     LuaGetFieldAt() { return reinterpret_cast<LuaGetField>(WXL_SITE("lua_getfield", 0)); }
    LuaSetField     LuaSetFieldAt() { return reinterpret_cast<LuaSetField>(WXL_SITE("lua_setfield", 0)); }
    LuaError        LuaErrorAt() { return reinterpret_cast<LuaError>(WXL_SITE("lua_error", 0)); }

    // Two overloads carry each FrameScript name: the (source, name, taint) Execute and the
    // (index, L, argCount) SignalEvent are the ones wxl.script drives.
    ScriptExecute    ScriptExecuteAt() { return reinterpret_cast<ScriptExecute>(WXL_SITE_AT("FrameScript_Execute", 0x00819210, 0)); }
    ScriptCreateEvts ScriptCreateEventsAt() { return reinterpret_cast<ScriptCreateEvts>(WXL_SITE("FrameScript_CreateEvents", 0)); }
    ScriptSignal     ScriptSignalEventAt() { return reinterpret_cast<ScriptSignal>(WXL_SITE_AT("FrameScript_SignalEvent", 0x0081AA00, 0)); }

    uint32_t ValueType(int32_t luaType)
    {
        switch (luaType)
        {
        case kLuaTNumber: return WXL_VALUE_NUMBER;
        case kLuaTString: return WXL_VALUE_STRING;
        case kLuaTBoolean: return WXL_VALUE_BOOL;
        default: return WXL_VALUE_NIL;
        }
    }
}

namespace wxl::core::binding::x86
{
    void Prepare() { g_mainThread = GetCurrentThreadId(); }
}

namespace wxl::core::binding
{
    // --- the field rows, generated from the table --------------------------------------------

#define WXL_BINDING(name, ret, params, args, absent)

#define WXL_BINDING_FIELD(name, ret, Type, member, absent)                                       \
    ret name(const void* object)                                                                 \
    {                                                                                            \
        return object && IsClient() ? Read<ret>(object, WXL_FIELD(#Type, #member)) : (absent);   \
    }

#define WXL_BINDING_FIELD_VIA(name, ret, Type, via, ViaType, member, absent)                     \
    ret name(const void* object)                                                                 \
    {                                                                                            \
        const void* block = IsClient() ? Via(object, WXL_FIELD(#Type, #via)) : nullptr;          \
        return block ? Read<ret>(block, WXL_FIELD(#ViaType, #member)) : (absent);                \
    }

#define WXL_BINDING_FIELD_VIA_INDEX(name, ret, Type, via, ViaType, member)                       \
    wxl_bool name(const void* object, uint32_t index, ret* out)                                  \
    {                                                                                            \
        constexpr std::size_t count = WXL_MEMBER_SIZE(#ViaType, #member) / sizeof(ret);          \
        const void* block = IsClient() ? Via(object, WXL_FIELD(#Type, #via)) : nullptr;          \
        if (!block || !out || index >= count) return WXL_FALSE;                                  \
        *out = Read<ret>(block, WXL_FIELD(#ViaType, #member) + index * sizeof(ret));             \
        return WXL_TRUE;                                                                         \
    }

#define WXL_BINDING_FIELD_VIA_AT(name, ret, Type, via, ViaType, member, Elem, elem_member)       \
    wxl_bool name(const void* object, uint32_t index, ret* out)                                  \
    {                                                                                            \
        constexpr std::size_t stride = WXL_TYPE_SIZE(#Elem);                                     \
        constexpr std::size_t count  = WXL_MEMBER_SIZE(#ViaType, #member) / stride;              \
        const void* block = IsClient() ? Via(object, WXL_FIELD(#Type, #via)) : nullptr;          \
        if (!block || !out || index >= count) return WXL_FALSE;                                  \
        *out = Read<ret>(block, WXL_FIELD(#ViaType, #member) + index * stride +                  \
                                     WXL_FIELD(#Elem, #elem_member));                            \
        return WXL_TRUE;                                                                         \
    }

#include "wxl/core/binding.def"

#undef WXL_BINDING
#undef WXL_BINDING_FIELD
#undef WXL_BINDING_FIELD_VIA
#undef WXL_BINDING_FIELD_VIA_INDEX
#undef WXL_BINDING_FIELD_VIA_AT

    // --- the call rows, one body per WXL_BINDING row of the table ----------------------------

    int32_t map_id(void)
    {
        // The Map.cpp s_mapId: MinimapFrame.cpp and DayNight.cpp each have one of the same name.
        constexpr std::uintptr_t kMapId = WXL_STATIC_AT("s_mapId", 0x00ADFBC4);
        return IsClient() ? *reinterpret_cast<const int32_t*>(kMapId) : -1;
    }

    wxl_bool world_position(wxl_vec3* out)
    {
        if (!IsClient() || !out || map_id() < 0) return WXL_FALSE;
        *out = *reinterpret_cast<const wxl_vec3*>(WXL_STATIC("World::s_currentWorldPos"));
        return WXL_TRUE;
    }

    const void* local_player_object(void)
    {
        if (!IsClient() || !OnMainThread()) return nullptr;
        return reinterpret_cast<PlayerFn>(WXL_SITE("ClntObjMgrGetActivePlayerObj", 0))();
    }

    const void* object_by_guid(uint64_t guid)
    {
        // ClntObjMgrObjectPtr(WOWGUID, TypeMask, const char* file, int line), cdecl: the guid as
        // two dwords, the mask, and a source position it reads only to report a wrong kind.
        // TYPE_OBJECT (1) is every kind.
        using ObjectPtrFn = void*(__cdecl*)(uint64_t guid, uint32_t mask, const char* file, int32_t line);
        if (!IsClient() || !OnMainThread() || !guid) return nullptr;
        return reinterpret_cast<ObjectPtrFn>(WXL_SITE("ClntObjMgrObjectPtr", 0))(guid, 1u, "WarcraftXL", 0);
    }

    wxl_bool object_watch_type(uint32_t type, uint32_t offset, uint32_t size, wxl_core_mirror_fn handler,
                               void* param)
    {
        // ClntObjMgrSetTypeMirrorHandler(OBJECT_TYPE_ID, fieldOffset, fieldSize, MIRRORHANDLERFUNC,
        // param, HANDLER_PRIORITY, alwaysFire), cdecl: it reads [ebp+8]..[ebp+0x20] and returns
        // with a bare `ret` (0x004D5C13); CGUnit_C::ClientInitialize pushes seven dwords, calls,
        // and `add esp, 0x1c` (0x006E4648-0x006E465E). Priority 0 links the handler after those
        // already on the field; alwaysFire 0 calls it only when the bytes changed.
        using SetTypeFn = void(__cdecl*)(uint32_t type, uint32_t offset, uint32_t size, wxl_core_mirror_fn handler,
                                         void* param, int32_t priority, int32_t always);
        if (!IsClient() || !OnMainThread() || !handler) return WXL_FALSE;
        reinterpret_cast<SetTypeFn>(WXL_SITE("ClntObjMgrSetTypeMirrorHandler", 0))(type, offset, size, handler,
                                                                                    param, 0, 0);
        return WXL_TRUE;
    }

    wxl_bool object_watch_one(uint64_t guid, uint32_t type, uint32_t offset, uint32_t size,
                              wxl_core_mirror_fn handler, void* param)
    {
        // ClntObjMgrSetObjMirrorHandler(WOWGUID, OBJECT_TYPE_ID, fieldOffset, fieldSize,
        // MIRRORHANDLERFUNC, param, HANDLER_PRIORITY, alwaysFire), cdecl: the guid as two dwords
        // at [ebp+8] and [ebp+0xC], the rest to [ebp+0x28], a bare `ret` (0x004D5B33); the
        // explored-zones handler's setter at 0x00547120-0x00547143 pushes seven dwords and the
        // active player's guid (edx:eax), calls, and `add esp, 0x24`. It does nothing for a guid
        // the manager does not hold, which is asked first.
        using SetObjFn = void(__cdecl*)(uint64_t guid, uint32_t type, uint32_t offset, uint32_t size,
                                        wxl_core_mirror_fn handler, void* param, int32_t priority, int32_t always);
        if (!handler || !object_by_guid(guid)) return WXL_FALSE;
        reinterpret_cast<SetObjFn>(WXL_SITE("ClntObjMgrSetObjMirrorHandler", 0))(guid, type, offset, size, handler,
                                                                                  param, 0, 0);
        return WXL_TRUE;
    }

    wxl_bool object_unwatch_type(uint32_t type, uint32_t offset, wxl_core_mirror_fn handler, void* param)
    {
        // ClntObjMgrUnsetTypeMirrorHandler(OBJECT_TYPE_ID, fieldOffset, MIRRORHANDLERFUNC, param),
        // cdecl: it reads [ebp+8]..[ebp+0x14] and returns with a bare `ret` (0x004D5C70);
        // CGUnit_C::UnregisterMirrorHandlers pushes four dwords, calls, and `add esp, 0x10`
        // (0x00741FF6-0x00742008).
        using UnsetTypeFn = void(__cdecl*)(uint32_t type, uint32_t offset, wxl_core_mirror_fn handler, void* param);
        if (!IsClient() || !OnMainThread() || !handler) return WXL_FALSE;
        reinterpret_cast<UnsetTypeFn>(WXL_SITE("ClntObjMgrUnsetTypeMirrorHandler", 0))(type, offset, handler, param);
        return WXL_TRUE;
    }

    wxl_bool object_unwatch_one(uint64_t guid, uint32_t type, uint32_t offset, wxl_core_mirror_fn handler,
                                void* param)
    {
        // ClntObjMgrUnsetObjMirrorHandler(WOWGUID, OBJECT_TYPE_ID, fieldOffset, MIRRORHANDLERFUNC,
        // param), cdecl: the guid as two dwords at [ebp+8] and [ebp+0xC], the rest to [ebp+0x1C],
        // a bare `ret` (0x004D5B9B). It does nothing for a guid the manager does not hold, which is
        // asked first.
        using UnsetObjFn = void(__cdecl*)(uint64_t guid, uint32_t type, uint32_t offset, wxl_core_mirror_fn handler,
                                          void* param);
        if (!handler || !object_by_guid(guid)) return WXL_FALSE;
        reinterpret_cast<UnsetObjFn>(WXL_SITE("ClntObjMgrUnsetObjMirrorHandler", 0))(guid, type, offset, handler,
                                                                                      param);
        return WXL_TRUE;
    }

    uint64_t ui_target_guid(void)
    {
        return IsClient() ? *reinterpret_cast<const uint64_t*>(WXL_STATIC("CGGameUI::m_lockedTarget")) : 0;
    }

    int32_t ui_initialized(void)
    {
        return IsClient() ? *reinterpret_cast<const int32_t*>(WXL_STATIC("CGGameUI::m_initialized")) : 0;
    }

    uint8_t ui_logging_in(void)
    {
        return IsClient() ? *reinterpret_cast<const uint8_t*>(WXL_STATIC("CGGameUI::m_loggingIn")) : 0;
    }

    uint8_t ui_reloading(void)
    {
        return IsClient() ? *reinterpret_cast<const uint8_t*>(WXL_STATIC("CGGameUI::m_currentlyReloadingUI")) : 0;
    }

    // --- glue.def: the login screens' state -------------------------------------------------

    const char* glue_current_screen(void)
    {
        // A char[64] static: its address is the string.
        return IsClient() ? reinterpret_cast<const char*>(WXL_STATIC("CGlueMgr::m_currentScreen")) : nullptr;
    }

    int32_t login_server_type(const void* login)
    {
        // Login::GetLoginServerType is the vtable's slot 50 (+0xC8): ClientServices::
        // GetLoginServerType calls it there (0x004D9FF2 `mov eax,[edx+0xc8]`), and the slot holds
        // 0x008E5250 (`xor eax,eax; ret`) in GruntLogin's vtable at 0x00A94928 and 0x008C8DE0
        // (`mov eax,1; ret`) in BattlenetLogin's at 0x00A94768.
        if (!IsClient() || !login) return -1;
        using ServerTypeFn = int32_t(__fastcall*)(const void* self, void* edx);
        const void* const* vtable = Read<const void* const*>(login, 0);
        return reinterpret_cast<ServerTypeFn>(const_cast<void*>(vtable[50]))(login, nullptr);
    }

    // --- chat.def: the lines the chat holds -------------------------------------------------

    const void* chat_pending_last(void)
    {
        // TSList::Tail as stpl.h writes it: the terminator's m_prevLink, that link's m_prevLink,
        // and its m_next unless the low bit marks the terminator. The list is the link offset
        // (+0), then the terminator: m_prevLink (+4), m_next (+8). QueueChatText reads the same
        // m_prevLink to append (0x00502D68 `mov ecx,[0x00AC7D40]`).
        if (!IsClient()) return nullptr;
        const void* list       = reinterpret_cast<const void*>(WXL_STATIC("CGChat::s_pendingChat"));
        const void* last       = Read<const void*>(list, 4);
        const void* beforeLast = last ? Read<const void*>(last, 0) : nullptr;
        if (!beforeLast) return nullptr;
        const std::uintptr_t next = Read<std::uintptr_t>(beforeLast, 4);
        return (next & 1u) ? nullptr : reinterpret_cast<const void*>(next);
    }

    // --- script.def: the client's Lua, called as the C functions they are ---------------------

    void* script_state(void)
    {
        // The FrameScript.cpp s_context: OsIME.cpp has one of the same name.
        constexpr std::uintptr_t kContext = WXL_STATIC_AT("s_context", 0x00D3F78C);
        return IsClient() ? *reinterpret_cast<void* const*>(kContext) : nullptr;
    }

    wxl_bool script_global_is_set(void* state, const char* name)
    {
        if (!IsClient() || !state || !name) return WXL_FALSE;
        LuaGetFieldAt()(state, kLuaGlobalsIndex, name);
        const int32_t type = LuaTypeAt()(state, -1);
        LuaSetTopAt()(state, -2);
        return type > 0 ? WXL_TRUE : WXL_FALSE;
    }

    void script_set_global_closure(void* state, const char* name, wxl_script_native_fn fn, uint32_t slot)
    {
        if (!IsClient() || !state || !name || !fn) return;
        LuaPushNumberAt()(state, static_cast<double>(slot));
        LuaPushCClosureAt()(state, fn, 1);
        LuaSetFieldAt()(state, kLuaGlobalsIndex, name);
    }

    void script_clear_global(void* state, const char* name)
    {
        if (!IsClient() || !state || !name) return;
        LuaPushNilAt()(state);
        LuaSetFieldAt()(state, kLuaGlobalsIndex, name);
    }

    uint32_t script_closure_slot(void* state)
    {
        if (!IsClient() || !state) return 0;
        return static_cast<uint32_t>(LuaToNumberAt()(state, kLuaUpvalue1));
    }

    int32_t script_top(void* state)
    {
        return IsClient() && state ? LuaGetTopAt()(state) : 0;
    }

    uint32_t script_value_type(void* state, int32_t index)
    {
        return IsClient() && state ? ValueType(LuaTypeAt()(state, index)) : WXL_VALUE_NIL;
    }

    double script_to_number(void* state, int32_t index)
    {
        return IsClient() && state ? LuaToNumberAt()(state, index) : 0.0;
    }

    const char* script_to_string(void* state, int32_t index, uint32_t* length)
    {
        if (!IsClient() || !state) return nullptr;
        std::size_t size = 0;
        const char* text = LuaToLStringAt()(state, index, &size);
        if (length) *length = static_cast<uint32_t>(size);
        return text;
    }

    wxl_bool script_to_bool(void* state, int32_t index)
    {
        return IsClient() && state && LuaToBooleanAt()(state, index) ? WXL_TRUE : WXL_FALSE;
    }

    void script_push_number(void* state, double value)
    {
        if (IsClient() && state) LuaPushNumberAt()(state, value);
    }

    void script_push_string(void* state, const char* text, uint32_t length)
    {
        if (IsClient() && state) LuaPushLStringAt()(state, text ? text : "", text ? length : 0u);
    }

    void script_push_bool(void* state, wxl_bool value)
    {
        if (IsClient() && state) LuaPushBooleanAt()(state, value ? 1 : 0);
    }

    void script_push_nil(void* state)
    {
        if (IsClient() && state) LuaPushNilAt()(state);
    }

    void script_pop(void* state, int32_t count)
    {
        if (IsClient() && state && count > 0) LuaSetTopAt()(state, -count - 1);
    }

    void script_error(void* state, const char* message)
    {
        if (!IsClient() || !state) return;
        const char* text = message ? message : "WarcraftXL: error";
        LuaPushLStringAt()(state, text, std::strlen(text));
        LuaErrorAt()(state);
    }

    void script_run(const char* source, const char* name)
    {
        if (IsClient() && source) ScriptExecuteAt()(source, name ? name : "WarcraftXL", nullptr);
    }

    void script_create_events(const char** names, uint32_t count)
    {
        if (IsClient() && (names || count == 0)) ScriptCreateEventsAt()(names, count);
    }

    void script_signal_event(uint32_t index, void* state, int32_t arg_count)
    {
        if (IsClient() && state) ScriptSignalEventAt()(index, state, arg_count);
    }

    wxl_bool file_exists(const char* path)
    {
        if (!IsClient() || !path) return WXL_FALSE;
        const FileExists call = reinterpret_cast<FileExists>(WXL_SITE("SFile::FileExists", 0));
        return call(path) ? WXL_TRUE : WXL_FALSE;
    }

    void* file_open(const char* path)
    {
        if (!IsClient() || !path) return nullptr;
        const FileOpen call = reinterpret_cast<FileOpen>(WXL_SITE("SFileOpenFile", 0));
        void*          file = nullptr;
        return call(path, &file) ? file : nullptr;
    }

    uint64_t file_size(void* file)
    {
        if (!IsClient() || !file) return 0;
        const FileSize call = reinterpret_cast<FileSize>(WXL_SITE("SFileGetFileSize", 0));
        uint32_t       high = 0;
        const uint32_t low  = call(file, &high);
        return (static_cast<uint64_t>(high) << 32) | low;
    }

    wxl_bool file_seek(void* file, uint64_t offset)
    {
        if (!IsClient() || !file) return WXL_FALSE;
        const FileSeek call = reinterpret_cast<FileSeek>(WXL_SITE("SFile::SetFilePointer", 0));
        int32_t        high = static_cast<int32_t>(offset >> 32);
        // Method 0 is FILE_BEGIN: the offset is absolute, which is what read_at means. The answer
        // is the cursor afterwards, 0 for a seek to the start (0x00421C75: `mov eax, edi`).
        const int32_t at = call(file, static_cast<int32_t>(offset & 0xFFFFFFFFu), &high, 0);
        return wxl::core::storm::SeekLanded(static_cast<uint32_t>(at), offset) ? WXL_TRUE : WXL_FALSE;
    }

    uint32_t file_read(void* file, void* buffer, uint32_t count)
    {
        if (!IsClient() || !file || !buffer || !count) return 0;
        const FileRead call = reinterpret_cast<FileRead>(WXL_SITE("SFileReadFile", 0));
        uint32_t       read = 0;
        call(file, buffer, count, &read, nullptr, 0);
        return read;
    }

    void file_close(void* file)
    {
        if (!IsClient() || !file) return;
        reinterpret_cast<FileClose>(WXL_SITE("SFileCloseFile", 0))(file);
    }

    wxl_bool archive_mount(const char* path, int32_t priority)
    {
        if (!IsClient() || !path) return WXL_FALSE;
        const ArchiveOpen call =
            reinterpret_cast<ArchiveOpen>(WXL_SITE("SFile::OpenArchive", 0));
        void* archive = nullptr;
        return call(path, priority, 0, &archive) ? WXL_TRUE : WXL_FALSE;
    }

    const char* storage_base_path()
    {
        if (!IsClient()) return nullptr;
        return reinterpret_cast<const char*>(WXL_STATIC("SFile::m_basePath"));
    }

    void** filestack_top()
    {
        if (!IsClient()) return nullptr;
        return reinterpret_cast<void**>(WXL_STATIC("System_File::Stacked::s_manager"));
    }

    const void* filestack_initial()
    {
        if (!IsClient()) return nullptr;
        return reinterpret_cast<const void*>(WXL_STATIC("System_File::Stacked::s_initstack"));
    }

    uint32_t archive_list(wxl_core_name_fn fn, void* user)
    {
        if (!IsClient() || !fn) return 0;

        // The records first, walked in order; the listing opens files, which may reach the set.
        const std::vector<void*> records = ArchiveRecords();
        using List = int32_t(__stdcall*)(void* archive, wxl_core_name_fn callback, void* param);

        // SFile::List takes an SArchive: its type (0, an MPQ) then the record, as OpenEx reads it
        // ([eax], [eax+4] at 0x00424C6F). stdcall: `ret 0xc` at 0x004250C8.
        const List list  = reinterpret_cast<List>(WXL_SITE("SFile::List", 0));
        uint32_t   listed = 0;
        for (void* record : records)
        {
            struct
            {
                uint32_t type;
                void*    archive;
            } archive{0, record};
            if (record && list(&archive, fn, user)) ++listed;
        }
        return listed;
    }

    wxl_bool gx_device_exists()
    {
        return IsClient() && GxDevice() ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool gx_has_context()
    {
        // GxHasContext tests the device pointer itself, so it is safe to call without one.
        if (!IsClient()) return WXL_FALSE;
        const GxFlag call = reinterpret_cast<GxFlag>(WXL_SITE("GxHasContext", 0));
        return call() ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool gx_window_visible()
    {
        if (!IsClient()) return WXL_FALSE;
        const GxFlag call = reinterpret_cast<GxFlag>(WXL_SITE("GxIsWindowVisible", 0));
        return call() ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool gx_window_size(uint32_t* width, uint32_t* height)
    {
        // GxCapsWindowSizeInScreenCoords dereferences g_theGxDevicePtr without testing it
        // (0x00493BF3 `mov ecx,[g_theGxDevicePtr]` then `mov eax,[ecx]`), so the test is here.
        if (!IsClient() || !width || !height || !GxDevice()) return WXL_FALSE;
        const GxWindowSize call =
            reinterpret_cast<GxWindowSize>(WXL_SITE("GxCapsWindowSizeInScreenCoords", 0));
        float rect[4] = {};
        call(rect);
        const float w = rect[3] - rect[1];
        const float h = rect[2] - rect[0];
        *width  = w > 0.0f ? static_cast<uint32_t>(w + 0.5f) : 0u;
        *height = h > 0.0f ? static_cast<uint32_t>(h + 0.5f) : 0u;
        return WXL_TRUE;
    }

    // --- Gx: wxl.gx ---------------------------------------------------------------------------
    // Both D3D9 devices keep a copy of what they last set (m_deviceStates, the current
    // declaration and streams); spoiling it makes the next state sync send everything again.

    void* gx_d3d9_device()
    {
        const void* device = IsClient() ? GxDevice() : nullptr;
        if (!device) return nullptr;
        switch (Read<uint32_t>(device, WXL_FIELD("CGxDevice", "m_api")))
        {
        case 1: return Read<void*>(device, WXL_FIELD("CGxDeviceD3d", "m_d3dDevice"));     // GxApi_D3d9
        case 2: return Read<void*>(device, WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dDevice"));  // GxApi_D3d9Ex
        default: return nullptr;
        }
    }

    namespace
    {
        template <std::size_t Decl, std::size_t Streams, std::size_t StreamsSize, std::size_t States,
                  std::size_t StatesSize>
        void SpoilD3d(void* device)
        {
            uint8_t* bytes = static_cast<uint8_t*>(device);
            std::memset(bytes + States, 0xA5, StatesSize);
            std::memset(bytes + Decl, 0, sizeof(void*));
            std::memset(bytes + Streams, 0, StreamsSize);
            const int32_t dirty = 1;
            std::memcpy(bytes + WXL_FIELD("CGxDevice", "m_viewportDirty"), &dirty, sizeof dirty);
            std::memcpy(bytes + WXL_FIELD("CGxDevice", "m_scissorRectDirty"), &dirty, sizeof dirty);
        }
    }

    void gx_invalidate_state()
    {
        void* device = IsClient() ? const_cast<void*>(GxDevice()) : nullptr;
        if (!device) return;
        const uint32_t api = Read<uint32_t>(device, WXL_FIELD("CGxDevice", "m_api"));
        if (api != 1 && api != 2) return;
        // CGxDevice::IRsForceUpdate(), the overload taking nothing: __thiscall, `ret` (0x00685B4D).
        x86::CallMember<void>(WXL_SITE_AT("CGxDevice::IRsForceUpdate", 0x00685A70, 0), device);
        if (api == 1)
            SpoilD3d<WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentVertexDecl"), WXL_FIELD("CGxDeviceD3d", "m_d3dVertexStreamBuf"),
                     WXL_MEMBER_SIZE("CGxDeviceD3d", "m_d3dVertexStreamBuf"), WXL_FIELD("CGxDeviceD3d", "m_deviceStates"),
                     WXL_MEMBER_SIZE("CGxDeviceD3d", "m_deviceStates")>(device);
        else
            SpoilD3d<WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dCurrentVertexDecl"),
                     WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dVertexStreamBuf"), WXL_MEMBER_SIZE("CGxDeviceD3d9Ex", "m_d3dVertexStreamBuf"),
                     WXL_FIELD("CGxDeviceD3d9Ex", "m_deviceStates"), WXL_MEMBER_SIZE("CGxDeviceD3d9Ex", "m_deviceStates")>(device);
    }

    // CM2SceneRender::DrawBatch keeps no record of its draw's numbers: rebuilt from what it drew
    // with (ModelDrawX86.hpp).
    wxl_bool gx_model_draw(const void* render, wxl_model_batch_args* out)
    {
        if (!IsClient()) return WXL_FALSE;
        const x86::model_draw::Globals globals{
            GxDevice(), *reinterpret_cast<const int32_t*>(WXL_STATIC("CShaderEffect::s_enableShaders")),
            reinterpret_cast<const uint32_t*>(WXL_STATIC("CGxDeviceD3d::s_primitiveConversion")),
            reinterpret_cast<const uint32_t*>(WXL_STATIC("CGxDeviceD3d9Ex::s_primitiveConversion"))};
        return x86::model_draw::Rebuild(render, globals, out) ? WXL_TRUE : WXL_FALSE;
    }

    // --- M2: wxl.m2, AppearanceChanged ----------------------------------------------------------

    namespace
    {
        constexpr uint32_t kLoaded      = 0x1;     // CM2Model::m_fFlags: the file has loaded
        constexpr uint32_t kAttachRoot  = 0x40000; // CM2Model::m_fFlags: attached to the root
        constexpr uint16_t kNoIndex     = 0xFFFF;
        constexpr uint32_t kItemSlots   = 12;      // NUM_ITEM_SLOTS: CCharacterComponent::m_itemDisplayID

        bool Loaded(const void* model) { return (Read<uint32_t>(model, WXL_FIELD("CM2Model", "m_fFlags")) & kLoaded) != 0; }

        /// The parsed header (M2Data) of a model's file.
        const void* Header(const void* model)
        {
            const void* shared = Via(model, WXL_FIELD("CM2Model", "m_shared"));
            return shared ? Via(shared, WXL_FIELD("CM2Shared", "m_data")) : nullptr;
        }

        /// A parsed array of the stock client: its count, and the address of its elements.
        template <class T>
        const T* Array(const void* header, std::size_t at, uint32_t& count)
        {
            count = Read<uint32_t>(header, at);
            return count ? Read<const T*>(header, at + 4) : nullptr;
        }

        /// The point `attachment` of a loaded model's file, as M2Data::m_attachmentIndicesById maps
        /// it; kNoIndex for none.
        uint16_t AttachmentIndex(const void* header, uint32_t attachment)
        {
            uint32_t        count = 0;
            const uint16_t* ids   = Array<uint16_t>(header, WXL_FIELD("M2Data", "m_attachmentIndicesById.m_count"), count);
            return ids && attachment < count ? ids[attachment] : kNoIndex;
        }
    }

    wxl_bool component_wearer(const void* component, uint32_t item_slot, wxl_appearance_args* out)
    {
        if (!IsClient() || !component || !out) return WXL_FALSE;
        out->wearer     = const_cast<void*>(component);
        out->model      = Read<void*>(component, WXL_FIELD("CCharacterComponent", "m_data.m_model"));
        out->race_id    = Read<uint32_t>(component, WXL_FIELD("CCharacterComponent", "m_data.m_preferences.m_raceID"));
        out->sex_id     = Read<uint32_t>(component, WXL_FIELD("CCharacterComponent", "m_data.m_preferences.m_sexID"));
        out->item_slot  = item_slot;
        out->display_id = item_slot < kItemSlots
                              ? Read<uint32_t>(component, WXL_FIELD("CCharacterComponent", "m_itemDisplayID") + item_slot * 4)
                              : 0u;
        out->npc        = (Read<uint32_t>(component, WXL_FIELD("CCharacterComponent", "m_flags")) & 0x1) ? WXL_TRUE : WXL_FALSE;
        return WXL_TRUE;
    }

    void* m2_object_model(const void* object)
    {
        if (!IsClient() || !object) return nullptr;
        void* model = Read<void*>(object, WXL_FIELD("CGObject_C", "m_worldModel"));
        // Bounded: an attachment chain is a few links (weapon, rider, mount).
        for (int i = 0; model && i < 16; ++i)
        {
            void* parent = Read<void*>(model, WXL_FIELD("CM2Model", "m_attachParent"));
            if (!parent) break;
            model = parent;
        }
        return model;
    }

    wxl_bool m2_item_display(uint32_t id, wxl_m2_item_display* out)
    {
        if (!IsClient() || !out || !id) return WXL_FALSE;
        // WowClientDB_RLE<ItemDisplayInfoRec>::GetRecord(id, rec): __thiscall, `ret 8`, copies the
        // 100-byte row, decompressing it when the store is compressed (0x004CFD90).
        alignas(4) uint8_t rec[WXL_TYPE_SIZE("ItemDisplayInfoRec")];
        if (!x86::CallMember<int32_t>(WXL_SITE_AT("WowClientDB_RLE<T>::GetRecord", 0x004CFD90, 0),
                                      reinterpret_cast<const void*>(WXL_STATIC("g_itemDisplayInfoDB")),
                                      static_cast<int32_t>(id), static_cast<void*>(rec)))
            return WXL_FALSE;
        const auto text = [&](std::size_t at) {
            const char* value = Read<const char*>(rec, at);
            return value ? value : "";
        };
        out->id = Read<uint32_t>(rec, 0);
        for (int i = 0; i < 2; ++i)
        {
            out->model[i]             = text(WXL_FIELD("ItemDisplayInfoRec", "m_modelName[0]") + i * 4);
            out->model_texture[i]     = text(WXL_FIELD("ItemDisplayInfoRec", "m_modelTexture[0]") + i * 4);
            out->icon[i]              = text(WXL_FIELD("ItemDisplayInfoRec", "m_inventoryIcon[0]") + i * 4);
            out->helmet_geoset_vis[i] = Read<uint32_t>(rec, WXL_FIELD("ItemDisplayInfoRec", "m_helmetGeosetVisID") + i * 4);
        }
        for (int i = 0; i < 3; ++i)
            out->geoset_group[i] = Read<uint32_t>(rec, WXL_FIELD("ItemDisplayInfoRec", "m_geosetGroup") + i * 4);
        for (int i = 0; i < 8; ++i) out->texture[i] = text(WXL_FIELD("ItemDisplayInfoRec", "m_texture") + i * 4);
        out->flags             = Read<uint32_t>(rec, WXL_FIELD("ItemDisplayInfoRec", "m_flags"));
        out->spell_visual_id   = Read<uint32_t>(rec, WXL_FIELD("ItemDisplayInfoRec", "m_spellVisualID"));
        out->group_sound_index = Read<uint32_t>(rec, WXL_FIELD("ItemDisplayInfoRec", "m_groupSoundIndex"));
        out->item_visual       = Read<int32_t>(rec, WXL_FIELD("ItemDisplayInfoRec", "m_itemVisual"));
        out->particle_color_id = Read<uint32_t>(rec, WXL_FIELD("ItemDisplayInfoRec", "m_particleColorID"));
        return WXL_TRUE;
    }

    const char* m2_race_prefix(uint32_t race)
    {
        if (!IsClient()) return nullptr;
        // WowClientDB<ChrRacesRec>::GetRecord, read in place: m_recordsById[id - m_minID].
        const void*   store = reinterpret_cast<const void*>(WXL_STATIC("g_chrRacesDB"));
        const int32_t minId = Read<int32_t>(store, WXL_FIELD("WowClientDB_SpellRec", "m_minID"));
        const int32_t maxId = Read<int32_t>(store, WXL_FIELD("WowClientDB_SpellRec", "m_maxID"));
        const int32_t id    = static_cast<int32_t>(race);
        if (id < minId || id > maxId) return nullptr;
        const void* const* byId = Read<const void* const*>(store, WXL_FIELD("WowClientDB_SpellRec", "m_recordsById"));
        const void*        rec  = byId ? byId[id - minId] : nullptr;
        return rec ? Read<const char*>(rec, WXL_FIELD("ChrRacesRec", "m_clientPrefix")) : nullptr;
    }

    void* m2_create_model(void* scene_model, const char* path)
    {
        if (!IsClient() || !scene_model || !path || !*path) return nullptr;
        void* scene = Read<void*>(scene_model, WXL_FIELD("CM2Model", "m_scene"));
        // CM2Scene::CreateModel(path, flags): __thiscall, `ret 8` (0x0081F8FF).
        return scene ? x86::CallMember<void*>(WXL_SITE("CM2Scene::CreateModel", 0), scene, path, uint32_t(0)) : nullptr;
    }

    void m2_release_model(void* model)
    {
        // CM2Model::Release(): __thiscall, `ret` (0x00824EFC).
        if (IsClient() && model) x86::CallMember<void>(WXL_SITE("CM2Model::Release", 0), model);
    }

    wxl_bool m2_attach_model(void* model, void* parent, uint32_t attachment, wxl_bool to_root)
    {
        if (!IsClient() || !model || !parent || model == parent) return WXL_FALSE;
        // AttachToParent drops a request for a point a loaded parent lacks without saying so.
        if (!to_root && Loaded(parent))
        {
            const void* header = Header(parent);
            if (!header || AttachmentIndex(header, attachment) == kNoIndex) return WXL_FALSE;
        }
        // CM2Model::AttachToParent(parent, id, position, toRoot): __thiscall, `ret 0x10` (0x008317D1).
        x86::CallMember<void>(WXL_SITE("CM2Model::AttachToParent", 0), model, parent, attachment,
                              static_cast<const void*>(nullptr), static_cast<int32_t>(to_root ? 1 : 0));
        return Read<void*>(model, WXL_FIELD("CM2Model", "m_attachParent")) == parent ? WXL_TRUE : WXL_FALSE;
    }

    void m2_detach_model(void* model)
    {
        // CM2Model::DetachFromParent(): __thiscall, `ret` (0x00827552).
        if (IsClient() && model && Read<void*>(model, WXL_FIELD("CM2Model", "m_attachParent")))
            x86::CallMember<void>(WXL_SITE("CM2Model::DetachFromParent", 0), model);
    }

    void* m2_model_parent(const void* model)
    {
        return IsClient() && model ? Read<void*>(model, WXL_FIELD("CM2Model", "m_attachParent")) : nullptr;
    }

    void* m2_model_source(const void* model)
    {
        return IsClient() && model ? Read<void*>(model, WXL_FIELD("CM2Model", "m_model30")) : nullptr;
    }

    void m2_set_geosets(void* model, uint32_t first, uint32_t last, wxl_bool visible)
    {
        // CM2Model::SetGeometryVisible(first, last, visible): __thiscall, `ret 0xc` (0x0082C841).
        if (IsClient() && model)
            x86::CallMember<void>(WXL_SITE("CM2Model::SetGeometryVisible", 0), model, first, last,
                                  static_cast<int32_t>(visible ? 1 : 0));
    }

    wxl_bool m2_replace_texture(void* model, uint32_t type, const char* path)
    {
        if (!IsClient() || !model || !path || !*path) return WXL_FALSE;
        // CreateTexture(fileName, status): cdecl, two dwords (TextureCreate with the component's
        // flags, 0x004E8D30). The texture loads later and reports into the status then: the
        // client's own, GetGlobalStatusObj (cdecl, nothing, `mov eax, imm32; ret`, 0x0047C090).
        void* status  = x86::CallFree<void*>(WXL_SITE("GetGlobalStatusObj", 0));
        void* texture = x86::CallFree<void*>(WXL_SITE("CreateTexture", 0), path, status);
        if (!texture) return WXL_FALSE;
        // CM2Model::ReplaceTexture(type, texture): __thiscall, `ret 8`; the model takes a handle of
        // its own, as CCharacterComponent::AddLink relies on, so this one goes back: HandleClose,
        // cdecl, one dword (0x0047BF30).
        x86::CallMember<void>(WXL_SITE("CM2Model::ReplaceTexture", 0), model, type, texture);
        x86::CallFree<void>(WXL_SITE("HandleClose", 0), texture);
        return WXL_TRUE;
    }

    uint32_t m2_children(const void* model, void** out, uint32_t capacity)
    {
        if (!IsClient() || !model) return 0;
        uint32_t count = 0;
        for (void* child = Read<void*>(model, WXL_FIELD("CM2Model", "m_attachList")); child && count < 4096;
             child       = Read<void*>(child, WXL_FIELD("CM2Model", "m_attachNext")))
        {
            if (out && count < capacity) out[count] = child;
            ++count;
        }
        return count;
    }

    wxl_bool m2_attach_info(const void* model, uint32_t* attachment, wxl_bool* to_root)
    {
        if (!IsClient() || !model || !attachment || !to_root || !m2_model_parent(model)) return WXL_FALSE;
        *attachment = Read<uint32_t>(model, WXL_FIELD("CM2Model", "m_attachmentId"));
        *to_root    = (Read<uint32_t>(model, WXL_FIELD("CM2Model", "m_fFlags")) & kAttachRoot) ? WXL_TRUE : WXL_FALSE;
        return WXL_TRUE;
    }

    wxl_bool m2_attach_copy(void* source, void* parent, uint32_t attachment, wxl_bool to_root)
    {
        if (!IsClient() || !source || !parent) return WXL_FALSE;
        void* scene = Read<void*>(parent, WXL_FIELD("CM2Model", "m_scene"));
        if (!scene) return WXL_FALSE;
        // CM2Scene::DuplicateModel(model, flags): __thiscall, `ret 8` (0x0081F9D4).
        void* copy = x86::CallMember<void*>(WXL_SITE("CM2Scene::DuplicateModel", 0), scene, source, uint32_t(0));
        if (!copy) return WXL_FALSE;
        x86::CallMember<void>(WXL_SITE("CM2Model::AttachToParent", 0), copy, parent, attachment,
                              static_cast<const void*>(nullptr), static_cast<int32_t>(to_root ? 1 : 0));
        const wxl_bool attached = Read<void*>(copy, WXL_FIELD("CM2Model", "m_attachParent")) == parent ? WXL_TRUE : WXL_FALSE;
        m2_release_model(copy); // the parent holds it now, as CM2Model::Initialize leaves its copies
        return attached;
    }

    void* m2_model_file(const void* model)
    {
        return IsClient() && model ? Read<void*>(model, WXL_FIELD("CM2Model", "m_shared")) : nullptr;
    }

    wxl_bool m2_attachment_point(const void* model, uint32_t attachment, wxl_vec3* out)
    {
        if (!IsClient() || !model || !out || !Loaded(model)) return WXL_FALSE;
        const void* header = Header(model);
        if (!header) return WXL_FALSE;
        const uint16_t index = AttachmentIndex(header, attachment);
        uint32_t       count = 0;
        const uint8_t* points = Array<uint8_t>(header, WXL_FIELD("M2Data", "m_attachments.m_count"), count);
        if (!points || index >= count) return WXL_FALSE;
        *out = Read<wxl_vec3>(points + static_cast<std::size_t>(index) * WXL_TYPE_SIZE("M2Attachment"),
                              WXL_FIELD("M2Attachment", "m_position"));
        return WXL_TRUE;
    }

    wxl_bool m2_skeleton(void* model, wxl_core_m2_skeleton* out)
    {
        if (!IsClient() || !model || !out || !Loaded(model)) return WXL_FALSE;
        const void* header = Header(model);
        float*      matrices = Read<float*>(model, WXL_FIELD("CM2Model", "m_boneMatrices"));
        if (!header || !matrices) return WXL_FALSE;
        out->file        = header;
        out->bones       = Array<uint8_t>(header, WXL_FIELD("M2Data", "m_bones.m_count"), out->bone_count);
        out->bone_stride = WXL_TYPE_SIZE("M2CompBone");
        out->key_bones   = Array<uint16_t>(header, WXL_FIELD("M2Data", "m_boneIndicesById.m_count"), out->key_bone_count);
        out->matrices    = matrices;
        return out->bones ? WXL_TRUE : WXL_FALSE;
    }

    // --- M2: the model loader -------------------------------------------------------------------

    wxl_bool m2_file_info(const void* file, wxl_core_m2_file* out)
    {
        if (!IsClient() || !file || !out) return WXL_FALSE;
        void* header = Read<void*>(file, WXL_FIELD("CM2Shared", "m_data"));
        if (!header) return WXL_FALSE;
        out->path   = static_cast<const char*>(file) + WXL_FIELD("CM2Shared", "m_fileName");
        out->header = header;
        out->size   = Read<uint32_t>(file, WXL_FIELD("CM2Shared", "m_fileSize"));
        out->skin   = Read<void*>(file, WXL_FIELD("CM2Shared", "m_skinData"));
        return WXL_TRUE;
    }

    void m2_file_animate_bones(void* file)
    {
        if (!IsClient() || !file) return;
        uint8_t* count = static_cast<uint8_t*>(file) + WXL_FIELD("CM2Shared", "m_billboardBoneCount");
        uint32_t value;
        std::memcpy(&value, count, sizeof value);
        ++value;
        std::memcpy(count, &value, sizeof value);
    }

    wxl_bool object_position(const void* object, wxl_vec3* out)
    {
        if (!IsClient() || !object || !out) return WXL_FALSE;
        switch (object_type_id(object))
        {
        case WXL_OBJECT_UNIT:
        case WXL_OBJECT_PLAYER: return CallPosition(WXL_SITE("CGUnit_C::GetPosition", 0), object, out);
        case WXL_OBJECT_GAMEOBJECT: return CallPosition(WXL_SITE("CGGameObject_C::GetPosition", 0), object, out);
        case WXL_OBJECT_DYNAMIC: return CallPosition(WXL_SITE("CGDynamicObject_C::GetPosition", 0), object, out);
        case WXL_OBJECT_CORPSE:
            // The fork declares no CGCorpse_C::GetPosition; the corpse's CPassenger holds it.
            *out = Read<wxl_vec3>(object, kCorpsePosition);
            return WXL_TRUE;
        default: return WXL_FALSE;
        }
    }

    wxl_bool object_facing(const void* object, float* out)
    {
        if (!IsClient() || !object || !out) return WXL_FALSE;
        switch (object_type_id(object))
        {
        case WXL_OBJECT_UNIT:
        case WXL_OBJECT_PLAYER: return CallFacing(WXL_SITE("CGUnit_C::GetFacing", 0), object, out);
        case WXL_OBJECT_GAMEOBJECT: return CallFacing(WXL_SITE("CGGameObject_C::GetFacing", 0), object, out);
        case WXL_OBJECT_DYNAMIC: return CallFacing(WXL_SITE("CGDynamicObject_C::GetFacing", 0), object, out);
        case WXL_OBJECT_CORPSE: return CallFacing(WXL_SITE("CGCorpse_C::GetFacing", 0), object, out);
        default: return WXL_FALSE;
        }
    }

    const char* object_name(const void* object)
    {
        if (!IsClient() || !object) return nullptr;
        switch (object_type_id(object))
        {
        case WXL_OBJECT_UNIT:
        case WXL_OBJECT_PLAYER: return CallName(WXL_SITE("CGUnit_C::GetObjectName", 0), object);
        case WXL_OBJECT_GAMEOBJECT: return CallName(WXL_SITE("CGGameObject_C::GetObjectName", 0), object);
        case WXL_OBJECT_ITEM:
        case WXL_OBJECT_CONTAINER: return CallName(WXL_SITE("CGItem_C::GetObjectName", 0), object);
        default: return nullptr;
        }
    }

    // --- unit.def, item.def: what the client works out of an object ---------------------------
    //
    // Each a call into the client, at the address its OFFSET tag gives, by the convention its
    // code has (checked in Wow.exe, below each). The object manager is the main thread's.

    using x86::CallFree;
    using x86::CallMember;

    wxl_bool unit_speed(const void* object, uint32_t index, float* out)
    {
        // CMovementShared: m_walkSpeed (+0x90) to m_pitchRate (+0xB0), nine floats in a row.
        static_assert(WXL_FIELD("CMovementShared", "m_pitchRate") - WXL_FIELD("CMovementShared", "m_walkSpeed") ==
                          8 * sizeof(float),
                      "the movement block's nine speeds follow each other");
        const void* movement = IsClient() ? Via(object, WXL_FIELD("CGUnit_C", "m_movement")) : nullptr;
        if (!movement || !out || index > 8) return WXL_FALSE;
        *out = Read<float>(movement, WXL_FIELD("CMovementShared", "m_walkSpeed") + index * sizeof(float));
        return WXL_TRUE;
    }

    int32_t unit_reaction(const void* object, const void* other)
    {
        // The (const CGUnit_C*) overload at 0x007251C0: __thiscall, `ret 4` (0x007254DB).
        if (!IsClient() || !OnMainThread() || !object || !other) return -1;
        return CallMember<int32_t>(WXL_SITE_AT("CGUnit_C::UnitReaction", 0x007251C0, 0), object, other);
    }

    int32_t unit_creature_type(const void* object)
    {
        // __thiscall, no argument, `ret`.
        if (!IsClient() || !object) return 0;
        return CallMember<int32_t>(WXL_SITE("CGUnit_C::GetCreatureType", 0), object);
    }

    uint32_t unit_creature_family(const void* object)
    {
        if (!IsClient() || !object) return 0;
        return CallMember<uint32_t>(WXL_SITE("CGUnit_C::GetCreatureFamily", 0), object);
    }

    uint32_t unit_classification(const void* object)
    {
        if (!IsClient() || !object) return 0;
        return CallMember<uint32_t>(WXL_SITE("CGUnit_C::GetCreatureClassification", 0), object);
    }

    wxl_bool unit_knows_spell(const void* object, uint32_t spell)
    {
        // __thiscall, `ret 4` (0x00726127); the answer is AL alone (`setne al`).
        if (!IsClient() || !OnMainThread() || !object) return WXL_FALSE;
        return CallMember<bool>(WXL_SITE("CGUnit_C::IsSpellKnown", 0), object, spell) ? WXL_TRUE : WXL_FALSE;
    }

    uint32_t unit_aura_count(const void* object)
    {
        if (!IsClient() || !object) return 0;
        return CallMember<uint32_t>(WXL_SITE("CGUnit_C::GetAuraCount", 0), object);
    }

    const void* unit_aura_at(const void* object, uint32_t index)
    {
        // __thiscall, `ret 4` (0x00556E45); null past the count.
        if (!IsClient() || !object) return nullptr;
        return CallMember<const void*>(WXL_SITE("CGUnit_C::GetAura", 0), object, index);
    }

    wxl_bool unit_is_outdoors(const void* object)
    {
        // __thiscall, no argument: World::QueryOutdoors(m_worldObject); 0 or 1 in EAX
        // (`neg eax; sbb eax, eax; neg eax` at 0x0071B7FF), a bare `ret`.
        if (!IsClient() || !OnMainThread() || !object) return WXL_FALSE;
        return CallMember<int32_t>(WXL_SITE("CGUnit_C::IsOutdoors", 0), object) ? WXL_TRUE : WXL_FALSE;
    }

    const void* item_stats(const void* object)
    {
        // __thiscall, no argument: g_itemDBCache.GetRecord(entry, no guid, no callback).
        if (!IsClient() || !OnMainThread() || !object) return nullptr;
        return CallMember<const void*>(WXL_SITE("CGItem_C::GetStats", 0), object);
    }

    // --- action.def: the clock, the group, what the player can do ------------------------------

    uint64_t party_member_guid(uint32_t index)
    {
        if (!IsClient() || index >= 4) return 0;
        return reinterpret_cast<const uint64_t*>(WXL_STATIC("CGPartyInfo::m_members"))[index];
    }

    uint32_t raid_member_count(void)
    {
        return IsClient() ? *reinterpret_cast<const uint32_t*>(WXL_STATIC("CGRaidInfo::m_numMembers")) : 0;
    }

    uint64_t raid_member_guid(uint32_t index)
    {
        if (!IsClient() || index >= 40) return 0;
        const void* member = reinterpret_cast<const void* const*>(WXL_STATIC("CGRaidInfo::m_members"))[index];
        return member ? Read<uint64_t>(member, WXL_FIELD("CGRaidMember", "m_guid")) : 0;
    }

    wxl_bool action_target(uint64_t guid)
    {
        // cdecl (WOWGUID by value: two dwords), the caller pops 8 (0x00525A25).
        if (!IsClient() || !OnMainThread()) return WXL_FALSE;
        CallFree<void>(WXL_SITE("CGGameUI::Target", 0), guid);
        return WXL_TRUE;
    }

    wxl_bool action_interact(uint64_t guid)
    {
        // cdecl (WOWGUID by value), int answer: 0 when the manager does not hold the object.
        if (!IsClient() || !OnMainThread()) return WXL_FALSE;
        return CallFree<int32_t>(WXL_SITE("CGGameUI::OnSpriteRightClick", 0), guid) ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool action_cast_spell(uint32_t spell, uint64_t target)
    {
        // Spell_C::CastSpell(spellId, item, WOWGUID target, confirmed): cdecl, five dwords
        // (0x0080DA40 hands them on to Spell_C_CastSpell). Its bool answer is AL.
        if (!IsClient() || !OnMainThread()) return WXL_FALSE;
        return CallFree<bool>(WXL_SITE("Spell_C::CastSpell", 0), spell, static_cast<const void*>(nullptr), target,
                              int32_t(0))
                   ? WXL_TRUE
                   : WXL_FALSE;
    }

    wxl_bool action_stop_casting(void)
    {
        // Spell_C_IsActivePlayerCasting and Spell_C_StopCasting take nothing and touch no register
        // a caller keeps (0x007FFDE0, 0x00809A80); Spell_C_CancelAutoRepeat(bool) is cdecl.
        if (!IsClient() || !OnMainThread()) return WXL_FALSE;
        if (CallFree<int32_t>(WXL_SITE("Spell_C_IsActivePlayerCasting", 0)) & 0xFF)
        {
            CallFree<void>(WXL_SITE("Spell_C_StopCasting", 0));
            return WXL_TRUE;
        }
        if (!CallFree<int32_t>(WXL_SITE("Spell_C_GetAutoRepeatingSpell", 0))) return WXL_FALSE;
        CallFree<void>(WXL_SITE("Spell_C_CancelAutoRepeat", 0), int32_t(1));
        return WXL_TRUE;
    }

    wxl_bool action_use_item(const void* item, uint64_t target)
    {
        // CGItem_C::UseItem(WOWGUID* target, int confirmed): __thiscall, `ret 8`; its answer
        // is AL (Wow.exe returns a byte).
        if (!IsClient() || !OnMainThread() || !item) return WXL_FALSE;
        uint64_t on = target;
        return CallMember<bool>(WXL_SITE("CGItem_C::UseItem", 0), item, &on, int32_t(0)) ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool action_follow(const void* mover, uint64_t guid)
    {
        // CGUnit_C::SetTrackingTarget(const WOWGUID&, int type): __thiscall, `ret 8`; bool in AL.
        if (!IsClient() || !OnMainThread() || !mover) return WXL_FALSE;
        constexpr int32_t kTrackFollow = 3; // TRACKTYPE_FOLLOW
        const uint64_t    target       = guid;
        return CallMember<bool>(WXL_SITE("CGUnit_C::SetTrackingTarget", 0), mover, &target, kTrackFollow) ? WXL_TRUE
                                                                                                         : WXL_FALSE;
    }

    wxl_bool action_move_to(const void* player, wxl_vec3 point)
    {
        // CGPlayer_C::ClickToMove(int type, const WOWGUID*, const C3Vector*, float facing):
        // __thiscall, `ret 0x10`; bool in AL.
        if (!IsClient() || !OnMainThread() || !player) return WXL_FALSE;
        constexpr int32_t kTrackMove = 4; // TRACKTYPE_MOVE
        const uint64_t    none       = 0;
        return CallMember<bool>(WXL_SITE("CGPlayer_C::ClickToMove", 0), player, kTrackMove, &none, &point, 0.0f)
                   ? WXL_TRUE
                   : WXL_FALSE;
    }

    wxl_bool action_face(const void* player, float facing)
    {
        // CGInputControl::CanTurn(CGUnit_C*): __stdcall, `ret 4`; int in EAX.
        // CGUnit_C::OnSetRawFacingLocal(uint32_t time, float facing): __thiscall, `ret 8`, called
        // so by CameraTurnPlayer (0x005FB33A: the facing then the time pushed, ECX the mover).
        using CanTurnFn = int32_t(__stdcall*)(const void* unit);
        if (!IsClient() || !OnMainThread() || !player) return WXL_FALSE;
        const uint64_t mover = *reinterpret_cast<const uint64_t*>(WXL_STATIC("CGUnit_C::s_activeMover"));
        if (mover != object_guid(player)) return WXL_FALSE;
        if (!reinterpret_cast<CanTurnFn>(WXL_SITE("CGInputControl::CanTurn", 0))(player)) return WXL_FALSE;
        CallMember<void>(WXL_SITE("CGUnit_C::OnSetRawFacingLocal", 0), player, os_time_ms(), facing);
        return WXL_TRUE;
    }

    // --- Camera ---------------------------------------------------------------------------

    namespace
    {
        // CSimpleCamera's facing is a C33Matrix of three rows, forward, the client's right, up.
        constexpr std::size_t kFacing = WXL_FIELD("CSimpleCamera", "m_facing.a0");

        wxl_vec3 Row(const void* camera, uint32_t row)
        {
            return Read<wxl_vec3>(camera, kFacing + row * sizeof(wxl_vec3));
        }

        float Dot(const wxl_vec3& a, const wxl_vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

        /// v * m, the client's operator*(C3Vector, C33Matrix): v's components weigh m's rows.
        wxl_vec3 Times(const wxl_vec3& v, const float (&m)[9])
        {
            return wxl_vec3{v.x * m[0] + v.y * m[3] + v.z * m[6], v.x * m[1] + v.y * m[4] + v.z * m[7],
                            v.x * m[2] + v.y * m[5] + v.z * m[8]};
        }

        /// The facing rows CSimpleCamera::SetFacing(forward) builds (FaceDirection, 0x00607DF0):
        /// the direction normalised, the client's right a quarter turn to the left of it in the
        /// ground plane (+x when the direction is vertical), and up from the two. False for a
        /// zero direction.
        bool FaceDirection(wxl_vec3 direction, wxl_vec3 (&rows)[3])
        {
            const float length = std::sqrt(Dot(direction, direction));
            if (!(length > 1e-6f)) return false;
            const wxl_vec3 f{direction.x / length, direction.y / length, direction.z / length};
            wxl_vec3       r{1.0f, 0.0f, 0.0f};
            const float    flat = f.x * f.x + f.y * f.y;
            if (flat >= 0.00000023841858f)
            {
                const float scale = 1.0f / std::sqrt(flat);
                r                 = wxl_vec3{-f.y * scale, f.x * scale, 0.0f};
            }
            rows[0] = f;
            rows[1] = r;
            rows[2] = wxl_vec3{-(r.y * f.z), f.z * r.x, r.y * f.x - f.y * r.x};
            return true;
        }

        /// The rotation from the camera's parent to the world, when it has one: ParentToWorld,
        /// __thiscall with the C33Matrix returned through a hidden pointer, `ret 4`.
        bool ParentRotation(const void* camera, float (&m)[9])
        {
            if (!Read<uint64_t>(camera, WXL_FIELD("CGCamera", "m_relativeTo"))) return false;
            CallMember<void*>(WXL_SITE("CGCamera::ParentToWorld", 0), camera, static_cast<void*>(m));
            return true;
        }
    }

    void* camera_world(void)
    {
        if (!IsClient()) return nullptr;
        const void* frame = *reinterpret_cast<const void* const*>(WXL_STATIC("CGWorldFrame::s_currentWorldFrame"));
        return frame ? Read<void*>(frame, WXL_FIELD("CGWorldFrame", "m_camera")) : nullptr;
    }

    wxl_bool camera_read(const void* camera, wxl_core_camera* out)
    {
        if (!IsClient() || !camera || !out) return WXL_FALSE;
        out->position = Read<wxl_vec3>(camera, WXL_FIELD("CSimpleCamera", "m_position"));
        out->forward  = Row(camera, 0);
        out->left     = Row(camera, 1);
        out->up       = Row(camera, 2);
        float parent[9];
        if (ParentRotation(camera, parent))
        {
            // CGCamera::Forward/Right/Up: the row times ParentToWorld().
            out->forward = Times(out->forward, parent);
            out->left    = Times(out->left, parent);
            out->up      = Times(out->up, parent);
        }
        // CGCamera::FOV: m_targetFov + m_fov, clamped to [0, 3.1241393].
        out->fov_base    = Read<float>(camera, WXL_FIELD("CSimpleCamera", "m_fov"));
        const float fov  = Read<float>(camera, WXL_FIELD("CGCamera", "m_targetFov")) + out->fov_base;
        out->fov         = fov < 0.0f ? 0.0f : (fov >= 3.1241393f ? 3.1241393f : fov);
        out->near_z      = Read<float>(camera, WXL_FIELD("CSimpleCamera", "m_nearZ"));
        out->far_z       = Read<float>(camera, WXL_FIELD("CSimpleCamera", "m_farZ"));
        out->aspect      = Read<float>(camera, WXL_FIELD("CSimpleCamera", "m_aspect"));
        out->pivot       = Read<wxl_vec3>(camera, WXL_FIELD("CGCamera", "m_targetPosition"));
        out->pivot.z    += Read<float>(camera, WXL_FIELD("CGCamera", "m_height"));
        out->followed    = Read<uint64_t>(camera, WXL_FIELD("CGCamera", "m_targetGUID"));
        out->cinematic   = Read<const void*>(camera, WXL_FIELD("CGCamera", "m_model")) ? 1u : 0u;
        return WXL_TRUE;
    }

    void camera_write(void* camera, const wxl_core_camera* view)
    {
        if (!IsClient() || !camera || !view) return;
        wxl_vec3 rows[3];
        if (!FaceDirection(view->forward, rows)) return;
        float parent[9];
        if (ParentRotation(camera, parent))
        {
            // The facing is kept in the parent's frame: each world row times the inverse
            // rotation, which is the transpose, so its dot with each of the parent's rows.
            for (wxl_vec3& row : rows)
            {
                const wxl_vec3 world = row;
                row = wxl_vec3{Dot(world, wxl_vec3{parent[0], parent[1], parent[2]}),
                               Dot(world, wxl_vec3{parent[3], parent[4], parent[5]}),
                               Dot(world, wxl_vec3{parent[6], parent[7], parent[8]})};
            }
        }
        char* bytes = static_cast<char*>(camera);
        std::memcpy(bytes + WXL_FIELD("CSimpleCamera", "m_position"), &view->position, sizeof(wxl_vec3));
        std::memcpy(bytes + kFacing, rows, sizeof rows);
        std::memcpy(bytes + WXL_FIELD("CSimpleCamera", "m_fov"), &view->fov_base, sizeof(float));
    }

    wxl_bool world_trace(wxl_vec3 start, wxl_vec3 finish, uint32_t flags, wxl_vec3* hit)
    {
        // World::Intersect(const C3Vector&, const C3Vector&, C3Vector*, float*, uint32_t,
        // HitTestDebug*): cdecl, a tail jump to CMap::Intersect (0x0077F314), which reads its six
        // arguments and returns with a bare `ret`; bool in AL.
        using IntersectFn = int32_t(__cdecl*)(const wxl_vec3* start, const wxl_vec3* end, wxl_vec3* hit,
                                              float* fraction, uint32_t flags, void* debug);
        if (!IsClient() || !OnMainThread() || !hit || map_id() < 0) return WXL_FALSE;
        float fraction = 1.0f;
        wxl_vec3 point{};
        const int32_t met = reinterpret_cast<IntersectFn>(WXL_SITE("World::Intersect", 0))(&start, &finish, &point,
                                                                                           &fraction, flags, nullptr);
        if (!(met & 0xFF)) return WXL_FALSE;
        *hit = point;
        return WXL_TRUE;
    }

    // --- Input ----------------------------------------------------------------------------

    namespace
    {
        // CGxDevice::CursorSetVisible(int) is the device vtable's slot 73 (+0x124):
        // CGInputControl::UpdateMouseMode calls it so (`mov edx, [eax+0x124]; call edx` at
        // 0x005FA8FA), and every device overrides it (CGxDeviceD3d at 0x0068E750).
        constexpr std::size_t kCursorSetVisibleSlot = 0x124 / sizeof(void*);
        using CursorSetVisibleFn = void(__fastcall*)(void* self, void* edx, int32_t visible);

        void* Device() { return *reinterpret_cast<void* const*>(WXL_STATIC("g_theGxDevicePtr")); }
    }

    wxl_bool input_cursor_visible(void)
    {
        const void* device = IsClient() ? Device() : nullptr;
        return device && Read<int32_t>(device, WXL_FIELD("CGxDevice", "m_cursorVisible")) ? WXL_TRUE : WXL_FALSE;
    }

    void input_cursor_set_visible(wxl_bool visible)
    {
        void* device = IsClient() ? Device() : nullptr;
        if (!device) return;
        const void* const* vtable = Read<const void* const*>(device, 0);
        reinterpret_cast<CursorSetVisibleFn>(const_cast<void*>(vtable[kCursorSetVisibleSlot]))(device, nullptr,
                                                                                               visible ? 1 : 0);
    }

    void input_cursor_restore(void)
    {
        // CGInputControl::UpdateMouseMode(int force): __thiscall, `ret 4` (0x005FA90B).
        if (!IsClient()) return;
        const void* control = *reinterpret_cast<const void* const*>(WXL_STATIC("CGInputControl::s_inputControl"));
        if (control)
            CallMember<void>(WXL_SITE("CGInputControl::UpdateMouseMode", 0), control, int32_t(1));
        else
            input_cursor_set_visible(WXL_TRUE);
    }

    wxl_bool input_pointer_over_world(void)
    {
        if (!IsClient()) return WXL_FALSE;
        const void* frame = *reinterpret_cast<const void* const*>(WXL_STATIC("CGWorldFrame::s_currentWorldFrame"));
        const void* top   = *reinterpret_cast<const void* const*>(WXL_STATIC("CSimpleTop::s_instance"));
        if (!frame || !top) return WXL_FALSE;
        return Read<const void*>(top, WXL_FIELD("CSimpleTop", "m_mouseFocus")) == frame ? WXL_TRUE : WXL_FALSE;
    }

    // --- The game's mouse look -------------------------------------------------------------

    uint32_t input_pending_action(const void* control)
    {
        if (!IsClient() || !control) return 0;
        return Read<uint32_t>(control, WXL_FIELD("CGInputControl", "m_pendingDefaultAction"));
    }

    void input_clear_pending_action(void* control)
    {
        if (!IsClient() || !control) return;
        const uint32_t none = 0; // INPUT_RELEASE_NONE
        std::memcpy(static_cast<char*>(control) + WXL_FIELD("CGInputControl", "m_pendingDefaultAction"), &none, sizeof none);
    }

    wxl_bool input_default_action(uint32_t button)
    {
        // CGWorldFrame::PerformDefaultAction(MOUSEBUTTON): __thiscall, `ret 4` (0x004F7A38).
        if (!IsClient() || !OnMainThread()) return WXL_FALSE;
        const void* frame = *reinterpret_cast<const void* const*>(WXL_STATIC("CGWorldFrame::s_currentWorldFrame"));
        if (!frame) return WXL_FALSE;
        CallMember<void>(WXL_SITE("CGWorldFrame::PerformDefaultAction", 0), frame, button);
        return WXL_TRUE;
    }

    void input_mouse_look_stop(void)
    {
        // CGInputControl::UnsetControlBit(bit, time, keepSmoothing): __thiscall, `ret 0xC`
        // (0x005FA46C, 0x005FA6A6). Its gate lets bits it did not keep go through.
        if (!IsClient() || !OnMainThread()) return;
        void* control = *reinterpret_cast<void* const*>(WXL_STATIC("CGInputControl::s_inputControl"));
        if (!control) return;
        if (!(Read<uint32_t>(control, WXL_FIELD("CGInputControl", "m_flags")) & 0x3u)) return;
        input_clear_pending_action(control);
        CallMember<int32_t>(WXL_SITE("CGInputControl::UnsetControlBit", 0), control, 0x3u, os_time_ms(), int32_t(0));
    }

    // --- Pose -----------------------------------------------------------------------------

    void model_bone_flags(void* model, uint32_t bone, uint32_t flags, uint32_t mask)
    {
        // CM2Model::SetBoneFlags(boneId, flags, mask): __thiscall, `ret 0xC` (0x00826642).
        if (!IsClient() || !model) return;
        CallMember<void>(WXL_SITE("CM2Model::SetBoneFlags", 0), model, bone, flags, mask);
    }

    void model_bone_transform(void* model, uint32_t bone, const float* matrix)
    {
        // CM2Model::SetBoneProceduralTransform(boneId, const C44Matrix&): __thiscall, `ret 8`
        // (0x00827357); the matrix is copied.
        if (!IsClient() || !model || !matrix) return;
        CallMember<void>(WXL_SITE("CM2Model::SetBoneProceduralTransform", 0), model, bone, matrix);
    }

    // --- Memory ---------------------------------------------------------------------------

    namespace
    {
        /**
         * @brief Calls `visit` with every element of a TSList, from its terminator.
         *
         * A TSList starts with the offset of the link inside each element; a link is its
         * previous link then its next element, and the terminator's next is the first element. A
         * next with its low bit set (or null) is the terminator again: the end.
         */
        template <class F>
        void WalkList(const uint8_t* list, std::size_t terminatorNext, F&& visit)
        {
            const std::ptrdiff_t linkOffset = Read<std::ptrdiff_t>(list, 0);
            const uint8_t*       node       = Read<const uint8_t*>(list, terminatorNext);
            for (uint32_t guard = 0; node && !(reinterpret_cast<uintptr_t>(node) & 1) && guard < (1u << 20); ++guard)
            {
                visit(node);
                node = Read<const uint8_t*>(node, static_cast<std::size_t>(linkOffset) + sizeof(void*));
            }
        }

        /// The bytes a MSVC 9 std::vector reserves: its first and its end of storage, at +4 and +0xC
        /// (InitializeTables reads the hash table's at [rec+0x1C0] and [rec+0x1C4], 0x0045B8F4).
        uint64_t VectorBytes(const uint8_t* vector)
        {
            const uintptr_t first = Read<uintptr_t>(vector, 4);
            const uintptr_t end   = Read<uintptr_t>(vector, 0xC);
            return first && end > first ? end - first : 0;
        }

        /// The bytes a BitPacked* holds: its bit count at +4, rounded up to bytes.
        uint64_t BitPackedBytes(const uint8_t* bits)
        {
            return bits ? (static_cast<uint64_t>(Read<uint32_t>(bits, 4)) + 7) / 8 + 8 : 0;
        }

        /**
         * @brief What one ARCHIVEREC says of itself.
         *
         * The layout is the fork's (SFileArchives.h, measured for Win32 in its comments), checked
         * against the code that reads it: the tables-initialised flag at 0x1D9, the unpacked hash
         * table (a vector of 16-byte entries) at 0x1BC, the packed block table at 0x1A4 and the
         * header's hash table size at 0x40 (all read by ARCHIVEREC::InitializeTables, 0x0045B8D2
         * to 0x0045B926). The priority is at 0x1C, the name inline at 0x84, the header's block
         * table size at 0x44, the unpacked block table (0x31-byte entries) at 0x1AC, the packed
         * hash table at 0x1A8 and the listfile, a std::map of names, at 0x204 with its size at +8.
         * A listfile name is counted at 96 bytes: a map node and the name's heap block.
         */
        bool DescribeArchive(const uint8_t* rec, wxl_core_archive* out)
        {
            if (!rec || !out) return false;
            // m_name, or a folder archive's m_directPath (a std::string at 0x188).
            out->path          = wxl::core::storm::ArchiveName(reinterpret_cast<const char*>(rec + 0x84),
                                                               wxl::core::storm::Msvc9String(rec + 0x188));
            out->priority      = Read<int32_t>(rec, 0x1C);
            out->hash_entries  = Read<uint32_t>(rec, 0x40);
            out->block_entries = Read<uint32_t>(rec, 0x44);

            uint64_t bytes = VectorBytes(rec + 0x1BC) + VectorBytes(rec + 0x1AC);
            if (const uint8_t* blocks = Read<const uint8_t*>(rec, 0x1A4))
            {
                // BlockEntryTable: its vectors of flags, file times, CRCs and MD5s at 0, 0x54,
                // 0x64 and 0x74, its packed hashes and entries at 0x48 and 0x4C; 0x88 bytes itself.
                bytes += 0x88 + VectorBytes(blocks) + VectorBytes(blocks + 0x54) + VectorBytes(blocks + 0x64) +
                         VectorBytes(blocks + 0x74) + BitPackedBytes(Read<const uint8_t*>(blocks, 0x48)) +
                         BitPackedBytes(Read<const uint8_t*>(blocks, 0x4C));
            }
            if (const uint8_t* hashes = Read<const uint8_t*>(rec, 0x1A8))
            {
                // HashEntryTable: a tag byte per slot (the count at 0x14), packed indices at 0x10.
                bytes += 0x38 + Read<uint32_t>(hashes, 0x14) + BitPackedBytes(Read<const uint8_t*>(hashes, 0x10));
            }
            bytes += static_cast<uint64_t>(Read<uint32_t>(rec, 0x204 + 8)) * 96u;
            out->bytes = bytes;
            return true;
        }

        // The client's Lua: lua_gc(L, LUA_GCCOUNT) in kilobytes and LUA_GCCOUNTB, the rest in bytes.
        using LuaGc = int32_t(__cdecl*)(void* L, int32_t what, int32_t data);
        constexpr int32_t kLuaGcCount  = 3;
        constexpr int32_t kLuaGcCountB = 4;
    }

    uint32_t gx_resources(wxl_core_gpu_fn fn, void* user)
    {
        if (!IsClient() || !fn) return 0;
        const uint8_t* device = static_cast<const uint8_t*>(GxDevice());
        if (!device) return 0;
        // EGxApi: 1 is Direct3D 9, 2 Direct3D 9Ex; the others hold no IDirect3D resources.
        const int32_t api = Read<int32_t>(device, WXL_FIELD("CGxDevice", "m_api"));
        if (api != 1 && api != 2) return 0;

        uint32_t told = 0;
        // The textures: each one created holds its IDirect3DBaseTexture9 in m_apiSpecificData; one
        // waiting to be created again holds a placeholder there and is skipped.
        WalkList(device + WXL_FIELD("CGxDevice", "m_textures"),
                 WXL_FIELD("CGxDevice", "m_textures.m_terminator.m_next") - WXL_FIELD("CGxDevice", "m_textures"),
                 [&](const uint8_t* texture) {
                     if (Read<int8_t>(texture, WXL_FIELD("CGxTex", "m_needsCreation"))) return;
                     auto* object = Read<IDirect3DBaseTexture9*>(texture, WXL_FIELD("CGxTex", "m_apiSpecificData"));
                     wxl_core_gpu_resource resource{};
                     if (object && wxl::core::gpu::DescribeTexture(object, &resource))
                     {
                         fn(&resource, user);
                         ++told;
                     }
                 });
        // The pools: each one holds its IDirect3DVertexBuffer9 or IndexBuffer9 in m_apiSpecific.
        WalkList(device + WXL_FIELD("CGxDevice", "m_poolList"),
                 WXL_FIELD("CGxDevice", "m_poolList.m_terminator.m_next") - WXL_FIELD("CGxDevice", "m_poolList"),
                 [&](const uint8_t* pool) {
                     auto* object = Read<IDirect3DResource9*>(pool, WXL_FIELD("CGxPool", "m_apiSpecific"));
                     wxl_core_gpu_resource resource{};
                     if (object && wxl::core::gpu::DescribeBuffer(object, &resource))
                     {
                         fn(&resource, user);
                         ++told;
                     }
                 });
        return told;
    }

    wxl_bool sound_heap(uint64_t* current, uint64_t* peak)
    {
        if (!IsClient() || !current || !peak) return WXL_FALSE;
        // FMOD::gGlobal, the sound engine's globals, as FMOD_Memory_allocC loads it: `mov eax,
        // [gGlobal]` (A1, then the address) at +0x11. Its gSystemPool at +4 is the MemPool every
        // FMOD allocation goes through, which counts what it holds at +0x1C and the most it held
        // at +0x120 (MemPool::alloc: `add [esi+0x1c], edx`, then `cmp eax, [esi+0x120]` and
        // `mov [esi+0x120], eax`, 0x008CFE9C to 0x008CFEB7).
        const uint8_t* load = reinterpret_cast<const uint8_t*>(WXL_SITE("FMOD_Memory_allocC", 0x11));
        if (load[0] != 0xA1) return WXL_FALSE;
        const uint8_t* global = Read<const uint8_t*>(Read<const void*>(load, 1), 0);
        const uint8_t* pool   = global ? Read<const uint8_t*>(global, 4) : nullptr;
        if (!pool) return WXL_FALSE;
        *current = Read<uint32_t>(pool, 0x1C);
        *peak    = Read<uint32_t>(pool, 0x120);
        return WXL_TRUE;
    }

    uint64_t script_heap()
    {
        void* L = IsClient() ? script_state() : nullptr;
        if (!L) return 0;
        const LuaGc gc = reinterpret_cast<LuaGc>(WXL_SITE("lua_gc", 0));
        return static_cast<uint64_t>(gc(L, kLuaGcCount, 0)) * 1024u + static_cast<uint64_t>(gc(L, kLuaGcCountB, 0));
    }

    uint32_t archive_tables(wxl_core_archive_fn fn, void* user)
    {
        if (!IsClient() || !fn) return 0;
        uint32_t told = 0;
        for (void* record : ArchiveRecords())
        {
            wxl_core_archive archive{};
            if (!DescribeArchive(static_cast<const uint8_t*>(record), &archive)) continue;
            fn(&archive, user);
            ++told;
        }
        return told;
    }

    wxl_bool archive_of(void* archive, wxl_core_archive* out)
    {
        // An SArchive: its type, 0 for an MPQ, then its ARCHIVEREC* (SFile2-Core.h).
        if (!IsClient() || !archive || !out || Read<uint32_t>(archive, 0) != 0) return WXL_FALSE;
        return DescribeArchive(Read<const uint8_t*>(archive, 4), out) ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool archive_record_of(void* record, wxl_core_archive* out)
    {
        if (!IsClient() || !record || !out) return WXL_FALSE;
        return DescribeArchive(static_cast<const uint8_t*>(record), out) ? WXL_TRUE : WXL_FALSE;
    }

    uint32_t client_dbs(wxl_core_db_fn fn, void* user)
    {
        if (!IsClient() || !fn) return 0;
        // Every store shares WowClientDB_Base's counts; a plain store has its records at 0x1C and
        // its index by id at 0x20, a compressed one its records (maybe compressed) at 0x18 and its
        // row pointers -- the index by id after them, in the same block -- at 0x1C.
        struct Row
        {
            uintptr_t   address;
            const char* file;
            uint32_t    row_size;
            bool        rle;
        };
        static const Row kRows[] = {
#define WXL_DB_RLE_plain false
#define WXL_DB_RLE_rle   true
#define WXL_CLIENT_DB(global, Rec, store, file, row_size) {WXL_STATIC(#global), file, row_size, WXL_DB_RLE_##store},
#include "wxl/core/client_dbs.def"
#undef WXL_CLIENT_DB
#undef WXL_DB_RLE_rle
#undef WXL_DB_RLE_plain
        };
        constexpr std::size_t kPlainRecords = WXL_FIELD("WowClientDB_SpellRec", "m_records");
        constexpr std::size_t kPlainIndex   = WXL_FIELD("WowClientDB_SpellRec", "m_recordsById");
        constexpr std::size_t kRleData      = WXL_FIELD("WowClientDB_RLE_SpellRec", "m_data");
        constexpr std::size_t kRleRows      = WXL_FIELD("WowClientDB_RLE_SpellRec", "m_records");

        uint32_t told = 0;
        for (const Row& row : kRows)
        {
            const void* store   = reinterpret_cast<const void*>(row.address);
            const int32_t count = Read<int32_t>(store, WXL_FIELD("WowClientDB_SpellRec", "m_numRecords"));
            const int32_t minId = Read<int32_t>(store, WXL_FIELD("WowClientDB_SpellRec", "m_minID"));
            const int32_t maxId = Read<int32_t>(store, WXL_FIELD("WowClientDB_SpellRec", "m_maxID"));
            const uint64_t ids  = count > 0 && maxId >= minId ? static_cast<uint64_t>(maxId - minId) + 1 : 0;

            wxl_core_db db{};
            db.file        = row.file;
            db.loaded      = Read<int32_t>(store, WXL_FIELD("WowClientDB_SpellRec", "m_loaded")) ? WXL_TRUE : WXL_FALSE;
            db.records     = count > 0 ? static_cast<uint32_t>(count) : 0u;
            db.record_size = row.row_size;
            db.blocks[0]   = Read<const void*>(store, row.rle ? kRleData : kPlainRecords);
            db.estimates[0] = static_cast<uint64_t>(db.records) * row.row_size;
            db.blocks[1]   = Read<const void*>(store, WXL_FIELD("WowClientDB_SpellRec", "m_strings"));
            db.blocks[2]   = Read<const void*>(store, row.rle ? kRleRows : kPlainIndex);
            db.estimates[2] = (row.rle ? db.records + ids : ids) * sizeof(void*);
            fn(&db, user);
            ++told;
        }
        return told;
    }
}

// --- Asset: models, map objects, doodads (asset.def) ----------------------------------------

namespace
{
    // The allocator of model data: 16-byte aligned blocks of Storm's heap (SMemAlloc).
    using AllocA16Fn = void*(__cdecl*)(uint32_t size, const char* file, int32_t line);
    using FreeA16Fn  = void(__cdecl*)(void* block);

    /// PumpBlpTextureAsync(texture, data) (0x004B7BD0): the texture in EAX (`mov esi, eax` at
    /// 0x004B7BE3), the data the one stack dword ([ebp+8]), popped by the caller, as
    /// FinalizeAsyncBlpTextureLoad calls it (0x004B7E8E: `mov eax, esi; call; add esp, 4`).
    /// Kept out of line: the block sets EAX, which a frame of the caller's must not hold.
    __declspec(noinline) int32_t CallPumpBlp(void* texture, const void* data)
    {
        const std::uintptr_t site   = WXL_SITE("PumpBlpTextureAsync", 0);
        int32_t              result = 0;
        __asm {
            mov  eax, dword ptr [texture]
            push dword ptr [data]
            call dword ptr [site]
            add  esp, 4
            mov  dword ptr [result], eax
        }
        return result;
    }
}

namespace wxl::core::binding
{
    const char* model_path(const void* shared)
    {
        if (!IsClient() || !shared) return nullptr;
        return static_cast<const char*>(shared) + WXL_FIELD("CM2Shared", "m_fileName");
    }

    const void* model_bytes(const void* shared, uint32_t* size)
    {
        if (!IsClient() || !shared) return nullptr;
        if (size) *size = Read<uint32_t>(shared, WXL_FIELD("CM2Shared", "m_fileSize"));
        return Read<const void*>(shared, WXL_FIELD("CM2Shared", "m_data"));
    }

    wxl_bool model_replace_bytes(void* shared, const void* bytes, uint32_t size)
    {
        if (!IsClient() || !shared || !bytes || !size) return WXL_FALSE;
        const AllocA16Fn alloc = reinterpret_cast<AllocA16Fn>(WXL_SITE("SIMD::AllocA16", 0));
        const FreeA16Fn  release = reinterpret_cast<FreeA16Fn>(WXL_SITE("SIMD::FreeA16", 0));
        void* copy = alloc(size, __FILE__, __LINE__);
        if (!copy) return WXL_FALSE;
        std::memcpy(copy, bytes, size);
        char* base = static_cast<char*>(shared);
        void* read = Read<void*>(shared, WXL_FIELD("CM2Shared", "m_data"));
        std::memcpy(base + WXL_FIELD("CM2Shared", "m_data"), &copy, sizeof copy);
        std::memcpy(base + WXL_FIELD("CM2Shared", "m_fileSize"), &size, sizeof size);
        if (read) release(read);
        return WXL_TRUE;
    }

    const void* map_area_bytes(const void* area, int32_t* x, int32_t* y, void** file, uint32_t* size)
    {
        if (!IsClient() || !area) return nullptr;
        if (x) *x = Read<int32_t>(area, WXL_FIELD("CMapArea", "m_index.x"));
        if (y) *y = Read<int32_t>(area, WXL_FIELD("CMapArea", "m_index.y"));
        if (file) *file = Read<void*>(area, WXL_FIELD("CMapArea", "m_file"));
        if (size) *size = Read<uint32_t>(area, WXL_FIELD("CMapArea", "m_fileSize"));
        return Read<const void*>(area, WXL_FIELD("CMapArea", "m_fileBuffer"));
    }

    wxl_bool file_name(void* file, char* buffer, uint32_t capacity)
    {
        if (!IsClient() || !file || !buffer || !capacity) return WXL_FALSE;
        using GetName = int32_t(__stdcall*)(void* file, char* buffer, uint32_t size);
        const GetName call = reinterpret_cast<GetName>(WXL_SITE("SFile::GetActualFileName", 0));
        buffer[0]          = 0;
        return call(file, buffer, capacity) ? WXL_TRUE : WXL_FALSE;
    }

    const void* map_obj_group_bytes(const void* group, void** parent, uint32_t* index, uint32_t* size)
    {
        if (!IsClient() || !group) return nullptr;
        void* root = Read<void*>(group, WXL_FIELD("CMapObjGroup", "m_parent"));
        if (parent) *parent = root;
        if (size) *size = Read<uint32_t>(group, WXL_FIELD("CMapObjGroup", "m_fileSize"));
        if (index)
        {
            *index = 0;
            const uint32_t count = root ? Read<uint32_t>(root, WXL_FIELD("CMapObj", "m_mapObjGroupCount")) : 0;
            for (uint32_t i = 0; i < count && i < 512; ++i)
                if (Read<const void*>(root, WXL_FIELD("CMapObj", "m_mapObjGroupArray") + i * 4) == group)
                {
                    *index = i;
                    break;
                }
        }
        return Read<const void*>(group, WXL_FIELD("CMapObjGroup", "m_filePtr"));
    }

    const void* model_skin_bytes(const void* shared)
    {
        if (!IsClient() || !shared) return nullptr;
        return Read<const void*>(shared, WXL_FIELD("CM2Shared", "m_skinData"));
    }

    const char* texture_name(const void* texture)
    {
        if (!IsClient() || !texture) return nullptr;
        return static_cast<const char*>(texture) + WXL_FIELD("CTexture", "m_filename");
    }

    uint32_t m2_cache_evict(const char* leaf, void** out, uint32_t capacity)
    {
        if (!IsClient() || !leaf || !*leaf) return 0;
        // CM2Cache's 1021 buckets at +0x10, each a CM2Shared chained through m_next (+0x148) and
        // m_previous (+0x144), the address of the link that points at it: CM2Cache::ClearSharedHash
        // (0x0081BF50: `lea edx, [ecx+0x10]`, `mov edi, 0x3fd`, [eax+0x144], [eax+0x148]).
        constexpr std::uintptr_t kCache    = WXL_STATIC_AT("s_cache", 0x00D3FCF0);
        constexpr std::size_t    kBuckets  = 0x10;
        constexpr uint32_t       kCount    = 0x3FD;
        constexpr std::size_t    kPrevious = 0x144;
        constexpr std::size_t    kNext     = 0x148;
        static_assert(WXL_FIELD("CM2Shared", "m_fileNameWithoutPath") == 0x140, "m_previous follows it");

        uint32_t evicted = 0;
        for (uint32_t i = 0; i < kCount; ++i)
        {
            uint8_t* shared = *reinterpret_cast<uint8_t**>(kCache + kBuckets + i * 4);
            while (shared)
            {
                uint8_t*    next = Read<uint8_t*>(shared, kNext);
                const char* name = Read<const char*>(shared, WXL_FIELD("CM2Shared", "m_fileNameWithoutPath"));
                if (name && _stricmp(name, leaf) == 0)
                {
                    // As ~CM2Shared unlinks it (0x0083D5B0).
                    uint8_t** previous = Read<uint8_t**>(shared, kPrevious);
                    if (previous) *previous = next;
                    if (next) std::memcpy(next + kPrevious, &previous, sizeof previous);
                    const void* none = nullptr;
                    std::memcpy(shared + kPrevious, &none, sizeof none);
                    std::memcpy(shared + kNext, &none, sizeof none);
                    if (out && evicted < capacity) out[evicted] = shared;
                    ++evicted;
                }
                shared = next;
            }
        }
        return evicted;
    }

    wxl_bool unit_update_display(void* object)
    {
        if (!IsClient() || !object) return WXL_FALSE;
        const uint32_t type = object_type_id(object);
        if (type != WXL_OBJECT_UNIT && type != WXL_OBJECT_PLAYER) return WXL_FALSE;
        // CGUnit_C::UpdateDisplayInfo(force): __thiscall, `ret 4`, reached through the unit's and
        // the player's vtables only (SMSG_FORCE_DISPLAY_UPDATE's `unit->UpdateDisplayInfo(1)`).
        x86::CallMember<void>(WXL_SITE("CGUnit_C::UpdateDisplayInfo", 0), object, int32_t(1));
        return WXL_TRUE;
    }

    uint32_t texture_reload(const char* name, uint32_t* busy)
    {
        if (busy) *busy = 0;
        if (!IsClient() || !name || !*name) return 0;
        // s_textureList, a TSExplicitList<CTexture>: its terminator's m_next at +8, each texture's
        // TSLink m_link at +0x64, its m_next at +4; an end pointer has its low bit set (TSList::IsEnd
        // 0x00407900). The CTexture constructor links each one there (0x004B880D). m_flags +0x28
        // (0x004B87B2), m_asyncObject +0x40, m_atlas +0x5C (0x004B87E4, 0x004B8800).
        constexpr std::uintptr_t kList        = WXL_STATIC_AT("s_textureList", 0x00AC3348);
        constexpr std::size_t    kLink        = 0x64;
        constexpr std::size_t    kFlags       = 0x28;
        constexpr std::size_t    kAsyncObject = 0x40;
        constexpr std::size_t    kAtlas       = 0x5C;
        static_assert(WXL_FIELD("CTexture", "m_filename") == kLink + 8, "m_filename follows m_link");

        char path[MAX_PATH];
        if (std::snprintf(path, sizeof path, "%s.blp", name) >= static_cast<int>(sizeof path)) return 0;

        std::vector<uint8_t> data;
        bool                 tried    = false;
        uint32_t             reloaded = 0;
        const auto           end      = [](const uint8_t* node) { return !node || (reinterpret_cast<std::uintptr_t>(node) & 1); };
        for (uint8_t* texture = *reinterpret_cast<uint8_t**>(kList + 8); !end(texture);
             texture          = Read<uint8_t*>(texture, kLink + 4))
        {
            const char* own = reinterpret_cast<const char*>(texture + WXL_FIELD("CTexture", "m_filename"));
            if (_stricmp(own, name) != 0 && _stricmp(own, path) != 0) continue;
            if (Read<const void*>(texture, kAsyncObject) || Read<const void*>(texture, kAtlas) ||
                (Read<uint16_t>(texture, kFlags) & 4))
            {
                if (busy) ++*busy;
                continue;
            }
            if (!tried)
            {
                tried      = true;
                void* file = file_open(path);
                if (file)
                {
                    data.resize(static_cast<std::size_t>(file_size(file)));
                    const uint32_t got = data.empty() ? 0 : file_read(file, data.data(), static_cast<uint32_t>(data.size()));
                    file_close(file);
                    if (got != data.size()) data.clear();
                }
            }
            if (data.empty()) break;
            if (CallPumpBlp(texture, data.data())) ++reloaded;
        }
        return reloaded;
    }

    const char* map_obj_path(const void* map_obj)
    {
        if (!IsClient() || !map_obj) return nullptr;
        return static_cast<const char*>(map_obj) + WXL_FIELD("CMapObj", "m_wmoName");
    }

    const void* map_obj_bytes(const void* map_obj, uint32_t* size)
    {
        if (!IsClient() || !map_obj) return nullptr;
        if (size) *size = Read<uint32_t>(map_obj, WXL_FIELD("CMapObj", "m_wmoFileSize"));
        return Read<const void*>(map_obj, WXL_FIELD("CMapObj", "m_pWmoData"));
    }

    wxl_bool doodad_position(const void* def, wxl_vec3* out)
    {
        if (!IsClient() || !def || !out) return WXL_FALSE;
        out->x = Read<float>(def, WXL_FIELD("CMapDoodadDef", "m_position.x"));
        out->y = Read<float>(def, WXL_FIELD("CMapDoodadDef", "m_position.y"));
        out->z = Read<float>(def, WXL_FIELD("CMapDoodadDef", "m_position.z"));
        return WXL_TRUE;
    }
}

// --- Gameplay: auras, loot, reputation, the dialogs' answers (gameplay.def) --------------------

namespace
{
    // The client functions the answers call, as the binary declares them.
    using AcceptGroupFn     = void(__stdcall*)(uint32_t roles);
    using DeclineGroupFn    = void(__stdcall*)();
    using AcceptResurrectFn = void(__stdcall*)(int32_t accept);
    using DuelReplyFn       = void(__cdecl*)();
    using AsyncTimeFn       = uint32_t(__cdecl*)();

    template <class T>
    T& At(std::uintptr_t address)
    {
        return *reinterpret_cast<T*>(address);
    }
}

namespace wxl::core::binding
{
    int32_t unit_aura_slot(const void* unit, const void* aura)
    {
        if (!IsClient() || !unit || !aura) return -1;
        // TSCGrowableArray<AuraData, 16>: sixteen slots inline, or, once m_count reads
        // 0xFFFFFFFF, a TSGrowableArray in the same bytes whose m_data holds them all (the test
        // OnAuraUpdate makes at 0x0072FAEC).
        const char*    slots = static_cast<const char*>(unit) + WXL_FIELD("CGUnit_C", "m_auras");
        const uint32_t count = Read<uint32_t>(unit, WXL_FIELD("CGUnit_C", "m_auras.m_count"));
        uint32_t       total = 16;
        if (count == 0xFFFFFFFFu)
        {
            slots = Read<const char*>(unit, WXL_FIELD("CGUnit_C", "m_auras.m_dynamic.m_data"));
            total = Read<uint32_t>(unit, WXL_FIELD("CGUnit_C", "m_auras.m_dynamic.m_data") - 4);
        }
        else
            total = count < 16 ? count : 16;
        if (!slots) return -1;
        const std::ptrdiff_t at     = static_cast<const char*>(aura) - slots;
        const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(WXL_TYPE_SIZE("AuraData"));
        if (at < 0 || at % stride != 0 || at / stride >= static_cast<std::ptrdiff_t>(total)) return -1;
        return static_cast<int32_t>(at / stride);
    }

    uint32_t os_time_ms(void)
    {
        if (!IsClient()) return 0;
        return reinterpret_cast<AsyncTimeFn>(WXL_SITE("OsGetAsyncTimeMs", 0))();
    }

    uint64_t loot_object_guid(void)
    {
        return IsClient() ? At<const uint64_t>(WXL_STATIC("CGLootInfo::m_lootObj")) : 0;
    }

    uint32_t loot_kind(void)
    {
        return IsClient() ? At<const uint32_t>(WXL_STATIC("CGLootInfo::m_lootAcquire")) : 0;
    }

    uint32_t loot_money(void)
    {
        return IsClient() ? At<const uint32_t>(WXL_STATIC("CGLootInfo::m_coins")) : 0;
    }

    wxl_bool loot_slot(uint32_t index, int32_t* out)
    {
        if (!IsClient() || !out || index >= 18) return WXL_FALSE;
        const char* slot = reinterpret_cast<const char*>(WXL_STATIC("CGLootInfo::m_slots")) +
                           index * WXL_TYPE_SIZE("LootSlot");
        const int32_t item = Read<int32_t>(slot, WXL_FIELD("LootSlot", "m_itemID"));
        if (!item) return WXL_FALSE;
        out[0] = item;
        out[1] = Read<int32_t>(slot, WXL_FIELD("LootSlot", "m_count"));
        out[2] = Read<int32_t>(slot, WXL_FIELD("LootSlot", "m_displayId"));
        out[3] = Read<int32_t>(slot, WXL_FIELD("LootSlot", "m_randomSuffix"));
        out[4] = Read<int32_t>(slot, WXL_FIELD("LootSlot", "m_randomPropertyId"));
        out[5] = Read<uint8_t>(slot, WXL_FIELD("LootSlot", "m_slot"));
        out[6] = Read<int32_t>(slot, WXL_FIELD("LootSlot", "m_slotType"));
        return WXL_TRUE;
    }

    uint32_t loot_next_roll_id(void)
    {
        return IsClient() ? At<const uint32_t>(WXL_STATIC("s_nextLootRollId")) : 0;
    }

    int32_t ui_map_id(void)
    {
        return IsClient() ? At<const int32_t>(WXL_STATIC("CGGameUI::m_iCurrentMapID")) : -1;
    }

    wxl_bool faction_standing(uint32_t list_id, int32_t* faction_id, int32_t* base, int32_t* standing)
    {
        if (!IsClient() || list_id >= 128) return WXL_FALSE;
        const char* entry = reinterpret_cast<const char*>(WXL_STATIC("CGReputationInfo::m_factionStandings")) +
                            list_id * WXL_TYPE_SIZE("FactionStanding");
        if (faction_id) *faction_id = Read<int32_t>(entry, WXL_FIELD("FactionStanding", "m_factionId"));
        if (base) *base = Read<int32_t>(entry, WXL_FIELD("FactionStanding", "m_base"));
        if (standing) *standing = Read<int32_t>(entry, WXL_FIELD("FactionStanding", "m_standing"));
        return WXL_TRUE;
    }

    uint64_t ui_interact_target(void)
    {
        return IsClient() ? At<const uint64_t>(WXL_STATIC("CGGameUI::m_interactTarget")) : 0;
    }

    uint64_t trade_partner(void)
    {
        return IsClient() ? At<const uint64_t>(WXL_STATIC("CGTradeInfo::m_tradingPlayer")) : 0;
    }

    void group_invite_answer(wxl_bool accept)
    {
        if (!IsClient()) return;
        // CGPlayer_C::AcceptGroup ends in `ret 4` (0x006D44BE), DeclineGroup in a bare `ret`
        // (0x006D4532): both __stdcall in the client's own source.
        if (accept) reinterpret_cast<AcceptGroupFn>(WXL_SITE("CGPlayer_C::AcceptGroup", 0))(0);
        else reinterpret_cast<DeclineGroupFn>(WXL_SITE("CGPlayer_C::DeclineGroup", 0))();
    }

    void resurrect_answer(uint64_t offerer, uint32_t sickness, uint32_t use_timer, wxl_bool accept)
    {
        if (!IsClient()) return;
        At<uint64_t>(WXL_STATIC("CGPlayer_C::s_resurrectOfferer"))       = offerer;
        At<uint32_t>(WXL_STATIC("CGPlayer_C::s_resurrectOfferSickness")) = sickness;
        At<uint32_t>(WXL_STATIC("CGPlayer_C::s_resurrectOfferUseTimer")) = use_timer;
        // `ret 4` at 0x006D1DD4: __stdcall, the answer as its one argument.
        reinterpret_cast<AcceptResurrectFn>(WXL_SITE("CGPlayer_C::AcceptResurrectRequest", 0))(accept ? 1 : 0);
    }

    void duel_answer(uint64_t arbiter, uint64_t challenger, wxl_bool accept)
    {
        if (!IsClient()) return;
        // AcceptDuel and CancelDuel read the arbiter at 0x00C22B58 (0x005CFC05, 0x005CFC85) and
        // end in a bare `ret`: static members taking nothing.
        At<uint64_t>(WXL_STATIC("s_duelArbiter")) = arbiter;
        if (accept)
        {
            reinterpret_cast<DuelReplyFn>(WXL_SITE("CGDuelInfo::AcceptDuel", 0))();
            At<uint64_t>(WXL_STATIC("CGDuelInfo::m_dueler")) = challenger;
            return;
        }
        reinterpret_cast<DuelReplyFn>(WXL_SITE("CGDuelInfo::CancelDuel", 0))();
        At<uint64_t>(WXL_STATIC("s_duelArbiter")) = 0;
    }
}
