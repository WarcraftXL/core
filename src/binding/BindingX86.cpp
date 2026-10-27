// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The x86 realisation of the client binding: memory reads at measured offsets, calls at resolved addresses.

#include "binding/Binding.hpp"
#include "binding/BindingX86.hpp"
#include "gx/Vulkan.hpp"
#include "binding/CallX86.hpp"
#include "binding/HighlightDrawX86.hpp"
#include "binding/ModelDrawX86.hpp"

#include "common/Log.hpp"
#include "runtime/Core.hpp"
#include "runtime/Image.hpp"

#include "wxl/events.h"
#include "wxl/core/camera_facing.hpp"
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
#include <unordered_map>
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
    using LuaFClose        = void(__cdecl*)(void* L, void* level);
    using LuaDReallocCI    = void(__cdecl*)(void* L, int32_t size);

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
    LuaFClose        LuaFCloseAt() { return reinterpret_cast<LuaFClose>(WXL_SITE("luaF_close", 0)); }
    LuaDReallocCI    LuaDReallocCIAt() { return reinterpret_cast<LuaDReallocCI>(WXL_SITE("luaD_reallocCI", 0)); }

    // --- A mark of the script state: what luaD_pcall keeps before a protected call (ldo.c) -----

    /// The words of a wxl_script_mark on this side.
    enum MarkWord : uint32_t
    {
        kMarkTop,             ///< L->top, as an offset from L->stack
        kMarkCall,            ///< L->ci, as an offset from L->base_ci
        kMarkCCalls,          ///< L->nCcalls
        kMarkAllowHook,       ///< L->allowhook
        kMarkErrFunc,         ///< L->errfunc
        kMarkErrorJmp,        ///< L->errorJmp
        kMarkTainted,         ///< lua_tainted
        kMarkTaintExpected,   ///< lua_taintexpected
        kMarkTaintedClosure,  ///< lua_taintedclosure
    };

    constexpr int32_t kLuaTFunction = 6;     // LUA_TFUNCTION
    constexpr int32_t kLuaMaxCalls  = 20000; // LUAI_MAXCALLS, the call array's limit

    /// A member of the client's state, by reference.
    template <typename T>
    T& Slot(void* base, std::size_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
    }

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

    // wxl.highlight keeps a batch's draw to draw it again: the device, the engine's copy of the
    // vertex constants (CGxDevice::s_shadowConstants[1], after the pixel program's [0]) and
    // whether the client draws with shaders at all (HighlightDrawX86.hpp).
    gx::d3d9::highlight::Source HighlightSource()
    {
        gx::d3d9::highlight::Source source;
        source.device = [] { return GxDevice(); };
        source.vertexShadow =
            reinterpret_cast<uint8_t*>(WXL_STATIC("CGxDevice::s_shadowConstants")) + x86::highlight_draw::kShadowSize;
        source.enableShaders = reinterpret_cast<const int32_t*>(WXL_STATIC("CShaderEffect::s_enableShaders"));
        return source;
    }
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

    void script_mark(void* state, wxl_script_mark* mark)
    {
        if (!mark) return;
        *mark = wxl_script_mark{};
        if (!IsClient() || !state) return;
        uint8_t* const stack = Slot<uint8_t*>(state, WXL_FIELD("lua_State", "stack"));
        uint8_t* const calls = Slot<uint8_t*>(state, WXL_FIELD("lua_State", "base_ci"));
        mark->words[kMarkTop]       = static_cast<uintptr_t>(Slot<uint8_t*>(state, WXL_FIELD("lua_State", "top")) - stack);
        mark->words[kMarkCall]      = static_cast<uintptr_t>(Slot<uint8_t*>(state, WXL_FIELD("lua_State", "ci")) - calls);
        mark->words[kMarkCCalls]    = Slot<uint16_t>(state, WXL_FIELD("lua_State", "nCcalls"));
        mark->words[kMarkAllowHook] = Slot<uint8_t>(state, WXL_FIELD("lua_State", "allowhook"));
        mark->words[kMarkErrFunc]   = static_cast<uintptr_t>(Slot<int32_t>(state, WXL_FIELD("lua_State", "errfunc")));
        mark->words[kMarkErrorJmp]  = reinterpret_cast<uintptr_t>(Slot<void*>(state, WXL_FIELD("lua_State", "errorJmp")));
        mark->words[kMarkTainted]   = *reinterpret_cast<const uintptr_t*>(WXL_STATIC("lua_tainted"));
        mark->words[kMarkTaintExpected]  = static_cast<uintptr_t>(*reinterpret_cast<const int32_t*>(WXL_STATIC("lua_taintexpected")));
        mark->words[kMarkTaintedClosure] = static_cast<uintptr_t>(*reinterpret_cast<const int32_t*>(WXL_STATIC("lua_taintedclosure")));
    }

    wxl_bool script_unwind(void* state, const wxl_script_mark* mark)
    {
        if (!IsClient() || !state || !mark) return WXL_FALSE;
        constexpr std::size_t kCall = WXL_TYPE_SIZE("CallInfo");
        uint8_t* const calls = Slot<uint8_t*>(state, WXL_FIELD("lua_State", "base_ci"));
        uint8_t* const call  = calls + mark->words[kMarkCall];
        uint8_t*       ci    = Slot<uint8_t*>(state, WXL_FIELD("lua_State", "ci"));
        // A protected call still open shows in its call, its C-call depth or its recovery point
        // (the parser's runs at the caller's call level).
        if (ci == call && Slot<uint16_t>(state, WXL_FIELD("lua_State", "nCcalls")) == mark->words[kMarkCCalls] &&
            reinterpret_cast<uintptr_t>(Slot<void*>(state, WXL_FIELD("lua_State", "errorJmp"))) == mark->words[kMarkErrorJmp])
            return WXL_FALSE;

        // A profiled interface counts how deep each Lua function runs; the calls dropped leave it.
        void* const global = Slot<void*>(state, WXL_FIELD("lua_State", "l_G"));
        if (Slot<uint8_t>(global, WXL_FIELD("global_State", "profile")))
            while (ci > call + kCall)
            {
                ci -= kCall;
                void* const func = Slot<void*>(ci, WXL_FIELD("CallInfo", "func"));
                if (Slot<int32_t>(func, WXL_FIELD("TValue", "tt")) != kLuaTFunction) continue;
                void* const closure = Slot<void*>(func, WXL_FIELD("TValue", "value"));
                if (Slot<uint8_t>(closure, WXL_FIELD("LClosure", "isC"))) continue;
                Slot<int32_t>(Slot<void*>(closure, WXL_FIELD("LClosure", "profile")), WXL_FIELD("Profile", "depth")) -= 1;
            }

        uint8_t* const top = Slot<uint8_t*>(state, WXL_FIELD("lua_State", "stack")) + mark->words[kMarkTop];
        LuaFCloseAt()(state, top);
        Slot<uint8_t*>(state, WXL_FIELD("lua_State", "top"))      = top;
        Slot<uint16_t>(state, WXL_FIELD("lua_State", "nCcalls"))  = static_cast<uint16_t>(mark->words[kMarkCCalls]);
        Slot<uint8_t*>(state, WXL_FIELD("lua_State", "ci"))       = call;
        Slot<void*>(state, WXL_FIELD("lua_State", "base"))        = Slot<void*>(call, WXL_FIELD("CallInfo", "base"));
        Slot<void*>(state, WXL_FIELD("lua_State", "savedpc"))     = Slot<void*>(call, WXL_FIELD("CallInfo", "savedpc"));
        Slot<uint8_t>(state, WXL_FIELD("lua_State", "allowhook")) = static_cast<uint8_t>(mark->words[kMarkAllowHook]);
        Slot<int32_t>(state, WXL_FIELD("lua_State", "errfunc"))   = static_cast<int32_t>(mark->words[kMarkErrFunc]);
        Slot<void*>(state, WXL_FIELD("lua_State", "errorJmp"))    = reinterpret_cast<void*>(mark->words[kMarkErrorJmp]);
        // restore_stack_limit: a call array grown past its limit by an overflow goes back to it.
        if (Slot<int32_t>(state, WXL_FIELD("lua_State", "size_ci")) > kLuaMaxCalls &&
            static_cast<int32_t>((call - calls) / kCall) + 1 < kLuaMaxCalls)
            LuaDReallocCIAt()(state, kLuaMaxCalls);

        *reinterpret_cast<uintptr_t*>(WXL_STATIC("lua_tainted"))      = mark->words[kMarkTainted];
        *reinterpret_cast<int32_t*>(WXL_STATIC("lua_taintexpected"))  = static_cast<int32_t>(mark->words[kMarkTaintExpected]);
        *reinterpret_cast<int32_t*>(WXL_STATIC("lua_taintedclosure")) = static_cast<int32_t>(mark->words[kMarkTaintedClosure]);
        return WXL_TRUE;
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

    int32_t gx_device_api()
    {
        const void* device = IsClient() ? GxDevice() : nullptr;
        return device ? static_cast<int32_t>(Read<uint32_t>(device, WXL_FIELD("CGxDevice", "m_api"))) : -1;
    }

    wxl_bool gx_world_view(wxl_core_world_view* out)
    {
        const void* device = IsClient() ? GxDevice() : nullptr;
        if (!device || !out) return WXL_FALSE;
        constexpr std::size_t kViewStack = 10; // GxXform_View
        const char*           bytes      = static_cast<const char*>(device);
        const std::size_t     stack      = WXL_FIELD("CGxDevice", "m_xforms") + kViewStack * WXL_TYPE_SIZE("CGxMatrixStack");
        uint32_t              level      = Read<uint32_t>(device, stack + WXL_FIELD("CGxMatrixStack", "m_level"));
        if (level > 3) level = 3;
        std::memcpy(out->projection, bytes + WXL_FIELD("CGxDevice", "m_projNative"), sizeof out->projection);
        std::memcpy(out->view, bytes + stack + WXL_FIELD("CGxMatrixStack", "m_mtx") + level * sizeof out->view, sizeof out->view);
        std::memcpy(out->viewport, bytes + WXL_FIELD("CGxDevice", "m_viewport.m_x.m_l"), sizeof out->viewport);
        out->window[0] = Read<float>(device, WXL_FIELD("CGxDevice", "m_curWindowRect.maxX"));
        out->window[1] = Read<float>(device, WXL_FIELD("CGxDevice", "m_curWindowRect.maxY"));
        out->eye       = *reinterpret_cast<const wxl_vec3*>(WXL_STATIC("CWorldScene::s_activeWorldView"));
        return WXL_TRUE;
    }

    void* gx_texture_ready(void* texture)
    {
        void* device = IsClient() && OnMainThread() && texture ? const_cast<void*>(GxDevice()) : nullptr;
        if (!device) return nullptr;
        void* gx = Read<void*>(texture, WXL_FIELD("CTexture", "m_gxTex"));
        if (!gx) return nullptr; // the file is still loading
        // CGxDevice::ITexMarkAsUpdated(CGxTex*) is the device vtable's slot 0, which every device
        // overrides (CGxDeviceD3d at 0x006A3070, the Vulkan adapter's copy of the table): it makes
        // the texture on the device and fills it, as the engine does before binding it.
        using MarkFn                 = void(__fastcall*)(void* self, void* edx, void* tex);
        const void* const* vtable    = Read<const void* const*>(device, 0);
        reinterpret_cast<MarkFn>(const_cast<void*>(vtable[0]))(device, nullptr, gx);
        if (Read<int8_t>(gx, WXL_FIELD("CGxTex", "m_needsCreation"))) return nullptr;
        return Read<void*>(gx, WXL_FIELD("CGxTex", "m_apiSpecificData")) ? gx : nullptr;
    }

    void* gx_d3d9_device()
    {
        // Under Vulkan the device keeps D3D9's EGxApi (so the engine takes D3D9's paths), but no
        // IDirect3DDevice9 behind it.
        const void* device = IsClient() && !gx::VulkanActive() ? GxDevice() : nullptr;
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
        template <std::size_t Decl, std::size_t Index, std::size_t Streams, std::size_t StreamsSize, std::size_t States,
                  std::size_t StatesSize>
        void SpoilD3d(void* device)
        {
            uint8_t* bytes = static_cast<uint8_t*>(device);
            std::memset(bytes + States, 0xA5, StatesSize);
            std::memset(bytes + Decl, 0, sizeof(void*));
            std::memset(bytes + Streams, 0, StreamsSize);
            // The index buffer is bound again only when the engine marks its own as changed
            // (CGxDeviceD3d::IStateSync tests m_primIndexDirty first): both, or a draw that keeps
            // the engine's buffer would draw with the one set behind its back.
            std::memset(bytes + Index, 0, sizeof(void*));
            const int32_t dirty = 1;
            std::memcpy(bytes + WXL_FIELD("CGxDevice", "m_primIndexDirty"), &dirty, sizeof dirty);
            std::memcpy(bytes + WXL_FIELD("CGxDevice", "m_viewportDirty"), &dirty, sizeof dirty);
            std::memcpy(bytes + WXL_FIELD("CGxDevice", "m_scissorRectDirty"), &dirty, sizeof dirty);
        }
    }

    void gx_invalidate_state()
    {
        void* device = IsClient() && !gx::VulkanActive() ? const_cast<void*>(GxDevice()) : nullptr;
        if (!device) return;
        const uint32_t api = Read<uint32_t>(device, WXL_FIELD("CGxDevice", "m_api"));
        if (api != 1 && api != 2) return;
        // CGxDevice::IRsForceUpdate(), the overload taking nothing: __thiscall, `ret` (0x00685B4D).
        x86::CallMember<void>(WXL_SITE_AT("CGxDevice::IRsForceUpdate", 0x00685A70, 0), device);
        if (api == 1)
            SpoilD3d<WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentVertexDecl"), WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentIndexBuf"),
                     WXL_FIELD("CGxDeviceD3d", "m_d3dVertexStreamBuf"),
                     WXL_MEMBER_SIZE("CGxDeviceD3d", "m_d3dVertexStreamBuf"), WXL_FIELD("CGxDeviceD3d", "m_deviceStates"),
                     WXL_MEMBER_SIZE("CGxDeviceD3d", "m_deviceStates")>(device);
        else
            SpoilD3d<WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dCurrentVertexDecl"), WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dCurrentIndexBuf"),
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

    void* circle_swap_texture(void* texture)
    {
        if (!IsClient() || !OnMainThread()) return nullptr;
        void** slot = reinterpret_cast<void**>(WXL_STATIC("CGObject_C::s_selectionTexture"));
        void*  held = *slot;
        *slot       = texture;
        return held;
    }

    void* texture_open(const char* path)
    {
        if (!IsClient() || !OnMainThread() || !path || !*path) return nullptr;
        // CreateTexture(fileName, status): cdecl, two dwords (TextureCreate with clamped
        // CGxTexFlags(GxTex_LinearMipNearest), 0x004E8D30); the client's global status takes
        // what the load reports later (GetGlobalStatusObj, cdecl, nothing).
        void* status = x86::CallFree<void*>(WXL_SITE("GetGlobalStatusObj", 0));
        return x86::CallFree<void*>(WXL_SITE("CreateTexture", 0), path, status);
    }

    void texture_close(void* texture)
    {
        // HandleClose(handle): cdecl, one dword (0x0047BF30).
        if (IsClient() && OnMainThread() && texture) x86::CallFree<void>(WXL_SITE("HandleClose", 0), texture);
    }

    void* cvar_lookup(const char* name)
    {
        // CVar::Lookup(name): cdecl, one dword (0x00767440); the hash compares without case.
        if (!IsClient() || !OnMainThread() || !name) return nullptr;
        return x86::CallFree<void*>(WXL_SITE("CVar::Lookup", 0), name);
    }

    void* cvar_register(const char* name, const char* help, uint32_t flags, const char* value, void* callback,
                        uint32_t category)
    {
        // CVar::Register(name, help, flags, value, callback, category, setCommand, arg, unk):
        // cdecl, nine dwords (0x00767FC0), as the client's code registers its own variables.
        if (!IsClient() || !OnMainThread() || !name || !value) return nullptr;
        return x86::CallFree<void*>(WXL_SITE("CVar::Register", 0), name, help, flags, value, callback, category,
                                    uint8_t(0), static_cast<void*>(nullptr), uint8_t(0));
    }

    wxl_bool cvar_set(void* cvar, const char* value)
    {
        // CVar::Set(value, setValue, setReset, setDefault, setNeedsSave): __thiscall, `ret 0x14`
        // (0x007668C0); always answers true, whether the callback took the value or not.
        if (!IsClient() || !OnMainThread() || !cvar || !value) return WXL_FALSE;
        x86::CallMember<bool>(WXL_SITE_AT("CVar::Set", 0x007668C0, 0), cvar, value, uint8_t(1), uint8_t(0), uint8_t(0),
                              uint8_t(1));
        return WXL_TRUE;
    }

    void cvar_write_flags(void* cvar, uint32_t flags)
    {
        if (!IsClient() || !OnMainThread() || !cvar) return;
        std::memcpy(static_cast<uint8_t*>(cvar) + WXL_FIELD("CVar", "m_flags"), &flags, sizeof flags);
    }

    // --- interface.def: an interface's signature ------------------------------------------

    int32_t interface_check_signature(const char* toc, const char* extra, const uint8_t* modulus, uint8_t* digest)
    {
        // FrameXML_CheckSignature(toc, extra, modulus, digest): cdecl, four dwords (0x008165E0).
        // Its detour is the FrameXML.CheckSignature gate, which lets this call through.
        if (!IsClient() || !OnMainThread() || !toc || !digest) return -1;
        return x86::CallFree<int32_t>(WXL_SITE("FrameXML_CheckSignature", 0), toc, extra, modulus, digest);
    }

    wxl_bool interface_signature_digest(const char* toc, const char* extra, uint8_t* digest)
    {
        // CalculateSignatureMD5 (0x008164D0) takes `extra` in ESI; its steps are made here
        // instead, by the same calls: MD5Init, ProcessFileForMakeSign(ctx, toc, ""), the bytes of
        // `extra` read whole through the client's files, MD5Final(digest, ctx).
        if (!IsClient() || !OnMainThread() || !toc || !digest) return WXL_FALSE;
        alignas(8) uint8_t context[WXL_TYPE_SIZE("MD5_CTX")];
        x86::CallFree<void>(WXL_SITE("MD5Init", 0), static_cast<void*>(context));
        x86::CallFree<int32_t>(WXL_SITE("ProcessFileForMakeSign", 0), static_cast<void*>(context), toc, "");
        if (extra && *extra)
        {
            if (void* file = file_open(extra))
            {
                std::vector<uint8_t> bytes(static_cast<std::size_t>(file_size(file)));
                const uint32_t got = bytes.empty() ? 0 : file_read(file, bytes.data(), static_cast<uint32_t>(bytes.size()));
                file_close(file);
                if (got && got == bytes.size())
                    x86::CallFree<void>(WXL_SITE("MD5Update", 0), static_cast<void*>(context),
                                        static_cast<const uint8_t*>(bytes.data()), got);
            }
        }
        x86::CallFree<void>(WXL_SITE("MD5Final", 0), digest, static_cast<void*>(context));
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

    // --- M2: the scene's animate list (the animate gates) ---------------------------------------

    wxl_bool m2_is_animate_thread(const void* function)
    {
        return IsClient() && function && reinterpret_cast<std::uintptr_t>(function) == WXL_SITE("CM2Scene::AnimateThread", 0)
                   ? WXL_TRUE
                   : WXL_FALSE;
    }

    uint32_t m2_animate_list(const void* scene, void** out, uint32_t capacity)
    {
        if (!IsClient() || !scene) return 0;
        // A list the client keeps acyclic; the bound only stops a walk through a broken one.
        constexpr uint32_t kMost = 1u << 20;
        uint32_t           count = 0;
        for (void* model = Read<void*>(scene, WXL_FIELD("CM2Scene", "m_animateList")); model && count < kMost;
             model       = Read<void*>(model, WXL_FIELD("CM2Model", "m_animateNext")))
        {
            if (out && count < capacity) out[count] = model;
            ++count;
        }
        return count;
    }

    void* m2_swap_animate_list(void* scene, void* head)
    {
        if (!IsClient() || !scene) return nullptr;
        void** slot = reinterpret_cast<void**>(static_cast<uint8_t*>(scene) + WXL_FIELD("CM2Scene", "m_animateList"));
        void*  was  = *slot;
        *slot       = head;
        return was;
    }

    void m2_animate_model(void* model, void* scene)
    {
        if (!IsClient() || !model || !scene) return;
        // As CM2Scene::Animate and AnimateThread call them (0x00821B1E, 0x00821B4E): __thiscall,
        // (view, &diffuse, &emissive, alpha, alpha2), `ret 0x14`; diffuse (1,1,1), emissive
        // (0,0,0), both alphas 1.
        constexpr uint32_t kSimple    = 0x1000;
        const float        diffuse[3] = {1.0f, 1.0f, 1.0f};
        const float        emissive[3] = {0.0f, 0.0f, 0.0f};
        const void*        view        = static_cast<const uint8_t*>(scene) + WXL_FIELD("CM2Scene", "m_view");
        const bool         simple      = (Read<uint32_t>(model, WXL_FIELD("CM2Model", "m_fFlags")) & kSimple) != 0;
        x86::CallMember<void>(simple ? WXL_SITE("CM2Model::AnimateMTSimple", 0) : WXL_SITE("CM2Model::AnimateMT", 0), model, view,
                              static_cast<const void*>(diffuse), static_cast<const void*>(emissive), 1.0f, 1.0f);
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

    // --- M2: models of the world (wxl.m2's world calls) -----------------------------------------
    //
    // As the client places its spell effects (CEffect::PostInitialize, CGWorldFrame::
    // SpellEffectEnumProc): the model's world matrix set by hand, a static map entity that culls
    // it, and an enumeration callback the map calls while it draws, which animates the model and
    // shows it while the map does.

    // Every function the calls below reach, as calls_x86.def declares them for check_conventions.
#define WXL_BINDING_CALL_X86(domain, name, symbol, ret, conv, params) \
    static_assert(WXL_SEAM_SITE(symbol, 0) != 0, #domain "." #name " resolves in Wow.exe");
#include "wxl/core/binding/calls_x86.def"
#undef WXL_BINDING_CALL_X86

    namespace
    {
        constexpr uint32_t kWorldMatrix = 0x8000;  // m_fFlags: m_worldTransform set by hand (SetWorldTransform)
        constexpr uint32_t kProjected   = 0x4;     // m_fFlags: projected batches lie on the terrain
        constexpr uint32_t kShown       = 0x8;     // m_fFlags: drawn, for a model hung on nothing
        constexpr uint32_t kAnimShown   = 0x10000; // m_fFlags: animated while drawn, likewise
        constexpr std::size_t kMatrixSize = 16 * sizeof(float);
        constexpr std::size_t kFileNameSize = 260; // CM2Shared::m_fileName

        struct PendingAnim
        {
            uint32_t anim;
            float    speed;
        };

        // Main thread only, as wxl.m2 calls it and the client calls back.
        std::unordered_map<void*, PendingAnim> g_pendingAnims;
        std::unordered_map<void*, void*>       g_worldEntities;

        void PlayNow(void* model, uint32_t anim, float speed)
        {
            // CM2Model::HasSequence(id): __thiscall, `ret 4` (0x00825F36). The stand animation
            // when the file has none of that id, as the client falls back.
            if (!x86::CallMember<int32_t>(WXL_SITE_AT("CM2Model::HasSequence", 0x00825EE0, 0), model, anim)) anim = 0;
            // CM2Model::SetBoneSequence(bone, sequence, variation, time, speed, blend, primary):
            // __thiscall, `ret 0x1c` (0x00832ADA); bone 0xFFFFFFFF is every bone.
            x86::CallMember<void>(WXL_SITE("CM2Model::SetBoneSequence", 0), model, 0xFFFFFFFFu, anim, 0xFFFFFFFFu,
                                  0u, speed, int32_t(1), int32_t(1));
        }

        /// The model's loaded callback: CM2Model::UpdateLoaded calls it cdecl (model, arg) and
        /// pops both (0x0082519A).
        void __cdecl OnWorldModelLoaded(void* model, void*)
        {
            const auto it   = g_pendingAnims.find(model);
            PendingAnim anim{0, 1.0f};
            if (it != g_pendingAnims.end())
            {
                anim = it->second;
                g_pendingAnims.erase(it);
            }
            PlayNow(model, anim.anim, anim.speed);
        }

        /// What SpellEffectEnumProc does for its effect: a MAP_OBJECT_FUNC, cdecl (param, flags,
        /// guid, param32), a bare `ret` (0x004F5959).
        int32_t __cdecl WorldModelEnum(void*, uint32_t flags, uint64_t, uintptr_t param32)
        {
            void* model = reinterpret_cast<void*>(param32);
            if (!model) return 1;
            // CM2Model::SetAnimating(int): __thiscall, `ret 4` (0x00823F59).
            x86::CallMember<void>(WXL_SITE("CM2Model::SetAnimating", 0), model, int32_t(1));
            // CM2Model::IsLoaded(wait, recursive): __thiscall, `ret 8` (0x00824F19).
            if (x86::CallMember<uint32_t>(WXL_SITE("CM2Model::IsLoaded", 0), model, int32_t(0), int32_t(0)) &&
                !Read<void*>(model, WXL_FIELD("CM2Model", "m_attachParent")))
            {
                const uint32_t visible = flags & 1;
                uint32_t       f       = Read<uint32_t>(model, WXL_FIELD("CM2Model", "m_fFlags"));
                f = (f & ~kShown) | (visible << 3);
                f = (f & ~kAnimShown) | (visible << 16);
                std::memcpy(static_cast<uint8_t*>(model) + WXL_FIELD("CM2Model", "m_fFlags"), &f, sizeof f);
            }
            return 1;
        }

        /// The placement's matrix, built by the client's own C44Matrix members as the recompiled
        /// client builds it: identity, turned around z, y and x, scaled, then moved.
        void PlacementMatrix(const wxl_m2_placement& at, float* m)
        {
            std::memset(m, 0, kMatrixSize);
            m[0] = m[5] = m[10] = m[15] = 1.0f;
            // C44Matrix::RotateAroundZ/Y/X(angle), Scale(float): __thiscall, `ret 4` each
            // (0x004C33B9, 0x004C3379, 0x004C3339, 0x004C1C3D).
            if (at.yaw != 0.0f) x86::CallMember<void>(WXL_SITE("C44Matrix::RotateAroundZ", 0), m, at.yaw);
            if (at.pitch != 0.0f) x86::CallMember<void>(WXL_SITE("C44Matrix::RotateAroundY", 0), m, at.pitch);
            if (at.roll != 0.0f) x86::CallMember<void>(WXL_SITE("C44Matrix::RotateAroundX", 0), m, at.roll);
            x86::CallMember<void>(WXL_SITE_AT("C44Matrix::Scale", 0x004C1BF0, 0), m, at.scale > 0.0f ? at.scale : 1.0f);
            m[12] = at.position.x;
            m[13] = at.position.y;
            m[14] = at.position.z;
        }

        /// CM2Model::SetWorldTransform(const C44Matrix&) (0x004D8630), as the fork writes it.
        void SetWorldTransform(void* model, const float* m)
        {
            uint8_t* base = static_cast<uint8_t*>(model);
            uint32_t f    = Read<uint32_t>(model, WXL_FIELD("CM2Model", "m_fFlags")) | kWorldMatrix;
            std::memcpy(base + WXL_FIELD("CM2Model", "m_fFlags"), &f, sizeof f);
            std::memcpy(base + WXL_FIELD("CM2Model", "m_worldTransform"), m, kMatrixSize);
        }
    }

    void* m2_create_world_model(const char* path)
    {
        if (!IsClient() || !path || !*path) return nullptr;
        void* scene = *reinterpret_cast<void* const*>(WXL_STATIC("World::s_m2Scene"));
        if (!scene) return nullptr;
        // CM2Scene::CreateModel(path, flags): __thiscall, `ret 8` (0x0081F8FF).
        void* model = x86::CallMember<void*>(WXL_SITE("CM2Scene::CreateModel", 0), scene, path, uint32_t(0));
        // CM2Model::SetLoadedCallback(fn, arg): __thiscall, `ret 8` (0x008251C5); it calls back at
        // once when the model has loaded already.
        if (model)
            x86::CallMember<void>(WXL_SITE("CM2Model::SetLoadedCallback", 0), model, &OnWorldModelLoaded,
                                  static_cast<void*>(nullptr));
        return model;
    }

    wxl_bool m2_place_world(void* model, const wxl_m2_placement* placement)
    {
        if (!IsClient() || !model || !placement || map_id() < 0) return WXL_FALSE;
        if (!*reinterpret_cast<void* const*>(WXL_STATIC("World::s_m2Scene"))) return WXL_FALSE;
        m2_detach_model(model);
        float matrix[16];
        PlacementMatrix(*placement, matrix);
        SetWorldTransform(model, matrix);
        uint8_t* base = static_cast<uint8_t*>(model);
        uint32_t f    = Read<uint32_t>(model, WXL_FIELD("CM2Model", "m_fFlags")) | kProjected;
        std::memcpy(base + WXL_FIELD("CM2Model", "m_fFlags"), &f, sizeof f);
        float world[16];
        std::memcpy(world, base + WXL_FIELD("CM2Model", "m_worldTransform"), sizeof world);

        void*& entity = g_worldEntities[model];
        if (!entity)
        {
            // CMap::ObjectCreateStatic(model, matrix, func, funcParam, param32): cdecl, a bare
            // `ret` (0x00782342).
            using CreateStatic = void*(__cdecl*)(void*, const float*, int32_t(__cdecl*)(void*, uint32_t, uint64_t, uintptr_t),
                                                 void*, uintptr_t);
            entity = reinterpret_cast<CreateStatic>(WXL_SITE("CMap::ObjectCreateStatic", 0))(
                model, world, &WorldModelEnum, nullptr, reinterpret_cast<uintptr_t>(model));
            if (!entity)
            {
                g_worldEntities.erase(model);
                return WXL_FALSE;
            }
        }

        float box[6]    = {};  // CAaBox: b, t
        float sphere[4] = {};  // CAaSphere: c, r
        float light[3]  = {};
        if (x86::CallMember<uint32_t>(WXL_SITE("CM2Model::IsLoaded", 0), model, int32_t(0), int32_t(0)))
        {
            // CM2Model::GetBoundingBox() / GetBoundingSphere(): __thiscall with the hidden return
            // pointer, `ret 4` (0x004F5E70, 0x004F5EF7).
            x86::CallMember<void*>(WXL_SITE("CM2Model::GetBoundingBox", 0), model, static_cast<void*>(box));
            x86::CallMember<void*>(WXL_SITE("CM2Model::GetBoundingSphere", 0), model, static_cast<void*>(sphere));
            light[0] = sphere[0], light[1] = sphere[1], light[2] = sphere[2];
        }
        light[2] = 1.0f;
        // CMap::ObjectUpdate(entity, matrix, box, sphere, light, bool, flags): cdecl, a bare
        // `ret` (0x007804F4).
        using Update = void(__cdecl*)(void*, float*, float*, float*, float*, int32_t, uint32_t);
        reinterpret_cast<Update>(WXL_SITE("CMap::ObjectUpdate", 0))(entity, world, box, sphere, light, 0, 0xFFFFFFFFu);
        return WXL_TRUE;
    }

    void m2_unplace_world(void* model)
    {
        const auto it = g_worldEntities.find(model);
        if (it == g_worldEntities.end()) return;
        void* entity = it->second;
        g_worldEntities.erase(it);
        if (!IsClient() || !model) return;
        // World::ObjectDestroy(entity): cdecl, a bare `ret` (0x00782731).
        if (entity) x86::CallFree<void>(WXL_SITE("World::ObjectDestroy", 0), entity);
        uint32_t f = Read<uint32_t>(model, WXL_FIELD("CM2Model", "m_fFlags")) & ~(kShown | kAnimShown);
        std::memcpy(static_cast<uint8_t*>(model) + WXL_FIELD("CM2Model", "m_fFlags"), &f, sizeof f);
    }

    void m2_set_transform(void* model, const wxl_m2_placement* placement)
    {
        if (!IsClient() || !model || !placement) return;
        float matrix[16];
        PlacementMatrix(*placement, matrix);
        SetWorldTransform(model, matrix);
    }

    void m2_play_animation(void* model, uint32_t anim, float speed)
    {
        if (!IsClient() || !model) return;
        if (!Loaded(model))
        {
            g_pendingAnims[model] = PendingAnim{anim, speed};
            return;
        }
        PlayNow(model, anim, speed);
    }

    wxl_bool m2_loaded(const void* model) { return IsClient() && model && Loaded(model) ? WXL_TRUE : WXL_FALSE; }

    uint32_t m2_animation_length(void* model, uint32_t anim)
    {
        if (!IsClient() || !model || !Loaded(model)) return 0;
        if (!x86::CallMember<int32_t>(WXL_SITE_AT("CM2Model::HasSequence", 0x00825EE0, 0), model, anim)) return 0;
        // CM2Model::GetSequenceInfo(id, variation, M2SequenceInfo&): __thiscall, `ret 0xc` (0x0082D11A).
        alignas(4) uint8_t info[WXL_TYPE_SIZE("M2SequenceInfo")] = {};
        x86::CallMember<void>(WXL_SITE("CM2Model::GetSequenceInfo", 0), model, anim, 0u, static_cast<void*>(info));
        return Read<uint32_t>(info, WXL_FIELD("M2SequenceInfo", "m_duration"));
    }

    void m2_set_alpha(void* model, float alpha)
    {
        if (IsClient() && model)
            std::memcpy(static_cast<uint8_t*>(model) + WXL_FIELD("CM2Model", "m_baseAlpha"), &alpha, sizeof alpha);
    }

    void m2_set_emitting(void* model, wxl_bool emitting)
    {
        if (!IsClient() || !model) return;
        // CM2Model::SetEmittersEnabled(int) / SetRibbonsEnabled(uint): __thiscall, `ret 4`
        // (0x00827A3C, 0x00824270).
        x86::CallMember<void>(WXL_SITE("CM2Model::SetEmittersEnabled", 0), model, int32_t(emitting ? 1 : 0));
        x86::CallMember<void>(WXL_SITE("CM2Model::SetRibbonsEnabled", 0), model, uint32_t(emitting ? 1 : 0));
    }

    wxl_bool m2_attachment_world(void* model, uint32_t attachment, wxl_vec3* out)
    {
        if (!IsClient() || !model || !out || !Loaded(model) || !Read<void*>(model, WXL_FIELD("CM2Model", "m_scene")))
            return WXL_FALSE;
        // CM2Model::HasAttachment(id): __thiscall, `ret 4` (0x00827437).
        if (!x86::CallMember<int32_t>(WXL_SITE("CM2Model::HasAttachment", 0), model, attachment)) return WXL_FALSE;
        // CM2Model::GetAttachmentPosition(id): __thiscall with the hidden return pointer, `ret 8`
        // (0x0083140C).
        wxl_vec3 position{};
        x86::CallMember<void*>(WXL_SITE("CM2Model::GetAttachmentPosition", 0), model, static_cast<void*>(&position),
                               attachment);
        *out = position;
        return WXL_TRUE;
    }

    // --- M2: bones for wxl.m2's bone watchers, on the animate threads -----------------------------

    wxl_bool m2_bones(void* model, wxl_m2_bones* out)
    {
        if (!IsClient() || !model || !out || !Loaded(model)) return WXL_FALSE;
        const uint8_t* shared   = static_cast<const uint8_t*>(Via(model, WXL_FIELD("CM2Model", "m_shared")));
        const void*    scene    = Via(model, WXL_FIELD("CM2Model", "m_scene"));
        float*         matrices = Read<float*>(model, WXL_FIELD("CM2Model", "m_boneMatrices"));
        const void*    header   = shared ? Via(shared, WXL_FIELD("CM2Shared", "m_data")) : nullptr;
        if (!shared || !scene || !matrices || !header) return WXL_FALSE;
        const char* name = reinterpret_cast<const char*>(shared + WXL_FIELD("CM2Shared", "m_fileName"));
        out->model       = model;
        out->file        = const_cast<uint8_t*>(shared);
        out->path        = wxl_str{name, static_cast<uint32_t>(strnlen(name, kFileNameSize))};
        out->bone_count  = Read<uint32_t>(header, WXL_FIELD("M2Data", "m_bones.m_count"));
        out->matrices    = matrices;
        std::memcpy(out->model_matrix, static_cast<const uint8_t*>(model) + WXL_FIELD("CM2Model", "m_matrixF4"),
                    sizeof out->model_matrix);
        std::memcpy(out->view_to_world, static_cast<const uint8_t*>(scene) + WXL_FIELD("CM2Scene", "m_viewInv"),
                    sizeof out->view_to_world);
        out->time_ms = Read<uint32_t>(scene, WXL_FIELD("CM2Scene", "m_time"));
        return WXL_TRUE;
    }

    wxl_bool m2_file_bone(const void* file, uint32_t index, int16_t* parent, wxl_vec3* pivot)
    {
        if (!IsClient() || !file) return WXL_FALSE;
        const void* header = Via(file, WXL_FIELD("CM2Shared", "m_data"));
        if (!header) return WXL_FALSE;
        uint32_t       count = 0;
        const uint8_t* bones = Array<uint8_t>(header, WXL_FIELD("M2Data", "m_bones.m_count"), count);
        if (!bones || index >= count) return WXL_FALSE;
        const uint8_t* bone = bones + static_cast<std::size_t>(index) * WXL_TYPE_SIZE("M2CompBone");
        if (parent) *parent = Read<int16_t>(bone, WXL_FIELD("M2CompBone", "m_parentIndex"));
        if (pivot) *pivot = Read<wxl_vec3>(bone, WXL_FIELD("M2CompBone", "m_pivot"));
        return WXL_TRUE;
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

    wxl_bool unit_can_attack(const void* unit, const void* target)
    {
        // CGUnit_C::CanAttack(const CGUnit_C*): __thiscall, `ret 4`; the answer is AL alone.
        if (!IsClient() || !OnMainThread() || !unit || !target) return WXL_FALSE;
        return CallMember<bool>(WXL_SITE("CGUnit_C::CanAttack", 0), unit, target) ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool unit_can_interact(const void* unit, const void* target)
    {
        // CGUnit_C::CanInteractNow(const CGUnit_C*): __thiscall, `ret 4`; the answer is AL alone.
        if (!IsClient() || !OnMainThread() || !unit || !target) return WXL_FALSE;
        return CallMember<bool>(WXL_SITE("CGUnit_C::CanInteractNow", 0), unit, target) ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool gameobject_can_use(const void* object)
    {
        // CGGameObject_C::CanHighlight: __thiscall, no argument (a virtual of the object, called
        // here at its address); its type helper's CanUseNow(error, distance, text): __thiscall,
        // `ret 0xC`, virtual slot 7 (`call [eax + 0x1C]` in CanChangeCursor, 0x0070F9BE); both
        // answer AL alone.
        if (!IsClient() || !OnMainThread() || !object) return WXL_FALSE;
        if (!CallMember<bool>(WXL_SITE("CGGameObject_C::CanHighlight", 0), object)) return WXL_FALSE;
        const void* helper = Read<const void*>(object, WXL_FIELD("CGGameObject_C", "m_typeData"));
        if (!helper) return WXL_FALSE;
        const uintptr_t canUseNow = (*reinterpret_cast<const uintptr_t* const*>(helper))[7];
        uint32_t        error     = 0;
        float           distance  = 0.0f;
        const char*     text      = nullptr;
        if (CallMember<bool>(canUseNow, helper, &error, &distance, &text)) return WXL_TRUE;
        return error == 240 ? WXL_TRUE : WXL_FALSE; // ERR_USE_TOO_FAR: usable once in reach
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

        using camera_facing::Dot;

        /// v * m, the client's operator*(C3Vector, C33Matrix): v's components weigh m's rows.
        wxl_vec3 Times(const wxl_vec3& v, const float (&m)[9])
        {
            return wxl_vec3{v.x * m[0] + v.y * m[3] + v.z * m[6], v.x * m[1] + v.y * m[4] + v.z * m[7],
                            v.x * m[2] + v.y * m[5] + v.z * m[8]};
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
        // The rows SetFacing(forward) builds, turned by the roll: the view matrix takes its up
        // from the third (SetGxProjectionAndView), so the world and the pick turn with it.
        wxl_vec3 rows[3];
        if (!camera_facing::Facing(view->forward, view->roll, rows)) return;
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

    uint32_t input_click_hit(const void* frame, uint64_t* guid, wxl_vec3* point)
    {
        // As CGWorldFrame::PerformDefaultAction reads it (0x004F7880): the active player, not
        // charmed; a name plate under the pointer (CGUnit_C::GetNamePlateFocus reads the static);
        // then m_defaultActionHitKind (0 none, 1 ground, 2 a model, 3 a map object).
        if (guid) *guid = 0;
        if (point) *point = wxl_vec3{0.0f, 0.0f, 0.0f};
        if (!IsClient() || !frame) return 0;
        const void* player = local_player_object();
        if (!player || unit_charmed_by(player)) return 0;
        const void* plate = *reinterpret_cast<const void* const*>(WXL_STATIC("CGUnit_C::s_namePlateFocus"));
        if (plate)
        {
            if (guid) *guid = Read<uint64_t>(plate, WXL_FIELD("CGNamePlateFrame", "m_guid"));
            return 4;
        }
        const uint32_t kind = Read<uint32_t>(frame, WXL_FIELD("CGWorldFrame", "m_defaultActionHitKind"));
        if (kind > 3) return 0;
        if (guid) *guid = Read<uint64_t>(frame, WXL_FIELD("CGWorldFrame", "m_defaultActionHit.m_guid"));
        if (point) *point = Read<wxl_vec3>(frame, WXL_FIELD("CGWorldFrame", "m_defaultActionHit.m_point"));
        // A ground or map-object hit passes its guid on only when it names a game object.
        if (guid && (kind == 1 || kind == 3))
        {
            const uint32_t high = static_cast<uint32_t>(*guid >> 32);
            if ((high & 0xF0000000u) != 0x10000000u || (high & 0x0FF00000u) != 0x0FC00000u) *guid = 0;
        }
        return kind;
    }

    wxl_bool input_spell_targeting(void)
    {
        // Spell_C_IsTargeting (0x007FD620) answers s_targetingSpell != nullptr.
        if (!IsClient()) return WXL_FALSE;
        return *reinterpret_cast<const void* const*>(WXL_STATIC("s_targetingSpell")) ? WXL_TRUE : WXL_FALSE;
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
        // EGxApi: 1 is Direct3D 9, 2 Direct3D 9Ex; the others hold no IDirect3D resources, nor
        // does a D3D9 device the Vulkan renderer draws (its records are the renderer's).
        if (gx::VulkanActive()) return 0;
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

    uint32_t texture_reload(const char* name, uint32_t* busy, uint32_t* atlas)
    {
        if (busy) *busy = 0;
        if (atlas) *atlas = 0;
        if (!IsClient() || !name || !*name) return 0;
        // s_textureList, a TSExplicitList<CTexture>: its terminator's m_next at +8, each texture's
        // TSLink m_link at +0x64, its m_next at +4; an end pointer has its low bit set (TSList::IsEnd
        // 0x00407900). The CTexture constructor links each one there (0x004B880D). m_flags +0x28
        // (0x004B87B2), m_asyncObject +0x40, m_atlas +0x5C (0x004B87E4, 0x004B8800).
        constexpr std::uintptr_t kList        = WXL_STATIC_AT("s_textureList", 0x00AC3348);
        constexpr std::size_t    kLink        = 0x64;
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
            if (Read<const void*>(texture, kAsyncObject))
            {
                if (busy) ++*busy;
                continue;
            }
            if (Read<const void*>(texture, kAtlas))
            {
                if (atlas) ++*atlas;
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
    // --- the map's ground (binding/terrain.def) ----------------------------------------------

    wxl_bool terrain_present(void)
    {
        if (!IsClient() || !OnMainThread() || map_id() < 0) return WXL_FALSE;
        if (!*reinterpret_cast<const int32_t*>(WXL_STATIC("CMap::bActive"))) return WXL_FALSE;
        return *reinterpret_cast<const int32_t*>(WXL_STATIC("CMap::bDungeon")) ? WXL_FALSE : WXL_TRUE;
    }

    void* terrain_chunk(int32_t chunk_x, int32_t chunk_y)
    {
        if (!IsClient() || !OnMainThread() || chunk_x < 0 || chunk_y < 0 || chunk_x >= 1024 || chunk_y >= 1024)
            return nullptr;
        // CMap::m_areaTable[64 * tile row + tile column], as CMap::GetChunkFacets indexes it.
        const auto* table = reinterpret_cast<void* const*>(WXL_STATIC("CMap::m_areaTable"));
        const void* area  = table[64 * (chunk_y >> 4) + (chunk_x >> 4)];
        if (!area || Read<const void*>(area, WXL_FIELD("CMapArea", "m_asyncObject"))) return nullptr;
        const std::size_t slot = WXL_FIELD("CMapArea", "m_mapChunks") +
                                 sizeof(void*) * static_cast<std::size_t>(16 * (chunk_y & 15) + (chunk_x & 15));
        return Read<void*>(area, slot);
    }

    void* map_area_chunk(const void* area, int32_t chunk_x, int32_t chunk_y)
    {
        if (!IsClient() || !OnMainThread() || !area || chunk_x < 0 || chunk_y < 0 || chunk_x > 15 || chunk_y > 15)
            return nullptr;
        return Read<void*>(area, WXL_FIELD("CMapArea", "m_mapChunks") +
                                     sizeof(void*) * static_cast<std::size_t>(16 * chunk_y + chunk_x));
    }

    /// Whether `chunk` is committed, readable memory of a chunk's size: a value left in a
    /// register, or a freed chunk's page, is not.
    bool ChunkMemory(const void* chunk)
    {
        const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(chunk);
        if (at < 0x10000 || (at & 3)) return false;
        MEMORY_BASIC_INFORMATION info{};
        const std::size_t        size     = WXL_FIELD("CMapChunk", "m_soundEmitters") + sizeof(void*);
        const DWORD              readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return VirtualQuery(chunk, &info, sizeof info) && info.State == MEM_COMMIT &&
               !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (info.Protect & readable) &&
               at + size <= reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }

    wxl_bool terrain_chunk_place(const void* chunk, int32_t* chunk_x, int32_t* chunk_y)
    {
        if (!IsClient() || !OnMainThread() || !chunk_x || !chunk_y || !ChunkMemory(chunk)) return WXL_FALSE;
        *chunk_x = Read<int32_t>(chunk, WXL_FIELD("CMapChunk", "m_cOffset.x"));
        *chunk_y = Read<int32_t>(chunk, WXL_FIELD("CMapChunk", "m_cOffset.y"));
        return *chunk_x >= 0 && *chunk_x < 1024 && *chunk_y >= 0 && *chunk_y < 1024 ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool terrain_chunk_view(void* chunk, wxl_core_terrain_chunk* out)
    {
        if (!IsClient() || !OnMainThread() || !chunk || !out || !ChunkMemory(chunk)) return WXL_FALSE;
        // Only a live chunk: its own place on the map leads back to it through the tile's slot.
        const int32_t cx = Read<int32_t>(chunk, WXL_FIELD("CMapChunk", "m_cOffset.x"));
        const int32_t cy = Read<int32_t>(chunk, WXL_FIELD("CMapChunk", "m_cOffset.y"));
        if (terrain_chunk(cx, cy) != chunk) return WXL_FALSE;
        const void* header = Read<const void*>(chunk, WXL_FIELD("CMapChunk", "m_header"));
        out->chunk_x = Read<int32_t>(chunk, WXL_FIELD("CMapChunk", "m_cOffset.x"));
        out->chunk_y = Read<int32_t>(chunk, WXL_FIELD("CMapChunk", "m_cOffset.y"));
        out->heights = Read<float*>(chunk, WXL_FIELD("CMapChunk", "m_height"));
        out->normals = Read<int8_t*>(chunk, WXL_FIELD("CMapChunk", "m_normals"));
        out->base_z  = Read<float>(chunk, WXL_FIELD("CMapChunk", "m_topLeftCoords.z"));
        out->holes   = header ? Read<uint16_t>(header, WXL_FIELD("SMChunk", "m_holes")) : 0u;
        return out->heights && out->normals ? WXL_TRUE : WXL_FALSE;
    }

    void terrain_chunk_rebuild(void* chunk, wxl_bool doodads)
    {
        if (!IsClient() || !OnMainThread() || !chunk) return;
        // CMapChunk::CreateBounds: __thiscall, no argument, a bare `ret` (0x007C5220).
        x86::CallMember<void>(WXL_SITE("CMapChunk::CreateBounds", 0), chunk);
        char* render = Read<char*>(chunk, WXL_FIELD("CMapChunk", "m_renderChunk"));
        if (render)
        {
            // CMapRenderChunk::AddBatch(chunk1, chunk2, position, flags): __thiscall, `ret 0x10`
            // (0x007B7AF0); it copies the position before it reads the chunks' boxes.
            void*    first  = Read<void*>(render, WXL_FIELD("CMapRenderChunk", "m_mapChunks[0]"));
            void*    second = Read<void*>(render, WXL_FIELD("CMapRenderChunk", "m_mapChunks[1]"));
            wxl_vec3 at     = Read<wxl_vec3>(render, WXL_FIELD("CMapRenderChunk", "m_vec1"));
            const uint32_t flags = Read<uint8_t>(render, WXL_FIELD("CMapRenderChunk", "m_unkFlags"));
            if (first) x86::CallMember<void>(WXL_SITE("CMapRenderChunk::AddBatch", 0), render, first, second, &at, flags);
            const void* buffers = Read<const void*>(render, WXL_FIELD("CMapRenderChunk", "m_chunkBuf"));
            char*       vertex  = buffers ? Read<char*>(buffers, WXL_FIELD("CMapChunkBuf", "m_vertexBuf")) : nullptr;
            if (vertex) vertex[WXL_FIELD("CGxBuf", "m_hasData")] = 0;
        }
        if (doodads)
        {
            const std::size_t at   = WXL_FIELD("CMapChunk", "m_detailDoodadInst");
            void*             inst = Read<void*>(chunk, at);
            if (inst)
            {
                // CDetailDoodad::FreeInst(inst): cdecl, one dword, a bare `ret` (0x007B3960).
                x86::CallFree<void>(WXL_SITE("CDetailDoodad::FreeInst", 0), inst);
                void* none = nullptr;
                std::memcpy(static_cast<char*>(chunk) + at, &none, sizeof none);
            }
        }
    }

    void terrain_forget_collision(void)
    {
        if (!IsClient() || !OnMainThread()) return;
        // Movement.cpp's s_queryBox, a CAaBox (b then t): one that holds no point.
        float* box = reinterpret_cast<float*>(WXL_STATIC_AT("s_queryBox", 0x00CA1660));
        for (int i = 0; i < 3; ++i)
        {
            box[i]     = 3.4028235e38f;
            box[3 + i] = -3.4028235e38f;
        }
    }

    void unit_fall_local(const void* unit)
    {
        if (!IsClient() || !OnMainThread() || !unit) return;
        const void* movement = Read<const void*>(unit, WXL_FIELD("CGUnit_C", "m_movement"));
        if (!movement) return;
        // CMovement_C::OnFallLocal(eventTime): __thiscall, `ret 4` (0x006ECCF0).
        x86::CallMember<void>(WXL_SITE("CMovement_C::OnFallLocal", 0), movement, os_time_ms());
    }

    // --- Animation: a unit's animation as the client plays it --------------------------------

    void unit_play_animation(void* unit, uint32_t anim, uint32_t flags)
    {
        if (!IsClient() || !OnMainThread() || !unit) return;
        // CGUnit_C::PlayBaseAnimation(animId, flags): __thiscall, `ret 8` (0x007385C0).
        x86::CallMember<void>(WXL_SITE("CGUnit_C::PlayBaseAnimation", 0), unit, anim, static_cast<int32_t>(flags));
    }

    void unit_update_animation(void* unit)
    {
        if (!IsClient() || !OnMainThread() || !unit) return;
        // CGUnit_C::UpdateBaseAnimation(animFlags, mask): __thiscall, `ret 8` (0x0073AC30).
        x86::CallMember<void>(WXL_SITE("CGUnit_C::UpdateBaseAnimation", 0), unit, 0u, int32_t(-1));
    }

    uint32_t unit_shown_animation(const void* unit)
    {
        if (!IsClient() || !OnMainThread() || !unit) return 0xFFFFFFFFu;
        // CGUnit_C::GetCurrentTorsoAnimId(): __thiscall, bare `ret` (0x00717260).
        return x86::CallMember<uint32_t>(WXL_SITE("CGUnit_C::GetCurrentTorsoAnimId", 0), unit);
    }

    void unit_animation_speed(void* unit, uint32_t anim, float speed)
    {
        if (!IsClient() || !OnMainThread() || !unit) return;
        void* model = Read<void*>(unit, WXL_FIELD("CGObject_C", "m_worldModel"));
        if (!model || (Read<uint32_t>(model, WXL_FIELD("CM2Model", "m_fFlags")) & 1u) == 0) return;
        const int32_t torso = Read<int32_t>(unit, WXL_FIELD("CGUnit_C", "m_torsoKeyBone"));
        const uint32_t bones[2] = {0xFFFFFFFFu, static_cast<uint32_t>(torso)};
        for (int i = 0; i < (torso == -1 ? 1 : 2); ++i)
        {
            // CM2Model::GetBoneSequenceId(bone): __thiscall, `ret 4` (0x008267E0).
            if (x86::CallMember<uint32_t>(WXL_SITE("CM2Model::GetBoneSequenceId", 0), model, bones[i]) != anim) continue;
            // CGUnit_C::SetKeyBoneSequenceSpeed(model, bone, speed, force): __thiscall, `ret 0x10` (0x00735CC0).
            x86::CallMember<void>(WXL_SITE("CGUnit_C::SetKeyBoneSequenceSpeed", 0), unit, model, bones[i], speed, int32_t(0));
        }
    }

    uint32_t unit_animation_length(const void* unit, uint32_t anim)
    {
        if (!IsClient() || !OnMainThread() || !unit) return 0;
        void* model = Read<void*>(unit, WXL_FIELD("CGObject_C", "m_worldModel"));
        return model ? m2_animation_length(model, anim) : 0;
    }

    void unit_lift_local(void* unit, float z)
    {
        if (!IsClient() || !OnMainThread() || !unit) return;
        void* movement = Read<void*>(unit, WXL_FIELD("CGUnit_C", "m_movement"));
        if (!movement) return;
        // CMovement_C's position, a CPassenger's: its z.
        std::memcpy(static_cast<char*>(movement) + WXL_FIELD("CPassenger", "m_position") + 2 * sizeof(float), &z, sizeof z);
    }
}
