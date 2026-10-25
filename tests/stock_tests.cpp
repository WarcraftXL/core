// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The runtime's recompiled bodies (src/deinlined/) held to Wow.exe 12340's own code.
//
// A stock Wow.exe (the first argument, or $WXL_WOW_EXE) is copied into this process at its own
// base, 0x00400000, without running anything of it: its sections are laid into a reservation of
// this executable's own, which is linked low (CMakeLists.txt) so that the reservation covers
// Wow.exe's addresses. Its CM2Shared::LoadSucceededCallback and FinishLoading then run here on
// model files made up on the spot, twice each: as Wow.exe has them (M2Init copied into
// FinishLoading), and with the de-inlined point M2Init armed, so that the runtime's recompiled
// FinishLoading runs in place of the stock one, M2Init a real function in it. What the client
// would do around them -- initialise the model (textures, buffers), allocate through Storm, free
// the file read -- is stubbed at Wow.exe's own addresses. Both runs must answer the same, leave
// the model's fields and its relocated bytes the same and ask Storm for the same block.
//
// Without a Wow.exe the test says so and passes: nothing of Wow.exe is in this repository.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/Log.hpp"
#include "deinlined/Deinlined.hpp"
#include "hook/Engine.hpp"
#include "points/Points.hpp"
#include "points/PointsX86.hpp"
#include "wxl/engine/m2.hpp"
#include "wxl/host/mod_host.hpp"

namespace
{
    std::unique_ptr<wxl::host::ModHost> g_host;
    int                                 g_failures = 0, g_checks = 0;

    void Check(bool ok, const char* what)
    {
        ++g_checks;
        if (!ok) ++g_failures;
        std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    }

    /// Where Wow.exe goes: zeroes in this image's .bss, as large as Wow.exe and then some.
    uint8_t g_room[0xE00000];

    constexpr uintptr_t kStockBase        = 0x00400000;
    constexpr uintptr_t kLoadSucceeded    = 0x0083D340;
    constexpr uintptr_t kFinishLoading    = 0x0083CF00;
    constexpr uintptr_t kFinishLength     = 962;
    constexpr uintptr_t kSharedInitialize = 0x0083CC80;
    constexpr uintptr_t kSMemAlloc        = 0x0076E540;
    constexpr uintptr_t kAsyncDestroy     = 0x004B9DE0;
    constexpr uintptr_t kSequenceIndex    = 0x00AF59D8; ///< M2Shared.cpp's s_sequenceIndex

