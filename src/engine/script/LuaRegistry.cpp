// SPDX-License-Identifier: GPL-3.0-or-later
// FrameXML_CreateFrames runs before every interface load (glue, game, each /reload): the functions go in first.

#include "engine/script/LuaRegistry.hpp"

#include "common/Log.hpp"
#include "engine/script/Profiles.hpp"
#include "runtime/HookPoints.hpp"
#include "wxl/game/Script.hpp"
#include "wxl/offsets/free/F.hpp"
#include "wxl/offsets/free/O.hpp"

#include <cstring>

namespace
{
    namespace gs = wxl::game::script;
    namespace off = wxl::offsets;

    constexpr int kMaxFunctions = 256;
    constexpr int kMaxChunks    = 16;

    struct Entry
    {
        char            name[64];
        WXL_LuaFunction fn;
    };
    Entry g_entries[kMaxFunctions];
    int   g_count = 0;

    /// Script run after each interface load, once its frames exist.
    struct Chunk
    {
        const char* name;
        const char* source;
    };
    Chunk g_chunks[kMaxChunks];
    int   g_chunkCount = 0;

    off::OsValidateFunctionPointerFn g_origValidate = nullptr;
    off::FrameXML_CreateFramesFn    g_origLoad     = nullptr;
    bool                           g_hooked       = false;

    bool Registered(uintptr_t fn)
    {
        for (int i = 0; i < g_count; ++i)
            if (reinterpret_cast<uintptr_t>(g_entries[i].fn) == fn) return true;
        return false;
    }

    /// The client refuses, fatally, any callback outside Wow.exe: ours pass, every other one is the
    /// original's to judge.
    void __cdecl ValidateDetour(const void* function)
    {
        if (Registered(reinterpret_cast<uintptr_t>(function))) return;
        g_origValidate(function);
    }

    /// WXL_PROFILE loads here: every extension has registered its settings by the first interface load.
    int __cdecl InterfaceLoadDetour(const char* tocPath, const char* addOnName, void* md5Context, void* status)
    {
        wxl::script::profiles::LoadStartupOnce();
        if (g_count && gs::Context())
        {
            for (int i = 0; i < g_count; ++i)
                gs::Register(g_entries[i].name, reinterpret_cast<gs::Function>(g_entries[i].fn));
            WLOG_INFO_ONCE("lua: %d extension function(s) registered ahead of the interface load (%s)", g_count,
                           tocPath ? tocPath : "?");
        }
        const int loaded = g_origLoad(tocPath, addOnName, md5Context, status);
        if (gs::Context())
            for (int i = 0; i < g_chunkCount; ++i) gs::Execute(g_chunks[i].source, g_chunks[i].name);
        return loaded;
    }
}

namespace wxl::script
{
    bool InstallSeams()
    {
        const bool validate = wxl::runtime::hookpoints::Attach("OsValidateFunctionPointer", &ValidateDetour, &g_origValidate);
        const bool load     = wxl::runtime::hookpoints::Attach("FrameXML_CreateFrames", &InterfaceLoadDetour, &g_origLoad);
        g_hooked = validate && load;
        if (!g_hooked)
            WLOG_WARN("lua: the script seams could not be hooked (validate %s, interface load %s)",
                      validate ? "ok" : "failed", load ? "ok" : "failed");
        return g_hooked;
    }

    bool AddFunction(const char* name, WXL_LuaFunction fn)
    {
        if (!g_hooked || !name || !*name || !fn || std::strlen(name) >= sizeof g_entries[0].name) return false;
        Entry* e = nullptr;
        for (int i = 0; i < g_count && !e; ++i)
            if (std::strcmp(g_entries[i].name, name) == 0) e = &g_entries[i];
        if (!e)
        {
            if (g_count == kMaxFunctions) return false;
            e = &g_entries[g_count++];
            std::strcpy(e->name, name);
        }
        e->fn = fn;
        // A live context gets it now; every later load gets it again.
        if (gs::Context()) gs::Register(e->name, reinterpret_cast<gs::Function>(fn));
        WLOG_INFO("lua: %s registered", e->name);
        return true;
    }

    bool AddChunk(const char* name, const char* source)
    {
        if (!g_hooked || !name || !source || g_chunkCount == kMaxChunks) return false;
        g_chunks[g_chunkCount++] = Chunk{ name, source };
        if (gs::Context()) gs::Execute(source, name);
        return true;
    }
}
