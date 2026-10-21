// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The x86 realisation of the client binding: memory reads at measured offsets, calls at resolved addresses.

#include "binding/Binding.hpp"
#include "binding/BindingX86.hpp"

#include "common/Log.hpp"
#include "runtime/Core.hpp"
#include "runtime/Image.hpp"

#include "wxl/events.h"
#include "wxl/offsets/layout.hpp"
#include "wxl/offsets/site.hpp"
#include "wxl/services/script.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

    const void* local_player_object(void)
    {
        if (!IsClient() || !OnMainThread()) return nullptr;
        return reinterpret_cast<PlayerFn>(WXL_SITE("ClntObjMgrGetActivePlayerObj", 0))();
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
        // Method 0 is FILE_BEGIN: the offset is absolute, which is what read_at means.
        return call(file, static_cast<int32_t>(offset & 0xFFFFFFFFu), &high, 0) ? WXL_TRUE
                                                                                : WXL_FALSE;
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

    // wxl.gx is served only where the client hands its device and model renderer over (x64).
    // Here the rows answer absent, and the M2 seam's body finds nothing to report.
    // Not measured on x86 yet: the reaction answers absent there.
    int32_t unit_reaction(const void*, const void*) { return -1; }

    void* gx_d3d9_device() { return nullptr; }

    void gx_invalidate_state() {}

    void* gx_object_model(const void*) { return nullptr; }

    wxl_bool gx_model_draw(const void*, wxl_gx_model_draw*) { return WXL_FALSE; }

    // wxl.m2 is served only where the client hands its appearances and models over (x64). Here
    // the rows answer absent, and the Component and AnimateMT seams find nothing to hand on.
    wxl_bool component_wearer(const void*, uint32_t, wxl_m2_equipment*) { return WXL_FALSE; }

    wxl_bool m2_item_display(uint32_t, wxl_m2_item_display*) { return WXL_FALSE; }

    const char* m2_race_prefix(uint32_t) { return nullptr; }

    void* m2_create_model(void*, const char*) { return nullptr; }

    void m2_release_model(void*) {}

    wxl_bool m2_attach_model(void*, void*, uint32_t, wxl_bool) { return WXL_FALSE; }

    void m2_detach_model(void*) {}

    void* m2_model_parent(const void*) { return nullptr; }

    void* m2_model_source(const void*) { return nullptr; }

    void m2_set_geosets(void*, uint32_t, uint32_t, wxl_bool) {}

    wxl_bool m2_replace_texture(void*, uint32_t, const char*) { return WXL_FALSE; }

    wxl_bool m2_pose_from_parent(void*) { return WXL_FALSE; }

    uint32_t m2_children(const void*, void**, uint32_t) { return 0; }

    wxl_bool m2_attach_info(const void*, uint32_t*, wxl_bool*) { return WXL_FALSE; }

    wxl_bool m2_attach_copy(void*, void*, uint32_t, wxl_bool) { return WXL_FALSE; }

    void* m2_model_file(const void*) { return nullptr; }

    wxl_bool m2_attachment_point(const void*, uint32_t, wxl_vec3*) { return WXL_FALSE; }

    wxl_bool m2_has_key_bone(const void*, uint32_t) { return WXL_FALSE; }

    wxl_bool m2_pose_on_bone(void*, uint32_t, const wxl_vec3*) { return WXL_FALSE; }

    // wxl.m2file is served only where the client hands its model loader over (x64). Here the rows
    // answer absent, and the loader seams find no file to hand on.
    wxl_bool m2_file_info(const void*, wxl_m2file*) { return WXL_FALSE; }

    void m2_file_set_size(void*, uint32_t) {}

    void m2_file_animate_bones(void*) {}

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
}