    /// Copies Wow.exe's headers and sections to their addresses; nothing of it runs.
    bool MapStockClient(const char* path, std::string& why)
    {
        FILE* file = std::fopen(path, "rb");
        if (!file)
        {
            why = std::string(path) + " cannot be read";
            return false;
        }
        std::vector<uint8_t> bytes;
        uint8_t              chunk[65536];
        for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, file)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
        std::fclose(file);
        auto u16 = [&](size_t at) { return static_cast<uint32_t>(bytes[at] | bytes[at + 1] << 8); };
        auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
        if (bytes.size() < 0x400 || u16(0) != 0x5A4D) return why = "not a PE file", false;
        const size_t   pe       = u32(0x3C);
        const uint32_t sections = u16(pe + 6);
        const size_t   optional = pe + 24;
        const uint32_t base     = u32(optional + 28);
        const uint32_t size     = u32(optional + 56);
        const uint32_t headers  = u32(optional + 60);
        const uintptr_t room    = reinterpret_cast<uintptr_t>(g_room);
        if (base != kStockBase) return why = "its base is not 0x00400000", false;
        if (room > base || room + sizeof g_room < base + size)
        {
            char line[160];
            std::snprintf(line, sizeof line, "this test's room (%p, %zu bytes) does not cover 0x%08X-0x%08X",
                          static_cast<void*>(g_room), sizeof g_room, base, base + size);
            return why = line, false;
        }
        DWORD was = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(base), size, PAGE_EXECUTE_READWRITE, &was))
            return why = "its room cannot be made executable", false;
        std::memcpy(reinterpret_cast<void*>(base), bytes.data(), headers);
        const size_t table = optional + u16(pe + 20);
        for (uint32_t i = 0; i < sections; ++i)
        {
            const size_t   s       = table + 40 * i;
            const uint32_t rva     = u32(s + 12);
            const uint32_t rawSize = u32(s + 16);
            const uint32_t raw     = u32(s + 20);
            if (raw + rawSize > bytes.size()) return why = "a section runs past the file", false;
            std::memcpy(reinterpret_cast<void*>(base + rva), bytes.data() + raw, rawSize);
        }
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(base), size);
        // LoadSucceededCallback's first bytes: Wow.exe 12340.
        const uint8_t head[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0x8B, 0x46, 0x0C};
        if (std::memcmp(reinterpret_cast<const void*>(kLoadSucceeded), head, sizeof head) != 0)
            return why = "it is not Wow.exe 12340", false;
        // Its C runtime never ran, so no static initialiser did: s_sequenceIndex is set as its
        // initialiser sets it, "no sequence being read".
        *reinterpret_cast<uint32_t*>(kSequenceIndex) = 0xFFFFFFFF;
        return true;
    }

    int         g_initCalls  = 0;
    int32_t     g_initAnswer = 1;
    int         g_allocCalls = 0;
    size_t      g_allocSize  = 0;
    std::string g_allocFile;
    void*       g_destroyed  = nullptr;
    void*       g_unusedNext = nullptr;

    int32_t __fastcall StubInitialize(void*, void*)
    {
        ++g_initCalls;
        return g_initAnswer;
    }

    // SMemAlloc is STORMAPI: __stdcall, the callee pops its four arguments.
    void* __stdcall StubAlloc(size_t size, const char* file, int32_t, uint32_t)
    {
        ++g_allocCalls;
        g_allocSize = size;
        g_allocFile = file ? file : "";
        return std::calloc(1, size ? size : 1);
    }

    void __cdecl StubDestroy(void* object) { g_destroyed = object; }

    /// A model file: the header and the arrays a 3.3.5a model carries, each where the header says.
    std::vector<uint8_t> MakeModel(int variant)
    {
        using namespace wxl::engine;
        std::vector<uint8_t> bytes(0x1000, 0);
        auto&                data = *reinterpret_cast<M2Data*>(bytes.data());
        uint32_t             at   = sizeof(M2Data);
        auto place = [&](auto& array, uint32_t count, uint32_t size) {
            array.m_count  = count;
            array.m_offset = at;
            at             = (at + count * size + 15) & ~15u;
            return bytes.data() + array.m_offset;
        };
        data.m_magic   = 0x3032444D;
        data.m_version = 0x108;
        std::memcpy(place(data.m_name, 8, 1), "Test.m2", 8);
        place(data.m_loops, 2, sizeof(M2Loop));
        auto*          sequences = reinterpret_cast<M2Sequence*>(place(data.m_sequences, 4, sizeof(M2Sequence)));
        const uint16_t ids[4]    = {0, 4, 7, 12};
        const uint32_t flags[4]  = {0x20, 0x21, 0x0, 0x1};
        for (int i = 0; i < 4; ++i)
        {
            sequences[i].m_id        = ids[i];
            sequences[i].m_flags     = flags[i];
            sequences[i].m_aliasNext = static_cast<uint16_t>(i);
        }
        place(data.m_sequenceIdxHashById, 4, 2);
        place(data.m_vertices, 6, sizeof(M2Vertex));
        auto* texture = reinterpret_cast<M2Texture*>(place(data.m_textures, 1, sizeof(M2Texture)));
        std::memcpy(place(texture->m_filename, 11, 1), "Skin00.blp", 11);
        place(data.m_materials, 2, sizeof(M2Material));
        place(data.m_textureCombos, 3, 2);
        place(data.m_collisionIndices, 6, 2);
        place(data.m_collisionPositions, 4, sizeof(C3Vector));
        data.m_flags = 8;
        place(data.m_textureCombinerCombos, 2, 2);
        switch (variant)
        {
        case 1: data.m_magic = 0x3132444D; break;             // not MD20
        case 2: data.m_version = 0x107; break;                // a version the client refuses
        case 3: data.m_vertices.m_offset = 0x7FFFFF00; break; // an array past the file's end
        case 4: data.m_flags = 0; break;                      // no combiner combos to read
        default: break;
        }
        bytes.resize(at);
        return bytes;
    }

    struct Outcome
    {
        int32_t              answer = 0;
        uint32_t             flags = 0, capacity = 0, count = 0;
        bool                 buffers = false;
        int                  inits = 0, allocs = 0;
        size_t               allocSize = 0;
        std::string          allocFile;
        std::vector<uint8_t> bytes;

        bool operator==(const Outcome& o) const
        {
            return answer == o.answer && flags == o.flags && capacity == o.capacity && count == o.count &&
                   buffers == o.buffers && inits == o.inits && allocs == o.allocs && allocSize == o.allocSize &&
                   allocFile == o.allocFile && bytes == o.bytes;
        }
    };

    alignas(16) uint8_t g_file[0x2000];

    /// One model through Wow.exe's LoadSucceededCallback (which tail-jumps into FinishLoading),
    /// then FinishLoading's own answer on a fresh copy: the callback drops it.
    Outcome LoadOnce(const std::vector<uint8_t>& file, bool loaded)
    {
        using namespace wxl::engine;
        std::memcpy(g_file, file.data(), file.size());
        CM2Shared shared{};
        shared.m_flags       = loaded ? 1u : 0u;
        shared.m_data        = reinterpret_cast<M2Data*>(g_file);
        shared.m_fileSize    = static_cast<uint32_t>(file.size());
        shared.m_asyncObject = reinterpret_cast<void*>(uintptr_t{0x5A5A0000});
        std::strcpy(shared.m_fileName, "Creature\\Test\\Test.m2");
        g_initCalls = g_allocCalls = 0;
        g_allocSize = 0;
        g_allocFile.clear();
        g_destroyed = nullptr;
        reinterpret_cast<void(__cdecl*)(void*)>(kLoadSucceeded)(&shared);
        Outcome out;
        out.flags     = shared.m_flags;
        out.capacity  = shared.m_lowPrioritySequenceCapacity;
        out.count     = shared.m_lowPrioritySequenceCount;
        out.buffers   = shared.m_lowPrioritySequenceBuffers != nullptr;
        out.inits     = g_initCalls;
        out.allocs    = g_allocCalls;
        out.allocSize = g_allocSize;
        out.allocFile = g_allocFile;
        out.bytes.assign(g_file, g_file + file.size());
        if (g_destroyed != reinterpret_cast<void*>(uintptr_t{0x5A5A0000}) || shared.m_asyncObject != nullptr)
            out.answer = -1000; // the callback did not hand the read back: no outcome compares equal
        std::free(shared.m_lowPrioritySequenceBuffers);
        std::memcpy(g_file, file.data(), file.size());
        shared.m_flags                      = loaded ? 1u : 0u;
        shared.m_lowPrioritySequenceBuffers = nullptr;
        out.answer += reinterpret_cast<int32_t(__thiscall*)(void*)>(kFinishLoading)(&shared);
        std::free(shared.m_lowPrioritySequenceBuffers);
        return out;
    }

    int      g_childCalls  = 0;
    uint32_t g_childSize   = 0;
    int      g_parentCalls = 0;

    void WXL_CALL ChildBefore(wxl_point_call* call, void*)
    {
        ++g_childCalls;
        g_childSize = *static_cast<uint32_t*>(call->args[1]);
    }

    void WXL_CALL ParentBefore(wxl_point_call*, void*) { ++g_parentCalls; }

    uint32_t Attach(uint32_t index, uint32_t side, wxl_point_fn fn)
    {
        return wxl::core::points::Api().attach(index + 1, side, fn, nullptr, 0, 0);
    }
}

namespace wxl::core
{
    wxl::host::ModHost* Host() { return g_host.get(); }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    namespace pts = wxl::core::points;

    static char fromEnv[1024];
    const char* path = argc > 1 ? argv[1] : (GetEnvironmentVariableA("WXL_WOW_EXE", fromEnv, sizeof fromEnv) ? fromEnv : nullptr);
    if (!path || !*path)
    {
        std::printf("no Wow.exe 12340 given (first argument, or $WXL_WOW_EXE): the recompiled bodies are not run "
                    "against Wow.exe's own\n");
        return 0;
    }
    std::string why;
    if (!MapStockClient(path, why))
    {
        std::printf("FAIL %s: %s\n", path, why.c_str());
        return 1;
    }
    std::printf("%s laid at 0x00400000\n", path);

    wxl::host::HostConfig config;
    config.runtime_id   = "stock-tests";
    config.runtime_name = "stock tests";
    g_host              = std::make_unique<wxl::host::ModHost>(std::move(config));

    // The runtime's warnings (a hook refused) next to the checks.
    wxl::log::EnableConsole("     ");
    wxl::log::SetMinLevel(wxl::log::Level::Warn);
    wxl::hook::Init();
    const wxl::hook::Id stubs[] = {
        wxl::hook::Attach("Stub.Initialize", reinterpret_cast<void*>(kSharedInitialize),
                          reinterpret_cast<void*>(&StubInitialize), &g_unusedNext),
        wxl::hook::Attach("Stub.SMemAlloc", reinterpret_cast<void*>(kSMemAlloc), reinterpret_cast<void*>(&StubAlloc),
                          &g_unusedNext),
        wxl::hook::Attach("Stub.AsyncDestroy", reinterpret_cast<void*>(kAsyncDestroy),
                          reinterpret_cast<void*>(&StubDestroy), &g_unusedNext),
    };
    Check(stubs[0] && stubs[1] && stubs[2], "Wow.exe's model initialisation, Storm's allocator and the file read's "
                                            "release are stubbed at their addresses");
    // Wow.exe is no client process here: the parent point is placed at its own address.
    pts::x86::Place(pts::kCM2Shared_FinishLoading, kFinishLoading, kFinishLength);
    const char* what[] = {"a model", "a file that is not MD20", "a version the client refuses", "an array past the end",
                          "no combiner combos"};
    for (int variant = 0; variant < 5; ++variant)
        for (int round = 0; round < 3; ++round)
        {
            const std::vector<uint8_t> file   = MakeModel(variant);
            const bool                 loaded = round == 2;
            g_initAnswer                      = round == 1 ? 0 : 1;
            const Outcome stock               = LoadOnce(file, loaded);

            const uint32_t child  = Attach(pts::kM2Init_uint8_t_uint32_t_M2Data, WXL_POINT_BEFORE, &ChildBefore);
            const uint32_t parent = Attach(pts::kCM2Shared_FinishLoading, WXL_POINT_BEFORE, &ParentBefore);
            g_childCalls = g_parentCalls = 0;
            g_childSize                  = 0;
            const Outcome recompiled     = LoadOnce(file, loaded);
            const bool    inPlace        = pts::x86::BodyInPlace(pts::kCM2Shared_FinishLoading);
            pts::Api().detach(child);
            pts::Api().detach(parent);

            char line[320];
            std::snprintf(line, sizeof line,
                          "%s%s%s: Wow.exe and the recompiled body answer %d/%d, flags %X/%X, %d/%d init(s), "
                          "%zu/%zu byte(s) from Storm, the same relocated file",
                          what[variant], round == 1 ? ", its initialisation failing" : "", loaded ? ", already loaded" : "",
                          stock.answer, recompiled.answer, stock.flags, recompiled.flags, stock.inits, recompiled.inits,
                          stock.allocSize, recompiled.allocSize);
            Check(child && parent && inPlace && stock == recompiled, line);
            Check(g_parentCalls == 2 && g_childCalls == (loaded ? 0 : 2) &&
                      (loaded || g_childSize == static_cast<uint32_t>(file.size())),
                  "  FinishLoading's point fired at both calls, and the de-inlined M2Init's in each body that read the file");
            if (variant == 0 && round == 0)
                Check(stock.answer == 1 && stock.flags == 1 && stock.capacity == 2 && stock.allocSize == 8 &&
                          stock.allocFile == ".\\M2Shared.cpp",
                      "  a good model: loaded, two sequences left for later, eight bytes from Storm for them, under "
                      "Wow.exe's own file name");
            if (variant >= 1 && variant <= 3 && round == 0)
                Check(stock.answer == 0 && stock.inits == 0, "  refused before the model is initialised");
        }
    Check(!pts::x86::BodyInPlace(pts::kCM2Shared_FinishLoading) && !wxl::hook::Armed(reinterpret_cast<void*>(kFinishLoading)),
          "with the handlers gone, Wow.exe's FinishLoading is its own again, not detoured");
    for (wxl::hook::Id id : stubs) wxl::hook::Detach(id);
    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
