// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The seam harness: the seam bodies, driven by hand against a real mod host and a fake client.

#include "binding/Binding.hpp"
#include "objects/Objects.hpp"
#include "points/Points.hpp"
#include "profile/Profile.hpp"
#include "services/EngineService.hpp"
#if defined(_M_IX86)
#include "deinlined/Deinlined.hpp"
#include "gx/x86/Adapter.hpp"
#endif
#include "runtime/Attachment.hpp"
#include "runtime/Code.hpp"
#include "runtime/Crash.hpp"
#include "runtime/Frame.hpp"
#include "runtime/Listening.hpp"
#include "gx/Vulkan.hpp"
#include "runtime/Stages.hpp"
#include "seams/Gameplay.hpp"
#include "seams/Table.hpp"
#include "services/ActionService.hpp"
#include "services/CameraService.hpp"
#include "services/InputService.hpp"
#include "services/WorldService.hpp"
#include "services/ImGuiService.hpp"
#include "services/AnimationService.hpp"
#include "services/NetService.hpp"
#include "net/Zlib.hpp"
#include "services/PoseService.hpp"
#include "services/HighlightService.hpp"
#include "services/SelectionCircleService.hpp"
#include "services/TextureService.hpp"
#include "imaging/Imaging.hpp"
#include "services/CVarService.hpp"
#include "services/TerrainService.hpp"
#include "services/DrawService.hpp"
#include "gx/d3d9/Device.hpp"
#include "gx/d3d9/Shapes.hpp"
#include "gx/shapes/Geometry.hpp"
#include <imgui.h>
#include <implot.h>
#include "services/DataService.hpp"
#include "services/FootprintService.hpp"
#include "services/GxService.hpp"
#include "services/M2FileService.hpp"
#include "services/M2Service.hpp"
#include "services/ScriptService.hpp"
#include "services/Assets.hpp"
#include "services/Hashes.hpp"
#include "services/RelayService.hpp"
#include "services/RenderService.hpp"
#include "tuning/Animate.hpp"
#include "tuning/Drops.hpp"
#include "services/StorageService.hpp"

#include <chrono>

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"
#include "wxl/seams/seams.h"
#include "wxl/core/camera_facing.hpp"
#include "wxl/core/points.h"
#include "wxl/core/gx_vk.h"
#include "wxl/engine/gx.hpp"
#include "services/ShaderCode.hpp"
#include "services/TerrainLayers.hpp"
#include "services/TerrainTiles.hpp"
#include "services/ModelGate.hpp"
#include "wxl/core/gx_vk.h"
#include "wxl/core/profile.h"
#include "wxl/core/storm.hpp"
#include "wxl/objects/map.hpp"
#include "wxl/runtime/handles.hpp"
#include "wxl/core/filestack.hpp"
#include "wxl/services/script.h"
#include "wxl/services/storage.h"
#include "wxl/terrain/grid.hpp"

#if defined(_M_IX86)
    #include "binding/CallX86.hpp"
    #include "binding/ModelDrawX86.hpp"
    #include "binding/HighlightDrawX86.hpp"
    #include "gx/d3d9/Device.hpp"
    #include "gx/d3d9/Highlight.hpp"
    #include "seams/Detour.hpp"
    #include "hook/Engine.hpp"
    #include "hook/Safety.hpp"
    #include "points/PointsX86.hpp"
    #include "seams/RegparmX86.hpp"
#else
    #include "gx/vk/render/Render.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <deque>
#include <string>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#undef far
#undef near

// --- What Core.cpp and the attachments supply in the DLL -------------------------------------

namespace
{
    std::unique_ptr<wxl::host::ModHost> g_host;
}

namespace wxl::core
{
    wxl::host::ModHost* Host() { return g_host.get(); }
}

namespace
{
    int g_released = 0;
}

// The attachment is per-architecture and the harness is neither, so it stands in for it. What
// the seam bodies ask of it is counted, which is how the checks below know it was asked.
namespace wxl::core::attachment
{
    void Release(uint32_t) { ++g_released; }
    void ReleaseAll() { ++g_released; }
}

// --- A fake client, handed to the binding as a wxl_core_init ---------------------------------

namespace
{
    /// An object as the fake client keeps it; the binding's accessors read these.
    struct FakeObject
    {
        uint64_t guid   = 0;
        uint32_t typeId = 0;
        uint32_t entry  = 0;
        wxl_vec3 position{};
        int32_t  health = 0;
        int32_t  visibleItems[19]{};
        uint32_t castingSpellId = 0; ///< the cast the client shows on a unit
        uint32_t channelSpellId = 0; ///< the channel it shows
        uint32_t castEnd        = 0; ///< when the cast ends, on the fake clock
        uint32_t channelEnd     = 0;
        int32_t  mountModel     = 0;
        uint32_t clientFlags    = 0;
        int32_t  reaction       = 3; ///< towards anyone
        int32_t  creatureType   = 0;
        uint32_t family         = 0;
        uint32_t rank           = 0;
        uint32_t knownSpell     = 0;
        float    speeds[9]{};
        struct Stats
        {
            uint32_t quality, level, requiredLevel, itemClass, subclass, inventoryType;
            int32_t  stackable;
            uint32_t sellPrice;
        };
        const Stats* stats = nullptr; ///< the item cache's record, null until cached
        uint32_t movementFlags  = 0; ///< the movement block's flags
        float    facing         = 0.0f;
        uint32_t money          = 0; ///< the local block's coinage
        // The animation rows: what the model shows, and what was asked of it.
        uint32_t shownAnim      = 0;     ///< 0 Stand
        bool     modelLoading   = false; ///< the model shows nothing yet
        bool     refusePlay     = false; ///< the client plays nothing (a transition)
        uint32_t plays          = 0;
        uint32_t lastPlayFlags  = 0;
        uint32_t updates        = 0;
        float    animSpeed      = 1.0f;
        uint32_t lifts          = 0;
        std::vector<uint32_t> block; ///< the descriptor block, empty for none
    };

    /// The map's ground as wxl.terrain's rows read it: a block of 3 x 3 chunks, columns 500-502
    /// and rows 600-602, flat at 100 yards, normals up as a map file has them, one hole.
    struct FakeChunk
    {
        int32_t  cx = 0, cy = 0;
        float    heights[wxl::terrain::kVertexCount]{};
        int8_t   normals[wxl::terrain::kVertexCount * 3]{};
        float    base   = 100.0f;
        uint32_t holes  = 0;
        bool     loaded = true;
        int      rebuilds = 0;
        int      clutterFreed = 0;

        void Reset()
        {
            for (uint32_t i = 0; i < wxl::terrain::kVertexCount; ++i)
            {
                heights[i]         = 0.0f;
                normals[3 * i]     = 0;
                normals[3 * i + 1] = 0;
                normals[3 * i + 2] = 127;
            }
        }
    };
    FakeChunk g_ground[9];
    bool      g_groundPresent = false;
    int       g_groundForgets = 0;
    int       g_groundFalls   = 0;

    FakeChunk* GroundChunk(int32_t cx, int32_t cy)
    {
        if (cx < 500 || cx > 502 || cy < 600 || cy > 602) return nullptr;
        FakeChunk& chunk = g_ground[(cy - 600) * 3 + (cx - 500)];
        return chunk.loaded ? &chunk : nullptr;
    }

    wxl_bool WXL_CALL FakeTerrainPresent(void) { return g_groundPresent ? WXL_TRUE : WXL_FALSE; }
    void* WXL_CALL FakeTerrainChunk(int32_t cx, int32_t cy) { return GroundChunk(cx, cy); }
    /// A tile holding the block's chunks at local columns and rows 0-2.
    int g_groundArea = 0;

    void* WXL_CALL FakeMapAreaChunk(const void* area, int32_t x, int32_t y)
    {
        return area == &g_groundArea ? GroundChunk(500 + x, 600 + y) : nullptr;
    }

    /// Whether a pointer is one of the fake chunks: the real rows refuse anything else.
    bool IsGroundChunk(const void* chunk)
    {
        for (const FakeChunk& c : g_ground)
            if (&c == chunk) return true;
        return false;
    }

    wxl_bool WXL_CALL FakeTerrainPlace(const void* chunk, int32_t* x, int32_t* y)
    {
        if (!IsGroundChunk(chunk) || !x || !y) return WXL_FALSE;
        *x = static_cast<const FakeChunk*>(chunk)->cx;
        *y = static_cast<const FakeChunk*>(chunk)->cy;
        return WXL_TRUE;
    }

    // wxl.terrain's layers: a render chunk of the test's, the client's vertex constants, and what the
    // runtime sends the device through the engine (constant rows, texture stage matrices).
    struct FakeRenderChunk
    {
        uint32_t layers = 0;
        uint32_t render_flags = 0;
        uint32_t flags[4] = {};
        uint32_t textures[4] = {};
        char     batch = 0;
        const void* map_chunk = nullptr;
        const void* map_chunk2 = nullptr;
        uint32_t    pair = 0;
    };
    float                                           g_terrainConsts[37 * 4];
    std::vector<std::pair<uint32_t, std::vector<float>>> g_rowsSent;
    float                                           g_stage[11][16];
    std::vector<std::pair<uint32_t, std::vector<float>>> g_stagesSet;
    std::vector<uint8_t>                            g_areaFile(64, 0);

    wxl_bool WXL_CALL FakeRenderChunkRead(const void* chunk, wxl_core_render_chunk* out)
    {
        if (!chunk || !out) return WXL_FALSE;
        const auto* c = static_cast<const FakeRenderChunk*>(chunk);
        *out              = wxl_core_render_chunk{};
        out->layers       = c->layers;
        out->render_flags = c->render_flags;
        for (uint32_t i = 0; i < 4; ++i)
        {
            out->layer_flags[i]   = c->flags[i];
            out->layer_texture[i] = c->textures[i];
        }
        out->batch      = &c->batch;
        out->map_chunk  = c->map_chunk;
        out->map_chunk2 = c->map_chunk2;
        out->pair       = c->pair;
        return WXL_TRUE;
    }
    wxl_bool WXL_CALL FakeAreaFile(const void* area, wxl_core_area_file* out)
    {
        if (!area || !out) return WXL_FALSE;
        out->tile_x = 32;
        out->tile_y = 48;
        out->data   = g_areaFile.data();
        out->size   = static_cast<uint32_t>(g_areaFile.size());
        return WXL_TRUE;
    }
    const float* WXL_CALL FakeTerrainConsts() { return g_terrainConsts; }
    void WXL_CALL FakeVertexConstantsSet(uint32_t first, const float* rows, uint32_t count)
    {
        g_rowsSent.push_back({first, std::vector<float>(rows, rows + 4 * count)});
    }
    wxl_bool WXL_CALL FakeXformGet(uint32_t xform, float* m)
    {
        if (xform > 10 || !m) return WXL_FALSE;
        std::memcpy(m, g_stage[xform], sizeof g_stage[xform]);
        return WXL_TRUE;
    }
    std::vector<std::pair<uint32_t, std::vector<float>>> g_pixelRowsSent;
    void* g_stageTexture[16] = {};
    int   g_texturesWrapped  = 0;
    void WXL_CALL FakePixelConstantsSet(uint32_t first, const float* rows, uint32_t count)
    {
        g_pixelRowsSent.push_back({first, std::vector<float>(rows, rows + 4 * count)});
    }
    void* WXL_CALL FakeTextureStage(uint32_t stage) { return stage < 16 ? g_stageTexture[stage] : nullptr; }
    void WXL_CALL FakeTextureStageSet(uint32_t stage, void* texture)
    {
        if (stage < 16) g_stageTexture[stage] = texture;
    }
    void WXL_CALL FakeTextureWrap(void*) { ++g_texturesWrapped; }

    // Render states and the device's draws, for a chunk's second draw.
    int32_t g_renderState[100] = {};
    struct FakeDraw
    {
        const void* batch;
        int32_t     blending, depthWrite;
        void*       stage[16];
        std::vector<float> pixelRows; ///< the last pixel rows sent before it
        std::vector<std::pair<uint32_t, std::vector<float>>> vertexRows;
    };
    std::vector<FakeDraw> g_draws;
    int32_t WXL_CALL FakeRenderState(uint32_t which) { return which < 100 ? g_renderState[which] : 0; }
    void WXL_CALL FakeRenderStateSet(uint32_t which, int32_t value)
    {
        if (which < 100) g_renderState[which] = value;
    }
    void WXL_CALL FakeGxDraw(const void* batch, int32_t)
    {
        FakeDraw d{batch, g_renderState[6], g_renderState[15], {}, {}, g_rowsSent};
        std::memcpy(d.stage, g_stageTexture, sizeof d.stage);
        if (!g_pixelRowsSent.empty()) d.pixelRows = g_pixelRowsSent.back().second;
        g_draws.push_back(d);
        // The device's own draw seams fire for it, as they do on every device.
        wxl::seam::Gx::D3dDraw::Before(nullptr, batch, 1);
        wxl::seam::Gx::D3dDraw::After(nullptr, batch, 1);
    }
    // The map's WDT as the client loaded it.
    uint32_t g_wdtFlags = 0;
    int      g_shadersValidated = 0;
    wxl_bool WXL_CALL FakeMapWdt(wxl_core_wdt* out)
    {
        out->path  = "World\\Maps\\Missing\\Missing.wdt";
        out->flags = &g_wdtFlags;
        return WXL_TRUE;
    }
    void WXL_CALL FakeValidateShaders() { ++g_shadersValidated; }

    // A program's record (CGxShader), as the D3D9 device makes it.
    struct FakeShader
    {
        const char* name;
        int32_t     target;
        const void* code;
        uint32_t    size;
    };
    wxl_bool WXL_CALL FakeShaderRecord(const void* shader, wxl_core_shader_record* out)
    {
        if (!shader || !out) return WXL_FALSE;
        const auto* f = static_cast<const FakeShader*>(shader);
        *out = wxl_core_shader_record{f->name, f->target, f->code, f->size};
        return WXL_TRUE;
    }
    void WXL_CALL FakeShaderCodeSet(void* shader, const void* code, uint32_t size)
    {
        auto* f = static_cast<FakeShader*>(shader);
        f->code = code;
        f->size = size;
    }
    void WXL_CALL FakeXformSet(uint32_t xform, const float* m)
    {
        if (xform > 10 || !m) return;
        std::memcpy(g_stage[xform], m, sizeof g_stage[xform]);
        g_stagesSet.push_back({xform, std::vector<float>(m, m + 16)});
    }

    wxl_bool WXL_CALL FakeTerrainView(void* chunk, wxl_core_terrain_chunk* out)
    {
        FakeChunk* c = static_cast<FakeChunk*>(chunk);
        if (!IsGroundChunk(chunk) || !out || GroundChunk(c->cx, c->cy) != c) return WXL_FALSE;
        out->chunk_x = c->cx;
        out->chunk_y = c->cy;
        out->heights = c->heights;
        out->normals = c->normals;
        out->base_z  = c->base;
        out->holes   = c->holes;
        return WXL_TRUE;
    }
    void WXL_CALL FakeTerrainRebuild(void* chunk, wxl_bool doodads)
    {
        FakeChunk* c = static_cast<FakeChunk*>(chunk);
        ++c->rebuilds;
        if (doodads) ++c->clutterFreed;
    }
    void WXL_CALL FakeForgetCollision(void) { ++g_groundForgets; }
    void WXL_CALL FakeFallLocal(const void*) { ++g_groundFalls; }

    /// A roof over the ground for the lift's traces, at this height; 0 for none.
    float g_roofZ = 0.0f;

    /// A terrain click and a sprite click, as the GameUI rows read them.
    struct FakeTerrainClick
    {
        uint64_t guid   = 0;
        wxl_vec3 point{};
        uint32_t button = 0;
    };

    struct FakeSpriteClick
    {
        uint64_t guid   = 0;
        uint32_t button = 0;
    };

    /// A console variable, as the ConsoleVar rows read one.
    struct FakeCVar
    {
        const char* name         = nullptr;
        const char* value        = nullptr;
        uint32_t    flags        = 0;
        const char* defaultValue = nullptr;
        const char* latched      = nullptr;
    };

    /// The glue screen the fake client shows.
    char g_glueScreen[64] = {};

    /// A visible item as CGPlayer_C::UpdateInventoryComponent is handed one.
    struct FakeVisibleItem
    {
        int32_t itemId = 0;
    };

    struct FakeArea
    {
        int32_t tileX = 0;
        int32_t tileY = 0;
    };

    int32_t     g_mapId       = -1;
    uint64_t    g_targetGuid  = 0;
    FakeObject* g_localPlayer = nullptr;

    const FakeObject* Obj(const void* object) { return static_cast<const FakeObject*>(object); }

    int32_t WXL_CALL FakeMapId(void) { return g_mapId; }
    const void* WXL_CALL FakeLocalPlayer(void) { return g_localPlayer; }

    /// What the fake client's object manager holds, announced or not: object_by_guid's table.
    std::vector<FakeObject*> g_held;

    const void* WXL_CALL FakeObjectByGuid(uint64_t guid);
    uint64_t WXL_CALL FakeTargetGuid(void) { return g_targetGuid; }

    uint64_t WXL_CALL FakeGuid(const void* object) { return Obj(object)->guid; }

    const void* WXL_CALL FakeObjectByGuid(uint64_t guid)
    {
        for (FakeObject* object : g_held)
            if (object->guid == guid) return object;
        return nullptr;
    }
    uint32_t WXL_CALL FakeTypeId(const void* object) { return Obj(object)->typeId; }
    uint32_t WXL_CALL FakeEntry(const void* object) { return Obj(object)->entry; }
    int32_t WXL_CALL FakeHealth(const void* object) { return Obj(object)->health; }

    // The animation rows. The fake model has no EmoteWorkNoSheathe (136): it falls back to
    // EmoteWork (62) unless the id is taken as given; every animation lasts a second.
    void WXL_CALL FakePlayAnimation(void* object, uint32_t anim, uint32_t flags)
    {
        FakeObject* o = static_cast<FakeObject*>(object);
        ++o->plays;
        o->lastPlayFlags = flags;
        if (o->refusePlay || o->modelLoading) return;
        o->shownAnim = (anim == 136 && !(flags & 0x4)) ? 62u : anim;
        o->animSpeed = 1.0f;
    }
    void WXL_CALL FakeUpdateAnimation(void* object)
    {
        FakeObject* o = static_cast<FakeObject*>(object);
        ++o->updates;
        o->shownAnim = 0;
    }
    uint32_t WXL_CALL FakeShownAnimation(const void* object)
    {
        return Obj(object)->modelLoading ? 0xFFFFFFFFu : Obj(object)->shownAnim;
    }
    void WXL_CALL FakeAnimationSpeed(void* object, uint32_t anim, float speed)
    {
        FakeObject* o = static_cast<FakeObject*>(object);
        if (o->shownAnim == anim) o->animSpeed = speed;
    }
    uint32_t WXL_CALL FakeAnimationLength(const void*, uint32_t) { return 1000; }
    void WXL_CALL FakeLiftLocal(void* object, float z)
    {
        FakeObject* o = static_cast<FakeObject*>(object);
        o->position.z = z;
        ++o->lifts;
    }
    uint32_t WXL_CALL FakeCastingSpellId(const void* object) { return Obj(object)->castingSpellId; }
    uint32_t WXL_CALL FakeChannelSpellId(const void* object) { return Obj(object)->channelSpellId; }

    wxl_bool WXL_CALL FakePosition(const void* object, wxl_vec3* out)
    {
        if (Obj(object)->typeId == WXL_OBJECT_ITEM) return WXL_FALSE;
        *out = Obj(object)->position;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeFacing(const void* object, float* out)
    {
        if (Obj(object)->typeId == WXL_OBJECT_ITEM) return WXL_FALSE;
        *out = Obj(object)->facing;
        return WXL_TRUE;
    }

    uint32_t WXL_CALL FakeMovementFlags(const void* object) { return Obj(object)->movementFlags; }
    uint32_t WXL_CALL FakeMoney(const void* object) { return Obj(object)->money; }

    const FakeTerrainClick* Terrain(const void* evt) { return static_cast<const FakeTerrainClick*>(evt); }
    uint64_t WXL_CALL FakeTerrainGuid(const void* evt) { return Terrain(evt)->guid; }
    float WXL_CALL FakeTerrainX(const void* evt) { return Terrain(evt)->point.x; }
    float WXL_CALL FakeTerrainY(const void* evt) { return Terrain(evt)->point.y; }
    float WXL_CALL FakeTerrainZ(const void* evt) { return Terrain(evt)->point.z; }
    uint32_t WXL_CALL FakeTerrainButton(const void* evt) { return Terrain(evt)->button; }

    const FakeSpriteClick* Sprite(const void* evt) { return static_cast<const FakeSpriteClick*>(evt); }
    uint64_t WXL_CALL FakeSpriteGuid(const void* evt) { return Sprite(evt)->guid; }
    uint32_t WXL_CALL FakeSpriteButton(const void* evt) { return Sprite(evt)->button; }

    const FakeCVar* CVarOf(const void* cvar) { return static_cast<const FakeCVar*>(cvar); }
    const char* WXL_CALL FakeCVarName(const void* cvar) { return CVarOf(cvar)->name; }
    const char* WXL_CALL FakeCVarValue(const void* cvar) { return CVarOf(cvar)->value; }
    uint32_t WXL_CALL FakeCVarFlags(const void* cvar) { return CVarOf(cvar)->flags; }
    const char* WXL_CALL FakeCVarDefault(const void* cvar) { return CVarOf(cvar)->defaultValue; }
    const char* WXL_CALL FakeCVarLatched(const void* cvar) { return CVarOf(cvar)->latched; }

    // The client's variable table, as ConsoleVar.cpp of the fork keeps it: a hash without case,
    // CVar::Register, Set and InternalSet with the callback and the latch, the console's `set`
    // as Config.wtf's lines reach it, and the deletion of a category.
    struct FakeTableCVar : FakeCVar
    {
        std::string key, text, fallback, reset, pending;
        bool        hasText = false, hasDefault = false, hasReset = false, hasPending = false;
        void*       callback = nullptr;
        void*       arg      = nullptr;
        void Sync()
        {
            name         = key.c_str();
            value        = hasText ? text.c_str() : nullptr;
            defaultValue = hasDefault ? fallback.c_str() : nullptr;
            latched      = hasPending ? pending.c_str() : nullptr;
        }
    };
    using FakeCVarCallback = bool (*)(void*, const char*, const char*, void*);
    std::map<std::string, std::unique_ptr<FakeTableCVar>> g_cvars;
    std::vector<std::string>                              g_cvarCommands; // ConsoleCommandRegister's names
    std::map<std::string, const char*>                    g_cvarHelp;     // kept by pointer, as the console does

    std::string FakeCVarKey(const char* name)
    {
        std::string key(name ? name : "");
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return key;
    }

    FakeTableCVar* FakeFindCVar(const char* name)
    {
        const auto found = g_cvars.find(FakeCVarKey(name));
        return found == g_cvars.end() ? nullptr : found->second.get();
    }

    bool FakeSameText(const std::string& a, const char* b)
    {
        return FakeCVarKey(a.c_str()) == FakeCVarKey(b);
    }

    void FakeInternalSet(FakeTableCVar& var, const char* text, bool setValue, bool setReset, bool setDefault)
    {
        wxl::seam::ConsoleVar::InternalSet::Before(&var, text, setValue, setReset, setDefault, 1);
        if ((var.flags & 0x4) || !text) return;
        if (setValue && (!var.hasText || !FakeSameText(var.text, text)))
        {
            var.text    = text;
            var.hasText = true;
        }
        if (setReset && !var.hasReset)
        {
            var.reset    = text;
            var.hasReset = true;
        }
        if (setDefault && !var.hasDefault)
        {
            var.fallback   = text;
            var.hasDefault = true;
        }
        var.Sync();
    }

    void FakeSet(FakeTableCVar& var, const char* text, bool setValue, bool setReset, bool setDefault)
    {
        if (setValue)
        {
            if (var.callback && !reinterpret_cast<FakeCVarCallback>(var.callback)(&var, var.value, text, var.arg)) return;
            if (var.flags & 0x2)
            {
                var.pending    = text;
                var.hasPending = true;
                var.Sync();
                return;
            }
        }
        FakeInternalSet(var, text, setValue, setReset, setDefault);
    }

    FakeTableCVar* FakeRegisterFull(const char* name, const char* help, uint32_t flags, const char* text, void* callback,
                                    bool setCommand, bool unk)
    {
        if (!name || !*name || !text) return nullptr;
        if (FakeTableCVar* var = FakeFindCVar(name))
        {
            const bool setReset   = !var->hasReset;
            var->flags            = flags | (var->flags & ~0x30u);
            const bool setDefault = !var->hasDefault;
            var->callback         = callback;
            bool setValue         = false;
            if (callback && !reinterpret_cast<FakeCVarCallback>(callback)(var, var->value, var->value, nullptr)) setValue = true;
            FakeSet(*var, text, setValue, setReset, setDefault);
            if (!setCommand) var->flags |= 0x80000000u;
            if (unk && !(var->flags & 0x80000000u)) var->flags |= 0x80u;
            return var;
        }
        auto made     = std::make_unique<FakeTableCVar>();
        made->key     = name;
        made->callback = callback;
        made->Sync();
        FakeTableCVar* var = made.get();
        g_cvars[FakeCVarKey(name)] = std::move(made);
        if (setCommand)
            FakeSet(*var, text, true, true, false);
        else
            FakeSet(*var, text, true, false, true);
        var->flags = flags | 0x1u;
        if (!setCommand) var->flags |= 0x80000000u;
        if (unk && !(var->flags & 0x80000000u)) var->flags |= 0x80u;
        if (std::find(g_cvarCommands.begin(), g_cvarCommands.end(), FakeCVarKey(name)) == g_cvarCommands.end())
        {
            g_cvarCommands.push_back(FakeCVarKey(name));
            g_cvarHelp[FakeCVarKey(name)] = help;
        }
        return var;
    }

    /// A `SET name "value"` line of Config.wtf, as the stock client's SetCommandHandler takes it:
    /// Set and InternalSet inlined, a name nobody has registered made archived with that value.
    void FakeConfigLine(const char* name, const char* text)
    {
        FakeTableCVar* var = FakeFindCVar(name);
        if (!var)
        {
            FakeRegisterFull(name, "", 0, text, nullptr, true, false);
            return;
        }
        if (var->callback && !reinterpret_cast<FakeCVarCallback>(var->callback)(var, var->value, text, var->arg)) return;
        if (var->flags & 0x2)
        {
            var->pending    = text;
            var->hasPending = true;
            var->Sync();
            return;
        }
        if (var->flags & 0x4) return;
        if (var->hasText && FakeSameText(var->text, text)) return;
        var->text    = text;
        var->hasText = true;
        var->Sync();
    }

    void FakeDeleteCategory(uint32_t category)
    {
        for (auto it = g_cvars.begin(); it != g_cvars.end();)
            if ((it->second->flags & 0x1) && (it->second->flags & 0x30) == category)
                it = g_cvars.erase(it);
            else
                ++it;
    }

    void* WXL_CALL FakeCVarLookup(const char* name) { return FakeFindCVar(name); }
    void* WXL_CALL FakeCVarRegister(const char* name, const char* help, uint32_t flags, const char* text, void* callback,
                                    uint32_t)
    {
        return FakeRegisterFull(name, help, flags, text, callback, false, false);
    }
    wxl_bool WXL_CALL FakeCVarSet(void* cvar, const char* text)
    {
        if (!cvar || !text) return WXL_FALSE;
        FakeSet(*static_cast<FakeTableCVar*>(cvar), text, true, false, false);
        return WXL_TRUE;
    }
    void WXL_CALL FakeCVarWriteFlags(void* cvar, uint32_t flags)
    {
        if (cvar) static_cast<FakeCVar*>(cvar)->flags = flags;
    }

    // The client's signature check as the fake client answers it: the verdict to give, how many
    // times it was asked, and whether the gate at its top let the runtime's own call through.
    int32_t  g_sigVerdict     = 2;
    uint32_t g_sigChecks      = 0;
    uint32_t g_sigDigests     = 0;
    bool     g_sigLetThrough  = false;

    /// What the client's check does once the gate at its top has let it run: the digest written
    /// for a verdict that gets that far (modified or signed).
    int32_t FakeJudge(uint8_t* digest)
    {
        ++g_sigChecks;
        if (g_sigVerdict >= WXL_INTERFACE_SIGNATURE_MODIFIED && digest) std::memset(digest, 0xAB, 16);
        return g_sigVerdict;
    }

    /// The client's FrameXML_CheckSignature as the fork has it: the gate first, then the check.
    int32_t (*g_sigClient)(const char*, const char*, const uint8_t*, uint8_t*) =
        [](const char* toc, const char* extra, const uint8_t* modulus, uint8_t* digest) -> int32_t
    {
        int32_t  answer = -2;
        uint8_t* out    = digest;
        g_sigLetThrough = !wxl::seam::FrameXML::CheckSignature::Gate(&answer, &toc, &extra, &modulus, &out);
        return g_sigLetThrough ? FakeJudge(digest) : answer;
    };

    int32_t WXL_CALL FakeCheckSignature(const char* toc, const char* extra, const uint8_t* modulus, uint8_t* digest)
    {
        return g_sigClient(toc, extra, modulus, digest);
    }

    wxl_bool WXL_CALL FakeSignatureDigest(const char* toc, const char*, uint8_t* digest)
    {
        if (!toc || !digest) return WXL_FALSE;
        ++g_sigDigests;
        std::memset(digest, 0xCD, 16);
        return WXL_TRUE;
    }

    const char* WXL_CALL FakeGlueScreen(void) { return g_glueScreen; }

    /// A login object as the fake client keeps it: what its GetLoginServerType answers.
    struct FakeLogin
    {
        int32_t serverType = 0;
    };

    int32_t WXL_CALL FakeLoginServerType(const void* login)
    {
        return login ? static_cast<const FakeLogin*>(login)->serverType : -1;
    }

    /// A line the chat holds, as PENDINGCHAT keeps it: the copy of its text.
    struct FakePendingChat
    {
        const char* text = nullptr;
    };

    /// The tail of the chat's held lines: the one a test just queued.
    const FakePendingChat* g_pendingLast = nullptr;

    const void* WXL_CALL FakePendingLast() { return g_pendingLast; }

    const char* WXL_CALL FakePendingText(const void* line)
    {
        return line ? static_cast<const FakePendingChat*>(line)->text : nullptr;
    }

    wxl_bool WXL_CALL FakeVisibleItemId(const void* object, uint32_t slot, int32_t* out)
    {
        if (slot >= 19) return WXL_FALSE;
        *out = Obj(object)->visibleItems[slot];
        return WXL_TRUE;
    }

    int32_t WXL_CALL FakeVisibleItemIdOf(const void* item)
    {
        return static_cast<const FakeVisibleItem*>(item)->itemId;
    }

    int32_t WXL_CALL FakeTileX(const void* area) { return static_cast<const FakeArea*>(area)->tileX; }
    int32_t WXL_CALL FakeTileY(const void* area) { return static_cast<const FakeArea*>(area)->tileY; }

    // --- A fake script engine: a value stack, globals, an event table, as the script rows see them

    struct FakeValue
    {
        uint32_t    type = WXL_VALUE_NIL;
        double      number = 0.0;
        std::string string;
        bool        boolean = false;
    };

    /// A global the runtime set: the C closure and the slot in its upvalue.
    struct FakeClosure
    {
        wxl_script_native_fn fn   = nullptr;
        uint32_t             slot = 0;
    };

    struct FakeSignal
    {
        uint32_t               index = 0;
        std::vector<FakeValue> args;
    };

    struct FakeVm
    {
        bool                                  live = false;
        std::vector<FakeValue>                stack;
        std::map<std::string, FakeClosure>    closures;      ///< globals the runtime set
        std::set<std::string>                 clientGlobals; ///< globals the client set
        uint32_t                              runningSlot = 0;
        std::vector<std::string>              errors;
        std::vector<std::string>              ran;
        std::vector<std::vector<std::string>> eventTables; ///< every table installed, in order
        std::vector<FakeSignal>               signals;
        uint32_t                              calls = 0; ///< calls running, as L->ci counts them
    };

    FakeVm  g_vm;
    int32_t g_uiInitialized = 0;
    uint8_t g_uiLoggingIn   = 0;

    FakeVm* Vm(void* state) { return static_cast<FakeVm*>(state); }

    /// The stack slot a positive Lua index names, or null.
    const FakeValue* At(void* state, int32_t index)
    {
        FakeVm* vm = Vm(state);
        if (!vm || index <= 0 || static_cast<size_t>(index) > vm->stack.size()) return nullptr;
        return &vm->stack[static_cast<size_t>(index) - 1];
    }

    void* WXL_CALL FakeScriptState(void) { return g_vm.live ? &g_vm : nullptr; }
    int32_t WXL_CALL FakeUiInitialized(void) { return g_uiInitialized; }
    uint8_t WXL_CALL FakeUiLoggingIn(void) { return g_uiLoggingIn; }
    uint8_t g_uiReloading = 0;
    uint8_t WXL_CALL FakeUiReloading(void) { return g_uiReloading; }

    /// World::s_currentWorldPos, answered while a map is loaded.
    wxl_vec3 g_worldPosition{};
    wxl_bool WXL_CALL FakeWorldPosition(wxl_vec3* out)
    {
        if (g_mapId < 0) return WXL_FALSE;
        *out = g_worldPosition;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeGlobalIsSet(void* state, const char* name)
    {
        return Vm(state)->closures.count(name) || Vm(state)->clientGlobals.count(name) ? WXL_TRUE
                                                                                      : WXL_FALSE;
    }

    void WXL_CALL FakeSetGlobalClosure(void* state, const char* name, wxl_script_native_fn fn, uint32_t slot)
    {
        Vm(state)->closures[name] = FakeClosure{fn, slot};
    }

    void WXL_CALL FakeClearGlobal(void* state, const char* name) { Vm(state)->closures.erase(name); }
    uint32_t WXL_CALL FakeClosureSlot(void* state) { return Vm(state)->runningSlot; }
    int32_t WXL_CALL FakeTop(void* state) { return static_cast<int32_t>(Vm(state)->stack.size()); }

    uint32_t WXL_CALL FakeValueType(void* state, int32_t index)
    {
        const FakeValue* value = At(state, index);
        return value ? value->type : WXL_VALUE_NIL;
    }

    double WXL_CALL FakeToNumber(void* state, int32_t index)
    {
        const FakeValue* value = At(state, index);
        return value ? value->number : 0.0;
    }

    const char* WXL_CALL FakeToString(void* state, int32_t index, uint32_t* length)
    {
        const FakeValue* value = At(state, index);
        if (!value) return nullptr;
        // A number reads as its text, as lua_tolstring converts one in place.
        if (value->type == WXL_VALUE_NUMBER)
        {
            static std::string rendered;
            char               text[32];
            snprintf(text, sizeof text, "%.14g", value->number);
            rendered = text;
            if (length) *length = static_cast<uint32_t>(rendered.size());
            return rendered.c_str();
        }
        if (length) *length = static_cast<uint32_t>(value->string.size());
        return value->string.c_str();
    }

    wxl_bool WXL_CALL FakeToBool(void* state, int32_t index)
    {
        const FakeValue* value = At(state, index);
        return value && value->boolean ? WXL_TRUE : WXL_FALSE;
    }

    void WXL_CALL FakePushNumber(void* state, double number)
    {
        FakeValue value;
        value.type   = WXL_VALUE_NUMBER;
        value.number = number;
        Vm(state)->stack.push_back(value);
    }

    void WXL_CALL FakePushString(void* state, const char* text, uint32_t length)
    {
        FakeValue value;
        value.type   = WXL_VALUE_STRING;
        value.string = std::string(text, length);
        Vm(state)->stack.push_back(value);
    }

    void WXL_CALL FakePushBool(void* state, wxl_bool flag)
    {
        FakeValue value;
        value.type    = WXL_VALUE_BOOL;
        value.boolean = flag != WXL_FALSE;
        Vm(state)->stack.push_back(value);
    }

    void WXL_CALL FakePushNil(void* state) { Vm(state)->stack.push_back(FakeValue{}); }

    void WXL_CALL FakePop(void* state, int32_t count)
    {
        std::vector<FakeValue>& stack = Vm(state)->stack;
        const size_t            n     = count > 0 ? static_cast<size_t>(count) : 0;
        stack.resize(stack.size() > n ? stack.size() - n : 0);
    }

    /// The fake records the error and returns, where the client's would not; the thunk is
    /// written so that it returns nothing to the caller either way.
    void WXL_CALL FakeError(void* state, const char* message) { Vm(state)->errors.push_back(message); }

    void WXL_CALL FakeRun(const char* source, const char* name)
    {
        g_vm.ran.push_back(std::string(name) + ": " + source);
        const std::string chunk(source);
        // "leak:" leaves a value on the stack, as a C function of the client that pushes and
        // returns outside a call would.
        if (chunk.rfind("leak:", 0) == 0) FakePushNumber(&g_vm, 1.0);
#if defined(_MSC_VER)
        // "fault:" stops inside a call of the chunk, as GetBindingKey does before the client has
        // loaded its key bindings (CGUIBindings::s_bindings null): the protected call, the chunk
        // and the function stay open with their values on the stack.
        if (chunk.rfind("fault:", 0) == 0)
        {
            g_vm.calls += 2;
            FakePushString(&g_vm, "error handler", 13);
            FakePushString(&g_vm, "chunk", 5);
            FakePushString(&g_vm, "TURNLEFT", 8);
            volatile int* nowhere = nullptr;
            *nowhere              = 1;
        }
#endif
    }

    void WXL_CALL FakeMark(void* state, wxl_script_mark* mark)
    {
        *mark          = wxl_script_mark{};
        mark->words[0] = Vm(state)->stack.size();
        mark->words[1] = Vm(state)->calls;
    }

    wxl_bool WXL_CALL FakeUnwind(void* state, const wxl_script_mark* mark)
    {
        if (Vm(state)->calls == mark->words[1]) return WXL_FALSE;
        Vm(state)->calls = static_cast<uint32_t>(mark->words[1]);
        Vm(state)->stack.resize(mark->words[0]);
        return WXL_TRUE;
    }

    void WXL_CALL FakeCreateEvents(const char** names, uint32_t count)
    {
        g_vm.eventTables.push_back(std::vector<std::string>(names, names + count));
    }

    void WXL_CALL FakeSignalEvent(uint32_t index, void* state, int32_t argCount)
    {
        FakeSignal signal;
        signal.index                  = index;
        std::vector<FakeValue>& stack = Vm(state)->stack;
        const size_t            n     = argCount > 0 ? static_cast<size_t>(argCount) : 0;
        signal.args.assign(stack.end() - static_cast<std::ptrdiff_t>(n), stack.end());
        Vm(state)->signals.push_back(signal);
    }

    /// Calls a global the runtime set, as the client's Lua would: the arguments on the stack,
    /// the closure's slot in its upvalue, the results being whatever the function left on top.
    std::vector<FakeValue> CallGlobal(const char* name, std::vector<FakeValue> args, bool* found)
    {
        const auto it = g_vm.closures.find(name);
        *found         = it != g_vm.closures.end();
        if (!*found) return {};
        g_vm.stack       = std::move(args);
        g_vm.runningSlot = it->second.slot;
        const int32_t pushed = it->second.fn(&g_vm);
        const size_t  n      = pushed > 0 ? static_cast<size_t>(pushed) : 0;
        std::vector<FakeValue> results(g_vm.stack.end() - static_cast<std::ptrdiff_t>(n), g_vm.stack.end());
        g_vm.stack.clear();
        return results;
    }

    FakeValue Number(double number)
    {
        FakeValue value;
        value.type   = WXL_VALUE_NUMBER;
        value.number = number;
        return value;
    }

    FakeValue String(const char* text)
    {
        FakeValue value;
        value.type   = WXL_VALUE_STRING;
        value.string = text;
        return value;
    }

    /// The accessors the fake client hands over: what the seams and the checks below read.
    /// A message as the client holds one: a buffer, its end, and where reading starts.
    struct FakeStore
    {
        const uint8_t* data = nullptr;
        uint32_t       size = 0;
        uint32_t       read = 0;
    };

    const void* WXL_CALL FakeMessageData(const void* store)
    {
        return store ? static_cast<const FakeStore*>(store)->data : nullptr;
    }

    uint32_t WXL_CALL FakeMessageSize(const void* store)
    {
        return store ? static_cast<const FakeStore*>(store)->size : 0u;
    }

    uint32_t WXL_CALL FakeMessageRead(const void* store)
    {
        return store ? static_cast<const FakeStore*>(store)->read : 0u;
    }

    /// What the fake client sent through net_send: the opcode as a u32 and the body.
    std::vector<std::vector<uint8_t>>& NetSent()
    {
        static std::vector<std::vector<uint8_t>> sent;
        return sent;
    }

    wxl_bool WXL_CALL FakeNetSend(const void* message, uint32_t size)
    {
        const uint8_t* at = static_cast<const uint8_t*>(message);
        NetSent().emplace_back(at, at + size);
        return WXL_TRUE;
    }

    /// A pending cast, as the spell rows read one.
    struct FakePendingCast
    {
        uint64_t caster  = 0;
        uint32_t spellId = 0;
        uint8_t  count   = 0;
    };

    uint64_t WXL_CALL FakePendingCaster(const void* cast)
    {
        return static_cast<const FakePendingCast*>(cast)->caster;
    }

    uint32_t WXL_CALL FakePendingSpellId(const void* cast)
    {
        return static_cast<const FakePendingCast*>(cast)->spellId;
    }

    uint8_t WXL_CALL FakePendingCount(const void* cast)
    {
        return static_cast<const FakePendingCast*>(cast)->count;
    }

    /// One file the fake client holds, and the cursor a reader is at.
    struct FakeFile
    {
        std::string name;
        std::string bytes;
        uint64_t    at    = 0;
        bool        open  = false;
    };

    std::vector<FakeFile> g_files;
    int                   g_opens  = 0;
    int                   g_closes = 0;

    FakeFile* FileNamed(const char* name)
    {
        for (FakeFile& file : g_files)
            if (file.name == name) return &file;
        return nullptr;
    }

    std::string g_basePath; ///< the client's folder, as SFile::m_basePath holds it

    // The client's file stack, as Blizzard::File dispatches through it: one node whose verbs
    // know no file of their own, which the runtime's file layer writes its verbs into.
    namespace fsk = wxl::core::filestack;

    bool FakeVerbFails(fsk::Filesystem*, fsk::FileParms*) { return false; }

    fsk::Filesystem  g_fakeInitial{};
    fsk::Filesystem* g_fakeNode = nullptr;
    void*            g_fakeTop  = &g_fakeInitial;

    /// Builds the stack as file_init does: a node of its own on top.
    void BuildFakeStack()
    {
        static fsk::Filesystem node;
        node = fsk::Filesystem{};
        fsk::Verb* verbs = &node.verbs.cd;
        for (size_t i = 0; i < sizeof node.verbs / sizeof(fsk::Verb); ++i) verbs[i] = &FakeVerbFails;
        g_fakeNode = &node;
        g_fakeTop  = &node;
        wxl::core::services::assets::ForgetInstall();
    }

    void** WXL_CALL FakeFilestackTop() { return &g_fakeTop; }
    const void* WXL_CALL FakeFilestackInitial() { return &g_fakeInitial; }

    fsk::Verb TopVerb(uintptr_t offset) { return fsk::VerbAt(static_cast<fsk::Filesystem*>(g_fakeTop), offset); }

    /// An open file of the fake client: one it holds, one of the disk, or one of the file stack.
    struct FakeHandle
    {
        FakeFile*          file = nullptr; ///< a file it holds, or one read off the disk
        std::unique_ptr<FakeFile> disk;
        fsk::StreamRecord* stream = nullptr;
    };

    /// The record the fake's archive files are found in: what FindFile answers through `archive`.
    char g_fakeRecord = 0;

    /// FindFile as the client runs it: the gate first, then a name behind two backslashes off
    /// the disk at the base path, else the files it holds (in an archive, `g_fakeRecord`), then
    /// the observing half. An open asks the archive back, an existence check does not. Answers
    /// the path to open, and whether that path is the file stack's.
    bool FakeFind(const char* name, std::string& path, bool& stacked, bool open = true)
    {
        char        buffer[260] = {};
        char*       out         = buffer;
        uint32_t    size        = sizeof buffer;
        uint32_t    flags       = 0;
        uint32_t    type        = 3;
        uint32_t*   typeAt      = &type;
        void*       archive     = nullptr;
        void**      archiveAt   = open ? &archive : nullptr;
        int32_t     answer      = 0;
        const char* asked       = name;
        if (wxl::seam::Storage::FindFile::Gate(&answer, &asked, &out, &size, &flags, &typeAt, &archiveAt))
        {
            if (!answer) return false;
            path    = buffer;
            stacked = true;
            return true;
        }
        stacked    = false;
        bool found = false;
        if (asked[0] == '\\' && asked[1] == '\\')
        {
            path = g_basePath + asked;
            const DWORD attributes = ::GetFileAttributesA(path.c_str());
            found = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
            type  = 0;
            std::snprintf(buffer, sizeof buffer, "%s", asked);
        }
        else
        {
            path  = asked;
            found = FileNamed(asked) != nullptr;
            if (found && archiveAt) *archiveAt = &g_fakeRecord;
        }
        wxl::seam::Storage::FindFileResult::After(found ? 1 : 0, asked, buffer, size, flags, &type, archiveAt);
        return found;
    }

    wxl_bool WXL_CALL FakeArchiveRecordOf(void* record, wxl_core_archive* out)
    {
        if (record != &g_fakeRecord || !out) return WXL_FALSE;
        *out      = wxl_core_archive{};
        out->path = "Data\\fake.MPQ";
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeFileExists(const char* path)
    {
        std::string found;
        bool        stacked = false;
        return path && FakeFind(path, found, stacked, false) ? WXL_TRUE : WXL_FALSE;
    }

    /// SFileOpenFile: SFile::OpenEx's gate, FindFile, then the open of what it found.
    void* WXL_CALL FakeFileOpen(const char* path)
    {
        if (!path) return nullptr;
        void*       archive = nullptr;
        const char* name    = path;
        uint32_t    flags   = 0;
        void**      result  = nullptr;
        int32_t     answer  = 0;
        if (wxl::seam::Storage::OpenFile::Gate(&answer, &archive, &name, &flags, &result)) return nullptr;

        std::string found;
        bool        stacked = false;
        if (!FakeFind(name, found, stacked)) return nullptr;
        auto handle = std::make_unique<FakeHandle>();
        if (stacked)
        {
            fsk::FileParms parms{};
            parms.verb = offsetof(fsk::Filesystem, verbs.open);
            parms.path = found.data();
            parms.mode = 0x1005;
            if (!TopVerb(parms.verb)(static_cast<fsk::Filesystem*>(g_fakeTop), &parms) || !parms.stream) return nullptr;
            handle->stream = parms.stream;
        }
        else if (found.size() > 1 && found.find(":\\") != std::string::npos)
        {
            std::ifstream in(found, std::ios::binary);
            if (!in) return nullptr;
            handle->disk = std::make_unique<FakeFile>();
            handle->disk->name.assign(found);
            handle->disk->bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            handle->file = handle->disk.get();
        }
        else
        {
            handle->file = FileNamed(found.c_str());
            if (!handle->file) return nullptr;
        }
        if (handle->file)
        {
            handle->file->at   = 0;
            handle->file->open = true;
        }
        ++g_opens;
        return handle.release();
    }

    FakeHandle* Handle(void* file) { return static_cast<FakeHandle*>(file); }

    uint64_t WXL_CALL FakeFileSize(void* file)
    {
        if (!file) return 0;
        FakeHandle* handle = Handle(file);
        if (handle->file) return handle->file->bytes.size();
        fsk::FileParms parms{};
        parms.verb   = offsetof(fsk::Filesystem, verbs.getfileinfo);
        parms.stream = handle->stream;
        if (!TopVerb(parms.verb)(static_cast<fsk::Filesystem*>(g_fakeTop), &parms) || !parms.info) return 0;
        return static_cast<uint64_t>(parms.info->size);
    }

    wxl_bool WXL_CALL FakeFileSeek(void* file, uint64_t offset)
    {
        if (!file) return WXL_FALSE;
        FakeHandle* handle = Handle(file);
        if (handle->file)
        {
            handle->file->at = offset;
            return WXL_TRUE;
        }
        fsk::FileParms parms{};
        parms.verb     = offsetof(fsk::Filesystem, verbs.setpos);
        parms.stream   = handle->stream;
        parms.pos      = static_cast<int64_t>(offset);
        parms.pos_mode = 0;
        return TopVerb(parms.verb)(static_cast<fsk::Filesystem*>(g_fakeTop), &parms) ? WXL_TRUE : WXL_FALSE;
    }

    uint32_t WXL_CALL FakeFileRead(void* opened, void* buffer, uint32_t count)
    {
        if (!opened || !buffer) return 0;
        FakeHandle* handle = Handle(opened);
        if (!handle->file)
        {
            fsk::FileParms parms{};
            parms.verb   = offsetof(fsk::Filesystem, verbs.read);
            parms.stream = handle->stream;
            parms.buffer = buffer;
            parms.amount = static_cast<int32_t>(count);
            if (!TopVerb(parms.verb)(static_cast<fsk::Filesystem*>(g_fakeTop), &parms)) return 0;
            return static_cast<uint32_t>(parms.amount);
        }
        FakeFile* file = handle->file;
        if (file->at >= file->bytes.size()) return 0;
        const uint64_t left = file->bytes.size() - file->at;
        const uint32_t got  = left < count ? static_cast<uint32_t>(left) : count;
        memcpy(buffer, file->bytes.data() + file->at, got);
        file->at += got;
        return got;
    }

    void WXL_CALL FakeFileClose(void* opened)
    {
        if (!opened) return;
        FakeHandle* handle = Handle(opened);
        if (handle->file) handle->file->open = false;
        if (handle->stream)
        {
            fsk::FileParms parms{};
            parms.verb   = offsetof(fsk::Filesystem, verbs.close);
            parms.stream = handle->stream;
            TopVerb(parms.verb)(static_cast<fsk::Filesystem*>(g_fakeTop), &parms);
        }
        delete handle;
        ++g_closes;
    }

    wxl_bool WXL_CALL FakeArchiveMount(const char*, int32_t) { return WXL_TRUE; }

    // The client's archives' listfiles, as find sees them.
    std::vector<std::string> g_listed = {"Interface\\FrameXML\\FrameXML.toc",
                                         "Interface\\FrameXML\\Bindings.xml",
                                         "INTERFACE\\Icons\\A.blp", "Interface\\Glues\\Glues.toc"};

    const char* WXL_CALL FakeBasePath() { return g_basePath.c_str(); }

    uint32_t WXL_CALL FakeArchiveList(wxl_core_name_fn fn, void* user)
    {
        for (const std::string& name : g_listed)
            if (!fn(name.c_str(), user)) break;
        return 1;
    }

    // What wxl.footprint reads of the client: a texture of each pool and a buffer, the sound and
    // script heaps, two archives, two databases.
    wxl_core_gpu_resource g_gpu[] = {
        {WXL_CORE_GPU_TEXTURE, 1, 4096}, {WXL_CORE_GPU_TEXTURE, 1, 1024}, {WXL_CORE_GPU_TEXTURE, 0, 8192},
        {WXL_CORE_GPU_BUFFER, 0, 65536},
    };

    uint32_t WXL_CALL FakeGxResources(wxl_core_gpu_fn fn, void* user)
    {
        for (const wxl_core_gpu_resource& resource : g_gpu) fn(&resource, user);
        return static_cast<uint32_t>(sizeof g_gpu / sizeof g_gpu[0]);
    }

    wxl_bool WXL_CALL FakeSoundHeap(uint64_t* current, uint64_t* peak)
    {
        *current = 3000000;
        *peak    = 5000000;
        return WXL_TRUE;
    }

    uint64_t WXL_CALL FakeScriptHeap() { return 12345678; }

    wxl_core_archive g_archives[] = {
        {"Data\\common.MPQ", 0, 65536, 40000, 2000000},
        {"Data\\patch-3.MPQ", 30, 16384, 9000, 500000},
    };

    /// An SArchive as SFile::OpenArchive hands one out: its type, then its record.
    struct FakeSArchive
    {
        uint32_t          type;
        wxl_core_archive* record;
    };

    uint32_t WXL_CALL FakeArchiveTables(wxl_core_archive_fn fn, void* user)
    {
        for (const wxl_core_archive& archive : g_archives) fn(&archive, user);
        return 2;
    }

    wxl_bool WXL_CALL FakeArchiveOf(void* archive, wxl_core_archive* out)
    {
        const FakeSArchive* handle = static_cast<const FakeSArchive*>(archive);
        if (!handle || handle->type != 0 || !handle->record) return WXL_FALSE;
        *out = *handle->record;
        return WXL_TRUE;
    }

    alignas(8) char g_spellRecords[64];
    alignas(8) char g_spellStrings[32];

    uint32_t WXL_CALL FakeClientDbs(wxl_core_db_fn fn, void* user)
    {
        wxl_core_db spell{};
        spell.file         = "DBFilesClient\\Spell.dbc";
        spell.loaded       = WXL_TRUE;
        spell.records      = 2;
        spell.record_size  = 32;
        spell.blocks[0]    = g_spellRecords;
        spell.estimates[0] = 64;
        spell.blocks[1]    = g_spellStrings;
        spell.blocks[2]    = nullptr;
        fn(&spell, user);
        wxl_core_db unread{};
        unread.file = "DBFilesClient\\Map.dbc";
        fn(&unread, user);
        return 2;
    }

    // The graphics device, as the paint and the device's resets see it.
    bool     g_gxContext = true;
    bool     g_gxVisible = true;
    bool     g_gxDevice  = true;
    uint32_t g_gxWidth   = 1920;
    uint32_t g_gxHeight  = 1080;

    wxl_bool WXL_CALL FakeGxDeviceExists() { return g_gxDevice ? WXL_TRUE : WXL_FALSE; }
    int               g_gxInvalidated = 0; ///< how often the runtime told the client its state changed
    void WXL_CALL     FakeGxInvalidate() { ++g_gxInvalidated; }
    wxl_bool WXL_CALL FakeGxHasContext() { return g_gxDevice && g_gxContext ? WXL_TRUE : WXL_FALSE; }
    wxl_bool WXL_CALL FakeGxWindowVisible() { return g_gxDevice && g_gxVisible ? WXL_TRUE : WXL_FALSE; }

    wxl_bool WXL_CALL FakeGxWindowSize(uint32_t* width, uint32_t* height)
    {
        if (!g_gxDevice || !width || !height) return WXL_FALSE;
        *width  = g_gxWidth;
        *height = g_gxHeight;
        return WXL_TRUE;
    }

    // The descriptor blocks and the mirror handlers, as the field rows see them.
    void* WXL_CALL FakeDescriptor(const void* object)
    {
        FakeObject* fake = const_cast<FakeObject*>(Obj(object));
        return fake->block.empty() ? nullptr : fake->block.data();
    }

    /// A mirror handler the runtime set: on a kind, or on one object when `guid` is not 0.
    struct FakeMirror
    {
        uint64_t           guid;
        uint32_t           type;
        uint32_t           offset;
        uint32_t           size;
        wxl_core_mirror_fn handler;
        void*              param;
    };
    std::vector<FakeMirror> g_mirrors;

    wxl_bool WXL_CALL FakeWatchType(uint32_t type, uint32_t offset, uint32_t size, wxl_core_mirror_fn handler,
                                    void* param)
    {
        g_mirrors.push_back(FakeMirror{0, type, offset, size, handler, param});
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeWatchOne(uint64_t guid, uint32_t type, uint32_t offset, uint32_t size,
                                   wxl_core_mirror_fn handler, void* param)
    {
        g_mirrors.push_back(FakeMirror{guid, type, offset, size, handler, param});
        return WXL_TRUE;
    }

    /// Takes a handler back as the client does: the first one set with that function and param.
    wxl_bool Unset(uint64_t guid, uint32_t type, uint32_t offset, wxl_core_mirror_fn handler, void* param)
    {
        for (auto it = g_mirrors.begin(); it != g_mirrors.end(); ++it)
        {
            if (it->guid != guid || it->type != type || it->offset != offset || it->handler != handler ||
                it->param != param)
                continue;
            g_mirrors.erase(it);
            return WXL_TRUE;
        }
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeUnwatchType(uint32_t type, uint32_t offset, wxl_core_mirror_fn handler, void* param)
    {
        return Unset(0, type, offset, handler, param);
    }

    wxl_bool WXL_CALL FakeUnwatchOne(uint64_t guid, uint32_t type, uint32_t offset, wxl_core_mirror_fn handler,
                                     void* param)
    {
        return Unset(guid, type, offset, handler, param);
    }

    /// The changes a mod other than the harness received, by the handler it subscribed.
    std::vector<wxl_object_field_args> g_modChanges;

    void WXL_CALL OnModFieldChanged(void*, const void* args)
    {
        g_modChanges.push_back(*static_cast<const wxl_object_field_args*>(args));
    }

    // --- Assets: a model, a map object and a placed doodad, as the asset rows read them -------

    struct FakeModel
    {
        char        name[260] = {};
        std::string bytes;
        std::string replaced;    ///< what model_replace_bytes put in place, when it was asked
        bool        replace_fails = false;
        std::string skin;        ///< its skin profile, as read
    };

    /// A terrain tile, a map object's group and a texture, as the asset event rows read them.
    struct FakeTile
    {
        int32_t     x = 0, y = 0;
        const char* file = nullptr; ///< stands for the open file: its name
        std::string bytes;
    };

    struct FakeGroup
    {
        void*       parent = nullptr;
        uint32_t    index  = 0;
        std::string bytes;
    };

    struct FakeTexture
    {
        char name[260] = {};
    };

    const void* WXL_CALL FakeAreaBytes(const void* area, int32_t* x, int32_t* y, void** file, uint32_t* size)
    {
        const FakeTile* tile = static_cast<const FakeTile*>(area);
        if (!tile) return nullptr;
        *x    = tile->x;
        *y    = tile->y;
        *file = const_cast<char*>(tile->file);
        *size = static_cast<uint32_t>(tile->bytes.size());
        return tile->bytes.data();
    }

    wxl_bool WXL_CALL FakeFileName(void* file, char* buffer, uint32_t capacity)
    {
        if (!file) return WXL_FALSE;
        std::snprintf(buffer, capacity, "%s", static_cast<const char*>(file));
        return WXL_TRUE;
    }

    const void* WXL_CALL FakeGroupBytes(const void* group, void** parent, uint32_t* index, uint32_t* size)
    {
        const FakeGroup* g = static_cast<const FakeGroup*>(group);
        if (!g) return nullptr;
        *parent = g->parent;
        *index  = g->index;
        *size   = static_cast<uint32_t>(g->bytes.size());
        return g->bytes.data();
    }

    const void* WXL_CALL FakeSkinBytes(const void* shared)
    {
        const FakeModel* model = static_cast<const FakeModel*>(shared);
        return model && !model->skin.empty() ? model->skin.data() : nullptr;
    }

    const char* WXL_CALL FakeTextureName(const void* texture)
    {
        return texture ? static_cast<const FakeTexture*>(texture)->name : nullptr;
    }

    struct FakeMapObj
    {
        char        name[260] = {};
        std::string bytes;
    };

    struct FakeDoodad
    {
        wxl_vec3 position{};
    };

    const char* WXL_CALL FakeModelPath(const void* shared)
    {
        return shared ? static_cast<const FakeModel*>(shared)->name : nullptr;
    }

    const void* WXL_CALL FakeModelBytes(const void* shared, uint32_t* size)
    {
        if (!shared) return nullptr;
        const FakeModel* model = static_cast<const FakeModel*>(shared);
        const std::string& now = model->replaced.empty() ? model->bytes : model->replaced;
        if (size) *size = static_cast<uint32_t>(now.size());
        return now.data();
    }

    wxl_bool WXL_CALL FakeModelReplace(void* shared, const void* bytes, uint32_t size)
    {
        FakeModel* model = static_cast<FakeModel*>(shared);
        if (!model || !bytes || !size || model->replace_fails) return WXL_FALSE;
        model->replaced.assign(static_cast<const char*>(bytes), size);
        return WXL_TRUE;
    }

    const char* WXL_CALL FakeMapObjPath(const void* map_obj)
    {
        return map_obj ? static_cast<const FakeMapObj*>(map_obj)->name : nullptr;
    }

    const void* WXL_CALL FakeMapObjBytes(const void* map_obj, uint32_t* size)
    {
        if (!map_obj) return nullptr;
        const FakeMapObj* root = static_cast<const FakeMapObj*>(map_obj);
        if (size) *size = static_cast<uint32_t>(root->bytes.size());
        return root->bytes.data();
    }

    wxl_bool WXL_CALL FakeDoodadPosition(const void* def, wxl_vec3* out)
    {
        if (!def || !out) return WXL_FALSE;
        *out = static_cast<const FakeDoodad*>(def)->position;
        return WXL_TRUE;
    }


    // --- The gameplay rows: aura slots, the loot window, the factions, the dialogs' answers ---

    /// An aura slot as the client keeps one (AuraData), as the aura rows read it.
    struct FakeAura
    {
        uint64_t caster   = 0;
        int32_t  spell    = 0;
        uint8_t  flags    = 0;
        uint8_t  level    = 0;
        uint8_t  stacks   = 0;
        int32_t  duration = 0;
        int32_t  end      = 0;
    };

    const FakeAura* Aura(const void* aura) { return static_cast<const FakeAura*>(aura); }

    uint64_t WXL_CALL FakeAuraCaster(const void* aura) { return Aura(aura)->caster; }
    int32_t WXL_CALL FakeAuraSpell(const void* aura) { return Aura(aura)->spell; }
    uint8_t WXL_CALL FakeAuraFlags(const void* aura) { return Aura(aura)->flags; }
    uint8_t WXL_CALL FakeAuraLevel(const void* aura) { return Aura(aura)->level; }
    uint8_t WXL_CALL FakeAuraStacks(const void* aura) { return Aura(aura)->stacks; }
    int32_t WXL_CALL FakeAuraDuration(const void* aura) { return Aura(aura)->duration; }
    int32_t WXL_CALL FakeAuraEnd(const void* aura) { return Aura(aura)->end; }

    /// The aura slots of the one unit the checks give some, and the client's clock.
    const void* g_auraOwner = nullptr;
    FakeAura    g_auraSlots[4];
    uint32_t    g_timeMs = 0;

    int32_t WXL_CALL FakeAuraSlot(const void* unit, const void* aura)
    {
        const FakeAura* slot = Aura(aura);
        if (unit != g_auraOwner || slot < g_auraSlots || slot >= g_auraSlots + 4) return -1;
        return static_cast<int32_t>(slot - g_auraSlots);
    }

    uint32_t WXL_CALL FakeTimeMs(void) { return g_timeMs; }

    /// The loot window: its object, kind, money and entries (item, quantity, display, suffix,
    /// property, server slot, slot type; item 0 for an empty one).
    uint64_t g_lootGuid  = 0;
    uint32_t g_lootKind  = 0;
    uint32_t g_lootMoney = 0;
    int32_t  g_lootSlots[18][7]{};
    uint32_t g_nextRoll  = 0;
    int32_t  g_uiMap     = 0;

    uint64_t WXL_CALL FakeLootGuid(void) { return g_lootGuid; }
    uint32_t WXL_CALL FakeLootKind(void) { return g_lootKind; }
    uint32_t WXL_CALL FakeLootMoney(void) { return g_lootMoney; }
    uint32_t WXL_CALL FakeNextRoll(void) { return g_nextRoll; }
    int32_t WXL_CALL FakeUiMap(void) { return g_uiMap; }

    wxl_bool WXL_CALL FakeLootSlot(uint32_t index, int32_t* out)
    {
        if (index >= 18 || !out || !g_lootSlots[index][0]) return WXL_FALSE;
        memcpy(out, g_lootSlots[index], sizeof g_lootSlots[index]);
        return WXL_TRUE;
    }

    /// The reputation list: one faction at list id 3, the others empty.
    int32_t g_factionId = 0, g_factionBase = 0, g_factionStanding = 0;

    wxl_bool WXL_CALL FakeFaction(uint32_t list_id, int32_t* faction, int32_t* base, int32_t* standing)
    {
        if (list_id >= 128) return WXL_FALSE;
        const bool mine = list_id == 3;
        if (faction) *faction = mine ? g_factionId : 0;
        if (base) *base = mine ? g_factionBase : 0;
        if (standing) *standing = mine ? g_factionStanding : 0;
        return WXL_TRUE;
    }

    uint64_t g_interact = 0, g_tradePartner = 0;
    uint64_t WXL_CALL FakeInteract(void) { return g_interact; }
    uint64_t WXL_CALL FakeTradePartner(void) { return g_tradePartner; }

    /// The answers the seams gave in a dialog's place, in order.
    std::vector<std::string> g_answers;

    void WXL_CALL FakeGroupAnswer(wxl_bool accept) { g_answers.push_back(accept ? "group accept" : "group decline"); }

    void WXL_CALL FakeResurrectAnswer(uint64_t offerer, uint32_t sickness, uint32_t, wxl_bool accept)
    {
        g_answers.push_back(std::string(accept ? "resurrect accept " : "resurrect decline ") +
                            std::to_string(offerer) + (sickness ? " sick" : ""));
    }

    void WXL_CALL FakeDuelAnswer(uint64_t arbiter, uint64_t challenger, wxl_bool accept)
    {
        g_answers.push_back(std::string(accept ? "duel accept " : "duel decline ") + std::to_string(arbiter) + " " +
                            std::to_string(challenger));
    }

    // --- M2: the batch a fake renderer drew, appearances, a fake model heap, a model file ----------

    /// The batch the fake model renderer last drew: what gx_model_draw answers, once per draw.
    wxl_model_batch_args g_gxDraw{};
    bool                 g_gxDrew = false;

    /// The API the fake device runs on: Direct3D 9.
    int32_t WXL_CALL FakeGxDeviceApi() { return 1; }

    /// The world's view wxl.draw reads: pixels of a 1600 x 900 window on the clip space, from an eye
    /// at (100, 200, 0); false until a check sets it up.
    bool g_fakeWorldView = false;
    wxl_bool WXL_CALL FakeGxWorldView(wxl_core_world_view* out)
    {
        if (!g_fakeWorldView || !out) return WXL_FALSE;
        *out = wxl_core_world_view{};
        for (int i = 0; i < 4; ++i) out->view[5 * i] = 1.0f;
        out->projection[0]  = 2.0f / 1600.0f;
        out->projection[5]  = -2.0f / 900.0f;
        out->projection[10] = 0.001f;
        out->projection[12] = -1.0f;
        out->projection[13] = 1.0f;
        out->projection[14] = 0.5f;
        out->projection[15] = 1.0f;
        const float box[6]  = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
        std::memcpy(out->viewport, box, sizeof box);
        out->window[0] = 1600.0f;
        out->window[1] = 900.0f;
        out->eye       = wxl_vec3{100.0f, 200.0f, 0.0f};
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeGxModelDraw(const void* render, wxl_model_batch_args* out)
    {
        if (!render || !out || !g_gxDrew) return WXL_FALSE;
        g_gxDrew = false;
        *out     = g_gxDraw;
        return WXL_TRUE;
    }

    struct FakeComponent
    {
        void*    model;
        uint32_t race;
        uint32_t sex;
        uint32_t display[12];
        bool     npc;
    };

    /// A model of the fake heap: its references, what it hangs on, what was done to it, and a
    /// skeleton of three bones whose matrices the posing writes.
    struct FakeM2Model
    {
        int         refs       = 1;
        void*       parent     = nullptr;
        void*       source     = nullptr; ///< what an interface copy was duplicated from
        uint32_t    geoFirst   = 0, geoLast = 0;
        wxl_bool    geoVisible = WXL_FALSE;
        uint32_t    texType    = 0;
        std::string texture;
        uint32_t    attachment = 0;
        wxl_bool    toRoot     = WXL_FALSE;
        void*       file       = nullptr; ///< a copy's: the model file it shares; a made model is its own
        bool        loaded     = true;
        float       matrices[3][16]{};
    };

    std::vector<FakeM2Model*> g_fakeLive; // every fake model alive, for m2_children
    int                       g_fakeCopies      = 0;
    int                       g_wearerAsked     = 0;
    int                       g_fakeModelsFreed = 0;
    FakeComponent             g_component{};

    /// The character model the appearance wears: a skeleton of its own, never released.
    FakeM2Model g_charModel;

    FakeM2Model* AsFake(void* model) { return static_cast<FakeM2Model*>(model); }

    /// The bones of every fake file, as the file lays them out (0x58 bytes, key bone id at 0,
    /// parent at 8, name hash at 12, pivot at 0x4C): the body's root, its spine (key bone 5),
    /// and a hand (name 0xAB); a piece made of a root, a bone of key bone 5 and one named 0xAB.
    struct FakeBones
    {
        uint8_t  bytes[3][0x58]{};
        uint16_t keys[8];

        FakeBones()
        {
            for (uint16_t& key : keys) key = 0xFFFF;
            const int32_t  ids[3]     = {-1, 5, -1};
            const int16_t  parents[3] = {-1, 0, 1};
            const uint32_t names[3]   = {0, 0, 0xAB};
            for (int i = 0; i < 3; ++i)
            {
                std::memcpy(bytes[i] + 0x00, &ids[i], 4);
                std::memcpy(bytes[i] + 0x08, &parents[i], 2);
                std::memcpy(bytes[i] + 0x0C, &names[i], 4);
                const float pivot[3] = {0.0f, 0.0f, static_cast<float>(i)};
                std::memcpy(bytes[i] + 0x4C, pivot, sizeof pivot);
            }
            keys[5] = 1;
        }
    };

    FakeBones g_bodyBones;

    wxl_bool WXL_CALL FakeComponentWearer(const void* component, uint32_t item_slot, wxl_appearance_args* out)
    {
        ++g_wearerAsked;
        if (component != &g_component || !out) return WXL_FALSE;
        out->wearer     = const_cast<void*>(component);
        out->model      = g_component.model;
        out->race_id    = g_component.race;
        out->sex_id     = g_component.sex;
        out->item_slot  = item_slot;
        out->display_id = item_slot < 12 ? g_component.display[item_slot] : 0;
        out->npc        = g_component.npc ? WXL_TRUE : WXL_FALSE;
        return WXL_TRUE;
    }

    void* WXL_CALL FakeObjectModel(const void* object) { return object ? static_cast<void*>(&g_charModel) : nullptr; }

    wxl_bool WXL_CALL FakeItemDisplay(uint32_t id, wxl_m2_item_display* out)
    {
        if (id != 416036 || !out) return WXL_FALSE;
        out->id               = id;
        out->model[0]         = "belt.mdx";
        out->model[1]         = "";
        out->model_texture[0] = "belt_skin";
        out->model_texture[1] = "";
        out->icon[0]          = "";
        out->icon[1]          = "53::96";
        out->particle_color_id = 7;
        return WXL_TRUE;
    }

    const char* WXL_CALL FakeRacePrefix(uint32_t race) { return race == 1 ? "Hu" : nullptr; }

    void* WXL_CALL FakeCreateModel(void* scene_model, const char* path)
    {
        if (!scene_model || !path || !*path) return nullptr;
        FakeM2Model* model = new FakeM2Model{};
        g_fakeLive.push_back(model);
        return model;
    }

    void WXL_CALL FakeReleaseModel(void* model)
    {
        FakeM2Model* fake = AsFake(model);
        if (!fake || fake == &g_charModel || --fake->refs > 0) return;
        std::erase(g_fakeLive, fake);
        delete fake;
        ++g_fakeModelsFreed;
    }

    wxl_bool WXL_CALL FakeAttachModel(void* model, void* parent, uint32_t attachment, wxl_bool to_root)
    {
        FakeM2Model* fake = AsFake(model);
        if (!fake || !parent || (attachment == 99 && !to_root)) return WXL_FALSE; // 99: a point the parent lacks
        if (!fake->parent) ++fake->refs;                                           // the parent's reference
        fake->parent     = parent;
        fake->attachment = attachment;
        fake->toRoot     = to_root;
        return WXL_TRUE;
    }

    void WXL_CALL FakeDetachModel(void* model)
    {
        FakeM2Model* fake = AsFake(model);
        if (!fake || !fake->parent) return;
        fake->parent = nullptr;
        FakeReleaseModel(model);
    }

    void* WXL_CALL FakeModelParent(const void* model) { return model ? AsFake(const_cast<void*>(model))->parent : nullptr; }

    // --- The scene's animate list (the M2 BeginThread and WaitThread gates) ---------------------

    /// A model of a fake scene's animate list: how often, on which thread and with which floating
    /// point state it was animated, and a stand-in for its bones' work (the bench's).
    struct FakeAnimModel : FakeM2Model
    {
        FakeAnimModel*            next = nullptr;
        std::atomic<int>          animated{0};
        DWORD                     thread = 0;
        wxl::core::tuning::FloatState state;
        uint32_t                  work = 0; ///< bones to pose, the bench's
        float                     out  = 0.0f;
    };

    struct FakeAnimScene
    {
        FakeAnimModel* head = nullptr;
    };

    /// CM2Scene::AnimateThread, as the fake client names it.
    void FakeAnimateThread(void*) {}

    wxl_bool WXL_CALL FakeIsAnimateThread(const void* function)
    {
        return function == reinterpret_cast<const void*>(&FakeAnimateThread) ? WXL_TRUE : WXL_FALSE;
    }

    uint32_t WXL_CALL FakeAnimateList(const void* scene, void** out, uint32_t capacity)
    {
        uint32_t count = 0;
        for (FakeAnimModel* m = scene ? static_cast<const FakeAnimScene*>(scene)->head : nullptr; m; m = m->next)
        {
            if (out && count < capacity) out[count] = static_cast<FakeM2Model*>(m);
            ++count;
        }
        return count;
    }

    void* WXL_CALL FakeSwapAnimateList(void* scene, void* head)
    {
        if (!scene) return nullptr;
        auto* s   = static_cast<FakeAnimScene*>(scene);
        void* was = s->head ? static_cast<FakeM2Model*>(s->head) : nullptr;
        s->head   = head ? static_cast<FakeAnimModel*>(static_cast<FakeM2Model*>(head)) : nullptr;
        return was;
    }

    /// A model's bones posed: a chain of 4x4 products, about what CM2Model::AnimateMT does per bone.
    float FakePose(uint32_t bones, float seed)
    {
        float m[16], r[16];
        for (int i = 0; i < 16; ++i) m[i] = seed + static_cast<float>(i) * 0.01f;
        for (uint32_t b = 0; b < bones; ++b)
        {
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    r[i * 4 + j] = m[i * 4] * m[j] + m[i * 4 + 1] * m[4 + j] + m[i * 4 + 2] * m[8 + j] + m[i * 4 + 3] * m[12 + j];
            for (int i = 0; i < 16; ++i) m[i] = r[i] * 0.25f + 0.1f;
        }
        return m[0];
    }

    void WXL_CALL FakeAnimateModel(void* model, void* scene)
    {
        if (!model || !scene) return;
        auto* m   = static_cast<FakeAnimModel*>(static_cast<FakeM2Model*>(model));
        m->thread = GetCurrentThreadId();
        m->state  = wxl::core::tuning::CaptureFloat();
        if (m->work) m->out = FakePose(m->work, 0.01f);
        m->animated.fetch_add(1);
    }

    /// A fake scene of @p count models, every @p attachedEvery-th one attached to another.
    struct AnimScene
    {
        FakeAnimScene              scene;
        std::vector<FakeAnimModel> models;
        FakeM2Model                parent;

        AnimScene(uint32_t count, uint32_t attachedEvery, uint32_t work = 0) : models(count)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                models[i].next = i + 1 < count ? &models[i + 1] : nullptr;
                models[i].work = work;
                if (attachedEvery && i % attachedEvery == attachedEvery - 1) models[i].parent = &parent;
            }
            scene.head = count ? &models[0] : nullptr;
        }

        /// CM2Scene::Animate's threaded part, as the client runs it: BeginThread, its own loop over
        /// the list (every other model; none while the gate hides it), WaitThread.
        bool Frame(void* cache, void* function = reinterpret_cast<void*>(&FakeAnimateThread))
        {
            void* self = cache;
            void* fn   = function;
            void* arg  = &scene;
            const bool taken = wxl::seam::M2::BeginThread::Gate(&self, &fn, &arg);
            for (FakeAnimModel* m = scene.head; m; m = m->next ? m->next->next : nullptr)
                if (!m->parent) FakeAnimateModel(static_cast<FakeM2Model*>(m), &scene);
            void* wait = cache;
            const bool waited = wxl::seam::M2::WaitThread::Gate(&wait);
            return taken && waited;
        }
    };


    void* WXL_CALL FakeModelSource(const void* model) { return model ? AsFake(const_cast<void*>(model))->source : nullptr; }

    void WXL_CALL FakeSetGeosets(void* model, uint32_t first, uint32_t last, wxl_bool visible)
    {
        if (FakeM2Model* fake = AsFake(model))
        {
            fake->geoFirst   = first;
            fake->geoLast    = last;
            fake->geoVisible = visible;
        }
    }

    wxl_bool WXL_CALL FakeReplaceTexture(void* model, uint32_t type, const char* path)
    {
        FakeM2Model* fake = AsFake(model);
        if (!fake || !path || std::strcmp(path, "missing.blp") == 0) return WXL_FALSE;
        fake->texType = type;
        fake->texture = path;
        return WXL_TRUE;
    }

    void* WXL_CALL FakeModelFile(const void* model)
    {
        FakeM2Model* fake = AsFake(const_cast<void*>(model));
        return fake ? (fake->file ? fake->file : fake) : nullptr;
    }

    // The client's model cache and texture list, as the reload rows reach them.
    int g_evictAsked   = 0;
    int g_displayAgain = 0;

    uint32_t WXL_CALL FakeCacheEvict(const char* leaf, void** out, uint32_t capacity)
    {
        ++g_evictAsked;
        if (!leaf || std::strcmp(leaf, "character.m2") != 0) return 0;
        if (out && capacity) out[0] = &g_charModel; // the character model is its own file
        return 1;
    }

    wxl_bool WXL_CALL FakeUpdateDisplay(void* object)
    {
        const FakeObject* unit = static_cast<const FakeObject*>(object);
        if (!unit || (unit->typeId != WXL_OBJECT_UNIT && unit->typeId != WXL_OBJECT_PLAYER)) return WXL_FALSE;
        ++g_displayAgain;
        return WXL_TRUE;
    }

    uint32_t WXL_CALL FakeTextureReload(const char* name, uint32_t* busy, uint32_t* atlas)
    {
        if (busy) *busy = 0;
        if (atlas) *atlas = 0;
        if (name && _stricmp(name, "Textures\\Loading") == 0)
        {
            if (busy) *busy = 1;
            return 0;
        }
        if (name && _stricmp(name, "Interface\\Icons\\Shared") == 0)
        {
            if (atlas) *atlas = 1;
            return 0;
        }
        if (!name || _stricmp(name, "Textures\\Fake") != 0) return 0;
        if (busy) *busy = 1;
        return 2;
    }

    wxl_bool WXL_CALL FakeAttachmentPoint(const void* model, uint32_t attachment, wxl_vec3* out)
    {
        if (model != &g_charModel || attachment != 32 || !out) return WXL_FALSE;
        *out = wxl_vec3{0.1f, 0.2f, 1.1f};
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeSkeleton(void* model, wxl_core_m2_skeleton* out)
    {
        FakeM2Model* fake = AsFake(model);
        if (!fake || !out || !fake->loaded) return WXL_FALSE;
        out->file           = fake == &g_charModel ? static_cast<const void*>(&g_charModel) : FakeModelFile(fake);
        out->bones          = &g_bodyBones.bytes[0][0];
        out->bone_count     = 3;
        out->bone_stride    = 0x58;
        out->key_bones      = g_bodyBones.keys;
        out->key_bone_count = 8;
        out->matrices       = &fake->matrices[0][0];
        return WXL_TRUE;
    }

    uint32_t WXL_CALL FakeChildren(const void* model, void** out, uint32_t capacity)
    {
        uint32_t count = 0;
        for (FakeM2Model* fake : g_fakeLive)
        {
            if (fake->parent != model) continue;
            if (out && count < capacity) out[count] = fake;
            ++count;
        }
        return count;
    }

    wxl_bool WXL_CALL FakeAttachInfo(const void* model, uint32_t* attachment, wxl_bool* to_root)
    {
        FakeM2Model* fake = AsFake(const_cast<void*>(model));
        if (!fake || !fake->parent) return WXL_FALSE;
        *attachment = fake->attachment;
        *to_root    = fake->toRoot;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeAttachCopy(void* source, void* parent, uint32_t attachment, wxl_bool to_root)
    {
        if (!source || !parent) return WXL_FALSE;
        FakeM2Model* copy = new FakeM2Model{};
        copy->parent = parent, copy->source = source, copy->attachment = attachment, copy->toRoot = to_root;
        g_fakeLive.push_back(copy);
        ++g_fakeCopies;
        return WXL_TRUE;
    }

    // --- wxl.m2's models of the world and bone watchers: what the binding was asked ------------

    struct FakeWorldState
    {
        int              made     = 0;
        int              placed   = 0;
        int              unplaced = 0;
        wxl_m2_placement placement{};
        wxl_m2_placement transform{};
        uint32_t         anim  = 0;
        float            speed = 0.0f;
        float            alpha = 1.0f;
        int              emitting = -1;
    };
    FakeWorldState g_world;

    void* WXL_CALL FakeCreateWorldModel(const char* path)
    {
        if (!path || !*path) return nullptr;
        ++g_world.made;
        FakeM2Model* model = new FakeM2Model{};
        g_fakeLive.push_back(model);
        return model;
    }
    wxl_bool WXL_CALL FakePlaceWorld(void* model, const wxl_m2_placement* placement)
    {
        if (!model || !placement || g_mapId < 0) return WXL_FALSE;
        ++g_world.placed;
        g_world.placement = *placement;
        return WXL_TRUE;
    }
    void WXL_CALL FakeUnplaceWorld(void* model)
    {
        if (model) ++g_world.unplaced;
    }
    void WXL_CALL FakeSetTransform(void*, const wxl_m2_placement* placement) { g_world.transform = *placement; }
    void WXL_CALL FakePlayAnimation(void*, uint32_t anim, float speed) { g_world.anim = anim, g_world.speed = speed; }
    wxl_bool WXL_CALL FakeM2Loaded(const void* model)
    {
        return model && static_cast<const FakeM2Model*>(model)->loaded ? WXL_TRUE : WXL_FALSE;
    }
    uint32_t WXL_CALL FakeAnimationLength(void*, uint32_t anim) { return anim == 0 ? 1333u : 0u; }
    void WXL_CALL FakeSetAlpha(void*, float alpha) { g_world.alpha = alpha; }
    void WXL_CALL FakeSetEmitting(void*, wxl_bool emitting) { g_world.emitting = emitting ? 1 : 0; }
    wxl_bool WXL_CALL FakeAttachmentWorld(void* model, uint32_t attachment, wxl_vec3* out)
    {
        if (model != &g_charModel || attachment != 32) return WXL_FALSE;
        *out = wxl_vec3{100.0f, 200.0f, 31.5f};
        return WXL_TRUE;
    }

    /// The bones of a fake model as the binding hands them to watchers: two files, told apart by
    /// the model's `file` (null: the model is its own file), path "a.m2" or "b.m2".
    int g_bonesAsked = 0;
    wxl_bool WXL_CALL FakeM2Bones(void* model, wxl_m2_bones* out)
    {
        ++g_bonesAsked;
        FakeM2Model* fake = AsFake(model);
        if (!fake || !fake->loaded) return WXL_FALSE;
        out->model      = model;
        out->file       = fake->file ? fake->file : model;
        out->path       = fake->texType == 2 ? wxl_str{"b.m2", 4} : wxl_str{"a.m2", 4};
        out->bone_count = 3;
        out->matrices   = &fake->matrices[0][0];
        out->time_ms    = 42;
        return WXL_TRUE;
    }
    wxl_bool WXL_CALL FakeFileBone(const void* file, uint32_t index, int16_t* parent, wxl_vec3* pivot)
    {
        if (!file || index >= 3) return WXL_FALSE;
        std::memcpy(parent, g_bodyBones.bytes[index] + 0x08, sizeof *parent);
        std::memcpy(pivot, g_bodyBones.bytes[index] + 0x4C, sizeof *pivot);
        return WXL_TRUE;
    }

    /// One fake model file, its bytes and skin, and the shadow bones counted on it.
    struct FakeM2File
    {
        uint8_t bytes[64];
        uint8_t skin[48];
        bool    hasSkin;
        int     asked;    // m2_file_info calls
        int     animated; // m2_file_animate_bones calls
    };

    FakeM2File g_m2File{};

    wxl_bool WXL_CALL FakeM2FileInfo(const void* file, wxl_core_m2_file* out)
    {
        ++g_m2File.asked;
        if (file != &g_m2File || !out) return WXL_FALSE;
        out->path   = "creature\\fake\\fake.m2";
        out->header = g_m2File.bytes;
        out->size   = sizeof g_m2File.bytes;
        out->skin   = g_m2File.hasSkin ? g_m2File.skin : nullptr;
        return WXL_TRUE;
    }

    void WXL_CALL FakeM2FileAnimateBones(void* file)
    {
        if (file == &g_m2File) ++g_m2File.animated;
    }

    /// Every other row stays null, which is what a client that hands nothing over looks like.
    // --- the typed reads and the actions (unit.def, item.def, action.def) ---------------------

    uint64_t g_party[4]{};
    uint64_t g_raid[40]{};
    uint32_t g_raidCount = 0;

    /// What the fake client was asked to do, as "target 0x..." lines.
    std::vector<std::string> g_actions;
    bool                     g_refuseActions = false;

    void Did(const char* what, uint64_t a = 0, uint64_t b = 0)
    {
        char line[96];
        snprintf(line, sizeof line, "%s %llx %llx", what, static_cast<unsigned long long>(a),
                 static_cast<unsigned long long>(b));
        g_actions.emplace_back(line);
    }

    uint32_t WXL_CALL FakeCastStart(const void*) { return 0; }
    uint32_t WXL_CALL FakeCastEnd(const void* o) { return Obj(o)->castEnd; }
    uint32_t WXL_CALL FakeChannelStart(const void*) { return 0; }
    uint32_t WXL_CALL FakeChannelEnd(const void* o) { return Obj(o)->channelEnd; }
    int32_t WXL_CALL FakeMountModel(const void* o) { return Obj(o)->mountModel; }
    uint32_t WXL_CALL FakeClientFlags(const void* o) { return Obj(o)->clientFlags; }
    wxl_bool WXL_CALL FakeSpeed(const void* o, uint32_t i, float* out)
    {
        if (i > 8) return WXL_FALSE;
        *out = Obj(o)->speeds[i];
        return WXL_TRUE;
    }
    int32_t WXL_CALL FakeReaction(const void* o, const void*) { return Obj(o)->reaction; }
    int32_t WXL_CALL FakeCreatureType(const void* o) { return Obj(o)->creatureType; }
    uint32_t WXL_CALL FakeFamily(const void* o) { return Obj(o)->family; }
    uint32_t WXL_CALL FakeRank(const void* o) { return Obj(o)->rank; }
    wxl_bool WXL_CALL FakeKnows(const void* o, uint32_t spell) { return Obj(o)->knownSpell == spell ? WXL_TRUE : WXL_FALSE; }
    /// The auras of the units the typed reads give some: the unit, its slots.
    std::map<const void*, std::vector<FakeAura>> g_unitAuras;

    uint32_t WXL_CALL FakeAuraCount(const void* o)
    {
        const auto found = g_unitAuras.find(o);
        return found == g_unitAuras.end() ? 0u : static_cast<uint32_t>(found->second.size());
    }
    const void* WXL_CALL FakeAuraAt(const void* o, uint32_t i)
    {
        const auto found = g_unitAuras.find(o);
        return found != g_unitAuras.end() && i < found->second.size() ? &found->second[i] : nullptr;
    }
    const void* WXL_CALL FakeItemStats(const void* o) { return Obj(o)->stats; }
    using FakeStats = FakeObject::Stats;
    const FakeStats* StatsOf(const void* s) { return static_cast<const FakeStats*>(s); }
    uint32_t WXL_CALL FakeStatQuality(const void* s) { return StatsOf(s)->quality; }
    uint32_t WXL_CALL FakeStatLevel(const void* s) { return StatsOf(s)->level; }
    uint32_t WXL_CALL FakeStatRequired(const void* s) { return StatsOf(s)->requiredLevel; }
    uint32_t WXL_CALL FakeStatClass(const void* s) { return StatsOf(s)->itemClass; }
    uint32_t WXL_CALL FakeStatSubclass(const void* s) { return StatsOf(s)->subclass; }
    uint32_t WXL_CALL FakeStatInventory(const void* s) { return StatsOf(s)->inventoryType; }
    int32_t WXL_CALL FakeStatStack(const void* s) { return StatsOf(s)->stackable; }
    uint32_t WXL_CALL FakeStatSell(const void* s) { return StatsOf(s)->sellPrice; }
    uint64_t WXL_CALL FakePartyGuid(uint32_t i) { return i < 4 ? g_party[i] : 0; }
    uint32_t WXL_CALL FakeRaidCount(void) { return g_raidCount; }
    uint64_t WXL_CALL FakeRaidGuid(uint32_t i) { return i < 40 ? g_raid[i] : 0; }
    wxl_bool WXL_CALL FakeActTarget(uint64_t guid)
    {
        Did("target", guid);
        return g_refuseActions ? WXL_FALSE : WXL_TRUE;
    }
    wxl_bool WXL_CALL FakeActInteract(uint64_t guid)
    {
        Did("interact", guid);
        return g_refuseActions ? WXL_FALSE : WXL_TRUE;
    }
    wxl_bool WXL_CALL FakeActCast(uint32_t spell, uint64_t target)
    {
        Did("cast", spell, target);
        return g_refuseActions ? WXL_FALSE : WXL_TRUE;
    }
    wxl_bool WXL_CALL FakeActStop(void)
    {
        Did("stop");
        return g_refuseActions ? WXL_FALSE : WXL_TRUE;
    }
    wxl_bool WXL_CALL FakeActUse(const void* item, uint64_t target)
    {
        Did("use", Obj(item)->guid, target);
        return g_refuseActions ? WXL_FALSE : WXL_TRUE;
    }
    wxl_bool WXL_CALL FakeActFollow(const void* mover, uint64_t guid)
    {
        Did("follow", Obj(mover)->guid, guid);
        return g_refuseActions ? WXL_FALSE : WXL_TRUE;
    }
    wxl_bool WXL_CALL FakeActMove(const void* player, wxl_vec3 point)
    {
        Did("move", Obj(player)->guid, static_cast<uint64_t>(point.x));
        return g_refuseActions ? WXL_FALSE : WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeActFace(const void* player, float facing)
    {
        Did("face", Obj(player)->guid, static_cast<uint64_t>(facing * 100.0f + 0.5f));
        return g_refuseActions ? WXL_FALSE : WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeOutdoors(const void* o) { return Obj(o)->clientFlags & 0x1u ? WXL_FALSE : WXL_TRUE; }

    /// The world camera as the fake client keeps it, and what was written to it.
    struct FakeCamera
    {
        wxl_core_camera view{};
        uint32_t        writes = 0;
    };
    FakeCamera  g_camera;
    FakeCamera* g_worldCamera = nullptr; ///< null out of the world

    // The game's input control as the mouse-look rows read it, and what they were asked.
    struct FakeInputControl
    {
        uint32_t pending = 0;
    };
    FakeInputControl      g_inputControl;
    std::vector<uint32_t> g_defaultActions;
    int                   g_lookStops = 0;

    uint32_t WXL_CALL FakePendingAction(const void* c) { return static_cast<const FakeInputControl*>(c)->pending; }
    void WXL_CALL FakeClearPending(void* c) { static_cast<FakeInputControl*>(c)->pending = 0; }
    wxl_bool WXL_CALL FakeDefaultAction(uint32_t button)
    {
        g_defaultActions.push_back(button);
        return WXL_TRUE;
    }
    void WXL_CALL FakeLookStop() { ++g_lookStops; }

    // What the world frame's default action would act on, and whether a spell waits for its target.
    struct FakeClickHit
    {
        uint32_t kind = 0;
        uint64_t guid = 0;
        wxl_vec3 point{};
    };
    FakeClickHit g_clickHit;
    wxl_bool     g_spellTargeting = WXL_FALSE;
    uint32_t WXL_CALL FakeClickHitRead(const void* frame, uint64_t* guid, wxl_vec3* point)
    {
        if (guid) *guid = frame ? g_clickHit.guid : 0;
        if (point) *point = frame ? g_clickHit.point : wxl_vec3{};
        return frame ? g_clickHit.kind : 0;
    }
    wxl_bool WXL_CALL FakeSpellTargeting() { return g_spellTargeting; }

    // What the player can do with a unit or a game object: hostile (reaction below 3) is
    // attackable, friendly or neutral talks; a game object's client flag 0x2 makes it usable.
    wxl_bool WXL_CALL FakeCanAttack(const void*, const void* t) { return Obj(t)->reaction < 3 ? WXL_TRUE : WXL_FALSE; }
    wxl_bool WXL_CALL FakeCanInteract(const void*, const void* t) { return Obj(t)->reaction >= 3 ? WXL_TRUE : WXL_FALSE; }
    wxl_bool WXL_CALL FakeCanUse(const void* o) { return Obj(o)->clientFlags & 0x2u ? WXL_TRUE : WXL_FALSE; }

    // A model's head bone as the pose rows set it.
    struct FakeHeadBone
    {
        uint32_t flags = 0;
        float    matrix[16]{};
        int      writes = 0;
    };
    FakeHeadBone g_headBone;
    void WXL_CALL FakeBoneFlags(void*, uint32_t bone, uint32_t flags, uint32_t mask)
    {
        if (bone == 6) g_headBone.flags = (g_headBone.flags & ~mask) | (flags & mask);
    }
    void WXL_CALL FakeBoneTransform(void*, uint32_t bone, const float* m)
    {
        if (bone != 6) return;
        std::memcpy(g_headBone.matrix, m, sizeof g_headBone.matrix);
        ++g_headBone.writes;
    }

    // The circles' texture static and the textures mods give (wxl.selectioncircle).
    struct FakeCircleTexture
    {
        std::string path;
    };
    FakeCircleTexture             g_gameCircleTexture{"Textures\\UnitSelectTexture.blp"};
    void*                   g_circleTexture = &g_gameCircleTexture;
    std::deque<FakeCircleTexture> g_openedTextures;
    int                     g_texturesClosed = 0;
    void* WXL_CALL FakeSwapCircleTexture(void* texture)
    {
        void* held      = g_circleTexture;
        g_circleTexture = texture;
        return held;
    }
    void* WXL_CALL FakeTextureOpen(const char* path)
    {
        if (!path || std::strstr(path, "Missing")) return nullptr;
        g_openedTextures.push_back(FakeCircleTexture{path});
        return &g_openedTextures.back();
    }
    void WXL_CALL FakeTextureClose(void*) { ++g_texturesClosed; }

    // The device's side of a texture (wxl.draw): a CGxTex whose Direct3D 9 texture is a made-up
    // pointer, once the file has loaded (a path naming "Loading" never does).
    wxl::engine::CGxTex g_fakeGxTex{};
    char                g_fakeD3dTexture[16]{};
    int                 g_texturesReadied = 0;
    void* WXL_CALL FakeGxTextureReady(void* texture)
    {
        const auto* t = static_cast<const FakeCircleTexture*>(texture);
        if (!t || t->path.find("Loading") != std::string::npos) return nullptr;
        ++g_texturesReadied;
        g_fakeGxTex.m_apiSpecificData = g_fakeD3dTexture;
        return &g_fakeGxTex;
    }

    void* WXL_CALL FakeCameraWorld() { return g_worldCamera; }

    wxl_bool WXL_CALL FakeCameraRead(const void* camera, wxl_core_camera* out)
    {
        if (!camera || !out) return WXL_FALSE;
        *out = static_cast<const FakeCamera*>(camera)->view;
        return WXL_TRUE;
    }

    void WXL_CALL FakeCameraWrite(void* camera, const wxl_core_camera* view)
    {
        FakeCamera& fake   = *static_cast<FakeCamera*>(camera);
        fake.view.position = view->position;
        fake.view.forward  = view->forward;
        // The rows as the bindings build them: SetFacing's, turned by the roll.
        wxl_vec3 rows[3];
        if (wxl::core::camera_facing::Facing(view->forward, view->roll, rows))
        {
            fake.view.forward = rows[0];
            fake.view.left    = rows[1];
            fake.view.up      = rows[2];
        }
        // FOV() is m_fov plus the effects' offset, which the fake keeps as the difference.
        fake.view.fov      = view->fov_base + (fake.view.fov - fake.view.fov_base);
        fake.view.fov_base = view->fov_base;
        ++fake.writes;
    }

    uint32_t g_traceFlags = 0;

    wxl_bool WXL_CALL FakeTrace(wxl_vec3 start, wxl_vec3 finish, uint32_t flags, wxl_vec3* hit)
    {
        g_traceFlags = flags;
        // A roof over the terrain, for an upright ray across it.
        if (g_roofZ != 0.0f && start.x == finish.x && start.y == finish.y && (start.z - g_roofZ) * (finish.z - g_roofZ) <= 0.0f)
        {
            *hit = wxl_vec3{start.x, start.y, g_roofZ};
            return WXL_TRUE;
        }
        // The ground at z = 0, for a ray straight down.
        if (start.x == finish.x && start.y == finish.y && start.z >= 0.0f && finish.z <= 0.0f)
        {
            *hit = wxl_vec3{start.x, start.y, 0.0f};
            return WXL_TRUE;
        }
        // A wall at x = 10.
        if ((start.x < 10.0f) == (finish.x < 10.0f)) return WXL_FALSE;
        const float t = (10.0f - start.x) / (finish.x - start.x);
        *hit = wxl_vec3{10.0f, start.y + (finish.y - start.y) * t, start.z + (finish.z - start.z) * t};
        return WXL_TRUE;
    }

    uint32_t g_cursorHides = 0, g_cursorRestores = 0;
    void WXL_CALL FakeCursorSetVisible(wxl_bool visible) { if (!visible) ++g_cursorHides; }
    void WXL_CALL FakeCursorRestore() { ++g_cursorRestores; }
    wxl_bool WXL_CALL FakeCursorVisible() { return WXL_TRUE; }
    wxl_bool WXL_CALL FakeOverWorld() { return WXL_TRUE; }

    // --- A split tile and its monolithic twin, as the client reads them --------------------------
    // The client's own reading (CMapArea::ProcessIffChunks, CMapChunk::ProcessIffChunks, Create's
    // counts, CreateRefs) written out over fake structures, so the seams of a split tile can be held
    // to what the client sets for the same tile in one file.

    struct SplitFakeArea : FakeTile
    {
        uint8_t*                 header = nullptr;
        uint8_t*                 chunkInfo = nullptr;
        uint8_t*                 doodadDef = nullptr;
        uint8_t*                 mapObjDef = nullptr;
        int32_t                  doodadCount = 0, mapObjCount = 0;
        char*                    m2Names = nullptr;
        char*                    wmoNames = nullptr;
        uint32_t*                modelOffsets = nullptr;
        uint32_t*                wmoOffsets = nullptr;
        int16_t*                 flying = nullptr;
        int32_t*                 textureFlags = nullptr;
        std::vector<std::string> textures;
    };

    struct SplitFakeChunk
    {
        SplitFakeArea*           area = nullptr;
        int32_t                  ix = 0, iy = 0;
        uint8_t*                 begin = nullptr;
        uint8_t*                 header = nullptr;
        float*                   height = nullptr;
        uint8_t*                 normals = nullptr;
        uint8_t*                 shadow = nullptr;
        uint8_t*                 layers = nullptr;
        uint8_t*                 alpha = nullptr;
        uint8_t*                 mcrf = nullptr;
        std::vector<std::string> placed; ///< "wmo:<name>:<id>", "m2:<name>:<id>" in the order made
    };

    uint32_t SplitU32(const uint8_t* at)
    {
        uint32_t v;
        std::memcpy(&v, at, sizeof v);
        return v;
    }

    /// CMapArea::LoadTextures: one texture a name.
    void SplitLoadTextures(SplitFakeArea& a, const char* names, uint32_t size)
    {
        for (uint32_t at = 0; at < size;)
        {
            a.textures.emplace_back(names + at);
            at += static_cast<uint32_t>(std::strlen(names + at)) + 1;
        }
    }

    /// CMapArea::ProcessIffChunks, as the fork has it.
    void SplitStockArea(SplitFakeArea& a)
    {
        uint8_t* buffer = reinterpret_cast<uint8_t*>(&a.bytes[0]);
        uint8_t* header = buffer + SplitU32(buffer + 4) + 16;
        auto     data   = [&](uint32_t slot) { return header + SplitU32(header + slot) + 8; };
        auto     size   = [&](uint32_t slot) { return SplitU32(header + SplitU32(header + slot) + 4); };
        a.header       = header;
        a.chunkInfo    = data(0x04);
        a.doodadDef    = data(0x1C);
        a.mapObjDef    = data(0x20);
        a.m2Names      = reinterpret_cast<char*>(data(0x0C));
        a.wmoNames     = reinterpret_cast<char*>(data(0x14));
        a.modelOffsets = reinterpret_cast<uint32_t*>(data(0x10));
        a.wmoOffsets   = reinterpret_cast<uint32_t*>(data(0x18));
        if (SplitU32(header) & 1) a.flying = reinterpret_cast<int16_t*>(data(0x24));
        if (SplitU32(header + 0x2C)) a.textureFlags = reinterpret_cast<int32_t*>(data(0x2C));
        SplitLoadTextures(a, reinterpret_cast<char*>(data(0x08)), size(0x08));
        a.doodadCount = static_cast<int32_t>(size(0x1C) / 36);
        a.mapObjCount = static_cast<int32_t>(size(0x20) / 64);
    }

    void SplitCreateRefs(SplitFakeChunk& c, SplitFakeArea& a, uint32_t* refs, uint32_t doodads, uint32_t mapObjs);

    /// PrepareChunk -> CMapChunk::Create: its sub-chunks walked, then its counts and references.
    void SplitStockChunk(SplitFakeArea& a, SplitFakeChunk& c, int32_t x, int32_t y, bool seams)
    {
        c.area  = &a;
        c.ix    = x;
        c.iy    = y;
        c.begin = reinterpret_cast<uint8_t*>(&a.bytes[0]) + SplitU32(a.chunkInfo + 16 * (x + 16 * y));
        c.header = c.begin + 8;
        int32_t  remaining = static_cast<int32_t>(SplitU32(c.begin + 4)) - 128;
        uint8_t* chunk     = c.header + 128;
        while (remaining > 0)
        {
            uint8_t* d = chunk + 8;
            remaining -= 8;
            switch (SplitU32(chunk))
            {
            case 0x4D434E52: c.normals = d; break;
            case 0x4D435348: c.shadow = d; break;
            case 0x4D435654: c.height = reinterpret_cast<float*>(d); break;
            case 0x4D435246: c.mcrf = d; break;
            case 0x4D434C59: c.layers = d; break;
            case 0x4D43414C: c.alpha = d; break;
            default: break;
            }
            remaining -= static_cast<int32_t>(SplitU32(chunk + 4));
            chunk = d + SplitU32(chunk + 4);
        }
        if (seams) wxl::seam::MapChunk::ProcessIffChunks::After(&c, true);
        SplitCreateRefs(c, a, reinterpret_cast<uint32_t*>(c.mcrf), SplitU32(c.header + 0x10), SplitU32(c.header + 0x38));
    }

    /// CMapChunk::CreateRefs: map objects, then doodads, from one list; the fork's gate first.
    void SplitCreateRefs(SplitFakeChunk& c, SplitFakeArea& a, uint32_t* refs, uint32_t doodads, uint32_t mapObjs)
    {
        void* self = &c;
        void* area = &a;
        if (wxl::seam::MapChunk::CreateRefs::Gate(&self, &area, &refs, &doodads, &mapObjs)) return;
        for (uint32_t i = 0; i < mapObjs; ++i)
        {
            const uint8_t* def = a.mapObjDef + 64 * refs[doodads + i];
            c.placed.push_back("wmo:" + std::string(a.wmoNames + a.wmoOffsets[SplitU32(def)]) + ":" + std::to_string(SplitU32(def + 4)));
        }
        for (uint32_t i = 0; i < doodads; ++i)
        {
            const uint8_t* def = a.doodadDef + 36 * refs[i];
            c.placed.push_back("m2:" + std::string(a.m2Names + a.modelOffsets[SplitU32(def)]) + ":" + std::to_string(SplitU32(def + 4)));
        }
    }

    wxl_bool WXL_CALL FakeAreaSplitSet(void* area, const wxl_core_area_split* s)
    {
        SplitFakeArea& a = *static_cast<SplitFakeArea*>(area);
        a.chunkInfo    = static_cast<uint8_t*>(s->chunk_info);
        a.doodadDef    = static_cast<uint8_t*>(s->doodad_defs);
        a.mapObjDef    = static_cast<uint8_t*>(s->map_obj_defs);
        a.doodadCount  = static_cast<int32_t>(s->doodad_count);
        a.mapObjCount  = static_cast<int32_t>(s->map_obj_count);
        a.m2Names      = s->model_names;
        a.wmoNames     = s->map_obj_names;
        a.modelOffsets = s->model_offsets;
        a.wmoOffsets   = s->map_obj_offsets;
        a.textureFlags = s->texture_flags;
        if (s->texture_names && s->texture_names_size) SplitLoadTextures(a, s->texture_names, s->texture_names_size);
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeAreaPlacements(const void* area, wxl_core_area_split* out)
    {
        const SplitFakeArea& a = *static_cast<const SplitFakeArea*>(area);
        *out                 = wxl_core_area_split{};
        out->doodad_defs     = a.doodadDef;
        out->map_obj_defs    = a.mapObjDef;
        out->doodad_count    = static_cast<uint32_t>(a.doodadCount);
        out->map_obj_count   = static_cast<uint32_t>(a.mapObjCount);
        out->model_names     = a.m2Names;
        out->map_obj_names   = a.wmoNames;
        out->model_offsets   = a.modelOffsets;
        out->map_obj_offsets = a.wmoOffsets;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeChunkParts(const void* chunk, wxl_core_map_chunk* out)
    {
        const SplitFakeChunk& c = *static_cast<const SplitFakeChunk*>(chunk);
        out->begin   = c.begin;
        out->header  = c.header;
        out->index_x = c.ix;
        out->index_y = c.iy;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeChunkSplitSet(void* chunk, const wxl_core_chunk_split* s)
    {
        SplitFakeChunk& c = *static_cast<SplitFakeChunk*>(chunk);
        c.layers = static_cast<uint8_t*>(s->layers);
        c.alpha  = static_cast<uint8_t*>(s->alpha);
        c.shadow = static_cast<uint8_t*>(s->shadow);
        c.mcrf   = static_cast<uint8_t*>(s->refs);
        return WXL_TRUE;
    }

    void WXL_CALL FakeChunkCreateRefs(void* chunk, void* area, const uint32_t* refs, uint32_t doodads, uint32_t mapObjs)
    {
        SplitCreateRefs(*static_cast<SplitFakeChunk*>(chunk), *static_cast<SplitFakeArea*>(area), const_cast<uint32_t*>(refs), doodads,
                        mapObjs);
    }

    wxl_core_init FakeInit()
    {
        wxl_core_init init{};
        init.struct_size            = sizeof init;
        init.map_id                 = &FakeMapId;
        init.local_player_object    = &FakeLocalPlayer;
        init.object_by_guid         = &FakeObjectByGuid;
        init.ui_target_guid         = &FakeTargetGuid;
        init.object_guid            = &FakeGuid;
        init.object_type_id         = &FakeTypeId;
        init.object_entry           = &FakeEntry;
        init.object_position        = &FakePosition;
        init.unit_health            = &FakeHealth;
        init.unit_casting_spell_id  = &FakeCastingSpellId;
        init.unit_channel_spell_id  = &FakeChannelSpellId;
        init.player_visible_item_id = &FakeVisibleItemId;
        init.visible_item_id        = &FakeVisibleItemIdOf;
        init.map_area_tile_x        = &FakeTileX;
        init.map_area_tile_y        = &FakeTileY;
        init.ui_initialized         = &FakeUiInitialized;
        init.ui_logging_in          = &FakeUiLoggingIn;
        init.ui_reloading           = &FakeUiReloading;
        init.world_position         = &FakeWorldPosition;
        init.script_state           = &FakeScriptState;
        init.script_global_is_set   = &FakeGlobalIsSet;
        init.script_set_global_closure = &FakeSetGlobalClosure;
        init.script_clear_global    = &FakeClearGlobal;
        init.script_closure_slot    = &FakeClosureSlot;
        init.script_top             = &FakeTop;
        init.script_value_type      = &FakeValueType;
        init.script_to_number       = &FakeToNumber;
        init.script_to_string       = &FakeToString;
        init.script_to_bool         = &FakeToBool;
        init.script_push_number     = &FakePushNumber;
        init.script_push_string     = &FakePushString;
        init.script_push_bool       = &FakePushBool;
        init.script_push_nil        = &FakePushNil;
        init.script_pop             = &FakePop;
        init.script_error           = &FakeError;
        init.script_run             = &FakeRun;
        init.script_create_events   = &FakeCreateEvents;
        init.script_signal_event    = &FakeSignalEvent;
        init.script_mark            = &FakeMark;
        init.script_unwind          = &FakeUnwind;
        init.message_data           = &FakeMessageData;
        init.message_size           = &FakeMessageSize;
        init.message_read           = &FakeMessageRead;
        init.file_exists            = &FakeFileExists;
        init.file_open              = &FakeFileOpen;
        init.file_size              = &FakeFileSize;
        init.file_seek              = &FakeFileSeek;
        init.file_read              = &FakeFileRead;
        init.file_close             = &FakeFileClose;
        init.archive_mount          = &FakeArchiveMount;
        init.storage_base_path      = &FakeBasePath;
        init.archive_list           = &FakeArchiveList;
        init.filestack_top          = &FakeFilestackTop;
        init.filestack_initial      = &FakeFilestackInitial;
        init.gx_device_exists       = &FakeGxDeviceExists;
        init.gx_has_context         = &FakeGxHasContext;
        init.gx_window_visible      = &FakeGxWindowVisible;
        init.gx_window_size         = &FakeGxWindowSize;
        init.gx_model_draw          = &FakeGxModelDraw;
        init.gx_device_api          = &FakeGxDeviceApi;
        init.gx_invalidate_state    = &FakeGxInvalidate;
        init.gx_world_view          = &FakeGxWorldView;
        init.gx_texture_ready       = &FakeGxTextureReady;
        init.component_wearer       = &FakeComponentWearer;
        init.m2_object_model        = &FakeObjectModel;
        init.m2_item_display        = &FakeItemDisplay;
        init.m2_race_prefix         = &FakeRacePrefix;
        init.m2_create_model        = &FakeCreateModel;
        init.m2_release_model       = &FakeReleaseModel;
        init.m2_attach_model        = &FakeAttachModel;
        init.m2_detach_model        = &FakeDetachModel;
        init.m2_model_parent        = &FakeModelParent;
        init.m2_model_source        = &FakeModelSource;
        init.m2_set_geosets         = &FakeSetGeosets;
        init.m2_replace_texture     = &FakeReplaceTexture;
        init.m2_children            = &FakeChildren;
        init.m2_attach_info         = &FakeAttachInfo;
        init.m2_attach_copy         = &FakeAttachCopy;
        init.m2_model_file          = &FakeModelFile;
        init.m2_attachment_point    = &FakeAttachmentPoint;
        init.m2_skeleton            = &FakeSkeleton;
        init.m2_is_animate_thread   = &FakeIsAnimateThread;
        init.m2_animate_list        = &FakeAnimateList;
        init.m2_swap_animate_list   = &FakeSwapAnimateList;
        init.m2_animate_model       = &FakeAnimateModel;
        init.m2_file_info           = &FakeM2FileInfo;
        init.m2_file_animate_bones  = &FakeM2FileAnimateBones;
        init.m2_create_world_model  = &FakeCreateWorldModel;
        init.m2_place_world         = &FakePlaceWorld;
        init.m2_unplace_world       = &FakeUnplaceWorld;
        init.m2_set_transform       = &FakeSetTransform;
        init.m2_play_animation      = &FakePlayAnimation;
        init.m2_loaded              = &FakeM2Loaded;
        init.m2_animation_length    = &FakeAnimationLength;
        init.m2_set_alpha           = &FakeSetAlpha;
        init.m2_set_emitting        = &FakeSetEmitting;
        init.m2_attachment_world    = &FakeAttachmentWorld;
        init.m2_bones               = &FakeM2Bones;
        init.m2_file_bone           = &FakeFileBone;
        init.pending_cast_caster    = &FakePendingCaster;
        init.pending_cast_spell_id  = &FakePendingSpellId;
        init.pending_cast_count     = &FakePendingCount;
        init.object_facing          = &FakeFacing;
        init.unit_movement_flags    = &FakeMovementFlags;
        init.player_money           = &FakeMoney;
        init.unit_cast_start        = &FakeCastStart;
        init.unit_cast_end          = &FakeCastEnd;
        init.unit_channel_start     = &FakeChannelStart;
        init.unit_channel_end       = &FakeChannelEnd;
        init.unit_mount_model       = &FakeMountModel;
        init.unit_client_flags      = &FakeClientFlags;
        init.unit_speed             = &FakeSpeed;
        init.unit_reaction          = &FakeReaction;
        init.unit_creature_type     = &FakeCreatureType;
        init.unit_creature_family   = &FakeFamily;
        init.unit_classification    = &FakeRank;
        init.unit_knows_spell       = &FakeKnows;
        init.unit_aura_count        = &FakeAuraCount;
        init.unit_aura_at           = &FakeAuraAt;
        init.aura_caster            = &FakeAuraCaster;
        init.aura_spell_id          = &FakeAuraSpell;
        init.aura_flags             = &FakeAuraFlags;
        init.aura_level             = &FakeAuraLevel;
        init.aura_stacks            = &FakeAuraStacks;
        init.aura_duration          = &FakeAuraDuration;
        init.aura_end_time          = &FakeAuraEnd;
        init.item_stats             = &FakeItemStats;
        init.item_stats_quality     = &FakeStatQuality;
        init.item_stats_level       = &FakeStatLevel;
        init.item_stats_required_level = &FakeStatRequired;
        init.item_stats_class       = &FakeStatClass;
        init.item_stats_subclass    = &FakeStatSubclass;
        init.item_stats_inventory_type = &FakeStatInventory;
        init.item_stats_stackable   = &FakeStatStack;
        init.item_stats_sell_price  = &FakeStatSell;
        init.party_member_guid      = &FakePartyGuid;
        init.raid_member_count      = &FakeRaidCount;
        init.raid_member_guid       = &FakeRaidGuid;
        init.action_target          = &FakeActTarget;
        init.action_interact        = &FakeActInteract;
        init.action_cast_spell      = &FakeActCast;
        init.action_stop_casting    = &FakeActStop;
        init.action_use_item        = &FakeActUse;
        init.action_follow          = &FakeActFollow;
        init.action_move_to         = &FakeActMove;
        init.action_face            = &FakeActFace;
        init.unit_is_outdoors       = &FakeOutdoors;
        init.camera_world           = &FakeCameraWorld;
        init.input_pending_action   = &FakePendingAction;
        init.input_clear_pending_action = &FakeClearPending;
        init.input_default_action   = &FakeDefaultAction;
        init.input_mouse_look_stop  = &FakeLookStop;
        init.input_click_hit        = &FakeClickHitRead;
        init.input_spell_targeting  = &FakeSpellTargeting;
        init.unit_can_attack        = &FakeCanAttack;
        init.unit_can_interact      = &FakeCanInteract;
        init.gameobject_can_use     = &FakeCanUse;
        init.model_bone_flags       = &FakeBoneFlags;
        init.model_bone_transform   = &FakeBoneTransform;
        init.circle_swap_texture    = &FakeSwapCircleTexture;
        init.texture_open           = &FakeTextureOpen;
        init.texture_close          = &FakeTextureClose;
        init.camera_read            = &FakeCameraRead;
        init.camera_write           = &FakeCameraWrite;
        init.world_trace            = &FakeTrace;
        init.input_cursor_visible   = &FakeCursorVisible;
        init.input_cursor_set_visible = &FakeCursorSetVisible;
        init.input_cursor_restore   = &FakeCursorRestore;
        init.input_pointer_over_world = &FakeOverWorld;
        init.terrain_click_guid     = &FakeTerrainGuid;
        init.terrain_click_x        = &FakeTerrainX;
        init.terrain_click_y        = &FakeTerrainY;
        init.terrain_click_z        = &FakeTerrainZ;
        init.terrain_click_button   = &FakeTerrainButton;
        init.sprite_click_guid      = &FakeSpriteGuid;
        init.sprite_click_button    = &FakeSpriteButton;
        init.cvar_name              = &FakeCVarName;
        init.cvar_value             = &FakeCVarValue;
        init.cvar_flags             = &FakeCVarFlags;
        init.cvar_default           = &FakeCVarDefault;
        init.cvar_latched           = &FakeCVarLatched;
        init.cvar_lookup            = &FakeCVarLookup;
        init.cvar_register          = &FakeCVarRegister;
        init.cvar_set               = &FakeCVarSet;
        init.cvar_write_flags       = &FakeCVarWriteFlags;
        init.interface_check_signature  = &FakeCheckSignature;
        init.interface_signature_digest = &FakeSignatureDigest;
        init.glue_current_screen    = &FakeGlueScreen;
        init.login_server_type      = &FakeLoginServerType;
        init.chat_pending_last      = &FakePendingLast;
        init.pending_chat_text      = &FakePendingText;
        init.object_descriptor      = &FakeDescriptor;
        init.object_watch_type      = &FakeWatchType;
        init.object_watch_one       = &FakeWatchOne;
        init.object_unwatch_type    = &FakeUnwatchType;
        init.object_unwatch_one     = &FakeUnwatchOne;
        init.gx_resources           = &FakeGxResources;
        init.sound_heap             = &FakeSoundHeap;
        init.script_heap            = &FakeScriptHeap;
        init.archive_tables         = &FakeArchiveTables;
        init.archive_of             = &FakeArchiveOf;
        init.archive_record_of      = &FakeArchiveRecordOf;
        init.m2_cache_evict         = &FakeCacheEvict;
        init.unit_update_display    = &FakeUpdateDisplay;
        init.texture_reload         = &FakeTextureReload;
        init.client_dbs             = &FakeClientDbs;
        init.model_path             = &FakeModelPath;
        init.model_bytes            = &FakeModelBytes;
        init.model_replace_bytes    = &FakeModelReplace;
        init.map_obj_path           = &FakeMapObjPath;
        init.map_obj_bytes          = &FakeMapObjBytes;
        init.map_area_bytes         = &FakeAreaBytes;
        init.file_name              = &FakeFileName;
        init.map_obj_group_bytes    = &FakeGroupBytes;
        init.model_skin_bytes       = &FakeSkinBytes;
        init.texture_name           = &FakeTextureName;
        init.doodad_position        = &FakeDoodadPosition;
        init.unit_aura_slot         = &FakeAuraSlot;
        init.os_time_ms             = &FakeTimeMs;
        init.loot_object_guid       = &FakeLootGuid;
        init.loot_kind              = &FakeLootKind;
        init.loot_money             = &FakeLootMoney;
        init.loot_slot              = &FakeLootSlot;
        init.loot_next_roll_id      = &FakeNextRoll;
        init.ui_map_id              = &FakeUiMap;
        init.faction_standing       = &FakeFaction;
        init.ui_interact_target     = &FakeInteract;
        init.trade_partner          = &FakeTradePartner;
        init.group_invite_answer    = &FakeGroupAnswer;
        init.resurrect_answer       = &FakeResurrectAnswer;
        init.duel_answer            = &FakeDuelAnswer;
        init.terrain_present        = &FakeTerrainPresent;
        init.terrain_chunk          = &FakeTerrainChunk;
        init.terrain_chunk_view     = &FakeTerrainView;
        init.terrain_chunk_place    = &FakeTerrainPlace;
        init.terrain_render_chunk   = &FakeRenderChunkRead;
        init.terrain_area_file      = &FakeAreaFile;
        init.terrain_vertex_constants = &FakeTerrainConsts;
        init.gx_vertex_constants_set = &FakeVertexConstantsSet;
        init.gx_xform_get           = &FakeXformGet;
        init.gx_xform_set           = &FakeXformSet;
        init.gx_pixel_constants_set = &FakePixelConstantsSet;
        init.gx_texture_stage       = &FakeTextureStage;
        init.gx_texture_stage_set   = &FakeTextureStageSet;
        init.gx_texture_wrap        = &FakeTextureWrap;
        init.gx_render_state        = &FakeRenderState;
        init.gx_render_state_set    = &FakeRenderStateSet;
        init.gx_draw                = &FakeGxDraw;
        init.map_wdt                = &FakeMapWdt;
        init.map_validate_shaders   = &FakeValidateShaders;
        init.gx_shader_record       = &FakeShaderRecord;
        init.gx_shader_code_set     = &FakeShaderCodeSet;
        init.map_area_chunk         = &FakeMapAreaChunk;
        init.map_area_split_set     = &FakeAreaSplitSet;
        init.map_area_placements    = &FakeAreaPlacements;
        init.map_chunk_parts        = &FakeChunkParts;
        init.map_chunk_split_set    = &FakeChunkSplitSet;
        init.map_chunk_create_refs  = &FakeChunkCreateRefs;
        init.terrain_chunk_rebuild  = &FakeTerrainRebuild;
        init.terrain_forget_collision = &FakeForgetCollision;
        init.unit_fall_local        = &FakeFallLocal;
        init.unit_play_animation    = &FakePlayAnimation;
        init.unit_update_animation  = &FakeUpdateAnimation;
        init.unit_shown_animation   = &FakeShownAnimation;
        init.unit_animation_speed   = &FakeAnimationSpeed;
        init.unit_animation_length  = &FakeAnimationLength;
        init.unit_lift_local        = &FakeLiftLocal;
        init.net_send               = &FakeNetSend;
        return init;
    }
}

// --- What a mod receives ----------------------------------------------------------------------

namespace
{
    /// One event as a handler saw it: which, and a copy of the bytes it was handed.
    struct Received
    {
        wxl_event         event;
        std::vector<char> args;

        template <class T>
        T As() const
        {
            T typed{};
            if (args.size() >= sizeof typed) memcpy(&typed, args.data(), sizeof typed);
            return typed;
        }
    };

    std::vector<Received> g_received;

    /// The size of the struct each event carries, so the harness can keep a copy of it.
    size_t ArgsSize(wxl_event event)
    {
        switch (event)
        {
#define WXL_EVENT(family, name, id, args) case WXL_EV_##name: return sizeof(args);
#include "wxl/events.def"
#undef WXL_EVENT
        default: return 0;
        }
    }

    /// One tag per event id; a subscription's `user` is the address of its own, which is how a
    /// handler that is only handed `user` and `args` knows which event arrived.
    wxl_event g_tags[WXL_EV_COUNT]{};

    void WXL_CALL OnEvent(void* user, const void* args)
    {
        const wxl_event event = *static_cast<const wxl_event*>(user);
        Received record{ event, {} };
        const size_t size = ArgsSize(event);
        if (args && size)
        {
            record.args.resize(size);
            memcpy(record.args.data(), args, size);
        }
        g_received.push_back(std::move(record));
    }

    int g_failures = 0;

    /// The harness's own subscription to each event it records, so a check can take it away to
    /// see what a seam does when nobody listens, and give it back.
    wxl_subscription g_heard[WXL_EV_COUNT]{};

    void Mute(wxl_event event)
    {
        g_host->Events().Unsubscribe(g_heard[event]);
        g_heard[event] = WXL_SUBSCRIPTION_NONE;
    }

    void Unmute(wxl_event event)
    {
        if (g_heard[event] == WXL_SUBSCRIPTION_NONE)
            g_heard[event] = g_host->Events().Subscribe(event, &OnEvent, &g_tags[event], 0);
    }

    void Check(bool ok, const char* what)
    {
        printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++g_failures;
    }

    /// How many of an event arrived.
    size_t Count(wxl_event event)
    {
        size_t n = 0;
        for (const Received& record : g_received) n += record.event == event;
        return n;
    }

    /// The one event received, or null when none or more than one arrived.
    const Received* Only(wxl_event event)
    {
        const Received* found = nullptr;
        for (const Received& record : g_received)
        {
            if (record.event != event) continue;
            if (found) return nullptr;
            found = &record;
        }
        return found;
    }
}

// --- The checks -------------------------------------------------------------------------------

namespace
{
    /// How far the client has booted: announced once each, in order, gaps left as gaps.
    void CheckStages()
    {
        namespace stages = wxl::core::stages;
        stages::Restart();

        stages::Reach(WXL_STAGE_EARLY);
        Check(stages::Reached() == WXL_STAGE_EARLY, "the first stage is announced");

        stages::Reach(WXL_STAGE_STORAGE);
        Check(stages::Reached() == WXL_STAGE_STORAGE, "and the next one after it");

        stages::Reach(WXL_STAGE_STORAGE);
        stages::Reach(WXL_STAGE_EARLY);
        Check(stages::Reached() == WXL_STAGE_STORAGE,
              "a stage already announced, and one behind it, are both ignored");

        stages::Reach(WXL_STAGE_SCRIPT);
        Check(stages::Reached() == WXL_STAGE_SCRIPT,
              "a stage that was never reached is skipped, not invented: with no device brought "
              "up, script follows storage and graphics is a gap");

        stages::Restart();
        Check(stages::Reached() == WXL_STAGE_EARLY, "a runtime that starts over starts over");

        stages::Reach(WXL_STAGE_EARLY);
        std::thread([] { stages::Reach(WXL_STAGE_STORAGE); }).join();
        Check(stages::Reached() == WXL_STAGE_EARLY,
              "a stage reached on another thread is not announced there");
        stages::Reach(WXL_STAGE_STORAGE);
        Check(stages::Reached() == WXL_STAGE_STORAGE, "the main thread announces it on its next pass");
        stages::Restart();
    }

    /// The rows a client hands over decide whether its accessors are called at all.
    void CheckInitRows()
    {
        namespace binding = wxl::core::binding;
        char why[512] = {};

        wxl_core_init init = FakeInit();
        init.row_count     = WXL_CORE_INIT_ROW_COUNT;
        init.rows          = wxl_core_init_rows;
        Check(binding::Agrees(init, why, sizeof why), "a client built from the same table agrees");

        const wxl_core_init bare = FakeInit();
        Check(!binding::Agrees(bare, why, sizeof why) && strstr(why, "no rows"),
              "one that hands over no rows is refused");

        std::vector<const char*> rows(wxl_core_init_rows, wxl_core_init_rows + WXL_CORE_INIT_ROW_COUNT);
        rows[5]   = "something: else ()";
        init.rows = rows.data();
        Check(!binding::Agrees(init, why, sizeof why) && strstr(why, "row 5 differs") &&
                  strstr(why, wxl_core_init_rows[5]),
              "one row that differs is refused, naming both sides of it");

        init.rows      = wxl_core_init_rows;
        init.row_count = WXL_CORE_INIT_ROW_COUNT - 3;
        Check(binding::Agrees(init, why, sizeof why), "an older client, with fewer rows, agrees");

        init.row_count   = WXL_CORE_INIT_ROW_COUNT;
        init.struct_size = static_cast<uint32_t>(offsetof(wxl_core_init, map_id) + 8 * sizeof(void*));
        Check(!binding::Agrees(init, why, sizeof why) && strstr(why, "struct_size"),
              "rows that its struct_size does not hold are refused");
    }

    /// Every event the seam table says some body raises is one the host will let a mod subscribe
    /// to. A seam that raises an event the attachment never published is a silent dead event.
    void CheckEveryRaisedEventIsSubscribable()
    {
        for (const wxl_event event : wxl::core::seams::RaisedEvents())
        {
            g_tags[event] = event;
            const wxl_subscription subscription =
                g_host->Events().Subscribe(event, &OnEvent, &g_tags[event], 0);
            g_heard[event] = subscription;
            char what[128];
            snprintf(what, sizeof what, "a mod can subscribe to event %u", unsigned(event));
            Check(subscription != WXL_SUBSCRIPTION_NONE, what);
        }
    }

    /// Every row declares a return type and a convention the generator knows.
    void CheckEveryRowDeclaresItsAbi()
    {
        for (const wxl::core::seams::Seam& seam : wxl::core::seams::Table())
        {
            const bool known = strcmp(seam.convention, "cdecl") == 0 ||
                               strcmp(seam.convention, "thiscall") == 0 ||
                               strcmp(seam.convention, "stdcall") == 0 ||
                               strcmp(seam.convention, "regparm") == 0;
            char what[160];
            snprintf(what, sizeof what, "%s declares %s %s %s", seam.name, seam.returns,
                     seam.convention, seam.params);
            Check(known && seam.returns[0] != 0, what);
        }
    }

    /// Event.DispatchUpdate: Before raises Update carrying the tick's own elapsed_sec.
    void CheckEventDispatchUpdate()
    {
        g_received.clear();
        wxl::seam::Event::DispatchUpdate::Before(nullptr, 1000u, 0.125f);

        const Received* update = Only(WXL_EV_Update);
        Check(update != nullptr, "Event.DispatchUpdate raises Update exactly once");
        if (!update) return;
        Check(update->As<wxl_update_args>().delta == 0.125f,
              "Update carries the seam's elapsed_sec as its delta");
    }

    /// The World seams: CGGameUI::EnterWorld and LeaveWorld raise WorldEnter and WorldLeave once
    /// per entry and exit (login, a teleport to another map, logout), never for a /reload; a
    /// same-map teleport's World::Preload with the loading screen raises the pair while the
    /// player is in the world, and nothing otherwise.
    void CheckWorld()
    {
        FakeObject me;
        me.guid     = 0x0000000000000042ull;
        me.typeId   = WXL_OBJECT_PLAYER;
        me.position = { 5.0f, 6.0f, 7.0f };
        const wxl_vec3 there{ 1.0f, 2.0f, 3.0f };
        auto same = [](const wxl_vec3& a, const wxl_vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };

        // Before any login: no preload enters a world.
        g_mapId = 571;
        g_received.clear();
        wxl::seam::World::Preload::Before(&there, 0);
        wxl::seam::World::Preload::After(&there, 0);
        wxl::seam::World::Preload::Before(&there, 1);
        wxl::seam::World::Preload::After(&there, 1);
        Check(g_received.empty(), "World.Preload raises nothing before the player is in the world");

        // Login: the active player's object arrives and the interface enters the world.
        g_localPlayer = &me;
        g_received.clear();
        wxl::seam::World::EnterWorld::After();
        const Received* entered = Only(WXL_EV_WorldEnter);
        Check(entered != nullptr && Only(WXL_EV_WorldLeave) == nullptr,
              "World.EnterWorld raises WorldEnter at login, and no WorldLeave");
        if (entered)
            Check(entered->As<wxl_world_args>().map_id == 571u && same(entered->As<wxl_world_args>().position, me.position),
                  "WorldEnter carries the loaded map and the player's position");
        g_received.clear();
        wxl::seam::World::EnterWorld::After();
        Check(g_received.empty(), "an entry already announced is not raised again");

        // /reload: the interface leaves and enters the world again, the player never left.
        g_uiReloading = 1;
        g_received.clear();
        wxl::seam::World::LeaveWorld::Before();
        wxl::seam::World::EnterWorld::After();
        g_uiReloading = 0;
        Check(g_received.empty(), "a /reload raises neither WorldLeave nor WorldEnter");

        // A teleport far across the same map: the loading screen of World::Preload.
        g_worldPosition = me.position;
        g_received.clear();
        wxl::seam::World::Preload::Before(&there, 0);
        wxl::seam::World::Preload::After(&there, 0);
        Check(g_received.empty(), "World.Preload raises nothing when show_loading is clear");
        wxl::seam::World::Preload::Before(&there, 1);
        const Received* left = Only(WXL_EV_WorldLeave);
        Check(left != nullptr && Only(WXL_EV_WorldEnter) == nullptr,
              "a same-map teleport's preload raises WorldLeave first");
        if (left)
            Check(left->As<wxl_world_args>().map_id == 571u && same(left->As<wxl_world_args>().position, me.position),
                  "WorldLeave names the map and the place being left, not the destination");
        g_received.clear();
        wxl::seam::World::Preload::After(&there, 1);
        entered = Only(WXL_EV_WorldEnter);
        Check(entered != nullptr, "and WorldEnter after it");
        if (entered)
            Check(entered->As<wxl_world_args>().map_id == 571u && same(entered->As<wxl_world_args>().position, there),
                  "WorldEnter carries the position the preload loaded around");

        // A teleport to another map: the old player object goes while its map is loaded.
        g_received.clear();
        wxl::seam::World::LeaveWorld::Before();
        left = Only(WXL_EV_WorldLeave);
        Check(left != nullptr && left->As<wxl_world_args>().map_id == 571u,
              "World.LeaveWorld raises WorldLeave with the map entered");
        g_received.clear();
        wxl::seam::World::LeaveWorld::Before();
        wxl::seam::World::Preload::Before(&there, 1);
        wxl::seam::World::Preload::After(&there, 1);
        Check(g_received.empty(), "out of the world, neither a leave nor a preload raises anything");
        g_mapId = 0;
        wxl::seam::World::EnterWorld::After();
        entered = Only(WXL_EV_WorldEnter);
        Check(entered != nullptr && entered->As<wxl_world_args>().map_id == 0u,
              "the new map's entry raises WorldEnter with its id");

        // Logout: the player leaves; the next login enters again.
        g_received.clear();
        wxl::seam::World::LeaveWorld::Before();
        left = Only(WXL_EV_WorldLeave);
        Check(left != nullptr && left->As<wxl_world_args>().map_id == 0u, "a logout raises WorldLeave");
        g_received.clear();
        wxl::seam::World::EnterWorld::After();
        Check(Only(WXL_EV_WorldEnter) != nullptr, "and the relog WorldEnter");
        wxl::seam::World::LeaveWorld::Before();

        g_localPlayer   = nullptr;
        g_worldPosition = {};
    }

    /// ObjectMgr: InitObject's After tracks and announces; ObjDelete's Before announces and
    /// untracks, after which the handle a mod kept reads as absent; DestroyRemovedObject
    /// announces only what is still tracked.
    void CheckObjectMgr()
    {
        FakeObject boar;
        boar.guid     = 0xF130000000000123ull;
        boar.typeId   = WXL_OBJECT_UNIT;
        boar.entry    = 113;
        boar.position = { 4.0f, 5.0f, 6.0f };
        boar.health   = 90;

        g_received.clear();
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 1000u);

        const Received* created = Only(WXL_EV_ObjectCreate);
        Check(created != nullptr, "ObjectMgr.InitObject raises ObjectCreate exactly once");
        if (!created) return;
        const wxl_object_args args = created->As<wxl_object_args>();
        Check(args.object != WXL_HANDLE_NONE, "ObjectCreate carries a handle");
        Check(args.guid == boar.guid, "ObjectCreate carries the object's guid, read through the binding");
        Check(args.type == WXL_OBJECT_UNIT, "ObjectCreate carries the client's type id");

        const wxl_object_api* api = wxl::core::objects::Api();
        Check(api->valid(args.object) == WXL_TRUE, "the handle resolves through wxl.object");
        Check(api->guid(args.object) == boar.guid, "and reads the guid");
        int32_t health = 0;
        Check(api->unit_health(args.object, &health) == WXL_OK && health == 90,
              "a unit read goes through the binding to the client's object");
        wxl_vec3 position{};
        Check(api->position(args.object, &position) == WXL_OK && position.y == 5.0f,
              "so does a position");
        Check(api->from_guid(boar.guid) == args.object, "the guid resolves back to the handle");

        // Announcing the same object again tracks it once.
        g_received.clear();
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 1001u);
        Check(Only(WXL_EV_ObjectCreate) != nullptr &&
                  Only(WXL_EV_ObjectCreate)->As<wxl_object_args>().object == args.object,
              "a second InitObject on the same object answers the same handle");

        // ObjDelete: announced while resident, then absent.
        g_received.clear();
        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
        const Received* destroyed = Only(WXL_EV_ObjectDestroy);
        Check(destroyed != nullptr, "ObjectMgr.ObjDelete raises ObjectDestroy exactly once");
        if (destroyed)
        {
            Check(destroyed->As<wxl_object_args>().object == args.object,
                  "ObjectDestroy carries the handle ObjectCreate did");
            Check(destroyed->As<wxl_object_args>().guid == boar.guid, "and its guid");
        }
        Check(api->valid(args.object) == WXL_FALSE, "after ObjDelete the handle no longer resolves");
        Check(api->unit_health(args.object, &health) == WXL_E_NOT_FOUND,
              "a read through the stale handle is absent, not the object's last value");
        Check(api->from_guid(boar.guid) == WXL_HANDLE_NONE, "and the guid resolves to nothing");

        // DestroyRemovedObject on what ObjDelete already let go: nothing.
        g_received.clear();
        wxl::seam::ObjectMgr::DestroyRemovedObject::Before(&boar);
        Check(g_received.empty(), "DestroyRemovedObject after ObjDelete raises nothing");

        // DestroyRemovedObject on a tracked object (the DestroyAllObjects path): once.
        FakeObject chest;
        chest.guid   = 0xF110000000000456ull;
        chest.typeId = WXL_OBJECT_GAMEOBJECT;
        wxl::seam::ObjectMgr::InitObject::After(&chest, nullptr, 1002u);
        g_received.clear();
        wxl::seam::ObjectMgr::DestroyRemovedObject::Before(&chest);
        Check(Only(WXL_EV_ObjectDestroy) != nullptr &&
                  Only(WXL_EV_ObjectDestroy)->As<wxl_object_args>().type == WXL_OBJECT_GAMEOBJECT,
              "DestroyRemovedObject on a tracked object raises ObjectDestroy once");
        Check(api->valid(Only(WXL_EV_ObjectDestroy)->As<wxl_object_args>().object) == WXL_FALSE,
              "and the handle is gone after it");
    }

    /// GetUpdateObject: an object brought back from the lazy-cleanup set is tracked again with a
    /// handle of its own and announced; one already tracked raises nothing; from_guid finds what
    /// the client holds even when no seam announced it.
    void CheckRevivedObjects()
    {
        const wxl_object_api* api = wxl::core::objects::Api();

        FakeObject wolf;
        wolf.guid   = 0xF130000000000789ull;
        wolf.typeId = WXL_OBJECT_UNIT;
        wolf.health = 70;

        g_received.clear();
        wxl::seam::ObjectMgr::InitObject::After(&wolf, nullptr, 8000u);
        const wxl_handle first = Only(WXL_EV_ObjectCreate) ? Only(WXL_EV_ObjectCreate)->As<wxl_object_args>().object
                                                           : WXL_HANDLE_NONE;
        int32_t reenabled = 0;
        g_received.clear();
        Check(wxl::core::objects::IsTracked(&wolf), "a created object is tracked");
        wxl::seam::ObjectMgr::GetUpdateObject::After(&wolf, wolf.guid, &reenabled);
        Check(g_received.empty(), "GetUpdateObject on a tracked object raises nothing");

        // Out of range: ObjDelete lets it go to the lazy-cleanup set, its handle goes stale.
        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
        Check(api->valid(first) == WXL_FALSE && api->from_guid(wolf.guid) == WXL_HANDLE_NONE,
              "after ObjDelete neither the handle nor the guid resolves");

        // Back in range before the client freed it: GetUpdateObject revives the same object.
        g_received.clear();
        reenabled = 1;
        wxl::seam::ObjectMgr::GetUpdateObject::After(&wolf, wolf.guid, &reenabled);
        const Received* revived = Only(WXL_EV_ObjectCreate);
        Check(revived != nullptr, "GetUpdateObject on a revived object raises ObjectCreate exactly once");
        if (!revived) return;
        const wxl_object_args args = revived->As<wxl_object_args>();
        Check(args.object != WXL_HANDLE_NONE && args.object != first,
              "with a new handle: one per visibility episode");
        Check(args.guid == wolf.guid && args.type == WXL_OBJECT_UNIT, "and the object's guid and type");
        Check(api->valid(args.object) == WXL_TRUE && api->valid(first) == WXL_FALSE,
              "the new handle resolves and the old one stays stale");
        Check(api->from_guid(wolf.guid) == args.object, "the guid resolves to the new handle");
        int32_t health = 0;
        Check(api->unit_health(args.object, &health) == WXL_OK && health == 70,
              "and reads reach the revived object");

        g_received.clear();
        wxl::seam::ObjectMgr::GetUpdateObject::After(&wolf, wolf.guid, &reenabled);
        wxl::seam::ObjectMgr::GetUpdateObject::After(nullptr, 0x1234ull, &reenabled);
        Check(g_received.empty(), "the next update of it raises nothing, nor does an update of nothing");

        // Revived while still on the reenabled list: `*reenabled` stays clear, it is tracked anyway.
        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
        g_received.clear();
        reenabled = 0;
        wxl::seam::ObjectMgr::GetUpdateObject::After(&wolf, wolf.guid, &reenabled);
        Check(Only(WXL_EV_ObjectCreate) != nullptr &&
                  Only(WXL_EV_ObjectCreate)->As<wxl_object_args>().object != args.object,
              "an untracked object handed back without the reenabled flag is announced too");

        g_received.clear();
        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
        Check(Only(WXL_EV_ObjectDestroy) != nullptr, "and leaves with ObjectDestroy like any other");

        // from_guid through the client: an object the client holds that no seam announced.
        FakeObject ghost;
        ghost.guid   = 0xF130000000000999ull;
        ghost.typeId = WXL_OBJECT_UNIT;
        g_held.push_back(&ghost);
        g_received.clear();
        const wxl_handle found = api->from_guid(ghost.guid);
        Check(found != WXL_HANDLE_NONE && api->valid(found) == WXL_TRUE,
              "from_guid resolves a guid the client holds, through ClntObjMgrObjectPtr");
        Check(g_received.empty(), "tracking it on demand raises nothing");
        Check(api->from_guid(ghost.guid) == found, "and the next lookup answers the same handle");
        Check(api->from_guid(0xF130000000000998ull) == WXL_HANDLE_NONE,
              "a guid the client does not hold resolves to nothing");
        wxl::seam::ObjectMgr::ObjDelete::Before(&ghost);
        Check(Only(WXL_EV_ObjectDestroy) != nullptr &&
                  Only(WXL_EV_ObjectDestroy)->As<wxl_object_args>().object == found,
              "an object tracked on demand is announced when it leaves");
        g_held.clear();
        Check(api->from_guid(ghost.guid) == WXL_HANDLE_NONE, "and is not found once the client let it go");
    }

    /// The descriptor fields: read_fields and write_fields through the block the client hands
    /// over; watch_field setting the client's mirror handlers -- on the kinds while in a world,
    /// again at each InitializeShared, on the active player for its local fields -- and a
    /// handler's report raising ObjectFieldChanged.
    void CheckFields()
    {
        const wxl_object_api* api = wxl::core::objects::Api();

        FakeObject me;
        me.guid   = 0x0000000000000042ull;
        me.typeId = WXL_OBJECT_PLAYER;
        me.block.assign(0x52E, 0u);
        me.block[0x18]  = 5000;
        me.block[0x27A] = 100; // PLAYER_XP
        FakeObject troll;
        troll.guid   = 0x0000000000000043ull;
        troll.typeId = WXL_OBJECT_PLAYER;
        troll.block.assign(0x52E, 0u);
        FakeObject wolf;
        wolf.guid   = 0xF130000000000555ull;
        wolf.typeId = WXL_OBJECT_UNIT;
        wolf.block.assign(0x94, 0u);
        wolf.block[0x18] = 70;
        g_held = {&me, &troll, &wolf};

        g_mirrors.clear();
        wxl::seam::ObjectMgr::DestroyShared::Before(); // out of any world
        g_received.clear();
        wxl::seam::ObjectMgr::InitObject::After(&wolf, nullptr, 9000u);
        wxl::seam::ObjectMgr::InitObject::After(&troll, nullptr, 9001u);
        const wxl_handle hWolf  = api->from_guid(wolf.guid);
        const wxl_handle hTroll = api->from_guid(troll.guid);

        uint32_t value = 0;
        Check(api->field_count(hWolf) == 0x94 && api->read_fields(hWolf, 0x18, 1, &value) == WXL_OK && value == 70,
              "read_fields reads a unit's block through object_descriptor");
        Check(api->field_count(hTroll) == 0x144 && api->read_fields(hTroll, 0x27A, 1, &value) == WXL_E_INVALID,
              "a player other than the active one has no fields past PLAYER_FIELD_PAD_0");
        const uint32_t sixty = 60;
        Check(api->write_fields(hWolf, 0x18, 1, &sixty, WXL_FIELD_WRITE_PLAIN) == WXL_OK && wolf.block[0x18] == 60,
              "write_fields writes the client's dword");
        Check(api->write_fields(hWolf, 0x95, 1, &sixty, 0) == WXL_E_INVALID,
              "and refuses a dword past the block");

        // A watch asked for out of a world waits for the next one.
        Check(api->watch_field(WXL_OBJECT_UNIT, 0x18, 1) == WXL_OK && g_mirrors.empty(),
              "a watch asked for outside a world sets nothing on the client yet");
        wxl::seam::ObjectMgr::InitializeShared::After();
        Check(g_mirrors.size() == 1 && g_mirrors[0].guid == 0 && g_mirrors[0].type == WXL_OBJECT_UNIT &&
                  g_mirrors[0].offset == (0x18 - 6) * 4 && g_mirrors[0].size == 4,
              "InitializeShared sets it on the unit's fields, offset from their first");
        Check(api->watch_field(WXL_OBJECT_UNKNOWN, 0x3, 1) == WXL_OK && g_mirrors.size() == 2 &&
                  g_mirrors[1].type == WXL_OBJECT_UNKNOWN && g_mirrors[1].offset == 12,
              "in a world, a new watch is set at once, OBJECT_FIELD_ENTRY on every object's");
        Check(api->watch_field(WXL_OBJECT_UNIT, 0x10, 2) == WXL_E_UNSUPPORTED && g_mirrors.size() == 2,
              "a field the client keeps no copy of is refused and sets nothing");

        // The local fields go on the active player, when there is one.
        Check(api->watch_field(WXL_OBJECT_PLAYER, 0x27A, 1) == WXL_OK && g_mirrors.size() == 2,
              "PLAYER_XP waits for the active player");
        g_localPlayer = &me;
        wxl::seam::ObjectMgr::InitObject::After(&me, nullptr, 9002u);
        Check(g_mirrors.size() == 3 && g_mirrors[2].guid == me.guid && g_mirrors[2].type == WXL_OBJECT_PLAYER &&
                  g_mirrors[2].offset == (0x27A - 0x94) * 4,
              "and is set on it, by guid, when the client builds it");
        wxl::seam::ObjectMgr::InitObject::After(&me, nullptr, 9003u);
        Check(g_mirrors.size() == 3, "once");

        // The client reports a change through the handler it was given.
        const FakeMirror health = g_mirrors[0];
        const uint32_t   was    = 70;
        wolf.block[0x18]        = 55;
        g_received.clear();
        health.handler(wolf.guid, health.offset, health.size, &was, health.param);
        const Received* changed = Only(WXL_EV_ObjectFieldChanged);
        Check(changed != nullptr, "a report becomes ObjectFieldChanged");
        if (changed)
        {
            const wxl_object_field_args args = changed->As<wxl_object_field_args>();
            Check(args.object == hWolf && args.type == WXL_OBJECT_UNIT && args.field == 0x18 && args.count == 1 &&
                      args.old_value == 70 && args.new_value == 55,
                  "with the object, its type, the field's index, its old value and the new one");
        }
        const uint32_t xp = 100;
        me.block[0x27A]   = 250;
        g_received.clear();
        g_mirrors[2].handler(me.guid, g_mirrors[2].offset, g_mirrors[2].size, &xp, g_mirrors[2].param);
        Check(Only(WXL_EV_ObjectFieldChanged) &&
                  Only(WXL_EV_ObjectFieldChanged)->As<wxl_object_field_args>().new_value == 250,
              "the active player's experience too");
        g_received.clear();
        health.handler(0x1234ull, health.offset, health.size, &was, health.param);
        Check(g_received.empty(), "a report on an object the client does not hold raises nothing");

        // Leaving the world drops the client's handlers; the next world gets them again.
        wxl::seam::ObjectMgr::DestroyShared::Before();
        g_mirrors.clear();
        wxl::seam::ObjectMgr::InitializeShared::After();
        Check(g_mirrors.size() == 2, "the next world's InitializeShared sets the kinds' handlers again");
        wxl::seam::ObjectMgr::InitObject::After(&me, nullptr, 9004u);
        Check(g_mirrors.size() == 3 && g_mirrors[2].guid == me.guid,
              "and the active player's local ones when the client builds it again");

        // A watch belongs to the mod that asked for it, and only that mod receives its changes.
        // Mod 7 subscribes like any mod; the harness, which is nobody's code, watched the rest.
        constexpr wxl::host::ModHost::ModToken kMod = 7;
        const wxl_subscription modSub =
            g_host->Events().Subscribe(WXL_EV_ObjectFieldChanged, &OnModFieldChanged, nullptr, 0, kMod);
        const auto asMod = [&](auto call) {
            const wxl::host::ModHost::ModToken previous = g_host->SwapCurrentMod(kMod);
            const wxl_result                   result   = call();
            g_host->SwapCurrentMod(previous);
            return result;
        };
        const size_t before = g_mirrors.size();
        Check(asMod([&] { return api->watch_field(WXL_OBJECT_UNIT, 0x20, 1); }) == WXL_OK &&
                  g_mirrors.size() == before + 1,
              "a mod's watch of UNIT_FIELD_MAXHEALTH sets one handler on the client");
        const FakeMirror maxHealth = g_mirrors.back();
        const uint32_t   wasMax    = 90;
        wolf.block[0x20]           = 120;
        g_received.clear();
        g_modChanges.clear();
        maxHealth.handler(wolf.guid, maxHealth.offset, maxHealth.size, &wasMax, maxHealth.param);
        Check(g_modChanges.size() == 1 && g_modChanges[0].field == 0x20 && g_modChanges[0].new_value == 120 &&
                  Count(WXL_EV_ObjectFieldChanged) == 0,
              "its change reaches that mod's handler and no one else's");
        const FakeMirror healthNow = g_mirrors[0];
        g_received.clear();
        g_modChanges.clear();
        healthNow.handler(wolf.guid, healthNow.offset, healthNow.size, &was, healthNow.param);
        Check(g_modChanges.empty() && Count(WXL_EV_ObjectFieldChanged) == 1,
              "and a change the mod did not watch reaches only who watched it");

        Check(asMod([&] { return api->watch_field(WXL_OBJECT_UNIT, 0x18, 1); }) == WXL_OK &&
                  g_mirrors.size() == before + 1,
              "a second mod's watch of a watched field sets no second handler");
        g_received.clear();
        g_modChanges.clear();
        healthNow.handler(wolf.guid, healthNow.offset, healthNow.size, &was, healthNow.param);
        Check(g_modChanges.size() == 1 && Count(WXL_EV_ObjectFieldChanged) == 1, "and both receive it");
        Check(asMod([&] { return api->unwatch_field(WXL_OBJECT_UNIT, 0x18, 1); }) == WXL_OK &&
                  g_mirrors.size() == before + 1,
              "giving one watch back keeps the handler the other still needs");
        g_received.clear();
        g_modChanges.clear();
        healthNow.handler(wolf.guid, healthNow.offset, healthNow.size, &was, healthNow.param);
        Check(g_modChanges.empty() && Count(WXL_EV_ObjectFieldChanged) == 1, "and the other goes on receiving");
        Check(asMod([&] { return api->unwatch_field(WXL_OBJECT_UNIT, 0x18, 1); }) == WXL_OK,
              "giving back what is not watched is no mistake");
        Check(api->unwatch_field(WXL_OBJECT_UNIT, 0x19, 2) == WXL_OK &&
                  api->unwatch_field(WXL_OBJECT_UNIT, 0x95, 1) == WXL_E_INVALID,
              "unwatch_field checks its range as watch_field does");

        Check(wxl::core::objects::Service().WatchCount(kMod) == 1, "the mod holds one watch");
        wxl::core::objects::Service().DropWatches(kMod);
        Check(wxl::core::objects::Service().WatchCount(kMod) == 0 && g_mirrors.size() == before,
              "a mod that goes gives back every watch, and the handler no one needs leaves the client");
        g_modChanges.clear();
        maxHealth.handler(wolf.guid, maxHealth.offset, maxHealth.size, &wasMax, maxHealth.param);
        Check(g_modChanges.empty(), "a report still in flight for it raises nothing");

        Check(api->unwatch_field(WXL_OBJECT_UNKNOWN, 0x3, 1) == WXL_OK && g_mirrors.size() == before - 1,
              "the last watch of OBJECT_FIELD_ENTRY takes its handler off the kind");
        Check(api->unwatch_field(WXL_OBJECT_PLAYER, 0x27A, 1) == WXL_OK && g_mirrors.size() == before - 2 &&
                  std::none_of(g_mirrors.begin(), g_mirrors.end(), [&](const FakeMirror& m) { return m.guid == me.guid; }),
              "and the last watch of PLAYER_XP its handler off the active player");
        Check(api->watch_field(WXL_OBJECT_UNKNOWN, 0x3, 1) == WXL_OK && g_mirrors.size() == before - 1,
              "watching again sets it again");
        g_host->Events().Unsubscribe(modSub);

        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
        wxl::seam::ObjectMgr::ObjDelete::Before(&troll);
        wxl::seam::ObjectMgr::ObjDelete::Before(&me);
        g_localPlayer = nullptr;
        g_held.clear();
    }

    /// Unit: OnDeath's After raises UnitDeath for a tracked unit; the aura pair raises
    /// UnitAuraChanged with the spell and the direction; an untracked unit raises nothing.
    void CheckUnit()
    {
        FakeObject wolf;
        wolf.guid   = 0xF130000000000789ull;
        wolf.typeId = WXL_OBJECT_UNIT;
        wxl::seam::ObjectMgr::InitObject::After(&wolf, nullptr, 2000u);
        const wxl_handle handle = wxl::core::objects::HandleOf(&wolf);

        g_received.clear();
        wxl::seam::Unit::OnDeath::After(&wolf);
        Check(Only(WXL_EV_UnitDeath) != nullptr &&
                  Only(WXL_EV_UnitDeath)->As<wxl_unit_args>().unit == handle,
              "Unit.OnDeath raises UnitDeath once with the unit's handle");

        g_received.clear();
        wxl::seam::Unit::OnAuraAdded::After(&wolf, 3u, 0, nullptr, 8921);
        const Received* added = Only(WXL_EV_UnitAuraChanged);
        Check(added != nullptr, "Unit.OnAuraAdded raises UnitAuraChanged once");
        if (added)
        {
            const wxl_unit_aura_args args = added->As<wxl_unit_aura_args>();
            Check(args.unit == handle && args.spell_id == 8921u && args.removed == WXL_FALSE,
                  "with the unit, the spell, and removed clear");
        }

        g_received.clear();
        wxl::seam::Unit::OnAuraRemoved::After(&wolf, 3u, 0, nullptr, 8921);
        const Received* removed = Only(WXL_EV_UnitAuraChanged);
        Check(removed != nullptr && removed->As<wxl_unit_aura_args>().removed == WXL_TRUE,
              "Unit.OnAuraRemoved raises UnitAuraChanged once with removed set");

        FakeObject stranger;
        stranger.guid   = 0xF13000000000ABCDull;
        stranger.typeId = WXL_OBJECT_UNIT;
        g_received.clear();
        wxl::seam::Unit::OnDeath::After(&stranger);
        wxl::seam::Unit::OnAuraAdded::After(&stranger, 0u, 0, nullptr, 1);
        Check(g_received.empty(), "a unit the runtime never tracked raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
    }

    /// Player: UpdateInventoryComponent raises EquipmentChanged when the slot's item id moved,
    /// with the new id, and nothing for an enchant-only refresh.
    void CheckPlayer()
    {
        FakeObject me;
        me.guid            = 0x0000000000000042ull;
        me.typeId          = WXL_OBJECT_PLAYER;
        me.visibleItems[15] = 19019;
        wxl::seam::ObjectMgr::InitObject::After(&me, nullptr, 3000u);
        const wxl_handle handle = wxl::core::objects::HandleOf(&me);

        FakeVisibleItem old;
        old.itemId = 0;
        g_received.clear();
        wxl::seam::Player::UpdateInventoryComponent::Before(&me, 15, &old);
        wxl::seam::Player::UpdateInventoryComponent::After(&me, 15, &old);
        const Received* equipped = Only(WXL_EV_EquipmentChanged);
        Check(equipped != nullptr, "Player.UpdateInventoryComponent raises EquipmentChanged once when the slot filled");
        if (equipped)
        {
            const wxl_equipment_args args = equipped->As<wxl_equipment_args>();
            Check(args.unit == handle && args.slot == 15u && args.item_id == 19019u,
                  "with the player's handle, the slot and the new item id");
        }

        old.itemId = 19019;
        g_received.clear();
        wxl::seam::Player::UpdateInventoryComponent::Before(&me, 15, &old);
        wxl::seam::Player::UpdateInventoryComponent::After(&me, 15, &old);
        Check(g_received.empty(), "the same id again (an enchant refresh) raises nothing");

        me.visibleItems[15] = 0;
        g_received.clear();
        wxl::seam::Player::UpdateInventoryComponent::Before(&me, 15, &old);
        wxl::seam::Player::UpdateInventoryComponent::After(&me, 15, &old);
        Check(Only(WXL_EV_EquipmentChanged) != nullptr &&
                  Only(WXL_EV_EquipmentChanged)->As<wxl_equipment_args>().item_id == 0u,
              "clearing the slot raises EquipmentChanged with an item id of 0");

        wxl::seam::ObjectMgr::ObjDelete::Before(&me);
    }

    /// The typed reads (abi 0.1.17) a runtime answers itself, through wxl.object over the
    /// binding: the client's own functions and members, as the fake client hands them over.
    void CheckTypedReads()
    {
        const wxl_object_api* api = wxl::core::objects::Api();

        FakeObject me;
        me.guid   = 0x0000000000000042ull;
        me.typeId = WXL_OBJECT_PLAYER;
        FakeObject wolf;
        wolf.guid         = 0xF130000000000789ull;
        wolf.typeId       = WXL_OBJECT_UNIT;
        wolf.reaction     = 1;
        wolf.creatureType = 1;
        wolf.family       = 1;
        wolf.rank         = 2;
        wolf.mountModel   = 0;
        wolf.speeds[1]    = 7.0f;
        wolf.castingSpellId = 133;
        wolf.castEnd        = 1500;
        g_unitAuras[&wolf]  = {FakeAura{0x42, 1459, 0x10, 80, 0, 1800000, 1801000},
                               FakeAura{0x42, 9484, 0x80, 80, 3, 0, 0}};
        FakeObject stone;
        stone.guid   = 0x4000000000000123ull;
        stone.typeId = WXL_OBJECT_ITEM;

        g_localPlayer = &me;
        wxl::seam::ObjectMgr::InitObject::After(&me, nullptr, 100u);
        wxl::seam::ObjectMgr::InitObject::After(&wolf, nullptr, 100u);
        wxl::seam::ObjectMgr::InitObject::After(&stone, nullptr, 100u);
        const wxl_handle player = wxl::core::objects::HandleOf(&me);
        const wxl_handle unit   = wxl::core::objects::HandleOf(&wolf);
        const wxl_handle item   = wxl::core::objects::HandleOf(&stone);

        uint32_t value = 0;
        wxl_bool yes   = WXL_FALSE;
        Check(api->unit_reaction(unit, &value) == WXL_OK && value == WXL_REACTION_HOSTILE,
              "unit_reaction asks the client's UnitReaction towards the active player");
        Check(api->unit_reaction_to(unit, player, &value) == WXL_OK && value == WXL_REACTION_HOSTILE &&
                  api->unit_reaction_to(unit, item, &value) == WXL_E_INVALID,
              "unit_reaction_to another unit, refused for an item");
        Check(api->unit_can_attack(unit, &yes) == WXL_OK && yes == WXL_TRUE &&
                  api->unit_can_attack(item, &yes) == WXL_E_INVALID,
              "unit_can_attack asks the client's CanAttack from the active player, refused for an item");
        yes = WXL_TRUE;
        Check(api->unit_can_interact(unit, &yes) == WXL_OK && yes == WXL_FALSE,
              "unit_can_interact: a unit that offers nothing (no NPC flags) cannot be talked to");
        {
            FakeObject herb;
            herb.guid        = 0xF110000000000456ull;
            herb.typeId      = WXL_OBJECT_GAMEOBJECT;
            herb.clientFlags = 0x2;
            wxl::seam::ObjectMgr::InitObject::After(&herb, nullptr, 100u);
            const wxl_handle node = wxl::core::objects::HandleOf(&herb);
            yes = WXL_FALSE;
            Check(api->gameobject_can_use(node, &yes) == WXL_OK && yes == WXL_TRUE &&
                      api->gameobject_can_use(unit, &yes) == WXL_E_INVALID,
                  "gameobject_can_use asks the client's own test, refused for a unit");
            wxl::seam::ObjectMgr::ObjDelete::Before(&herb);
        }
        Check(api->unit_creature_type(unit, &value) == WXL_OK && value == WXL_CREATURE_BEAST &&
                  api->unit_creature_family(unit, &value) == WXL_OK && value == 1 &&
                  api->unit_classification(unit, &value) == WXL_OK && value == WXL_CLASSIFICATION_RARE_ELITE,
              "the creature's type, family and rank are the client's");
        float speed = 0.0f;
        Check(api->unit_speed(unit, WXL_MOVE_RUN, &speed) == WXL_OK && speed == 7.0f &&
                  api->unit_speed(unit, 9, &speed) == WXL_E_INVALID,
              "a speed by wxl_move_type, invalid past the ninth");
        Check(api->unit_is_mounted(unit, &yes) == WXL_OK && yes == WXL_FALSE, "no mount model, not mounted");
        wolf.mountModel = 14334;
        Check(api->unit_is_mounted(unit, &yes) == WXL_OK && yes == WXL_TRUE, "a mount model, mounted");
        wolf.clientFlags = 0x10000000u;
        Check(api->unit_is_mounted(unit, &yes) == WXL_OK && yes == WXL_FALSE,
              "unless the client flags the mount as going, as IsMounted reads it");
        wolf.clientFlags = 0x200000u;
        Check(api->unit_is_swimming(unit, &yes) == WXL_OK && yes == WXL_TRUE, "swimming is the client's flag");
        g_timeMs = 1000;
        Check(api->unit_is_casting(unit, &yes) == WXL_OK && yes == WXL_TRUE, "a cast whose end is to come is a cast");
        g_timeMs = 1500;
        Check(api->unit_is_casting(unit, &yes) == WXL_OK && yes == WXL_FALSE, "and none once its end has passed");
        Check(api->unit_aura_count(unit, &value) == WXL_OK && value == 2, "the client's aura count");
        wxl_aura aura{};
        aura.struct_size = sizeof aura;
        Check(api->unit_aura(unit, 1, &aura) == WXL_OK && aura.spell_id == 9484 && aura.stacks == 3 &&
                  aura.flags == 0x80 && aura.caster == 0x42 && aura.level == 80,
              "an aura's spell, stacks, flags, caster and level");
        Check(api->unit_aura(unit, 0, &aura) == WXL_OK && aura.duration == 1800000 && aura.expiration == 1801000,
              "its duration and expiration");
        Check(api->unit_aura(unit, 2, &aura) == WXL_E_INVALID, "a slot past the count is invalid");
        Check(api->unit_has_aura(unit, 1459, &yes) == WXL_OK && yes == WXL_TRUE &&
                  api->unit_has_aura(unit, 1, &yes) == WXL_OK && yes == WXL_FALSE,
              "has_aura, through the shared runtime, over the client's auras");
        me.knownSpell = 133;
        Check(api->unit_knows_spell(player, 133, &yes) == WXL_OK && yes == WXL_TRUE &&
                  api->unit_knows_spell(player, 134, &yes) == WXL_OK && yes == WXL_FALSE,
              "knows_spell asks the client's spell book");

        Check(api->item_quality(item, &value) == WXL_E_FAILED, "an item the cache lacks answers failed");
        const FakeStats hearth{1, 1, 0, 15, 0, 0, 1, 0};
        stone.stats = &hearth;
        Check(api->item_quality(item, &value) == WXL_OK && value == WXL_QUALITY_COMMON &&
                  api->item_class(item, &value) == WXL_OK && value == 15 &&
                  api->item_max_stack(item, &value) == WXL_OK && value == 1,
              "and the cache's record once the client has it");

        g_party[1] = 0x77;
        g_raidCount = 0;
        wxl_guid guid = 0;
        Check(api->player_party_size(player, &value) == WXL_OK && value == 1 &&
                  api->player_party_member_guid(player, 1, &guid) == WXL_OK && guid == 0x77 &&
                  api->player_party_member_guid(player, 4, &guid) == WXL_E_INVALID,
              "the party: its size and its members' guids by slot");
        Check(api->player_party_size(unit, &value) == WXL_E_INVALID, "the group is the active player's alone");
        g_party[1] = 0;

        g_unitAuras.clear();
        wxl::seam::ObjectMgr::ObjDelete::Before(&stone);
        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
        wxl::seam::ObjectMgr::ObjDelete::Before(&me);
        g_localPlayer = nullptr;
    }

    /// wxl.action over the binding: the thread, the world and the object checked first, then
    /// the client asked.
    void CheckActions()
    {
        const wxl_action_api& act = wxl::core::services::ActionApi();

        FakeObject me;
        me.guid   = 0x42;
        me.typeId = WXL_OBJECT_PLAYER;
        FakeObject wolf;
        wolf.guid   = 0x789;
        wolf.typeId = WXL_OBJECT_UNIT;
        FakeObject stone;
        stone.guid   = 0x123;
        stone.typeId = WXL_OBJECT_ITEM;
        wxl::seam::ObjectMgr::InitObject::After(&me, nullptr, 100u);
        wxl::seam::ObjectMgr::InitObject::After(&wolf, nullptr, 100u);
        wxl::seam::ObjectMgr::InitObject::After(&stone, nullptr, 100u);
        const wxl_handle unit = wxl::core::objects::HandleOf(&wolf);
        const wxl_handle item = wxl::core::objects::HandleOf(&stone);

        g_actions.clear();
        Check(act.target(unit) == WXL_E_UNSUPPORTED && g_actions.empty(), "out of the world nothing is asked");
        g_localPlayer = &me;
        Check(act.target(unit) == WXL_OK && act.clear_target() == WXL_OK && act.interact(unit) == WXL_OK &&
                  act.cast_spell(133, unit) == WXL_OK && act.cast_spell(133, WXL_HANDLE_NONE) == WXL_OK &&
                  act.stop_casting() == WXL_OK && act.use_item(item, unit) == WXL_OK && act.follow(unit) == WXL_OK &&
                  act.move_to(wxl_vec3{12.0f, 0.0f, 0.0f}) == WXL_OK && act.face(1.0) == WXL_OK &&
                  act.face(-1.0) == WXL_OK,
              "every action reaches the client");
        // Facings as hundredths of a radian: 1, and -1 wrapped into [0, 2 pi).
        const std::vector<std::string> expected = {"target 789 0", "target 0 0",    "interact 789 0",
                                                   "cast 85 789",  "cast 85 0",     "stop 0 0",
                                                   "use 123 789",  "follow 42 789", "move 42 c",
                                                   "face 42 64",   "face 42 210"};
        Check(g_actions == expected, "with the guids and the spell, none for the current target");
        Check(act.assist(unit) == WXL_E_FAILED, "assisting a unit that targets nothing fails");
        Check(act.use_item(unit, WXL_HANDLE_NONE) == WXL_E_INVALID && act.follow(item) == WXL_E_INVALID,
              "an object of the wrong kind is invalid");
        Check(act.cast_spell(0, WXL_HANDLE_NONE) == WXL_E_INVALID, "a spell id of 0 is invalid");
        Check(act.face(std::numeric_limits<double>::quiet_NaN()) == WXL_E_INVALID, "a facing that is no number is invalid");
        Check(act.target(0xDEAD) == WXL_E_NOT_FOUND, "a stale handle is gone");
        g_refuseActions = true;
        Check(act.interact(unit) == WXL_E_FAILED, "what the client refuses answers failed");
        g_refuseActions = false;

        g_actions.clear();
        wxl_result fromThread = WXL_OK;
        std::thread([&] { fromThread = act.target(unit); }).join();
        Check(fromThread == WXL_E_INVALID && g_actions.empty(), "off the main thread an action is refused");

        wxl::seam::ObjectMgr::ObjDelete::Before(&stone);
        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
        wxl::seam::ObjectMgr::ObjDelete::Before(&me);
        g_localPlayer = nullptr;
    }

    /// GameUI: the target is announced once per change, with the previous and current handles,
    /// through SendTarget after Target's internal clear, and through a standalone ClearTarget.
    void CheckGameUI()
    {
        FakeObject rat;
        rat.guid   = 0xF130000000000AAAull;
        rat.typeId = WXL_OBJECT_UNIT;
        FakeObject bat;
        bat.guid   = 0xF130000000000BBBull;
        bat.typeId = WXL_OBJECT_UNIT;
        wxl::seam::ObjectMgr::InitObject::After(&rat, nullptr, 4000u);
        wxl::seam::ObjectMgr::InitObject::After(&bat, nullptr, 4001u);
        const wxl_handle ratHandle = wxl::core::objects::HandleOf(&rat);
        const wxl_handle batHandle = wxl::core::objects::HandleOf(&bat);

        // CGGameUI::Target(rat): ClearTarget(old, 0), the static written, SendTarget(rat).
        g_received.clear();
        wxl::seam::GameUI::ClearTarget::After(0, 0);
        g_targetGuid = rat.guid;
        wxl::seam::GameUI::SendTarget::After(rat.guid);
        const Received* first = Only(WXL_EV_TargetChanged);
        Check(first != nullptr, "targeting from nothing raises TargetChanged once");
        if (first)
            Check(first->As<wxl_target_args>().previous == WXL_HANDLE_NONE &&
                      first->As<wxl_target_args>().current == ratHandle,
                  "from the null handle to the target's");

        // CGGameUI::Target(bat): the internal clear is not announced, the change is, once.
        g_received.clear();
        g_targetGuid = 0;
        wxl::seam::GameUI::ClearTarget::After(rat.guid, 0);
        Check(g_received.empty(), "Target's own pre-clear (a2 clear) announces nothing");
        g_targetGuid = bat.guid;
        wxl::seam::GameUI::SendTarget::After(bat.guid);
        const Received* second = Only(WXL_EV_TargetChanged);
        Check(second != nullptr, "retargeting raises TargetChanged once");
        if (second)
            Check(second->As<wxl_target_args>().previous == ratHandle &&
                      second->As<wxl_target_args>().current == batHandle,
                  "from the previous target's handle to the new one's");

        // The summon-friend script: SendTarget with the static unchanged announces nothing.
        g_received.clear();
        wxl::seam::GameUI::SendTarget::After(rat.guid);
        Check(g_received.empty(), "SendTarget without a change of the static announces nothing");

        // A standalone ClearTarget (a2 set): announced, to the null handle.
        g_received.clear();
        g_targetGuid = 0;
        wxl::seam::GameUI::ClearTarget::After(bat.guid, 1);
        const Received* cleared = Only(WXL_EV_TargetChanged);
        Check(cleared != nullptr, "a standalone ClearTarget raises TargetChanged once");
        if (cleared)
            Check(cleared->As<wxl_target_args>().previous == batHandle &&
                      cleared->As<wxl_target_args>().current == WXL_HANDLE_NONE,
                  "from the target's handle to the null handle");

        wxl::seam::ObjectMgr::ObjDelete::Before(&rat);
        wxl::seam::ObjectMgr::ObjDelete::Before(&bat);
    }

    /// Listening: the host's answer follows the subscriptions, and so do the flags the x64
    /// client tests before a seam.
    void CheckListening()
    {
        Check(g_host->Listening(WXL_EV_TextureLoad), "an event the harness subscribed to is listened to");
        Mute(WXL_EV_TextureLoad);
        Check(!g_host->Listening(WXL_EV_TextureLoad), "with its one subscription gone, nobody listens");
        Unmute(WXL_EV_TextureLoad);
        Check(g_host->Listening(WXL_EV_TextureLoad), "and subscribing again is heard at once");
        Mute(WXL_EV_ModelBatchDrawn);
        const bool quiet = !wxl::seam::Listening(WXL_EV_ModelBatchDrawn);
        Unmute(WXL_EV_ModelBatchDrawn);
        Check(quiet && wxl::seam::Listening(WXL_EV_ModelBatchDrawn),
              "the flag a hot seam reads first follows its event's first and last handler");
    }

    /// Texture, M2Shared and MapObj: an asset named, read or walked raises its event with what
    /// the binding reads, and nothing is read when nobody listens.
    void CheckAssets()
    {
        g_received.clear();
        wxl::seam::Texture::Create::Before("Interface\\Icons\\Spell_Fire_Fireball.blp", 0u, nullptr, 0u);
        const Received* texture = Only(WXL_EV_TextureLoad);
        Check(texture && texture->As<wxl_texture_load_args>().path.size == 39,
              "Texture.Create raises TextureLoad with the name asked for");
        g_received.clear();
        wxl::seam::Texture::Create::Before("", 0u, nullptr, 0u);
        wxl::seam::Texture::Create::Before(nullptr, 0u, nullptr, 0u);
        Check(g_received.empty(), "an empty or missing name raises nothing");
        Mute(WXL_EV_TextureLoad);
        wxl::seam::Texture::Create::Before("Textures\\a.blp", 0u, nullptr, 0u);
        Unmute(WXL_EV_TextureLoad);
        Check(g_received.empty(), "nor does any name while nobody listens");

        FakeModel boar;
        strcpy(boar.name, "Creature\\Boar\\Boar.m2");
        boar.bytes = std::string("MD20") + std::string(60, '\x01');
        g_received.clear();
        wxl::seam::M2Shared::LoadSucceededCallback::Before(&boar);
        const Received* model = Only(WXL_EV_ModelLoad);
        Check(model != nullptr, "M2Shared.LoadSucceededCallback raises ModelLoad once");
        if (model)
        {
            const wxl_model_load_args args = model->As<wxl_model_load_args>();
            Check(args.path.size == strlen(boar.name) && args.size == 64u,
                  "with the model's path and the size of the bytes read");
            Check(args.replacement && args.replacement_size, "and the native answer to write");
        }
        Check(boar.replaced.empty(), "bytes nobody answers stay the client's");

        // A native handler that answers other bytes: the client parses those.
        static const char kOther[] = "MD20 other bytes";
        struct Answer
        {
            static void WXL_CALL Give(void*, const void* raw)
            {
                const wxl_model_load_args* args = static_cast<const wxl_model_load_args*>(raw);
                *args->replacement      = kOther;
                *args->replacement_size = static_cast<uint32_t>(sizeof kOther - 1);
            }
        };
        const wxl_subscription answer = g_host->Events().Subscribe(WXL_EV_ModelLoad, &Answer::Give, nullptr, 0);
        wxl::seam::M2Shared::LoadSucceededCallback::Before(&boar);
        Check(boar.replaced == std::string(kOther, sizeof kOther - 1),
              "bytes a handler answers are put in place of the ones read");
        FakeModel stuck;
        strcpy(stuck.name, "Creature\\Rat\\Rat.m2");
        stuck.bytes         = "MD20";
        stuck.replace_fails = true;
        wxl::seam::M2Shared::LoadSucceededCallback::Before(&stuck);
        Check(stuck.replaced.empty(), "a copy the client cannot make leaves the model as read");
        g_host->Events().Unsubscribe(answer);

        FakeMapObj keep;
        strcpy(keep.name, "World\\wmo\\Azeroth\\Buildings\\Stormwind\\Stormwind.wmo");
        keep.bytes = std::string("REVM") + std::string(28, '\0');
        g_received.clear();
        wxl::seam::MapObj::Load::Before(&keep);
        const Received* root = Only(WXL_EV_MapObjectLoad);
        Check(root && root->As<wxl_map_object_args>().size == 32u &&
                  root->As<wxl_map_object_args>().path.size == strlen(keep.name),
              "MapObj.Load raises MapObjectLoad with the root's path and bytes");
        Mute(WXL_EV_MapObjectLoad);
        g_received.clear();
        wxl::seam::MapObj::Load::Before(&keep);
        Unmute(WXL_EV_MapObjectLoad);
        Check(g_received.empty(), "and nothing while nobody listens");
    }

    /// Map: a doodad an overload placed is announced once; one it answered from the hash is not.

    // --- the engine's own loads: tiles, groups, skins, textures, shaders, sounds ----------------

    void CheckAssetEvents()
    {
        FakeTile tile;
        tile.x     = 32;
        tile.y     = 48;
        tile.file  = "|wxl|00000000|World\\Maps\\Azeroth\\Azeroth_32_48.adt";
        tile.bytes = "REVM tile";
        g_received.clear();
        wxl::seam::MapArea::AsyncCallback::Before(&tile);
        const Received* loaded = Only(WXL_EV_TerrainTileLoad);
        Check(loaded && loaded->As<wxl_terrain_tile_args>().tile_x == 32 && loaded->As<wxl_terrain_tile_args>().tile_y == 48 &&
                  loaded->As<wxl_terrain_tile_args>().size == 9u,
              "MapArea.AsyncCallback raises TerrainTileLoad with the tile's place and its bytes");
        Check(loaded && loaded->As<wxl_terrain_tile_args>().path.size == strlen("World\\Maps\\Azeroth\\Azeroth_32_48.adt"),
              "named by the client's name, not the file layer's path");

        FakeMapObj root;
        strcpy(root.name, "World\\wmo\\Inn.wmo");
        FakeGroup group;
        group.parent = &root;
        group.index  = 3;
        group.bytes  = "REVM group";
        g_received.clear();
        wxl::seam::MapObj::CreateGroup::Before(&group);
        const Received* grouped = Only(WXL_EV_MapObjectGroupLoad);
        Check(grouped && grouped->As<wxl_map_object_group_args>().group == 3u &&
                  grouped->As<wxl_map_object_group_args>().size == 10u &&
                  grouped->As<wxl_map_object_group_args>().path.size == strlen(root.name),
              "MapObj.CreateGroup raises MapObjectGroupLoad with the root's name, the group's number and bytes");

        FakeModel model;
        strcpy(model.name, "Creature\\Boar\\Boar.m2");
        model.skin = "SKIN data";
        g_received.clear();
        wxl::seam::M2Shared::FinishLoadingSkinProfile::Before(&model, 9);
        const Received* skin = Only(WXL_EV_SkinProfileLoad);
        Check(skin && skin->As<wxl_skin_profile_args>().size == 9u &&
                  skin->As<wxl_skin_profile_args>().model.size == strlen(model.name),
              "M2Shared.FinishLoadingSkinProfile raises SkinProfileLoad with the model and the skin's bytes");

        // A BLP2 header: 256 x 128, DXT, 8-bit alpha, DXT5, three mip levels.
        std::vector<uint8_t> blp(0x94 + 16, 0);
        const uint32_t       header[] = {0x32504C42u, 1u};
        std::memcpy(blp.data(), header, sizeof header);
        blp[8]  = 2;
        blp[9]  = 8;
        blp[10] = 7;
        blp[11] = 1;
        const uint32_t size[] = {256u, 128u};
        std::memcpy(blp.data() + 12, size, sizeof size);
        for (int i = 0; i < 3; ++i)
        {
            const uint32_t mip = 100u >> i;
            std::memcpy(blp.data() + 0x54 + 4 * i, &mip, 4);
        }
        FakeTexture texture;
        strcpy(texture.name, "World\\Grass.blp");
        g_received.clear();
        wxl::seam::Texture::AsyncTextureSuccessCallback::Before(&texture);
        wxl::seam::BLPFile::Source::Before(nullptr, blp.data());
        const Received* decoded = Only(WXL_EV_TextureDecode);
        Check(decoded && decoded->As<wxl_texture_decode_args>().width == 256u &&
                  decoded->As<wxl_texture_decode_args>().height == 128u &&
                  decoded->As<wxl_texture_decode_args>().encoding == WXL_TEXTURE_DXT &&
                  decoded->As<wxl_texture_decode_args>().alpha_bits == 8u &&
                  decoded->As<wxl_texture_decode_args>().alpha_kind == 7u &&
                  decoded->As<wxl_texture_decode_args>().mip_levels == 3u &&
                  decoded->As<wxl_texture_decode_args>().path.size == strlen(texture.name),
              "BLPFile.Source raises TextureDecode with the header's facts, named by the texture the read was for");
        g_received.clear();
        wxl::seam::BLPFile::Open::Before(nullptr, "Interface\\Icons\\A.blp", 0);
        wxl::seam::BLPFile::Source::Before(nullptr, blp.data());
        decoded = Only(WXL_EV_TextureDecode);
        Check(decoded && decoded->As<wxl_texture_decode_args>().path.size == strlen("Interface\\Icons\\A.blp"),
              "or by CBLPFile::Open's name");
        g_received.clear();
        blp[0] = 'X';
        wxl::seam::BLPFile::Source::Before(nullptr, blp.data());
        Check(g_received.empty(), "bytes that are not a BLP2 file raise nothing");

        g_received.clear();
        wxl::seam::Gx::IShaderLoad::Before(nullptr, nullptr, 4u, "Shaders\\Pixel", "MapObjDiffuse", 2u);
        const Received* shader = Only(WXL_EV_ShaderLoad);
        Check(shader && shader->As<wxl_shader_load_args>().stage == WXL_SHADER_PIXEL &&
                  shader->As<wxl_shader_load_args>().permutations == 2u &&
                  shader->As<wxl_shader_load_args>().name.size == 13u,
              "Gx.IShaderLoad raises ShaderLoad with the stage, the folder, the name and the permutations");

        g_received.clear();
        wxl::seam::Sound::CreateWaveData::Before(nullptr, "Sound\\Music\\Zone.mp3", 0u, nullptr, nullptr, nullptr, 0u, 50, 0,
                                                 0xFFFFFF01u, 0u, 0, 0.f, 0.f, 0.f, nullptr);
        const Received* sound = Only(WXL_EV_SoundFileLoad);
        Check(sound && sound->As<wxl_sound_file_args>().streaming == WXL_TRUE && sound->As<wxl_sound_file_args>().priority == 50,
              "Sound.CreateWaveData raises SoundFileLoad: a bool pushed as a dword is read by its low byte");
        g_received.clear();
        wxl::seam::Sound::CreateWaveData::Before(nullptr, "", 0u, nullptr, nullptr, nullptr, 0u, 0, 0, 0u, 0u, 0, 0.f, 0.f,
                                                 0.f, nullptr);
        Check(g_received.empty(), "an empty sound name raises nothing");

        const wxl_event quiet[] = {WXL_EV_TerrainTileLoad, WXL_EV_MapObjectGroupLoad, WXL_EV_SkinProfileLoad,
                                   WXL_EV_TextureDecode, WXL_EV_ShaderLoad, WXL_EV_SoundFileLoad};
        for (wxl_event event : quiet) Mute(event);
        g_received.clear();
        blp[0] = 'B';
        wxl::seam::MapArea::AsyncCallback::Before(&tile);
        wxl::seam::MapObj::CreateGroup::Before(&group);
        wxl::seam::M2Shared::FinishLoadingSkinProfile::Before(&model, 9);
        wxl::seam::BLPFile::Source::Before(nullptr, blp.data());
        wxl::seam::Gx::IShaderLoad::Before(nullptr, nullptr, 0u, "Shaders\\Vertex", "X", 1u);
        wxl::seam::Sound::CreateWaveData::Before(nullptr, "a.wav", 0u, nullptr, nullptr, nullptr, 0u, 0, 0, 0u, 0u, 0, 0.f,
                                                 0.f, 0.f, nullptr);
        for (wxl_event event : quiet) Unmute(event);
        Check(g_received.empty(), "none of them raises anything while nobody listens");
    }

    void CheckDoodads()
    {
        FakeDoodad tree;
        tree.position = {100.0f, 200.0f, 30.0f};
        const wxl_vec3 center{0.0f, 0.0f, 0.0f};

        g_received.clear();
        wxl::seam::Map::AllocDoodadDef::After(&tree);
        wxl::seam::Map::CreateDoodadDef::After(&tree, "World\\Azeroth\\Elwynn\\Tree01.m2", nullptr, &center);
        const Received* spawn = Only(WXL_EV_DoodadSpawn);
        Check(spawn != nullptr, "Map.CreateDoodadDef raises DoodadSpawn for the def it allocated");
        if (spawn)
        {
            const wxl_doodad_args args = spawn->As<wxl_doodad_args>();
            Check(args.position.x == 100.0f && args.position.z == 30.0f &&
                      args.path.size == strlen("World\\Azeroth\\Elwynn\\Tree01.m2"),
                  "with the model's path and where the def was placed");
            Check(args.doodad == WXL_HANDLE_NONE, "a doodad is no object of wxl.object: no handle");
        }

        g_received.clear();
        wxl::seam::Map::CreateDoodadDef::After(&tree, "World\\Azeroth\\Elwynn\\Tree01.m2", nullptr, &center);
        Check(g_received.empty(), "the same def answered from the hash is not placed again");

        FakeDoodad lamp, chair;
        g_received.clear();
        wxl::seam::Map::AllocDoodadDef::After(&lamp);
        wxl::seam::Map::CreateDoodadDefAt::After(&lamp, "World\\Generic\\Lamp.m2", &center, 1.5f, 0);
        wxl::seam::Map::AllocDoodadDef::After(&chair);
        wxl::seam::Map::CreateDoodadDefInMapObj::After(&chair, 3u, nullptr, const_cast<char*>("Chair.m2"), 7u,
                                                       nullptr, 0);
        Check(Count(WXL_EV_DoodadSpawn) == 2, "the other two overloads announce theirs");

        Mute(WXL_EV_DoodadSpawn);
        wxl::seam::Map::AllocDoodadDef::After(&lamp);
        Unmute(WXL_EV_DoodadSpawn);
        g_received.clear();
        wxl::seam::Map::CreateDoodadDefAt::After(&lamp, "World\\Generic\\Lamp.m2", &center, 1.5f, 0);
        Check(g_received.empty(), "a def allocated while nobody listened is not announced later");
    }

    /// Storage.OpenEx: the result of every open, found or not, with the name the client opened.
    void CheckFileOpened()
    {
        FakeFile file;
        file.name  = "DBFilesClient\\Spell.dbc";
        file.bytes = std::string(1234, 'x');
        FakeHandle opened_file;
        opened_file.file = &file;
        void* handle     = &opened_file;

        g_received.clear();
        wxl::seam::Storage::OpenEx::After(1, nullptr, "DBFilesClient\\Spell.dbc", 0u, &handle);
        const Received* opened = Only(WXL_EV_FileOpened);
        Check(opened != nullptr, "Storage.OpenEx raises FileOpened once");
        if (opened)
        {
            const wxl_file_opened_args args = opened->As<wxl_file_opened_args>();
            Check(args.found == WXL_TRUE && args.size == 1234u && args.path.size == 23u,
                  "with the name, found, and the size the binding reads");
        }

        void* none = nullptr;
        g_received.clear();
        wxl::seam::Storage::OpenEx::After(0, nullptr, "Missing\\File.blp", 0u, &none);
        const Received* missing = Only(WXL_EV_FileOpened);
        Check(missing && missing->As<wxl_file_opened_args>().found == WXL_FALSE &&
                  missing->As<wxl_file_opened_args>().size == 0u,
              "an open that found nothing is raised too, not found and empty");

        Mute(WXL_EV_FileOpened);
        g_received.clear();
        wxl::seam::Storage::OpenEx::After(1, nullptr, "DBFilesClient\\Spell.dbc", 0u, &handle);
        Unmute(WXL_EV_FileOpened);
        Check(g_received.empty(), "nothing is raised while nobody listens");
    }

    /// ObjectMgr: ObjectUpdate once per tracked object per packet, after the whole packet.
    void CheckObjectUpdate()
    {
        FakeObject wolf, hawk, egg;
        wolf.guid   = 0xF130000000000201ull;
        wolf.typeId = WXL_OBJECT_UNIT;
        hawk.guid   = 0xF130000000000202ull;
        hawk.typeId = WXL_OBJECT_UNIT;
        egg.guid    = 0xF110000000000203ull;
        egg.typeId  = WXL_OBJECT_GAMEOBJECT;

        wxl::seam::ObjectMgr::InitObject::After(&wolf, nullptr, 1u);
        wxl::seam::ObjectMgr::InitObject::After(&hawk, nullptr, 1u);

        g_received.clear();
        wxl::seam::ObjectMgr::FillInPartialObjectData::After(1, &wolf, wolf.guid, nullptr, false, false);
        wxl::seam::ObjectMgr::FillInPartialObjectData::After(1, &hawk, hawk.guid, nullptr, false, false);
        wxl::seam::ObjectMgr::FillInPartialObjectData::After(1, &wolf, wolf.guid, nullptr, false, false);
        wxl::seam::ObjectMgr::FillInPartialObjectData::After(1, &egg, egg.guid, nullptr, true, false);
        wxl::seam::ObjectMgr::FillInPartialObjectData::After(0, &hawk, hawk.guid, nullptr, false, false);
        Check(Count(WXL_EV_ObjectUpdate) == 0, "nothing is raised while the packet is being applied");
        wxl::seam::ObjectMgr::Packet_SMSG_UPDATE_OBJECT::After(1, nullptr, 0xA9u, 0u, nullptr);
        Check(Count(WXL_EV_ObjectUpdate) == 2, "the packet raises ObjectUpdate once per tracked object it wrote");
        bool ordered = false;
        if (Count(WXL_EV_ObjectUpdate) == 2)
        {
            std::vector<uint64_t> guids;
            for (const Received& r : g_received)
                if (r.event == WXL_EV_ObjectUpdate) guids.push_back(r.As<wxl_object_args>().guid);
            ordered = guids[0] == wolf.guid && guids[1] == hawk.guid;
        }
        Check(ordered, "in the order the packet first named them, the untracked one left out");

        g_received.clear();
        wxl::seam::ObjectMgr::Packet_SMSG_UPDATE_OBJECT::After(1, nullptr, 0xA9u, 0u, nullptr);
        Check(g_received.empty(), "a packet that wrote nothing raises nothing");

        // An object let go before the packet ends is not announced.
        wxl::seam::ObjectMgr::FillInPartialObjectData::After(1, &hawk, hawk.guid, nullptr, false, false);
        wxl::seam::ObjectMgr::ObjDelete::Before(&hawk);
        g_received.clear();
        wxl::seam::ObjectMgr::Packet_SMSG_UPDATE_OBJECT::After(1, nullptr, 0xA9u, 0u, nullptr);
        Check(Count(WXL_EV_ObjectUpdate) == 0, "an object gone before the packet ends is not updated");

        Mute(WXL_EV_ObjectUpdate);
        wxl::seam::ObjectMgr::FillInPartialObjectData::After(1, &wolf, wolf.guid, nullptr, false, false);
        Unmute(WXL_EV_ObjectUpdate);
        g_received.clear();
        wxl::seam::ObjectMgr::Packet_SMSG_UPDATE_OBJECT::After(1, nullptr, 0xA9u, 0u, nullptr);
        Check(g_received.empty(), "writes made while nobody listened are not noted");

        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
    }

    /// wxl.terrain over the fake ground: reads, edits with their copies across edges, normals,
    /// owners and layers, the drawing's rebuilds inside and outside the world pass, a chunk read
    /// again from the disk, the fall of a player standing still, refusals, the map unloading.
    /// wxl.terrain's layers: a provider's answers kept per render chunk and layer, then drawn through
    /// each path's own state (the vertex program's rows, the texture stages' matrices) for that draw
    /// only; a chunk whose layers are all at 1, or a draw of anything else, untouched.
    void CheckTerrainLayers()
    {
        namespace layers = wxl::core::services::terrain_layers;
        const wxl_terrain_api& api = wxl::core::services::TerrainApi();
        static std::vector<wxl_terrain_layer> told;
        told.clear();
        wxl_terrain_layer_provider provider{};
        provider.struct_size = sizeof provider;
        provider.fn          = [](void*, const wxl_terrain_layer* layer, wxl_terrain_layer_look* look) {
            told.push_back(*layer);
            if (layer->texture == 7) look->texture_scale = 4.0f;  // layer 1 of the chunk
            if (layer->texture == 9) look->texture_scale = std::nanf("");
        };
        Check(api.set_layer_provider(&provider) == WXL_OK, "terrain layers: a mod sets its layer provider");
        wxl_terrain_layer_provider none{};
        none.struct_size = sizeof none;
        Check(api.set_layer_provider(&none) == WXL_E_INVALID, "terrain layers: a provider without its function is refused");

        // The client's constants: c13 + k offsets (animated layer 1), c18 + k scales, -0.24.
        for (float& f : g_terrainConsts) f = 0.0f;
        for (int k = 0; k < 4; ++k)
        {
            g_terrainConsts[(18 + k) * 4 + 0] = g_terrainConsts[(18 + k) * 4 + 1] = -0.24f;
            g_terrainConsts[(13 + k) * 4 + 0] = 0.5f;
            g_terrainConsts[(13 + k) * 4 + 1] = -0.25f;
        }
        FakeRenderChunk chunk;
        char            area = 0;
        const uint32_t  mcly[3][4] = {{3, 0x0, 0, 0}, {7, 0x40, 0, 0}, {5, 0x0, 0, 0}};
        for (int i = 0; i < 3; ++i)
        {
            chunk.layers         = static_cast<uint32_t>(i + 1);
            chunk.textures[i]    = mcly[i][0];
            chunk.flags[i]       = mcly[i][1];
            wxl::seam::MapRenderChunk::CreateLayer::After(&chunk, &area, mcly[i], 0);
        }
        Check(told.size() == 3 && told[1].texture == 7 && told[1].layer == 1 && told[1].flags == 0x40 && told[1].tile_x == 32 &&
                  told[1].file == g_areaFile.data() && told[1].file_size == 64,
              "terrain layers: the provider is told of each layer as it is made, with its ADT's bytes");
        Check(layers::Count().chunks_kept == 1 && layers::Count().layers_scaled == 1, "terrain layers: the chunk with a scaled layer is kept");

        auto sent = [&](uint32_t row) -> const std::vector<float>* {
            for (const auto& r : g_rowsSent)
                if (r.first == row) return &r.second;
            return nullptr;
        };
        // Single pass, vertex program: c19 and c14 (layer 1) divided by 4, for this draw only.
        g_rowsSent.clear();
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::Before(&chunk);
        wxl::seam::Gx::D3dDraw::Before(nullptr, &chunk.batch, 1);
        const auto* c19 = sent(19);
        const auto* c14 = sent(14);
        Check(g_rowsSent.size() == 2 && c19 && std::fabs((*c19)[0] + 0.06f) < 1e-6f && std::fabs((*c19)[1] + 0.06f) < 1e-6f && c14 &&
                  std::fabs((*c14)[0] - 0.125f) < 1e-6f && (*c14)[2] == 0.0f,
              "terrain layers: single pass: layer 1's scale (c19) and its animation offset (c14) divided by its texture scale, the others left");
        g_rowsSent.clear();
        wxl::seam::Gx::D3dDraw::After(nullptr, &chunk.batch, 1);
        Check(g_rowsSent.size() == 2 && sent(19) && (*sent(19))[0] == -0.24f && sent(14) && (*sent(14))[0] == 0.5f,
              "terrain layers: after the draw, the client's own rows go back");
        g_rowsSent.clear();
        char other = 0;
        wxl::seam::Gx::D3dDraw::Before(nullptr, &other, 1);
        wxl::seam::Gx::D3dDraw::After(nullptr, &other, 1);
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::After(&chunk);
        wxl::seam::Gx::D3d9ExDraw::Before(nullptr, &chunk.batch, 1);
        Check(g_rowsSent.empty(), "terrain layers: a draw of anything else, or after the chunk's function, untouched");

        // A layer a pass, vertex program: the second draw (layer 1) reads c18 and c13.
        wxl::seam::MapRenderChunk::RenderMultiPassVertexPixelShader::Before(&chunk);
        int scaledDraws = 0;
        for (int d = 0; d < 4; ++d)
        {
            g_rowsSent.clear();
            wxl::seam::Gx::D3d9ExDraw::Before(nullptr, &chunk.batch, 1);
            if (!g_rowsSent.empty())
            {
                ++scaledDraws;
                Check(d == 1 && sent(18) && std::fabs((*sent(18))[0] + 0.06f) < 1e-6f && sent(13) && std::fabs((*sent(13))[0] - 0.125f) < 1e-6f,
                      "terrain layers: a layer a pass: layer 1's draw reads c18 and c13 divided by its scale");
            }
            wxl::seam::Gx::D3d9ExDraw::After(nullptr, &chunk.batch, 1);
        }
        wxl::seam::MapRenderChunk::RenderMultiPassVertexPixelShaderBigAlpha::After(&chunk);
        Check(scaledDraws == 1, "terrain layers: only the scaled layer's draw is changed, the shadow's pass past the layers not");

        // Fixed function, single pass: stage 1's matrix, its u and v columns (translation included).
        for (int x = 0; x < 11; ++x)
            for (int i = 0; i < 16; ++i) g_stage[x][i] = static_cast<float>(i + 1);
        g_stagesSet.clear();
        wxl::seam::MapRenderChunk::RenderSinglePassPixelShader::Before(&chunk);
        wxl::seam::Gx::D3dDraw::Before(nullptr, &chunk.batch, 1);
        Check(g_stagesSet.size() == 1 && g_stagesSet[0].first == 1 && g_stagesSet[0].second[0] == 0.25f && g_stagesSet[0].second[1] == 0.5f &&
                  g_stagesSet[0].second[2] == 3.0f && g_stagesSet[0].second[12] == 13.0f / 4.0f && g_stagesSet[0].second[13] == 14.0f / 4.0f,
              "terrain layers: single pass, fixed function: stage 1's matrix makes u and v four times smaller, its move too");
        wxl::seam::Gx::D3dDraw::After(nullptr, &chunk.batch, 1);
        Check(g_stagesSet.size() == 2 && g_stage[1][0] == 1.0f && g_stage[1][13] == 14.0f, "terrain layers: and gets its matrix back after the draw");
        wxl::seam::MapRenderChunk::RenderSinglePassPixelShader::After(&chunk);

        // Fixed function, a layer a pass: stage 0's matrix on layer 1's draw.
        for (auto path : {0, 1, 2, 3})
        {
            g_stagesSet.clear();
            switch (path)
            {
            case 0: wxl::seam::MapRenderChunk::RenderMultiPassPixelShader::Before(&chunk); break;
            case 1: wxl::seam::MapRenderChunk::RenderMultiPassPixelShaderBigAlpha::Before(&chunk); break;
            case 2: wxl::seam::MapRenderChunk::RenderMultiPassAdditive::Before(&chunk); break;
            default: wxl::seam::MapRenderChunk::RenderMultiPassAlpha::Before(&chunk); break;
            }
            for (int d = 0; d < 3; ++d)
            {
                wxl::seam::Gx::D3dDraw::Before(nullptr, &chunk.batch, 1);
                wxl::seam::Gx::D3dDraw::After(nullptr, &chunk.batch, 1);
            }
            wxl::seam::MapRenderChunk::RenderMultiPassAlpha::After(&chunk);
            Check(g_stagesSet.size() == 2 && g_stagesSet[0].first == 0 && g_stagesSet[0].second[0] == 0.25f && g_stage[0][0] == 1.0f,
                  "terrain layers: a layer a pass, fixed function: stage 0 on layer 1's draw, given back");
        }

        // The Vulkan devices' draw_begin and draw_end: the same.
        const wxl_gx_vk_backend* backend = WarcraftXLGxVulkan();
        g_rowsSent.clear();
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::Before(&chunk);
        backend->draw_begin(nullptr, reinterpret_cast<const wxl_gx_batch*>(&chunk.batch), 1);
        const size_t begun = g_rowsSent.size();
        backend->draw_end(nullptr, reinterpret_cast<const wxl_gx_batch*>(&chunk.batch), 1);
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::After(&chunk);
        Check(backend->struct_size >= sizeof(wxl_gx_vk_backend) && begun == 2 && g_rowsSent.size() == 4,
              "terrain layers: the Vulkan device's draw_begin and draw_end scale and give back the same rows");

        // Scale 0 everywhere: a chunk whose layers are all at 1 is not kept, and nothing is sent.
        FakeRenderChunk plain;
        const uint32_t  stock[2][4] = {{3, 0, 0, 0}, {5, 0x40, 0, 0}};
        for (int i = 0; i < 2; ++i)
        {
            plain.layers      = static_cast<uint32_t>(i + 1);
            plain.textures[i] = stock[i][0];
            wxl::seam::MapRenderChunk::CreateLayer::After(&plain, &area, stock[i], 0);
        }
        g_rowsSent.clear();
        g_stagesSet.clear();
        for (int path = 0; path < 2; ++path)
        {
            if (path == 0) wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::Before(&plain);
            else wxl::seam::MapRenderChunk::RenderSinglePassPixelShader::Before(&plain);
            wxl::seam::Gx::D3dDraw::Before(nullptr, &plain.batch, 1);
            wxl::seam::Gx::D3dDraw::After(nullptr, &plain.batch, 1);
            wxl::seam::MapRenderChunk::RenderSinglePassPixelShader::After(&plain);
        }
        Check(g_rowsSent.empty() && g_stagesSet.empty() && layers::Count().chunks_kept == 1,
              "terrain layers: a chunk of stock layers (scale 1) is drawn exactly as the client draws it");

        // An answer out of range is the client's own; the chunk made again forgets its old layers.
        chunk.layers      = 1;
        chunk.textures[0] = 9;
        const uint32_t bad[4] = {9, 0, 0, 0};
        wxl::seam::MapRenderChunk::CreateLayer::After(&chunk, &area, bad, 0);
        Check(layers::Count().chunks_kept == 0, "terrain layers: a NaN answer is 1, and a chunk made again keeps nothing of before");

        // The mod goes, its provider with it: layers are no longer asked.
        const size_t asked = told.size();
        wxl::core::services::terrain_layers::ForgetMod(0);
        chunk.layers = 1;
        wxl::seam::MapRenderChunk::CreateLayer::After(&chunk, &area, mcly[1], 0);
        Check(told.size() == asked, "terrain layers: a mod's provider goes with it");
        wxl::core::services::terrain_layers::Clear();
    }

    // abi 0.1.43: a layer's extra texture and numbers on the single-pass paths.
    void CheckTerrainLayerExtras()
    {
        namespace layers = wxl::core::services::terrain_layers;
        const wxl_terrain_api& api = wxl::core::services::TerrainApi();
        wxl_terrain_layer_provider provider{};
        provider.struct_size = sizeof provider;
        provider.flags       = WXL_TERRAIN_LAYER_PARAMS_EVERY_DRAW;
        provider.fn          = [](void*, const wxl_terrain_layer* layer, wxl_terrain_layer_look* look) {
            if (look->struct_size < sizeof *look) return;
            if (layer->texture == 7) look->extra_texture = "Tileset\\Rock_h.blp";
            if (layer->texture == 5) look->extra_texture = "Tileset\\Loading_h.blp";
            if (layer->texture == 7 || layer->texture == 5)
            {
                look->params[0] = 6.0f;
                look->params[1] = 1.0f;
                look->params[2] = 1.0f;
            }
        };
        Check(api.set_layer_provider(&provider) == WXL_OK, "terrain extras: a provider with its flags is set");
        const int closedBefore = g_texturesClosed;
        FakeRenderChunk chunk;
        char            area = 0;
        const uint32_t  mcly[3][4] = {{3, 0, 0, 0}, {7, 0, 0, 0}, {5, 0, 0, 0}};
        for (int i = 0; i < 3; ++i)
        {
            chunk.layers      = static_cast<uint32_t>(i + 1);
            chunk.textures[i] = mcly[i][0];
            wxl::seam::MapRenderChunk::CreateLayer::After(&chunk, &area, mcly[i], 0);
        }
        Check(layers::Count().chunks_kept == 1 && layers::Count().layers_extra == 2, "terrain extras: the chunk with extra textures is kept");

        static char before = 0;
        g_stageTexture[13] = &before;
        g_pixelRowsSent.clear();
        g_texturesWrapped = 0;
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::Before(&chunk);
        wxl::seam::Gx::D3dDraw::Before(nullptr, &chunk.batch, 1);
        const bool bound = g_stageTexture[13] == &g_fakeGxTex && !g_stageTexture[14] && g_texturesWrapped == 1;
        const bool rows  = g_pixelRowsSent.size() == 1 && g_pixelRowsSent[0].first == WXL_TERRAIN_LAYER_PARAM_ROW &&
                          g_pixelRowsSent[0].second.size() == 17 * 4 && g_pixelRowsSent[0].second[4] == 6.0f && g_pixelRowsSent[0].second[6] == 1.0f &&
                          g_pixelRowsSent[0].second[0] == 0.0f && g_pixelRowsSent[0].second[8] == 0.0f && g_pixelRowsSent[0].second[10] == 0.0f;
        wxl::seam::Gx::D3dDraw::After(nullptr, &chunk.batch, 1);
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::After(&chunk);
        Check(bound, "terrain extras: layer 1's texture on sampler 13, wrapped; layer 2's still loading, not bound");
        Check(rows, "terrain extras: the numbers in c32 + k, layer 1's (6, 1, 1), zeros for layer 2 whose texture is not loaded yet");
        bool zeros = g_pixelRowsSent.size() == 2;
        for (float f : g_pixelRowsSent.back().second) zeros = zeros && f == 0.0f;
        Check(g_stageTexture[13] == &before && zeros, "terrain extras: after the draw, the sampler's texture back and the numbers zero");

        // The Vulkan device: the same, through draw_begin and draw_end.
        const wxl_gx_vk_backend* backend = WarcraftXLGxVulkan();
        g_pixelRowsSent.clear();
        wxl::seam::MapRenderChunk::RenderSinglePassPixelShader::Before(&chunk);
        backend->draw_begin(nullptr, reinterpret_cast<const wxl_gx_batch*>(&chunk.batch), 1);
        const bool vk = g_stageTexture[13] == &g_fakeGxTex && g_pixelRowsSent.size() == 1;
        backend->draw_end(nullptr, reinterpret_cast<const wxl_gx_batch*>(&chunk.batch), 1);
        wxl::seam::MapRenderChunk::RenderSinglePassPixelShader::After(&chunk);
        Check(vk && g_stageTexture[13] == &before, "terrain extras: the Vulkan device's draw_begin binds, draw_end gives back");

        // A chunk of the client's own looks: its numbers written as zeros on the single-pass paths only.
        FakeRenderChunk plain;
        const uint32_t  stock[2][4] = {{3, 0, 0, 0}, {4, 0, 0, 0}};
        for (int i = 0; i < 2; ++i)
        {
            plain.layers      = static_cast<uint32_t>(i + 1);
            plain.textures[i] = stock[i][0];
            wxl::seam::MapRenderChunk::CreateLayer::After(&plain, &area, stock[i], 0);
        }
        g_pixelRowsSent.clear();
        g_rowsSent.clear();
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::Before(&plain);
        wxl::seam::Gx::D3dDraw::Before(nullptr, &plain.batch, 1);
        wxl::seam::Gx::D3dDraw::After(nullptr, &plain.batch, 1);
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::After(&plain);
        bool plainZeros = g_pixelRowsSent.size() == 2;
        for (const auto& r : g_pixelRowsSent)
            for (size_t i = 0; i < 9 * 4 && i < r.second.size(); ++i) plainZeros = plainZeros && r.second[i] == 0.0f; // numbers and pass row
        Check(plainZeros && g_rowsSent.empty() && g_stageTexture[12] == nullptr,
              "terrain extras: a stock chunk gets zeros (WXL_TERRAIN_LAYER_PARAMS_EVERY_DRAW), nothing else");
        g_pixelRowsSent.clear();
        wxl::seam::MapRenderChunk::RenderMultiPassVertexPixelShader::Before(&plain);
        wxl::seam::Gx::D3dDraw::Before(nullptr, &plain.batch, 1);
        wxl::seam::Gx::D3dDraw::After(nullptr, &plain.batch, 1);
        wxl::seam::MapRenderChunk::RenderMultiPassVertexPixelShader::After(&plain);
        Check(g_pixelRowsSent.empty(), "terrain extras: a layer-a-pass path is left as the client draws it");

        // Without the flag (a provider of abi 0.1.42's size), a stock chunk is not touched at all.
        wxl_terrain_layer_provider older{};
        older.struct_size = offsetof(wxl_terrain_layer_provider, flags);
        older.fn          = provider.fn;
        Check(api.set_layer_provider(&older) == WXL_OK, "terrain extras: a provider of abi 0.1.42 is still taken");
        g_pixelRowsSent.clear();
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::Before(&plain);
        wxl::seam::Gx::D3dDraw::Before(nullptr, &plain.batch, 1);
        wxl::seam::Gx::D3dDraw::After(nullptr, &plain.batch, 1);
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::After(&plain);
        Check(g_pixelRowsSent.empty(), "terrain extras: and without the flag a stock chunk is drawn as the client draws it");

        layers::MapUnloading();
        Check(g_texturesClosed - closedBefore == 2 && layers::Count().chunks_kept == 0, "terrain extras: the map goes, its extra textures closed");
        g_stageTexture[13] = nullptr;
        layers::Clear();
    }

    // abi 0.1.44: a chunk's layers past its four, drawn a second time over the first.
    void CheckTerrainExtraLayers()
    {
        namespace layers = wxl::core::services::terrain_layers;
        const wxl_terrain_api& api = wxl::core::services::TerrainApi();
        static int  asked = 0;
        static bool differ = false, bare = false;
        wxl_terrain_layer_provider provider{};
        provider.struct_size = sizeof provider;
        provider.flags       = WXL_TERRAIN_LAYER_PARAMS_EVERY_DRAW;
        provider.fn          = [](void*, const wxl_terrain_layer*, wxl_terrain_layer_look*) {};
        provider.chunk_fn    = [](void*, const wxl_terrain_chunk* chunk, wxl_terrain_extra_layers* out) {
            ++asked;
            if (chunk->layers != 4 || (bare && asked % 2 == 0)) return;
            out->count                   = 2;
            out->layer[0].texture        = differ && asked % 2 == 0 ? "Tileset\\Rock.blp" : "Tileset\\Moss.blp";
            out->layer[0].texture_scale  = 2.0f;
            out->layer[0].extra_texture  = "Tileset\\Moss_h.blp";
            out->layer[0].params[0]      = 5.0f;
            out->layer[0].params[1]      = 1.0f;
            out->layer[0].params[2]      = 1.0f;
            out->layer[1].texture        = "Tileset\\Sand.blp";
            out->layer[1].texture_scale  = 1.0f;
            out->alpha[0]                = 200;
            out->alpha[1]                = 100;
        };
        Check(api.set_layer_provider(&provider) == WXL_OK, "extra layers: a provider with a chunk function is set");
        static char     mapChunk = 0, neighbour = 0, area = 0;
        FakeRenderChunk chunk;
        chunk.map_chunk = &mapChunk;
        const auto make = [&](FakeRenderChunk& c) {
            for (uint32_t i = 0; i < 4; ++i)
            {
                const uint32_t mcly[4] = {20 + i, 0, 0, 0};
                c.layers      = i + 1;
                c.textures[i] = 20 + i;
                wxl::seam::MapRenderChunk::CreateLayer::After(&c, &area, mcly, 0);
            }
        };
        const size_t opened = g_openedTextures.size();
        make(chunk);
        bool served = false;
        for (size_t i = opened; i < g_openedTextures.size(); ++i)
            served = served || g_openedTextures[i].path.rfind("Interface\\WXL\\Data\\terrain_64x64_", 0) == 0;
        Check(asked == 1 && layers::Count().chunks_extra == 1 && served,
              "extra layers: asked once the chunk's fourth layer is made; their alpha maps served as a 64 x 64 texture and opened");

        for (int& v : g_renderState) v = 0;
        g_renderState[15] = 1; // depth written
        static char stock0 = 0;
        for (void*& t : g_stageTexture) t = nullptr;
        g_stageTexture[0] = &stock0;
        for (float& f : g_terrainConsts) f = 0.0f;
        for (int k = 0; k < 5; ++k) g_terrainConsts[(18 + k) * 4 + 0] = g_terrainConsts[(18 + k) * 4 + 1] = -0.24f;
        g_pixelRowsSent.clear();
        g_rowsSent.clear();
        g_draws.clear();
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::Before(&chunk);
        wxl::seam::Gx::D3dDraw::Before(nullptr, &chunk.batch, 1);
        const std::vector<float> first = g_pixelRowsSent.empty() ? std::vector<float>() : g_pixelRowsSent.back().second;
        const bool firstBound = g_stageTexture[9] == &g_fakeGxTex && g_stageTexture[10] == &g_fakeGxTex && !g_stageTexture[11];
        wxl::seam::Gx::D3dDraw::After(nullptr, &chunk.batch, 1);
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::After(&chunk);
        Check(first.size() == 17 * 4 && first[4 * 4] == 5.0f && first[4 * 4 + 2] == 1.0f && first[8 * 4] == 0.0f && first[8 * 4 + 1] == 1.0f &&
                  firstBound,
              "extra layers: the first draw has layer 4's numbers in c36, the pass row (0, 1), their alpha maps on 9, layer 4's height on 10");
        // u = alpha u * x + z: layer 4 at scale 2 is half the alpha maps' ratio of layer 0's.
        Check(first.size() == 17 * 4 && first[(9 + 4) * 4] * 2.0f == first[9 * 4] && first[9 * 4] != 0.0f,
              "extra layers: each layer's coordinates from the alpha maps' (c41 + k), at its own scale");
        const FakeDraw* second = g_draws.size() == 1 ? &g_draws[0] : nullptr;
        bool scaled = false, still = false;
        if (second)
            for (const auto& r : second->vertexRows)
            {
                if (r.first == 18 && r.second.size() == 16 && r.second[0] == -0.12f && r.second[4] == -0.24f) scaled = true;
                if (r.first == 13 && r.second.size() == 16 && r.second[0] == 0.0f) still = true;
            }
        Check(second && second->batch == &chunk.batch && second->blending == 2 && second->depthWrite == 0 && second->stage[0] == &g_fakeGxTex &&
                  second->stage[1] == &g_fakeGxTex && second->pixelRows.size() == 4 && second->pixelRows[0] == 1.0f,
              "extra layers: one second draw, alpha-blended without depth writes, layers 4 and 5 on stages 0 and 1, pass row (1, 1)");
        Check(scaled && still, "extra layers: its coordinates 0 and 1 at layers 4 and 5's scales (c18, c19), drawn still (c13 zero)");
        bool zeros = !g_pixelRowsSent.empty() && g_pixelRowsSent.back().second.size() == 17 * 4;
        for (float f : g_pixelRowsSent.back().second) zeros = zeros && f == 0.0f;
        Check(g_renderState[6] == 0 && g_renderState[15] == 1 && g_stageTexture[0] == &stock0 && !g_stageTexture[9] && zeros,
              "extra layers: after the draws, the states, the stages and the rows are the client's again");

        // The Vulkan devices: draw_end draws the second the same way.
        const wxl_gx_vk_backend* backend = WarcraftXLGxVulkan();
        g_draws.clear();
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::Before(&chunk);
        backend->draw_begin(nullptr, reinterpret_cast<const wxl_gx_batch*>(&chunk.batch), 1);
        backend->draw_end(nullptr, reinterpret_cast<const wxl_gx_batch*>(&chunk.batch), 1);
        wxl::seam::MapRenderChunk::RenderSinglePassVertexPixelShader::After(&chunk);
        Check(g_draws.size() == 1 && g_draws[0].blending == 2 && g_renderState[6] == 0, "extra layers: the Vulkan device's draw_end draws the second, once");

        // The fixed-function single pass draws the four: no second draw, the pass row (0, 0).
        g_draws.clear();
        g_pixelRowsSent.clear();
        wxl::seam::MapRenderChunk::RenderSinglePassPixelShader::Before(&chunk);
        wxl::seam::Gx::D3dDraw::Before(nullptr, &chunk.batch, 1);
        const std::vector<float> fixed = g_pixelRowsSent.empty() ? std::vector<float>() : g_pixelRowsSent.back().second;
        wxl::seam::Gx::D3dDraw::After(nullptr, &chunk.batch, 1);
        wxl::seam::MapRenderChunk::RenderSinglePassPixelShader::After(&chunk);
        Check(g_draws.empty() && fixed.size() == 17 * 4 && fixed[8 * 4 + 1] == 0.0f, "extra layers: the fixed-function single pass draws the client's four");

        // Drawn with its neighbour: the halves' layers put together by texture, their maps side by side.
        FakeRenderChunk paired;
        paired.map_chunk  = &mapChunk;
        paired.map_chunk2 = &neighbour;
        paired.pair       = 2;
        const size_t before = g_openedTextures.size();
        make(paired);
        bool wide = false;
        for (size_t i = before; i < g_openedTextures.size(); ++i)
            wide = wide || g_openedTextures[i].path.rfind("Interface\\WXL\\Data\\terrain_128x64_", 0) == 0;
        differ = true;
        FakeRenderChunk other = paired;
        make(other);
        Check(wide && layers::Count().chunks_extra == 3 && layers::Count().chunks_trimmed == 0,
              "extra layers: a chunk drawn beside its neighbour gets 128 x 64 maps, its layers too when its neighbour answers others (3 together)");
        differ = false;
        bare   = true;
        FakeRenderChunk half = paired;
        half.pair            = 1;
        const size_t tall    = g_openedTextures.size();
        make(half);
        bool high = false;
        for (size_t i = tall; i < g_openedTextures.size(); ++i)
            high = high || g_openedTextures[i].path.rfind("Interface\\WXL\\Data\\terrain_64x128_", 0) == 0;
        Check(high && layers::Count().chunks_extra == 4 && layers::Count().chunks_half == 1,
              "extra layers: a chunk below its neighbour, only one half with layers past four: 64 x 128 maps, the other half's zero");
        bare = false;

        const int closed = g_texturesClosed;
        layers::MapUnloading();
        Check(g_texturesClosed - closed >= 5 && layers::Count().chunks_kept == 0, "extra layers: the map goes, their textures and maps closed");

        // The map's WDT: height texturing without big alpha read as retail reads it.
        g_wdtFlags = 0x80;
        g_shadersValidated = 0;
        Check(layers::WdtLoaded() == 0x84 && g_wdtFlags == 0x84 && g_shadersValidated == 1 && layers::Wdt().empty(),
              "the map's WDT: 0x80 without 0x4 is given 0x4 and the programs checked again; a WDT not found is no bytes");
        g_wdtFlags = 0x4;
        Check(layers::WdtLoaded() == 0x4 && g_shadersValidated == 1, "the map's WDT: other flags are left as the client read them");
        layers::Clear();
    }

    // A tile whose chunks name more layers than the client holds: cut to 4 before its chunks are walked.
    // --- Tiles split by file name, loaded natively (abi 0.1.45) ----------------------------------

    /// Bytes of an IFF file: chunks appended one after another.
    struct SplitIff
    {
        std::string bytes;
        void Raw(const void* p, size_t n) { bytes.append(static_cast<const char*>(p), n); }
        void U32(uint32_t v) { Raw(&v, 4); }
        /// A chunk: its id as the file stores it (reversed), its size, its bytes; answers its offset.
        size_t Chunk(const char (&id)[5], const std::string& data)
        {
            const size_t at = bytes.size();
            const char   rev[4] = {id[3], id[2], id[1], id[0]};
            Raw(rev, 4);
            U32(static_cast<uint32_t>(data.size()));
            bytes += data;
            return at;
        }
    };

    std::string SplitWords(std::initializer_list<uint32_t> words)
    {
        std::string out;
        for (uint32_t w : words) out.append(reinterpret_cast<const char*>(&w), 4);
        return out;
    }

    /// The test tile: what one chunk holds, whichever way it is written.
    struct SplitChunkData
    {
        std::string header; ///< 128 bytes, monolithic counts
        std::string mcvt, mcnr, mcly, mcal, mcsh;
        std::vector<uint32_t> doodads, mapObjs;
        uint16_t holes = 0;
    };

    struct SplitTwin
    {
        SplitIff mono, root, tex0, obj0;
    };

    /// One tile, written once in one file (3.3.5a) and once split by name (retail); @p byId: the
    /// split one names its textures and models by FileDataID (8.1 and later): MDID 1001 to 1005,
    /// models 2001 and 2002, the map object 3001.
    SplitTwin MakeSplitTwin(bool byId = false)
    {
        const std::string mtex("t\\a.blp\0t\\b.blp\0t\\c.blp\0t\\d.blp\0t\\e.blp\0", 40);
        const std::string mtxf = SplitWords({0, 1, 0x10, 0, 0x20});
        const std::string mmdx("m\\a.m2\0m\\bb.m2\0", 15), mmid = SplitWords({0, 7});
        const std::string mwmo("w\\a.wmo\0", 8), mwid = SplitWords({0});
        std::string mddf, modf;
        for (uint32_t i = 0; i < 3; ++i)
        {
            std::string def(36, '\0');
            const uint32_t name = i % 2, id = 100 + i;
            std::memcpy(&def[0], &name, 4);
            std::memcpy(&def[4], &id, 4);
            def[8] = static_cast<char>(i);
            mddf += def;
        }
        for (uint32_t i = 0; i < 2; ++i)
        {
            std::string def(64, '\0');
            const uint32_t id = 200 + i;
            std::memcpy(&def[4], &id, 4);
            def[12] = static_cast<char>(i + 1);
            modf += def;
        }
        const std::string mfbo(36, '\x05');

        SplitChunkData chunks[256];
        for (uint32_t i = 0; i < 256; ++i)
        {
            SplitChunkData& c = chunks[i];
            const uint32_t  layers = i == 7 ? 6u : i % 5;
            for (uint32_t k = 0; k < layers; ++k)
            {
                const uint32_t entry[4] = {(i + k) % 5, k ? 0x100u : 0u, k ? (k - 1) * 2048u : 0u, 0u};
                c.mcly.append(reinterpret_cast<const char*>(entry), 16);
            }
            for (uint32_t k = 1; k < layers; ++k)
                for (uint32_t t = 0; t < 2048; ++t) c.mcal.push_back(static_cast<char>(i * 7 + k * 31 + t));
            if (i % 3 == 0)
                for (uint32_t t = 0; t < 512; ++t) c.mcsh.push_back(static_cast<char>(i + t * 3));
            for (uint32_t v = 0; v < 145; ++v)
            {
                const float h = static_cast<float>(i) + 0.5f * static_cast<float>(v);
                c.mcvt.append(reinterpret_cast<const char*>(&h), 4);
            }
            for (uint32_t n = 0; n < 448; ++n) c.mcnr.push_back(static_cast<char>(i ^ n));
            for (uint32_t d = 0; d < i % 4; ++d) c.doodads.push_back((i + d) % 3);
            if (i % 2) c.mapObjs.push_back(i % 4 == 1 ? 0u : 1u);
            c.holes = i % 5 == 0 ? static_cast<uint16_t>(i * 37u) : 0;
            c.header.assign(128, '\0');
            const uint32_t flags = (c.mcsh.empty() ? 0u : 1u), ix = i % 16, iy = i / 16, area = 1000 + i;
            std::memcpy(&c.header[0x00], &flags, 4);
            std::memcpy(&c.header[0x04], &ix, 4);
            std::memcpy(&c.header[0x08], &iy, 4);
            std::memcpy(&c.header[0x34], &area, 4);
            const float z = 10.0f * static_cast<float>(i);
            std::memcpy(&c.header[0x70], &z, 4);
        }

        SplitTwin twin;
        // Monolithic: MVER, MHDR, MCIN, MTEX, MMDX, MMID, MWMO, MWID, MDDF, MODF, MFBO, MTXF, 256 MCNK.
        SplitIff& m = twin.mono;
        m.Chunk("MVER", SplitWords({18}));
        const size_t mhdr = m.Chunk("MHDR", std::string(64, '\0'));
        const size_t base = mhdr + 8;
        const size_t mcin = m.Chunk("MCIN", std::string(4096, '\0'));
        uint32_t     slots[12] = {1, static_cast<uint32_t>(mcin - base)};
        slots[2]  = static_cast<uint32_t>(m.Chunk("MTEX", mtex) - base);
        slots[3]  = static_cast<uint32_t>(m.Chunk("MMDX", mmdx) - base);
        slots[4]  = static_cast<uint32_t>(m.Chunk("MMID", mmid) - base);
        slots[5]  = static_cast<uint32_t>(m.Chunk("MWMO", mwmo) - base);
        slots[6]  = static_cast<uint32_t>(m.Chunk("MWID", mwid) - base);
        slots[7]  = static_cast<uint32_t>(m.Chunk("MDDF", mddf) - base);
        slots[8]  = static_cast<uint32_t>(m.Chunk("MODF", modf) - base);
        slots[9]  = static_cast<uint32_t>(m.Chunk("MFBO", mfbo) - base);
        slots[11] = static_cast<uint32_t>(m.Chunk("MTXF", mtxf) - base);
        std::memcpy(&m.bytes[base], slots, sizeof slots);
        for (uint32_t i = 0; i < 256; ++i)
        {
            const SplitChunkData& c = chunks[i];
            std::string header = c.header;
            const uint32_t layers = static_cast<uint32_t>(std::min<size_t>(c.mcly.size() / 16, 4));
            const uint32_t doodads = static_cast<uint32_t>(c.doodads.size()), mapObjs = static_cast<uint32_t>(c.mapObjs.size());
            const uint32_t sizeAlpha = c.mcal.empty() ? 0u : static_cast<uint32_t>(c.mcal.size() + 8);
            std::memcpy(&header[0x0C], &layers, 4);
            std::memcpy(&header[0x10], &doodads, 4);
            std::memcpy(&header[0x28], &sizeAlpha, 4);
            std::memcpy(&header[0x38], &mapObjs, 4);
            std::memcpy(&header[0x3C], &c.holes, 2);
            SplitIff body;
            body.bytes = header;
            body.Chunk("MCVT", c.mcvt);
            body.Chunk("MCNR", c.mcnr);
            body.Chunk("MCLY", c.mcly);
            std::string mcrf;
            for (uint32_t d : c.doodads) mcrf += SplitWords({d});
            for (uint32_t w : c.mapObjs) mcrf += SplitWords({w});
            body.Chunk("MCRF", mcrf);
            if (!c.mcsh.empty()) body.Chunk("MCSH", c.mcsh);
            if (!c.mcal.empty()) body.Chunk("MCAL", c.mcal);
            const size_t at = m.Chunk("MCNK", body.bytes);
            const uint32_t info[2] = {static_cast<uint32_t>(at), static_cast<uint32_t>(body.bytes.size() + 8)};
            std::memcpy(&m.bytes[mcin + 8 + 16 * i], info, 8);
        }

        // Split: the root keeps heights, normals and MFBO; the holes in 64 bits.
        SplitIff& r = twin.root;
        r.Chunk("MVER", SplitWords({18}));
        const size_t rh = r.Chunk("MHDR", std::string(64, '\0'));
        uint32_t     rslots[12] = {1};
        rslots[9] = static_cast<uint32_t>(r.Chunk("MFBO", mfbo) - (rh + 8));
        std::memcpy(&r.bytes[rh + 8], rslots, sizeof rslots);
        SplitIff& t = twin.tex0;
        t.Chunk("MVER", SplitWords({18}));
        if (byId) t.Chunk("MDID", SplitWords({1001, 1002, 1003, 1004, 1005}));
        else t.Chunk("MTEX", mtex);
        t.Chunk("MTXF", mtxf);
        SplitIff& o = twin.obj0;
        o.Chunk("MVER", SplitWords({18}));
        if (byId)
        {
            std::string ddf = mddf, odf = modf;
            for (uint32_t i = 0; i < 3; ++i)
            {
                const uint32_t id = 2001 + i % 2;
                const uint16_t flag = 0x40;
                std::memcpy(&ddf[36 * i], &id, 4);
                std::memcpy(&ddf[36 * i + 34], &flag, 2);
            }
            for (uint32_t i = 0; i < 2; ++i)
            {
                const uint32_t id = 3001;
                const uint16_t flag = 0x8;
                std::memcpy(&odf[64 * i], &id, 4);
                std::memcpy(&odf[64 * i + 56], &flag, 2);
            }
            o.Chunk("MDDF", ddf);
            o.Chunk("MODF", odf);
        }
        else
        {
            o.Chunk("MMDX", mmdx);
            o.Chunk("MMID", mmid);
            o.Chunk("MWMO", mwmo);
            o.Chunk("MWID", mwid);
            o.Chunk("MDDF", mddf);
            o.Chunk("MODF", modf);
        }
        for (uint32_t i = 0; i < 256; ++i)
        {
            const SplitChunkData& c = chunks[i];
            std::string header = c.header;
            if (c.holes)
            {
                // Each of the 16 holes as the four of the 64 it covers.
                uint8_t high[8] = {};
                for (uint32_t row = 0; row < 4; ++row)
                    for (uint32_t col = 0; col < 4; ++col)
                        if (c.holes & (1u << (4 * row + col)))
                        {
                            high[2 * row] |= static_cast<uint8_t>(3u << (2 * col));
                            high[2 * row + 1] |= static_cast<uint8_t>(3u << (2 * col));
                        }
                const uint32_t flags = SplitU32(reinterpret_cast<const uint8_t*>(header.data())) | 0x10000u;
                std::memcpy(&header[0x00], &flags, 4);
                std::memcpy(&header[0x14], high, 8);
            }
            SplitIff body;
            body.bytes = header;
            body.Chunk("MCVT", c.mcvt);
            body.Chunk("MCNR", c.mcnr);
            r.Chunk("MCNK", body.bytes);
            SplitIff tb;
            tb.Chunk("MCLY", c.mcly);
            if (!c.mcsh.empty()) tb.Chunk("MCSH", c.mcsh);
            if (!c.mcal.empty()) tb.Chunk("MCAL", c.mcal);
            t.Chunk("MCNK", tb.bytes);
            SplitIff ob;
            std::string mcrd, mcrw;
            for (uint32_t d : c.doodads) mcrd += SplitWords({d});
            for (uint32_t w : c.mapObjs) mcrw += SplitWords({w});
            if (!mcrd.empty()) ob.Chunk("MCRD", mcrd);
            if (!mcrw.empty()) ob.Chunk("MCRW", mcrw);
            o.Chunk("MCNK", ob.bytes);
        }
        return twin;
    }

    /// The split files the test's provider answers.
    const SplitTwin* g_splitFiles = nullptr;
    uint32_t         g_splitAsked = 0;
    std::string      g_splitPath;

    /// The listfile the test's provider knows: the by-FileDataID twin's ids, and not 1005.
    wxl_bool WXL_CALL SplitName(void*, uint32_t id, char* out, uint32_t capacity)
    {
        const char* name = nullptr;
        switch (id)
        {
        case 1001: name = "t\\a.blp"; break;
        case 1002: name = "t\\b.blp"; break;
        case 1003: name = "t\\c.blp"; break;
        case 1004: name = "t\\d.blp"; break;
        case 2001: name = "m\\a.m2"; break;
        case 2002: name = "m\\bb.m2"; break;
        case 3001: name = "w\\a.wmo"; break;
        default: return WXL_FALSE;
        }
        std::snprintf(out, capacity, "%s", name);
        return WXL_TRUE;
    }

    void WXL_CALL SplitProvide(void*, const wxl_terrain_tile* tile, wxl_terrain_tile_files* out)
    {
        ++g_splitAsked;
        g_splitPath.assign(tile->path.data, tile->path.size);
        if (!g_splitFiles) return;
        out->data[WXL_TERRAIN_TILE_TEX0] = g_splitFiles->tex0.bytes.data();
        out->size[WXL_TERRAIN_TILE_TEX0] = static_cast<uint32_t>(g_splitFiles->tex0.bytes.size());
        out->data[WXL_TERRAIN_TILE_OBJ0] = g_splitFiles->obj0.bytes.data();
        out->size[WXL_TERRAIN_TILE_OBJ0] = static_cast<uint32_t>(g_splitFiles->obj0.bytes.size());
    }

    /// The tile read as the client reads it, with the seams called where the fork calls them.
    void SplitLoad(SplitFakeArea& a, std::vector<SplitFakeChunk>& chunks, bool seams)
    {
        if (seams) wxl::seam::MapArea::ProcessIffChunks::Before(&a);
        SplitStockArea(a);
        if (seams) wxl::seam::MapArea::ProcessIffChunks::After(&a);
        chunks.assign(256, SplitFakeChunk{});
        for (int32_t i = 0; i < 256; ++i) SplitStockChunk(a, chunks[static_cast<size_t>(i)], i % 16, i / 16, seams);
    }

    std::string SplitSpan(const void* p, size_t n) { return p ? std::string(static_cast<const char*>(p), n) : std::string(); }

    void CheckSplitTiles()
    {
        namespace tiles = wxl::core::services::terrain_tiles;
        const SplitTwin twin = MakeSplitTwin();
        // The tile's map object is in the client's files (a missing one is left out: it would end the client).
        wxl::core::services::model_gate::Clear();
        g_files.push_back(FakeFile{"w\\a.wmo", std::string("REVM\4\0\0\0\x11\0\0\0", 12)});

        // The monolithic tile, with and without the seams: the seams leave it alone.
        SplitFakeArea mono;
        mono.x = 30, mono.y = 40;
        mono.file  = "World\\Maps\\T\\T_30_40.adt";
        mono.bytes = twin.mono.bytes;
        std::vector<SplitFakeChunk> monoChunks;
        SplitLoad(mono, monoChunks, false);
        SplitFakeArea seen = mono;
        seen.bytes         = twin.mono.bytes;
        seen.textures.clear();
        std::vector<SplitFakeChunk> seenChunks;
        wxl_terrain_tile_provider provider{};
        provider.struct_size = sizeof provider;
        provider.fn          = &SplitProvide;
        Check(tiles::SetProvider(0, &provider) == WXL_OK, "a tile provider is taken");
        g_splitFiles = &twin;
        g_splitAsked = 0;
        SplitLoad(seen, seenChunks, true);
        bool same = g_splitAsked == 0 && tiles::Held() == 0 && seen.textures == mono.textures && seen.doodadCount == mono.doodadCount;
        for (size_t i = 0; i < 256; ++i)
            same = same && seenChunks[i].placed == monoChunks[i].placed && SplitU32(seenChunks[i].header + 0x0C) == SplitU32(monoChunks[i].header + 0x0C);
        Check(same, "a tile with MCIN: no provider asked, nothing held, read as the client reads it");

        // The split tile.
        SplitFakeArea split;
        split.x = 30, split.y = 40;
        split.file  = "World\\Maps\\T\\T_30_40.adt";
        split.bytes = twin.root.bytes;
        std::vector<SplitFakeChunk> splitChunks;
        SplitLoad(split, splitChunks, true);
        Check(g_splitAsked == 1 && g_splitPath == "World\\Maps\\T\\T_30_40.adt" && tiles::Held() == 1,
              "a root without MCIN: the provider is asked once, by the tile's name, and the tile held");

        Check(split.textures == mono.textures && split.textures.size() == 5, "its textures are _tex0's MTEX, loaded by LoadTextures in order");
        bool flags = split.textureFlags && mono.textureFlags;
        for (size_t k = 0; flags && k < 5; ++k) flags = split.textureFlags[k] == mono.textureFlags[k];
        Check(flags, "its texture flags are _tex0's MTXF");
        Check(split.doodadCount == mono.doodadCount && split.mapObjCount == mono.mapObjCount &&
                  SplitSpan(split.doodadDef, 36 * 3) == SplitSpan(mono.doodadDef, 36 * 3) &&
                  SplitSpan(split.mapObjDef, 64 * 2) == SplitSpan(mono.mapObjDef, 64 * 2),
              "its placements are _obj0's MDDF and MODF, with their counts");
        bool names = true;
        for (int32_t d = 0; d < 3; ++d)
        {
            const uint32_t id = SplitU32(split.doodadDef + 36 * d);
            names = names && std::string(split.m2Names + split.modelOffsets[id]) == std::string(mono.m2Names + mono.modelOffsets[id]);
        }
        names = names && std::string(split.wmoNames + split.wmoOffsets[0]) == std::string(mono.wmoNames + mono.wmoOffsets[0]);
        Check(names, "each placement's model name through MMID and MWID, as in one file");
        Check(split.flying && mono.flying && SplitSpan(split.flying, 36) == SplitSpan(mono.flying, 36), "MFBO the client's own, from the root");
        Check(SplitU32(split.chunkInfo + 8) == 0, "the chunk index is the root's MCNKs, flags left to the client");

        bool heights = true, layers = true, alpha = true, shadow = true, holes = true, placed = true, counts = true;
        for (size_t i = 0; i < 256; ++i)
        {
            const SplitFakeChunk& s = splitChunks[i];
            const SplitFakeChunk& o = monoChunks[i];
            heights = heights && SplitSpan(s.height, 145 * 4) == SplitSpan(o.height, 145 * 4) && SplitSpan(s.normals, 448) == SplitSpan(o.normals, 448) &&
                      SplitU32(s.header + 0x34) == SplitU32(o.header + 0x34);
            const uint32_t n = SplitU32(s.header + 0x0C);
            layers = layers && n == SplitU32(o.header + 0x0C) && SplitSpan(s.layers, 16 * n) == SplitSpan(o.layers, 16 * n) && (n || !s.layers);
            const uint32_t sizeAlpha = SplitU32(s.header + 0x28);
            alpha = alpha && sizeAlpha == SplitU32(o.header + 0x28) && SplitSpan(s.alpha, sizeAlpha ? sizeAlpha - 8 : 0) == SplitSpan(o.alpha, sizeAlpha ? sizeAlpha - 8 : 0);
            shadow = shadow && (SplitU32(s.header) & 1) == (SplitU32(o.header) & 1) && SplitSpan(s.shadow, s.shadow ? 512 : 0) == SplitSpan(o.shadow, o.shadow ? 512 : 0);
            uint16_t hs, ho;
            std::memcpy(&hs, s.header + 0x3C, 2);
            std::memcpy(&ho, o.header + 0x3C, 2);
            holes  = holes && hs == ho;
            placed = placed && s.placed == o.placed;
            counts = counts && SplitU32(s.header + 0x10) == SplitU32(o.header + 0x10) && SplitU32(s.header + 0x38) == SplitU32(o.header + 0x38);
        }
        Check(heights, "every chunk's heights, normals and header are the root's own");
        Check(layers, "every chunk's layers are its _tex0 MCLY, its count the client's (at most 4: a chunk naming 6 keeps its first 4)");
        Check(alpha, "its alpha maps are its _tex0 MCAL, sizeAlpha as one file says it");
        Check(shadow, "its shadow its _tex0 MCSH, under the root's flag");
        Check(holes && (SplitU32(splitChunks[5].header) & 0x10000) && splitChunks[5].header[0x3C], "64 holes folded into the client's 16 (the 2 x 2 under each)");
        Check(placed, "map objects then doodads made in the order one file makes them, from MCRW and MCRD");
        Check(counts, "the header's reference counts are MCRD's and MCRW's");

        // References the client cannot follow are left out; holes are not lost.
        SplitTwin odd = twin;
        {
            // Chunk 1's first doodad reference names a placement that does not exist; doodad 2 by FileDataID.
            const uint32_t bad = 9;
            size_t at = odd.obj0.bytes.find("DRCM");
            std::memcpy(&odd.obj0.bytes[at + 8], &bad, 4);
            at = odd.obj0.bytes.find("FDDM");
            const uint16_t byId = 0x40;
            std::memcpy(&odd.obj0.bytes[at + 8 + 36 * 2 + 34], &byId, 2);
        }
        g_splitFiles = &odd;
        SplitFakeArea oddArea;
        oddArea.x = 31, oddArea.y = 40;
        oddArea.file  = "World\\Maps\\T\\T_31_40.adt";
        oddArea.bytes = twin.root.bytes;
        uint8_t partial[8] = {0, 0, 0, 0, 0, 0, 0, 0x80}; // one hole of 64, at the bottom right
        const size_t mcnk0 = oddArea.bytes.find("KNCM");
        std::memcpy(&oddArea.bytes[mcnk0 + 8 + 0x14], partial, 8);
        const uint32_t hiFlag = 0x10000u;
        std::memcpy(&oddArea.bytes[mcnk0 + 8], &hiFlag, 4);
        const uint64_t leftBefore = tiles::Count().refsLeftOut;
        std::vector<SplitFakeChunk> oddChunks;
        SplitLoad(oddArea, oddChunks, true);
        size_t made = 0, expected = 0;
        for (size_t i = 0; i < 256; ++i)
        {
            made += oddChunks[i].placed.size();
            expected += monoChunks[i].placed.size();
        }
        uint16_t folded;
        std::memcpy(&folded, oddChunks[0].header + 0x3C, 2);
        Check(tiles::Count().refsLeftOut > leftBefore && made < expected && made > 0,
              "a reference to no placement, or to one named by FileDataID, is left out and the rest made");
        Check(folded == 0x8000, "a single hole of the 64 makes its quarter of the 16 a hole");

        // No provider: the ground loads, untextured, without placements.
        tiles::SetProvider(0, nullptr);
        SplitFakeArea bare;
        bare.x = 32, bare.y = 40;
        bare.file  = "World\\Maps\\T\\T_32_40.adt";
        bare.bytes = twin.root.bytes;
        std::vector<SplitFakeChunk> bareChunks;
        SplitLoad(bare, bareChunks, true);
        bool empty = bare.textures.empty() && bare.doodadCount == 0 && bare.mapObjCount == 0;
        for (size_t i = 0; i < 256; ++i)
            empty = empty && SplitU32(bareChunks[i].header + 0x0C) == 0 && bareChunks[i].placed.empty() &&
                    SplitSpan(bareChunks[i].height, 145 * 4) == SplitSpan(monoChunks[i].height, 145 * 4);
        Check(empty, "without a provider a split tile loads its ground, no layers, no placements");

        // The client frees the tiles: what was kept goes.
        wxl::seam::Map::FreeArea::Before(&split);
        wxl::seam::Map::FreeArea::Before(&oddArea);
        wxl::seam::Map::FreeArea::Before(&bare);
        Check(tiles::Held() == 0, "Map.FreeArea lets go of a split tile's files and index");

        // 8.1 and later: textures and models by FileDataID, named by the provider's name function.
        const SplitTwin byId = MakeSplitTwin(true);
        g_splitFiles         = &byId;
        provider.name_fn     = &SplitName;
        tiles::SetProvider(0, &provider);
        SplitFakeArea ids;
        ids.x = 33, ids.y = 40;
        ids.file  = "World\\Maps\\T\\T_33_40.adt";
        ids.bytes = byId.root.bytes;
        std::vector<SplitFakeChunk> idChunks;
        SplitLoad(ids, idChunks, true);
        Check(ids.textures.size() == 5 && ids.textures[0] == "t\\a.blp" && ids.textures[3] == "t\\d.blp" && ids.textures[4] == "FileDataID_1005.blp",
              "MDID's textures loaded by the names the provider gives, in order; one it does not name a placeholder");
        bool idSame = SplitSpan(ids.doodadDef, 36 * 3) == SplitSpan(mono.doodadDef, 36 * 3) &&
                      SplitSpan(ids.mapObjDef, 64 * 2) == SplitSpan(mono.mapObjDef, 64 * 2);
        bool idPlaced = true, idLayers = true;
        for (size_t i = 0; i < 256; ++i)
        {
            idPlaced = idPlaced && idChunks[i].placed == monoChunks[i].placed;
            // A layer naming texture 4 (FileDataID 1005, not named) ends the chunk's layers there.
            const uint32_t want = SplitU32(monoChunks[i].header + 0x0C);
            uint32_t       stop = want;
            for (uint32_t k = 0; k < want; ++k)
                if (SplitU32(monoChunks[i].layers + 16 * k) == 4)
                {
                    stop = k;
                    break;
                }
            idLayers = idLayers && SplitU32(idChunks[i].header + 0x0C) == stop;
        }
        Check(idSame, "placements by FileDataID become placements by name, as in one file");
        Check(idPlaced, "their models placed by the names the provider gives, in the client's order");
        Check(idLayers, "a layer of a texture no one named, and those over it, are left out");
        Check(tiles::Count().named >= 7 && tiles::Count().unnamed >= 1, "the ids named and not named are counted");
        wxl::seam::Map::FreeArea::Before(&ids);
        provider.name_fn = nullptr;
        tiles::SetProvider(0, nullptr);
        g_splitFiles = nullptr;

        // Models the client cannot load: their placements are left out, on any tile.
        namespace gate = wxl::core::services::model_gate;
        const uint8_t md21[8] = {'M', 'D', '2', '1', 0, 0, 0, 0}, md20new[8] = {'M', 'D', '2', '0', 0x10, 1, 0, 0},
                      md20[8] = {'M', 'D', '2', '0', 8, 1, 0, 0}, wmo18[12] = {'R', 'E', 'V', 'M', 4, 0, 0, 0, 18, 0, 0, 0},
                      wmo17[12] = {'R', 'E', 'V', 'M', 4, 0, 0, 0, 17, 0, 0, 0};
        Check(gate::Judge(md21, 8, false) == gate::Verdict::chunked && gate::Judge(md20new, 8, false) == gate::Verdict::newer &&
                  gate::Judge(md20, 8, false) == gate::Verdict::loadable && gate::Judge(wmo18, 12, true) == gate::Verdict::newer &&
                  gate::Judge(wmo17, 12, true) == gate::Verdict::loadable && gate::Judge(nullptr, 0, false) == gate::Verdict::loadable,
              "an M2 of MD21 or past version 264, a WMO root past 17, cannot be loaded; MD20 264 and WMO 17 can");
        gate::Clear();
        g_files.back().bytes[8] = 18; // w\a.wmo, now of version 18
        g_files.push_back(FakeFile{"m\\bb.m2", std::string("MD21\0\0\0\0", 8)});
        SplitFakeArea gated;
        gated.x = 30, gated.y = 41;
        gated.file  = "World\\Maps\\T\\T_30_41.adt";
        gated.bytes = twin.mono.bytes;
        std::vector<SplitFakeChunk> gatedChunks;
        SplitLoad(gated, gatedChunks, true);
        bool keptOthers = true;
        size_t left = 0;
        for (size_t i = 0; i < 256; ++i)
        {
            std::vector<std::string> expect;
            for (const std::string& p : monoChunks[i].placed)
                if (p.rfind("m2:m\\a.m2", 0) == 0) expect.push_back(p);
            left += monoChunks[i].placed.size() - expect.size();
            keptOthers = keptOthers && gatedChunks[i].placed == expect;
        }
        Check(keptOthers && left > 0 && gate::LeftOut() == left,
              "a tile in one file: placements of an MD21 M2 and of a WMO past 17 left out, the others made in the client's order");
        Check(tiles::Held() == 0, "and nothing kept for it");
        g_files.pop_back();
        g_files.pop_back();
        gate::Clear();
        SplitFakeArea missing;
        missing.x = 30, missing.y = 42;
        missing.file  = "World\\Maps\\T\\T_30_42.adt";
        missing.bytes = twin.mono.bytes;
        std::vector<SplitFakeChunk> missingChunks;
        SplitLoad(missing, missingChunks, true);
        bool noWmo = true, m2s = true;
        for (size_t i = 0; i < 256; ++i)
        {
            size_t models = 0;
            for (const std::string& p : missingChunks[i].placed) noWmo = noWmo && p.rfind("wmo:", 0) != 0;
            for (const std::string& p : monoChunks[i].placed) models += p.rfind("m2:", 0) == 0 ? 1 : 0;
            m2s = m2s && missingChunks[i].placed.size() == models;
        }
        Check(noWmo && m2s, "a map object root the client's files do not have is left out (it would end the client), the doodads made");
        gate::Clear();
    }

    void CheckTerrainLayerLimit()
    {
        namespace layers = wxl::core::services::terrain_layers;
        std::vector<uint8_t> file;
        const auto put = [&file](uint32_t v) {
            for (int i = 0; i < 4; ++i) file.push_back(static_cast<uint8_t>(v >> (8 * i)));
        };
        put(0x4D564552u); // MVER
        put(4);
        put(18);
        const uint32_t named[3] = {8, 4, 6};
        size_t         at[3]    = {};
        for (int c = 0; c < 3; ++c)
        {
            put(0x4D434E4Bu); // MCNK, its 128-byte header and nothing else
            put(128);
            at[c] = file.size();
            file.resize(file.size() + 128, 0);
            std::memcpy(&file[at[c] + 0x0C], &named[c], 4);
        }
        const uint64_t before = layers::Count().chunks_cut;
        const uint32_t cut    = layers::TileLoading(file.data(), static_cast<uint32_t>(file.size()));
        uint32_t       now[3];
        for (int c = 0; c < 3; ++c) std::memcpy(&now[c], &file[at[c] + 0x0C], 4);
        Check(cut == 2 && now[0] == 4 && now[1] == 4 && now[2] == 4 && layers::Count().chunks_cut == before + 2,
              "terrain layers: an MCNK naming 8 or 6 layers is cut to the client's 4 as its tile is read, one of 4 left");
        file.resize(file.size() - 64); // the last MCNK cut short: not read past the end
        std::memcpy(&file[at[2] + 0x0C], &named[2], 4);
        Check(layers::TileLoading(file.data(), static_cast<uint32_t>(file.size())) == 0, "terrain layers: a chunk running past the file's end is left");
        Check(layers::TileLoading(nullptr, 64) == 0, "terrain layers: no bytes, nothing");
    }

    // abi 0.1.43: a program made from a shader provider's answer, on Direct3D 9 and Vulkan.
    void CheckShaderCode()
    {
        namespace code = wxl::core::services::shader_code;
        const wxl_gx_api& gx = wxl::core::services::GxApi();
        static const uint32_t answer[3] = {0xFFFF0300u, 0x0000FFFFu, 0x0000FFFFu};
        static int            asked     = 0;
        wxl_gx_shader_provider provider{};
        provider.struct_size = sizeof provider;
        provider.fn          = [](void*, const wxl_gx_shader_code* in, wxl_gx_shader_answer* out) -> wxl_bool {
            ++asked;
            if (in->stage != 1 || std::strcmp(in->name, "Terrain1:6") != 0) return WXL_FALSE;
            out->code = answer;
            out->size = std::strcmp(in->name, "Terrain1:6") == 0 ? sizeof answer : 7;
            return WXL_TRUE;
        };
        Check(gx.struct_size >= offsetof(wxl_gx_api, set_shader_provider) + sizeof(void*) && gx.set_shader_provider(&provider) == WXL_OK,
              "shader code: a mod sets its shader provider");
        wxl_gx_shader_provider none{};
        none.struct_size = sizeof none;
        Check(gx.set_shader_provider(&none) == WXL_E_INVALID, "shader code: a provider without its function is refused");

        static const uint32_t own[2] = {0xFFFF0300u, 0x0000FFFFu};
        FakeShader terrain{"Terrain1:6", 4, own, sizeof own};
        wxl::seam::Gx::D3dIShaderCreatePixel::Before(nullptr, &terrain);
        const bool swapped = terrain.size == sizeof answer && std::memcmp(terrain.code, answer, sizeof answer) == 0 && terrain.code != answer;
        wxl::seam::Gx::D3dIShaderCreatePixel::After(nullptr, &terrain);
        Check(swapped && terrain.code == own && terrain.size == sizeof own,
              "shader code: Direct3D 9 makes the program from the answer (a copy), and the record gets its own code back");
        FakeShader ui{"UI:0", 4, own, sizeof own};
        wxl::seam::Gx::D3dIShaderCreatePixel::Before(nullptr, &ui);
        const bool left = ui.code == own;
        wxl::seam::Gx::D3dIShaderCreatePixel::After(nullptr, &ui);
        FakeShader vertex{"Terrain1:6", 0, own, sizeof own};
        wxl::seam::Gx::D3dIShaderCreateVertex::Before(nullptr, &vertex);
        const bool vleft = vertex.code == own;
        wxl::seam::Gx::D3dIShaderCreateVertex::After(nullptr, &vertex);
        Check(left && vleft && ui.code == own, "shader code: a program the provider does not answer is made from its own code");
        std::vector<uint8_t> out;
        Check(code::Answer("Terrain1:6", 1, own, sizeof own, out) && out.size() == sizeof answer && !code::Answer("Terrain1:6", 0, own, sizeof own, out),
              "shader code: the Vulkan renderer's translation is handed the same answer");
        const int before = asked;
        code::ForgetMod(0);
        Check(!code::Answer("Terrain1:6", 1, own, sizeof own, out) && asked == before && code::Count().answered >= 2,
              "shader code: a mod's provider goes with it");
        code::Clear();
    }

    void CheckTerrainService()
    {
        namespace grid = wxl::terrain;
        const wxl_terrain_api& api = wxl::core::services::TerrainApi();
        for (int32_t y = 0; y < 3; ++y)
            for (int32_t x = 0; x < 3; ++x)
            {
                FakeChunk& chunk = g_ground[y * 3 + x];
                chunk.cx = 500 + x;
                chunk.cy = 600 + y;
                chunk.Reset();
            }
        g_ground[6].holes = 1u; // chunk (500, 602), cells (0-1, 0-1)
        g_mapId           = 0;
        constexpr wxl::host::ModHost::ModToken kMod = 21, kOther = 22;
        const wxl::host::ModHost::ModToken     previous = g_host->SwapCurrentMod(kMod);

        const auto at = [](int32_t cx, int32_t cy, uint32_t i) { return grid::VertexPlane(cx, cy, i); };
        const auto heightAt = [&](grid::Point2 p, bool original) {
            double z = -1.0;
            return api.height(p.x, p.y, original ? WXL_TRUE : WXL_FALSE, &z) == WXL_TRUE ? z : -1.0;
        };
        const auto rel = [](int32_t cx, int32_t cy, uint32_t i) { return GroundChunk(cx, cy)->heights[i]; };
        const auto near = [](double a, double b) { return std::fabs(a - b) < 0.01; };
        const auto up = [](int32_t cx, int32_t cy, uint32_t i) { return GroundChunk(cx, cy)->normals[3 * i + 2] == 127; };
        const wxl_terrain_height lower{501, 601, grid::OuterIndex(0, 0), 90.0f};

        // No ground: out of the world, or a map without terrain.
        g_groundPresent = false;
        Check(api.set_heights(&lower, 1, 0) == WXL_E_UNSUPPORTED && heightAt(at(501, 601, 50), false) < 0.0,
              "terrain: without ground an edit is unsupported and no height answers");
        g_groundPresent = true;

        // Reads.
        const grid::Point2 inner = at(501, 601, grid::InnerIndex(4, 4));
        Check(near(heightAt(inner, false), 100.0) && api.presence(inner.x, inner.y) == WXL_TERRAIN_GROUND,
              "terrain: the height of flat ground, and ground there");
        const grid::Point2 hole = at(500, 602, grid::InnerIndex(0, 1));
        Check(api.presence(hole.x, hole.y) == WXL_TERRAIN_HOLE && heightAt(hole, false) < 0.0, "terrain: a hole has no height");
        const grid::Point2 far = at(510, 601, 70);
        Check(api.presence(far.x, far.y) == WXL_TERRAIN_NOT_LOADED && api.presence(20000.0, 0.0) == WXL_TERRAIN_NONE,
              "terrain: a chunk not loaded, and off the map");
        wxl_terrain_place place{};
        place.struct_size = sizeof place;
        Check(api.locate(inner.x, inner.y, &place) == WXL_TRUE && place.chunk_x == 501 && place.chunk_y == 601 &&
                  place.cell_row == 4 && place.cell_col == 4 && place.nearest == grid::InnerIndex(4, 4) && place.loaded == WXL_TRUE,
              "terrain: locate answers the chunk, the cell and the nearest vertex");
        wxl_vec3 vertices[145];
        Check(api.vertices(501, 601, WXL_FALSE, vertices, 145) == 145 && vertices[144].z == 100.0f &&
                  near(vertices[144].x, at(501, 601, 144).x) && near(vertices[144].y, at(501, 601, 144).y) &&
                  api.vertices(510, 601, WXL_FALSE, vertices, 145) == 0,
              "terrain: a loaded chunk's 145 vertices, none of one not loaded");

        // An edit of a corner vertex: the same ground in four chunks.
        for (FakeChunk& c : g_ground) c.rebuilds = c.clutterFreed = 0;
        g_groundForgets = 0;
        Check(api.set_heights(&lower, 1, 0) == WXL_OK, "terrain: an edit is taken");
        Check(rel(501, 601, 0) == -10.0f && rel(500, 601, grid::OuterIndex(0, 8)) == -10.0f &&
                  rel(501, 600, grid::OuterIndex(8, 0)) == -10.0f && rel(500, 600, 144) == -10.0f,
              "terrain: a corner vertex is set in the four chunks holding it, relative to each one's base");
        Check(!up(501, 601, grid::OuterIndex(0, 1)) && !up(500, 600, grid::InnerIndex(7, 7)) && up(501, 601, grid::OuterIndex(4, 4)),
              "terrain: the normals around it lean, across the edges, and the others keep the file's");
        Check(GroundChunk(500, 600)->normals[3 * 144 + 2] == 127, "terrain: the vertex's own normal stays up at a symmetric dip");
        Check(near(heightAt(at(501, 601, 0), false), 90.0) && near(heightAt(at(501, 601, 0), true), 100.0),
              "terrain: the height follows, the original stays readable");
        Check(g_ground[0].rebuilds == 1 && g_ground[1].rebuilds == 1 && g_ground[3].rebuilds == 1 && g_ground[4].rebuilds == 1 &&
                  g_ground[4].clutterFreed == 1 && g_ground[8].rebuilds == 0 && g_groundForgets == 1,
              "terrain: the client rebuilds the chunks reshaped, frees their clutter, and forgets the movement's facets");

        // A region in world space: each vertex once, the corner of four chunks in its own chunk.
        std::vector<wxl_terrain_vertex> region(64);
        const grid::Point2 corner = at(501, 601, 0);
        const uint32_t inCircle = api.vertices_in(corner.x, corner.y, 3.0, region.data(), 64);
        int  corners = 0;
        bool known   = true;
        for (uint32_t i = 0; i < inCircle && i < 64; ++i)
        {
            const wxl_terrain_vertex& v = region[i];
            if (std::fabs(v.x - corner.x) < 0.01f && std::fabs(v.y - corner.y) < 0.01f)
            {
                ++corners;
                known = known && v.chunk_x == 501 && v.chunk_y == 601 && v.index == 0 && near(v.z, 90.0) &&
                        near(v.original_z, 100.0) && v.loaded == WXL_TRUE && v.inner == WXL_FALSE;
            }
        }
        Check(inCircle == 5 && corners == 1 && known && api.vertices_in(corner.x, corner.y, 3.0, nullptr, 0) == 5,
              "terrain: vertices_in lists a circle's vertices once, the corner of four chunks in its own, with its height and original");
        const grid::Point2 beyond = at(503, 601, 0);
        std::vector<wxl_terrain_vertex> edge(32);
        const uint32_t onEdge = api.vertices_in(beyond.x, beyond.y, 3.0, edge.data(), 32);
        bool shared = false, away = false;
        for (uint32_t i = 0; i < onEdge && i < 32; ++i)
        {
            shared = shared || (edge[i].chunk_x == 503 && edge[i].index == 0 && edge[i].loaded == WXL_TRUE && near(edge[i].z, 100.0));
            away   = away || (edge[i].chunk_x == 503 && edge[i].index != 0 && edge[i].loaded == WXL_FALSE);
        }
        Check(shared && away, "terrain: a vertex shared with a loaded chunk reads its height there; one of a chunk not loaded says so");
        Check(api.vertices_in(corner.x, corner.y, 200.0, nullptr, 0) == 0, "terrain: a radius beyond the limit is refused");

        // Inside the world's drawing the clutter waits for its end.
        wxl::core::services::terrain::WorldBegin();
        const wxl_terrain_height again{501, 601, grid::InnerIndex(3, 3), 95.0f};
        api.set_heights(&again, 1, 0);
        const int freedInside = g_ground[4].clutterFreed;
        wxl::core::services::terrain::WorldEnd();
        Check(freedInside == 1 && g_ground[4].clutterFreed == 2, "terrain: clutter reshaped while the world is drawn is freed after it");

        // A trace meets the edited ground.
        wxl_terrain_hit hit{};
        hit.struct_size = sizeof hit;
        const grid::Point2 dip = at(501, 601, grid::InnerIndex(3, 3));
        Check(api.trace(wxl_vec3{dip.x, dip.y, 200.0f}, wxl_vec3{dip.x, dip.y, 0.0f}, &hit) == WXL_TRUE &&
                  std::fabs(hit.point.z - 95.0f) < 0.01f && hit.chunk_x == 501 && hit.chunk_y == 601 && hit.normal.z > 0.0f &&
                  std::fabs(hit.fraction - 0.525f) < 0.001f,
              "terrain: a trace meets the edited ground, with its chunk and an upward normal");
        const grid::Point2 a = at(500, 601, 70), b = at(502, 601, 70);
        Check(api.trace(wxl_vec3{a.x, a.y, 101.0f}, wxl_vec3{b.x, b.y, 99.0f}, &hit) == WXL_TRUE && hit.chunk_x == 501 &&
                  std::fabs(hit.point.z - 100.0f) < 0.01f,
              "terrain: a slanting trace across chunks meets the ground where it crosses it");
        Check(api.trace(wxl_vec3{hole.x, hole.y, 200.0f}, wxl_vec3{hole.x, hole.y, 0.0f}, &hit) == WXL_FALSE,
              "terrain: a trace goes through a hole");

        // Owners: another mod's edit shows over this one's, and goes back to it.
        g_host->SwapCurrentMod(kOther);
        const wxl_terrain_height deeper{501, 601, grid::OuterIndex(0, 0), 80.0f};
        api.set_heights(&deeper, 1, 0);
        const double shown = heightAt(at(501, 601, 0), false);
        Check(api.reset_chunk(501, 601, 0) == WXL_OK && near(heightAt(at(501, 601, 0), false), 90.0) && near(shown, 80.0),
              "terrain: the newest edit shows; its reset gives the one before back");
        g_host->SwapCurrentMod(kMod);
        Check(api.reset_chunk(501, 601, 0) == WXL_OK && rel(500, 600, 144) == 0.0f && near(heightAt(at(501, 601, 0), false), 100.0),
              "terrain: a chunk's reset takes the edge vertex off its neighbours too");
        bool allUp = true;
        for (const FakeChunk& c : g_ground)
            for (uint32_t i = 0; i < 145; ++i) allUp = allUp && c.normals[3 * i] == 0 && c.normals[3 * i + 2] == 127;
        Check(allUp, "terrain: with no edit left, every normal is the file's again");
        Check(api.reset_chunk(501, 601, 0) == WXL_E_NOT_FOUND && api.reset_all(0) == WXL_E_NOT_FOUND,
              "terrain: nothing to reset is not found");

        // Layers.
        const wxl_terrain_height one{502, 602, 70, 101.0f}, two{502, 602, 71, 102.0f};
        api.set_heights(&one, 1, 1);
        api.set_heights(&two, 1, 2);
        Check(api.reset_all(1) == WXL_OK && rel(502, 602, 70) == 0.0f && rel(502, 602, 71) == 2.0f,
              "terrain: a layer is reset alone");

        // A chunk read again from the disk gets its edits back.
        g_received.clear();
        wxl::seam::MapChunk::Purge::Before(GroundChunk(502, 602));
        const Received* gone = Only(WXL_EV_TerrainChunkUnloaded);
        Check(gone && gone->As<wxl_terrain_chunk_state_args>().chunk_x == 502 && gone->As<wxl_terrain_chunk_state_args>().edited == WXL_TRUE,
              "terrain: MapChunk.Purge raises TerrainChunkUnloaded, edited");
        g_ground[8].Reset();
        g_received.clear();
        wxl_terrain_stats stats{};
        stats.struct_size = sizeof stats;
        api.stats(&stats);
        const uint64_t reapplied = stats.reapplied;
        wxl::seam::MapArea::PrepareChunk::After(&g_groundArea, 2, 2);
        const Received* back = Only(WXL_EV_TerrainChunkLoaded);
        api.stats(&stats);
        Check(rel(502, 602, 71) == 2.0f && !up(502, 602, 70) && back && back->As<wxl_terrain_chunk_state_args>().edited == WXL_TRUE &&
                  stats.reapplied == reapplied + 1,
              "terrain: MapArea.PrepareChunk After puts the edits back before TerrainChunkLoaded");
        Check(near(heightAt(at(502, 602, 71), true), 100.0), "terrain: and the originals are what the disk had");

        // PrepareChunk answers nothing on the stock client: EAX holds what CMapChunk::Create left
        // (5 in the crash of 2026-10-27). The After takes no answer and reads the tile's slot; a
        // stray value reaching the service is refused, and an empty slot raises nothing.
        static_assert(std::is_same_v<decltype(&wxl::seam::MapArea::PrepareChunk::After), void(WXL_CALL*)(void*, int32_t, int32_t)>,
                      "MapArea.PrepareChunk's After is handed no answer");
        g_received.clear();
        wxl::seam::MapArea::PrepareChunk::After(&g_groundArea, 9, 9);
        wxl::seam::MapArea::PrepareChunk::After(nullptr, 2, 2);
        wxl_terrain_chunk_state_args stray{};
        Check(g_received.empty() && !wxl::core::services::terrain::ChunkLoaded(reinterpret_cast<void*>(5), stray) &&
                  !wxl::core::services::terrain::ChunkUnloading(reinterpret_cast<void*>(5), stray),
              "terrain: an empty slot raises nothing, and a value left in EAX (5) is refused as a chunk, never read");
        {
            namespace ground = wxl::core::services::terrain;
            const uint64_t before = ground::Generation();
            Mute(WXL_EV_TerrainChunkUnloaded);
            Mute(WXL_EV_TerrainChunkLoaded);
            wxl::seam::MapArea::PrepareChunk::After(&g_groundArea, 1, 1);
            const uint64_t loaded = ground::Generation();
            wxl::seam::MapChunk::Purge::Before(GroundChunk(501, 601));
            Unmute(WXL_EV_TerrainChunkUnloaded);
            Unmute(WXL_EV_TerrainChunkLoaded);
            Check(loaded > before && ground::Generation() > loaded,
                  "terrain: a chunk read from its tile's slot and one freed move the ground's generation, nobody listening");
        }
        GroundChunk(502, 602)->loaded = false;
        Check(!wxl::core::services::terrain::ChunkLoaded(&g_ground[8], stray), "terrain: a chunk its tile no longer holds is refused");
        g_ground[8].loaded = true;

        // A player standing still on the ground falls when it goes down; moving, or above it, not.
        FakeObject me;
        me.typeId = WXL_OBJECT_PLAYER;
        const grid::Point2 feet = at(501, 601, grid::InnerIndex(5, 5));
        me.position = wxl_vec3{feet.x, feet.y, 100.0f};
        g_localPlayer = &me;
        g_groundFalls = 0;
        const wxl_terrain_height underfoot{501, 601, grid::InnerIndex(5, 5), 98.0f};
        api.set_heights(&underfoot, 1, 0);
        Check(g_groundFalls == 1, "terrain: a player standing on ground that goes down falls");
        me.position.z     = 98.0f;
        me.movementFlags  = 0x1u;
        const wxl_terrain_height lowerStill{501, 601, grid::InnerIndex(5, 5), 96.0f};
        api.set_heights(&lowerStill, 1, 0);
        me.movementFlags = 0;
        me.position.z    = 150.0f;
        const wxl_terrain_height lowest{501, 601, grid::InnerIndex(5, 5), 94.0f};
        api.set_heights(&lowest, 1, 0);
        const wxl_terrain_height raise{501, 601, grid::InnerIndex(5, 5), 160.0f};
        me.position.z = 94.0f;
        api.set_heights(&raise, 1, 0);
        Check(g_groundFalls == 1, "terrain: one moving, one on something above the ground, and ground raised start no fall");
        wxl::core::services::terrain::ForgetMod(kMod);

        // Ground raised under the player's feet lifts them onto it through the client's movement.
        const auto groundAt = [&](float x, float y) {
            double z = -1.0;
            api.height(x, y, WXL_FALSE, &z);
            return static_cast<float>(z);
        };
        me.position      = wxl_vec3{feet.x, feet.y, 100.0f};
        me.movementFlags = 0;
        me.lifts         = 0;
        g_groundFalls    = 0;
        const wxl_terrain_height step{501, 601, grid::InnerIndex(5, 5), 100.4f};
        api.set_heights(&step, 1, 0);
        Check(me.lifts == 1 && std::fabs(me.position.z - 100.4f) < 0.01f && g_groundFalls == 1,
              "terrain: a step's rise under a player standing still puts them on it at once, landing through the client's fall");
        me.movementFlags = 0x1u;
        const wxl_terrain_height stepMore{501, 601, grid::InnerIndex(5, 5), 100.9f};
        api.set_heights(&stepMore, 1, 0);
        Check(me.lifts == 2 && std::fabs(me.position.z - 100.9f) < 0.01f && g_groundFalls == 1,
              "terrain: a moving player is put on it too, the movement's next message carrying it");
        me.movementFlags = 0;

        // Partly under: the highest ground under the footprint, as the collision box rests.
        const grid::Point2 aside{feet.x + 0.15f, feet.y + 0.15f};
        wxl::core::services::terrain::ForgetMod(kMod);
        me.position = wxl_vec3{aside.x, aside.y, groundAt(aside.x, aside.y)};
        const wxl_terrain_height partly{501, 601, grid::InnerIndex(5, 5), 100.5f};
        api.set_heights(&partly, 1, 0);
        Check(me.position.z > groundAt(aside.x, aside.y) + 0.005f && me.position.z < 100.5f,
              "terrain: an edit partly under the player lifts them to the highest ground under their footprint");

        // A higher rise lifts them frame by frame, never more than a step at a time.
        wxl::core::services::terrain::ForgetMod(kMod);
        g_localPlayer = &me;
        me.position   = wxl_vec3{feet.x, feet.y, 100.0f};
        me.lifts      = 0;
        g_groundFalls = 0;
        g_timeMs      = 50000;
        const wxl_terrain_height mound{501, 601, grid::InnerIndex(5, 5), 103.0f};
        api.set_heights(&mound, 1, 0);
        Check(me.lifts == 0 && wxl::core::services::terrain::Lifting(), "terrain: three yards are not put at once");
        float lastZ = me.position.z, worstStep = 0.0f;
        int   frames = 0;
        while (wxl::core::services::terrain::Lifting() && frames < 100)
        {
            g_timeMs += 33;
            wxl::seam::Unit::PreAnimate::After(&me, nullptr);
            worstStep = std::max(worstStep, me.position.z - lastZ);
            lastZ     = me.position.z;
            ++frames;
        }
        Check(!wxl::core::services::terrain::Lifting() && std::fabs(me.position.z - groundAt(feet.x, feet.y)) < 0.05f &&
                  worstStep <= 8.0f * 0.033f + 0.01f && frames > 5 && g_groundFalls == 1,
              "terrain: they rise at a steady pace, in steps no higher than a frame's, and land on top");

        // Nothing is lifted through what stands above: a roof, a floor of a building.
        wxl::core::services::terrain::ForgetMod(kMod);
        me.position = wxl_vec3{feet.x, feet.y, 100.0f};
        me.lifts    = 0;
        g_roofZ     = 101.5f;
        api.set_heights(&step, 1, 0);
        Check(me.lifts == 0 && !wxl::core::services::terrain::Lifting(), "terrain: ground raised under a roof lifts nobody");
        g_roofZ = 0.0f;
        wxl::core::services::terrain::ForgetMod(kMod);
        g_localPlayer = nullptr;

        // Refusals: nothing set.
        const wxl_terrain_height bad[2] = {{501, 601, 3, 90.0f}, {501, 601, 145, 90.0f}};
        const wxl_terrain_height nan{501, 601, 3, std::nanf("")};
        Check(api.set_heights(bad, 2, 0) == WXL_E_INVALID && rel(501, 601, 3) == 0.0f && api.set_heights(&nan, 1, 0) == WXL_E_INVALID &&
                  api.reset_chunk(-1, 0, 0) == WXL_E_INVALID,
              "terrain: an entry off the grid or a height not a number refuses the whole edit");
        wxl_terrain_hit unsized{};
        Check(api.trace(wxl_vec3{0, 0, 0}, wxl_vec3{0, 0, 1}, &unsized) == WXL_FALSE, "terrain: an answer without its struct_size is refused");

        // The cost of an edit: a digging brush (about 5 yards: the vertices within it) and every
        // vertex of the 3 x 3 chunks, with their normals and rebuilds.
        std::vector<wxl_terrain_height> brush;
        for (uint32_t i = 0; i < 145; ++i)
        {
            const grid::Point2 p = at(501, 601, i);
            const float dx = p.x - feet.x, dy = p.y - feet.y;
            if (dx * dx + dy * dy <= 25.0f) brush.push_back({501, 601, i, 97.0f});
        }
        uint32_t brushWorst = 0, brushTotal = 0;
        for (int n = 0; n < 50; ++n)
        {
            for (auto& e : brush) e.height = 97.0f - 0.01f * n;
            api.set_heights(brush.data(), static_cast<uint32_t>(brush.size()), 3);
            api.stats(&stats);
            brushTotal += stats.last_microseconds;
            brushWorst = std::max(brushWorst, stats.last_microseconds);
        }
        std::vector<wxl_terrain_height> block;
        for (int32_t cy = 600; cy <= 602; ++cy)
            for (int32_t cx = 500; cx <= 502; ++cx)
                for (uint32_t i = 0; i < 145; ++i) block.push_back({cx, cy, i, 99.0f + 0.001f * i});
        api.set_heights(block.data(), static_cast<uint32_t>(block.size()), 3);
        api.stats(&stats);
        std::printf("  terrain: an edit of %zu vertices (a 5-yard brush) took %u us on average, %u at worst, over 50; "
                    "all %zu vertices of 3 x 3 chunks %u us (%u written, %u chunks)\n",
                    brush.size(), brushTotal / 50, brushWorst, block.size(), stats.last_microseconds, stats.last_vertices,
                    stats.last_chunks);
        Check(stats.last_chunks == 9 && stats.edited_chunks == 25,
              "terrain: the block edit reshaped the nine chunks loaded, and holds the copies of its edge in the sixteen around");

        // A mod that goes takes its edits; the map unloading drops all and says so.
        wxl::core::services::terrain::ForgetMod(kMod);
        api.stats(&stats);
        Check(stats.edited_chunks == 0 && rel(501, 601, 0) == 0.0f, "terrain: a mod's edits come off when it goes");
        api.set_heights(&lower, 1, 0);
        g_received.clear();
        wxl::seam::Map::Unload::Before();
        const Received* cleared = Only(WXL_EV_TerrainEditsCleared);
        api.stats(&stats);
        Check(cleared && cleared->As<wxl_terrain_cleared_args>().chunks == 4 && stats.edited_chunks == 0,
              "terrain: Map.Unload drops every edit and raises TerrainEditsCleared with the chunks that had some");

        g_host->SwapCurrentMod(previous);
        g_groundPresent = false;
        wxl::core::services::terrain::Clear();
    }

    /// MapArea: PrepareChunk's Before raises TerrainChunkBuild with the map, the tile and the chunk.
    void CheckMapArea()
    {
        FakeArea area;
        area.tileX = 32;
        area.tileY = 48;
        g_mapId    = 0;

        g_received.clear();
        wxl::seam::MapArea::PrepareChunk::Before(&area, 3, 9);
        const Received* built = Only(WXL_EV_TerrainChunkBuild);
        Check(built != nullptr, "MapArea.PrepareChunk raises TerrainChunkBuild once");
        if (!built) return;
        const wxl_terrain_chunk_args args = built->As<wxl_terrain_chunk_args>();
        Check(args.map_id == 0u && args.tile_x == 32 && args.tile_y == 48,
              "with the map and the tile index read through the binding");
        Check(args.chunk_x == 3 && args.chunk_y == 9, "and the chunk the call named");
    }

    // --- wxl.script: what a mod registers, called back through the fake engine -----------------

    wxl_str Str(const char* text) { return wxl_str{text, static_cast<uint32_t>(strlen(text))}; }

    const wxl_script_api& Script() { return wxl::core::services::ScriptApi(); }

    int      g_greetCalls      = 0;
    uint32_t g_greetArgTypes[3]{};
    bool     g_greetWrongTypeMissed = false;

    /// Greet(n, name): answers "hi <name>", n * 2, true, nil; fails on anything else.
    void WXL_CALL Greet(wxl_script_call* call, void* user)
    {
        const wxl_script_api& api = Script();
        if (user) *static_cast<int*>(user) += 1;

        double  n = 0.0;
        wxl_str s{nullptr, 0};
        if (api.arg_count(call) != 2 || !api.arg_number(call, 0, &n) || !api.arg_string(call, 1, &s))
        {
            api.fail(call, Str("usage: Greet(number, string)"));
            return;
        }
        for (uint32_t i = 0; i < 3; ++i) g_greetArgTypes[i] = api.arg_type(call, i);
        wxl_bool flag = WXL_FALSE;
        wxl_str  text{nullptr, 0};
        g_greetWrongTypeMissed = api.arg_bool(call, 0, &flag) == WXL_FALSE &&
                                 api.arg_string(call, 0, &text) == WXL_FALSE &&
                                 api.arg_number(call, 1, &n) == WXL_FALSE && n == 21.0;

        char line[64];
        snprintf(line, sizeof line, "hi %.*s", static_cast<int>(s.size), s.data);
        api.push_string(call, Str(line));
        api.push_number(call, n * 2);
        api.push_bool(call, WXL_TRUE);
        api.push_nil(call);
    }

    /// Fails after pushing: the push must not reach the caller.
    void WXL_CALL Failing(wxl_script_call* call, void*)
    {
        Script().push_number(call, 1.0);
        Script().fail(call, Str("nope"));
    }

#if defined(_MSC_VER)
    void WXL_CALL Crashing(wxl_script_call*, void*)
    {
        volatile int* nowhere = nullptr;
        *nowhere              = 1;
    }
#endif

    /// A mod's ScriptVmReady handler: registers its function on every new context and keeps
    /// the answer in `user`.
    void WXL_CALL OnVmReadyRegister(void* user, const void*)
    {
        *static_cast<wxl_result*>(user) = Script().register_function("WxlHarnessGreet", &Greet, &g_greetCalls);
    }

    __declspec(noinline) int32_t WXL_CALL CodeProbe(void*) { return 0; }

    /// The code the client accepts: the runtime's image and the binaries the host maps, by their
    /// executable sections; nothing else, the heap, the stack and generated code included.
    void CheckCode()
    {
        namespace code = wxl::core::code;
        namespace os   = wxl::seam::Os::ValidateFunctionPointer;
        code::Reset();
        const void* own = reinterpret_cast<const void*>(&CodeProbe);
        Check(!code::Accepts(own), "with nothing registered, no pointer is accepted");
        Check(code::AddOwnImage() && code::Accepts(own) &&
                  code::Accepts(reinterpret_cast<const void*>(&wxl::core::code::Accepts)),
              "the runtime's own image is accepted once registered");
        Check(code::AddOwnImage() && code::Accepts(own), "registering it twice keeps one set of ranges");

        std::unique_ptr<char[]> heap(new char[64]);
        int                     local     = 0;
        void* const             generated = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        Check(!code::Accepts(heap.get()) && !code::Accepts(&local) && !code::Accepts(generated) &&
                  !code::Accepts(nullptr),
              "the heap, the stack, generated code and null are not");
        HMODULE exe = GetModuleHandleW(nullptr);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const uint8_t*>(exe) + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
        Check(!code::Accepts(exe) && !code::Accepts(reinterpret_cast<const uint8_t*>(exe) + nt->OptionalHeader.SizeOfImage - 1),
              "nor the image's headers and data: only its executable sections");

        const void* pointer = own;
        Check(os::Gate(&pointer) && pointer == own, "Os.ValidateFunctionPointer takes the call for the runtime's code");
        pointer = heap.get();
        Check(!os::Gate(&pointer) && pointer == heap.get(), "and leaves any other pointer to the client's check");

        // A binary the host maps, through the loader the runtime gives it: a system DLL stands in.
        wchar_t system[MAX_PATH];
        const UINT n = GetSystemDirectoryW(system, MAX_PATH);
        const std::filesystem::path dll = std::filesystem::path(std::wstring(system, n)) / L"version.dll";
        std::string     error;
        wxl::host::LibraryLoader& loader  = code::HostLoader();
        void* const               library = loader.Open(dll, error);
        const void* const         export_ = library ? loader.Symbol(library, "GetFileVersionInfoSizeW") : nullptr;
        Check(library && export_ && code::Accepts(export_), "a function of a binary the host loader opened is accepted");
        pointer = export_;
        Check(export_ && os::Gate(&pointer), "and the gate takes the call for it");
        if (library) loader.Close(library);
        Check(export_ && !code::Accepts(export_) && code::Accepts(own),
              "closing the binary takes its code back and leaves the runtime's");
        Check(!code::Handable(heap.get(), "harness"), "a pointer outside is refused before it is handed over");
        if (generated) VirtualFree(generated, 0, MEM_RELEASE);
    }

    /// FrameScript: a context Initialize makes is announced at its first CreateEvents, with the
    /// context CGGameUI's state implied when it was made; Destroy's Before raises ScriptVmClosing
    /// naming the same one, and a placeholder the client destroys before any interface raises
    /// nothing. The service takes registrations only between the two, drives calls through the
    /// engine, and starts over on the next context.
    void CheckFrameScript()
    {
        const wxl_script_api& api = Script();
        Check(g_host->Services().Get(WXL_SCRIPT_SERVICE, WXL_SCRIPT_SERVICE_VERSION) == &api,
              "wxl.script is published to mods");
        Check(api.struct_size == sizeof(wxl_script_api), "with the layout script.h declares");

        Check(api.register_function("WxlHarnessEarly", &Greet, nullptr) == WXL_E_FAILED,
              "register_function before any context exists is refused");
        Check(api.register_event("WXL_HARNESS_EARLY") == WXL_E_FAILED &&
                  api.fire_event("WXL_HARNESS_EARLY", nullptr, 0) == WXL_E_FAILED &&
                  api.run(Str("x = 1"), "early") == WXL_E_FAILED,
              "so are register_event, fire_event and run");
        Check(api.live() == WXL_FALSE, "and live() says no context is live");

        // The placeholder InitializeClient makes: destroyed before any interface is loaded.
        g_vm.live = true;
        g_received.clear();
        wxl::seam::FrameScript::Initialize::After(1, 0);
        Check(g_received.empty() && api.live() == WXL_FALSE &&
                  api.register_function("WxlHarnessEarly", &Greet, nullptr) == WXL_E_FAILED,
              "a context is not announced when it is made, and takes nothing before");
        wxl::seam::FrameScript::Destroy::Before();
        Check(g_received.empty(), "a context destroyed before its event table is installed raises nothing at all");

        wxl_result registered = WXL_E_FAILED;
        const wxl_subscription onReady =
            g_host->Events().Subscribe(WXL_EV_ScriptVmReady, &OnVmReadyRegister, &registered, 0);

        // The login-side context: CGGameUI is neither logging in nor initialised.
        g_vm.live       = true;
        g_uiInitialized = 0;
        g_uiLoggingIn   = 0;
        g_received.clear();
        const char* clientNames[] = {"PLAYER_LOGIN", "PLAYER_LOGOUT"};
        wxl::seam::FrameScript::Initialize::After(1, 0);
        wxl::seam::FrameScript::CreateEvents::After(clientNames, 2);
        const Received* ready = Only(WXL_EV_ScriptVmReady);
        Check(ready != nullptr, "FrameScript.CreateEvents raises ScriptVmReady once, for the context made before it");
        if (ready)
            Check(ready->As<wxl_script_vm_args>().context == WXL_SCRIPT_CONTEXT_GLUE &&
                      ready->As<wxl_script_vm_args>().native_state == &g_vm,
                  "it says glue while CGGameUI is neither logging in nor initialised, and carries the client's state");
        Check(api.context() == WXL_SCRIPT_CONTEXT_GLUE && api.live() == WXL_TRUE, "context() says the same, live() yes");
        g_received.clear();
        wxl::seam::FrameScript::CreateEvents::After(clientNames, 2);
        Check(!Only(WXL_EV_ScriptVmReady), "a second table in the same context announces nothing");
        Check(registered == WXL_OK && g_vm.closures.count("WxlHarnessGreet") == 1,
              "a function registered from the ScriptVmReady handler is a global of that context");
        {
            const void* pushed = reinterpret_cast<const void*>(g_vm.closures["WxlHarnessGreet"].fn);
            Check(pushed && wxl::seam::Os::ValidateFunctionPointer::Gate(&pushed),
                  "the function the global is pushed as is code the client's pointer check accepts");
            wxl::core::code::Reset();
            Check(api.register_function("WxlHarnessUnhanded", &Greet, nullptr) == WXL_E_FAILED &&
                      g_vm.closures.count("WxlHarnessUnhanded") == 0,
                  "with the runtime's image unregistered, nothing is pushed: the client would end at the call");
            wxl::core::code::AddOwnImage();
        }
        Check(api.register_function("WxlHarnessGreet", &Greet, nullptr) == WXL_E_INVALID,
              "registering a name again is refused");
        g_vm.clientGlobals.insert("GetTime");
        Check(api.register_function("GetTime", &Greet, nullptr) == WXL_E_INVALID,
              "a global the client set is not shadowed");
        Check(api.register_function(nullptr, &Greet, nullptr) == WXL_E_INVALID &&
                  api.register_function("", &Greet, nullptr) == WXL_E_INVALID &&
                  api.register_function("WxlHarnessNull", nullptr, nullptr) == WXL_E_INVALID,
              "a null name or function is refused");

        bool found = false;
        std::vector<FakeValue> results = CallGlobal("WxlHarnessGreet", {Number(21.0), String("bob")}, &found);
        Check(found && g_greetCalls == 1, "a call from script reaches the mod's function");
        Check(results.size() == 4 && results[0].type == WXL_VALUE_STRING && results[0].string == "hi bob" &&
                  results[1].type == WXL_VALUE_NUMBER && results[1].number == 42.0 &&
                  results[2].type == WXL_VALUE_BOOL && results[2].boolean &&
                  results[3].type == WXL_VALUE_NIL,
              "with its arguments, and brings back what it pushed, in order");
        Check(g_greetArgTypes[0] == WXL_VALUE_NUMBER && g_greetArgTypes[1] == WXL_VALUE_STRING &&
                  g_greetArgTypes[2] == WXL_VALUE_NIL,
              "arg_type names each argument's type and nil past the last");
        Check(g_greetWrongTypeMissed, "an argument of the wrong type is a miss, not a conversion");
        results = CallGlobal("WxlHarnessGreet", {Number(1.0)}, &found);
        Check(results.empty() && !g_vm.errors.empty() && g_vm.errors.back() == "usage: Greet(number, string)",
              "fail raises its message on the script side once the function returns");

        Check(api.register_function("WxlHarnessFail", &Failing, nullptr) == WXL_OK, "a second function registers");
        results = CallGlobal("WxlHarnessFail", {}, &found);
        Check(found && results.empty() && g_vm.errors.back() == "nope",
              "a function that fails after pushing hands the caller the error, not the results");

        Check(api.run(Str("print(1)"), "harness") == WXL_OK && !g_vm.ran.empty() &&
                  g_vm.ran.back() == "harness: print(1)",
              "run hands the chunk and its name to the engine");

        // The client ends the process at its next frame when its script stack is not empty
        // (CGGameUI::Idle, error #134): every call the runtime makes into the state leaves it as
        // it found it.
        g_vm.stack.clear();
#if defined(_MSC_VER)
        {
            // As Dawnlight's ScriptVmReady handler did at a /reload: the chunk runs from a handler
            // the host guards, and calls GetBindingKey before the client has loaded the bindings.
            wxl_result ran = WXL_OK;
            const auto handler = [](void* context)
            { *static_cast<wxl_result*>(context) = Script().run(Str("fault: GetBindingKey('TURNLEFT')"), "harness"); };
            wxl::host::Fault fault;
            const bool       returned = wxl::host::CallGuarded(handler, &ran, &fault);
            Check(returned && ran == WXL_E_FAILED && g_vm.stack.empty() && g_vm.calls == 0,
                  "a chunk the client faults inside fails in the handler that ran it, and the calls and values it "
                  "left are dropped from the state");
            g_vm.stack.clear();
            g_vm.calls = 0;
        }
        Check(api.run(Str("print(2)"), "harness") == WXL_OK && g_vm.ran.back() == "harness: print(2)",
              "and the next chunk runs");
#endif
        Check(api.run(Str("leak: 1"), "harness") == WXL_OK && g_vm.stack.empty(),
              "a value a call into the client leaves on its stack is dropped");
        {
            namespace script = wxl::core::services::script;
            auto pushes  = [] { FakePushNil(&g_vm); FakePushNil(&g_vm); };
            auto pops    = [] { g_vm.stack.pop_back(); };
            auto stopped = [] { g_vm.calls += 1; FakePushNil(&g_vm); }; // a call a guard further in abandoned
            Check(script::Keep("harness pushes", pushes, nullptr) && g_vm.stack.empty(),
                  "Keep drops what its call left on the stack");
            FakePushNumber(&g_vm, 3.0);
            FakePushNumber(&g_vm, 4.0);
            Check(script::Keep("harness pops", pops, nullptr) && g_vm.stack.size() == 2 &&
                      g_vm.stack[1].type == WXL_VALUE_NIL,
                  "puts a nil back for a value its call took that it had not pushed");
            g_vm.stack.clear();
            Check(script::Keep("harness stopped", stopped, nullptr) && g_vm.stack.empty() && g_vm.calls == 0,
                  "and drops the calls its call left unfinished");
        }

        // Events: the client's table is installed when the context is announced; a declared
        // event is installed behind it.
        const size_t tables = g_vm.eventTables.size();
        Check(api.register_event("WXL_HARNESS_EVENT") == WXL_OK && g_vm.eventTables.size() == tables + 1 &&
                  g_vm.eventTables.back() == std::vector<std::string>{"PLAYER_LOGIN", "PLAYER_LOGOUT", "WXL_HARNESS_EVENT"},
              "an event declared is installed behind the client's table");
        Check(api.register_event("WXL_HARNESS_EVENT") == WXL_OK && g_vm.eventTables.size() == tables + 1,
              "declaring it again changes nothing");
        Check(api.register_event("WXL_HARNESS_LATE") == WXL_OK && g_vm.eventTables.size() == tables + 2 &&
                  g_vm.eventTables.back() == std::vector<std::string>{"PLAYER_LOGIN", "PLAYER_LOGOUT", "WXL_HARNESS_EVENT", "WXL_HARNESS_LATE"},
              "an event declared afterwards is appended, keeping every index");
        const wxl_value payload[2] = {wxl_value{WXL_VALUE_NUMBER, {7.0}}, wxl_value{WXL_VALUE_STRING, {0.0}}};
        wxl_value       text       = payload[1];
        text.as.string             = Str("x");
        const wxl_value args[2]    = {payload[0], text};
        Check(api.fire_event("WXL_HARNESS_LATE", args, 2) == WXL_OK && g_vm.signals.size() == 1 &&
                  g_vm.signals[0].index == 3 && g_vm.signals[0].args.size() == 3 &&
                  g_vm.signals[0].args[0].string == "WXL_HARNESS_LATE" && g_vm.signals[0].args[1].number == 7.0 &&
                  g_vm.signals[0].args[2].string == "x" && g_vm.stack.empty(),
              "fire_event signals the event at its index with its name then the values pushed, as the client does, and "
              "leaves the stack as it found it");
        Check(api.fire_event("WXL_HARNESS_NOT_DECLARED", nullptr, 0) == WXL_E_NOT_FOUND,
              "firing an event nobody declared is not found");

#if defined(_MSC_VER)
        Check(api.register_function("WxlHarnessCrash", &Crashing, nullptr) == WXL_OK, "a faulting function registers like any other");
        results = CallGlobal("WxlHarnessCrash", {}, &found);
        Check(found && results.empty() && g_vm.errors.back().find("WxlHarnessCrash faulted (access violation") != std::string::npos,
              "a script function that faults answers the caller with an error naming the fault, and the harness is still here");
        Check(g_vm.closures.count("WxlHarnessCrash") == 0, "and the global is taken away");
        Check(g_vm.closures.count("WxlHarnessGreet") == 1, "the others stay");
#endif

        // Closing: announced with the context Ready named, then nothing is accepted.
        g_received.clear();
        wxl::seam::FrameScript::Destroy::Before();
        const Received* closing = Only(WXL_EV_ScriptVmClosing);
        Check(closing != nullptr && closing->As<wxl_script_vm_args>().context == WXL_SCRIPT_CONTEXT_GLUE,
              "FrameScript.Destroy raises ScriptVmClosing once, naming the context Ready did");
        g_vm.live = false;
        Check(api.register_function("WxlHarnessLate", &Greet, nullptr) == WXL_E_FAILED &&
                  api.fire_event("WXL_HARNESS_LATE", nullptr, 0) == WXL_E_FAILED && api.live() == WXL_FALSE,
              "after ScriptVmClosing nothing is accepted until the next context, and live() says so");
        {
            // A closure the dead context left behind, called anyway: an error, not the mod.
            g_greetCalls      = 0;
            FakeClosure stale = g_vm.closures["WxlHarnessGreet"];
            g_vm.closures.clear();
            g_vm.runningSlot = stale.slot;
            const int32_t pushed = stale.fn(&g_vm);
            Check(pushed == 0 && g_greetCalls == 0 && g_vm.errors.back().find("no longer provides") != std::string::npos,
                  "a closure outliving its registration answers an error instead of calling the mod");
        }
        g_received.clear();
        wxl::seam::FrameScript::Destroy::Before();
        Check(g_received.empty(), "a Destroy with no context announced raises nothing");

        // The game context: InitializeFrameXML sets m_loggingIn before it creates the context.
        g_vm.live     = true;
        g_uiLoggingIn = 1;
        registered    = WXL_E_FAILED;
        g_received.clear();
        wxl::seam::FrameScript::Initialize::After(1, 1);
        wxl::seam::FrameScript::CreateEvents::After(clientNames, 2);
        ready = Only(WXL_EV_ScriptVmReady);
        Check(ready != nullptr && ready->As<wxl_script_vm_args>().context == WXL_SCRIPT_CONTEXT_GAME,
              "the context created while CGGameUI is logging in is the game's");
        Check(registered == WXL_OK && g_vm.closures.count("WxlHarnessGreet") == 1,
              "the handler registered again on the new context, and the name was free: the old registration died with its context");
        Check(api.fire_event("WXL_HARNESS_LATE", nullptr, 0) == WXL_E_NOT_FOUND,
              "the events declared on the old context are gone too");
        results = CallGlobal("WxlHarnessGreet", {Number(2.0), String("ann")}, &found);
        Check(found && results.size() == 4 && results[0].string == "hi ann", "and the new registration works");

        // ReloadUI: Flush destroys and recreates with m_initialized still set.
        wxl::seam::FrameScript::Destroy::Before();
        g_vm.closures.clear();
        g_uiLoggingIn   = 0;
        g_uiInitialized = 1;
        g_received.clear();
        wxl::seam::FrameScript::Initialize::After(1, 0);
        wxl::seam::FrameScript::CreateEvents::After(clientNames, 2);
        ready = Only(WXL_EV_ScriptVmReady);
        Check(ready != nullptr && ready->As<wxl_script_vm_args>().context == WXL_SCRIPT_CONTEXT_GAME,
              "a context recreated while the game interface is initialised is the game's");

        // Back to the glue: ShutdownGame cleared both flags before its flush, which makes a
        // placeholder; the glue's own flush makes the context it loads into.
        wxl::seam::FrameScript::Destroy::Before();
        g_vm.closures.clear();
        g_uiInitialized = 0;
        wxl::seam::FrameScript::Initialize::After(1, 0);
        g_received.clear();
        wxl::seam::FrameScript::Destroy::Before();
        Check(g_received.empty(), "the placeholder between the game and the glue raises nothing");
        wxl::seam::FrameScript::Initialize::After(1, 0);
        wxl::seam::FrameScript::CreateEvents::After(clientNames, 2);
        ready = Only(WXL_EV_ScriptVmReady);
        Check(ready != nullptr && ready->As<wxl_script_vm_args>().context == WXL_SCRIPT_CONTEXT_GLUE,
              "a context recreated once the game interface is down is the glue's");

        wxl::seam::FrameScript::Destroy::Before();
        g_vm.live = false;
        g_host->Events().Unsubscribe(onReady);
    }

    // --- NetClient: the gated seams, and what a handler's answer does to the call -------------

    /// What the message handler below saw, and whether it is to stop what it is shown.
    struct MessageWatch
    {
        int      calls  = 0;
        uint32_t opcode = 0;
        uint32_t size   = 0;
        uint8_t  first  = 0;
        bool     stop   = false;
    };

    void WXL_CALL OnMessage(void* user, const void* args)
    {
        MessageWatch&           watch   = *static_cast<MessageWatch*>(user);
        const wxl_message_args& message = *static_cast<const wxl_message_args*>(args);
        ++watch.calls;
        watch.opcode = message.opcode;
        watch.size   = message.size;
        watch.first  = message.size ? static_cast<const uint8_t*>(message.data)[0] : uint8_t(0);
        if (watch.stop && message.suppressed) *message.suppressed = WXL_TRUE;
    }

    void CheckNetClient()
    {
        MessageWatch incoming;
        MessageWatch outgoing;
        const wxl_subscription in =
            g_host->Events().Subscribe(WXL_EV_MessageReceived, &OnMessage, &incoming, 0);
        const wxl_subscription out =
            g_host->Events().Subscribe(WXL_EV_MessageSent, &OnMessage, &outgoing, 0);

        // An incoming message: a uint16 opcode, then three bytes of body.
        const uint8_t received[] = { 0x34, 0x12, 0xAA, 0xBB, 0xCC };
        FakeStore     store{ received, sizeof received, 0 };

        void*    self    = reinterpret_cast<void*>(0x1000);
        void*    message = &store;
        uint32_t when    = 7;
        int32_t  unused  = 0;
        int32_t  answer  = -1;

        bool taken = wxl::seam::NetClient::ProcessMessage::Gate(&answer, &self, &when, &message,
                                                                &unused);
        Check(!taken, "a message nobody stops lets the client's own dispatch run");
        Check(incoming.calls == 1 && incoming.opcode == 0x1234u,
              "the opcode is the uint16 the server wrote");
        Check(incoming.size == 3u && incoming.first == 0xAA,
              "and the body is what follows the opcode");
        Check(store.read == 0u, "reading the message left the store's cursor where it was");

        incoming.stop = true;
        taken = wxl::seam::NetClient::ProcessMessage::Gate(&answer, &self, &when, &message,
                                                           &unused);
        Check(taken && answer == 0,
              "a handler that suppresses it takes the call, and the client answers 0");

        // An outgoing message: a uint32 opcode, then two bytes of body.
        const uint8_t sent[] = { 0x78, 0x56, 0x00, 0x00, 0x11, 0x22 };
        FakeStore     outStore{ sent, sizeof sent, 0 };
        message = &outStore;

        Check(!wxl::seam::NetClient::Send::Gate(&self, &message), "a message nobody stops is sent");
        Check(outgoing.calls == 1 && outgoing.opcode == 0x5678u,
              "the opcode is the uint32 the client wrote");
        Check(outgoing.size == 2u && outgoing.first == 0x11, "and the body is what follows it");

        outgoing.stop = true;
        Check(wxl::seam::NetClient::Send::Gate(&self, &message),
              "a handler that suppresses it stops the send");

        // A store that has been reset carries a cursor past its end: there is no message in it.
        outgoing.stop = false;
        FakeStore reset{ sent, 0, 0xFFFFFFFFu };
        message = &reset;
        const int before = outgoing.calls;
        Check(!wxl::seam::NetClient::Send::Gate(&self, &message),
              "a reset store is not a message, and the call goes through untouched");

        FakeStore stub{ sent, 2, 0 };
        message = &stub;
        Check(!wxl::seam::NetClient::Send::Gate(&self, &message),
              "neither is a store too short to name an opcode");
        Check(outgoing.calls == before, "and neither of those raised anything");

        g_host->Events().Unsubscribe(in);
        g_host->Events().Unsubscribe(out);
    }

    // --- wxl.net: claimed numbers through the ProcessMessage gate, sends behind the gate -------

    /// What one handler of a claimed number was handed.
    struct NetWatch
    {
        int                  calls  = 0;
        uint32_t             opcode = 0;
        std::vector<uint8_t> body;
    };

    void WXL_CALL OnNetMessage(void* user, uint32_t opcode, const void* data, uint32_t size)
    {
        NetWatch& watch = *static_cast<NetWatch*>(user);
        ++watch.calls;
        watch.opcode = opcode;
        watch.body.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    }

    /// A message as a 3.3.5a world server puts it on the wire: the size of what follows (the
    /// opcode included) in 2 bytes big-endian, or 3 with the high bit of the first set past
    /// 0x7FFF, then the opcode as a u16 little-endian, then the body.
    std::vector<uint8_t> ServerFrame(uint16_t opcode, const std::vector<uint8_t>& body)
    {
        const uint32_t       size = static_cast<uint32_t>(body.size()) + 2;
        std::vector<uint8_t> frame;
        if (size > 0x7FFF) frame.push_back(static_cast<uint8_t>(0x80 | (size >> 16)));
        frame.push_back(static_cast<uint8_t>(size >> 8));
        frame.push_back(static_cast<uint8_t>(size));
        frame.push_back(static_cast<uint8_t>(opcode));
        frame.push_back(static_cast<uint8_t>(opcode >> 8));
        frame.insert(frame.end(), body.begin(), body.end());
        return frame;
    }

    /// Reads a frame as WowConnection::DoMessageReads does (the header's size bytes, the high bit
    /// for 3) and runs what NetClient::HandleData would: a CDataStore over what follows the
    /// header, through the ProcessMessage gate. True when the gate took the call.
    bool ReceiveFrame(const std::vector<uint8_t>& frame, int32_t* answer)
    {
        const bool     large  = (frame[0] & 0x80) != 0;
        const uint32_t header = large ? 3u : 2u;
        const uint32_t size   = large ? ((frame[0] & 0x7Fu) << 16 | frame[1] << 8 | frame[2]) : (frame[0] << 8 | frame[1]);
        if (header + size != frame.size()) return false;
        FakeStore store{ frame.data() + header, size, 0 };
        void*     self    = reinterpret_cast<void*>(0x1000);
        void*     message = &store;
        uint32_t  when    = 1;
        int32_t   unused  = 0;
        return wxl::seam::NetClient::ProcessMessage::Gate(answer, &self, &when, &message, &unused);
    }

    std::vector<uint8_t> Bytes(std::initializer_list<uint8_t> bytes) { return std::vector<uint8_t>(bytes); }

    void CheckNetOpcodes()
    {
        using namespace wxl::core::services;
        constexpr wxl::host::ModHost::ModToken kMod = 41, kOther = 42;
        const wxl_net_api& api = NetApi();
        net::Clear();
        NetSent().clear();
        const wxl::host::ModHost::ModToken previous = g_host->SwapCurrentMod(kMod);

        NetWatch updates, syncs;
        Check(api.claim_opcode(0x521) == WXL_OK && api.claim_opcode(0x524) == WXL_OK &&
                  api.claim_opcode(0x529) == WXL_OK && api.claim_opcode(0x52A) == WXL_OK &&
                  api.claim_opcode(0x524) == WXL_OK,
              "a mod claims numbers above 0x520, and its own again");
        Check(api.set_handler(0x524, &OnNetMessage, &updates) == WXL_OK &&
                  api.set_handler(0x529, &OnNetMessage, &syncs) == WXL_OK && api.set_compressed(0x529, 0x52A) == WXL_OK,
              "it sets handlers, and pairs a compressed number with a plain one");
        Check(api.set_handler(0x600, &OnNetMessage, &updates) == WXL_E_INVALID && api.set_compressed(0x529, 0x600) == WXL_E_INVALID,
              "a handler or a pairing of a number it does not hold is refused");
        Check(api.claim_opcode(0x10000) == WXL_E_INVALID && api.claim_opcode(0x100) == WXL_E_INVALID &&
                  api.claim_opcode(0x520) == WXL_E_INVALID,
              "a number past 0xFFFF, and the client's own without stock, are refused");
        Check(api.reserved_opcode_limit() == 0x520 && api.max_send() == 10000, "the limit and the largest body");

        g_host->SwapCurrentMod(kOther);
        Check(api.claim_opcode(0x524) == WXL_E_INVALID && api.release_opcode(0x524) == WXL_E_NOT_FOUND &&
                  api.send(0x521, "x", 1) == WXL_E_INVALID,
              "another mod can neither claim, release nor send a held number");
        g_host->SwapCurrentMod(kMod);

        const uint8_t seq[4] = {1, 0, 0, 0};
        Check(api.state() == WXL_NET_OFFLINE && api.connection() == 0 && api.send(0x521, seq, 4) == WXL_E_FAILED,
              "offline: no connection, nothing sent");
        wxl::seam::NetClient::HandleConnect::After(1, nullptr);
        const uint32_t first = api.connection();
        Check(api.state() == WXL_NET_CONNECTED && first != 0 && api.send(0x521, seq, 4) == WXL_E_FAILED,
              "connected but not accepted: nothing sent");

        // The client's own SMSG_AUTH_RESPONSE (0x1EE), AUTH_OK (0x0C): the session is accepted.
        int32_t answer = -1;
        Check(!ReceiveFrame(ServerFrame(0x1EE, Bytes({0x0C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2})), &answer) &&
                  api.state() == WXL_NET_AUTHENTICATED,
              "the client's own answer goes to its dispatch, and accepts the session");
        Check(api.send(0x521, seq, 4) == WXL_E_FAILED && api.heard() == WXL_FALSE,
              "the send gate: nothing of the mod's goes out before its server speaks");

        MessageWatch observed;
        const wxl_subscription watch = g_host->Events().Subscribe(WXL_EV_MessageReceived, &OnMessage, &observed, 0);
        // SMSG 0x524: a packed guid, a u32, a float.
        const std::vector<uint8_t> update = Bytes({0x03, 0x05, 0x01, 0x2A, 0, 0, 0, 0, 0, 0x20, 0x40});
        answer                           = -1;
        Check(ReceiveFrame(ServerFrame(0x524, update), &answer) && answer == 0,
              "a claimed number past the client's 0x51F handlers stops at the gate");
        Check(observed.calls == 1 && observed.opcode == 0x524, "MessageReceived saw it first");
        Check(updates.calls == 1 && updates.opcode == 0x524 && updates.body == update,
              "its handler gets the body, byte for byte");
        Check(api.heard() == WXL_TRUE, "and the mod's gate opens");
        g_host->SwapCurrentMod(kOther);
        Check(api.heard() == WXL_FALSE, "for that mod alone");
        g_host->SwapCurrentMod(kMod);

        Check(api.send(0x521, seq, 4) == WXL_OK && NetSent().size() == 1 &&
                  NetSent().back() == Bytes({0x21, 0x05, 0, 0, 1, 0, 0, 0}),
              "the mod sends: the client is handed the opcode as a u32 and the body");
        Check(api.send(0x521, nullptr, 0) == WXL_OK && NetSent().back() == Bytes({0x21, 0x05, 0, 0}),
              "an empty body is the opcode alone");
        std::vector<uint8_t> big(10001, 0x55);
        Check(api.send(0x521, big.data(), 10001) == WXL_E_INVALID && api.send(0x521, big.data(), 10000) == WXL_OK,
              "a body is at most 10000 bytes");
        // Counted by the second: a run that crosses into the next one counts again from there.
        int        sent = 3;
        wxl_result last = WXL_OK;
        while (sent < 200 && (last = api.send(0x521, seq, 4)) == WXL_OK) ++sent;
        Check(last == WXL_E_BUSY && sent >= 64 && sent <= 128, "past 64 messages of a number in a second, busy");

        // SMSG 0x52A: a u32 size and a zlib stream, inflated and handed to 0x529's handler.
        std::vector<uint8_t> payload(40000);
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i % 7 + i / 1000);
        std::vector<uint8_t> stream;
        Check(wxl::core::zlib::Deflate(payload.data(), payload.size(), stream) && stream.size() < payload.size(),
              "the runtime's deflate compresses");
        std::vector<uint8_t> packed(4);
        const uint32_t       length = static_cast<uint32_t>(payload.size());
        std::memcpy(packed.data(), &length, 4);
        packed.insert(packed.end(), stream.begin(), stream.end());
        Check(ReceiveFrame(ServerFrame(0x52A, packed), &answer) && syncs.calls == 1 && syncs.opcode == 0x529 &&
                  syncs.body == payload,
              "a compressed message is inflated and handed to the plain number's handler as one of it");
        // The same, raw, past 0x7FFF: the server's 3-byte header.
        Check(ReceiveFrame(ServerFrame(0x529, payload), &answer) && syncs.calls == 2 && syncs.body == payload,
              "a message past 0x7FFF bytes comes with the 3-byte header and arrives whole");
        std::vector<uint8_t> lying = packed;
        const uint32_t       more  = length + 1;
        std::memcpy(lying.data(), &more, 4);
        Check(ReceiveFrame(ServerFrame(0x52A, lying), &answer) && syncs.calls == 2,
              "a stream that does not inflate to its size is dropped");
        std::vector<uint8_t> huge(4 + 8);
        const uint32_t       tooBig = WXL_NET_MAX_INFLATED + 1;
        std::memcpy(huge.data(), &tooBig, 4);
        Check(ReceiveFrame(ServerFrame(0x52A, huge), &answer) && syncs.calls == 2, "and so is one past 8 MB");

        uint64_t got = 0;
        std::vector<uint8_t> roundOut(payload.size());
        Check(api.deflate(payload.data(), length, nullptr, 0, &got) == WXL_E_INVALID && got > 0 && got < payload.size(),
              "deflate with no room says the room it needs");
        std::vector<uint8_t> zipped(static_cast<size_t>(got));
        Check(api.deflate(payload.data(), length, zipped.data(), got, &got) == WXL_OK &&
                  api.inflate(zipped.data(), static_cast<uint32_t>(got), roundOut.data(), roundOut.size(), &got) == WXL_OK &&
                  got == payload.size() && roundOut == payload,
              "deflate and inflate go round");
        Check(api.inflate("junk", 4, roundOut.data(), roundOut.size(), &got) == WXL_E_INVALID && got == 0,
              "inflate refuses what is not zlib");

        // A number nobody claimed past 0x51F is left to the client, which drops it.
        Check(!ReceiveFrame(ServerFrame(0x600, Bytes({1})), &answer), "an unclaimed number goes to the client's dispatch");
        // A MessageReceived handler that suppresses one stops it before the mod's handler.
        observed.stop = true;
        Check(ReceiveFrame(ServerFrame(0x524, update), &answer) && updates.calls == 1,
              "a suppressing observer stops the message before the mod's handler");
        observed.stop = false;
        g_host->Events().Unsubscribe(watch);

        wxl::seam::NetClient::HandleDisconnect::Before(nullptr);
        Check(api.state() == WXL_NET_OFFLINE && api.connection() == 0 && api.heard() == WXL_FALSE &&
                  api.send(0x521, seq, 4) == WXL_E_FAILED,
              "a disconnect closes every gate");
        wxl::seam::NetClient::HandleConnect::After(1, nullptr);
        ReceiveFrame(ServerFrame(0x1EE, Bytes({0x0C})), &answer);
        Check(api.connection() != first && api.state() == WXL_NET_AUTHENTICATED && api.send(0x521, seq, 4) == WXL_E_FAILED,
              "a new connection is a new number, and the gate stays closed until the server speaks again");

        net::ForgetMod(kMod);
        g_host->SwapCurrentMod(kOther);
        Check(api.claim_opcode(0x524) == WXL_OK && !ReceiveFrame(ServerFrame(0x529, Bytes({1})), &answer),
              "a mod that goes gives its numbers back");
        g_host->SwapCurrentMod(previous);
        net::Clear();
    }

    // --- Input: a gate that can answer a window message in the client's place -----------------

    bool g_swallow = false;
    int  g_messages = 0;

    void WXL_CALL OnInput(void*, const void* args)
    {
        const wxl_input_args& input = *static_cast<const wxl_input_args*>(args);
        ++g_messages;
        if (g_swallow && input.handled) *input.handled = WXL_TRUE;
    }

    void CheckInput()
    {
        const wxl_subscription sub =
            g_host->Events().Subscribe(WXL_EV_Input, &OnInput, nullptr, 0);

        void*     window  = reinterpret_cast<void*>(0x2000);
        uint32_t  message = 0x0100; // WM_KEYDOWN
        uintptr_t wparam  = 0x41;
        intptr_t  lparam  = 1;
        intptr_t  answer  = -1;

        g_swallow = false;
        Check(!wxl::seam::Input::WindowProc::Gate(&answer, &window, &message, &wparam, &lparam),
              "a message nobody claims reaches the client's own window procedure");
        Check(g_messages == 1, "and the handler saw it once");

        g_swallow = true;
        Check(wxl::seam::Input::WindowProc::Gate(&answer, &window, &message, &wparam, &lparam),
              "a handler that writes handled takes the call away from the client");
        Check(answer == 0, "and the window procedure answers 0, as one that handled a message does");

        g_host->Events().Unsubscribe(sub);
    }


    // --- wxl.camera: the view a mod sets, over the client's, around the camera's update -------

    uint32_t g_cameraUpdates = 0;
    void WXL_CALL OnCameraUpdate(void*, const void*) { ++g_cameraUpdates; }

    void CheckCamera()
    {
        namespace cam = wxl::core::services::camera;
        const wxl_camera_api& api = wxl::core::services::CameraApi();

        wxl_vec3 point{};
        double   fov = 0.0;
        g_worldCamera = nullptr;
        Check(api.position(&point) == WXL_E_UNSUPPORTED && api.fov(&fov) == WXL_E_UNSUPPORTED,
              "out of the world the camera reads as unsupported");

        g_camera                = FakeCamera{};
        g_camera.view.position  = wxl_vec3{1.0f, 2.0f, 3.0f};
        g_camera.view.forward   = wxl_vec3{1.0f, 0.0f, 0.0f};
        g_camera.view.up        = wxl_vec3{0.0f, 0.0f, 1.0f};
        g_camera.view.fov       = 1.6f; // with an effect's offset of 0.1
        g_camera.view.fov_base  = 1.5f;
        g_camera.view.pivot     = wxl_vec3{5.0f, 5.0f, 1.8f};
        g_camera.view.followed  = 0;
        g_worldCamera           = &g_camera;
        Check(api.position(&point) == WXL_OK && point.z == 3.0f && api.forward(&point) == WXL_OK && point.x == 1.0f &&
                  api.up(&point) == WXL_OK && point.z == 1.0f && api.fov(&fov) == WXL_OK && std::fabs(fov - 1.6) < 1e-6,
              "the camera reads where it is, where it looks and what it draws with");
        Check(api.pivot(&point) == WXL_E_NOT_FOUND, "a camera that follows nothing has no pivot");
        g_camera.view.followed = 0x42;
        Check(api.pivot(&point) == WXL_OK && point.z == 1.8f, "the pivot is the followed object's at its height");
        Check(api.cinematic() == WXL_FALSE, "no cinematic");

        const wxl_subscription sub = g_host->Events().Subscribe(WXL_EV_CameraUpdate, &OnCameraUpdate, nullptr, 0);
        wxl::core::frame::Reset();
        wxl_frame_args frame{};
        wxl::core::frame::Begin(frame);

        g_cameraUpdates = 0;
        wxl::seam::Camera::UpdateCallback::Before(nullptr, &g_camera);
        wxl::seam::Camera::UpdateCallback::After(1, nullptr, &g_camera);
        Check(g_cameraUpdates == 1 && g_camera.writes == 0, "an update raises CameraUpdate and writes nothing with no view");

        FakeCamera other;
        wxl::seam::Camera::UpdateCallback::After(1, nullptr, &other);
        Check(g_cameraUpdates == 1, "another camera's update is not the world camera's");

        Check(api.set_view(wxl_vec3{0, 0, 0}, wxl_vec3{0, 0, 0}, 0.0) == WXL_E_INVALID &&
                  api.set_view(wxl_vec3{0, 0, 0}, wxl_vec3{1, 0, 0}, 4.0) == WXL_E_INVALID,
              "a view with no direction, or a field of view past pi, is invalid");
        Check(api.release_view() == WXL_E_NOT_FOUND, "releasing a view one does not hold is not found");

        Check(api.set_view(wxl_vec3{7, 8, 9}, wxl_vec3{0, 1, 0}, 1.2) == WXL_OK, "a view is set");
        wxl::seam::Camera::UpdateCallback::Before(nullptr, &g_camera);
        wxl::seam::Camera::UpdateCallback::After(1, nullptr, &g_camera);
        Check(g_camera.writes == 1 && g_camera.view.position.x == 7.0f && g_camera.view.forward.y == 1.0f,
              "the view is written over the client's at the end of the update");
        Check(std::fabs(g_camera.view.fov - 1.2f) < 1e-6f && std::fabs(g_camera.view.fov_base - 1.1f) < 1e-6f,
              "the field of view drawn is the view's, the effects' offset kept");
        wxl::seam::Camera::UpdateCallback::Before(nullptr, &g_camera);
        Check(std::fabs(g_camera.view.fov_base - 1.5f) < 1e-6f, "the client's own field of view is back before its update");

        g_camera.view.cinematic = 1;
        const uint32_t writes   = g_camera.writes;
        wxl::seam::Camera::UpdateCallback::After(1, nullptr, &g_camera);
        Check(g_camera.writes == writes, "a cinematic keeps the view");
        g_camera.view.cinematic = 0;

        wxl::seam::Camera::UpdateCallback::After(1, nullptr, &g_camera);
        Check(api.release_view() == WXL_OK && std::fabs(g_camera.view.fov_base - 1.5f) < 1e-6f,
              "releasing the view puts the client's field of view back");
        const uint32_t released = g_camera.writes;
        wxl::seam::Camera::UpdateCallback::After(1, nullptr, &g_camera);
        Check(g_camera.writes == released, "and the client has its camera again");

        api.set_view(wxl_vec3{1, 1, 1}, wxl_vec3{1, 0, 0}, 0.0);
        cam::ForgetMod(0);
        wxl::seam::Camera::UpdateCallback::After(1, nullptr, &g_camera);
        Check(g_camera.writes == released, "a mod's view goes with it");

        wxl_result fromThread = WXL_OK;
        std::thread([&] { fromThread = api.set_view(wxl_vec3{1, 1, 1}, wxl_vec3{1, 0, 0}, 0.0); }).join();
        Check(fromThread == WXL_E_INVALID, "off the main thread a view is refused");

        // A roll: the rows turned around the forward, read back, and 0 the unrolled rows exactly.
        {
            namespace facing = wxl::core::camera_facing;
            wxl_vec3 level[3], none[3], rolled[3];
            facing::FaceDirection(wxl_vec3{0.6f, -0.3f, 0.2f}, level);
            facing::Facing(wxl_vec3{0.6f, -0.3f, 0.2f}, 0.0, none);
            Check(std::memcmp(level, none, sizeof level) == 0, "a roll of 0 leaves SetFacing's rows bit for bit");
            facing::Facing(wxl_vec3{1.0f, 0.0f, 0.0f}, 0.3, rolled);
            Check(rolled[2].y < 0.0f && std::fabs(rolled[2].z - std::cos(0.3f)) < 1e-6f &&
                      std::fabs(facing::Dot(rolled[0], rolled[2])) < 1e-6f && std::fabs(facing::Dot(rolled[1], rolled[2])) < 1e-6f,
                  "a positive roll leans the camera's up to its right (east, looking north), the rows kept square");
            Check(std::fabs(facing::RollOf(wxl_vec3{1.0f, 0.0f, 0.0f}, rolled[2]) - 0.3) < 1e-6 &&
                      std::fabs(facing::RollOf(level[0], level[2])) < 1e-6,
                  "the roll is read back from the rows, 0 for level ones");
        }
        Check(api.set_view_rolled(wxl_vec3{1, 1, 1}, wxl_vec3{1, 0, 0}, 0.0, std::nan("")) == WXL_E_INVALID,
              "a roll that is not a number is invalid");
        Check(api.set_view_rolled(wxl_vec3{7, 8, 9}, wxl_vec3{0, 1, 0.2f}, 0.0, -0.4) == WXL_OK, "a rolled view is set");
        wxl::seam::Camera::UpdateCallback::Before(nullptr, &g_camera);
        wxl::seam::Camera::UpdateCallback::After(1, nullptr, &g_camera);
        double roll = 0.0;
        Check(api.roll(&roll) == WXL_OK && std::fabs(roll + 0.4) < 1e-5, "it is written rolled, and Roll reads it back");
        Check(api.set_view_rolled(wxl_vec3{7, 8, 9}, wxl_vec3{0, 1, 0.2f}, 0.0, 0.4 + 6.283185307179586) == WXL_OK,
              "a roll past a turn is taken modulo a turn");
        wxl::seam::Camera::UpdateCallback::After(1, nullptr, &g_camera);
        Check(api.roll(&roll) == WXL_OK && std::fabs(roll - 0.4) < 1e-5, "and drawn as its remainder");
        Check(api.set_view(wxl_vec3{7, 8, 9}, wxl_vec3{0, 1, 0.2f}, 0.0) == WXL_OK, "SetView after it");
        wxl::seam::Camera::UpdateCallback::After(1, nullptr, &g_camera);
        Check(api.roll(&roll) == WXL_OK && std::fabs(roll) < 1e-6, "draws level again");
        Check(api.release_view() == WXL_OK, "and the view goes");

        wxl::core::frame::End(frame);
        g_host->Events().Unsubscribe(sub);
        g_worldCamera = nullptr;
    }

    // --- wxl.world: the client's line test ----------------------------------------------------

    // --- wxl.input's block of the game's mouse look ---------------------------------------------

    void CheckMouseLookBlock()
    {
        namespace in = wxl::core::services::input;
        const wxl_input_api& api = wxl::core::services::InputApi();
        void*    control = &g_inputControl;
        uint32_t bit     = 0x2;
        uint32_t time    = 1000;
        int32_t  result  = -1;

        Check(!wxl::seam::InputControl::SetControlBit::Gate(&result, &control, &bit, &time),
              "with no block, the left button's look goes to the game");
        g_lookStops = 0;
        Check(api.block_mouse_look(WXL_TRUE) == WXL_OK && api.mouse_look_blocked() == WXL_TRUE && g_lookStops == 1,
              "a block ends a look the game is in");
        api.block_mouse_look(WXL_TRUE);
        Check(g_lookStops == 1, "blocking twice is one block");

        // A click of the left button: the press noted, the release within 0.8 s selects.
        g_inputControl.pending = 1; // INPUT_RELEASE_SELECT, as CameraOrSelectOrMoveStart notes it
        g_defaultActions.clear();
        bit = 0x2, time = 1000, result = -1;
        Check(wxl::seam::InputControl::SetControlBit::Gate(&result, &control, &bit, &time) && result == 0,
              "the press sets no look bit: the gate takes the call, answering 0");
        int32_t keep = 0;
        bit = 0x2, time = 1200, result = -1;
        Check(wxl::seam::InputControl::UnsetControlBit::Gate(&result, &control, &bit, &time, &keep) && result == 0,
              "the release is the gate's too");
        Check(g_defaultActions.size() == 1 && g_defaultActions[0] == 1 && g_inputControl.pending == 0,
              "a short click does the left button's default action, once");

        // A right button held a second: nothing.
        g_inputControl.pending = 2; // INPUT_RELEASE_ACTION
        g_defaultActions.clear();
        bit = 0x1, time = 5000;
        wxl::seam::InputControl::SetControlBit::Gate(&result, &control, &bit, &time);
        bit = 0x1, time = 6000;
        wxl::seam::InputControl::UnsetControlBit::Gate(&result, &control, &bit, &time, &keep);
        Check(g_defaultActions.empty() && g_inputControl.pending == 0, "a long hold acts on nothing, as in the game");

        // A right click acts.
        g_inputControl.pending = 2;
        bit = 0x1, time = 7000;
        wxl::seam::InputControl::SetControlBit::Gate(&result, &control, &bit, &time);
        bit = 0x1, time = 7100;
        wxl::seam::InputControl::UnsetControlBit::Gate(&result, &control, &bit, &time, &keep);
        Check(g_defaultActions.size() == 1 && g_defaultActions[0] == 4, "a short right click does the right button's action");

        // Both buttons: no click, as the game makes none.
        g_defaultActions.clear();
        g_inputControl.pending = 1;
        bit = 0x2, time = 8000;
        wxl::seam::InputControl::SetControlBit::Gate(&result, &control, &bit, &time);
        bit = 0x1;
        wxl::seam::InputControl::SetControlBit::Gate(&result, &control, &bit, &time);
        bit = 0x3, time = 8100;
        wxl::seam::InputControl::UnsetControlBit::Gate(&result, &control, &bit, &time, &keep);
        Check(g_defaultActions.empty(), "pressing both buttons makes no click");

        // Other bits go on, the look bits kept out.
        bit = 0x12, time = 9000, result = -1;
        Check(!wxl::seam::InputControl::SetControlBit::Gate(&result, &control, &bit, &time) && bit == 0x10,
              "a call with a movement bit and a look bit goes on with the movement bit alone");
        bit = 0x12, time = 9100;
        Check(!wxl::seam::InputControl::UnsetControlBit::Gate(&result, &control, &bit, &time, &keep) && bit == 0x10,
              "and so does its release");
        bit = 0x10, result = -1;
        Check(!wxl::seam::InputControl::SetControlBit::Gate(&result, &control, &bit, &time), "a movement bit alone is the game's");

        // A movement key (MOVEBACKWARD, 0x20) pressed and let go around a held button, in either
        // order: the game gets both edges, and the button stays the gate's.
        const auto set = [&](uint32_t b, uint32_t t) {
            bit = b, time = t, result = -1;
            const bool taken = wxl::seam::InputControl::SetControlBit::Gate(&result, &control, &bit, &time);
            return std::make_pair(taken, bit);
        };
        const auto unset = [&](uint32_t b, uint32_t t) {
            bit = b, time = t, result = -1;
            const bool taken = wxl::seam::InputControl::UnsetControlBit::Gate(&result, &control, &bit, &time, &keep);
            return std::make_pair(taken, bit);
        };
        const auto passes = [](std::pair<bool, uint32_t> call, uint32_t bits) { return !call.first && call.second == bits; };
        Check(passes(set(0x20, 10000), 0x20) && set(0x2, 10100).first && passes(unset(0x20, 10200), 0x20) &&
                  unset(0x2, 10300).first,
              "a movement key let go while a button is held reaches the game");
        Check(set(0x1, 11000).first && passes(set(0x20, 11100), 0x20) && unset(0x1, 11200).first &&
                  passes(unset(0x20, 11300), 0x20),
              "a movement key pressed while a button is held, let go after it, reaches the game both times");
        Check(passes(set(0x20, 12000), 0x20) && passes(set(0x20, 12050), 0x20) && passes(unset(0x20, 12100), 0x20) &&
                  passes(unset(0x20, 12150), 0x20),
              "a movement key pressed again, or let go twice, is the game's each time");
        Check(set(0x2, 13000).first && passes(unset(0xFFFFFFFFu, 13100), 0xFFFFFFFDu),
              "the game's reset lets every bit go but the button the gate holds, which it lets go itself");
        Check(passes(unset(0x2, 13200), 0x2), "and a later release of that button is the game's");

        in::ForgetMod(0);
        Check(api.mouse_look_blocked() == WXL_FALSE, "a mod's block goes with it");
        bit = 0x2;
        Check(!wxl::seam::InputControl::SetControlBit::Gate(&result, &control, &bit, &time), "and the game looks again");
        wxl_result fromThread = WXL_OK;
        std::thread([&] { fromThread = api.block_mouse_look(WXL_TRUE); }).join();
        Check(fromThread == WXL_E_INVALID && api.mouse_look_blocked() == WXL_FALSE, "off the main thread a block is refused");
    }

    // --- wxl.pose: a head turned over the animation ----------------------------------------------

    void CheckPose()
    {
        const wxl_pose_api& api = wxl::core::services::PoseApi();
        FakeObject          wolf;
        wolf.guid   = 0x781;
        wolf.typeId = WXL_OBJECT_UNIT;
        FakeObject stone;
        stone.guid   = 0x782;
        stone.typeId = WXL_OBJECT_ITEM;
        wxl::seam::ObjectMgr::InitObject::After(&wolf, nullptr, 100u);
        wxl::seam::ObjectMgr::InitObject::After(&stone, nullptr, 100u);
        const wxl_handle unit = wxl::core::objects::HandleOf(&wolf);
        const wxl_handle item = wxl::core::objects::HandleOf(&stone);

        Check(api.head(item, 0, 0, 0) == WXL_E_INVALID && api.head(0xDEAD, 0, 0, 0) == WXL_E_NOT_FOUND &&
                  api.head(unit, std::numeric_limits<double>::quiet_NaN(), 0, 0) == WXL_E_INVALID,
              "a head is a unit's, its angles numbers");
        g_headBone = FakeHeadBone{};
        Check(api.head(unit, 0.5, 0.25, 0.0) == WXL_OK, "a unit's head is turned");
        wxl::seam::Unit::PreAnimate::After(&wolf, nullptr);
        // The turn about z by 0.5 after a nod by -0.25 about y: row a's y is sin(0.5) * cos(0.25).
        Check(g_headBone.flags == 0x80 && g_headBone.writes == 1 &&
                  std::fabs(g_headBone.matrix[1] - std::sin(0.5f) * std::cos(0.25f)) < 1e-4f,
              "the game's animation is told to apply it, and the head bone is turned as asked");
        Check(api.head(unit, 9.0, -9.0, 0.0) == WXL_OK, "angles past a neck's");
        wxl::seam::Unit::PreAnimate::After(&wolf, nullptr);
        Check(std::fabs(g_headBone.matrix[0] - std::cos(100.0f * 3.14159265f / 180.0f) * std::cos(80.0f * 3.14159265f / 180.0f)) < 1e-3f,
              "are held to 100 degrees of yaw and 80 of pitch");
        FakeObject other;
        g_headBone.writes = 0;
        wxl::seam::Unit::PreAnimate::After(&other, nullptr);
        Check(g_headBone.writes == 0, "another unit's animation leaves the head alone");

        Check(api.release_head(unit) == WXL_OK && api.release_head(unit) == WXL_E_NOT_FOUND, "a head is released once");
        wxl::seam::Unit::PreAnimate::After(&wolf, nullptr);
        Check(g_headBone.flags == 0, "released at once, the bone goes back to the animation");
        g_headBone.writes = 0;
        wxl::seam::Unit::PreAnimate::After(&wolf, nullptr);
        Check(g_headBone.writes == 0, "and is left alone after");

        api.head(unit, 0.5, 0.0, 10.0);
        wxl::seam::Unit::PreAnimate::After(&wolf, nullptr);
        Check(g_headBone.flags == 0x80 && std::fabs(g_headBone.matrix[1]) < std::sin(0.5f) * 0.5f,
              "a slow turn starts from where the head is");
        wxl::core::services::pose::ForgetMod(0);
        api.head(unit, 0, 0, 0); // turned again by the harness, which is mod 0
        wxl::core::services::pose::Clear();
        wxl_result fromThread = WXL_OK;
        std::thread([&] { fromThread = api.head(unit, 0, 0, 0); }).join();
        Check(fromThread == WXL_E_INVALID, "off the main thread a head is refused");
        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
        wxl::seam::ObjectMgr::ObjDelete::Before(&stone);
    }

    // --- wxl.animation: animations of the game played on units -------------------------------

    void CheckAnimation()
    {
        namespace anim = wxl::core::services::animation;
        const wxl_animation_api& api = wxl::core::services::AnimationApi();
        FakeObject me;
        me.guid   = 0x7A1;
        me.typeId = WXL_OBJECT_PLAYER;
        me.health = 100;
        FakeObject wolf;
        wolf.guid   = 0x7A2;
        wolf.typeId = WXL_OBJECT_UNIT;
        wolf.health = 50;
        FakeObject stone;
        stone.guid   = 0x7A3;
        stone.typeId = WXL_OBJECT_ITEM;
        wxl::seam::ObjectMgr::InitObject::After(&me, nullptr, 100u);
        wxl::seam::ObjectMgr::InitObject::After(&wolf, nullptr, 100u);
        wxl::seam::ObjectMgr::InitObject::After(&stone, nullptr, 100u);
        const wxl_handle player = wxl::core::objects::HandleOf(&me);
        const wxl_handle unit   = wxl::core::objects::HandleOf(&wolf);
        const wxl_handle item   = wxl::core::objects::HandleOf(&stone);
        g_localPlayer = &me;
        constexpr wxl::host::ModHost::ModToken kMod = 31, kOther = 32;
        const wxl::host::ModHost::ModToken     previous = g_host->SwapCurrentMod(kMod);
        const auto frame = [&](uint32_t at) {
            g_timeMs = at;
            wxl::seam::Unit::PreAnimate::After(&me, nullptr);
            wxl::seam::Unit::PreAnimate::After(&wolf, nullptr);
        };
        const auto ended = [&](wxl_animation_end reason, uint32_t anim_id) {
            const Received* got = Only(WXL_EV_UnitAnimationEnded);
            if (!got) return false;
            const auto args = got->As<wxl_unit_animation_args>();
            return args.reason == static_cast<uint32_t>(reason) && args.anim_id == anim_id;
        };

        // Refusals.
        Check(api.play(item, 17, 0, 0) == WXL_E_INVALID && api.play(0xDEAD, 17, 0, 0) == WXL_E_NOT_FOUND &&
                  api.play(unit, 600, 0, 0) == WXL_E_INVALID && api.play(unit, 17, 0x10, 0) == WXL_E_INVALID &&
                  api.play(unit, 17, 0, 50.0) == WXL_E_INVALID && api.play(unit, 17, 0, -1.0) == WXL_E_INVALID &&
                  api.play(unit, 17, 0, std::numeric_limits<double>::quiet_NaN()) == WXL_E_INVALID && wolf.plays == 0,
              "animation: an item, a unit gone, an id, a flag or a speed out of range are refused, nothing played");
        wolf.health = 0;
        Check(api.play(unit, 17, 0, 0) == WXL_E_FAILED, "animation: a dead unit plays nothing");
        wolf.health       = 50;
        wolf.modelLoading = true;
        Check(api.play(unit, 17, 0, 0) == WXL_E_FAILED, "animation: nor a unit whose model loads");
        wolf.modelLoading = false;
        wolf.refusePlay   = true;
        Check(api.play(unit, 17, 0, 0) == WXL_E_FAILED && wolf.plays == 1, "animation: nor one the client did not play");
        wolf.refusePlay = false;
        uint32_t shown = 0;
        Check(api.playing(unit, &shown) == WXL_E_NOT_FOUND && api.current(unit, &shown) == WXL_OK && shown == 0 &&
                  api.current(item, &shown) == WXL_E_INVALID,
              "animation: nothing played, the model shows its stand");

        // A one-shot: played its length, handed back to the game.
        g_received.clear();
        g_timeMs = 1000;
        Check(api.play(unit, 17, 0, 0) == WXL_OK && wolf.shownAnim == 17 && wolf.lastPlayFlags == 0, "animation: Attack1H is played as an emote is");
        Check(api.playing(unit, &shown) == WXL_OK && shown == 17 && api.current(unit, &shown) == WXL_OK && shown == 17,
              "animation: playing and current answer it");
        frame(1500);
        Check(g_received.empty() && wolf.updates == 0, "animation: halfway through it plays on");
        frame(2000);
        Check(ended(WXL_ANIMATION_END_FINISHED, 17) && wolf.updates == 1 && wolf.shownAnim == 0 &&
                  api.playing(unit, &shown) == WXL_E_NOT_FOUND,
              "animation: after its length the game is handed the unit back, and UnitAnimationEnded says finished");

        // Speed, fallback, flags.
        g_received.clear();
        Check(api.play(unit, 62, 0, 2.0) == WXL_OK && wolf.animSpeed == 2.0f, "animation: a speed is set on the model");
        frame(2400);
        Check(g_received.empty(), "animation: twice as fast, it plays on at 400 ms");
        frame(2500);
        Check(ended(WXL_ANIMATION_END_FINISHED, 62), "animation: and is over at 500 ms");
        Check(api.play(unit, 136, WXL_ANIMATION_NO_BLEND, 0) == WXL_OK && wolf.shownAnim == 62 && wolf.lastPlayFlags == 0x1 &&
                  api.playing(unit, &shown) == WXL_OK && shown == 136,
              "animation: an animation the model lacks falls back, unblended; playing answers what was asked");
        Check(api.play(unit, 136, WXL_ANIMATION_EXACT, 0) == WXL_OK && wolf.shownAnim == 136 && wolf.lastPlayFlags == 0x4,
              "animation: exact asks the client for that id alone");

        // Interrupted, and replaced.
        frame(2600);
        g_received.clear();
        g_timeMs = 3000;
        api.play(unit, 17, 0, 0);
        frame(3000);
        Check(ended(WXL_ANIMATION_END_REPLACED, 136), "animation: a play takes the place of the one before, said at the next frame");
        g_received.clear();
        wolf.shownAnim = 5; // the game runs
        frame(3100);
        Check(ended(WXL_ANIMATION_END_INTERRUPTED, 17) && wolf.plays == 6, "animation: the game replacing a one-shot ends it, not played again");
        g_received.clear();
        g_timeMs = 3200;
        api.play(unit, 18, WXL_ANIMATION_OVERRIDE, 0);
        wolf.shownAnim = 5;
        frame(3300);
        Check(ended(WXL_ANIMATION_END_INTERRUPTED, 18) && wolf.plays == 7 && wolf.shownAnim == 5,
              "animation: override is a loop's: a one-shot under it ends when the game replaces it, not played again");

        // A loop yields to the game and comes back when the unit is idle; override at once.
        g_received.clear();
        g_timeMs = 4000;
        api.play(unit, 62, WXL_ANIMATION_LOOP, 0);
        frame(6500);
        Check(g_received.empty() && wolf.shownAnim == 62, "animation: a loop goes on past its length");
        wolf.shownAnim = 5;
        const uint32_t plays = wolf.plays;
        frame(6600);
        Check(wolf.plays == plays && wolf.shownAnim == 5, "animation: while the unit runs, a loop waits");
        wolf.shownAnim = 0;
        frame(6700);
        Check(wolf.plays == plays + 1 && wolf.shownAnim == 62 && g_received.empty(), "animation: idle again, it comes back");
        api.play(unit, 62, WXL_ANIMATION_LOOP | WXL_ANIMATION_OVERRIDE, 0);
        wolf.shownAnim = 5;
        frame(6800);
        Check(wolf.plays == plays + 3 && wolf.shownAnim == 62, "animation: override plays it again over the run at once");
        wolf.health    = 0;
        wolf.shownAnim = 1;
        frame(6900);
        Check(wolf.plays == plays + 3, "animation: but never over death");
        wolf.health = 50;

        // Stopped by its mod only; a mod that goes stops its own.
        g_received.clear();
        g_host->SwapCurrentMod(kOther);
        Check(api.stop(unit) == WXL_E_NOT_FOUND, "animation: another mod cannot stop it");
        g_host->SwapCurrentMod(kMod);
        wolf.shownAnim     = 62;
        const uint32_t ups = wolf.updates;
        Check(api.stop(unit) == WXL_OK && wolf.updates == ups + 1 && api.stop(unit) == WXL_E_NOT_FOUND,
              "animation: its mod stops it once, and the game chooses again");
        frame(7000);
        Check(ended(WXL_ANIMATION_END_STOPPED, 62), "animation: UnitAnimationEnded says stopped at the next frame");
        g_received.clear();
        api.play(player, 53, 0, 0);
        anim::ForgetMod(kMod);
        frame(7100);
        Check(ended(WXL_ANIMATION_END_STOPPED, 53) && me.updates == 1, "animation: a mod that goes stops its animations");

        // A unit that leaves view ends its play, seen at the player's frame.
        g_received.clear();
        api.play(unit, 17, 0, 0);
        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
        g_timeMs = 7200;
        wxl::seam::Unit::PreAnimate::After(&me, nullptr);
        Check(ended(WXL_ANIMATION_END_GONE, 17) && !anim::Busy(), "animation: a unit gone ends its play");

        wxl_result fromThread = WXL_OK;
        std::thread([&] { fromThread = api.play(player, 17, 0, 0); }).join();
        Check(fromThread == WXL_E_INVALID, "animation: off the main thread a play is refused");
        anim::Clear();
        g_host->SwapCurrentMod(previous);
        g_localPlayer = nullptr;
        wxl::seam::ObjectMgr::ObjDelete::Before(&me);
        wxl::seam::ObjectMgr::ObjDelete::Before(&stone);
    }

    // --- wxl.selectioncircle: the game's circles hidden and restyled -----------------------------

    void CheckSelectionCircle()
    {
        namespace circles = wxl::core::services::selectioncircle;
        const wxl_selectioncircle_api& api = wxl::core::services::SelectionCircleApi();
        FakeObject                     boar;
        boar.guid     = 0x791;
        boar.typeId   = WXL_OBJECT_UNIT;
        boar.reaction = WXL_REACTION_HOSTILE;
        FakeObject friendBear;
        friendBear.guid     = 0x792;
        friendBear.typeId   = WXL_OBJECT_UNIT;
        friendBear.reaction = WXL_REACTION_FRIENDLY;
        FakeObject corpse;
        corpse.guid   = 0x793;
        corpse.typeId = WXL_OBJECT_CORPSE;
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 100u);
        wxl::seam::ObjectMgr::InitObject::After(&friendBear, nullptr, 100u);
        wxl::seam::ObjectMgr::InitObject::After(&corpse, nullptr, 100u);
        const wxl_handle boarHandle = wxl::core::objects::HandleOf(&boar);
        const wxl_handle bearHandle = wxl::core::objects::HandleOf(&friendBear);

        // One projection as a drawer makes it: a box 1 yard around the origin, a colour; the
        // gate is handed the addresses of the arguments, as both runtimes hand them.
        float          gameBox[6] = {-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
        uint32_t       gameColor  = 0xFF00FF00u;
        const void*    box        = gameBox;
        const void*    color      = &gameColor;
        const void*    matrix     = nullptr;
        float          fade = 0.5f, offset = 0.4f;
        uint32_t       a5 = 0x200122, a6 = 1;
        const auto project = [&] {
            box   = gameBox;
            color = &gameColor;
            return wxl::seam::ProjectTex::Render2d::Gate(&box, &color, &matrix, &fade, &a5, &a6, &offset);
        };
        const auto drawTarget = [&](FakeObject& unit) {
            wxl::seam::Unit::RenderTargetSelection::Before(&unit);
            const bool taken = project();
            wxl::seam::Unit::RenderTargetSelection::After(&unit);
            return taken;
        };
        const auto boxOf = [&] { return static_cast<const float*>(box); };
        const auto colorOf = [&] { return *static_cast<const uint32_t*>(color); };

        Check(!project() && box == gameBox && color == &gameColor, "a projection outside a circle (a spell's cursor) is left alone");
        Check(!drawTarget(boar) && box == gameBox && color == &gameColor, "with no mod's look, the target's circle is the game's");
        circles::WorldEnd();
        Check(api.drawn() == 1 && api.hidden() == 0, "and counted drawn");

        Check(api.hide(0x8u, 0) == WXL_E_INVALID && api.hide(0, 0x200u) == WXL_E_INVALID &&
                  api.set_style(0, 0, wxl_str{nullptr, 0}, 0, 9.0f) == WXL_E_INVALID &&
                  api.set_style(0, 0, wxl_str{nullptr, 0}, 0, std::numeric_limits<float>::quiet_NaN()) == WXL_E_INVALID &&
                  api.set_style(0, 0, wxl_str{nullptr, 0}, 0, -1.0f) == WXL_E_INVALID,
              "a bit that names nothing, or a scale out of range, is refused");
        Check(api.set_style(WXL_SELECTIONCIRCLE_TARGET, 0, wxl_str{"Textures\\Missing.blp", 20}, 0, 1.0f) == WXL_E_NOT_FOUND &&
                  api.hide_object(0xDEAD) == WXL_E_NOT_FOUND && api.reset_object(boarHandle) == WXL_E_NOT_FOUND,
              "a texture the client makes nothing of is not found, and so is an object gone or one never changed");

        Check(api.hide(WXL_SELECTIONCIRCLE_TARGET, 0) == WXL_OK, "the target's circles are hidden");
        Check(drawTarget(boar) && drawTarget(friendBear), "a hidden circle is not projected, whatever the reaction");
        wxl::seam::Unit::RenderPetTargetSelection::Before(&boar);
        Check(!project(), "the pet's target is another kind, still drawn");
        wxl::seam::Unit::RenderPetTargetSelection::After(&boar);
        circles::WorldEnd();
        Check(api.drawn() == 1 && api.hidden() == 2, "the frame counts two hidden and one drawn");

        const char kRing[] = "Interface\\Mod\\Ring.blp";
        Check(api.set_style(WXL_SELECTIONCIRCLE_TARGET, WXL_SELECTIONCIRCLE_REACTION(WXL_REACTION_HOSTILE),
                            wxl_str{kRing, static_cast<uint32_t>(sizeof kRing - 1)}, 0x80FF0000u, 2.0f) == WXL_OK,
              "hostile targets get a look of their own");
        wxl::seam::Unit::RenderTargetSelection::Before(&boar);
        const bool swapped = g_circleTexture != &g_gameCircleTexture &&
                             static_cast<FakeCircleTexture*>(g_circleTexture)->path == kRing;
        Check(swapped, "the mod's texture is in the game's place while the circle is drawn");
        Check(!project() && box != gameBox && boxOf()[0] == -2.0f && boxOf()[4] == 2.0f && boxOf()[5] == 2.0f &&
                  gameBox[0] == -1.0f && colorOf() == 0x80FF0000u && gameColor == 0xFF00FF00u,
              "the later look wins over the hide: drawn twice as large in the mod's colour, the game's box and colour untouched");
        gameColor = 0x80123456u;
        Check(!project() && colorOf() == 0x40FF0000u, "a fading ring of the game's keeps its fade over the mod's alpha");
        gameColor = 0xFF00FF00u;
        wxl::seam::Unit::RenderTargetSelection::After(&boar);
        Check(g_circleTexture == &g_gameCircleTexture, "and the game's texture is back after it");
        Check(drawTarget(friendBear), "a friendly target is still hidden");

        Check(api.set_object_style(boarHandle, wxl_str{nullptr, 0}, 0, 0.5f) == WXL_OK, "the boar gets a look of its own");
        Check(!drawTarget(boar) && boxOf()[0] == -0.5f && boxOf()[2] == -1.0f && color == &gameColor &&
                  g_circleTexture == &g_gameCircleTexture,
              "an object's look wins over its kind's: half the size across, never lower, in the game's colour and texture");
        Check(api.hide_object(bearHandle) == WXL_OK && drawTarget(friendBear), "an object's circles are hidden");
        wxl::seam::Unit::RenderAutoTrackCursor::Before(&friendBear);
        Check(project(), "every circle of it, the move marker of the unit moving too");
        wxl::seam::Unit::RenderAutoTrackCursor::After(&friendBear);

        Check(api.set_style(WXL_SELECTIONCIRCLE_TARGET, WXL_SELECTIONCIRCLE_NO_REACTION, wxl_str{nullptr, 0}, 0xFF0000FFu, 0.0f) ==
                  WXL_OK,
              "circles under no unit get a colour");
        wxl::seam::Corpse::RenderTargetSelection::Before(&corpse);
        Check(!project() && colorOf() == 0xFF0000FFu && box == gameBox, "a corpse's circle has no reaction, and keeps its size");
        wxl::seam::Corpse::RenderTargetSelection::After(&corpse);

        Check(api.reset_object(boarHandle) == WXL_OK && api.reset_object(bearHandle) == WXL_OK &&
                  api.reset(WXL_SELECTIONCIRCLE_TARGET, 0) == WXL_OK,
              "the looks are given back");
        Check(!drawTarget(boar) && box == gameBox && color == &gameColor && !drawTarget(friendBear) && g_texturesClosed == 1,
              "the game's circles again, and the mod's texture let go with its last look");

        // Two looks of one texture share it; it goes with the mod.
        g_texturesClosed = 0;
        const size_t opened = g_openedTextures.size();
        api.set_style(WXL_SELECTIONCIRCLE_PET_TARGET, 0, wxl_str{kRing, static_cast<uint32_t>(sizeof kRing - 1)}, 0, 0.0f);
        api.set_object_style(boarHandle, wxl_str{"interface/mod/RING.blp", 22}, 0, 0.0f);
        Check(g_openedTextures.size() == opened + 1, "one texture made for one path, however it is spelt");
        api.hide(WXL_SELECTIONCIRCLE_MOVE_MARKER, 0);
        circles::ForgetMod(0);
        Check(g_texturesClosed == 1 && !drawTarget(boar), "a mod's looks and its texture go with it");
        wxl::seam::Unit::RenderAutoTrackCursor::Before(&boar);
        Check(!project(), "the move marker is the game's again");
        wxl::seam::Unit::RenderAutoTrackCursor::After(&boar);

        api.hide(0, 0);
        circles::Clear();
        Check(!drawTarget(boar) && g_circleTexture == &g_gameCircleTexture, "the client's end forgets everything");
        circles::WorldEnd();
        circles::WorldEnd();
        wxl_result fromThread = WXL_OK;
        std::thread([&] { fromThread = api.hide(0, 0); }).join();
        Check(fromThread == WXL_E_INVALID && api.drawn() == 0, "off the main thread a change is refused");
        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
        wxl::seam::ObjectMgr::ObjDelete::Before(&friendBear);
        wxl::seam::ObjectMgr::ObjDelete::Before(&corpse);
    }

    // --- wxl.imgui: the contexts, with no device --------------------------------------------------

    void CheckImGuiService()
    {
        const wxl_imgui_api& api = wxl::core::services::ImGuiApi();
        Check(wxl::View(api.version()) == IMGUI_VERSION ";" IMPLOT_VERSION, "the version is Dear ImGui's and ImPlot's");
        Check(api.context() != nullptr && api.plot_context() != nullptr && api.context() == api.context(),
              "the contexts are made once, on first use");
        void* alloc = nullptr;
        void* release = nullptr;
        api.allocators(&alloc, &release, nullptr);
        Check(alloc && release, "with the allocator they were made with");
        Check(!wxl::core::services::imgui::WindowMessage(nullptr, 0x0201, 0, 0),
              "before a frame was drawn, no message is ImGui's");
        Check(!wxl::core::services::imgui::BeginFrame(), "with nobody listening, no frame is opened");
        Check(!wxl::core::services::imgui::WindowMessage(nullptr, 0x0201, 0, 0), "nor is a message ImGui's");
        const std::vector<wxl_event> raised = wxl::core::seams::RaisedEvents();
        Check(std::find(raised.begin(), raised.end(), WXL_EV_ImGuiFrame) != raised.end(),
              "ImGuiFrame is announced as raised, so a mod may subscribe to it");
    }

    void CheckWorldTrace()
    {
        const wxl_world_api& api = wxl::core::services::WorldApi();
        wxl_vec3             hit{};
        Check(api.trace(wxl_vec3{0, 0, 0}, wxl_vec3{20, 4, 0}, 0, &hit) == WXL_TRUE && hit.x == 10.0f && hit.y == 2.0f &&
                  g_traceFlags == WXL_TRACE_CAMERA,
              "a trace meets the wall where it is, through the camera's mask when given none");
        Check(api.trace(wxl_vec3{0, 0, 0}, wxl_vec3{5, 0, 0}, WXL_TRACE_LINE_OF_SIGHT, &hit) == WXL_FALSE &&
                  g_traceFlags == WXL_TRACE_LINE_OF_SIGHT,
              "a clear way meets nothing, through the mask given");
        wxl_bool fromThread = WXL_TRUE;
        std::thread([&] { fromThread = api.trace(wxl_vec3{0, 0, 0}, wxl_vec3{20, 0, 0}, 0, &hit); }).join();
        Check(fromThread == WXL_FALSE, "off the main thread nothing is met");
    }

    // --- wxl.input: captures per mod, the wheel kept from the client ---------------------------

    bool g_keepWheel = false;
    float g_wheel    = 0.0f;
    void WXL_CALL OnWheel(void*, const void* args)
    {
        const wxl_mouse_wheel_args& wheel = *static_cast<const wxl_mouse_wheel_args*>(args);
        g_wheel += wheel.delta;
        if (g_keepWheel && wheel.handled) *wheel.handled = WXL_TRUE;
    }

    void CheckInputService()
    {
        const wxl_input_api& api = wxl::core::services::InputApi();
        Check(api.cursor_captured() == WXL_FALSE, "nobody holds the pointer at first");
        Check(api.capture_cursor(WXL_TRUE) == WXL_OK && api.capture_cursor(WXL_TRUE) == WXL_OK &&
                  api.cursor_captured() == WXL_TRUE,
              "a capture holds the pointer, twice is still one");
        Check(api.capture_cursor(WXL_FALSE) == WXL_OK && api.cursor_captured() == WXL_FALSE,
              "giving it back lets it go");
        api.capture_cursor(WXL_TRUE);
        wxl::core::services::input::ForgetMod(0);
        Check(api.cursor_captured() == WXL_FALSE, "a mod's capture goes with it");
        Check(api.pointer_over_world() == WXL_TRUE, "the pointer over the world is the client's answer");
        // The harness's window is no window: holding the pointer there does nothing, and lets go.
        api.capture_cursor(WXL_TRUE);
        wxl::core::services::input::Present();
        wxl::core::services::input::Clear();
        Check(api.cursor_captured() == WXL_FALSE, "the shutdown lets every capture go");

        // A point of the window: the last to capture or move has its point, the earlier's comes back.
        wxl_vec3 point{};
        Check(api.capture_point(&point) == WXL_E_NOT_FOUND, "nobody holds the pointer: it is held nowhere");
        Check(api.capture_cursor_at(1.2, 0.5) == WXL_E_INVALID && api.capture_cursor_at(0.5, std::nan("")) == WXL_E_INVALID &&
                  api.cursor_captured() == WXL_FALSE,
              "a point off the window is invalid and captures nothing");
        Check(api.capture_cursor_at(0.5, 0.375) == WXL_OK && api.cursor_captured() == WXL_TRUE &&
                  api.capture_point(&point) == WXL_OK && point.x == 0.5f && point.y == 0.375f,
              "a capture at a point holds it there");
        const wxl::host::ModHost::ModToken previous = g_host->SwapCurrentMod(7);
        Check(api.capture_cursor(WXL_TRUE) == WXL_OK && api.capture_point(&point) == WXL_OK && point.y == 0.5f,
              "a later mod's capture at the centre has the pointer at the centre");
        g_host->SwapCurrentMod(previous);
        Check(api.capture_cursor_at(0.25, 0.375) == WXL_OK && api.capture_point(&point) == WXL_OK && point.x == 0.25f,
              "the first moving its hold has its point again");
        Check(api.capture_cursor(WXL_FALSE) == WXL_OK && api.capture_point(&point) == WXL_OK && point.y == 0.5f &&
                  api.cursor_captured() == WXL_TRUE,
              "and letting go gives the other's back");
        wxl::core::services::input::ForgetMod(7);
        Check(api.cursor_captured() == WXL_FALSE, "until it goes too");
        long px = 0, py = 0;
        wxl::core::services::input::PixelAt(1920, 1080, 0.5, 0.5, px, py);
        Check(px == 960 && py == 540, "the centre is the pixel it always was");
        wxl::core::services::input::PixelAt(1281, 721, 0.5, 0.5, px, py);
        Check(px == 640 && py == 360, "of an odd size too");
        wxl::core::services::input::PixelAt(1920, 1080, 0.5, 0.4, px, py);
        Check(px == 960 && py == 432, "a point above the centre");
        wxl::core::services::input::PixelAt(1920, 1080, 1.0, 0.0, px, py);
        Check(px == 1919 && py == 0, "the edges stay on the window");

        const wxl_subscription sub = g_host->Events().Subscribe(WXL_EV_MouseWheel, &OnWheel, nullptr, 0);
        void*     window  = reinterpret_cast<void*>(0x2000);
        uint32_t  message = 0x020A; // WM_MOUSEWHEEL
        uintptr_t wparam  = static_cast<uintptr_t>(static_cast<uint16_t>(-240)) << 16;
        intptr_t  lparam  = 0;
        intptr_t  answer  = -1;
        g_wheel           = 0.0f;
        g_keepWheel       = false;
        Check(!wxl::seam::Input::WindowProc::Gate(&answer, &window, &message, &wparam, &lparam) && g_wheel == -2.0f,
              "a turn of the wheel is raised in notches, towards the player negative, and reaches the client");
        g_keepWheel = true;
        Check(wxl::seam::Input::WindowProc::Gate(&answer, &window, &message, &wparam, &lparam) && answer == 0,
              "a MouseWheel handler that writes handled keeps the turn from the client");
        g_host->Events().Unsubscribe(sub);
    }

    // --- wxl.data: a client database served from a mod file instead of the client's ----------

    void CheckDataService()
    {
        g_files.clear();
        g_files.push_back(FakeFile{"Mods\\mymod\\Spell.dbc", "WDBC-mine", 0, false});
        wxl::core::services::data::Clear();

        const wxl_data_api& api = wxl::core::services::DataApi();
        const wxl_str mine = Str("Mods\\mymod\\Spell.dbc");

        Check(api.serve_table(Str("Spell.dbc"), Str("Mods\\mymod\\Nope.dbc")) == WXL_E_NOT_FOUND,
              "a table cannot be served from a file the client cannot read");
        Check(api.serve_table(Str("Spell.dbc"), mine) == WXL_OK, "and can be from one it can");
        Check(api.serve_table(Str("DBFilesClient\\Spell.dbc"), mine) == WXL_E_FAILED,
              "the same table under its full name is the same table, and is already taken");

        // What the gate answers for the path the client is about to open, written where the
        // seam keeps it: the service hands over a copy, never a pointer into its own tables.
        char        redirect[512] = {};
        const char* full = "DBFilesClient\\Spell.dbc";
        Check(wxl::core::services::data::ServedPath(full, strlen(full), redirect, sizeof redirect)
                  && strcmp(redirect, "Mods\\mymod\\Spell.dbc") == 0,
              "so the client opens the mod's file instead of its own");

        const char* other = "DBFilesClient\\Item.dbc";
        Check(!wxl::core::services::data::ServedPath(other, strlen(other), redirect,
                                                    sizeof redirect),
              "a database nobody serves is opened as the client asked");
        const char* texture = "World\\Maps\\Azeroth.wdt";
        Check(!wxl::core::services::data::ServedPath(texture, strlen(texture), redirect,
                                                    sizeof redirect),
              "and a file that is not a database is not even looked up");
        const char* upper = "DBFilesClient\\CHARTITLES.DBC";
        Check(!wxl::core::services::data::ServedPath(upper, strlen(upper), redirect,
                                                    sizeof redirect)
                  && api.is_loaded(Str("CharTitles.dbc")) == WXL_TRUE,
              "a database named in capitals is the same database, suffix and all");

        Check(api.is_loaded(Str("Spell.dbc")) == WXL_TRUE
                  && api.is_loaded(Str("Talent.dbc")) == WXL_FALSE,
              "a table the client has read is known to have been read");
        Check(api.serve_table(Str("Item.dbc"), mine) == WXL_OK,
              "serving one the client read already is allowed");
        Check(api.restore_table(Str("Item.dbc")) == WXL_OK
                  && api.served_from(Str("Item.dbc")).size == 0,
              "and giving a table back leaves the client's own file");

        wxl::core::services::data::Clear();
        g_files.clear();
    }

    // --- Sound: a gate that can keep a sound from starting -------------------------------------

    bool g_hushSound = false;
    int  g_sounds    = 0;
    uint32_t g_lastKit = 0;

    void WXL_CALL OnSound(void*, const void* args)
    {
        const wxl_sound_args& sound = *static_cast<const wxl_sound_args*>(args);
        ++g_sounds;
        g_lastKit = sound.sound_id;
        if (g_hushSound && sound.suppressed) *sound.suppressed = WXL_TRUE;
    }

    void CheckSound()
    {
        const wxl_subscription sub =
            g_host->Events().Subscribe(WXL_EV_SoundPlay, &OnSound, nullptr, 0);

        int32_t     kit        = 1234;
        const void* position   = nullptr;
        void*       object     = nullptr;
        void*       properties = nullptr;
        uint32_t    advanced   = 0;
        uint32_t*   soundId    = nullptr;
        int32_t     distance   = 1;
        int32_t     keep       = 0;
        int32_t     answer     = -1;

        g_hushSound = false;
        Check(!wxl::seam::Sound::PlaySoundKit::Gate(&answer, &kit, &position, &object,
                                                     &properties, &advanced, &soundId,
                                                     &distance, &keep),
              "a sound nobody stops is played by the client");
        Check(g_sounds == 1 && g_lastKit == 1234u,
              "and the handler is told which kit, whether it was named by id or by path");

        g_hushSound = true;
        Check(wxl::seam::Sound::PlaySoundKit::Gate(&answer, &kit, &position, &object,
                                                    &properties, &advanced, &soundId,
                                                    &distance, &keep),
              "a handler that suppresses it takes the call");
        Check(answer == 17, "and the client is answered as it answers itself when sound is off");

        g_host->Events().Unsubscribe(sub);
    }

    // --- Storage: a gate that changes what the client is about to open ------------------------

    const char* g_answerWith = nullptr;

    void WXL_CALL OnFileOpen(void*, const void* args)
    {
        const wxl_file_open_args& open = *static_cast<const wxl_file_open_args*>(args);
        if (!g_answerWith || !open.redirect || !open.redirect_capacity) return;
        const size_t length = strlen(g_answerWith);
        if (length + 1 > open.redirect_capacity) return;
        memcpy(open.redirect, g_answerWith, length + 1);
    }

    void CheckStorage()
    {
        const wxl_subscription sub =
            g_host->Events().Subscribe(WXL_EV_FileOpen, &OnFileOpen, nullptr, 0);

        void*       archive = nullptr;
        const char* asked   = "World\\Maps\\Azeroth\\Azeroth.wdt";
        const char* name    = asked;
        uint32_t    flags   = 0;
        void**      file    = nullptr;
        int32_t     answer  = -1;

        g_answerWith = nullptr;
        Check(!wxl::seam::Storage::OpenFile::Gate(&answer, &archive, &name, &flags, &file),
              "a file nobody claims is opened by the client, as asked");
        Check(name == asked, "and the name it was handed is untouched");

        g_answerWith = "Mods\\example\\Azeroth.wdt";
        Check(!wxl::seam::Storage::OpenFile::Gate(&answer, &archive, &name, &flags, &file),
              "a redirected file is still opened by the client: the gate does not take the call");
        Check(name != asked && strcmp(name, g_answerWith) == 0,
              "but what it opens is the path the handler wrote");

        // The redirected path has to outlive the gate: the client opens it afterwards.
        const char* kept = name;
        g_answerWith     = nullptr;
        name             = asked;
        wxl::seam::Storage::OpenFile::Gate(&answer, &archive, &name, &flags, &file);
        Check(kept[0] == 0,
              "a redirect lives in one buffer per thread, so it lasts until that thread opens "
              "the next file and no longer");

        g_host->Events().Unsubscribe(sub);
    }

    /// Client.Destroy: the last thing a mod is told, and the point at which what a mod left
    /// jumping into its own code is taken away before that code is freed.
    // --- Render: the slots of a frame --------------------------------------------------------
    //
    // FrameBegin and FrameEnd come from two seams, and the client presents more often than it paints
    // a frame: its loading screen presents frames of its own, and a paint with nothing to draw into
    // presents anyway. What a mod is promised is pairs, in the client's order, and nothing outside
    // a frame. Each way the client can break that is driven here.

    void CheckRender()
    {
        wxl::core::frame::Reset();
        int top   = 0; // CSimpleTop's `this`; the seam does not read it
        int world = 0; // the world frame the world pass is handed

        auto paint = [] { wxl::seam::Scrn::OnPaint::Before(nullptr, nullptr); };
        auto present = []
        {
            wxl::seam::Gx::ScenePresent::Before();
        };

        g_gxContext = false;
        g_received.clear();
        paint();
        present();
        Check(g_received.empty(), "a paint with no device context opens no frame, and ends none");

        g_gxContext = true;
        g_gxVisible = false;
        g_received.clear();
        paint();
        present();
        Check(g_received.empty(), "nor does one whose window is minimised");
        g_gxVisible = true;

        g_received.clear();
        present();
        Check(g_received.empty(), "a present no paint opened a frame for, as the loading screen's, "
                                  "raises nothing");

        g_received.clear();
        wxl::seam::WorldFrame::RenderWorld::Before(&world);
        const bool inWorld = wxl::core::gx::InWorldPass();
        wxl::seam::WorldFrame::RenderWorld::After(&world);
        Check(inWorld && !wxl::core::gx::InWorldPass(), "the world's pass is marked from its Before to its After (the Vulkan device leaves its draws out)");
        wxl::seam::SimpleTop::OnLayerRender::After(&top);
        Check(g_received.empty(), "the world pass and the interface's end raise nothing outside a frame");

        // A frame in the client's own order: the paint, the interface drawing with the world inside
        // it, the present. A frame announces no stage: by the time the client paints one, the
        // script context already exists, so the graphics stage is the device's to announce.
        wxl::core::stages::Restart();
        wxl::core::stages::Reach(WXL_STAGE_STORAGE);
        g_received.clear();
        paint();
        Check(wxl::core::stages::Reached() == WXL_STAGE_STORAGE, "a frame announces no stage");
        wxl::seam::WorldFrame::RenderWorld::Before(&world);
        wxl::seam::WorldFrame::RenderWorld::After(&world);
        wxl::seam::SimpleTop::OnLayerRender::After(&top);
        present();

        const wxl_event order[] = {WXL_EV_FrameBegin,  WXL_EV_WorldRenderBegin, WXL_EV_RenderPoint, WXL_EV_WorldRenderEnd,
                                   WXL_EV_UiRenderEnd, WXL_EV_RenderPoint,      WXL_EV_FrameEnd};
        bool inOrder = g_received.size() == 7;
        for (size_t i = 0; inOrder && i < 7; ++i) inOrder = g_received[i].event == order[i];
        Check(inOrder, "a frame raises FrameBegin, the world pass with the point after the world, the interface's end, "
                       "the point after it and FrameEnd, in that order");
        Check(inOrder && g_received[2].As<wxl_render_point_args>().point == WXL_RENDER_POINT_AFTER_WORLD &&
                  g_received[5].As<wxl_render_point_args>().point == WXL_RENDER_POINT_AFTER_UI,
              "the points are after the world, then after the interface");

        uint64_t first = 0;
        if (inOrder)
        {
            first     = g_received[0].As<wxl_frame_args>().index;
            bool same = first != 0;
            for (const Received& received : g_received)
                same = same && (received.event == WXL_EV_RenderPoint ? received.As<wxl_render_point_args>().frame
                                                                     : received.As<wxl_frame_args>().index) == first;
            Check(same, "and every slot of it carries that frame's index");
        }

        g_received.clear();
        present();
        Check(g_received.empty(), "a frame ends once: a second present after it raises nothing");

        g_received.clear();
        paint();
        paint();
        Check(Count(WXL_EV_FrameBegin) == 1, "a paint reached again with its frame still open "
                                             "opens no second one");
        present();
        Check(Count(WXL_EV_FrameEnd) == 1, "and that frame closes once");
        const Received* next = Only(WXL_EV_FrameBegin);
        Check(next && next->As<wxl_frame_args>().index == first + 1, "the next frame counts up by one");

        // The device going and coming back carries the size it renders at.
        g_gxWidth  = 1280;
        g_gxHeight = 720;
        g_received.clear();
        wxl::seam::Gx::ReleaseD3dResources::Before(&top, 0);
        const Received* lost = Only(WXL_EV_SwapchainLost);
        Check(lost && lost->As<wxl_swapchain_args>().width == 1280
                  && lost->As<wxl_swapchain_args>().height == 720,
              "SwapchainLost carries the size that is about to go away");

        g_gxWidth  = 2560;
        g_gxHeight = 1440;
        g_received.clear();
        wxl::seam::Gx::NotifyOnDeviceRestored::Before(&top);
        const Received* back = Only(WXL_EV_SwapchainRestored);
        Check(back && back->As<wxl_swapchain_args>().width == 2560
                  && back->As<wxl_swapchain_args>().height == 1440,
              "and SwapchainRestored the size it came back at");

        // With no device at all there is no size to report, and none is made up.
        g_gxDevice = false;
        g_received.clear();
        wxl::seam::Gx::ReleaseD3dResources::Before(&top, 0);
        const Received* gone = Only(WXL_EV_SwapchainLost);
        Check(gone && gone->As<wxl_swapchain_args>().width == 0
                  && gone->As<wxl_swapchain_args>().height == 0,
              "a device torn down reports no size rather than an invented one");
        g_gxDevice = true;
        g_gxWidth  = 1920;
        g_gxHeight = 1080;
        wxl::core::frame::Reset();

        // The graphics stage belongs to the device being made, between storage and script, which
        // is the order InitializeGlobal and InitializeClient bring them up in.
        wxl::core::stages::Restart();
        wxl::core::stages::Reach(WXL_STAGE_STORAGE);
        g_gxDevice = false;
        wxl::seam::ConsoleDevice::Initialize::After("World of Warcraft", true, true);
        Check(wxl::core::stages::Reached() == WXL_STAGE_STORAGE,
              "a device bring-up that left no device announces no graphics stage");
        g_gxDevice = true;
        wxl::seam::ConsoleDevice::Initialize::After("World of Warcraft", true, true);
        Check(wxl::core::stages::Reached() == WXL_STAGE_GRAPHICS,
              "one that left a device announces the graphics stage");
        Check(!GetModuleHandleW(L"vulkan-1.dll"),
              "without [gx] api = vulkan the device's bring-up loads no Vulkan loader: D3D9 as it was");

        // The table the recompiled client's CGxDeviceVk calls, without [gx] in the settings.
        const wxl_gx_vk_backend* vk = WarcraftXLGxVulkan();
        Check(vk && vk->struct_size == sizeof(wxl_gx_vk_backend) && vk->select_api && vk->device_create &&
                  vk->device_destroy && vk->device_set_window && vk->device_caps && vk->window_size && vk->scene_begin &&
                  vk->scene_clear && vk->draw && vk->scene_present && vk->device_restore && vk->gamma_changed &&
                  vk->cursor_changed && vk->cursor_set && vk->pool_memory && vk->pool_resize && vk->pool_destroy &&
                  vk->tex_create && vk->tex_destroy && vk->simulate_loss && vk->describe && vk->shader_constants &&
                  vk->shader_create && vk->shader_destroy && vk->tex_lock && vk->tex_unlock && vk->pool_discard &&
                  vk->pool_written && vk->shader_prewarm && vk->target_set && vk->tex_copy && vk->tex_stretch && vk->query_create &&
                  vk->query_destroy && vk->query_begin && vk->query_end && vk->query_result &&
                  vk->device_layout_hash == wxl::engine::CGxDevice::kLayoutHash,
              "the Vulkan table is whole, with the layout of CGxDevice it reads the engine by");
        if (vk)
        {
            // A device the runtime did not make is not its: every row answers as for no device.
            alignas(16) unsigned char fakeDevice[sizeof(wxl::engine::CGxDevice)] = {};
            alignas(16) unsigned char fakePool[sizeof(wxl::engine::CGxPool)]     = {};
            alignas(16) unsigned char fakeTex[sizeof(wxl::engine::CGxTex)]       = {};
            char                      said[128]                                  = {};
            Check(vk->scene_begin(fakeDevice) == WXL_GX_VK_FRAME_LOST && vk->scene_present(fakeDevice) == WXL_GX_VK_FRAME_LOST &&
                      !vk->pool_memory(fakeDevice, fakePool) && vk->tex_create(fakeDevice, fakeTex, 4, 4, 1) == 0 &&
                      !vk->tex_lock(fakeDevice, fakeTex, 0, 0, 0, 0, 4, 4, nullptr) &&
                      vk->shader_create(fakeDevice, fakeTex, 1, "UI:0", nullptr, 0) == 0 &&
                      vk->tex_copy(fakeDevice, fakeTex, 0, 0, 0, 0, 4, 4, 0, 0) == 0 &&
                      vk->tex_stretch(fakeDevice, fakeTex, nullptr, nullptr, 0, 0) == 0 && vk->query_create(fakeDevice) == 0 &&
                      vk->query_begin(fakeDevice, 1) == 0 && vk->query_end(fakeDevice, 1) == 0 &&
                      vk->query_result(fakeDevice, 1, nullptr) == 0 &&
                      vk->device_restore(fakeDevice, said, sizeof said) == WXL_E_INVALID &&
                      vk->device_set_window(fakeDevice, nullptr, said, sizeof said) == WXL_E_INVALID,
                  "the Vulkan rows refuse a device they did not make");
            vk->scene_clear(fakeDevice, 3, 0xFF000000u, 0, 0, 16, 16);
            vk->draw(fakeDevice, nullptr, 0);
            const float rows[4] = {};
            vk->shader_constants(fakeDevice, 0, 0, 1, rows);
            vk->shader_destroy(fakeDevice, fakeTex);
            vk->tex_unlock(fakeDevice, fakeTex);
            vk->pool_discard(fakeDevice, fakePool);
            vk->pool_written(fakeDevice, fakePool, 0, 16);
            const void*    codes[1] = {rows};
            const uint32_t sizes[1] = {sizeof rows};
            vk->shader_prewarm(fakeDevice, codes, sizes, 1);
            vk->target_set(fakeDevice);
            vk->query_destroy(fakeDevice, 1);
            vk->window_size(fakeDevice, 640, 480, 0);
            vk->gamma_changed(fakeDevice);
            vk->cursor_changed(fakeDevice);
            vk->simulate_loss(fakeDevice);
            vk->describe(fakeDevice, said, sizeof said);
            Check(std::strcmp(said, "Vulkan: no device") == 0, "and describe it as no device");
            std::memset(said, 0, sizeof said);
            Check(vk->device_create(fakeDevice, nullptr, said, sizeof said) == WXL_E_FAILED && said[0] &&
                      !GetModuleHandleW(L"vulkan-1.dll"),
                  "a device asked for without a window is refused with a reason, before any Vulkan loader is loaded");
        }
        Check(vk && vk->select_api(WXL_GX_API_D3D9) == WXL_GX_API_D3D9 && vk->select_api(WXL_GX_API_D3D9EX) == WXL_GX_API_D3D9EX &&
                  vk->select_api(WXL_GX_API_VULKAN) == WXL_GX_API_VULKAN,
              "without [gx] api the client's request stands");
        Check(!GetModuleHandleW(L"vulkan-1.dll"), "and choosing loads no Vulkan loader either");

        // [gx] api = vulkan with a device made without the renderer's adapter (here, as for an
        // OpenGL request): said once, with what the client made, and no loader is loaded for it.
        wxl::core::settings::Settings vulkan;
        vulkan.gx_api = wxl::core::settings::GxApi::vulkan;
        char why[512] = {};
        const wxl::core::gx::Outcome outcome = wxl::core::gx::DeviceReady(vulkan, why, sizeof why);
        Check(outcome == wxl::core::gx::Outcome::missed && std::strstr(why, "without the renderer"),
              "[gx] api = vulkan with a device made without the adapter says so");
        Check(!GetModuleHandleW(L"vulkan-1.dll"), "and loads no Vulkan loader for it");
        wxl::core::stages::Reach(WXL_STAGE_SCRIPT);
        Check(wxl::core::stages::Reached() == WXL_STAGE_SCRIPT, "and script still follows it");
    }

    void CheckClientDestroy()
    {
        namespace stages = wxl::core::stages;
        stages::Restart();
        stages::Reach(WXL_STAGE_READY);
        g_released = 0;

        wxl::seam::Client::Destroy::Before();
        Check(stages::Reached() == WXL_STAGE_SHUTDOWN,
              "Client.Destroy announces the shutdown stage");
        Check(g_released == 1,
              "and takes back every detour a mod left behind, before its binary is freed");

        stages::Restart();
    }

    // --- NetClient: the connection coming up and going away ------------------------------------

    void CheckNetConnection()
    {
        g_received.clear();
        wxl::seam::NetClient::HandleConnect::After(1, nullptr);
        Check(Only(WXL_EV_Connected) != nullptr,
              "it is raised once the client has moved itself into the connected state");

        g_received.clear();
        wxl::seam::NetClient::HandleDisconnect::Before(nullptr);
        Check(Only(WXL_EV_Disconnected) != nullptr,
              "Disconnected is raised before the client tears the connection down");
    }

    // --- wxl.storage: a folder in the search, and the names across the archives ---------------

    void WriteFileAt(const std::filesystem::path& path, const std::string& text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    void CheckSearchPath(const wxl_storage_api& api)
    {
        namespace fs = std::filesystem;
        namespace storage = wxl::core::services::storage;
        const fs::path root = fs::temp_directory_path() / "wxl-harness-storage";
        std::error_code ignored;
        fs::remove_all(root, ignored);
        const fs::path client = root / "client";
        WriteFileAt(client / "Mods" / "mymod" / "assets" / "Interface" / "Icons" / "Mine.blp", "mine");
        WriteFileAt(client / "Mods" / "mymod" / "assets" / "Interface" / "FrameXML" / "FrameXML.toc", "ours");
        WriteFileAt(client / "Mods" / "low" / "Interface" / "FrameXML" / "FrameXML.toc", "low");
        WriteFileAt(client / "Mods" / "low" / "Interface" / "Only.txt", "only here");
        WriteFileAt(root / "elsewhere" / "Interface" / "Far.txt", "far");
        g_basePath = client.string() + "\\";
        g_files.push_back(FakeFile{"Interface\\FrameXML\\FrameXML.toc", "the client's", 0, false});

        const std::string assets = (client / "Mods" / "mymod" / "assets").string();
        const std::string low    = (client / "Mods" / "low").string();
        const std::string far    = (root / "elsewhere").string();
        Check(api.add_search_path(Str(assets.c_str()), 1) == WXL_OK &&
                  api.add_search_path(Str(low.c_str()), 0) == WXL_OK &&
                  api.add_search_path(Str(far.c_str()), 5) == WXL_OK,
              "folders join the search, at their priorities");
        Check(api.add_search_path(Str((root / "nowhere").string().c_str()), 1) == WXL_E_NOT_FOUND &&
                  api.add_search_path(Str(""), 1) == WXL_E_INVALID,
              "a folder that is not there is not found; no name is invalid");
        Check(api.exists(Str("Interface\\Icons\\Mine.blp")) == WXL_TRUE &&
                  api.exists(Str("interface/only.txt")) == WXL_TRUE && api.exists(Str("Interface\\None.txt")) == WXL_FALSE,
              "exists sees a folder's files, by their client names");

        // The gate turns a name a folder wins into its path in the client's folder, behind two
        // backslashes: the client opens it from the disk without its table of loose files.
        const auto open = [](const char* asked, uint32_t& flags) {
            void*       archive = nullptr;
            const char* name    = asked;
            void**      file    = nullptr;
            int32_t     answer  = -1;
            flags               = 0;
            wxl::seam::Storage::OpenFile::Gate(&answer, &archive, &name, &flags, &file);
            // Then FindFile, as SFile::OpenEx calls it: a folder's file is a name rewritten.
            char      path[260] = {};
            char*     out       = path;
            uint32_t  size      = sizeof path;
            uint32_t  type      = 3;
            uint32_t* typeAt    = &type;
            void*     found     = nullptr;
            void**    foundAt   = &found;
            if (wxl::seam::Storage::FindFile::Gate(&answer, &name, &out, &size, &flags, &typeAt, &foundAt))
                return std::string(path);
            return std::string(name);
        };
        uint32_t flags = 0;
        Check(open("Interface\\Icons\\Mine.blp", flags) == "\\\\Mods\\mymod\\assets\\Interface\\Icons\\Mine.blp" &&
                  flags == 0,
              "a folder's file is opened by its path in the client's folder, the open's flags left alone");
        Check(open("Interface\\FrameXML\\FrameXML.toc", flags) ==
                      "\\\\Mods\\mymod\\assets\\Interface\\FrameXML\\FrameXML.toc",
              "a folder above 0 wins a name over the client's own");
        Check(open("Interface\\Only.txt", flags) == "\\\\Mods\\low\\Interface\\Only.txt",
              "a folder at 0 fills a name the client does not have");
        api.add_search_path(Str(assets.c_str()), -1);
        Check(open("Interface\\FrameXML\\FrameXML.toc", flags) == "Interface\\FrameXML\\FrameXML.toc",
              "but never one it has; a folder added again takes its new priority");
        Check(open("Interface\\Far.txt", flags) == "Interface\\Far.txt" && flags == 0,
              "a folder outside the client's folder cannot serve the client, and is passed over");
        Check(open("World\\Elsewhere.wdt", flags) == "World\\Elsewhere.wdt" && flags == 0 &&
                  open("C:\\x\\Interface\\Only.txt", flags) == "C:\\x\\Interface\\Only.txt",
              "a name no folder holds, or that is not a client name, is the client's");
        WriteFileAt(client / "Mods" / "mymod" / "assets" / "Interface" / "Icons" / "New.blp", "new");
        Check(open("Interface\\Icons\\New.blp", flags) == "\\\\Mods\\mymod\\assets\\Interface\\Icons\\New.blp",
              "a file put in a folder since it joined is found at the next open");
        fs::remove(client / "Mods" / "mymod" / "assets" / "Interface" / "Icons" / "New.blp", ignored);
        const fs::path working = fs::current_path();
        fs::current_path(client);
        g_basePath.clear();
        Check(open("Interface\\Icons\\Mine.blp", flags) == assets + "\\Interface\\Icons\\Mine.blp",
              "with no base path, in the client's folder, the name is the file's absolute path, opened as it is");
        g_basePath = client.string() + "\\";
        fs::current_path(working);

        // find: the listfiles of the archives, then the folders, each name once.
        std::vector<wxl_str> names(16);
        const uint32_t count = api.find(Str("interface\\framexml\\*.toc"), names.data(), 16);
        Check(count == 1 && std::string(names[0].data, names[0].size) == "Interface\\FrameXML\\FrameXML.toc",
              "find matches case aside, and names a file both an archive and a folder have once");
        const uint32_t icons = api.find(Str("Interface\\Icons\\*"), nullptr, 0);
        const uint32_t again = api.find(Str("Interface\\Icons\\*"), names.data(), 16);
        Check(icons == 2 && again == 2 && std::string(names[0].data, names[0].size) == "INTERFACE\\Icons\\A.blp" &&
                  std::string(names[1].data, names[1].size) == "Interface\\Icons\\Mine.blp",
              "a count then a fill give the same names, sorted, from the archives and the folders");
        Check(api.find(Str("*.txt"), nullptr, 0) == 1 && api.find(Str("Nothing*"), nullptr, 0) == 0 &&
                  api.exists(Str("Interface\\Far.txt")) == WXL_FALSE,
              "a pattern spans the folders the client can open from, and may match nothing");

        // wxl.data takes a table from a folder: its check is the storage's.
        const wxl_data_api& data = wxl::core::services::DataApi();
        Check(data.serve_table(Str("Only.dbc"), Str("Interface\\Only.txt")) == WXL_OK,
              "a table served from a folder's file is accepted");
        data.restore_table(Str("Only.dbc"));

        // A folder is the mod's that added it: removed by it, or when it goes.
        constexpr wxl::host::ModHost::ModToken kMod = 9;
        const auto asMod = [&](auto call) {
            const wxl::host::ModHost::ModToken previous = g_host->SwapCurrentMod(kMod);
            const wxl_result                   result   = call();
            g_host->SwapCurrentMod(previous);
            return result;
        };
        Check(api.remove_search_path(Str(low.c_str())) == WXL_OK &&
                  open("Interface\\Only.txt", flags) == "Interface\\Only.txt" && flags == 0,
              "a folder removed serves nothing from then on");
        Check(api.remove_search_path(Str(low.c_str())) == WXL_E_NOT_FOUND &&
                  api.remove_search_path(Str("")) == WXL_E_INVALID,
              "removing it again finds nothing; no name is invalid");
        Check(asMod([&] { return api.add_search_path(Str(low.c_str()), 0); }) == WXL_OK &&
                  open("Interface\\Only.txt", flags) == "\\\\Mods\\low\\Interface\\Only.txt",
              "a mod adds the folder again");
        Check(api.remove_search_path(Str(low.c_str())) == WXL_E_NOT_FOUND &&
                  open("Interface\\Only.txt", flags) == "\\\\Mods\\low\\Interface\\Only.txt",
              "and no other mod's code removes it for that mod");
        Check(asMod([&] { return api.remove_search_path(Str((low + "\\").c_str())); }) == WXL_OK,
              "its own code does, by another spelling of the same folder");
        asMod([&] { return api.add_search_path(Str(low.c_str()), 0); });
        storage::RemoveSearchPathsOf(kMod);
        Check(open("Interface\\Only.txt", flags) == "Interface\\Only.txt",
              "a mod that goes takes its folders with it");
        Check(open("Interface\\Icons\\Mine.blp", flags) == "\\\\Mods\\mymod\\assets\\Interface\\Icons\\Mine.blp",
              "and leaves the other folders");

        // Redirects: a name opens another, for the mod that made it; the latest answers.
        const std::string mine = "\\\\Mods\\mymod\\assets\\Interface\\Icons\\Mine.blp";
        Check(open("Interface\\Icons\\Q.blp", flags) == "Interface\\Icons\\Q.blp",
              "no name is redirected before a mod asks");
        Check(api.redirect(Str("Interface\\Icons\\Q.blp"), Str("Interface/Icons/Mine.blp")) == WXL_OK &&
                  open("interface/icons/q.BLP", flags) == mine,
              "a redirected name, in any case and with either slash, opens the other, found through the search path");
        Check(asMod([&] { return api.redirect(Str("INTERFACE\\ICONS\\Q.BLP"), Str("World\\Other.blp")); }) == WXL_OK &&
                  open("Interface\\Icons\\Q.blp", flags) == "World\\Other.blp",
              "another mod's later redirect of the name wins");
        Check(api.unredirect(Str("Interface\\Icons\\Nothing.blp")) == WXL_E_NOT_FOUND &&
                  api.redirect(Str(""), Str("x")) == WXL_E_INVALID &&
                  api.redirect(Str("x"), Str(std::string(512, 'a').c_str())) == WXL_E_INVALID,
              "a name nobody redirected is not found; an empty name or one past the buffer is invalid");
        Check(asMod([&] { return api.unredirect(Str("interface\\icons\\q.blp")); }) == WXL_OK &&
                  open("Interface\\Icons\\Q.blp", flags) == mine,
              "taking it back uncovers the earlier redirect");
        Check(asMod([&] { return api.unredirect(Str("Interface\\Icons\\Q.blp")); }) == WXL_E_NOT_FOUND,
              "and a mod takes back only its own");
        asMod([&] { return api.redirect(Str("Interface\\Icons\\Q.blp"), Str("World\\Other.blp")); });
        storage::RemoveRedirectsOf(kMod);
        Check(open("Interface\\Icons\\Q.blp", flags) == mine, "a mod that goes takes its redirects with it");
        {
            std::string seen;
            const auto  watch = [](void* user, const void* args) {
                const wxl_file_open_args& o = *static_cast<const wxl_file_open_args*>(args);
                *static_cast<std::string*>(user) = std::string(o.path.data, o.path.size) + " > " + o.redirect;
                if (g_answerWith) std::memcpy(o.redirect, g_answerWith, std::strlen(g_answerWith) + 1);
            };
            const wxl_subscription sub = g_host->Events().Subscribe(WXL_EV_FileOpen, watch, &seen, 0);
            g_answerWith               = nullptr;
            Check(open("Interface\\Icons\\Q.blp", flags) == mine &&
                      seen == "Interface\\Icons\\Q.blp > Interface\\Icons\\Mine.blp",
                  "a FileOpen handler sees the name asked for, and the redirect's answer in `redirect`");
            g_answerWith = "World\\Handler.blp";
            Check(open("Interface\\Icons\\Q.blp", flags) == "World\\Handler.blp",
                  "and a handler that answers has the last word");
            g_answerWith = nullptr;
            g_host->Events().Unsubscribe(sub);
        }
        std::string elsewhere;
        std::thread([&] {
            uint32_t loader = 0;
            elsewhere       = open("Interface\\Icons\\Q.blp", loader);
        }).join();
        Check(elsewhere == mine, "a redirect answers an open on another thread, where no handler can");
        Check(api.unredirect(Str("Interface\\Icons\\Q.blp")) == WXL_OK &&
                  open("Interface\\Icons\\Q.blp", flags) == "Interface\\Icons\\Q.blp",
              "taken back, the name opens as itself");

        storage::ForgetSearchPaths();
        Check(open("Interface\\Icons\\Mine.blp", flags) == "Interface\\Icons\\Mine.blp",
              "forgotten folders serve nothing");
        g_files.clear();
        g_basePath.clear();
        fs::remove_all(root, ignored);
    }


    // --- the asset pipeline: providers, served bytes, transforms --------------------------------

    /// A provider over a table of names, counting what it is asked.
    struct FakeProvider
    {
        std::map<std::string, std::string> files; ///< by lower-case name
        std::map<std::string, std::vector<std::pair<std::string, bool>>> folders;
        std::atomic<int> opens{0}, closes{0}, reads{0};
        bool             faultOnExists = false;
        uint64_t         removeFromInside = 0; ///< a handle exists tries to take back
        wxl_result       removedFromInside = WXL_OK;
        wxl_asset_provider table{};

        static std::string Lowered(wxl_str path)
        {
            std::string key(path.data, path.size);
            for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return key;
        }

        std::set<std::string> pending; ///< names it answers "not yet" for

        static wxl_result WXL_CALL Exists(void* user, wxl_str path)
        {
            FakeProvider& self = *static_cast<FakeProvider*>(user);
            if (self.pending.count(Lowered(path))) return WXL_E_PENDING;
            if (self.faultOnExists) *static_cast<volatile int*>(nullptr) = 1;
            if (self.removeFromInside)
                self.removedFromInside = wxl::core::services::StorageApi().remove_provider(self.removeFromInside);
            return self.files.count(Lowered(path)) ? WXL_OK : WXL_E_NOT_FOUND;
        }

        static void* WXL_CALL Open(void* user, wxl_str path, uint64_t* size)
        {
            FakeProvider& self = *static_cast<FakeProvider*>(user);
            const auto    it   = self.files.find(Lowered(path));
            if (it == self.files.end()) return nullptr;
            ++self.opens;
            *size = it->second.size();
            return const_cast<std::string*>(&it->second);
        }

        static wxl_result WXL_CALL ReadAt(void* user, void* file, uint64_t offset, void* buffer, uint32_t size,
                                          uint32_t* read)
        {
            ++static_cast<FakeProvider*>(user)->reads;
            const std::string& bytes = *static_cast<const std::string*>(file);
            const uint64_t     left  = offset < bytes.size() ? bytes.size() - offset : 0;
            *read                    = left < size ? static_cast<uint32_t>(left) : size;
            std::memcpy(buffer, bytes.data() + offset, *read);
            return WXL_OK;
        }

        static void WXL_CALL Close(void* user, void*) { ++static_cast<FakeProvider*>(user)->closes; }

        static void WXL_CALL List(void* user, wxl_str folder, wxl_asset_entry_fn fn, void* context)
        {
            FakeProvider& self = *static_cast<FakeProvider*>(user);
            const auto    it   = self.folders.find(Lowered(folder));
            if (it == self.folders.end()) return;
            for (const auto& [name, isFolder] : it->second)
                fn(context, wxl_str{name.data(), static_cast<uint32_t>(name.size())}, isFolder ? WXL_TRUE : WXL_FALSE);
        }

        const wxl_asset_provider* Table()
        {
            table = wxl_asset_provider{sizeof(wxl_asset_provider), this, &Exists, &Open, &ReadAt, &Close, &List};
            return &table;
        }
    };

    std::string ReadAll(const wxl_storage_api& api, const char* path)
    {
        std::string bytes(4096, '\0');
        uint64_t    read = 0;
        if (api.read(Str(path), bytes.data(), bytes.size(), &read) != WXL_OK) return "<none>";
        bytes.resize(static_cast<size_t>(read));
        return bytes;
    }

    /// A transform that upper-cases a file, one that adds a mark (a size change), one by magic.
    wxl_result WXL_CALL Upper(void*, wxl_str, const void* data, uint64_t size, const wxl_asset_output* out)
    {
        char* to = static_cast<char*>(out->replace(out->context, size));
        if (!to) return WXL_E_FAILED;
        for (uint64_t i = 0; i < size; ++i) to[i] = static_cast<char>(std::toupper(static_cast<const unsigned char*>(data)[i]));
        return WXL_OK;
    }

    wxl_result WXL_CALL Mark(void* user, wxl_str, const void* data, uint64_t size, const wxl_asset_output* out)
    {
        const std::string& mark = *static_cast<const std::string*>(user);
        char*              to   = static_cast<char*>(out->replace(out->context, size + mark.size()));
        std::memcpy(to, data, static_cast<size_t>(size));
        std::memcpy(to + size, mark.data(), mark.size());
        return WXL_OK;
    }

    wxl_result WXL_CALL Untouched(void* user, wxl_str, const void*, uint64_t, const wxl_asset_output*)
    {
        ++*static_cast<int*>(user);
        return WXL_E_NOT_FOUND;
    }

    std::vector<std::string> g_walked;

    bool Walked(const fsk::ProcessDirParms& parms)
    {
        g_walked.push_back(std::string(parms.filename) + (parms.is_dir ? "\\" : ""));
        return true;
    }

    void CheckAssetPipeline()
    {
        namespace assets = wxl::core::services::assets;
        const wxl_storage_api& api = wxl::core::services::StorageApi();
        Check(api.struct_size >= offsetof(wxl_storage_api, unserve) + sizeof(void*),
              "wxl.storage carries the asset pipeline's members");

        BuildFakeStack();
        g_files.clear();
        g_files.push_back(FakeFile{"World\\Base.txt", "the client's", 0, false});
        g_files.push_back(FakeFile{"World\\Model.bin", "MD20 model", 0, false});
        g_files.push_back(FakeFile{"World\\Other.bin", "MD21 model", 0, false});
        g_opens = g_closes = 0;

        Check(!assets::Active() && ReadAll(api, "World\\Base.txt") == "the client's",
              "with nothing registered, the client's files are its own");

        // Providers by priority: above 0 before the client, 0 or below only what it lacks.
        FakeProvider high, low;
        high.files = {{"interface\\p\\a.txt", "from high"}, {"world\\base.txt", "high's base"}};
        low.files  = {{"interface\\p\\fill.txt", "from low"}, {"world\\base.txt", "low's base"},
                      {"interface\\p\\a.txt", "low's a"}};
        uint64_t highId = 0, lowId = 0;
        Check(api.add_provider(high.Table(), 5, &highId) == WXL_OK && highId != 0 &&
                  api.add_provider(low.Table(), -1, &lowId) == WXL_OK && lowId != highId,
              "two providers join, at their priorities");
        wxl_asset_provider broken = *high.Table();
        broken.read_at            = nullptr;
        uint64_t ignored          = 0;
        Check(api.add_provider(&broken, 1, &ignored) == WXL_E_INVALID && api.add_provider(nullptr, 1, &ignored) == WXL_E_INVALID,
              "a provider missing a function is refused");
        Check(api.exists(Str("interface/P/A.TXT")) == WXL_TRUE && api.exists(Str("Interface\\P\\None.txt")) == WXL_FALSE,
              "exists sees a provider's files, in any case and with either slash");
        Check(ReadAll(api, "Interface\\P\\A.txt") == "from high", "the client reads a provider's file through its file stack");
        Check(ReadAll(api, "World\\Base.txt") == "high's base", "a provider above 0 wins a name over the client's own");
        Check(ReadAll(api, "Interface\\P\\Fill.txt") == "from low", "one at 0 or below fills a name the client lacks");
        Check(high.closes.load() == high.opens.load() && low.closes.load() == low.opens.load(),
              "and every file a provider opened is closed when the client closes it");

        {
            const wxl_handle file = api.open(Str("Interface\\P\\A.txt"));
            char             piece[8] = {};
            uint64_t         read     = 0;
            Check(file != WXL_HANDLE_NONE && api.read_at(file, 5, piece, 4, &read) == WXL_OK && read == 4 &&
                      std::string(piece, 4) == "high",
                  "a provider's file is read at an offset, a piece at a time");
            Check(api.read_at(file, 0, piece, 4, &read) == WXL_OK && std::string(piece, 4) == "from",
                  "and from the start again");
            api.close(file);
        }

        // Many threads at once: every read whole and right. The harness's own recorder of
        // events is not made for threads, so it hears nothing meanwhile.
        {
            Mute(WXL_EV_FileOpen);
            Mute(WXL_EV_FileOpened);
            std::atomic<int>         wrong{0};
            std::vector<std::thread> loaders;
            for (int t = 0; t < 8; ++t)
                loaders.emplace_back([&] {
                    for (int i = 0; i < 100; ++i)
                        if (ReadAll(api, i % 2 ? "Interface\\P\\A.txt" : "Interface\\P\\Fill.txt") !=
                            (i % 2 ? "from high" : "from low"))
                            ++wrong;
                });
            for (std::thread& loader : loaders) loader.join();
            Unmute(WXL_EV_FileOpen);
            Unmute(WXL_EV_FileOpened);
            Check(wrong.load() == 0, "eight threads reading providers' files at once each read them whole");
        }

        // Taken back: the client's files come back; a file still open reads as an error.
        void* held = FakeFileOpen("Interface\\P\\A.txt");
        Check(held != nullptr, "the client holds a provider's file open");
        const int closedBefore = high.closes.load();
        Check(api.remove_provider(lowId) == WXL_OK && api.remove_provider(lowId) == WXL_E_NOT_FOUND,
              "a provider is taken back once; again is not found");
        Check(api.remove_provider(highId + 1000) == WXL_E_NOT_FOUND, "a handle never given is not found");
        constexpr uint32_t kOther = 9;
        const uint32_t previous = g_host->SwapCurrentMod(kOther);
        Check(api.remove_provider(highId) == WXL_E_NOT_FOUND, "another mod cannot take a provider back");
        g_host->SwapCurrentMod(previous);
        assets::ReleaseOf(0);
        Check(high.closes.load() == closedBefore + 1, "a mod that goes has its provider close the files the client holds");
        char into[4];
        Check(FakeFileRead(held, into, 4) == 0, "and those files read nothing from then on");
        FakeFileClose(held);
        Check(high.closes.load() == closedBefore + 1, "the client's close then frees what is left, closing nothing twice");
        Check(ReadAll(api, "World\\Base.txt") == "the client's" && api.exists(Str("Interface\\P\\A.txt")) == WXL_FALSE,
              "with its providers gone, the client's own files answer again");

        // "Not yet": the name is looked for further down, and a mod's read is told so.
        {
            FakeProvider later;
            later.pending = {"world\\base.txt", "interface\\p\\soon.txt"};
            uint64_t id   = 0;
            api.add_provider(later.Table(), 4, &id);
            Check(ReadAll(api, "World\\Base.txt") == "the client's",
                  "a name a provider has not yet is the client's own meanwhile");
            uint64_t size = 0;
            char     none[4];
            uint64_t read = 0;
            Check(api.size(Str("Interface\\P\\Soon.txt"), &size) == WXL_E_PENDING &&
                      api.read(Str("Interface\\P\\Soon.txt"), none, sizeof none, &read) == WXL_E_PENDING &&
                      api.exists(Str("Interface\\P\\Soon.txt")) == WXL_FALSE,
                  "and one nobody else has reads as WXL_E_PENDING for a mod, absent for the client");
            Check(api.size(Str("Interface\\P\\Never.txt"), &size) == WXL_E_NOT_FOUND,
                  "while a name no provider waits for is not found");
            api.remove_provider(id);
        }

        // A provider taking itself back from inside its own call is refused.
        {
            FakeProvider self;
            uint64_t     id = 0;
            api.add_provider(self.Table(), 1, &id);
            self.removeFromInside = id;
            api.exists(Str("Interface\\Anything.txt"));
            Check(self.removedFromInside == WXL_E_INVALID, "a provider cannot take itself back from inside its own call");
            self.removeFromInside = 0;
            api.remove_provider(id);
        }

        // A provider that faults is never called again.
        {
            FakeProvider faulty;
            faulty.files         = {{"world\\base.txt", "never"}};
            faulty.faultOnExists = true;
            uint64_t id          = 0;
            api.add_provider(faulty.Table(), 3, &id);
            Check(ReadAll(api, "World\\Base.txt") == "the client's", "a provider that faults answers nothing");
            faulty.faultOnExists = false;
            Check(ReadAll(api, "World\\Base.txt") == "the client's", "and is not asked again");
            api.remove_provider(id);
        }

        // Served bytes.
        const char hello[] = "served here";
        Check(api.serve(Str("Interface\\Served.txt"), hello, 11, 1) == WXL_OK &&
                  ReadAll(api, "interface\\served.txt") == "served here",
              "bytes a mod serves are read as one of the client's files");
        Check(api.serve(Str("World\\Base.txt"), "x", 1, 0) == WXL_OK && ReadAll(api, "World\\Base.txt") == "the client's",
              "served at 0, a name the client has stays the client's");
        Check(api.serve(Str("Interface\\Served.txt"), "again", 5, 1) == WXL_OK &&
                  ReadAll(api, "Interface\\Served.txt") == "again",
              "serving a name again replaces the mod's bytes");
        Check(api.unserve(Str("Interface\\Served.txt")) == WXL_OK && api.unserve(Str("Interface\\Served.txt")) == WXL_E_NOT_FOUND &&
                  api.exists(Str("Interface\\Served.txt")) == WXL_FALSE,
              "unserved, the name is gone; unserving again is not found");
        Check(api.serve(Str(""), "x", 1, 1) == WXL_E_INVALID && api.serve(Str(std::string(241, 'a').c_str()), "x", 1, 1) == WXL_E_INVALID,
              "an empty name or one past 240 bytes is refused");
        api.unserve(Str("World\\Base.txt"));

        // Transforms: chained by priority, sizes changing, filtered by magic.
        const std::string bang = "!";
        int               untouched = 0;
        wxl_asset_transform upper{sizeof(wxl_asset_transform), nullptr, Str("TXT"), 0, &Upper};
        wxl_asset_transform mark{sizeof(wxl_asset_transform), const_cast<std::string*>(&bang), Str(".txt"), 0, &Mark};
        wxl_asset_transform byMagic{sizeof(wxl_asset_transform), const_cast<std::string*>(&bang), Str("bin"),
                                    0x3032444Du /* MD20 */, &Mark};
        wxl_asset_transform none{sizeof(wxl_asset_transform), &untouched, Str("txt"), 0, &Untouched};
        uint64_t upperId = 0, markId = 0, magicId = 0, noneId = 0;
        Check(api.add_transform(&mark, 5, &markId) == WXL_OK && api.add_transform(&upper, 10, &upperId) == WXL_OK &&
                  api.add_transform(&byMagic, 0, &magicId) == WXL_OK && api.add_transform(&none, 1, &noneId) == WXL_OK,
              "transforms join, by kind and priority");
        wxl_asset_transform noKind{sizeof(wxl_asset_transform), nullptr, Str(""), 0, &Upper};
        Check(api.add_transform(&noKind, 1, &ignored) == WXL_E_INVALID, "a transform with no kind is refused");
        Check(ReadAll(api, "World\\Base.txt") == "THE CLIENT'S!",
              "the highest runs first and the next gets what it left, the file growing by a byte");
        Check(untouched == 1, "a transform that answers nothing leaves the bytes as they came");
        Check(ReadAll(api, "World\\Model.bin") == "MD20 model!" && ReadAll(api, "World\\Other.bin") == "MD21 model",
              "a transform by magic sees only the files that start with it");
        Check(api.exists(Str("World\\Base.txt")) == WXL_TRUE && api.exists(Str("World\\Nothing.txt")) == WXL_FALSE,
              "a transformed kind still exists only where a file is");
        {
            FakeProvider source;
            source.files = {{"interface\\p\\t.txt", "provided"}};
            uint64_t id  = 0;
            api.add_provider(source.Table(), 2, &id);
            Check(ReadAll(api, "Interface\\P\\T.txt") == "PROVIDED!", "a provider's file is transformed too");
            api.remove_provider(id);
        }
        Check(api.remove_transform(upperId) == WXL_OK && ReadAll(api, "World\\Base.txt") == "the client's!",
              "a transform taken back no longer runs");
        assets::ReleaseOf(0);
        Check(!assets::Active() && ReadAll(api, "World\\Base.txt") == "the client's",
              "a mod that goes takes its transforms with it");

        // A folder's listing: the providers' entries join the disk's.
        {
            FakeProvider addons;
            addons.folders = {{"interface\\addons", {{"MyAddOn", true}}},
                              {"interface\\addons\\myaddon", {{"MyAddOn.toc", false}, {"Main.lua", false}}}};
            addons.files   = {{"interface\\addons\\myaddon\\myaddon.toc", "## Title: Mine"}};
            uint64_t id    = 0;
            api.add_provider(addons.Table(), 1, &id);
            g_basePath = "C:\\Game\\";
            assets::Install();
            fsk::FileParms parms{};
            parms.verb        = offsetof(fsk::Filesystem, verbs.dirwalk);
            std::string where = "C:\\Game\\Interface\\AddOns";
            parms.path        = where.data();
            parms.dircallback = &Walked;
            g_walked.clear();
            TopVerb(parms.verb)(static_cast<fsk::Filesystem*>(g_fakeTop), &parms);
            Check(g_walked.size() == 1 && g_walked[0] == "MyAddOn\\",
                  "a listing of the add-ons folder tells a provider's add-on as a folder of the disk");

            // The client then asks each entry's info by its path (OsFileList keeps the folders,
            // which the add-on list reads), and may ask whether it exists.
            const auto info = [](const char* path, fsk::FileInfo& out) {
                fsk::FileParms asked{};
                asked.verb = offsetof(fsk::Filesystem, verbs.getfileinfo);
                std::string named(path);
                asked.path = named.data();
                asked.info = &asked.info2;
                const bool answered = TopVerb(asked.verb)(static_cast<fsk::Filesystem*>(g_fakeTop), &asked);
                if (answered) out = *asked.info;
                return answered;
            };
            fsk::FileInfo got{};
            Check(info("C:\\Game\\Interface\\AddOns\\MyAddOn", got) && got.existence == 2 && (got.attributes & 0x40),
                  "the info of a provider's listed folder says a folder");
            Check(info("C:\\Game\\Interface\\AddOns\\myaddon\\MYADDON.toc", got) && got.existence == 1 &&
                      !(got.attributes & 0x40) && got.regular_file,
                  "and of a file it lists, a file, whatever the case");
            Check(!info("C:\\Game\\Interface\\AddOns\\Other", got) && !info("D:\\Elsewhere\\MyAddOn", got),
                  "a name no provider lists, or outside the client's folder, is the disk's answer");
            fsk::FileParms exists{};
            exists.verb        = offsetof(fsk::Filesystem, verbs.exists);
            std::string folder = "Interface\\AddOns\\MyAddOn";
            exists.path        = folder.data();
            exists.info        = &exists.info2;
            Check(TopVerb(exists.verb)(static_cast<fsk::Filesystem*>(g_fakeTop), &exists) && exists.info->existence == 2,
                  "a provider's listed folder exists, asked by a path relative to the client's folder");
            std::vector<wxl_str> names(8);
            const uint32_t       count = api.find(Str("Interface\\AddOns\\*.toc"), names.data(), 8);
            Check(count == 1 && std::string(names[0].data, names[0].size) == "Interface\\AddOns\\MyAddOn\\MyAddOn.toc",
                  "find walks a provider's folders");
            api.remove_provider(id);
            g_basePath.clear();
        }

        assets::ReleaseAll();
        assets::ForgetInstall();
        g_files.clear();
    }

    // --- the asset bricks: names nobody has, content hashes, overrides, reloads, the relay ------

    /// A FileMissing as a handler saw it, its strings copied while they were valid.
    struct Missing
    {
        std::string path;
        uint32_t    kind    = 0;
        bool        open    = false;
        bool        pending = false;
    };

    std::vector<Missing> g_missing;

    void WXL_CALL OnFileMissing(void*, const void* data)
    {
        const auto& args = *static_cast<const wxl_file_missing_args*>(data);
        Missing     seen;
        seen.path    = std::string(args.path.data, args.path.size);
        seen.kind    = args.kind;
        seen.open    = args.open == WXL_TRUE;
        seen.pending = args.pending == WXL_TRUE;
        g_missing.push_back(seen);
    }

    std::string Hex(const uint8_t (&md5)[16])
    {
        static const char kDigits[] = "0123456789abcdef";
        std::string       text;
        for (uint8_t byte : md5)
        {
            text += kDigits[byte >> 4];
            text += kDigits[byte & 0xF];
        }
        return text;
    }

    struct HashAnswer
    {
        std::atomic<int> calls{0};
        wxl_result       result = WXL_E_FAILED;
        std::string      md5;
        uint32_t         layer = 0;
    };

    void WXL_CALL OnHashed(void* user, uint64_t, wxl_result result, const wxl_file_hash* hash)
    {
        HashAnswer& answer = *static_cast<HashAnswer*>(user);
        answer.result      = result;
        if (hash)
        {
            answer.md5   = Hex(hash->md5);
            answer.layer = hash->layer;
        }
        ++answer.calls;
    }

    /// Pumps the main thread's queue until @p done or a second passes.
    template <class Done>
    bool PumpUntil(Done done)
    {
        for (int i = 0; i < 200 && !done(); ++i)
        {
            wxl::core::services::hashes::Pump();
            wxl::core::services::relay::Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return done();
    }

    // A service published through wxl.relay, and a caller of it.
    struct FakeService
    {
        std::vector<wxl_request> received;
        std::vector<std::string> methods;
        std::vector<wxl_request> cancelled;
        bool                     answerAtOnce = false;
        wxl_relay_endpoint       table{};

        static void WXL_CALL Request(void* user, wxl_request request, wxl_str method, const wxl_value* args,
                                     uint32_t count)
        {
            FakeService& self = *static_cast<FakeService*>(user);
            self.received.push_back(request);
            self.methods.push_back(std::string(method.data, method.size));
            if (!self.answerAtOnce) return;
            // An echo of its first argument and a number.
            wxl_value values[2]{};
            if (count && args[0].type == WXL_VALUE_STRING) values[0] = args[0];
            values[1].type      = WXL_VALUE_NUMBER;
            values[1].as.number = 42;
            wxl::core::services::RelayApi().answer(request, values, 2);
        }

        static void WXL_CALL Cancelled(void* user, wxl_request request)
        {
            static_cast<FakeService*>(user)->cancelled.push_back(request);
        }

        const wxl_relay_endpoint* Table()
        {
            table = wxl_relay_endpoint{sizeof table, this, &Request, &Cancelled};
            return &table;
        }
    };

    struct Reply
    {
        int         calls  = 0;
        wxl_result  result = WXL_E_UNSUPPORTED;
        std::string first;
        double      number = 0;
    };

    void WXL_CALL OnReply(void* user, wxl_request, wxl_result result, const wxl_value* values, uint32_t count)
    {
        Reply& reply = *static_cast<Reply*>(user);
        ++reply.calls;
        reply.result = result;
        if (count && values[0].type == WXL_VALUE_STRING) reply.first.assign(values[0].as.string.data, values[0].as.string.size);
        if (count > 1 && values[1].type == WXL_VALUE_NUMBER) reply.number = values[1].as.number;
    }

    // --- wxl.texture: textures made from an SVG or a PNG, served to the client ----------------------

    /// A file as the client reads it: opened through its file stack, read whole; "<none>" when
    /// it does not open.
    std::vector<uint8_t> ClientRead(const char* path)
    {
        void* file = FakeFileOpen(path);
        if (!file) return {};
        std::vector<uint8_t> bytes(static_cast<size_t>(FakeFileSize(file)));
        const uint32_t       got = bytes.empty() ? 0 : FakeFileRead(file, bytes.data(), static_cast<uint32_t>(bytes.size()));
        FakeFileClose(file);
        bytes.resize(got);
        return bytes;
    }

    /// A pixel of a BLP's first mip, straight: 0xAARRGGBB, or 0 when there is none.
    uint32_t BlpPixel(const std::vector<uint8_t>& blp, uint32_t x, uint32_t y)
    {
        uint32_t       w = 0, h = 0;
        const uint8_t* bgra = nullptr;
        if (!wxl::core::imaging::BlpMip(blp, 0, &w, &h, &bgra) || x >= w || y >= h) return 0;
        const uint8_t* p = bgra + (static_cast<size_t>(y) * w + x) * 4;
        return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
    }

    wxl_texture_group TextureGroup(const char* id, uint32_t set, uint32_t color, float width, bool visible)
    {
        wxl_texture_group group{};
        group.struct_size = sizeof group;
        group.id          = Str(id);
        group.set         = set;
        group.color       = color;
        group.width       = width;
        group.visible     = visible ? WXL_TRUE : WXL_FALSE;
        return group;
    }

    wxl_result MakeTexture(const char* source, uint32_t w, uint32_t h, std::vector<wxl_texture_group> groups, uint32_t tint,
                           wxl_texture& out)
    {
        wxl_texture_request request{};
        request.struct_size = sizeof request;
        request.source      = Str(source);
        request.width       = w;
        request.height      = h;
        request.groups      = groups.empty() ? nullptr : groups.data();
        request.group_count = static_cast<uint32_t>(groups.size());
        request.tint        = tint;
        out                 = wxl_texture{};
        out.struct_size     = sizeof out;
        return wxl::core::services::TextureApi().make(&request, &out);
    }

    void CheckTexture()
    {
        namespace fs      = std::filesystem;
        namespace texture = wxl::core::services::texture;
        const wxl_texture_api& api     = wxl::core::services::TextureApi();
        const wxl_storage_api& storage = wxl::core::services::StorageApi();
        Check(api.struct_size == sizeof(wxl_texture_api) && api.make && api.stats, "wxl.texture has the layout texture.h declares");

        BuildFakeStack();
        g_files.clear();
        texture::Clear();
        Mute(WXL_EV_FileOpen);
        Mute(WXL_EV_FileOpened);

        // The owner's reticle shape: an outline group under a fill group, centred on (10, 10.5).
        const std::string reticle =
            "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='21' viewBox='0 0 20 21'>"
            "<g id='outline'><circle cx='10' cy='10.5' r='3' fill='#17120C' stroke='#17120C' stroke-width='2' "
            "fill-opacity='0.6' stroke-opacity='0.6'/></g>"
            "<g id='fill'><circle cx='10' cy='10.5' r='3' fill='#F6F6EE'/></g></svg>";
        Check(storage.serve(Str("Interface\\AddOns\\WxlTest\\Reticle.svg"), reticle.data(), static_cast<uint32_t>(reticle.size()),
                            0) == WXL_OK,
              "texture: an add-on's SVG served to read from");

        // --- made, named, served ---
        wxl_texture made{};
        const std::vector<wxl_texture_group> red = {
            TextureGroup("fill", WXL_TEXTURE_SET_COLOR, 0xFFFF4040u, 0, true),
            TextureGroup("outline", WXL_TEXTURE_SET_COLOR | WXL_TEXTURE_SET_WIDTH, 0x99000000u, 2.0f, true)};
        Check(MakeTexture("Interface/AddOns/WxlTest/Reticle.svg", 40, 42, red, 0, made) == WXL_OK && !made.reused &&
                  std::strncmp(made.path, "Interface\\WXL\\WxlTest\\Reticle_40x42_", 35) == 0 &&
                  std::strcmp(made.path + std::strlen(made.path) - 4, ".blp") == 0,
              "texture: made under Interface\\WXL\\<add-on>\\<name>_<w>x<h>_<hash>.blp");
        Check(made.width == 40 && made.height == 42 && made.texture_width == 64 && made.texture_height == 64 &&
                  made.right == 40.0f / 64.0f && made.bottom == 42.0f / 64.0f,
              "texture: a 64 x 64 texture, the picture's corner at 40/64, 42/64");
        const std::string first = made.path;
        const std::vector<uint8_t> blp = ClientRead(first.c_str());
        Check(blp.size() == wxl::core::imaging::kBlpHeaderSize + (4096 + 1024 + 256 + 64 + 16 + 4 + 1) * 4u &&
                  std::memcmp(blp.data(), "BLP2", 4) == 0 && blp[8] == 3 && blp[9] == 8,
              "texture: the client reads it through its file stack: a raw BLP2, eight bits of alpha, seven levels");
        // At twice the drawing's size the centre (20, 21) is the fill, recoloured; the outline at
        // twice its width (4 units, from 1 to 5) covers column 28 (4.25 units out) at 0x99, dark.
        const uint32_t centre = BlpPixel(blp, 20, 21), ring = BlpPixel(blp, 28, 21), beyond = BlpPixel(blp, 31, 21);
        Check(centre == 0xFFFF4040u, "texture: the fill group in its colour, ff4040");
        Check((ring >> 24) >= 0x99 - 12 && (ring >> 24) <= 0x99 + 2 && (ring & 0xFFFFFFu) == 0,
              "texture: the outline group in its colour (black, alpha 0x99) at its width");
        Check((beyond >> 24) < 16, "texture: nothing past the outline's width");
        Check(BlpPixel(blp, 50, 50) >> 24 == 0, "texture: the padding is transparent");

        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Reticle.svg", 40, 42, {red[1], red[0]}, 0, made) == WXL_OK &&
                  made.reused && first == made.path,
              "texture: the same request, its groups in any order, answers the same path without drawing again");

        const std::vector<wxl_texture_group> thin = {
            TextureGroup("fill", WXL_TEXTURE_SET_COLOR, 0xFFFF4040u, 0, true),
            TextureGroup("outline", WXL_TEXTURE_SET_COLOR | WXL_TEXTURE_SET_WIDTH, 0x99000000u, 0.0f, true)};
        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Reticle.svg", 40, 42, thin, 0, made) == WXL_OK && first != made.path,
              "texture: another width is another path");
        const std::string noOutline = made.path;
        const std::vector<uint8_t> bare = ClientRead(noOutline.c_str());
        Check(BlpPixel(bare, 26, 21) >> 24 == 0 && BlpPixel(bare, 20, 21) == 0xFFFF4040u,
              "texture: width 0 takes the outline off, the fill stays");
        Check(!ClientRead(first.c_str()).empty(), "texture: the earlier texture is still served");

        const std::vector<wxl_texture_group> blue = {TextureGroup("fill", WXL_TEXTURE_SET_COLOR, 0xFF4040FFu, 0, true),
                                                     TextureGroup("outline", WXL_TEXTURE_SET_VISIBLE, 0, 0, false)};
        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Reticle.svg", 40, 42, blue, 0, made) == WXL_OK && first != made.path &&
                  BlpPixel(ClientRead(made.path), 20, 21) == 0xFF4040FFu && BlpPixel(ClientRead(made.path), 26, 21) >> 24 == 0,
              "texture: another colour is another path; a hidden group draws nothing");

        // Natural size, and one edge in proportion.
        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Reticle.svg", 0, 0, {}, 0, made) == WXL_OK && made.width == 20 &&
                  made.height == 21 && made.texture_width == 32 && made.texture_height == 32,
              "texture: 0 x 0 is the SVG's own size");
        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Reticle.svg", 40, 0, {}, 0, made) == WXL_OK && made.width == 40 &&
                  made.height == 42,
              "texture: one edge given, the other in proportion");

        // The source changes: a new path; the old one still answers what it was.
        const std::string changed = reticle.substr(0, reticle.size() - 6) + "<rect x='0' y='0' width='1' height='1'/></svg>";
        storage.serve(Str("Interface\\AddOns\\WxlTest\\Reticle.svg"), changed.data(), static_cast<uint32_t>(changed.size()), 0);
        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Reticle.svg", 40, 42, red, 0, made) == WXL_OK && !made.reused &&
                  first != made.path && ClientRead(first.c_str()) == blp,
              "texture: a source that changed makes a new path; the old path still reads as it was");

        // --- a PNG ---
        static const uint8_t png[] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
            0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x13, 0x49,
            0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x0c, 0x81, 0x34, 0x08, 0x30, 0x00, 0x00, 0x48,
            0xc9, 0x08, 0xf8, 0x71, 0xc5, 0x31, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
        storage.serve(Str("Interface\\AddOns\\WxlTest\\Dots.png"), png, sizeof png, 0);
        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Dots.png", 0, 0, {}, 0, made) == WXL_OK && made.width == 2 &&
                  made.texture_width == 8 && made.right == 0.25f,
              "texture: a PNG at its own size, in the smallest texture the client makes (8 x 8)");
        const std::vector<uint8_t> dots = ClientRead(made.path);
        Check(BlpPixel(dots, 0, 0) == 0xFFFF0000u && BlpPixel(dots, 1, 0) == 0xFF00FF00u && BlpPixel(dots, 0, 1) == 0xFF0000FFu &&
                  BlpPixel(dots, 1, 1) >> 24 == 0,
              "texture: its pixels as they were");
        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Dots.png", 0, 0, {}, 0x80FFFFFFu, made) == WXL_OK &&
                  BlpPixel(ClientRead(made.path), 0, 0) == 0x80FF0000u,
              "texture: a tint multiplies it");
        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Dots.png", 0, 0, {red[0]}, 0, made) == WXL_E_INVALID &&
                  std::strstr(made.reason, "is a PNG, which has no groups") != nullptr,
              "texture: groups on a PNG are refused, and say a tint colours it");

        // --- a mod's folder ---
        const fs::path root = fs::temp_directory_path() / "wxl-harness-texture";
        std::error_code ignored;
        fs::remove_all(root, ignored);
        WriteFileAt(root / "Mods" / "wxl.reticles" / "assets" / "dot.svg", reticle);
        texture::UseModsDirectory(root / "Mods");
        Check(MakeTexture("Mods\\wxl.reticles\\assets\\dot.svg", 16, 0, {}, 0, made) == WXL_OK &&
                  std::strncmp(made.path, "Interface\\WXL\\wxl.reticles\\dot_16x17_", 37) == 0,
              "texture: a file of a mod's folder, named after the mod's folder");
        Check(MakeTexture(".\\assets\\dot.svg", 16, 0, {}, 0, made) == WXL_E_INVALID &&
                  std::strstr(made.reason, "no mod is calling") != nullptr,
              "texture: \".\\\\\" with no mod calling is refused, and says how an add-on names its file");

        // --- refusals ---
        Check(MakeTexture("Mods\\wxl.reticles\\assets\\none.svg", 16, 16, {}, 0, made) == WXL_E_NOT_FOUND &&
                  std::strstr(made.reason, "no file") != nullptr,
              "texture: no such file is not found");
        storage.serve(Str("Interface\\AddOns\\WxlTest\\Notes.txt"), "plain words", 11, 0);
        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Notes.txt", 16, 16, {}, 0, made) == WXL_E_UNSUPPORTED &&
                  std::strstr(made.reason, "neither an SVG nor a PNG") != nullptr,
              "texture: a file neither SVG nor PNG is unsupported");
        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Reticle.svg", 4096, 16, {}, 0, made) == WXL_E_INVALID &&
                  std::strstr(made.reason, "2048") != nullptr,
              "texture: a size past 2048 is refused");
        Check(MakeTexture("Mods\\..\\Wow.exe", 16, 16, {}, 0, made) == WXL_E_INVALID && std::strstr(made.reason, "..") != nullptr &&
                  MakeTexture("C:\\x.svg", 16, 16, {}, 0, made) == WXL_E_INVALID,
              "texture: a name going up a folder, or a path of the disk, is refused");
        Check(MakeTexture("Interface\\AddOns\\WxlTest\\Reticle.svg", 16, 16, {red[0], red[0]}, 0, made) == WXL_E_INVALID &&
                  MakeTexture("Interface\\AddOns\\WxlTest\\Reticle.svg", 16, 16, {TextureGroup("", 1, 0, 0, true)}, 0, made) ==
                      WXL_E_INVALID,
              "texture: a group given twice, or with no id, is refused");

        // --- WxlTexture in the interface ---
        // The fake VM outlives its contexts, unlike the client's: the earlier contexts' globals go.
        g_vm.closures.clear();
        g_vm.live = true;
        wxl::seam::FrameScript::Initialize::After(1, 0);
        const char* clientNames[] = {"PLAYER_LOGIN"};
        wxl::seam::FrameScript::CreateEvents::After(clientNames, 1);
        const auto ranChunk = [&](const char* name, const char* function) {
            for (const std::string& chunk : g_vm.ran)
                if (chunk.find(name) == 0 && chunk.find(function) != std::string::npos) return true;
            return false;
        };
        Check(g_vm.closures.count("__wxl_texture") == 1 &&
                  ranChunk("WarcraftXL\\WxlTexture: ", "function WxlTexture(source, width, height, groups, tint)"),
              "texture: a context coming up gets WxlTexture, run before the mods hear of it");
        {
            // WxlDraw beside it: the native half draws a shape for the next frame, and answers why not.
            bool drawFound = false;
            Check(g_vm.closures.count("__wxl_draw") == 1 && ranChunk("WarcraftXL\\WxlDraw: ", "function WxlDraw(shape, style)"),
                  "draw: a context coming up gets WxlDraw");
            std::vector<FakeValue> args(22, Number(0));
            args[0]  = Number(1);  // a circle
            args[4]  = Number(3);  // its radius
            args[14] = Number(1);  // flat
            args[15] = String("");
            args[16] = Number(0xFF0000);
            args[17] = Number(1);
            args[19] = Number(2);
            std::vector<FakeValue> drawn = CallGlobal("__wxl_draw", args, &drawFound);
            Check(drawFound && drawn.size() == 1 && drawn[0].type == WXL_VALUE_BOOL, "draw: WxlDraw's native half draws a circle");
            args[4] = Number(0);
            drawn   = CallGlobal("__wxl_draw", args, &drawFound);
            Check(drawn.size() == 2 && drawn[0].type == WXL_VALUE_NIL && drawn[1].string.find("invalid") == 0,
                  "draw: a circle of no radius answers nil and why");
            // abi 0.1.38's arguments: a textured, modulated, feathered inside and a shadowed line.
            args.resize(43, Number(0));
            args[4]  = Number(3);
            args[18] = Number(0.8);
            args[22] = Number(-1);
            args[24] = Number(-1);
            args[25] = Number(-1);
            args[27] = Number(0.5);
            args[28] = Number(1.5);
            args[29] = Number(1.5);
            args[31] = String("SPELLS\\CrackedGround.blp");
            args[40] = Number(2);
            args[41] = Number(0.5);
            args.resize(44, Number(0));
            args[43] = Number(0.35);
            drawn    = CallGlobal("__wxl_draw", args, &drawFound);
            Check(drawn.size() == 1 && drawn[0].type == WXL_VALUE_BOOL, "draw: WxlDraw's native half takes a texture, a blend, a feather and a shadow");
            args[40] = Number(9);
            drawn    = CallGlobal("__wxl_draw", args, &drawFound);
            Check(drawn.size() == 2 && drawn[0].type == WXL_VALUE_NIL, "draw: a blend that is none answers nil and why");
            wxl::core::services::draw::Clear();
        }
        bool found = false;
        storage.serve(Str("Interface\\AddOns\\WxlTest\\Reticle.svg"), reticle.data(), static_cast<uint32_t>(reticle.size()), 0);
        std::vector<FakeValue> results =
            CallGlobal("__wxl_texture",
                       {String("Interface\\AddOns\\WxlTest\\Reticle.svg"), Number(40), Number(42),
                        String("fill\tff4040ff\t\t\noutline\t@2566914048\t2\t"), FakeValue{}},
                       &found);
        Check(found && results.size() == 5 && results[0].string == first && results[1].number == 40.0 / 64.0 &&
                  results[3].number == 64.0,
              "texture: the interface's call answers the path a mod's same request does, then the coordinates and sizes");
        results = CallGlobal("__wxl_texture",
                             {String("Interface\\AddOns\\WxlTest\\Reticle.svg"), Number(40), Number(42),
                              String("outline\t\t0\t\nfill\t#ff4040\t\t"), FakeValue{}},
                             &found);
        Check(results.size() == 5 && results[0].string != first, "texture: \"#rrggbb\" and a width alone read");
        results = CallGlobal("__wxl_texture",
                             {String("Interface\\AddOns\\WxlTest\\Reticle.svg"), Number(40), Number(42), String("fill\tred\t\t"),
                              FakeValue{}},
                             &found);
        Check(results.size() == 2 && results[0].type == WXL_VALUE_NIL &&
                  results[1].string.find("group 'fill': \"red\" is not a colour") != std::string::npos,
              "texture: a bad colour answers nil and a reason naming the group");
        results = CallGlobal("__wxl_texture", {String(".\\x.svg"), Number(8), Number(8), String(""), FakeValue{}}, &found);
        Check(results.size() == 2 && results[1].string.find("no mod is calling") != std::string::npos,
              "texture: an add-on has no \".\\\\\" folder");
        results = CallGlobal("__wxl_texture", {Number(3)}, &found);
        Check(results.size() == 2 && results[1].string.find("'source' (argument 1)") != std::string::npos,
              "texture: a source that is not a name is refused");
        wxl::seam::FrameScript::Destroy::Before();
        g_vm.live = false;
        g_vm.closures.clear();

        // --- the budget ---
        wxl_texture_stats stats{};
        stats.struct_size = sizeof stats;
        Check(api.stats(&stats) == WXL_OK && stats.textures >= 8 && stats.resident == stats.textures && stats.reused >= 2 &&
                  stats.bytes > 0 && stats.byte_limit == (64ull << 20),
              "texture: stats count the textures, the ones in memory, their bytes and the budget");
        const uint64_t madeBefore = stats.made;
        texture::SetLimits(1, 2048);
        api.stats(&stats);
        Check(stats.resident == 0 && stats.bytes == 0 && stats.released >= 8 && stats.textures >= 8,
              "texture: past the budget the bytes are let go, the names kept");
        Check(ClientRead(first.c_str()) == blp, "texture: a name let go is drawn again when the client asks, the same bytes");
        api.stats(&stats);
        Check(stats.made == madeBefore + 1, "texture: once");
        texture::SetLimits(64ull << 20, 1);
        api.stats(&stats);
        Check(stats.textures == 1 && ClientRead(noOutline.c_str()).empty(),
              "texture: past the count the least used names are forgotten, and no longer answer");

        texture::Clear();
        texture::UseModsDirectory(fs::path());
        Check(ClientRead(first.c_str()).empty(), "texture: cleared, nothing is served");
        for (const char* name : {"Interface\\AddOns\\WxlTest\\Reticle.svg", "Interface\\AddOns\\WxlTest\\Dots.png",
                                 "Interface\\AddOns\\WxlTest\\Notes.txt"})
            storage.unserve(Str(name));
        fs::remove_all(root, ignored);
        Unmute(WXL_EV_FileOpen);
        Unmute(WXL_EV_FileOpened);
    }

    void CheckAssetBricks()
    {
        namespace assets = wxl::core::services::assets;
        const wxl_storage_api& api = wxl::core::services::StorageApi();
        Check(api.struct_size >= offsetof(wxl_storage_api, reload_asset) + sizeof(void*),
              "wxl.storage carries hashes, overrides and reloads");

        BuildFakeStack();
        g_files.clear();
        g_files.push_back(FakeFile{"World\\Base.txt", "the client's", 0, false});
        const wxl_subscription heard = g_host->Events().Subscribe(WXL_EV_FileMissing, &OnFileMissing, nullptr, 0);
        Mute(WXL_EV_FileOpen);
        Mute(WXL_EV_FileOpened);

        // --- FileMissing ---
        g_missing.clear();
        void* none = FakeFileOpen("Creature\\Nowhere\\Nowhere.m2");
        Check(!none && g_missing.size() == 1 && g_missing[0].path == "Creature\\Nowhere\\Nowhere.m2" &&
                  g_missing[0].kind == WXL_ASSET_MODEL && g_missing[0].open && !g_missing[0].pending,
              "an open no layer answers raises FileMissing: the name, a model, an open");
        g_missing.clear();
        Check(FakeFileExists("Interface\\None.toc") == WXL_FALSE && g_missing.size() == 1 && !g_missing[0].open &&
                  g_missing[0].kind == WXL_ASSET_INTERFACE,
              "so does an existence check, told apart from an open");
        g_missing.clear();
        void* base = FakeFileOpen("World\\Base.txt");
        FakeFileClose(base);
        Check(base && g_missing.empty(), "a file some layer has raises nothing");
        Check(api.exists(Str("Creature\\Nowhere\\Nowhere.m2")) == WXL_FALSE && ReadAll(api, "World\\Nothing.txt") == "<none>" &&
                  g_missing.empty(),
              "nor does a mod's own wxl.storage call");
        {
            FakeProvider later;
            later.pending = {"creature\\later\\later.m2"};
            uint64_t id   = 0;
            api.add_provider(later.Table(), 0, &id);
            Check(!FakeFileOpen("Creature\\Later\\Later.m2") && g_missing.size() == 1 && g_missing[0].pending,
                  "a name a provider answers \"not yet\" for is missing and pending");
            api.remove_provider(id);
        }
        // --- a path of the mod's own, beside the client's ---
        Check(api.exists(Str("World\\Mine\\Base.txt")) == WXL_FALSE &&
                  api.serve(Str("World\\Mine\\Base.txt"), "mine", 4, 0) == WXL_OK &&
                  ReadAll(api, "World\\Mine\\Base.txt") == "mine" && ReadAll(api, "World\\Base.txt") == "the client's",
              "served at 0 under a new path, the mod's copy lives beside the client's file");

        // --- overrides ---
        Check(api.override_file(Str("World\\Base.txt"), Str("World\\Mine\\Base.txt")) == WXL_OK &&
                  ReadAll(api, "World\\Base.txt") == "mine",
              "an override puts the mod's copy in place of the client's file");
        Check(FakeFileExists("World\\Ghost.txt") == WXL_FALSE &&
                  api.override_file(Str("World\\Ghost.txt"), Str("World\\Base.txt")) == WXL_OK &&
                  FakeFileExists("World\\Ghost.txt") == WXL_TRUE,
              "and answers the client's existence checks too");
        {
            constexpr uint32_t kOther = 9;
            const uint32_t previous = g_host->SwapCurrentMod(kOther);
            api.serve(Str("World\\Theirs\\Base.txt"), "theirs", 6, 0);
            api.override_file(Str("World\\Base.txt"), Str("World\\Theirs\\Base.txt"));
            const std::string latest = ReadAll(api, "World\\Base.txt");
            wxl::core::services::storage::RemoveOverridesOf(kOther);
            assets::ReleaseOf(kOther);
            g_host->SwapCurrentMod(previous);
            Check(latest == "theirs" && ReadAll(api, "World\\Base.txt") == "mine",
                  "the latest of two mods' overrides wins, and its mod going uncovers the earlier");
        }
        api.unserve(Str("World\\Mine\\Base.txt"));
        g_missing.clear();
        Check(!FakeFileOpen("World\\Base.txt") && g_missing.size() == 1 && g_missing[0].path == "World\\Base.txt",
              "an override whose copy is gone leaves nothing to read, and FileMissing names the file asked for");
        Check(api.restore_file(Str("World\\Base.txt")) == WXL_OK && api.restore_file(Str("World\\Base.txt")) == WXL_E_NOT_FOUND &&
                  api.restore_file(Str("World\\Ghost.txt")) == WXL_OK && FakeFileExists("World\\Ghost.txt") == WXL_FALSE,
              "restored once; again is not found; the client's answers come back");
        Check(api.override_file(Str(""), Str("x")) == WXL_E_INVALID &&
                  api.override_file(Str(std::string(260, 'a').c_str()), Str("x")) == WXL_E_INVALID,
              "an empty name or one past 259 bytes is refused");

        // --- content hashes ---
        wxl::core::services::hashes::ForgetCache();
        wxl_file_hash hash{};
        hash.struct_size = sizeof hash;
        Check(api.hash_file(Str("World\\Base.txt"), 0, &hash) == WXL_OK && Hex(hash.md5) == "264705c71ad404bef10ffa7dfbe2a94b" &&
                  hash.size == 12 && hash.layer == WXL_FILE_LAYER_ARCHIVE &&
                  std::string(hash.source.data, hash.source.size) == "Data\\fake.MPQ",
              "an archive's file hashes to the MD5 of its bytes, its archive named");
        FileNamed("World\\Base.txt")->bytes = "THE CLIENT'S";
        Check(api.hash_file(Str("World\\Base.txt"), 0, &hash) == WXL_OK && Hex(hash.md5) == "264705c71ad404bef10ffa7dfbe2a94b",
              "a second ask answers from what was kept");
        Check(api.hash_file(Str("World\\Base.txt"), WXL_HASH_FRESH, &hash) == WXL_OK &&
                  Hex(hash.md5) == "06489070e9742e6455e883d71adf48a8",
              "WXL_HASH_FRESH reads it again");
        api.serve(Str("World\\Mine\\Base.txt"), "mine", 4, 0);
        Check(api.hash_file(Str("World\\Mine\\Base.txt"), 0, &hash) == WXL_OK && Hex(hash.md5) == "918b81db5e91d031548b963c93845e5b" &&
                  hash.layer == WXL_FILE_LAYER_SERVED,
              "served bytes hash as they are served");
        api.override_file(Str("World\\Base.txt"), Str("World\\Mine\\Base.txt"));
        Check(api.hash_file(Str("World\\Base.txt"), 0, &hash) == WXL_OK && hash.layer == WXL_FILE_LAYER_SERVED,
              "an overridden name hashes as what the client would read");
        api.restore_file(Str("World\\Base.txt"));
        api.unserve(Str("World\\Mine\\Base.txt"));
        g_missing.clear();
        Check(api.hash_file(Str("World\\Nothing.txt"), 0, &hash) == WXL_E_NOT_FOUND && g_missing.empty(),
              "a name no layer has is not found, and raises nothing");
        {
            FakeProvider later;
            later.pending = {"world\\soon.txt"};
            uint64_t id   = 0;
            api.add_provider(later.Table(), 1, &id);
            Check(api.hash_file(Str("World\\Soon.txt"), 0, &hash) == WXL_E_PENDING, "one a provider has not yet is pending");
            api.remove_provider(id);
        }
        {
            HashAnswer answer;
            uint64_t   request = 0;
            Check(api.hash_file_async(Str("World\\Base.txt"), WXL_HASH_FRESH, &OnHashed, &answer, &request) == WXL_OK &&
                      request != 0,
                  "hash_file_async answers a request at once");
            Check(PumpUntil([&] { return answer.calls.load() > 0; }) && answer.calls.load() == 1 &&
                      answer.result == WXL_OK && answer.md5 == "06489070e9742e6455e883d71adf48a8",
                  "and the hash comes later, at the main thread's pump");
            HashAnswer dropped;
            constexpr uint32_t kGoing = 9;
            const uint32_t previous = g_host->SwapCurrentMod(kGoing);
            api.hash_file_async(Str("World\\Base.txt"), WXL_HASH_FRESH, &OnHashed, &dropped, &request);
            g_host->SwapCurrentMod(previous);
            wxl::core::services::hashes::ForgetMod(kGoing);
            PumpUntil([] { return false; });
            Check(dropped.calls.load() == 0, "a request of a mod that went is never answered");
        }

        // --- reloads ---
        {
            wxl_asset_reload done{};
            done.struct_size = sizeof done;
            Check(api.reload_asset(Str("Textures\\Fake.blp"), &done) == WXL_OK && done.textures == 2 && done.busy == 1,
                  "a texture is read again in place, one still loading left alone");
            FakeObject unit;
            unit.guid   = 0x77;
            unit.typeId = WXL_OBJECT_UNIT;
            FakeObject item;
            item.guid   = 0x78;
            item.typeId = WXL_OBJECT_ITEM;
            wxl::core::objects::Service().Track(&unit);
            wxl::core::objects::Service().Track(&item);
            g_displayAgain = 0;
            done           = wxl_asset_reload{sizeof done, 0, 0, 0, 0};
            Check(api.reload_asset(Str("Character\\Character.MDX"), &done) == WXL_OK && done.models == 1 &&
                      done.objects == 1 && g_displayAgain == 1,
                  "a model is dropped from the cache and the unit drawn with it made again, not the item");
            done = wxl_asset_reload{sizeof done, 0, 0, 0, 0};
            Check(api.reload_asset(Str("Character\\Character00.skin"), &done) == WXL_OK && done.models == 1,
                  "a model's skin reloads its model");
            wxl::core::objects::Service().Untrack(&unit);
            wxl::core::objects::Service().Untrack(&item);
            Check(api.reload_asset(Str("Textures\\Loading.blp"), &done) == WXL_E_PENDING && done.busy == 1 &&
                      api.reload_asset(Str("Interface\\Icons\\Shared.blp"), &done) == WXL_E_UNSUPPORTED && done.busy == 0,
                  "a texture still loading is pending; one only on the interface's atlas cannot be read again");
            Check(api.reload_asset(Str("World\\Building.wmo"), &done) == WXL_E_UNSUPPORTED &&
                      api.reload_asset(Str("Textures\\None.blp"), &done) == WXL_E_NOT_FOUND,
                  "another kind is unsupported, a texture the client does not hold not found");
            wxl_result away = WXL_OK;
            std::thread([&] { away = api.reload_asset(Str("Textures\\Fake.blp"), &done); }).join();
            Check(away == WXL_E_INVALID, "and only the main thread may ask");
        }

        // --- the relay ---
        {
            namespace relay = wxl::core::services::relay;
            const wxl_relay_api& rapi = wxl::core::services::RelayApi();
            constexpr uint32_t kPublisher = 11, kCaller = 12;
            FakeService service;
            uint32_t    previous = g_host->SwapCurrentMod(kPublisher);
            Check(rapi.publish(Str("test.echo"), service.Table()) == WXL_OK && rapi.published(Str("TEST.echo")) == WXL_TRUE,
                  "a mod publishes a service, found in any case");
            g_host->SwapCurrentMod(kCaller);
            Check(rapi.publish(Str("test.echo"), service.Table()) == WXL_E_INVALID && rapi.unpublish(Str("test.echo")) == WXL_E_NOT_FOUND,
                  "another mod can neither take the name nor take it back");

            Reply       first;
            wxl_value   arg{};
            arg.type      = WXL_VALUE_STRING;
            arg.as.string = Str("hello");
            wxl_request request = 0;
            service.answerAtOnce = true;
            Check(rapi.call(Str("test.echo"), Str("echo"), &arg, 1, &OnReply, &first, &request) == WXL_OK && request &&
                      service.received.empty() && first.calls == 0,
                  "a call answers its number at once, and nothing is handed to anyone yet");
            g_host->SwapCurrentMod(previous);
            relay::Pump();
            Check(service.received.size() == 1 && service.methods[0] == "echo" && first.calls == 1 &&
                      first.result == WXL_OK && first.first == "hello" && first.number == 42,
                  "the pump hands the request to the service, and its answer to the caller");

            service.answerAtOnce = false;
            Reply later;
            g_host->SwapCurrentMod(kCaller);
            rapi.call(Str("test.echo"), Str("slow"), nullptr, 0, &OnReply, &later, &request);
            g_host->SwapCurrentMod(previous);
            relay::Pump();
            wxl_value value{};
            value.type      = WXL_VALUE_BOOL;
            value.as.boolean = WXL_TRUE;
            wxl_result fromThread = WXL_E_FAILED;
            std::thread([&] { fromThread = rapi.answer(request, &value, 1); }).join();
            Check(fromThread == WXL_OK && later.calls == 0, "a service answers later, from a thread of its own");
            relay::Pump();
            Check(later.calls == 1 && later.result == WXL_OK && rapi.answer(request, &value, 1) == WXL_E_NOT_FOUND,
                  "the answer comes at the next pump; a second one is refused");

            Reply failed;
            g_host->SwapCurrentMod(kCaller);
            rapi.call(Str("test.echo"), Str("bad"), nullptr, 0, &OnReply, &failed, &request);
            g_host->SwapCurrentMod(previous);
            relay::Pump();
            rapi.fail(request, Str("no such asset"));
            relay::Pump();
            Check(failed.calls == 1 && failed.result == WXL_E_FAILED && failed.first == "no such asset",
                  "a failure reaches the caller with its reason");

            Reply nobody;
            rapi.call(Str("test.nobody"), Str("x"), nullptr, 0, &OnReply, &nobody, &request);
            relay::Pump();
            Check(nobody.calls == 1 && nobody.result == WXL_E_NOT_FOUND, "a name nobody publishes fails as not found");

            Reply cancelled;
            g_host->SwapCurrentMod(kCaller);
            rapi.call(Str("test.echo"), Str("long"), nullptr, 0, &OnReply, &cancelled, &request);
            g_host->SwapCurrentMod(previous);
            relay::Pump();
            g_host->SwapCurrentMod(kPublisher);
            Check(rapi.cancel(request) == WXL_E_NOT_FOUND, "only its caller may cancel a request");
            g_host->SwapCurrentMod(kCaller);
            Check(rapi.cancel(request) == WXL_OK, "its caller takes it back");
            g_host->SwapCurrentMod(previous);
            relay::Pump();
            Check(service.cancelled.size() == 1 && service.cancelled[0] == request && rapi.answer(request, nullptr, 0) == WXL_E_NOT_FOUND &&
                      cancelled.calls == 0,
                  "the service is told, its answer refused, the caller never answered");

            Reply orphan;
            g_host->SwapCurrentMod(kCaller);
            rapi.call(Str("test.echo"), Str("orphan"), nullptr, 0, &OnReply, &orphan, &request);
            g_host->SwapCurrentMod(previous);
            relay::Pump();
            relay::ForgetMod(kCaller);
            relay::Pump();
            Check(service.cancelled.size() == 2 && orphan.calls == 0, "a caller that goes cancels its requests");

            Reply gone;
            g_host->SwapCurrentMod(kCaller);
            rapi.call(Str("test.echo"), Str("gone"), nullptr, 0, &OnReply, &gone, &request);
            g_host->SwapCurrentMod(previous);
            relay::Pump();
            relay::ForgetMod(kPublisher);
            relay::Pump();
            Check(gone.calls == 1 && gone.result == WXL_E_NOT_FOUND && rapi.published(Str("test.echo")) == WXL_FALSE,
                  "a service whose mod goes fails what it had not answered, and is gone");
            relay::Clear();
        }

        Unmute(WXL_EV_FileOpen);
        Unmute(WXL_EV_FileOpened);
        g_host->Events().Unsubscribe(heard);
        assets::ReleaseAll();
        assets::ForgetInstall();
        wxl::core::services::hashes::ForgetCache();
        g_files.clear();
    }

    // --- wxl.storage: the client's files, as a mod reads them ---------------------------------

    void CheckStorageService()
    {
        // How the binding reads Storm's answers.
        Check(wxl::core::storm::SeekLanded(0, 0) && wxl::core::storm::SeekLanded(12, 12) &&
                  !wxl::core::storm::SeekLanded(0, 12) && !wxl::core::storm::SeekLanded(0, 0x100000000ull),
              "a seek to 0 answers 0 and has landed; a cursor elsewhere, or an offset past 32 bits, has not");
        Check(std::strcmp(wxl::core::storm::ArchiveName("Data\\patch.MPQ", "x"), "Data\\patch.MPQ") == 0 &&
                  std::strcmp(wxl::core::storm::ArchiveName("", "Data\\Patch-4.MPQ\\"), "Data\\Patch-4.MPQ\\") == 0,
              "an archive is known by its name, a folder opened as one by its folder");
#if defined(_M_IX86)
        {
            uint8_t inline_string[0x1C] = {};
            std::memcpy(inline_string + 4, "Data\\", 6);
            inline_string[0x14] = 5;
            inline_string[0x18] = 15;
            const char* longText = "Data\\Patch-4.MPQ\\";
            uint8_t     heap_string[0x1C] = {};
            const uint32_t pointer = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(longText));
            std::memcpy(heap_string + 4, &pointer, 4);
            heap_string[0x18] = 31;
            Check(std::strcmp(wxl::core::storm::Msvc9String(inline_string), "Data\\") == 0 &&
                      wxl::core::storm::Msvc9String(heap_string) == longText,
                  "the stock client's std::string holds short text in place and points to longer text");
        }
#endif

        g_files.clear();
        g_files.push_back(FakeFile{"World\\Maps\\Azeroth\\Azeroth.wdt", "MVER....hello", 0, false});
        g_opens  = 0;
        g_closes = 0;

        const wxl_storage_api& api = wxl::core::services::StorageApi();
        const wxl_str          path{g_files[0].name.c_str(),
                                    static_cast<uint32_t>(g_files[0].name.size())};
        const wxl_str          missing = Str("World\\Maps\\Nowhere.wdt");

        Check(api.exists(path) == WXL_TRUE && api.exists(missing) == WXL_FALSE,
              "wxl.storage answers for a name the client has and one it does not");

        uint64_t size = 0;
        Check(api.size(path, &size) == WXL_OK && size == g_files[0].bytes.size(),
              "the length of a file is its length");
        Check(api.size(missing, &size) == WXL_E_NOT_FOUND,
              "and a file that is not there is not found, rather than empty");

        // A buffer too small answers what it would have taken, which is how the facade sizes.
        char     small[4] = {};
        uint64_t needed   = 0;
        Check(api.read(path, small, sizeof small, &needed) == WXL_E_INVALID
                  && needed == g_files[0].bytes.size(),
              "a buffer too small is refused, and says what would have fitted");

        std::vector<char> whole(static_cast<size_t>(size));
        uint64_t          read = 0;
        Check(api.read(path, whole.data(), whole.size(), &read) == WXL_OK && read == size
                  && std::string(whole.begin(), whole.end()) == g_files[0].bytes,
              "and a buffer the right size gets the whole file");

        // Streaming: a handle, a read at an offset, a close.
        const wxl_handle file = api.open(path);
        Check(file != WXL_HANDLE_NONE, "a file opens");

        char     part[5] = {};
        uint64_t got     = 0;
        Check(api.read_at(file, 8, part, 5, &got) == WXL_OK && got == 5
                  && std::string(part, 5) == "hello",
              "and reads where it is asked to, not where it last was");

        api.close(file);
        Check(g_closes == g_opens, "a closed file is closed on the client's side too");
        Check(api.read_at(file, 0, part, 1, &got) == WXL_E_NOT_FOUND,
              "a handle kept past its close answers gone, not whatever took its place");

        // What a mod leaves open is closed for it.
        const wxl_handle leaked = api.open(path);
        Check(leaked != WXL_HANDLE_NONE, "a mod opens a file and forgets it");
        wxl::core::services::storage::CloseEverything();
        Check(g_closes == g_opens, "and the runtime closes it on the way out");
        Check(api.read_at(leaked, 0, part, 1, &got) == WXL_E_NOT_FOUND,
              "its handle stops resolving with it");

        g_files.clear();
        CheckSearchPath(api);
    }

    // --- Spell: casts read off the server's messages, and the two requests a mod can refuse ---

    /// A message body, built byte by byte the way the server writes one.
    struct Body
    {
        std::vector<uint8_t> bytes;

        Body& U8(uint8_t value)
        {
            bytes.push_back(value);
            return *this;
        }

        Body& U32(uint32_t value)
        {
            for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
            return *this;
        }

        Body& U64(uint64_t value)
        {
            for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
            return *this;
        }

        Body& U16(uint16_t value)
        {
            bytes.push_back(static_cast<uint8_t>(value));
            bytes.push_back(static_cast<uint8_t>(value >> 8));
            return *this;
        }

        Body& F32(float value)
        {
            uint32_t bits = 0;
            memcpy(&bits, &value, sizeof bits);
            return U32(bits);
        }

        /// A NUL-terminated string.
        Body& Str(const char* text)
        {
            bytes.insert(bytes.end(), text, text + strlen(text) + 1);
            return *this;
        }

        /// A string as the chat carries one: its length with the NUL, then the bytes.
        Body& Sized(const char* text)
        {
            U32(static_cast<uint32_t>(strlen(text) + 1));
            return Str(text);
        }

        /// A guid packed as the server packs it: the mask, then the non-zero bytes.
        Body& Packed(uint64_t guid)
        {
            uint8_t mask = 0;
            for (int i = 0; i < 8; ++i)
                if ((guid >> (i * 8)) & 0xFF) mask |= static_cast<uint8_t>(1 << i);
            bytes.push_back(mask);
            for (int i = 0; i < 8; ++i)
                if (mask & (1 << i)) bytes.push_back(static_cast<uint8_t>(guid >> (i * 8)));
            return *this;
        }

        FakeStore Store() const { return FakeStore{ bytes.data(), static_cast<uint32_t>(bytes.size()), 0 }; }
    };

    constexpr uint32_t kMsgSpellStart   = 0x131;
    constexpr uint32_t kMsgSpellGo      = 0x132;
    constexpr uint32_t kMsgSpellFailure = 0x133;

    /// The hit and miss lists of a SpellCastGo, copied out while the args were valid.
    struct GoWatch
    {
        std::vector<uint8_t> hits;
        std::vector<uint8_t> misses;
        std::vector<uint8_t> reasons;
    };

    void WXL_CALL OnSpellGo(void* user, const void* args)
    {
        GoWatch&                   watch = *static_cast<GoWatch*>(user);
        const wxl_spell_cast_args& cast  = *static_cast<const wxl_spell_cast_args*>(args);
        const uint8_t* hits    = static_cast<const uint8_t*>(cast.hits);
        const uint8_t* misses  = static_cast<const uint8_t*>(cast.misses);
        const uint8_t* reasons = static_cast<const uint8_t*>(cast.miss_reasons);
        watch.hits.assign(hits, hits + (hits ? cast.hits_size : 0));
        watch.misses.assign(misses, misses + (misses ? cast.misses_size : 0));
        watch.reasons.assign(reasons, reasons + (reasons ? cast.miss_count : 0));
    }

    /// Eight bytes as a little-endian guid.
    uint64_t GuidAt(const std::vector<uint8_t>& bytes, size_t at)
    {
        uint64_t guid = 0;
        if (bytes.size() < at + 8) return 0;
        for (int i = 0; i < 8; ++i) guid |= static_cast<uint64_t>(bytes[at + i]) << (i * 8);
        return guid;
    }

    void CheckSpell()
    {
        FakeObject mage;
        mage.guid   = 0x0000000000000ABCull;
        mage.typeId = WXL_OBJECT_PLAYER;
        FakeObject boar;
        boar.guid   = 0xF130000000000123ull;
        boar.typeId = WXL_OBJECT_UNIT;
        wxl::seam::ObjectMgr::InitObject::After(&mage, nullptr, 3000u);
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 3001u);
        const wxl_handle mageHandle = wxl::core::objects::HandleOf(&mage);
        const wxl_handle boarHandle = wxl::core::objects::HandleOf(&boar);
        const uint64_t   stranger   = 0xF13000000000FFFFull;

        // SMSG_SPELL_START: the shared header, the cast time, then a unit target.
        Body start;
        start.Packed(0).Packed(mage.guid).U8(7).U32(133).U32(0x2).U32(1500).U32(0x2).Packed(boar.guid);
        FakeStore store = start.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_Group_30::Before(nullptr, kMsgSpellStart, 0, &store);
        const Received* started = Only(WXL_EV_SpellCastStart);
        Check(started != nullptr && Count(WXL_EV_SpellCastGo) == 0,
              "Spell.Packet_Group_30 raises SpellCastStart once for SMSG_SPELL_START");
        if (started)
        {
            const wxl_spell_cast_args args = started->As<wxl_spell_cast_args>();
            Check(args.caster == mageHandle && args.caster_guid == mage.guid,
                  "with the caster's handle and guid read off the packed guid");
            Check(args.target == boarHandle && args.target_guid == boar.guid && args.target_flags == 0x2,
                  "the object target the target flags name");
            Check(args.spell_id == 133 && args.cast_count == 7 && args.cast_flags == 0x2 &&
                      args.cast_time == 1500 && args.cast_item == 0,
                  "the spell, the count, the flags, the cast time, and no cast item");
            Check(args.hit_count == 0 && args.miss_count == 0 && args.hits == nullptr &&
                      args.misses == nullptr && args.miss_reasons == nullptr,
                  "and no hits or misses on a start");
        }
        Check(store.read == 0u, "reading the message left the store's cursor where it was");

        // SMSG_SPELL_GO: the header, the server's time, one hit, two misses, the first of them
        // reflected, no target.
        Body go;
        go.Packed(0x4000000000000042ull).Packed(stranger).U8(2).U32(5176).U32(0x800).U32(9999)
            .U8(1).U64(boar.guid).U8(2).U64(mage.guid).U8(11).U8(0).U64(stranger).U8(3).U32(0);
        store = go.Store();
        g_received.clear();
        GoWatch                watch;
        const wxl_subscription goSub =
            g_host->Events().Subscribe(WXL_EV_SpellCastGo, &OnSpellGo, &watch, 0);
        wxl::seam::Spell::Packet_Group_30::Before(nullptr, kMsgSpellGo, 0, &store);
        g_host->Events().Unsubscribe(goSub);
        const Received* went = Only(WXL_EV_SpellCastGo);
        Check(went != nullptr && Count(WXL_EV_SpellCastStart) == 0,
              "Spell.Packet_Group_30 raises SpellCastGo once for SMSG_SPELL_GO");
        if (went)
        {
            const wxl_spell_cast_args args = went->As<wxl_spell_cast_args>();
            Check(args.caster == WXL_HANDLE_NONE && args.caster_guid == stranger,
                  "a caster the client does not hold has no handle but keeps its guid");
            Check(args.hit_count == 1 && args.miss_count == 2 && args.target_guid == 0 &&
                      args.target == WXL_HANDLE_NONE && args.cast_item == 0x4000000000000042ull,
                  "the hits and misses are counted past a reflect result, and no target is none");
            Check(args.spell_id == 5176 && args.cast_time == 0, "and a go carries no cast time");
            Check(args.hits_size == 8 && watch.hits.size() == 8 && GuidAt(watch.hits, 0) == boar.guid,
                  "the hits are handed over as the guids the server listed");
            Check(args.misses_size == 16 && watch.misses.size() == 16 &&
                      GuidAt(watch.misses, 0) == mage.guid && GuidAt(watch.misses, 8) == stranger,
                  "the misses as the guids the server listed, the reflect result left out");
            Check(watch.reasons.size() == 2 && watch.reasons[0] == 11 && watch.reasons[1] == 3,
                  "and one reason per miss, in the same order");
        }

        // What raises nothing: another opcode, a message cut short.
        g_received.clear();
        store = start.Store();
        wxl::seam::Spell::Packet_Group_30::Before(nullptr, kMsgSpellFailure, 0, &store);
        FakeStore cut{ start.bytes.data(), 6, 0 };
        wxl::seam::Spell::Packet_Group_30::Before(nullptr, kMsgSpellStart, 0, &cut);
        Check(g_received.empty(), "an opcode the handler does not serve, or a truncated message, raises nothing");

        // SMSG_SPELL_FAILURE: the caster, the count, the spell, the reason.
        Body failure;
        failure.Packed(boar.guid).U8(3).U32(8921).U8(24);
        store = failure.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_SPELL_FAILURE::Before(nullptr, kMsgSpellFailure, 0, &store);
        const Received* interrupted = Only(WXL_EV_SpellCastInterrupted);
        Check(interrupted != nullptr, "Spell.Packet_SMSG_SPELL_FAILURE raises SpellCastInterrupted once");
        if (interrupted)
        {
            const wxl_spell_fail_args args = interrupted->As<wxl_spell_fail_args>();
            Check(args.caster == boarHandle && args.caster_guid == boar.guid && args.spell_id == 8921 &&
                      args.cast_count == 3 && args.reason == 24,
                  "with the caster, the spell, the count and the reason");
            Check(args.custom_error == -1 && args.custom_arg == -1, "and no custom error");
        }

        // Spell_C_SpellFailed: the pending cast, read through the binding.
        FakePendingCast pending;
        pending.caster  = mage.guid;
        pending.spellId = 116;
        pending.count   = 9;
        g_received.clear();
        wxl::seam::Spell::SpellFailed::Before(&pending, nullptr, 67, 30, 1, 1);
        const Received* failed = Only(WXL_EV_SpellCastFailed);
        Check(failed != nullptr, "Spell.SpellFailed raises SpellCastFailed once");
        if (failed)
        {
            const wxl_spell_fail_args args = failed->As<wxl_spell_fail_args>();
            Check(args.caster == mageHandle && args.caster_guid == mage.guid && args.spell_id == 116 &&
                      args.cast_count == 9 && args.reason == 67,
                  "with the cast's caster, spell and count, and the reason");
            Check(args.custom_error == 30 && args.custom_arg == 1, "and the custom error as passed");
        }
        g_received.clear();
        wxl::seam::Spell::SpellFailed::Before(nullptr, nullptr, 67, -1, -1, 0);
        Check(g_received.empty(), "a null cast raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&mage);
        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
    }

    bool g_refuse = false;
    int  g_requests = 0;
    wxl_spell_request_args g_lastSpellRequest{};
    wxl_item_use_args      g_lastItemUse{};

    void WXL_CALL OnSpellRequest(void*, const void* args)
    {
        const wxl_spell_request_args& request = *static_cast<const wxl_spell_request_args*>(args);
        ++g_requests;
        g_lastSpellRequest = request;
        if (g_refuse && request.refused) *request.refused = WXL_TRUE;
    }

    void WXL_CALL OnItemUse(void*, const void* args)
    {
        const wxl_item_use_args& use = *static_cast<const wxl_item_use_args*>(args);
        ++g_requests;
        g_lastItemUse = use;
        if (g_refuse && use.refused) *use.refused = WXL_TRUE;
    }

    void CheckSpellRequest()
    {
        const wxl_subscription sub =
            g_host->Events().Subscribe(WXL_EV_SpellCastRequest, &OnSpellRequest, nullptr, 0);

        FakeObject player;
        player.guid   = 0x0000000000000ABCull;
        player.typeId = WXL_OBJECT_PLAYER;
        FakeObject potion;
        potion.guid   = 0x4000000000000042ull;
        potion.typeId = WXL_OBJECT_ITEM;
        wxl::seam::ObjectMgr::InitObject::After(&player, nullptr, 4000u);
        wxl::seam::ObjectMgr::InitObject::After(&potion, nullptr, 4001u);

        uint8_t     answer    = 1;
        void*       caster    = &player;
        int32_t     spellId   = 133;
        const void* item      = &potion;
        uint64_t    target    = 0xF130000000000123ull;
        void*       cast      = nullptr;
        uint8_t     confirmed = 0;

        g_refuse   = false;
        g_requests = 0;
        Check(!wxl::seam::Spell::CastSpell::Gate(&answer, &caster, &spellId, &item, &target, &cast,
                                                 &confirmed),
              "a cast nobody refuses is left to the client");
        Check(g_requests == 1 && g_lastSpellRequest.caster == wxl::core::objects::HandleOf(&player) &&
                  g_lastSpellRequest.spell_id == 133 &&
                  g_lastSpellRequest.item == wxl::core::objects::HandleOf(&potion) &&
                  g_lastSpellRequest.target_guid == target && g_lastSpellRequest.confirmed == WXL_FALSE,
              "and the handler is told the caster, the spell, the item and the target");

        g_refuse  = true;
        confirmed = 1;
        item      = nullptr;
        Check(wxl::seam::Spell::CastSpell::Gate(&answer, &caster, &spellId, &item, &target, &cast,
                                                &confirmed),
              "a handler that refuses it takes the call");
        Check(answer == 0 && g_lastSpellRequest.item == WXL_HANDLE_NONE &&
                  g_lastSpellRequest.confirmed == WXL_TRUE,
              "the client is answered false; no item is no handle, and the second ask says so");

        g_host->Events().Unsubscribe(sub);

        // CGItem_C::UseItem: the item itself is `this`, the target comes by address.
        const wxl_subscription use =
            g_host->Events().Subscribe(WXL_EV_ItemUseRequest, &OnItemUse, nullptr, 0);
        int32_t         result     = 1;
        void*           self       = &potion;
        const uint64_t* targetPtr  = &target;
        int32_t         confirmed2 = 0;

        g_refuse   = false;
        g_requests = 0;
        Check(!wxl::seam::Item::UseItem::Gate(&result, &self, &targetPtr, &confirmed2),
              "an item use nobody refuses is left to the client");
        Check(g_requests == 1 && g_lastItemUse.item == wxl::core::objects::HandleOf(&potion) &&
                  g_lastItemUse.item_guid == potion.guid && g_lastItemUse.target_guid == target &&
                  g_lastItemUse.confirmed == WXL_FALSE,
              "and the handler is told the item, its guid and the target");

        g_refuse  = true;
        targetPtr = nullptr;
        Check(wxl::seam::Item::UseItem::Gate(&result, &self, &targetPtr, &confirmed2),
              "a handler that refuses it takes the call");
        Check(result == 0 && g_lastItemUse.target_guid == 0,
              "the client is answered 0, and no target address is no target");

        g_host->Events().Unsubscribe(use);
        wxl::seam::ObjectMgr::ObjDelete::Before(&player);
        wxl::seam::ObjectMgr::ObjDelete::Before(&potion);
    }

    // --- Spell, continued: channels, a resumed cast, a delay, cooldowns, a cancel ---------

    constexpr uint32_t kMsgChannelStart   = 0x139;
    constexpr uint32_t kMsgChannelUpdate  = 0x13A;
    constexpr uint32_t kMsgResumeCastBar  = 0x14D;
    constexpr uint32_t kMsgSpellDelayed   = 0x1E2;
    constexpr uint32_t kMsgSpellCooldown  = 0x134;
    constexpr uint32_t kMsgItemCooldown   = 0xB0;
    constexpr uint32_t kMsgCooldownEvent  = 0x135;
    constexpr uint32_t kMsgClearCooldown  = 0x1DE;
    constexpr uint32_t kMsgModifyCooldown = 0x491;
    constexpr uint32_t kMsgCooldownCheat  = 0x1E1;

    void CheckSpellProgress()
    {
        FakeObject priest;
        priest.guid           = 0x0000000000000ABCull;
        priest.typeId         = WXL_OBJECT_PLAYER;
        priest.castingSpellId = 585;
        priest.channelSpellId = 15407;
        FakeObject boar;
        boar.guid   = 0xF130000000000123ull;
        boar.typeId = WXL_OBJECT_UNIT;
        wxl::seam::ObjectMgr::InitObject::After(&priest, nullptr, 5000u);
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 5001u);
        const wxl_handle priestHandle = wxl::core::objects::HandleOf(&priest);
        const wxl_handle boarHandle   = wxl::core::objects::HandleOf(&boar);
        const uint64_t   stranger     = 0xF13000000000FFFFull;

        // MSG_CHANNEL_START: the caster, the spell, the length.
        Body start;
        start.Packed(priest.guid).U32(15407).U32(3000);
        FakeStore store = start.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_MSG_CHANNEL_START::Before(nullptr, kMsgChannelStart, 0, &store);
        const Received* started = Only(WXL_EV_SpellChannelStart);
        Check(started != nullptr, "Spell.Packet_MSG_CHANNEL_START raises SpellChannelStart once");
        if (started)
        {
            const wxl_spell_progress_args args = started->As<wxl_spell_progress_args>();
            Check(args.caster == priestHandle && args.caster_guid == priest.guid && args.spell_id == 15407,
                  "with the caster and the spell");
            Check(args.duration == 3000 && args.remaining == 3000 && args.target_guid == 0 &&
                      args.target == WXL_HANDLE_NONE,
                  "the length as both duration and remaining, and no target");
        }
        Check(store.read == 0u, "reading the message left the store's cursor where it was");
        Body none;
        none.Packed(priest.guid).U32(15407).U32(0);
        store = none.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_MSG_CHANNEL_START::Before(nullptr, kMsgChannelStart, 0, &store);
        Check(g_received.empty(), "a channel of no length raises nothing, as the client shows none");

        // MSG_CHANNEL_UPDATE with time left: the spell is the one the unit holds.
        Body update;
        update.Packed(priest.guid).U32(1200);
        store = update.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_MSG_CHANNEL_UPDATE::Before(nullptr, kMsgChannelUpdate, 0, &store);
        const Received* updated = Only(WXL_EV_SpellChannelUpdate);
        Check(updated != nullptr && Count(WXL_EV_SpellChannelStop) == 0,
              "Spell.Packet_MSG_CHANNEL_UPDATE raises SpellChannelUpdate while time is left");
        if (updated)
        {
            const wxl_spell_progress_args args = updated->As<wxl_spell_progress_args>();
            Check(args.caster == priestHandle && args.spell_id == 15407 && args.remaining == 1200 &&
                      args.duration == 0,
                  "with the channel's spell read off the unit, the time left and no length");
        }
        Body stop;
        stop.Packed(stranger).U32(0);
        store = stop.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_MSG_CHANNEL_UPDATE::Before(nullptr, kMsgChannelUpdate, 0, &store);
        const Received* stopped = Only(WXL_EV_SpellChannelStop);
        Check(stopped != nullptr && Count(WXL_EV_SpellChannelUpdate) == 0,
              "and SpellChannelStop when none is left");
        if (stopped)
        {
            const wxl_spell_progress_args args = stopped->As<wxl_spell_progress_args>();
            Check(args.caster == WXL_HANDLE_NONE && args.caster_guid == stranger && args.spell_id == 0,
                  "a caster not in view keeps its guid and has no spell to read");
        }

        // SMSG_UNIT_SPELLCAST_START: the caster, the target, the spell, the time left, the length.
        Body resume;
        resume.Packed(priest.guid).Packed(boar.guid).U32(585).U32(900).U32(2500);
        store = resume.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_UNIT_SPELLCAST_START::Before(nullptr, kMsgResumeCastBar, 0, &store);
        const Received* resumed = Only(WXL_EV_SpellCastResume);
        Check(resumed != nullptr, "Spell.Packet_SMSG_UNIT_SPELLCAST_START raises SpellCastResume once");
        if (resumed)
        {
            const wxl_spell_progress_args args = resumed->As<wxl_spell_progress_args>();
            Check(args.caster == priestHandle && args.target == boarHandle && args.target_guid == boar.guid &&
                      args.spell_id == 585 && args.remaining == 900 && args.duration == 2500,
                  "with the caster, the target, the spell, the time left and the length");
        }

        // SMSG_SPELL_DELAYED: the caster and the delay; the spell is the unit's cast.
        Body delayed;
        delayed.Packed(priest.guid).U32(500);
        store = delayed.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_SPELL_DELAYED::Before(nullptr, kMsgSpellDelayed, 0, &store);
        const Received* pushed = Only(WXL_EV_SpellCastDelayed);
        Check(pushed != nullptr, "Spell.Packet_SMSG_SPELL_DELAYED raises SpellCastDelayed once");
        if (pushed)
        {
            const wxl_spell_delay_args args = pushed->As<wxl_spell_delay_args>();
            Check(args.caster == priestHandle && args.caster_guid == priest.guid && args.spell_id == 585 &&
                      args.delay == 500,
                  "with the caster, the cast read off the unit and the delay");
        }
        FakeStore cut{ delayed.bytes.data(), 3, 0 };
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_SPELL_DELAYED::Before(nullptr, kMsgSpellDelayed, 0, &cut);
        Check(g_received.empty(), "a message cut short raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&priest);
        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
    }

    void CheckSpellCooldown()
    {
        FakeObject player;
        player.guid   = 0x0000000000000ABCull;
        player.typeId = WXL_OBJECT_PLAYER;
        FakeObject potion;
        potion.guid   = 0x4000000000000042ull;
        potion.typeId = WXL_OBJECT_ITEM;
        wxl::seam::ObjectMgr::InitObject::After(&player, nullptr, 6000u);
        wxl::seam::ObjectMgr::InitObject::After(&potion, nullptr, 6001u);
        const wxl_handle playerHandle = wxl::core::objects::HandleOf(&player);
        const wxl_handle potionHandle = wxl::core::objects::HandleOf(&potion);

        // SMSG_SPELL_COOLDOWN: the unit, the flags, two spells, and three bytes that are no pair.
        Body cooldown;
        cooldown.U64(player.guid).U8(1).U32(133).U32(8000).U32(116).U32(0).U8(7).U8(7).U8(7);
        FakeStore store = cooldown.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_SPELL_COOLDOWN::Before(nullptr, kMsgSpellCooldown, 0, &store);
        Check(Count(WXL_EV_SpellCooldown) == 2 && g_received.size() == 2,
              "Spell.Packet_SMSG_SPELL_COOLDOWN raises SpellCooldown once per whole pair");
        if (g_received.size() == 2)
        {
            const wxl_spell_cooldown_args first  = g_received[0].As<wxl_spell_cooldown_args>();
            const wxl_spell_cooldown_args second = g_received[1].As<wxl_spell_cooldown_args>();
            Check(first.unit == playerHandle && first.unit_guid == player.guid && first.spell_id == 133 &&
                      first.duration == 8000 && first.flags == 1 && first.item == WXL_HANDLE_NONE &&
                      first.item_guid == 0,
                  "with the unit, the spell, the duration, the flags and no item");
            Check(second.spell_id == 116 && second.duration == 0 && second.unit == playerHandle,
                  "the second pair the same unit, with the spell's own recovery time");
        }
        Check(store.read == 0u, "reading the message left the store's cursor where it was");

        // SMSG_ITEM_COOLDOWN: the item and the spell it casts.
        Body item;
        item.U64(potion.guid).U32(2370);
        store = item.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_ITEM_COOLDOWN::Before(nullptr, kMsgItemCooldown, 0, &store);
        const Received* onItem = Only(WXL_EV_SpellCooldown);
        Check(onItem != nullptr, "Spell.Packet_SMSG_ITEM_COOLDOWN raises SpellCooldown once");
        if (onItem)
        {
            const wxl_spell_cooldown_args args = onItem->As<wxl_spell_cooldown_args>();
            Check(args.item == potionHandle && args.item_guid == potion.guid && args.spell_id == 2370 &&
                      args.unit == WXL_HANDLE_NONE && args.unit_guid == 0 && args.duration == 0,
                  "with the item, the spell, no unit and no duration");
        }

        // Packet_Group_31: the spell and the unit, then what the opcode says.
        Body change;
        change.U32(133).U64(player.guid);
        const struct
        {
            uint32_t opcode;
            uint32_t change;
        } opcodes[] = { { kMsgCooldownEvent, 0 }, { kMsgClearCooldown, 1 } };
        for (const auto& opcode : opcodes)
        {
            store = change.Store();
            g_received.clear();
            wxl::seam::Spell::Packet_Group_31::Before(nullptr, opcode.opcode, 0, &store);
            const Received* changed = Only(WXL_EV_SpellCooldownChanged);
            char            what[128];
            snprintf(what, sizeof what, "Spell.Packet_Group_31 raises SpellCooldownChanged %u for opcode 0x%X",
                     unsigned(opcode.change), unsigned(opcode.opcode));
            const wxl_spell_cooldown_change_args args =
                changed ? changed->As<wxl_spell_cooldown_change_args>() : wxl_spell_cooldown_change_args{};
            Check(changed && args.change == opcode.change && args.delay == 0 && args.spell_id == 133 &&
                      args.unit == playerHandle && args.unit_guid == player.guid,
                  what);
        }
        Body modify;
        modify.U32(133).U64(player.guid).U32(static_cast<uint32_t>(-1000));
        store = modify.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_Group_31::Before(nullptr, kMsgModifyCooldown, 0, &store);
        const Received* shifted = Only(WXL_EV_SpellCooldownChanged);
        Check(shifted != nullptr && shifted->As<wxl_spell_cooldown_change_args>().change == 2 &&
                  shifted->As<wxl_spell_cooldown_change_args>().delay == -1000,
              "a modify carries change 2 and its shift, signed");
        store = change.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_Group_31::Before(nullptr, kMsgCooldownCheat, 0, &store);
        Check(g_received.empty(), "an opcode the row does not serve raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&player);
        wxl::seam::ObjectMgr::ObjDelete::Before(&potion);
    }

    void CheckSpellCancel()
    {
        FakeObject mage;
        mage.guid   = 0x0000000000000ABCull;
        mage.typeId = WXL_OBJECT_PLAYER;
        wxl::seam::ObjectMgr::InitObject::After(&mage, nullptr, 7000u);
        const wxl_handle mageHandle = wxl::core::objects::HandleOf(&mage);

        FakePendingCast pending;
        pending.caster  = mage.guid;
        pending.spellId = 133;
        pending.count   = 4;
        g_received.clear();
        wxl::seam::Spell::CancelSpell::Before(&pending, 1, 1, 27);
        const Received* cancelled = Only(WXL_EV_SpellCastCancel);
        Check(cancelled != nullptr, "Spell.CancelSpell raises SpellCastCancel once");
        if (cancelled)
        {
            const wxl_spell_cancel_args args = cancelled->As<wxl_spell_cancel_args>();
            Check(args.caster == mageHandle && args.caster_guid == mage.guid && args.spell_id == 133 &&
                      args.cast_count == 4 && args.reason == 27 && args.notify_server == WXL_TRUE,
                  "with the cast's caster, spell and count, the reason, and that the server may be told");
        }
        g_received.clear();
        wxl::seam::Spell::CancelSpell::Before(&pending, 0, 0, 40);
        const Received* quiet = Only(WXL_EV_SpellCastCancel);
        Check(quiet && quiet->As<wxl_spell_cancel_args>().notify_server == WXL_FALSE &&
                  quiet->As<wxl_spell_cancel_args>().reason == 40,
              "a cancel the server is not told of says so");
        g_received.clear();
        wxl::seam::Spell::CancelSpell::Before(nullptr, 1, 1, 27);
        Check(g_received.empty(), "a null cast raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&mage);
    }

    // --- Combat: attacks, rounds, the environment, and the spell logs ----------------------

    constexpr uint32_t kMsgAttackStart         = 0x143;
    constexpr uint32_t kMsgAttackStop          = 0x144;
    constexpr uint32_t kMsgAttackSwingNotInRange = 0x145;
    constexpr uint32_t kMsgAttackerStateUpdate = 0x14A;
    constexpr uint32_t kMsgEnvironmentalDamage = 0x1FC;

    void CheckCombat()
    {
        FakeObject warrior;
        warrior.guid   = 0x0000000000000ABCull;
        warrior.typeId = WXL_OBJECT_PLAYER;
        FakeObject boar;
        boar.guid   = 0xF130000000000123ull;
        boar.typeId = WXL_OBJECT_UNIT;
        wxl::seam::ObjectMgr::InitObject::After(&warrior, nullptr, 8000u);
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 8001u);
        const wxl_handle warriorHandle = wxl::core::objects::HandleOf(&warrior);
        const wxl_handle boarHandle    = wxl::core::objects::HandleOf(&boar);
        const uint64_t   stranger      = 0xF13000000000FFFFull;

        // SMSG_ATTACKSTART: two full guids.
        Body attack;
        attack.U64(warrior.guid).U64(boar.guid);
        FakeStore store = attack.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackStart, 0, &store);
        const Received* began = Only(WXL_EV_CombatAttackStart);
        Check(began != nullptr, "Combat.Packet_Group_29 raises CombatAttackStart for SMSG_ATTACKSTART");
        if (began)
        {
            const wxl_combat_attack_args args = began->As<wxl_combat_attack_args>();
            Check(args.attacker == warriorHandle && args.attacker_guid == warrior.guid &&
                      args.victim == boarHandle && args.victim_guid == boar.guid,
                  "with the attacker and the victim");
        }
        Check(store.read == 0u, "reading the message left the store's cursor where it was");

        // SMSG_ATTACKSTOP: packed guids and the facing flag.
        Body stop;
        stop.Packed(boar.guid).Packed(warrior.guid).U32(1);
        store = stop.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackStop, 0, &store);
        const Received* ended = Only(WXL_EV_CombatAttackStop);
        Check(ended && ended->As<wxl_combat_attack_args>().attacker == boarHandle &&
                  ended->As<wxl_combat_attack_args>().victim == warriorHandle,
              "CombatAttackStop for SMSG_ATTACKSTOP, with the packed guids");

        // SMSG_ATTACKERSTATEUPDATE that landed: a crit over two schools, absorbs per school, a block.
        const uint32_t landedFlags = 0x200 | 0x20 | 0x2000;
        Body           round;
        round.U32(landedFlags).Packed(warrior.guid).Packed(boar.guid).U32(150).U32(0).U8(2)
            .U32(1).U32(0).U32(100).U32(4).U32(0).U32(50).U32(10).U32(5)
            .U8(1).U32(0).U32(0).U32(20);
        store = round.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackerStateUpdate, 0, &store);
        const Received* landed = Only(WXL_EV_CombatDamage);
        Check(landed != nullptr && Count(WXL_EV_CombatMiss) == 0,
              "a round that landed raises CombatDamage once and no CombatMiss");
        if (landed)
        {
            const wxl_combat_damage_args args = landed->As<wxl_combat_damage_args>();
            Check(args.source == warriorHandle && args.target == boarHandle && args.kind == 0 &&
                      args.spell_id == 0 && args.amount == 150 && args.overkill == 0,
                  "with the attacker, the victim, a plain swing and the total");
            Check(args.school == 5 && args.absorbed == 15 && args.resisted == 0 && args.blocked == 20 &&
                      args.crit == WXL_TRUE && args.hit_info == landedFlags,
                  "the schools joined, the absorbs summed, the block, the crit and the flags");
        }
        Body dodged;
        dodged.U32(0).Packed(warrior.guid).Packed(boar.guid).U32(0).U32(0).U8(1).U32(1).U32(0).U32(0)
            .U8(2).U32(0).U32(845);
        store = dodged.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackerStateUpdate, 0, &store);
        const Received* escaped = Only(WXL_EV_CombatMiss);
        Check(escaped != nullptr && Count(WXL_EV_CombatDamage) == 0,
              "a round the victim dodged raises CombatMiss and no CombatDamage");
        if (escaped)
        {
            const wxl_combat_miss_args args = escaped->As<wxl_combat_miss_args>();
            Check(args.melee == WXL_TRUE && args.reason == 2 && args.spell_id == 845 && args.hit_info == 0 &&
                      args.source == warriorHandle && args.target == boarHandle,
                  "with the victim's state as the reason and the swing's spell");
        }
        Body missed;
        missed.U32(0x10).Packed(warrior.guid).Packed(boar.guid).U32(0).U32(0).U8(0).U8(0).U32(0).U32(0);
        store = missed.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackerStateUpdate, 0, &store);
        const Received* plain = Only(WXL_EV_CombatMiss);
        Check(plain && plain->As<wxl_combat_miss_args>().reason == 0 &&
                  plain->As<wxl_combat_miss_args>().hit_info == 0x10,
              "a plain miss carries no reason and the miss flag");

        // SMSG_ENVIRONMENTALDAMAGELOG: a full guid, the cause, the amounts.
        Body lava;
        lava.U64(warrior.guid).U8(3).U32(400).U32(0).U32(0);
        store = lava.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgEnvironmentalDamage, 0, &store);
        const Received* burned = Only(WXL_EV_CombatDamage);
        Check(burned != nullptr, "CombatDamage for SMSG_ENVIRONMENTALDAMAGELOG");
        if (burned)
        {
            const wxl_combat_damage_args args = burned->As<wxl_combat_damage_args>();
            Check(args.kind == 3 && args.environment == 3 && args.amount == 400 && args.school == 4 &&
                      args.source == WXL_HANDLE_NONE && args.source_guid == 0 && args.target == warriorHandle,
                  "with no source, the cause, the amount and the cause's school");
        }
        store = attack.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackSwingNotInRange, 0, &store);
        Check(g_received.empty(), "an opcode the row does not report raises nothing");

        // SMSG_SPELLLOGMISS: two targets, one not in view.
        Body missLog;
        missLog.U32(116).U64(warrior.guid).U8(0).U32(2).U64(boar.guid).U8(3).U64(stranger).U8(7);
        store = missLog.Store();
        g_received.clear();
        wxl::seam::Combat::HandleSpellLogMiss::Before(&store, 0);
        Check(Count(WXL_EV_CombatMiss) == 2 && g_received.size() == 2,
              "Combat.HandleSpellLogMiss raises CombatMiss once per target");
        if (g_received.size() == 2)
        {
            const wxl_combat_miss_args first  = g_received[0].As<wxl_combat_miss_args>();
            const wxl_combat_miss_args second = g_received[1].As<wxl_combat_miss_args>();
            Check(first.source == warriorHandle && first.target == boarHandle && first.spell_id == 116 &&
                      first.reason == 3 && first.melee == WXL_FALSE && first.hit_info == 0,
                  "with the caster, the target, the spell and the reason");
            Check(second.target == WXL_HANDLE_NONE && second.target_guid == stranger && second.reason == 7,
                  "a target not in view keeps its guid");
        }
        Body missDebug;
        missDebug.U32(116).U64(warrior.guid).U8(1).U32(1).U64(boar.guid).U8(1).U32(0).U32(0);
        store = missDebug.Store();
        g_received.clear();
        wxl::seam::Combat::HandleSpellLogMiss::Before(&store, 0);
        Check(Only(WXL_EV_CombatMiss) && Only(WXL_EV_CombatMiss)->As<wxl_combat_miss_args>().reason == 1,
              "the debug floats after a target are skipped");
        Body missCut;
        missCut.U32(116).U64(warrior.guid).U8(0).U32(2).U64(boar.guid).U8(3);
        store = missCut.Store();
        g_received.clear();
        wxl::seam::Combat::HandleSpellLogMiss::Before(&store, 0);
        Check(g_received.empty(), "a list cut short raises none of it");

        // SMSG_SPELLNONMELEEDAMAGELOG: the target first, then the caster.
        Body spell;
        spell.Packed(boar.guid).Packed(warrior.guid).U32(116).U32(900).U32(100).U8(16).U32(50).U32(25)
            .U8(0).U8(0).U32(0).U32(0x2).U8(0);
        store = spell.Store();
        g_received.clear();
        wxl::seam::Combat::HandleSpellNonMeleeDamageLog::Before(&store, 0);
        const Received* hit = Only(WXL_EV_CombatDamage);
        Check(hit != nullptr, "Combat.HandleSpellNonMeleeDamageLog raises CombatDamage once");
        if (hit)
        {
            const wxl_combat_damage_args args = hit->As<wxl_combat_damage_args>();
            Check(args.source == warriorHandle && args.target == boarHandle && args.kind == 1 &&
                      args.spell_id == 116 && args.amount == 900 && args.overkill == 100,
                  "with the caster, the target, the spell, the damage and the overkill");
            Check(args.school == 16 && args.absorbed == 50 && args.resisted == 25 && args.blocked == 0 &&
                      args.crit == WXL_TRUE && args.hit_info == 0x2,
                  "the school, the amounts, the crit and the flags");
        }

        // SMSG_SPELLHEALLOG: the target first, then the caster.
        Body heal;
        heal.Packed(warrior.guid).Packed(boar.guid).U32(2061).U32(1200).U32(200).U32(0).U8(1).U8(0);
        store = heal.Store();
        g_received.clear();
        wxl::seam::Combat::HandleSpellHealLog::Before(&store, 0);
        const Received* healed = Only(WXL_EV_CombatHeal);
        Check(healed != nullptr, "Combat.HandleSpellHealLog raises CombatHeal once");
        if (healed)
        {
            const wxl_combat_heal_args args = healed->As<wxl_combat_heal_args>();
            Check(args.source == boarHandle && args.target == warriorHandle && args.spell_id == 2061 &&
                      args.amount == 1200 && args.overheal == 200 && args.absorbed == 0 &&
                      args.crit == WXL_TRUE && args.periodic == WXL_FALSE,
                  "with the caster, the target, the spell, the amounts and the crit");
        }

        // SMSG_PERIODICAURALOG: a damage tick, an energize tick, a heal tick, then a type the
        // client stops at.
        Body ticks;
        ticks.Packed(boar.guid).Packed(warrior.guid).U32(589).U32(4)
            .U32(3).U32(80).U32(0).U32(32).U32(0).U32(10).U8(0)
            .U32(21).U32(0).U32(30)
            .U32(8).U32(60).U32(0).U32(0).U8(1)
            .U32(77).U32(1);
        store = ticks.Store();
        g_received.clear();
        wxl::seam::Combat::HandlePeriodicAuraLog::Before(&store, 0);
        Check(g_received.size() == 3 && g_received[0].event == WXL_EV_CombatDamage &&
                  g_received[1].event == WXL_EV_CombatEnergize && g_received[2].event == WXL_EV_CombatHeal,
              "Combat.HandlePeriodicAuraLog raises the damage, energize and heal ticks, in order, up to the stop");
        if (g_received.size() == 3)
        {
            const wxl_combat_damage_args tick = g_received[0].As<wxl_combat_damage_args>();
            const wxl_combat_heal_args   hot  = g_received[2].As<wxl_combat_heal_args>();
            Check(tick.kind == 2 && tick.spell_id == 589 && tick.amount == 80 && tick.school == 32 &&
                      tick.resisted == 10 && tick.crit == WXL_FALSE && tick.hit_info == 0 &&
                      tick.source == warriorHandle && tick.target == boarHandle,
                  "the damage tick with its amounts, the school and no flags");
            Check(hot.periodic == WXL_TRUE && hot.amount == 60 && hot.crit == WXL_TRUE &&
                      hot.spell_id == 589,
                  "the heal tick as periodic, with its amount and crit");
        }
        Body tickCut;
        tickCut.Packed(boar.guid).Packed(warrior.guid).U32(589).U32(2)
            .U32(3).U32(80).U32(0).U32(32).U32(0).U32(10).U8(0)
            .U32(3).U32(80);
        store = tickCut.Store();
        g_received.clear();
        wxl::seam::Combat::HandlePeriodicAuraLog::Before(&store, 0);
        Check(g_received.empty(), "a tick cut short raises none of the log");

        wxl::seam::ObjectMgr::ObjDelete::Before(&warrior);
        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
    }

    // --- The third wave: the world, the player, the console, the glue, the chat, energize --

    /// Strings of an args struct, copied out while they were valid: the harness keeps only
    /// the struct's bytes, and a wxl_str in it points at memory the seam owned for the call.
    struct StrWatch
    {
        std::vector<std::string> strings;
        int                      calls = 0;
    };

    void WXL_CALL OnZone(void* user, const void* args)
    {
        StrWatch&            watch = *static_cast<StrWatch*>(user);
        const wxl_zone_args& zone  = *static_cast<const wxl_zone_args*>(args);
        ++watch.calls;
        watch.strings = { std::string(zone.zone_text.data, zone.zone_text.size),
                          std::string(zone.subzone_text.data, zone.subzone_text.size),
                          std::string(zone.real_zone_text.data, zone.real_zone_text.size) };
    }

    /// Refuses a click on a model or a name plate, as a mod without click targeting would.
    void WXL_CALL RefuseClickOnObjects(void*, const void* args)
    {
        const auto* click = static_cast<const wxl_world_click_request_args*>(args);
        if (click->hit == WXL_WORLD_HIT_OBJECT || click->hit == WXL_WORLD_HIT_NAME_PLATE) *click->refused = WXL_TRUE;
    }

    void CheckWorldZoneMouseoverClick()
    {
        FakeObject boar;
        boar.guid     = 0xF130000000000123ull;
        boar.typeId   = WXL_OBJECT_UNIT;
        boar.position = wxl_vec3{ 10.0f, 20.0f, 30.0f };
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 9000u);
        const wxl_handle boarHandle = wxl::core::objects::HandleOf(&boar);

        // The zone: the first call announces, the same again does not, a subzone change does.
        StrWatch zoneWatch;
        const wxl_subscription zoneSub = g_host->Events().Subscribe(WXL_EV_ZoneChanged, &OnZone, &zoneWatch, 0);
        g_received.clear();
        wxl::seam::GameUI::NewZoneFeedback::Before(12, 0, -1, "Elwynn Forest", nullptr, "Elwynn Forest", 0);
        const Received* zone = Only(WXL_EV_ZoneChanged);
        Check(zone != nullptr, "GameUI.NewZoneFeedback raises ZoneChanged once for a new zone");
        if (zone)
        {
            const wxl_zone_args args = zone->As<wxl_zone_args>();
            Check(args.area_id == 12 && args.subzone_id == 0 && args.wmo_group_id == -1 &&
                      args.indoors == WXL_FALSE && args.new_area == WXL_TRUE,
                  "with the ids, outdoors, and a new area");
            Check(zoneWatch.strings.size() == 3 && zoneWatch.strings[0] == "Elwynn Forest" &&
                      zoneWatch.strings[1].empty() && zoneWatch.strings[2] == "Elwynn Forest",
                  "and the texts, an absent one empty");
        }
        g_received.clear();
        wxl::seam::GameUI::NewZoneFeedback::Before(12, 0, -1, "Elwynn Forest", nullptr, "Elwynn Forest", 0);
        Check(g_received.empty(), "the same zone again raises nothing");
        g_received.clear();
        wxl::seam::GameUI::NewZoneFeedback::Before(12, 87, -1, "Elwynn Forest", "Goldshire", "Elwynn Forest", 0);
        Check(Only(WXL_EV_ZoneChanged) && Only(WXL_EV_ZoneChanged)->As<wxl_zone_args>().new_area == WXL_FALSE &&
                  Only(WXL_EV_ZoneChanged)->As<wxl_zone_args>().subzone_id == 87 && zoneWatch.strings[1] == "Goldshire",
              "a subzone within the area raises ZoneChanged without a new area");
        g_received.clear();
        wxl::seam::GameUI::NewZoneFeedback::Before(12, 87, 3, "Elwynn Forest", "Goldshire", "Elwynn Forest", 1);
        Check(Only(WXL_EV_ZoneChanged) && Only(WXL_EV_ZoneChanged)->As<wxl_zone_args>().indoors == WXL_TRUE,
              "going indoors raises it with indoors set");
        g_host->Events().Unsubscribe(zoneSub);

        // The mouseover: announced once per change, from the previous object to the current.
        g_received.clear();
        wxl::seam::GameUI::HandleObjectTrackChange::Before(boar.guid, 0);
        const Received* over = Only(WXL_EV_MouseoverChanged);
        Check(over != nullptr, "GameUI.HandleObjectTrackChange raises MouseoverChanged once");
        if (over)
        {
            const wxl_mouseover_args args = over->As<wxl_mouseover_args>();
            Check(args.previous == WXL_HANDLE_NONE && args.previous_guid == 0 && args.current == boarHandle &&
                      args.current_guid == boar.guid,
                  "from nothing to the unit's handle");
        }
        g_received.clear();
        wxl::seam::GameUI::HandleObjectTrackChange::Before(boar.guid, 0);
        Check(g_received.empty(), "the same object again raises nothing");
        g_received.clear();
        wxl::seam::GameUI::HandleObjectTrackChange::Before(0, boar.guid);
        Check(Only(WXL_EV_MouseoverChanged) && Only(WXL_EV_MouseoverChanged)->As<wxl_mouseover_args>().previous == boarHandle &&
                  Only(WXL_EV_MouseoverChanged)->As<wxl_mouseover_args>().current == WXL_HANDLE_NONE,
              "leaving it raises MouseoverChanged to nothing");

        // A terrain click: the point, the game object (none here), the button renumbered.
        FakeTerrainClick terrain;
        terrain.point  = wxl_vec3{ 1.0f, 2.0f, 3.0f };
        terrain.button = 4;
        g_received.clear();
        wxl::seam::GameUI::HandleTerrainClick::Before(&terrain);
        const Received* ground = Only(WXL_EV_WorldClick);
        Check(ground != nullptr, "GameUI.HandleTerrainClick raises WorldClick once");
        if (ground)
        {
            const wxl_world_click_args args = ground->As<wxl_world_click_args>();
            Check(args.point.x == 1.0f && args.point.y == 2.0f && args.point.z == 3.0f &&
                      args.object == WXL_HANDLE_NONE && args.button == 1,
                  "with the hit point, no object, and the right button as 1");
        }

        // A sprite click: the object, its position as the point, the left button as 0.
        FakeSpriteClick sprite;
        sprite.guid   = boar.guid;
        sprite.button = 1;
        g_received.clear();
        wxl::seam::GameUI::HandleSpriteClick::Before(&sprite);
        const Received* model = Only(WXL_EV_WorldClick);
        Check(model != nullptr, "GameUI.HandleSpriteClick raises WorldClick once");
        if (model)
        {
            const wxl_world_click_args args = model->As<wxl_world_click_args>();
            Check(args.object == boarHandle && args.point.x == 10.0f && args.point.z == 30.0f && args.button == 0,
                  "with the object, its position, and the left button as 0");
        }

        // The click before the game acts on it: nobody listening, the gate never takes it.
        Mute(WXL_EV_WorldClickRequest);
        int      frame  = 0;
        void*    self   = &frame;
        uint32_t button = 4;
        g_clickHit      = FakeClickHit{2, boar.guid, wxl_vec3{10.0f, 20.0f, 31.0f}};
        Check(!wxl::seam::WorldFrame::PerformDefaultAction::Gate(&self, &button),
              "WorldFrame.PerformDefaultAction: with nobody listening, the click is the game's");
        Unmute(WXL_EV_WorldClickRequest);
        g_received.clear();
        Check(!wxl::seam::WorldFrame::PerformDefaultAction::Gate(&self, &button),
              "a click nobody refuses goes on to the game");
        const Received* request = Only(WXL_EV_WorldClickRequest);
        Check(request != nullptr, "and raises WorldClickRequest once");
        if (request)
        {
            const wxl_world_click_request_args args = request->As<wxl_world_click_request_args>();
            Check(args.button == WXL_MOUSE_RIGHT && args.hit == WXL_WORLD_HIT_OBJECT && args.object == boarHandle &&
                      args.object_guid == boar.guid && args.point.z == 31.0f && args.targeting == WXL_FALSE,
                  "with the right button, the model hit, the unit, its guid and the point");
        }
        const wxl_subscription refuse =
            g_host->Events().Subscribe(WXL_EV_WorldClickRequest, &RefuseClickOnObjects, nullptr, 0);
        button = 1;
        Check(wxl::seam::WorldFrame::PerformDefaultAction::Gate(&self, &button),
              "a handler that refuses a click on a model takes the call: the game does nothing");
        g_clickHit       = FakeClickHit{1, 0, wxl_vec3{1.0f, 2.0f, 3.0f}};
        g_spellTargeting = WXL_TRUE;
        g_received.clear();
        Check(!wxl::seam::WorldFrame::PerformDefaultAction::Gate(&self, &button),
              "the same handler lets a click on the ground through");
        request = Only(WXL_EV_WorldClickRequest);
        Check(request && request->As<wxl_world_click_request_args>().hit == WXL_WORLD_HIT_GROUND &&
                  request->As<wxl_world_click_request_args>().object == WXL_HANDLE_NONE &&
                  request->As<wxl_world_click_request_args>().button == WXL_MOUSE_LEFT &&
                  request->As<wxl_world_click_request_args>().targeting == WXL_TRUE,
              "which says the ground, no object, the left button and a spell waiting for its target");
        g_clickHit = FakeClickHit{4, boar.guid, wxl_vec3{}};
        Check(wxl::seam::WorldFrame::PerformDefaultAction::Gate(&self, &button),
              "a name plate's unit is refused too");
        g_host->Events().Unsubscribe(refuse);
        g_clickHit       = FakeClickHit{};
        g_spellTargeting = WXL_FALSE;

        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
    }

    void CheckPlayerMoveMoneyLevel()
    {
        FakeObject me;
        me.guid          = 0x0000000000000042ull;
        me.typeId        = WXL_OBJECT_PLAYER;
        me.position      = wxl_vec3{ -8949.0f, -132.0f, 83.5f };
        me.facing        = 1.5f;
        me.movementFlags = 0x1;
        me.money         = 12345;
        wxl::seam::ObjectMgr::InitObject::After(&me, nullptr, 9100u);
        const wxl_handle handle = wxl::core::objects::HandleOf(&me);

        g_received.clear();
        wxl::seam::Unit::SendMovementUpdate::Before(&me, 1000, 181, 0.0f, 0, 0, 255);
        const Received* start = Only(WXL_EV_PlayerMoveStart);
        Check(start != nullptr && Count(WXL_EV_PlayerMoveStop) == 0,
              "Unit.SendMovementUpdate raises PlayerMoveStart for MSG_MOVE_START_FORWARD");
        if (start)
        {
            const wxl_player_move_args args = start->As<wxl_player_move_args>();
            Check(args.unit == handle && args.unit_guid == me.guid && args.opcode == 181 && args.flags == 0x1 &&
                      args.position.x == -8949.0f && args.facing == 1.5f,
                  "with the unit, the opcode, the flags, the position and the facing");
        }
        g_received.clear();
        wxl::seam::Unit::SendMovementUpdate::Before(&me, 1200, 183, 0.0f, 0, 0, 255);
        Check(Only(WXL_EV_PlayerMoveStop) != nullptr && Count(WXL_EV_PlayerMoveStart) == 0,
              "PlayerMoveStop for MSG_MOVE_STOP");
        g_received.clear();
        wxl::seam::Unit::SendMovementUpdate::Before(&me, 1300, 238, 0.0f, 0, 0, 255);
        Check(g_received.empty(), "a heartbeat raises nothing");

        const uint32_t previous = 10000;
        g_received.clear();
        wxl::seam::Player::OnUpdateMoney::Before(me.guid, 0xFF8, 4, &previous, nullptr);
        const Received* money = Only(WXL_EV_PlayerMoneyChanged);
        Check(money != nullptr, "Player.OnUpdateMoney raises PlayerMoneyChanged once");
        if (money)
        {
            const wxl_money_args args = money->As<wxl_money_args>();
            Check(args.player == handle && args.previous == 10000 && args.current == 12345,
                  "with the player, the value the mirror hands over and the field's");
        }

        // SMSG_LEVEL_UP_INFO: the level, the health, seven powers, five stats.
        Body level;
        level.U32(10).U32(25).U32(40).U32(0).U32(0).U32(0).U32(0).U32(0).U32(0).U32(1).U32(2).U32(3).U32(4).U32(5);
        FakeStore store = level.Store();
        g_received.clear();
        wxl::seam::Player::Packet_Group_9::Before(nullptr, 0x1D4, 0, &store);
        const Received* leveled = Only(WXL_EV_PlayerLevelUp);
        Check(leveled != nullptr, "Player.Packet_Group_9 raises PlayerLevelUp for SMSG_LEVEL_UP_INFO");
        if (leveled)
        {
            const wxl_level_up_args args = leveled->As<wxl_level_up_args>();
            Check(args.level == 10 && args.health == 25 && args.mana == 40 && args.runic_power == 0 &&
                      args.strength == 1 && args.spirit == 5,
                  "with the level, the health, the powers and the stats");
        }
        Check(store.read == 0u, "reading the message left the store's cursor where it was");
        store = level.Store();
        g_received.clear();
        wxl::seam::Player::Packet_Group_9::Before(nullptr, 0x1D5, 0, &store);
        Check(g_received.empty(), "another opcode of the group raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&me);
    }

    void WXL_CALL OnCVar(void* user, const void* args)
    {
        StrWatch&            watch = *static_cast<StrWatch*>(user);
        const wxl_cvar_args& cvar  = *static_cast<const wxl_cvar_args*>(args);
        ++watch.calls;
        watch.strings = { std::string(cvar.name.data, cvar.name.size),
                          std::string(cvar.previous.data, cvar.previous.size),
                          std::string(cvar.value.data, cvar.value.size) };
    }

    void CheckConsoleVar()
    {
        StrWatch                      watch;
        const wxl_subscription sub = g_host->Events().Subscribe(WXL_EV_CVarChanged, &OnCVar, &watch, 0);

        FakeCVar cvar;
        cvar.name  = "farclip";
        cvar.value = "777";
        wxl::seam::ConsoleVar::InternalSet::Before(&cvar, "1000", 1, 0, 0, 1);
        Check(watch.calls == 1 && watch.strings.size() == 3 && watch.strings[0] == "farclip" &&
                  watch.strings[1] == "777" && watch.strings[2] == "1000",
              "ConsoleVar.InternalSet raises CVarChanged with the name, the previous and the new value");
        wxl::seam::ConsoleVar::InternalSet::Before(&cvar, "777", 1, 0, 0, 1);
        Check(watch.calls == 1, "setting the same value raises nothing");
        wxl::seam::ConsoleVar::InternalSet::Before(&cvar, "1000", 0, 1, 1, 0);
        Check(watch.calls == 1, "a set of the reset and default values alone raises nothing");
        cvar.flags = 0x4;
        wxl::seam::ConsoleVar::InternalSet::Before(&cvar, "1000", 1, 0, 0, 1);
        Check(watch.calls == 1, "a read-only variable raises nothing");
        cvar.flags = 0;
        cvar.value = nullptr;
        wxl::seam::ConsoleVar::InternalSet::Before(&cvar, "5", 1, 1, 1, 0);
        Check(watch.calls == 2 && watch.strings[1].empty() && watch.strings[2] == "5",
              "the first value of a variable raises it with an empty previous");

        g_host->Events().Unsubscribe(sub);
    }

    // --- wxl.cvar: variables registered, saved, read back and held ------------------------------

    void CheckCVarService()
    {
        namespace cvars = wxl::core::services::cvar;
        const wxl_cvar_api& api  = wxl::core::services::CVarApi();
        const auto          str  = [](const char* text) { return wxl_str{text, static_cast<uint32_t>(std::strlen(text))}; };
        const auto          read = [&](const char* name) {
            wxl_str out{};
            return api.get(str(name), &out) == WXL_OK ? std::string(out.data, out.size) : std::string("<none>");
        };
        const auto flagsOf = [&](const char* name) {
            uint32_t out = 0;
            return api.flags(str(name), &out) == WXL_OK ? out : 0xFFFFFFFFu;
        };
        constexpr wxl::host::ModHost::ModToken kMod = 11, kOther = 12;
        const wxl::host::ModHost::ModToken     previous = g_host->SwapCurrentMod(kMod);

        // Before the client reads Config.wtf: the registration waits.
        Check(api.register_cvar(str("test_early"), str("5"), WXL_CVAR_SAVED | WXL_CVAR_LATCHED, str("set early")) == WXL_OK,
              "a variable is registered before the client reads Config.wtf");
        Check(!FakeFindCVar("test_early") && api.exists(str("TEST_EARLY")) && read("test_early") == "5" &&
                  api.set(str("test_early"), str("6")) == WXL_E_PENDING &&
                  flagsOf("test_early") == (WXL_CVAR_SAVED | WXL_CVAR_LATCHED | WXL_CVAR_MOD),
              "it waits: found without case, its default read, a set answered pending, its flags its own");

        // CVar::Initialize: the registrations made, then the file's lines, then the latch.
        wxl::seam::ConsoleVar::Initialize::Before("Config.wtf");
        FakeTableCVar* early = FakeFindCVar("test_early");
        Check(early && early->defaultValue && std::string(early->defaultValue) == "5" && (early->flags & 0x80000001u) == 0x80000001u &&
                  !(early->flags & 0x2) && g_cvarHelp["test_early"] && std::string(g_cvarHelp["test_early"]) == "set early",
              "just before the file is read it is made: default, archived, registered, its help kept, not latched yet");
        FakeConfigLine("test_early", "7");
        FakeConfigLine("test_late", "9");
        FakeConfigLine("test_ranged", "50");
        FakeConfigLine("test_char", "z");
        wxl::seam::ConsoleVar::Initialize::After("Config.wtf");
        Check(read("test_early") == "7" && (early->flags & 0x2), "the saved line reached it, and it latches from now on");
        Check(!api.exists(str("test_late")) && read("test_late") == "<none>",
              "a line nobody registered is kept by the client, and found by nobody");

        Check(api.register_cvar(str("test_late"), str("1"), WXL_CVAR_SAVED, str("")) == WXL_OK && read("test_late") == "9",
              "a registration after the file was read adopts the saved value");
        wxl_str fallback{};
        Check(api.get_default(str("test_late"), &fallback) == WXL_OK && std::string(fallback.data, fallback.size) == "1",
              "and its default is the registered one");

        // A range: a saved value outside it goes back to the default; every set is held to it.
        Check(api.register_cvar(str("test_ranged"), str("2"), WXL_CVAR_SAVED, str("")) == WXL_OK && read("test_ranged") == "50",
              "a variable with a saved 50");
        Check(api.set_range(str("test_ranged"), 0.0, 10.0) == WXL_OK && read("test_ranged") == "2",
              "a range of 0 to 10 sends it back to its default");
        Check(api.set_range(str("test_ranged"), 5.0, 1.0) == WXL_E_INVALID && api.set_range(str("test_ranged"), 3.0, 10.0) == WXL_E_INVALID,
              "bounds the wrong way round, or that leave the default out, are refused");
        Check(api.set(str("test_ranged"), str("11")) == WXL_E_INVALID && api.set(str("test_ranged"), str("abc")) == WXL_E_INVALID &&
                  api.set_number(str("test_ranged"), 3.5) == WXL_OK && read("test_ranged") == "3.5",
              "a set outside the range, or no number, is refused; a number inside is written as it reads back");
        FakeSet(*FakeFindCVar("test_ranged"), "20", true, false, false);
        Check(read("test_ranged") == "3.5", "the console's set outside the range is refused by the callback");
        double number = 0.0;
        Check(api.get_number(str("test_ranged"), &number) == WXL_OK && number == 3.5 && api.get_number(str("test_late"), &number) == WXL_OK &&
                  number == 9.0,
              "the values read as numbers");

        // A latched variable: a set waits for the next start.
        Check(api.set(str("test_early"), str("8")) == WXL_OK && read("test_early") == "7" &&
                  (flagsOf("test_early") & WXL_CVAR_PENDING) && early->latched && std::string(early->latched) == "8",
              "a latched set keeps the value and waits, which flags tells");

        // CVarChanged follows a set made through the service.
        StrWatch                     watch;
        const wxl_subscription sub = g_host->Events().Subscribe(WXL_EV_CVarChanged, &OnCVar, &watch, 0);
        Check(api.set(str("test_late"), str("10")) == WXL_OK && watch.calls == 1 && watch.strings[0] == "test_late" &&
                  watch.strings[1] == "9" && watch.strings[2] == "10",
              "a set raises CVarChanged with the name, the old and the new value");
        Check(api.reset(str("test_late")) == WXL_OK && read("test_late") == "1" && watch.calls == 2, "a reset gives the default back");
        g_host->Events().Unsubscribe(sub);

        // Switches.
        Check(api.set_bool(str("test_late"), WXL_TRUE) == WXL_OK && read("test_late") == "1", "a switch is written 1");
        wxl_bool on = WXL_FALSE;
        api.set(str("test_late"), str("enabled"));
        Check(api.get_bool(str("test_late"), &on) == WXL_OK && on == WXL_TRUE, "\"enabled\" reads as on, as GetCVarBool reads it");
        api.set(str("test_late"), str("off"));
        Check(api.get_bool(str("test_late"), &on) == WXL_OK && on == WXL_FALSE, "and \"off\" as off");

        // Read-only: the console refused, the mod not, another mod refused.
        Check(api.register_cvar(str("test_ro"), str("1"), WXL_CVAR_READ_ONLY, str("")) == WXL_OK &&
                  (FakeFindCVar("test_ro")->flags & 0x100) && !(FakeFindCVar("test_ro")->flags & 0x1),
              "a read-only variable of the session: the console's read-only bit, not archived");
        FakeSet(*FakeFindCVar("test_ro"), "2", true, false, false);
        Check(read("test_ro") == "1" && api.set(str("test_ro"), str("3")) == WXL_OK && read("test_ro") == "3",
              "a set from outside is refused, its mod's goes through");
        g_host->SwapCurrentMod(kOther);
        Check(api.set(str("test_ro"), str("4")) == WXL_E_INVALID && read("test_ro") == "3", "another mod cannot set it");

        // Ownership.
        Check(api.register_cvar(str("TEST_LATE"), str("0"), 0, str("")) == WXL_E_INVALID && api.release(str("test_late")) == WXL_E_NOT_FOUND &&
                  api.set_range(str("test_late"), 0.0, 1.0) == WXL_E_NOT_FOUND,
              "another mod can neither register a held name, nor release it, nor range it");
        g_host->SwapCurrentMod(kMod);
        Check(api.release(str("test_late")) == WXL_OK && api.release(str("test_late")) == WXL_E_NOT_FOUND, "its mod releases it once");
        g_host->SwapCurrentMod(kOther);
        Check(api.register_cvar(str("test_late"), str("0"), WXL_CVAR_SAVED, str("")) == WXL_OK && read("test_late") == "off",
              "a released variable is registered by another mod, its value kept");
        g_host->SwapCurrentMod(kMod);

        // The client's own and the interface's.
        FakeRegisterFull("farclip", "", 0x1, "777", nullptr, false, false);
        FakeRegisterFull("addonSetting", nullptr, 0, "a", nullptr, true, true);
        Check(api.register_cvar(str("FARCLIP"), str("1"), 0, str("")) == WXL_E_INVALID && read("farclip") == "777" &&
                  (flagsOf("farclip") & (WXL_CVAR_CLIENT | WXL_CVAR_SAVED)) == (WXL_CVAR_CLIENT | WXL_CVAR_SAVED),
              "the client's own variable cannot be registered, and reads as the client's and saved");
        Check(api.set(str("farclip"), str("800")) == WXL_OK && read("farclip") == "800", "a mod sets a variable of the client");
        Check(read("addonSetting") == "a" && flagsOf("addonSetting") == WXL_CVAR_INTERFACE,
              "the interface's RegisterCVar is read, and never saved");
        FakeRegisterFull("lockedOne", "", 0x4, "x", nullptr, false, false);
        Check(api.set(str("lockedOne"), str("y")) == WXL_E_INVALID, "a variable the client locked is refused");

        // Malformed names, flags and values.
        std::string longName(64, 'n');
        std::string longValue(250, 'v');
        Check(api.register_cvar(str("a b"), str("1"), 0, str("")) == WXL_E_INVALID &&
                  api.register_cvar(str(""), str("1"), 0, str("")) == WXL_E_INVALID &&
                  api.register_cvar(str(longName.c_str()), str("1"), 0, str("")) == WXL_E_INVALID &&
                  api.register_cvar(str("test_q"), str("say \"hi\""), 0, str("")) == WXL_E_INVALID,
              "a name with a space, empty or too long, and a value with a quote, are refused");
        Check(api.register_cvar(str("test_f"), str("1"), 0x80u, str("")) == WXL_E_INVALID &&
                  api.register_cvar(str("test_f"), str("1"), WXL_CVAR_ACCOUNT | WXL_CVAR_CHARACTER, str("")) == WXL_E_INVALID &&
                  api.register_cvar(str("test_f"), str("1"), WXL_CVAR_LATCHED, str("")) == WXL_E_INVALID &&
                  api.register_cvar(str("test_f"), str("1"), WXL_CVAR_READ_ONLY | WXL_CVAR_CHARACTER, str("")) == WXL_E_INVALID,
              "a flag that names nothing, account with character, latched unsaved, read-only per character are refused");
        Check(api.register_cvar(str("test_long"), str(""), WXL_CVAR_SAVED, str("")) == WXL_OK &&
                  api.set(str("test_long"), str(longValue.c_str())) == WXL_E_INVALID &&
                  api.register_cvar(str("test_long2"), str(""), 0, str("")) == WXL_OK &&
                  api.set(str("test_long2"), str(longValue.c_str())) == WXL_OK,
              "a value too long for a line of Config.wtf is refused for a saved variable only");
        Check(api.register_cvar(str("test_seen"), str("1"), WXL_CVAR_PROTECTED | WXL_CVAR_HIDDEN, str("")) == WXL_OK &&
                  (FakeFindCVar("test_seen")->flags & 0x48) == 0x48 &&
                  flagsOf("test_seen") == (WXL_CVAR_PROTECTED | WXL_CVAR_HIDDEN | WXL_CVAR_MOD),
              "protected and hidden reach the client's bits and read back");

        // The account's and the character's: made again when the client deletes them.
        Check(api.register_cvar(str("test_char"), str("x"), WXL_CVAR_CHARACTER, str("")) == WXL_OK && read("test_char") == "z" &&
                  (FakeFindCVar("test_char")->flags & 0x31) == 0x21,
              "a character's variable: archived in the character's category");
        Check(api.register_cvar(str("test_account"), str("q"), WXL_CVAR_ACCOUNT, str("")) == WXL_OK &&
                  (FakeFindCVar("test_account")->flags & 0x31) == 0x11,
              "an account's variable: archived in the account's category");
        FakeDeleteCategory(0x20);
        Check(!FakeFindCVar("test_char") && FakeFindCVar("test_account"), "the character leaving the world deletes its own");
        wxl::seam::ConsoleVar::DeleteCharacterCVars::After();
        Check(read("test_char") == "x" && (FakeFindCVar("test_char")->flags & 0x80000021u) == 0x80000021u,
              "it is made again at once, at its default");
        FakeConfigLine("test_char", "w");
        Check(read("test_char") == "w", "and the next character's saved value reaches it");
        FakeDeleteCategory(0x10);
        wxl::seam::ConsoleVar::DeleteAccountCVars::After();
        Check(read("test_account") == "q" && FakeFindCVar("test_account"), "likewise the account's at login");

        // A mod's go with it; the variables stay.
        cvars::ForgetMod(kMod);
        Check(read("test_early") == "7" && api.release(str("test_early")) == WXL_E_NOT_FOUND, "a mod's variables are released, not removed");
        g_host->SwapCurrentMod(kOther);
        Check(api.register_cvar(str("test_early"), str("5"), WXL_CVAR_SAVED, str("")) == WXL_OK && !(FakeFindCVar("test_early")->flags & 0x2),
              "another mod takes it, with its own flags");

        wxl_result fromThread = WXL_OK;
        std::thread([&] { fromThread = api.set(str("test_late"), str("1")); }).join();
        Check(fromThread == WXL_E_INVALID, "off the main thread a call is refused");
        g_host->SwapCurrentMod(previous);
    }

    std::string_view SigText(wxl_str text) { return std::string_view(text.data ? text.data : "", text.size); }

    /// Has every interface taken as signed, as wxl.glue-unlock does with GlueXML and FrameXML.
    void WXL_CALL AcceptSignature(void*, const void* args)
    {
        *static_cast<const wxl_interface_signature_args*>(args)->accepted = WXL_TRUE;
    }

    /// Keeps the loose interface folders on disk.
    void WXL_CALL RefuseCleanup(void*, const void* args)
    {
        *static_cast<const wxl_interface_cleanup_args*>(args)->refused = WXL_TRUE;
    }

    void CheckInterfaceSignature()
    {
        namespace sig = wxl::seam::FrameXML::CheckSignature;
        uint8_t        digest[16]{};
        const char*    toc    = "Interface\\FrameXML\\FrameXML.toc";
        const char*    extra  = "Interface\\FrameXML\\Bindings.xml";
        const uint8_t* key    = digest;
        uint8_t*       out    = digest;
        int32_t        result = -5;

        g_sigChecks = g_sigDigests = 0;
        g_sigVerdict = WXL_INTERFACE_SIGNATURE_MODIFIED;
        Mute(WXL_EV_InterfaceSignature);
        Check(!sig::Gate(&result, &toc, &extra, &key, &out) && g_sigChecks == 0 && result == -5,
              "FrameXML.CheckSignature: with nobody listening, the client checks alone");
        Unmute(WXL_EV_InterfaceSignature);

        g_received.clear();
        Check(sig::Gate(&result, &toc, &extra, &key, &out) && result == WXL_INTERFACE_SIGNATURE_MODIFIED &&
                  g_sigChecks == 1 && g_sigLetThrough && digest[0] == 0xAB,
              "listened to, the gate has the client judge (its own call let through) and answers the client's verdict");
        const Received* judged = Only(WXL_EV_InterfaceSignature);
        Check(judged && SigText(judged->As<wxl_interface_signature_args>().file) == toc &&
                  SigText(judged->As<wxl_interface_signature_args>().extra) == extra &&
                  judged->As<wxl_interface_signature_args>().verdict == WXL_INTERFACE_SIGNATURE_MODIFIED,
              "and raises InterfaceSignature once, with the .toc, the file hashed with it and the verdict");

        const wxl_subscription accept = g_host->Events().Subscribe(WXL_EV_InterfaceSignature, &AcceptSignature, nullptr, 0);
        std::memset(digest, 0, sizeof digest);
        result = -5;
        Check(sig::Gate(&result, &toc, &extra, &key, &out) && result == WXL_INTERFACE_SIGNATURE_SIGNED &&
                  digest[0] == 0xAB && g_sigDigests == 0,
              "a modified interface a handler accepts answers signed, with the digest the client made");

        g_sigVerdict = WXL_INTERFACE_SIGNATURE_MISSING;
        std::memset(digest, 0, sizeof digest);
        const char* glue = "Interface\\GlueXML\\GlueXML.toc";
        const char* none = nullptr;
        g_received.clear();
        Check(sig::Gate(&result, &glue, &none, &key, &out) && result == WXL_INTERFACE_SIGNATURE_SIGNED &&
                  digest[0] == 0xCD && g_sigDigests == 1,
              "one with no .sig answers signed too, its digest made as the client makes it, which it had not");
        judged = Only(WXL_EV_InterfaceSignature);
        Check(judged && judged->As<wxl_interface_signature_args>().extra.size == 0 &&
                  judged->As<wxl_interface_signature_args>().verdict == WXL_INTERFACE_SIGNATURE_MISSING,
              "GlueXML.toc is hashed alone: its extra file is empty");

        g_sigVerdict = WXL_INTERFACE_SIGNATURE_SIGNED;
        Check(sig::Gate(&result, &toc, &extra, &key, &out) && result == WXL_INTERFACE_SIGNATURE_SIGNED && g_sigDigests == 1,
              "a signed interface stays signed, nothing made again");
        g_host->Events().Unsubscribe(accept);

        g_sigVerdict = WXL_INTERFACE_SIGNATURE_CORRUPT;
        Check(sig::Gate(&result, &toc, &extra, &key, &out) && result == WXL_INTERFACE_SIGNATURE_CORRUPT && g_sigDigests == 1,
              "with no handler accepting, the client's verdict stands");
        g_sigVerdict = WXL_INTERFACE_SIGNATURE_MODIFIED;

        namespace cleanup = wxl::seam::AddOns::DeleteInterfaceFiles;
        Mute(WXL_EV_InterfaceCleanupRequest);
        Check(!cleanup::Gate(), "AddOns.DeleteInterfaceFiles: with nobody listening, the client clears its folders");
        Unmute(WXL_EV_InterfaceCleanupRequest);
        g_received.clear();
        Check(!cleanup::Gate() && Count(WXL_EV_InterfaceCleanupRequest) == 1,
              "a request nobody refuses raises InterfaceCleanupRequest and lets the client clear them");
        const wxl_subscription keep = g_host->Events().Subscribe(WXL_EV_InterfaceCleanupRequest, &RefuseCleanup, nullptr, 0);
        Check(cleanup::Gate(), "a handler that refuses it takes the call: the folders stay");
        g_host->Events().Unsubscribe(keep);
    }

    void WXL_CALL OnGlueScreen(void* user, const void* args)
    {
        StrWatch&                   watch  = *static_cast<StrWatch*>(user);
        const wxl_glue_screen_args& screen = *static_cast<const wxl_glue_screen_args*>(args);
        ++watch.calls;
        watch.strings = { std::string(screen.screen.data, screen.screen.size),
                          std::string(screen.previous.data, screen.previous.size) };
    }

    void WXL_CALL OnCharacter(void* user, const void* args)
    {
        StrWatch&                 watch     = *static_cast<StrWatch*>(user);
        const wxl_character_args& character = *static_cast<const wxl_character_args*>(args);
        ++watch.calls;
        watch.strings.push_back(std::string(character.name.data, character.name.size));
    }

    void WXL_CALL OnRealm(void* user, const void* args)
    {
        StrWatch&             watch = *static_cast<StrWatch*>(user);
        const wxl_realm_args& realm = *static_cast<const wxl_realm_args*>(args);
        ++watch.calls;
        watch.strings.push_back(std::string(realm.name.data, realm.name.size));
        watch.strings.push_back(std::string(realm.address.data, realm.address.size));
    }

    /// One character of SMSG_CHAR_ENUM, as the server writes it.
    void WriteCharacter(Body& body, uint64_t guid, const char* name, uint8_t level, uint32_t map)
    {
        body.U64(guid).Str(name).U8(1).U8(4).U8(0).U8(1).U8(2).U8(3).U8(4).U8(5).U8(level).U32(12).U32(map)
            .F32(-8949.0f).F32(-132.0f).F32(83.5f).U32(0).U32(0x2).U32(0).U8(level == 1 ? 1 : 0)
            .U32(0).U32(0).U32(0);
        for (int slot = 0; slot < 23; ++slot) body.U32(0).U8(0).U32(0);
    }

    void CheckGlue()
    {
        StrWatch                      screens;
        const wxl_subscription screenSub = g_host->Events().Subscribe(WXL_EV_GlueScreenChanged, &OnGlueScreen, &screens, 0);
        strcpy(g_glueScreen, "login");
        wxl::seam::Glue::UpdateCurrentScreen::Before("charselect");
        Check(screens.calls == 1 && screens.strings.size() == 2 && screens.strings[0] == "charselect" &&
                  screens.strings[1] == "login",
              "Glue.UpdateCurrentScreen raises GlueScreenChanged with the screen coming up and the one left");
        g_host->Events().Unsubscribe(screenSub);

        // SMSG_AUTH_RESPONSE: a queue with the billing block, then a plain refusal.
        Body queue;
        queue.U8(27).U32(0).U8(0).U32(0).U8(2).U32(42).U8(1);
        FakeStore store = queue.Store();
        g_received.clear();
        wxl::seam::RealmConnection::HandleAuthResponse::Before(nullptr, 0x1EE, 0, &store);
        const Received* queued = Only(WXL_EV_GlueAuthResult);
        Check(queued != nullptr, "RealmConnection.HandleAuthResponse raises GlueAuthResult once");
        if (queued)
        {
            const wxl_auth_result_args args = queued->As<wxl_auth_result_args>();
            Check(args.result == 27 && args.queue_position == 42 && args.expansion == 2 &&
                      args.free_character_migration == WXL_TRUE,
                  "with the result, the queue position, the expansion and the migration flag");
        }
        Body refused;
        refused.U8(13);
        store = refused.Store();
        g_received.clear();
        wxl::seam::RealmConnection::HandleAuthResponse::Before(nullptr, 0x1EE, 0, &store);
        Check(Only(WXL_EV_GlueAuthResult) && Only(WXL_EV_GlueAuthResult)->As<wxl_auth_result_args>().result == 13 &&
                  Only(WXL_EV_GlueAuthResult)->As<wxl_auth_result_args>().queue_position == 0,
              "a refusal carries the result alone");

        // A queue message with fewer than the queue's five bytes left: the client's unsigned
        // test wraps and reads the billing block, which runs out after its first dword, and
        // tells the glue the result all the same.
        Body shortQueue;
        shortQueue.U8(27).U32(7);
        store = shortQueue.Store();
        g_received.clear();
        wxl::seam::RealmConnection::HandleAuthResponse::Before(nullptr, 0x1EE, 0, &store);
        {
            const Received* cut = Only(WXL_EV_GlueAuthResult);
            Check(cut && cut->As<wxl_auth_result_args>().result == 27 &&
                      cut->As<wxl_auth_result_args>().billing_time_remaining == 7 &&
                      cut->As<wxl_auth_result_args>().expansion == 0 &&
                      cut->As<wxl_auth_result_args>().queue_position == 0,
                  "a queue message cut short reads as the client reads it, and still carries the result");
        }

        // SMSG_CHAR_ENUM: two characters, then one cut short, then too many.
        StrWatch                      names;
        const wxl_subscription charSub = g_host->Events().Subscribe(WXL_EV_GlueCharacter, &OnCharacter, &names, 0);
        Body list;
        list.U8(2);
        WriteCharacter(list, 0x1ull, "Thrall", 80, 1);
        WriteCharacter(list, 0x2ull, "Jaina", 1, 0);
        store = list.Store();
        g_received.clear();
        wxl::seam::RealmConnection::HandleCharEnum::Before(nullptr, 59, 0, &store);
        Check(g_received.size() == 3 && g_received[0].event == WXL_EV_GlueCharacterList &&
                  g_received[1].event == WXL_EV_GlueCharacter && g_received[2].event == WXL_EV_GlueCharacter,
              "RealmConnection.HandleCharEnum raises GlueCharacterList, then GlueCharacter per entry");
        if (g_received.size() == 3)
        {
            const wxl_character_list_args head   = g_received[0].As<wxl_character_list_args>();
            const wxl_character_args      first  = g_received[1].As<wxl_character_args>();
            const wxl_character_args      second = g_received[2].As<wxl_character_args>();
            Check(head.count == 2 && head.valid == WXL_TRUE, "the list with its count, valid");
            Check(first.index == 0 && first.count == 2 && first.guid == 0x1ull && first.race == 1 && first.class_id == 4 &&
                      first.level == 80 && first.zone_id == 12 && first.map_id == 1 && first.position.z == 83.5f &&
                      first.flags == 0x2 && first.first_login == WXL_FALSE,
                  "the first with its guid, race, class, level, zone, map, position and flags");
            Check(second.index == 1 && second.level == 1 && second.first_login == WXL_TRUE &&
                      names.strings.size() == 2 && names.strings[0] == "Thrall" && names.strings[1] == "Jaina",
                  "the second as a first login, and both names");
        }
        Check(store.read == 0u, "reading the list left the store's cursor where it was");
        Body cut;
        cut.U8(2);
        WriteCharacter(cut, 0x1ull, "Thrall", 80, 1);
        cut.U64(0x2ull).Str("Jaina");
        store = cut.Store();
        g_received.clear();
        wxl::seam::RealmConnection::HandleCharEnum::Before(nullptr, 59, 0, &store);
        Check(g_received.empty(), "a list cut short raises none of it");
        Body many;
        many.U8(11);
        store = many.Store();
        g_received.clear();
        wxl::seam::RealmConnection::HandleCharEnum::Before(nullptr, 59, 0, &store);
        Check(Only(WXL_EV_GlueCharacterList) && Only(WXL_EV_GlueCharacterList)->As<wxl_character_list_args>().valid == WXL_FALSE &&
                  Count(WXL_EV_GlueCharacter) == 0,
              "a list the client refuses raises the list alone, not valid");
        g_host->Events().Unsubscribe(charSub);

        g_received.clear();
        wxl::seam::RealmConnection::RequestCharacterLogin::Before(nullptr, 0x1ull, 0);
        Check(Only(WXL_EV_GlueEnterWorld) && Only(WXL_EV_GlueEnterWorld)->As<wxl_enter_world_args>().character_guid == 0x1ull,
              "RealmConnection.RequestCharacterLogin raises GlueEnterWorld with the character");

        // The realm list: two realms, one with a version, positioned at the count.
        StrWatch                      realms;
        const wxl_subscription realmSub = g_host->Events().Subscribe(WXL_EV_GlueRealm, &OnRealm, &realms, 0);
        Body realmList;
        realmList.U16(2)
            .U8(1).U8(0).U8(0x4).Str("Dawnlight").Str("127.0.0.1:8085").F32(1.5f).U8(3).U8(2).U8(1).U8(3).U8(3).U8(5).U16(12340)
            .U8(0).U8(1).U8(0x2).Str("Old").Str("10.0.0.1:8085").F32(0.0f).U8(0).U8(1).U8(2);
        store = realmList.Store();
        FakeLogin grunt;
        g_received.clear();
        wxl::seam::Login::RealmListResult::Before(&grunt, &store);
        Check(g_received.size() == 3 && g_received[0].event == WXL_EV_GlueRealmList &&
                  g_received[0].As<wxl_realm_list_args>().count == 2,
              "Login.RealmListResult raises GlueRealmList with the count, then GlueRealm per entry");
        if (g_received.size() == 3)
        {
            const wxl_realm_args first  = g_received[1].As<wxl_realm_args>();
            const wxl_realm_args second = g_received[2].As<wxl_realm_args>();
            Check(first.index == 0 && first.type == 1 && first.flags == 0x4 && first.locked == WXL_FALSE &&
                      first.population == 1.5f && first.characters == 3 && first.category == 2 && first.realm_id == 1 &&
                      first.version_major == 3 && first.version_build == 12340,
                  "the first with its type, flags, population, characters, category, id and version");
            Check(second.index == 1 && second.locked == WXL_TRUE && second.realm_id == 2 && second.version_major == 0 &&
                      realms.strings.size() == 4 && realms.strings[0] == "Dawnlight" &&
                      realms.strings[1] == "127.0.0.1:8085" && realms.strings[2] == "Old",
                  "the second locked and without a version, and the names and addresses");
        }

        // The list BattlenetLogin::BuildRealmListResult writes: the region, the battlegroup, the
        // realm id and the build as dwords where a Grunt list has the id's byte.
        Body battlenetList;
        battlenetList.U16(1)
            .U8(0).U8(0).U8(0x4).Str("Lordaeron").Str("10.1.0.1:3724").F32(400.0f).U8(1).U8(9)
            .U32(2).U32(14).U32(301).U32(12340).U8(3).U8(3).U8(5).U16(12340)
            .U16(0);
        store = battlenetList.Store();
        FakeLogin battlenet;
        battlenet.serverType = 1;
        realms.strings.clear();
        g_received.clear();
        wxl::seam::Login::RealmListResult::Before(&battlenet, &store);
        Check(g_received.size() == 2 && g_received[0].event == WXL_EV_GlueRealmList &&
                  g_received[0].As<wxl_realm_list_args>().count == 1,
              "Login.RealmListResult reads a Battle.net login's list too");
        if (g_received.size() == 2)
        {
            const wxl_realm_args realm = g_received[1].As<wxl_realm_args>();
            Check(realm.region_id == 2 && realm.battlegroup_id == 14 && realm.realm_id == 301 &&
                      realm.build == 12340 && realm.category == 9 && realm.version_major == 3 &&
                      realm.version_build == 12340 && realms.strings.size() == 2 &&
                      realms.strings[0] == "Lordaeron",
                  "with its region, battlegroup, dword realm id, build and version");
        }

        // The layout follows the login, not the bytes: a Grunt login reads the id's byte.
        store = battlenetList.Store();
        g_received.clear();
        wxl::seam::Login::RealmListResult::Before(&grunt, &store);
        Check(g_received.size() == 2 && g_received[1].As<wxl_realm_args>().realm_id == 2 &&
                  g_received[1].As<wxl_realm_args>().region_id == 0,
              "a Grunt login reads the id's byte and no Battle.net members");

        store = realmList.Store();
        g_received.clear();
        wxl::seam::Login::RealmListResult::Before(nullptr, &store);
        Check(g_received.empty(), "and a login the binding cannot ask raises nothing");
        g_host->Events().Unsubscribe(realmSub);
    }

    void WXL_CALL OnChat(void* user, const void* args)
    {
        StrWatch&            watch = *static_cast<StrWatch*>(user);
        const wxl_chat_args& chat  = *static_cast<const wxl_chat_args*>(args);
        ++watch.calls;
        watch.strings = { std::string(chat.sender_name.data, chat.sender_name.size),
                          std::string(chat.target_name.data, chat.target_name.size),
                          std::string(chat.channel.data, chat.channel.size),
                          std::string(chat.text.data, chat.text.size),
                          std::string(chat.shown_text.data, chat.shown_text.size) };
    }

    /// What the chat-send handler saw and whether it suppressed the message.
    struct SendWatch
    {
        StrWatch strings;
        bool     suppress = false;
    };

    void WXL_CALL OnChatSend(void* user, const void* args)
    {
        SendWatch&                watch = *static_cast<SendWatch*>(user);
        const wxl_chat_send_args& send  = *static_cast<const wxl_chat_send_args*>(args);
        ++watch.strings.calls;
        watch.strings.strings = { std::string(send.text.data, send.text.size),
                                  std::string(send.type.data, send.type.size),
                                  std::string(send.language.data, send.language.size),
                                  std::string(send.target.data, send.target.size),
                                  std::string(send.prefix.data, send.prefix.size),
                                  send.addon ? "addon" : "chat" };
        if (watch.suppress) *send.suppressed = WXL_TRUE;
    }

    /// What the client does with a message it reads, up to AddChatMessage handing over the text
    /// where it lies in the message. A null `text` is a message ChatMessageReceived drops.
    const char* AddReceived(FakeStore& store, const char* text, int32_t shown_type, uint64_t sender_guid,
                            uint8_t is_gm, uint32_t language = 0)
    {
        if (!text) return nullptr;
        const uint8_t* at = std::search(store.data, store.data + store.size, text, text + strlen(text));
        const char*    in = reinterpret_cast<const char*>(at);
        wxl::seam::Chat::AddChatMessage::Before(in, shown_type, nullptr, language, nullptr, nullptr, "", sender_guid, 0,
                                                0, 0, is_gm, nullptr);
        return in;
    }

    /// The client committing the line AddChatMessage is adding: RecordChat with the line as
    /// the frames get it.
    void Commit(const char* shown, int32_t type, uint64_t sender_guid, uint32_t line_id,
                const char* sender_name = nullptr)
    {
        wxl::seam::Chat::RecordChat::Before(shown, type, sender_name, sender_guid, 0, 0, nullptr, line_id, nullptr);
    }

    /// The client holding the line AddChatMessage is adding: QueueChatText appends `line`.
    void Hold(FakePendingChat& line, const char* copy, int32_t type, uint64_t sender_guid, uint32_t language = 0)
    {
        line.text     = copy;
        g_pendingLast = &line;
        wxl::seam::Chat::QueueChatText::After(copy, type, nullptr, nullptr, language, nullptr, "", 0, 0, sender_guid, 0,
                                              0, 0, 0, nullptr);
    }

    /// The client adding a held line again from its copy.
    void AddHeld(const FakePendingChat& line, int32_t type, uint64_t sender_guid, uint32_t language = 0)
    {
        wxl::seam::Chat::AddChatMessage::Before(line.text, type, nullptr, language, nullptr, nullptr, "", sender_guid, 0,
                                                0, 0, 0, nullptr);
    }

    /// A message the client reads and shows at once: read, added, committed.
    void ReceiveChat(FakeStore& store, uint8_t is_gm, const char* text, int32_t shown_type,
                     uint64_t sender_guid)
    {
        wxl::seam::Chat::ChatMessageReceived::Before(&store, is_gm);
        if (AddReceived(store, text, shown_type, sender_guid, is_gm)) Commit(text, shown_type, sender_guid, 1);
        wxl::seam::Chat::ChatMessageReceived::After(1, &store, is_gm);
    }

    void CheckChat()
    {
        FakeObject orc;
        orc.guid   = 0x0000000000000ABCull;
        orc.typeId = WXL_OBJECT_PLAYER;
        wxl::seam::ObjectMgr::InitObject::After(&orc, nullptr, 9200u);
        const wxl_handle orcHandle = wxl::core::objects::HandleOf(&orc);

        StrWatch                      watch;
        const wxl_subscription sub = g_host->Events().Subscribe(WXL_EV_ChatMessage, &OnChat, &watch, 0);

        // A say: the default layout.
        Body say;
        say.U8(1).U32(7).U64(orc.guid).U32(0).U64(0).Sized("Lok'tar").U8(0);
        FakeStore store = say.Store();
        g_received.clear();
        ReceiveChat(store, 0, "Lok'tar", 1, orc.guid);
        const Received* said = Only(WXL_EV_ChatMessage);
        Check(said != nullptr, "Chat.RecordChat raises ChatMessage once for a say the client shows");
        if (said)
        {
            const wxl_chat_args args = said->As<wxl_chat_args>();
            Check(args.type == 1 && args.language == 7 && args.sender == orcHandle && args.sender_guid == orc.guid &&
                      args.tag == 0 && args.gm == WXL_FALSE && args.line_id == 1 && watch.strings[3] == "Lok'tar" &&
                      watch.strings[4] == "Lok'tar" && watch.strings[0].empty(),
                  "with the type, the language, the sender, the text, the line and no name");
        }
        Check(store.read == 0u, "reading the message left the store's cursor where it was");

        // A channel message with a chat tag, as a GM message with the sender's name.
        Body channel;
        channel.U8(17).U32(0).U64(orc.guid).U32(0).Sized("Thrall").Str("General").U64(0).Sized("hello").U8(0x4);
        store = channel.Store();
        g_received.clear();
        ReceiveChat(store, 1, "hello", 17, orc.guid);
        Check(Only(WXL_EV_ChatMessage) && Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().gm == WXL_TRUE &&
                  Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().tag == 0x4 && watch.strings[0] == "Thrall" &&
                  watch.strings[2] == "General" && watch.strings[3] == "hello",
              "a GM channel message carries the sender's name, the channel and the tag");

        // A monster yell at a creature, whose name the message carries; a player's it does not.
        Body yell;
        yell.U8(14).U32(0).U64(0xF130000000000123ull).U32(0).Sized("Hogger").U64(0xF130000000000456ull).Sized("Gnoll")
            .Sized("Grrr!").U8(0);
        store = yell.Store();
        g_received.clear();
        ReceiveChat(store, 0, "Grrr!", 14, 0xF130000000000123ull);
        Check(Only(WXL_EV_ChatMessage) &&
                  Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().target_guid == 0xF130000000000456ull &&
                  watch.strings[0] == "Hogger" && watch.strings[1] == "Gnoll" && watch.strings[3] == "Grrr!",
              "a monster yell carries the creature's name, the target and its name");
        Body yellAtPlayer;
        yellAtPlayer.U8(14).U32(0).U64(0xF130000000000123ull).U32(0).Sized("Hogger").U64(orc.guid).Sized("Grrr!")
            .U8(0);
        store = yellAtPlayer.Store();
        g_received.clear();
        ReceiveChat(store, 0, "Grrr!", 14, 0xF130000000000123ull);
        Check(Only(WXL_EV_ChatMessage) && Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().target_guid == orc.guid &&
                  watch.strings[1].empty() && watch.strings[3] == "Grrr!",
              "and at a player, no target name");

        // An achievement message, and a message cut short.
        Body achievement;
        achievement.U8(48).U32(0).U64(orc.guid).U32(0).U64(0).Sized("earned").U8(0).U32(1234);
        store = achievement.Store();
        g_received.clear();
        ReceiveChat(store, 0, "earned", 48, orc.guid);
        Check(Only(WXL_EV_ChatMessage) && Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().achievement_id == 1234,
              "an achievement message carries its id");
        Body cut;
        cut.U8(1).U32(7).U64(orc.guid).U32(0).U64(0).U32(50);
        store = cut.Store();
        g_received.clear();
        ReceiveChat(store, 0, nullptr, 0, orc.guid);
        Check(g_received.empty(), "a message cut short raises nothing");

        // A foreign whisper: 8 on the wire, shown as a whisper, which is the type raised.
        Body foreign;
        foreign.U8(8).U32(1).U64(orc.guid).U32(0).Sized("Thrall").U64(0).Sized("psst").U8(0);
        store = foreign.Store();
        g_received.clear();
        ReceiveChat(store, 0, "psst", 7, orc.guid);
        Check(Only(WXL_EV_ChatMessage) && Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().type == 7 &&
                  watch.strings[0] == "Thrall" && watch.strings[3] == "psst",
              "a foreign whisper is raised as the whisper the client shows");

        // A message ChatMessageReceived drops (an ignored sender): read, never added, never raised.
        store = say.Store();
        g_received.clear();
        ReceiveChat(store, 0, nullptr, 0, orc.guid);
        Check(g_received.empty(), "a message the client drops raises nothing");

        // A line AddChatMessage drops (the spam filter, the UTF-8 check): added, never committed.
        store = say.Store();
        g_received.clear();
        wxl::seam::Chat::ChatMessageReceived::Before(&store, 0);
        AddReceived(store, "Lok'tar", 1, orc.guid, 0);
        Check(g_received.empty(), "Chat.AddChatMessage raises nothing itself");
        wxl::seam::Chat::ChatMessageReceived::After(1, &store, 0);
        Commit("Lok'tar", 1, orc.guid, 2);
        Check(g_received.empty(), "a line the client's own filters drop is never raised, not even by a later commit");

        // The line as the frames get it: the name from the cache, the final type, the line id,
        // the text translated and filtered.
        store = say.Store();
        g_received.clear();
        wxl::seam::Chat::ChatMessageReceived::Before(&store, 0);
        AddReceived(store, "Lok'tar", 1, orc.guid, 0);
        Commit("Kek'ta", 10, orc.guid, 77, "Thrall");
        wxl::seam::Chat::ChatMessageReceived::After(1, &store, 0);
        Check(Only(WXL_EV_ChatMessage) && Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().type == 10 &&
                  Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().line_id == 77 && watch.strings[0] == "Thrall" &&
                  watch.strings[3] == "Lok'tar" && watch.strings[4] == "Kek'ta",
              "the committed line carries its final type, its number, the name it shows and the text as shown");

        // The client's own lines raise nothing, inside a received message or outside one.
        g_received.clear();
        wxl::seam::Chat::AddChatMessage::Before("You are now AFK.", 0, nullptr, 0, nullptr, nullptr, "", 0, 0, 0, 0,
                                                0, nullptr);
        Commit("You are now AFK.", 0, 0, 3);
        store = say.Store();
        wxl::seam::Chat::ChatMessageReceived::Before(&store, 0);
        wxl::seam::Chat::AddChatMessage::Before("Lok'tar", 1, nullptr, 0, nullptr, nullptr, "", orc.guid, 0, 0, 0, 0,
                                                nullptr);
        Commit("Lok'tar", 1, orc.guid, 4);
        wxl::seam::Chat::ChatMessageReceived::After(1, &store, 0);
        Check(g_received.empty(), "a line that is not the received message's own text raises nothing");

        // A line the client holds until it knows the name: nothing while it is held, the line
        // once the client adds its copy again and commits it, with the name it then shows.
        FakePendingChat held;
        char            copy[] = "Lok'tar";
        store = say.Store();
        g_received.clear();
        wxl::seam::Chat::ChatMessageReceived::Before(&store, 0);
        AddReceived(store, "Lok'tar", 1, orc.guid, 0);
        Hold(held, copy, 1, orc.guid);
        wxl::seam::Chat::ChatMessageReceived::After(1, &store, 0);
        store = FakeStore{};
        Check(g_received.empty(), "Chat.QueueChatText: a line the client holds raises nothing yet");
        AddHeld(held, 1, orc.guid);
        Commit(copy, 1, orc.guid, 5, "Thrall");
        wxl::seam::Chat::DestroyPendingChat::Before(&held);
        Check(Only(WXL_EV_ChatMessage) && Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().line_id == 5 &&
                  Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().language == 7 && watch.strings[0] == "Thrall" &&
                  watch.strings[3] == "Lok'tar",
              "the held line raises when the client adds it again, with what the message carried and the name");
        g_received.clear();
        AddHeld(held, 1, orc.guid);
        Commit(copy, 1, orc.guid, 6);
        Check(g_received.empty(), "and only once");

        // Held, added again and held again (the name still unknown), then shown.
        FakePendingChat again;
        char            secondCopy[] = "Lok'tar";
        store = say.Store();
        g_received.clear();
        wxl::seam::Chat::ChatMessageReceived::Before(&store, 0);
        AddReceived(store, "Lok'tar", 1, orc.guid, 0);
        Hold(held, copy, 1, orc.guid);
        wxl::seam::Chat::ChatMessageReceived::After(1, &store, 0);
        AddHeld(held, 1, orc.guid);
        Hold(again, secondCopy, 1, orc.guid);
        wxl::seam::Chat::DestroyPendingChat::Before(&held);
        AddHeld(again, 1, orc.guid);
        Commit(secondCopy, 1, orc.guid, 7);
        wxl::seam::Chat::DestroyPendingChat::Before(&again);
        Check(Only(WXL_EV_ChatMessage) && Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().line_id == 7,
              "a line held twice raises once, when it is shown");

        // Held, then given up on (a name query that failed): freed, never shown, never raised,
        // even if a later line comes with a copy at the same address.
        store = say.Store();
        g_received.clear();
        wxl::seam::Chat::ChatMessageReceived::Before(&store, 0);
        AddReceived(store, "Lok'tar", 1, orc.guid, 0);
        Hold(held, copy, 1, orc.guid);
        wxl::seam::Chat::ChatMessageReceived::After(1, &store, 0);
        wxl::seam::Chat::DestroyPendingChat::Before(&held);
        AddHeld(held, 1, orc.guid);
        Commit(copy, 1, orc.guid, 8);
        Check(g_received.empty(), "Chat.DestroyPendingChat: a held line the client frees raises nothing");

        // The client's own line, held: nothing kept, nothing raised.
        g_received.clear();
        wxl::seam::Chat::AddChatMessage::Before(copy, 0, nullptr, 0, nullptr, nullptr, "", orc.guid, 0, 0, 0, 0,
                                                nullptr);
        Hold(held, copy, 0, orc.guid);
        AddHeld(held, 0, orc.guid);
        Commit(copy, 0, orc.guid, 9);
        wxl::seam::Chat::DestroyPendingChat::Before(&held);
        Check(g_received.empty(), "a line of the client's own, held and shown, raises nothing");
        g_pendingLast = nullptr;
        g_host->Events().Unsubscribe(sub);

        // The gates: the script's arguments off the Lua stack, suppressed or not.
        SendWatch                     send;
        const wxl_subscription sendSub = g_host->Events().Subscribe(WXL_EV_ChatSend, &OnChatSend, &send, 0);
        g_vm.stack = { String("hello"), String("SAY"), FakeValue{}, FakeValue{} };
        int32_t result = 7;
        void*   state  = &g_vm;
        Check(!wxl::seam::Chat::Script_SendChatMessage::Gate(&result, &state) && result == 7,
              "Chat.Script_SendChatMessage lets the call through when nothing suppresses it");
        Check(send.strings.calls == 1 && send.strings.strings.size() == 6 && send.strings.strings[0] == "hello" &&
                  send.strings.strings[1] == "SAY" && send.strings.strings[2].empty() && send.strings.strings[3].empty() &&
                  send.strings.strings[5] == "chat",
              "having raised ChatSend with the text, the type and the absent arguments empty");
        send.suppress = true;
        g_vm.stack    = { String("psst"), String("WHISPER"), String("Orcish"), String("Thrall") };
        Check(wxl::seam::Chat::Script_SendChatMessage::Gate(&result, &state) && result == 0,
              "a suppressed message takes the call and answers no result");
        Check(send.strings.strings[2] == "Orcish" && send.strings.strings[3] == "Thrall",
              "with the language and the target as the script passed them");
        g_vm.stack = { String("MyAddon"), String("payload"), String("GUILD"), FakeValue{} };
        Check(wxl::seam::Chat::Script_SendAddonMessage::Gate(&result, &state) && send.strings.strings[4] == "MyAddon" &&
                  send.strings.strings[0] == "payload" && send.strings.strings[1] == "GUILD" &&
                  send.strings.strings[5] == "addon",
              "Chat.Script_SendAddonMessage raises ChatSend as an addon message with its prefix");
        g_vm.stack.clear();
        g_host->Events().Unsubscribe(sendSub);

        // SMSG_LOG_XPGAIN: a kill with its bonus and rate, then a quest.
        Body kill;
        kill.U64(0xF130000000000123ull).U32(150).U8(0).U32(30).F32(0.5f).U8(1);
        store = kill.Store();
        g_received.clear();
        wxl::seam::Chat::Packet_SMSG_LOG_XPGAIN::Before(nullptr, 0x1D0, 0, &store);
        const Received* gained = Only(WXL_EV_PlayerXpGain);
        Check(gained != nullptr, "Chat.Packet_SMSG_LOG_XPGAIN raises PlayerXpGain once");
        if (gained)
        {
            const wxl_xp_gain_args args = gained->As<wxl_xp_gain_args>();
            Check(args.source_guid == 0xF130000000000123ull && args.source == WXL_HANDLE_NONE && args.amount == 150 &&
                      args.kind == 0 && args.bonus == 30 && args.group_multiplier == 0.5f && args.refer_a_friend == WXL_TRUE,
                  "with the source, the amount, the kind, the bonus, the rate and the flag");
        }
        Body quest;
        quest.U64(orc.guid).U32(500).U8(1).U8(0);
        store = quest.Store();
        g_received.clear();
        wxl::seam::Chat::Packet_SMSG_LOG_XPGAIN::Before(nullptr, 0x1D0, 0, &store);
        Check(Only(WXL_EV_PlayerXpGain) && Only(WXL_EV_PlayerXpGain)->As<wxl_xp_gain_args>().kind == 1 &&
                  Only(WXL_EV_PlayerXpGain)->As<wxl_xp_gain_args>().group_multiplier == 1.0f &&
                  Only(WXL_EV_PlayerXpGain)->As<wxl_xp_gain_args>().source == orcHandle,
              "a gain that is not a kill carries no bonus and a rate of 1");

        wxl::seam::ObjectMgr::ObjDelete::Before(&orc);
    }

    void CheckEnergize()
    {
        FakeObject priest;
        priest.guid   = 0x0000000000000ABCull;
        priest.typeId = WXL_OBJECT_PLAYER;
        wxl::seam::ObjectMgr::InitObject::After(&priest, nullptr, 9300u);
        const wxl_handle priestHandle = wxl::core::objects::HandleOf(&priest);

        // SMSG_SPELLENERGIZELOG: the target first, then the caster.
        Body energize;
        energize.Packed(priest.guid).Packed(priest.guid).U32(29166).U32(0).U32(300);
        FakeStore store = energize.Store();
        g_received.clear();
        wxl::seam::Combat::HandleSpellLogEnergizeLog::Before(&store, 0);
        const Received* gained = Only(WXL_EV_CombatEnergize);
        Check(gained != nullptr, "Combat.HandleSpellLogEnergizeLog raises CombatEnergize once");
        if (gained)
        {
            const wxl_combat_energize_args args = gained->As<wxl_combat_energize_args>();
            Check(args.source == priestHandle && args.target == priestHandle && args.spell_id == 29166 &&
                      args.power_type == 0 && args.amount == 300 && args.periodic == WXL_FALSE && args.drain == WXL_FALSE,
                  "with the caster, the target, the spell, the power and the amount");
        }

        // SMSG_PERIODICAURALOG: an energize tick and a drain tick.
        Body ticks;
        ticks.Packed(priest.guid).Packed(priest.guid).U32(589).U32(2).U32(21).U32(3).U32(20).U32(64).U32(0).U32(50).F32(2.0f);
        store = ticks.Store();
        g_received.clear();
        wxl::seam::Combat::HandlePeriodicAuraLog::Before(&store, 0);
        Check(g_received.size() == 2 && Count(WXL_EV_CombatEnergize) == 2,
              "Combat.HandlePeriodicAuraLog raises CombatEnergize for an energize and a drain tick");
        if (g_received.size() == 2)
        {
            const wxl_combat_energize_args tick  = g_received[0].As<wxl_combat_energize_args>();
            const wxl_combat_energize_args drain = g_received[1].As<wxl_combat_energize_args>();
            Check(tick.periodic == WXL_TRUE && tick.drain == WXL_FALSE && tick.power_type == 3 && tick.amount == 20 &&
                      tick.spell_id == 589,
                  "the energize tick as periodic, with its power and amount");
            Check(drain.drain == WXL_TRUE && drain.power_type == 0 && drain.amount == 50 && drain.multiplier == 2.0f,
                  "the drain tick with its multiplier");
        }

        wxl::seam::ObjectMgr::ObjDelete::Before(&priest);
    }

    // --- FrameScript: the UI events the client fires, and the errors its Lua raises --------

    /// What the UI event handler below saw, copied out while the args were valid.
    struct UiWatch
    {
        int         calls = 0;
        uint32_t    index = 0;
        std::string name;
        std::string format;
        std::string values;
        uint32_t    count = 0;
    };

    void WXL_CALL OnUiEvent(void* user, const void* args)
    {
        UiWatch&                 watch = *static_cast<UiWatch*>(user);
        const wxl_ui_event_args& event = *static_cast<const wxl_ui_event_args*>(args);
        ++watch.calls;
        watch.index  = event.index;
        watch.name   = std::string(event.name.data, event.name.size);
        watch.format = std::string(event.format.data ? event.format.data : "", event.format.size);
        watch.values = std::string(static_cast<const char*>(event.values ? event.values : ""),
                                   event.values_size);
        watch.count  = event.value_count;
    }

    /// Fires an event the way the client's variadic wrapper does: through a va_list.
    void Fire(uint32_t index, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        wxl::seam::FrameScript::vSignalEvent::Before(index, format, args);
        va_end(args);
    }

    void CheckUiEvent()
    {
        // A game context with the client's event table installed.
        g_vm.live       = true;
        g_uiLoggingIn   = 1;
        g_uiInitialized = 0;
        wxl::seam::FrameScript::Initialize::After(1, 0);
        static const char* names[] = { "PLAYER_LOGIN", "CHAT_MSG_SAY", "UNIT_HEALTH" };
        wxl::seam::FrameScript::CreateEvents::After(names, 3);

        UiWatch                watch;
        const wxl_subscription sub =
            g_host->Events().Subscribe(WXL_EV_UiEvent, &OnUiEvent, &watch, 0);

        Fire(1, "%s%d%f%b%u%s", "hello", -5, 2.5, 1, 7u, static_cast<const char*>(nullptr));
        Check(watch.calls == 1 && watch.index == 1 && watch.name == "CHAT_MSG_SAY",
              "FrameScript.vSignalEvent raises UiEvent once, naming the event at the index");
        const std::string expected("shello\0d-5\0f2.5\0b1\0u7\0n\0", 24);
        Check(watch.format == "%s%d%f%b%u%s" && watch.count == 6 && watch.values == expected,
              "with the format and every argument rendered by its kind, a null string as nil");

        Fire(0, nullptr);
        Check(watch.calls == 2 && watch.name == "PLAYER_LOGIN" && watch.count == 0 &&
                  watch.values.empty() && watch.format.empty(),
              "an event without arguments carries none");

        Fire(2, "%x%d", 3);
        Check(watch.calls == 3 && watch.count == 1 && watch.values == std::string("d3\0", 3),
              "a letter the client does not read reads nothing here either");

        Fire(9, "%d", 1);
        Check(watch.calls == 3, "an index outside the installed table raises nothing");

        g_host->Events().Unsubscribe(sub);
        wxl::seam::FrameScript::Destroy::Before();
        g_vm.live = false;
    }

    void WXL_CALL OnChatAddon(void* user, const void* args)
    {
        StrWatch&                  watch = *static_cast<StrWatch*>(user);
        const wxl_chat_addon_args& addon = *static_cast<const wxl_chat_addon_args*>(args);
        ++watch.calls;
        watch.strings = { std::string(addon.prefix.data, addon.prefix.size),
                          std::string(addon.text.data, addon.text.size),
                          std::string(addon.channel.data, addon.channel.size),
                          std::string(addon.sender_name.data, addon.sender_name.size) };
    }

    /// CHAT_MSG_ADDON fired as AddChatMessage fires it: the prefix, the text, the channel and
    /// the sender's name.
    void DeliverAddon(const char* prefix, const char* text, const char* channel, const char* sender)
    {
        Fire(1, "%s%s%s%s", prefix, text, channel, sender);
    }

    void CheckChatAddon()
    {
        g_vm.live       = true;
        g_uiLoggingIn   = 1;
        g_uiInitialized = 0;
        wxl::seam::FrameScript::Initialize::After(1, 0);
        static const char* names[] = { "CHAT_MSG_SAY", "CHAT_MSG_ADDON" };
        wxl::seam::FrameScript::CreateEvents::After(names, 2);

        FakeObject orc;
        orc.guid   = 0x0000000000000ABDull;
        orc.typeId = WXL_OBJECT_PLAYER;
        wxl::seam::ObjectMgr::InitObject::After(&orc, nullptr, 9200u);
        const wxl_handle orcHandle = wxl::core::objects::HandleOf(&orc);

        StrWatch               watch;
        const wxl_subscription sub = g_host->Events().Subscribe(WXL_EV_ChatAddonMessage, &OnChatAddon, &watch, 0);

        // A party addon message: the addon language, the prefix and the text joined by a tab.
        Body party;
        party.U8(2).U32(0xFFFFFFFFu).U64(orc.guid).U32(0).U64(0).Sized("MyAddon\tping").U8(0);
        FakeStore store = party.Store();
        g_received.clear();
        wxl::seam::Chat::ChatMessageReceived::Before(&store, 0);
        AddReceived(store, "MyAddon\tping", 2, orc.guid, 0, 0xFFFFFFFFu);
        Check(g_received.empty(), "an addon message raises nothing before the client delivers it");
        DeliverAddon("MyAddon", "ping", "PARTY", "Thrall");
        wxl::seam::Chat::ChatMessageReceived::After(1, &store, 0);
        const Received* addon = Only(WXL_EV_ChatAddonMessage);
        Check(addon != nullptr && Count(WXL_EV_ChatMessage) == 0,
              "FrameScript.vSignalEvent raises ChatAddonMessage once at CHAT_MSG_ADDON, and no ChatMessage");
        if (addon)
        {
            const wxl_chat_addon_args args = addon->As<wxl_chat_addon_args>();
            Check(args.type == 2 && args.sender == orcHandle && args.sender_guid == orc.guid && args.gm == WXL_FALSE &&
                      watch.strings[0] == "MyAddon" && watch.strings[1] == "ping" && watch.strings[2] == "PARTY" &&
                      watch.strings[3] == "Thrall",
                  "with the type, the sender and the strings the interface receives");
        }

        // An addon message the client holds for the sender's name, delivered when it adds the copy.
        FakePendingChat held;
        char            copy[] = "MyAddon\tping";
        store = party.Store();
        g_received.clear();
        wxl::seam::Chat::ChatMessageReceived::Before(&store, 0);
        AddReceived(store, "MyAddon\tping", 2, orc.guid, 0, 0xFFFFFFFFu);
        Hold(held, copy, 2, orc.guid, 0xFFFFFFFFu);
        wxl::seam::Chat::ChatMessageReceived::After(1, &store, 0);
        Check(g_received.empty(), "a held addon message raises nothing yet");
        AddHeld(held, 2, orc.guid, 0xFFFFFFFFu);
        DeliverAddon("MyAddon", "ping", "PARTY", "Thrall");
        wxl::seam::Chat::DestroyPendingChat::Before(&held);
        Check(Only(WXL_EV_ChatAddonMessage) && watch.strings[3] == "Thrall",
              "and is raised when the client delivers it, with the name it then has");

        // CHAT_MSG_ADDON outside a received addon message, and another event inside one.
        g_received.clear();
        wxl::seam::Chat::AddChatMessage::Before("Own\tline", 2, nullptr, 0xFFFFFFFFu, nullptr, nullptr, "", 0, 0, 0, 0,
                                                0, nullptr);
        DeliverAddon("Own", "line", "PARTY", "");
        store = party.Store();
        wxl::seam::Chat::ChatMessageReceived::Before(&store, 0);
        AddReceived(store, "MyAddon\tping", 2, orc.guid, 0, 0xFFFFFFFFu);
        Fire(0, "%s%s%s%s", "MyAddon", "ping", "PARTY", "Thrall");
        wxl::seam::Chat::ChatMessageReceived::After(1, &store, 0);
        Check(Count(WXL_EV_ChatAddonMessage) == 0,
              "an addon line of the client's own, or another event, raises no ChatAddonMessage");

        // A chat line is not an addon message: CHAT_MSG_ADDON cannot be its outcome.
        Body say;
        say.U8(1).U32(7).U64(orc.guid).U32(0).U64(0).Sized("hello").U8(0);
        store = say.Store();
        g_received.clear();
        wxl::seam::Chat::ChatMessageReceived::Before(&store, 0);
        AddReceived(store, "hello", 1, orc.guid, 0, 7);
        DeliverAddon("hello", "", "UNKNOWN", "");
        wxl::seam::Chat::ChatMessageReceived::After(1, &store, 0);
        Check(Count(WXL_EV_ChatAddonMessage) == 0, "a message in a spoken language raises no ChatAddonMessage");

        g_pendingLast = nullptr;
        g_host->Events().Unsubscribe(sub);
        wxl::seam::ObjectMgr::ObjDelete::Before(&orc);
        wxl::seam::FrameScript::Destroy::Before();
        g_vm.live = false;
    }

    void CheckScriptError()
    {
        g_vm.live       = true;
        g_uiLoggingIn   = 0;
        g_uiInitialized = 0;
        wxl::seam::FrameScript::Initialize::After(1, 0);

        g_vm.stack.clear();
        g_vm.stack.push_back(String("Interface\\FrameXML\\Boom.lua:12: attempt to call nil"));
        g_received.clear();
        wxl::seam::FrameScript::HandleError::Before(&g_vm);
        const Received* error = Only(WXL_EV_ScriptError);
        Check(error != nullptr, "FrameScript.HandleError raises ScriptError once");
        if (error)
        {
            const wxl_script_error_args args = error->As<wxl_script_error_args>();
            Check(args.context == WXL_SCRIPT_CONTEXT_GLUE, "naming the live context");
            Check(std::string(args.message.data, args.message.size) ==
                      "Interface\\FrameXML\\Boom.lua:12: attempt to call nil",
                  "and carrying the message on top of the stack");
        }

        g_vm.stack.clear();
        g_vm.stack.push_back(Number(42));
        g_received.clear();
        wxl::seam::FrameScript::HandleError::Before(&g_vm);
        Check(Only(WXL_EV_ScriptError) != nullptr &&
                  std::string(Only(WXL_EV_ScriptError)->As<wxl_script_error_args>().message.data) == "42",
              "a number reads as its text");

        g_vm.stack.clear();
        g_vm.stack.push_back(FakeValue{});
        g_received.clear();
        wxl::seam::FrameScript::HandleError::Before(&g_vm);
        Check(Only(WXL_EV_ScriptError) != nullptr &&
                  std::string(Only(WXL_EV_ScriptError)->As<wxl_script_error_args>().message.data) ==
                      "UNKNOWN ERROR",
              "anything else is what the client will report it as");

        g_vm.stack.clear();
        wxl::seam::FrameScript::Destroy::Before();
        g_vm.live = false;
    }

    // --- wxl.footprint: Storm's allocations, the client's consumers, the address space -------

    std::string_view Text(wxl_str text) { return std::string_view(text.data ? text.data : "", text.size); }

    /// The source row of a file in the last picture, or null.
    const wxl_footprint_source* SourceNamed(const std::vector<wxl_footprint_source>& rows, const char* file)
    {
        for (const wxl_footprint_source& row : rows)
            if (row.file.size == std::strlen(file) && std::memcmp(row.file.data, file, row.file.size) == 0) return &row;
        return nullptr;
    }

    const wxl_footprint_category* CategoryNamed(const std::vector<wxl_footprint_category>& rows, const char* name)
    {
        for (const wxl_footprint_category& row : rows)
            if (row.name.size == std::strlen(name) && std::memcmp(row.name.data, name, row.name.size) == 0) return &row;
        return nullptr;
    }

    template <class T>
    std::vector<T> Listed(uint32_t (WXL_CALL* list)(T*, uint32_t))
    {
        std::vector<T> rows(list(nullptr, 0));
        for (T& row : rows) row.struct_size = sizeof(T);
        rows.resize(list(rows.data(), static_cast<uint32_t>(rows.size())) < rows.size() ? 0 : rows.size());
        return rows;
    }

    void CheckFootprint()
    {
        namespace Storm = wxl::seam::Storm;
        const wxl_footprint_api& api = wxl::core::services::FootprintApi();
        wxl::core::services::footprint::Reset();

        static const char kClient[] = ".\\Client.cpp";
        static const char kNew[]    = "new";
        void* const       a         = reinterpret_cast<void*>(0x10001000);
        void* const       b         = reinterpret_cast<void*>(0x10002000);
        void* const       c         = reinterpret_cast<void*>(0x10003000);

        Storm::SMemAlloc::After(a, 100, kClient, 10, 0);
        Check(api.running() == WXL_FALSE, "accounting is off until a mod starts it: an allocation counts nothing");
        api.snapshot();
        Check(Listed(api.sources).empty() && !CategoryNamed(Listed(api.categories), "storm"),
              "and a picture taken then has no source and no storm category");

        Check(api.start() == WXL_OK && api.running() == WXL_TRUE, "start begins accounting");
        Storm::SMemAlloc::After(a, 100, kClient, 10, 0);  // 104 with Storm's rounding
        Storm::SMemAlloc::After(b, 8, kClient, 11, 0);
        Storm::SMemAlloc::After(c, 1000, kNew, -1, 0);
        Storm::SMemFree::Before(b, "free", 0, 0);
        Storm::SMemFree::Before(reinterpret_cast<void*>(0x20000000), "free", 0, 0); // never counted
        api.snapshot();
        std::vector<wxl_footprint_source> sources = Listed(api.sources);
        const wxl_footprint_source*       client  = SourceNamed(sources, kClient);
        const wxl_footprint_source*       news    = SourceNamed(sources, kNew);
        Check(client && client->bytes == 104 && client->count == 1 && client->allocations == 2 && client->peak == 112,
              "a source counts its live bytes rounded to 8, its live blocks, every allocation and its peak");
        Check(news && news->bytes == 1000 && sources.front().bytes == 1000,
              "each file is its own row, and the largest comes first");
        const std::vector<wxl_footprint_category> categories = Listed(api.categories);
        const wxl_footprint_category*             storm      = CategoryNamed(categories, "storm");
        Check(storm && storm->bytes == 1104 && storm->count == 2 && storm->peak == 1112,
              "the storm category is every source together, with its own peak");

        // The client's arrays name their element type, decorated: a row spells it as C++ does.
        {
            static const char kPointers[] = ".PBVTaxiPathRec@@";
            static const char kBytes[]    = ".E";
            static const char kClass[]    = ".?AVCSimpleTexture@@";
            static const char kNested[]   = ".PAPAUInner@Outer@@";
            static const char kTemplate[] = ".?AV?$TSList@VX@@@@";
            void* const       typed[]     = {reinterpret_cast<void*>(0x10011000), reinterpret_cast<void*>(0x10012000),
                                             reinterpret_cast<void*>(0x10013000), reinterpret_cast<void*>(0x10014000),
                                             reinterpret_cast<void*>(0x10015000)};
            const char* const names[]     = {kPointers, kBytes, kClass, kNested, kTemplate};
            for (int i = 0; i < 5; ++i) Storm::SMemAlloc::After(typed[i], 8, names[i], -2, 0);
            api.snapshot();
            const std::vector<wxl_footprint_source> rows = Listed(api.sources);
            Check(SourceNamed(rows, "const TaxiPathRec*") && SourceNamed(rows, "unsigned char") &&
                      SourceNamed(rows, "CSimpleTexture") && SourceNamed(rows, "Outer::Inner**"),
                  "a type's decorated name reads as C++ spells it: pointers, const, scopes, basic types");
            Check(SourceNamed(rows, kTemplate) != nullptr, "one it cannot read is kept as the client wrote it");
            for (void* block : typed) Storm::SMemFree::Before(block, "free", 0, 0);
        }

        // SMemReAlloc: moved, failed, freed, and the calls that leave the block alone.
        void* const d = reinterpret_cast<void*>(0x10004000);
        Storm::SMemReAlloc::Before(a, 300, kNew, 0, 0);
        Storm::SMemReAlloc::After(d, a, 300, kNew, 0, 0);
        Storm::SMemReAlloc::Before(c, 4000, kNew, 0, 0);
        Storm::SMemReAlloc::After(nullptr, c, 4000, kNew, 0, 0); // the call failed: c stays
        Storm::SMemReAlloc::Before(c, 64, kNew, 0, 0x10);
        Storm::SMemReAlloc::After(nullptr, c, 64, kNew, 0, 0x10); // no move allowed: c stays
        api.snapshot();
        sources = Listed(api.sources);
        client  = SourceNamed(sources, kClient);
        news    = SourceNamed(sources, kNew);
        Check(client && client->bytes == 0 && client->count == 0 && news && news->bytes == 1304 && news->count == 2,
              "a block that moves is counted under the file that moved it; one a call left alone stays");
        Storm::SMemReAlloc::Before(d, 0, kNew, 0, 0);
        Storm::SMemReAlloc::After(nullptr, d, 0, kNew, 0, 0);
        api.snapshot();
        sources = Listed(api.sources);
        news    = SourceNamed(sources, kNew);
        Check(news && news->bytes == 1000 && news->count == 1, "and a resize to nothing frees it");

        // The client's consumers, read when the picture is taken.
        const std::vector<wxl_footprint_category> seen = Listed(api.categories);
        const wxl_footprint_category* managed = CategoryNamed(seen, "textures.managed");
        const wxl_footprint_category* target  = CategoryNamed(seen, "textures.default");
        const wxl_footprint_category* buffers = CategoryNamed(seen, "buffers.default");
        const wxl_footprint_category* sound   = CategoryNamed(seen, "sound");
        const wxl_footprint_category* script  = CategoryNamed(seen, "script");
        const wxl_footprint_category* paks    = CategoryNamed(seen, "archives");
        Check(managed && managed->bytes == 5120 && managed->count == 2 && target && target->bytes == 8192 && buffers &&
                  buffers->bytes == 65536,
              "the device's textures and buffers are summed by pool");
        Check(sound && sound->bytes == 3000000 && sound->peak == 5000000 && script && script->bytes == 12345678 &&
                  paks && paks->bytes == 2500000 && paks->count == 2,
              "the sound and script heaps and the archives' tables are categories of their own");
        Check(std::is_sorted(seen.begin(), seen.end(),
                             [](const wxl_footprint_category& x, const wxl_footprint_category& y) { return x.bytes > y.bytes; }),
              "categories come largest first");

        const std::vector<wxl_footprint_archive> archives = Listed(api.archives);
        Check(archives.size() == 2 && archives[1].priority == 30 && archives[1].hash_entries == 16384 &&
                  Text(archives[1].path) == "Data\\patch-3.MPQ",
              "each archive is listed with its priority, its tables and its path");

        // A database: a block Storm handed out while counting is known exactly; others are estimated.
        Storm::SMemAlloc::After(g_spellStrings, 20, kClient, 0, 0);
        api.snapshot();
        const std::vector<wxl_footprint_table> tables = Listed(api.tables);
        Check(tables.size() == 1 && Text(tables[0].name) == "DBFilesClient\\Spell.dbc" && tables[0].records == 2 &&
                  tables[0].bytes == 64 + 24,
              "a loaded database counts its records, and its strings as Storm handed them out; one unread is left out");
        Storm::SMemFree::Before(g_spellStrings, "free", 0, 0);

        const std::vector<wxl_footprint_region> regions = Listed(api.regions);
        uint64_t committed = 0, freeLargest = 0;
        for (const wxl_footprint_region& region : regions)
        {
            if (Text(region.state) == "committed") committed += region.bytes;
            if (Text(region.state) == "free") freeLargest = region.largest;
        }
        Check(regions.size() == 7 && committed > 0 && freeLargest > 0,
              "the address space is listed by state and type, with the largest free block");

        // What a mod reports of its own.
        Check(api.report(Str(""), 1, 1) == WXL_E_INVALID, "a report needs a name");
        api.report(Str("cache"), 777, 3);
        api.snapshot();
        const std::vector<wxl_footprint_category> reported = Listed(api.categories);
        const wxl_footprint_category*             cache    = CategoryNamed(reported, "cache");
        Check(cache && cache->bytes == 777 && cache->count == 3 && Text(cache->owner) == "runtime",
              "a report is a category, owned by whoever reported it");
        api.report(Str("cache"), 0, 0);
        api.snapshot();
        Check(!CategoryNamed(Listed(api.categories), "cache"), "and a report of nothing removes it");

        // MemoryPressure: armed by a threshold, raised once under it, again only after recovering.
        wxl_memory_pressure_args pressure{};
        Check(!wxl::core::services::footprint::PressureDue(&pressure), "with no threshold nothing is due");
        api.watch_pressure(~0ull);
        Check(wxl::core::services::footprint::PressureDue(&pressure) && pressure.largest_free > 0 &&
                  pressure.threshold == ~0ull && pressure.committed > 0,
              "a threshold above the largest free block is due at the first look, with what was seen");
        Check(!wxl::core::services::footprint::PressureDue(&pressure), "and not again within the second");
        wxl::core::services::footprint::ForgetMod(0);
        Check(!wxl::core::services::footprint::PressureDue(&pressure), "a mod's threshold goes with it");

        api.stop();
        Check(api.running() == WXL_FALSE, "stop ends accounting");
        Storm::SMemAlloc::After(a, 100, kClient, 10, 0);
        api.snapshot();
        sources = Listed(api.sources);
        news    = SourceNamed(sources, kNew);
        const wxl_footprint_source* stale = SourceNamed(sources, kClient);
        Check(news && news->bytes == 1000 && stale && stale->allocations == 3,
              "the last counts stay after stop, and nothing more is counted");
        wxl::core::services::footprint::Reset();
    }

    void CheckArchivesAndDatabases()
    {
        FakeSArchive patch{0, &g_archives[1]};
        void*        handle = &patch;

        g_received.clear();
        wxl::seam::Storage::OpenArchive::After(1, "Data\\patch-3.MPQ", 30, 0, &handle);
        const Received* mounted = Only(WXL_EV_ArchiveMounted);
        Check(mounted && mounted->As<wxl_archive_args>().priority == 30 &&
                  mounted->As<wxl_archive_args>().block_entries == 9000 && mounted->As<wxl_archive_args>().bytes == 500000,
              "Storage.OpenArchive raises ArchiveMounted with the archive's priority and tables");
        g_received.clear();
        wxl::seam::Storage::OpenArchive::After(0, "Data\\absent.MPQ", 30, 0, &handle);
        Check(g_received.empty(), "and nothing for an archive that did not open");
        wxl::seam::Storage::CloseArchive::Before(handle);
        Check(Only(WXL_EV_ArchiveUnmounted) != nullptr, "Storage.CloseArchive raises ArchiveUnmounted before it closes");

        g_received.clear();
        wxl::seam::ClientDb::Initialize::After();
        const Received* loaded = Only(WXL_EV_ClientDbLoaded);
        Check(loaded && loaded->As<wxl_client_db_args>().record_count == 2 &&
                  loaded->As<wxl_client_db_args>().records == g_spellRecords,
              "ClientDb.Initialize raises ClientDbLoaded for each database the client loaded, and only those");
    }

#if defined(_M_IX86)
    // --- The generated detour, on the architecture it is generated for --------------------------

    /// A client-shaped class: a non-static member, `this` in ECX, two stack arguments, a result.
    struct Thing
    {
        int32_t base = 100;
        int32_t __thiscall Add(int32_t a, int32_t b) { return base + a + b; }
    };

    int32_t g_shimBefore = 0;
    int32_t g_shimAfter  = 0;
    void*   g_shimSelf   = nullptr;

    void WXL_CALL ShimBefore(void* self, int32_t a, int32_t b)
    {
        g_shimSelf   = self;
        g_shimBefore = a * 10 + b;
    }

    void WXL_CALL ShimAfter(int32_t result, void* self, int32_t a, int32_t b)
    {
        g_shimAfter = self == g_shimSelf ? result + a + b : -1;
    }

    /// A function pointer read back through a volatile slot, so the optimiser cannot see which
    /// function it holds. The shim checks call across conventions on purpose (a __fastcall detour
    /// as the __thiscall it stands in for, a __thiscall original through the detour's __fastcall
    /// pointer); a call whose callee the optimiser can see with another convention than the
    /// call's own is undefined to it, and it drops the call and everything after it.
    template <class Fn>
    Fn Opaque(Fn fn)
    {
        Fn volatile slot = fn;
        return slot;
    }

    /// The x86 binding's calls into the client (binding/CallX86.hpp), against stand-ins with the
    /// conventions Wow.exe gives the functions it calls: __thiscall members popping their
    /// arguments, a bool answered in AL, a WOWGUID by value as two stack slots, cdecl free
    /// functions. Each stand-in is reached through an address read back through a volatile
    /// slot, as the client's are, so the optimiser cannot see the convention it is declared with.
    struct ClientStandIn
    {
        int32_t base = 70;

        __declspec(noinline) int32_t __thiscall Reaction(const void* other)
        {
            return other == this ? 4 : base / 10;
        }

        __declspec(noinline) bool __thiscall Knows(uint32_t spell) { return spell == 133; }

        __declspec(noinline) bool __thiscall Move(int32_t type, const uint64_t* guid, const wxl_vec3* point,
                                                  float facing)
        {
            return type == 4 && *guid == 0 && point->z == 3.0f && facing == 0.5f;
        }
    };

    uint64_t g_castGuid  = 0;
    uint32_t g_castSpell = 0;

    __declspec(noinline) bool __cdecl CastStandIn(uint32_t spell, const void* item, uint64_t target, int32_t confirmed)
    {
        g_castSpell = spell;
        g_castGuid  = target;
        return item == nullptr && confirmed == 0;
    }

    template <class M>
    std::uintptr_t AddressOf(M member)
    {
        return reinterpret_cast<std::uintptr_t>(Opaque(*reinterpret_cast<void**>(&member)));
    }

    void CheckCallX86()
    {
        using wxl::core::binding::x86::CallFree;
        using wxl::core::binding::x86::CallMember;

        ClientStandIn unit;
        volatile uint32_t canary = 0xC0FFEE;
        Check(CallMember<int32_t>(AddressOf(&ClientStandIn::Reaction), &unit, static_cast<const void*>(nullptr)) == 7 &&
                  CallMember<int32_t>(AddressOf(&ClientStandIn::Reaction), &unit, static_cast<const void*>(&unit)) == 4,
              "CallMember passes this in ECX and an argument the callee pops");
        Check(CallMember<bool>(AddressOf(&ClientStandIn::Knows), &unit, uint32_t(133)) &&
                  !CallMember<bool>(AddressOf(&ClientStandIn::Knows), &unit, uint32_t(134)),
              "a bool answer is read from AL");
        const uint64_t none = 0;
        const wxl_vec3 point{1.0f, 2.0f, 3.0f};
        Check(CallMember<bool>(AddressOf(&ClientStandIn::Move), &unit, int32_t(4), &none, &point, 0.5f),
              "four stack arguments, a float among them, reach a __thiscall callee");
        const auto cast = reinterpret_cast<std::uintptr_t>(Opaque(&CastStandIn));
        Check(CallFree<bool>(cast, uint32_t(133), static_cast<const void*>(nullptr), uint64_t(0xF130000000000789ull),
                             int32_t(0)) &&
                  g_castSpell == 133 && g_castGuid == 0xF130000000000789ull,
              "a cdecl call passes a 64-bit guid as two slots, low first");
        Check(canary == 0xC0FFEE, "and the stack is the caller's again after each");
    }

    using ShimSeam = wxl::core::seams::DetourOf<wxl::seam::detail::conv_thiscall,
                                                int32_t(void*, int32_t, int32_t)>;
    using ShimLink = ShimSeam::Link<&ShimBefore, &ShimAfter>;

    /// Calls the generated __fastcall detour the way the hook engine's caller would: as the
    /// __thiscall it stands in for, so a wrong shim corrupts the stack here rather than in a
    /// running client.
    void CheckThiscallShim()
    {
        Thing thing;
        auto  member        = &Thing::Add;
        ShimLink::original  = Opaque(reinterpret_cast<ShimSeam::Fn>(*reinterpret_cast<void**>(&member)));

        using AsThiscall = int32_t(__thiscall*)(Thing*, int32_t, int32_t);
        const AsThiscall detour = Opaque(reinterpret_cast<AsThiscall>(&ShimLink::Detour));
        const int32_t result = detour(&thing, 5, 7);

        Check(result == 112, "the thiscall shim returns the original's result (100 + 5 + 7)");
        Check(g_shimSelf == &thing && g_shimBefore == 57, "Before received `this` and the arguments");
        Check(g_shimAfter == 112 + 5 + 7, "After received the result first, then `this` and the arguments");
    }

    using ShimBeforeLink = ShimSeam::Link<&ShimBefore, nullptr>;
    using ShimAfterLink  = ShimSeam::Link<nullptr, &ShimAfter>;

    /// The detours of the one-sided shapes: the original runs, and only the declared side does.
    void CheckOneSidedShim()
    {
        Thing thing;
        auto  member = &Thing::Add;
        using AsThiscall = int32_t(__thiscall*)(Thing*, int32_t, int32_t);

        ShimBeforeLink::original = Opaque(reinterpret_cast<ShimSeam::Fn>(*reinterpret_cast<void**>(&member)));
        g_shimSelf = nullptr, g_shimBefore = 0, g_shimAfter = 0;
        int32_t result = Opaque(reinterpret_cast<AsThiscall>(&ShimBeforeLink::Detour))(&thing, 5, 7);
        Check(result == 112 && g_shimBefore == 57 && g_shimAfter == 0,
              "a Before-only detour runs Before and the original, and no After");

        ShimAfterLink::original = Opaque(reinterpret_cast<ShimSeam::Fn>(*reinterpret_cast<void**>(&member)));
        g_shimSelf = &thing, g_shimBefore = 0, g_shimAfter = 0;
        result = Opaque(reinterpret_cast<AsThiscall>(&ShimAfterLink::Detour))(&thing, 5, 7);
        Check(result == 112 && g_shimBefore == 0 && g_shimAfter == 112 + 5 + 7,
              "an After-only detour runs the original and After, and no Before");
    }

    int32_t g_gateCalls = 0;
    void*   g_gateSelf  = nullptr;
    bool    g_gateTakes = false;

    bool WXL_CALL ShimGate(int32_t* result, void** self, int32_t* a, int32_t* b)
    {
        ++g_gateCalls;
        g_gateSelf = *self;
        *a         = *a + 1; // what the original is called with, if it is called at all
        if (!g_gateTakes) return false;
        *result = *a * 1000 + *b;
        return true;
    }

    using ShimGateSeam = wxl::core::seams::GateOf<wxl::seam::detail::conv_thiscall,
                                                  int32_t(void*, int32_t, int32_t)>;
    using ShimGateLink = ShimGateSeam::Link<&ShimGate>;

    /// Drives the generated gate as the hook engine's caller would, both ways: once letting the
    /// original run with a rewritten argument, once taking the call away from it.
    void CheckThiscallGate()
    {
        Thing thing;
        auto  member = &Thing::Add;
        ShimGateLink::original =
            Opaque(reinterpret_cast<ShimGateSeam::Fn>(*reinterpret_cast<void**>(&member)));

        using AsThiscall = int32_t(__thiscall*)(Thing*, int32_t, int32_t);
        const AsThiscall detour = Opaque(reinterpret_cast<AsThiscall>(&ShimGateLink::Detour));

        g_gateTakes    = false;
        int32_t result = detour(&thing, 5, 7);
        Check(result == 100 + 6 + 7,
              "a gate that lets the call through rewrites what the original is given");
        Check(g_gateSelf == &thing && g_gateCalls == 1, "and it was handed the object once");

        g_gateTakes = true;
        result      = detour(&thing, 5, 7);
        Check(result == 6 * 1000 + 7, "a gate that takes the call answers in the original's place");
        Check(g_gateCalls == 2, "and the original never ran");
    }

    // --- ObjectMgr.GetUpdateObject's generated detour, against a stand-in of the client's -------

    FakeObject* g_lazyObject  = nullptr; ///< what the stand-in finds in its lazy-cleanup set
    uint32_t    g_seenGuidLo  = 0, g_seenGuidHi = 0;
    uint32_t    g_espBefore   = 0, g_espAfter = 0;
    void*       g_getAnswered = nullptr;

    /// A cdecl function of GetUpdateObject's shape: the guid as two stack dwords, the out-pointer.
    __declspec(noinline) void* __cdecl FakeGetUpdateObject(uint64_t guid, int32_t* reenabled)
    {
        g_seenGuidLo = static_cast<uint32_t>(guid);
        g_seenGuidHi = static_cast<uint32_t>(guid >> 32);
        *reenabled   = 0;
        if (!g_lazyObject || g_lazyObject->guid != guid) return nullptr;
        *reenabled = 1;
        return g_lazyObject;
    }

    /// Calls a detour the way ClntObjMgrReenableObject does at 0x004D6F1D: the pointer and the two
    /// halves of the guid pushed, the call, `add esp,0xc`; and records ESP on both sides.
    __declspec(noinline) void CallLikeReenable(void* detour, uint32_t lo, uint32_t hi, int32_t* reenabled)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            mov  eax, reenabled
            push eax
            push hi
            push lo
            call detour
            add  esp, 0Ch
            mov  dword ptr [g_getAnswered], eax
            mov  dword ptr [g_espAfter], esp
        }
    }

    using GetUpdateSeam = wxl::core::seams::DetourOf<wxl::seam::ObjectMgr::GetUpdateObject::Convention,
                                                     wxl::seam::ObjectMgr::GetUpdateObject::Signature>;
    using GetUpdateLink = GetUpdateSeam::Link<nullptr, &wxl::seam::ObjectMgr::GetUpdateObject::After>;

    /// The detour AttachX86.cpp generates for the row, called as the client calls the function.
    void CheckGetUpdateObjectDetour()
    {
        FakeObject bear;
        bear.guid   = 0xF1300000ABCD0042ull;
        bear.typeId = WXL_OBJECT_UNIT;
        g_lazyObject = &bear;

        GetUpdateLink::original = Opaque(&FakeGetUpdateObject);
        void* const detour      = Opaque(reinterpret_cast<void*>(&GetUpdateLink::Detour));

        int32_t reenabled = -1;
        g_received.clear();
        CallLikeReenable(detour, static_cast<uint32_t>(bear.guid), static_cast<uint32_t>(bear.guid >> 32),
                         &reenabled);
        Check(g_espBefore == g_espAfter, "the GetUpdateObject detour leaves the stack as cdecl's caller expects");
        Check(g_seenGuidLo == 0xABCD0042u && g_seenGuidHi == 0xF1300000u,
              "the original receives both halves of the guid in order");
        Check(g_getAnswered == &bear && reenabled == 1,
              "the detour answers the original's object and leaves its out-parameter");
        const Received* created = Only(WXL_EV_ObjectCreate);
        Check(created != nullptr && created->As<wxl_object_args>().guid == bear.guid,
              "and After announced the object it brought back");

        g_received.clear();
        CallLikeReenable(detour, 0x1u, 0x2u, &reenabled);
        Check(g_getAnswered == nullptr && reenabled == 0 && g_received.empty() && g_espBefore == g_espAfter,
              "an update of an object the client does not hold answers null and raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&bear);
        g_lazyObject = nullptr;
    }

    // --- The mirror handler and the shared seams, as the client calls them ----------------------

    /// Calls a mirror handler the way CallMirrorFunctions does at 0x004D5254: the param, the old
    /// value's address, the size, the offset and the guid's two halves pushed, `call ecx`,
    /// `add esp, 0x18`; and records ESP on both sides.
    __declspec(noinline) void CallLikeMirror(void* handler, uint32_t lo, uint32_t hi, uint32_t at,
                                             uint32_t bytes, const void* old_value, void* param)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            push param
            push old_value
            push bytes
            push at
            push hi
            push lo
            mov  ecx, handler
            call ecx
            add  esp, 18h
            mov  dword ptr [g_espAfter], esp
        }
    }

    /// Calls a detour of a function that takes nothing, as ClientInitializeGame calls
    /// ClntObjMgrInitializeShared at 0x004055D4: nothing pushed, nothing cleaned.
    __declspec(noinline) void CallWithNothing(void* detour)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            call detour
            mov  dword ptr [g_espAfter], esp
        }
    }

    uint32_t g_sharedCalls = 0;
    __declspec(noinline) void __cdecl FakeInitializeShared() { ++g_sharedCalls; }

    using SharedSeam = wxl::core::seams::DetourOf<wxl::seam::ObjectMgr::InitializeShared::Convention,
                                                  wxl::seam::ObjectMgr::InitializeShared::Signature>;
    using SharedLink = SharedSeam::Link<nullptr, &wxl::seam::ObjectMgr::InitializeShared::After>;

    void CheckMirrorDetours()
    {
        const wxl_object_api* api = wxl::core::objects::Api();
        FakeObject            boar;
        boar.guid   = 0xF130000000000777ull;
        boar.typeId = WXL_OBJECT_UNIT;
        boar.block.assign(0x94, 0u);
        boar.block[0x18] = 40;
        g_held = {&boar};
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 9100u);

        // The detour the row generates, called as the client calls the function: After runs and
        // sets the watched fields' handlers.
        wxl::seam::ObjectMgr::DestroyShared::Before();
        g_mirrors.clear();
        SharedLink::original = Opaque(&FakeInitializeShared);
        CallWithNothing(Opaque(reinterpret_cast<void*>(&SharedLink::Detour)));
        Check(g_espBefore == g_espAfter && g_sharedCalls == 1,
              "the InitializeShared detour runs the original and leaves the stack as it found it");
        Check(!g_mirrors.empty() && g_mirrors[0].type == WXL_OBJECT_UNIT && g_mirrors[0].offset == (0x18 - 6) * 4,
              "and its After sets the watched fields' handlers");

        // The runtime's handler, called through its pointer with the client's convention.
        uint32_t was = 45;
        g_received.clear();
        CallLikeMirror(Opaque(reinterpret_cast<void*>(g_mirrors[0].handler)), static_cast<uint32_t>(boar.guid),
                       static_cast<uint32_t>(boar.guid >> 32), g_mirrors[0].offset, g_mirrors[0].size, &was,
                       g_mirrors[0].param);
        Check(g_espBefore == g_espAfter, "the mirror handler leaves the stack as CallMirrorFunctions expects");
        const Received* changed = Only(WXL_EV_ObjectFieldChanged);
        Check(changed && changed->As<wxl_object_field_args>().object == api->from_guid(boar.guid) &&
                  changed->As<wxl_object_field_args>().old_value == 45 &&
                  changed->As<wxl_object_field_args>().new_value == 40,
              "and reads the guid's halves, the offset, the size, the old value and its param in order");

        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
        g_held.clear();
    }

    // --- The register thunks, against stand-ins with the client's conventions -------------------

    // What a stand-in original found in its registers and on its stack.
    uint32_t g_seenEdi = 0, g_seenEsi = 0, g_seenEax = 0, g_seenEcx = 0;
    uint32_t g_seenArg0 = 0, g_seenArg1 = 0;

    // What the caller's callee-saved registers held once the thunk returned.
    uint32_t g_keptEbx = 0, g_keptEsi = 0, g_keptEdi = 0;

    __declspec(naked) void FakeDispatchUpdate()
    {
        __asm {
            mov  dword ptr [g_seenEdi], edi
            mov  eax, dword ptr [esp+4]
            mov  dword ptr [g_seenArg0], eax
            mov  eax, dword ptr [esp+8]
            mov  dword ptr [g_seenArg1], eax
            ret
        }
    }

    __declspec(naked) void FakeInitObject()
    {
        __asm {
            mov  dword ptr [g_seenEsi], esi
            mov  dword ptr [g_seenEax], eax
            mov  dword ptr [g_seenEcx], ecx
            ret
        }
    }

    __declspec(naked) void FakeObjDelete()
    {
        __asm {
            mov  dword ptr [g_seenEax], eax
            ret
        }
    }

    __declspec(naked) void FakeDestroyRemovedObject()
    {
        __asm {
            mov  dword ptr [g_seenEsi], esi
            ret
        }
    }

    // What a stand-in combat log original answers, so the thunk can be seen handing it back.
    constexpr uint32_t kLogAnswer = 0x600D600Du;
    uint32_t           g_answered = 0;

    __declspec(naked) void FakeLogFromEdi()
    {
        __asm {
            mov  dword ptr [g_seenEdi], edi
            mov  eax, dword ptr [esp+4]
            mov  dword ptr [g_seenArg0], eax
            mov  eax, 600D600Dh         // kLogAnswer
            ret
        }
    }

    __declspec(naked) void FakeLogFromEax()
    {
        __asm {
            mov  dword ptr [g_seenEax], eax
            mov  eax, dword ptr [esp+4]
            mov  dword ptr [g_seenArg0], eax
            mov  eax, 600D600Dh         // kLogAnswer
            ret
        }
    }

    // The callers below load EBX, ESI and EDI by hand, so each stays a function of its own: inlined
    // into a caller whose frame is addressed through ESI (a realigned stack), the block would
    // overwrite that base and every local after it.

    /// Calls a thunk the way the combat log statics' callers do: the message in EDI, or in EAX
    /// when `inEax`, and the time offset in one stack slot the caller cleans.
    __declspec(noinline) void CallLog(void* thunk, bool inEax, void* msg, uint32_t timeOffset)
    {
        __asm {
            mov  ebx, 0B0B0B0Bh
            mov  esi, 05050505h
            mov  edi, 0D0D0D0Dh
            mov  eax, 0A0A0A0Ah
            cmp  byte ptr [inEax], 0
            mov  ecx, msg
            jne  in_eax
            mov  edi, ecx
            jmp  call_it
        in_eax:
            mov  eax, ecx
        call_it:
            push timeOffset
            call thunk
            add  esp, 4
            mov  dword ptr [g_answered], eax
            mov  dword ptr [g_keptEbx], ebx
            mov  dword ptr [g_keptEsi], esi
            mov  dword ptr [g_keptEdi], edi
        }
    }

    /// Calls a thunk the way IEvtDispatchUpdate's caller does: context in EDI, two stack slots
    /// the caller cleans.
    __declspec(noinline) void CallWithEdi(void* thunk, void* context, uint32_t arg0, uint32_t arg1)
    {
        __asm {
            mov  ebx, 0B0B0B0Bh
            mov  esi, 05050505h
            mov  edi, context
            push arg1
            push arg0
            call thunk
            add  esp, 8
            mov  dword ptr [g_keptEbx], ebx
            mov  dword ptr [g_keptEsi], esi
            mov  dword ptr [g_keptEdi], edi
        }
    }

    /// Calls a thunk the way InitObject's caller does: ESI, EAX and ECX, nothing pushed.
    __declspec(noinline) void CallWithEsiEaxEcx(void* thunk, void* esiValue, uint32_t eaxValue, uint32_t ecxValue)
    {
        __asm {
            mov  ebx, 0B0B0B0Bh
            mov  edi, 0D0D0D0Dh
            mov  esi, esiValue
            mov  eax, eaxValue
            mov  ecx, ecxValue
            call thunk
            mov  dword ptr [g_keptEbx], ebx
            mov  dword ptr [g_keptEsi], esi
            mov  dword ptr [g_keptEdi], edi
        }
    }

    /// Calls a thunk the way ObjDelete's callers do: the object in EAX, nothing pushed.
    __declspec(noinline) void CallWithEax(void* thunk, void* eaxValue)
    {
        __asm {
            mov  ebx, 0B0B0B0Bh
            mov  esi, 05050505h
            mov  edi, 0D0D0D0Dh
            mov  eax, eaxValue
            call thunk
            mov  dword ptr [g_keptEbx], ebx
            mov  dword ptr [g_keptEsi], esi
            mov  dword ptr [g_keptEdi], edi
        }
    }

    /// Every regparm row has a thunk, and each thunk hands the original its registers back.
    void CheckRegparmThunks()
    {
        namespace regparm = wxl::core::seams::regparm;

        for (const wxl::core::seams::Seam& seam : wxl::core::seams::Table())
        {
            if (strcmp(seam.convention, "regparm") != 0) continue;
            char what[160];
            snprintf(what, sizeof what, "%s takes its arguments in registers and has a thunk", seam.name);
            Check(regparm::Find(seam.name) != nullptr, what);
        }
        Check(regparm::Find("World.Preload") == nullptr, "a seam with a stack convention has none");

        const regparm::Row* update  = regparm::Find("Event.DispatchUpdate");
        const regparm::Row* init    = regparm::Find("ObjectMgr.InitObject");
        const regparm::Row* remove  = regparm::Find("ObjectMgr.ObjDelete");
        const regparm::Row* destroy = regparm::Find("ObjectMgr.DestroyRemovedObject");
        if (!update || !init || !remove || !destroy) return;

        *update->original  = reinterpret_cast<void*>(&FakeDispatchUpdate);
        *init->original    = reinterpret_cast<void*>(&FakeInitObject);
        *remove->original  = reinterpret_cast<void*>(&FakeObjDelete);
        *destroy->original = reinterpret_cast<void*>(&FakeDestroyRemovedObject);

        int         context = 0;
        const float elapsed = 0.25f;
        uint32_t    elapsedBits;
        memcpy(&elapsedBits, &elapsed, sizeof elapsedBits);

        g_received.clear();
        CallWithEdi(update->detour, &context, 4242u, elapsedBits);
        Check(g_seenEdi == reinterpret_cast<uint32_t>(&context),
              "Event.DispatchUpdate's original finds the context in EDI");
        Check(g_seenArg0 == 4242u && g_seenArg1 == elapsedBits, "and its two stack arguments");
        const Received* tick = Only(WXL_EV_Update);
        Check(tick && tick->As<wxl_update_args>().delta == elapsed,
              "Before read the arguments the thunk was handed");
        Check(g_keptEbx == 0x0B0B0B0Bu && g_keptEsi == 0x05050505u &&
                  g_keptEdi == reinterpret_cast<uint32_t>(&context),
              "and the caller gets EBX, ESI and EDI back");

        FakeObject spider;
        spider.guid   = 0xF130000000000777ull;
        spider.typeId = WXL_OBJECT_UNIT;

        g_received.clear();
        CallWithEsiEaxEcx(init->detour, &spider, 0x1234u, 77u);
        Check(g_seenEsi == reinterpret_cast<uint32_t>(&spider) && g_seenEax == 0x1234u &&
                  g_seenEcx == 77u,
              "ObjectMgr.InitObject's original finds the object in ESI, the block in EAX, the time in ECX");
        const Received* created = Only(WXL_EV_ObjectCreate);
        Check(created && created->As<wxl_object_args>().guid == spider.guid,
              "After was handed the object from ESI");
        Check(g_keptEbx == 0x0B0B0B0Bu && g_keptEdi == 0x0D0D0D0Du &&
                  g_keptEsi == reinterpret_cast<uint32_t>(&spider),
              "and the caller gets EBX, ESI and EDI back");

        g_received.clear();
        CallWithEax(remove->detour, &spider);
        Check(g_seenEax == reinterpret_cast<uint32_t>(&spider),
              "ObjectMgr.ObjDelete's original finds the object in EAX");
        Check(Only(WXL_EV_ObjectDestroy) != nullptr, "Before was handed the object from EAX");
        Check(g_keptEbx == 0x0B0B0B0Bu && g_keptEsi == 0x05050505u && g_keptEdi == 0x0D0D0D0Du,
              "and the caller gets EBX, ESI and EDI back");

        FakeObject moth;
        CallWithEsiEaxEcx(destroy->detour, &moth, 0u, 0u);
        Check(g_seenEsi == reinterpret_cast<uint32_t>(&moth),
              "ObjectMgr.DestroyRemovedObject's original finds the object in ESI");

        // The combat log statics: a miss with one target, a damage line, a heal line and an energize
        // line.
        const regparm::Row* miss   = regparm::Find("Combat.HandleSpellLogMiss");
        const regparm::Row* damage = regparm::Find("Combat.HandleSpellNonMeleeDamageLog");
        const regparm::Row* heal   = regparm::Find("Combat.HandleSpellHealLog");
        const regparm::Row* energy = regparm::Find("Combat.HandleSpellLogEnergizeLog");
        if (miss && damage && heal && energy)
        {
            *miss->original   = reinterpret_cast<void*>(&FakeLogFromEdi);
            *damage->original = reinterpret_cast<void*>(&FakeLogFromEax);
            *heal->original   = reinterpret_cast<void*>(&FakeLogFromEax);
            *energy->original = reinterpret_cast<void*>(&FakeLogFromEax);

            Body missLog;
            missLog.U32(116).U64(0xF130000000000001ull).U8(0).U32(1).U64(0xF130000000000002ull).U8(3);
            FakeStore store = missLog.Store();
            g_received.clear();
            CallLog(miss->detour, false, &store, 7u);
            Check(g_seenEdi == reinterpret_cast<uint32_t>(&store) && g_seenArg0 == 7u,
                  "Combat.HandleSpellLogMiss's original finds the message in EDI, the offset on the stack");
            Check(Only(WXL_EV_CombatMiss) != nullptr, "Before was handed the message from EDI");
            Check(g_answered == kLogAnswer, "and the caller gets the original's answer");
            Check(g_keptEbx == 0x0B0B0B0Bu && g_keptEsi == 0x05050505u &&
                      g_keptEdi == reinterpret_cast<uint32_t>(&store),
                  "and EBX, ESI and EDI back");

            Body spell;
            spell.Packed(0xF130000000000002ull).Packed(0xF130000000000001ull).U32(116).U32(900).U32(0)
                .U8(4).U32(0).U32(0).U8(0).U8(0).U32(0).U32(0);
            store = spell.Store();
            g_received.clear();
            CallLog(damage->detour, true, &store, 9u);
            Check(g_seenEax == reinterpret_cast<uint32_t>(&store) && g_seenArg0 == 9u,
                  "Combat.HandleSpellNonMeleeDamageLog's original finds the message in EAX, the offset on the stack");
            Check(Only(WXL_EV_CombatDamage) != nullptr, "Before was handed the message from EAX");
            Check(g_answered == kLogAnswer && g_keptEbx == 0x0B0B0B0Bu && g_keptEsi == 0x05050505u &&
                      g_keptEdi == 0x0D0D0D0Du,
                  "and the caller gets the answer, EBX, ESI and EDI back");

            Body healLog;
            healLog.Packed(0xF130000000000001ull).Packed(0xF130000000000002ull).U32(2061).U32(1200)
                .U32(0).U32(0).U8(0);
            store = healLog.Store();
            g_received.clear();
            CallLog(heal->detour, true, &store, 11u);
            Check(g_seenEax == reinterpret_cast<uint32_t>(&store) && g_seenArg0 == 11u,
                  "Combat.HandleSpellHealLog's original finds the message in EAX, the offset on the stack");
            Check(Only(WXL_EV_CombatHeal) != nullptr, "Before was handed the message from EAX");
            Check(g_answered == kLogAnswer && g_keptEbx == 0x0B0B0B0Bu && g_keptEsi == 0x05050505u &&
                      g_keptEdi == 0x0D0D0D0Du,
                  "and the caller gets the answer, EBX, ESI and EDI back");

            Body energizeLog;
            energizeLog.Packed(0xF130000000000001ull).Packed(0xF130000000000002ull).U32(29166).U32(0)
                .U32(300);
            store = energizeLog.Store();
            g_received.clear();
            CallLog(energy->detour, true, &store, 13u);
            Check(g_seenEax == reinterpret_cast<uint32_t>(&store) && g_seenArg0 == 13u,
                  "Combat.HandleSpellLogEnergizeLog's original finds the message in EAX, the offset on the stack");
            Check(Only(WXL_EV_CombatEnergize) != nullptr, "Before was handed the message from EAX");
            Check(g_answered == kLogAnswer && g_keptEbx == 0x0B0B0B0Bu && g_keptEsi == 0x05050505u &&
                      g_keptEdi == 0x0D0D0D0Du,
                  "and the caller gets the answer, EBX, ESI and EDI back");

            *miss->original   = nullptr;
            *damage->original = nullptr;
            *heal->original   = nullptr;
            *energy->original = nullptr;
        }

        *update->original  = nullptr;
        *init->original    = nullptr;
        *remove->original  = nullptr;
        *destroy->original = nullptr;
        wxl::core::stages::Restart();
    }

    // --- The chat's commit points, through the detours the attachment generates --------------

    /// What a stand-in for a client function was handed, to show a detour passed it on intact.
    struct OriginalSaw
    {
        const void* self   = nullptr;
        const char* text   = nullptr;
        uint64_t    guid   = 0;
        uint32_t    lineId = 0;
        int32_t     calls  = 0;
    };

    OriginalSaw g_originalSaw;

    /// PENDINGCHAT's destructor: a member with nothing on the stack.
    struct FakeLineNode
    {
        void __thiscall Destroy() { g_originalSaw.self = this, ++g_originalSaw.calls; }
    };

    void __cdecl FakeRecordChat(const char* text, int32_t, const char*, uint64_t sender_guid, uint32_t, uint32_t,
                                const char*, uint32_t line_id, const void*)
    {
        g_originalSaw.text = text, g_originalSaw.guid = sender_guid, g_originalSaw.lineId = line_id;
        ++g_originalSaw.calls;
    }

    void __cdecl FakeQueueChatText(const char* text, int32_t, const char*, const char*, uint32_t, const char*,
                                   const char*, int32_t, int32_t, uint64_t sender_guid, uint32_t, uint64_t, int32_t,
                                   uint8_t, const void* conversation)
    {
        g_originalSaw.text = text, g_originalSaw.guid = sender_guid, g_originalSaw.self = conversation;
        ++g_originalSaw.calls;
    }

    /// The x86 detours of the three new chat rows, called the way AddChatMessage and the list's
    /// DeleteNode call the functions: the guids split over two stack dwords, `this` in ECX.
    void CheckChatDetours()
    {
        namespace Chat = wxl::seam::Chat;
        using wxl::core::seams::DetourOf;
        using Record = DetourOf<Chat::RecordChat::Convention, Chat::RecordChat::Signature>::Link<
            &Chat::RecordChat::Before, nullptr>;
        using Queue = DetourOf<Chat::QueueChatText::Convention, Chat::QueueChatText::Signature>::Link<
            nullptr, &Chat::QueueChatText::After>;
        using DestroySeam = DetourOf<Chat::DestroyPendingChat::Convention, Chat::DestroyPendingChat::Signature>;
        using Destroy     = DestroySeam::Link<&Chat::DestroyPendingChat::Before, nullptr>;

        FakeObject orc;
        orc.guid   = 0x0000000000000ABEull;
        orc.typeId = WXL_OBJECT_PLAYER;
        wxl::seam::ObjectMgr::InitObject::After(&orc, nullptr, 9200u);

        Body say;
        say.U8(1).U32(7).U64(orc.guid).U32(0).U64(0).Sized("Zug zug").U8(0);
        FakeStore store = say.Store();
        g_received.clear();
        g_originalSaw = OriginalSaw{};
        Record::original = Opaque(&FakeRecordChat);
        Chat::ChatMessageReceived::Before(&store, 0);
        AddReceived(store, "Zug zug", 1, orc.guid, 0);
        Opaque(reinterpret_cast<void(__cdecl*)(const char*, int32_t, const char*, uint64_t, uint32_t, uint32_t,
                                               const char*, uint32_t, const void*)>(&Record::Detour))(
            "Zug zug", 1, "Thrall", orc.guid, 0, 0, nullptr, 31u, nullptr);
        Chat::ChatMessageReceived::After(1, &store, 0);
        Check(g_originalSaw.calls == 1 && g_originalSaw.guid == orc.guid && g_originalSaw.lineId == 31u,
              "Chat.RecordChat's detour hands the original every argument, the guid's two halves in place");
        Check(Only(WXL_EV_ChatMessage) && Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().line_id == 31u &&
                  Only(WXL_EV_ChatMessage)->As<wxl_chat_args>().sender_guid == orc.guid,
              "and its Before raises ChatMessage with them");

        FakePendingChat line;
        char            copy[] = "Zug zug";
        line.text     = copy;
        g_pendingLast = &line;
        g_originalSaw = OriginalSaw{};
        Queue::original = Opaque(&FakeQueueChatText);
        store = say.Store();
        g_received.clear();
        Chat::ChatMessageReceived::Before(&store, 0);
        AddReceived(store, "Zug zug", 1, orc.guid, 0);
        Opaque(reinterpret_cast<void(__cdecl*)(const char*, int32_t, const char*, const char*, uint32_t, const char*,
                                               const char*, int32_t, int32_t, uint64_t, uint32_t, uint64_t, int32_t,
                                               uint8_t, const void*)>(&Queue::Detour))(
            copy, 1, nullptr, nullptr, 7, nullptr, "", 0, 0, orc.guid, 0, 0, 0, 0, &line);
        Chat::ChatMessageReceived::After(1, &store, 0);
        Check(g_originalSaw.calls == 1 && g_originalSaw.text == copy && g_originalSaw.guid == orc.guid &&
                  g_originalSaw.self == &line,
              "Chat.QueueChatText's detour hands the original all seventeen dwords");

        auto member         = &FakeLineNode::Destroy;
        g_originalSaw       = OriginalSaw{};
        Destroy::original   = Opaque(reinterpret_cast<DestroySeam::Fn>(*reinterpret_cast<void**>(&member)));
        Opaque(reinterpret_cast<void(__thiscall*)(const FakePendingChat*)>(&Destroy::Detour))(&line);
        Check(g_originalSaw.calls == 1 && g_originalSaw.self == &line,
              "Chat.DestroyPendingChat's detour runs the destructor with `this` in ECX");
        g_received.clear();
        AddHeld(line, 1, orc.guid);
        Commit(copy, 1, orc.guid, 32u);
        Check(g_received.empty(), "and its Before forgot the held line, which the client then frees");

        wxl::seam::ObjectMgr::ObjDelete::Before(&orc);
        g_pendingLast     = nullptr;
        Record::original  = nullptr;
        Queue::original   = nullptr;
        Destroy::original = nullptr;
    }

    // --- Storm's allocator, through the detours the client's calls reach -----------------------

    alignas(16) char g_fakeHeap[1 << 16];
    uint32_t         g_fakeNext  = 0;
    void*            g_stormAnswered  = nullptr;
    uint32_t         g_freed     = 0;

    __declspec(noinline) void* __stdcall FakeSMemAlloc(size_t bytes, const char*, int32_t, uint32_t)
    {
        if (g_fakeNext + bytes + 8 > sizeof g_fakeHeap) g_fakeNext = 0;
        void* block = g_fakeHeap + g_fakeNext;
        g_fakeNext += static_cast<uint32_t>((bytes + 7) & ~static_cast<size_t>(7)) + 8;
        return block;
    }

    __declspec(noinline) int32_t __stdcall FakeSMemFree(void*, const char*, int32_t, uint32_t)
    {
        ++g_freed;
        return 1;
    }

    __declspec(noinline) void* __stdcall FakeSMemReAlloc(void*, size_t bytes, const char* file, int32_t line,
                                                         uint32_t flags)
    {
        return FakeSMemAlloc(bytes, file, line, flags);
    }

    namespace StormSeam = wxl::seam::Storm;
    using AllocLink = wxl::core::seams::DetourOf<StormSeam::SMemAlloc::Convention,
                                                 StormSeam::SMemAlloc::Signature>::Link<nullptr, &StormSeam::SMemAlloc::After>;
    using FreeLink  = wxl::core::seams::DetourOf<StormSeam::SMemFree::Convention,
                                                 StormSeam::SMemFree::Signature>::Link<&StormSeam::SMemFree::Before, nullptr>;
    using ReAllocLink = wxl::core::seams::DetourOf<StormSeam::SMemReAlloc::Convention, StormSeam::SMemReAlloc::Signature>::
        Link<&StormSeam::SMemReAlloc::Before, &StormSeam::SMemReAlloc::After>;

    /// Calls a detour as operator new calls SMemAlloc at 0x00401020: flags, line, file and size
    /// pushed, the call, no cleanup -- the callee pops them.
    __declspec(noinline) void CallLikeNew(void* detour, size_t bytes, const char* file)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            push 0
            push -1
            push file
            push bytes
            call detour
            mov  dword ptr [g_stormAnswered], eax
            mov  dword ptr [g_espAfter], esp
        }
    }

    /// Calls a detour as SMemFree is called: four pushed, the callee pops them.
    __declspec(noinline) void CallLikeFree(void* detour, void* block)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            push 0
            push 0
            push 0
            push block
            call detour
            mov  dword ptr [g_espAfter], esp
        }
    }

    /// Calls a detour as SMemReAlloc is called: five pushed, the callee pops them.
    __declspec(noinline) void CallLikeReAlloc(void* detour, void* block, size_t bytes, const char* file)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            push 0
            push 0
            push file
            push bytes
            push block
            call detour
            mov  dword ptr [g_stormAnswered], eax
            mov  dword ptr [g_espAfter], esp
        }
    }

    void CheckStormDetours()
    {
        const wxl_footprint_api& api = wxl::core::services::FootprintApi();
        wxl::core::services::footprint::Reset();
        api.start();
        static const char kFile[] = ".\\Detour.cpp";

        AllocLink::original   = Opaque(&FakeSMemAlloc);
        FreeLink::original    = Opaque(&FakeSMemFree);
        ReAllocLink::original = Opaque(&FakeSMemReAlloc);
        void* const alloc     = Opaque(reinterpret_cast<void*>(&AllocLink::Detour));
        void* const release   = Opaque(reinterpret_cast<void*>(&FreeLink::Detour));
        void* const resize    = Opaque(reinterpret_cast<void*>(&ReAllocLink::Detour));

        CallLikeNew(alloc, 100, kFile);
        void* const block = g_stormAnswered;
        Check(g_espBefore == g_espAfter && block != nullptr,
              "the SMemAlloc detour pops its four as stdcall does and answers the original's block");
        CallLikeReAlloc(resize, block, 200, kFile);
        void* const grown = g_stormAnswered;
        Check(g_espBefore == g_espAfter && grown != nullptr && grown != block,
              "the SMemReAlloc detour pops its five and answers the original's block");
        CallLikeFree(release, grown);
        Check(g_espBefore == g_espAfter && g_freed == 1, "the SMemFree detour pops its four and frees");

        api.snapshot();
        std::vector<wxl_footprint_source> rows(api.sources(nullptr, 0));
        for (wxl_footprint_source& row : rows) row.struct_size = sizeof row;
        api.sources(rows.data(), static_cast<uint32_t>(rows.size()));
        const wxl_footprint_source* seen = rows.empty() ? nullptr : &rows.front();
        Check(seen && seen->allocations == 2 && seen->bytes == 0 && seen->peak == 200,
              "and what went through them was counted: two allocations, nothing left");

        AllocLink::original   = nullptr;
        FreeLink::original    = nullptr;
        ReAllocLink::original = nullptr;
        wxl::core::services::footprint::Reset();
    }

    // --- OsValidateFunctionPointer, as luaD_precall calls it -------------------------------------

    uint32_t  g_validatorFatal = 0;
    uintptr_t g_ediAfter       = 0;

    /// The client's check with its fatal path stubbed: counts the pointers that reach it.
    __declspec(noinline) void __cdecl FakeValidate(const void*) { ++g_validatorFatal; }

    /// Calls a detour as luaD_precall calls OsValidateFunctionPointer at 0x00856513: the C
    /// function in EDI, pushed, the call, the push cleaned later; EDI must survive the call.
    __declspec(noinline) void CallLikePrecall(void* detour, const void* function)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            mov  edi, function
            push edi
            call detour
            mov  dword ptr [g_ediAfter], edi
            add  esp, 4
            mov  dword ptr [g_espAfter], esp
        }
    }

    using ValidateLink = wxl::core::seams::GateOf<wxl::seam::Os::ValidateFunctionPointer::Convention,
                                                  wxl::seam::Os::ValidateFunctionPointer::Signature>::
        Link<&wxl::seam::Os::ValidateFunctionPointer::Gate>;

    void CheckValidatorDetour()
    {
        ValidateLink::original = Opaque(&FakeValidate);
        void* const detour     = Opaque(reinterpret_cast<void*>(&ValidateLink::Detour));
        const void* own        = reinterpret_cast<const void*>(&CodeProbe);
        std::unique_ptr<char[]> heap(new char[16]);

        g_validatorFatal = 0;
        CallLikePrecall(detour, own);
        Check(g_espBefore == g_espAfter && g_ediAfter == reinterpret_cast<uintptr_t>(own) && g_validatorFatal == 0,
              "the OsValidateFunctionPointer detour takes a runtime function: the client's check is not reached");
        CallLikePrecall(detour, heap.get());
        Check(g_espBefore == g_espAfter && g_ediAfter == reinterpret_cast<uintptr_t>(heap.get()) && g_validatorFatal == 1,
              "and hands any other pointer to the client's check, its fatal path, with the stack and EDI as they were");
        ValidateLink::original = nullptr;
    }

    // --- FrameXML_CheckSignature and DeleteInterfaceFiles, through their detours ---------------

    /// The client's check behind the detour: what runs once the gate lets the call through.
    __declspec(noinline) int32_t __cdecl FakeCheckOriginal(const char*, const char*, const uint8_t*, uint8_t* digest)
    {
        return FakeJudge(digest);
    }

    uint32_t g_cleared = 0;
    __declspec(noinline) void __cdecl FakeDeleteOriginal() { ++g_cleared; }

    using SignatureLink = wxl::core::seams::GateOf<wxl::seam::FrameXML::CheckSignature::Convention,
                                                   wxl::seam::FrameXML::CheckSignature::Signature>::
        Link<&wxl::seam::FrameXML::CheckSignature::Gate>;
    using CleanupLink = wxl::core::seams::GateOf<wxl::seam::AddOns::DeleteInterfaceFiles::Convention,
                                                 wxl::seam::AddOns::DeleteInterfaceFiles::Signature>::
        Link<&wxl::seam::AddOns::DeleteInterfaceFiles::Gate>;
    using SignatureFn = int32_t(__cdecl*)(const char*, const char*, const uint8_t*, uint8_t*);

    void CheckInterfaceSignatureDetour()
    {
        // As on the stock client: the binding's call to the client's address enters the detour
        // again, whose gate lets it through to the original.
        SignatureLink::original = Opaque(&FakeCheckOriginal);
        static SignatureFn detour;
        detour = Opaque(reinterpret_cast<SignatureFn>(&SignatureLink::Detour));
        auto* const before = g_sigClient;
        g_sigClient = [](const char* toc, const char* extra, const uint8_t* modulus, uint8_t* digest) -> int32_t
        { return detour(toc, extra, modulus, digest); };

        uint8_t digest[16]{};
        g_sigChecks = g_sigDigests = 0;
        g_sigVerdict = WXL_INTERFACE_SIGNATURE_MODIFIED;
        Check(detour("Interface\\FrameXML\\FrameXML.toc", "Interface\\FrameXML\\Bindings.xml", nullptr, digest) ==
                      WXL_INTERFACE_SIGNATURE_MODIFIED && g_sigChecks == 1,
              "the FrameXML_CheckSignature detour, listened to: the client judges once, through the detour again");
        const wxl_subscription accept = g_host->Events().Subscribe(WXL_EV_InterfaceSignature, &AcceptSignature, nullptr, 0);
        Check(detour("Interface\\FrameXML\\FrameXML.toc", "Interface\\FrameXML\\Bindings.xml", nullptr, digest) ==
                      WXL_INTERFACE_SIGNATURE_SIGNED && g_sigChecks == 2 && digest[0] == 0xAB,
              "and answers signed in EAX for a file a handler accepts, the stack as the caller left it");
        g_host->Events().Unsubscribe(accept);
        Mute(WXL_EV_InterfaceSignature);
        Check(detour("Interface\\GlueXML\\GlueXML.toc", nullptr, nullptr, digest) == WXL_INTERFACE_SIGNATURE_MODIFIED &&
                  g_sigChecks == 3,
              "with nobody listening, straight to the client's check");
        Unmute(WXL_EV_InterfaceSignature);
        g_sigClient = before;
        SignatureLink::original = nullptr;

        CleanupLink::original = Opaque(&FakeDeleteOriginal);
        void (*const clear)() = Opaque(reinterpret_cast<void (*)()>(&CleanupLink::Detour));
        g_cleared = 0;
        clear();
        const wxl_subscription keep = g_host->Events().Subscribe(WXL_EV_InterfaceCleanupRequest, &RefuseCleanup, nullptr, 0);
        clear();
        g_host->Events().Unsubscribe(keep);
        Check(g_cleared == 1, "the DeleteInterfaceFiles detour runs the client's clearing unless a handler refuses it");
        CleanupLink::original = nullptr;
    }

    // --- SFile::OpenEx's two rows, chained as Install links them, and CMapObj::Load ---------------

    std::string g_openedName;

    /// A stdcall function of SFile::OpenEx's shape that finds nothing, noting the name it got.
    __declspec(noinline) int32_t __stdcall FakeOpenEx(void*, const char* name, uint32_t, void** result)
    {
        g_openedName = name ? name : "";
        if (result) *result = nullptr;
        return 0;
    }

    /// Calls a detour the way SFile::ReadFile calls SFile::OpenEx: four dwords pushed, none
    /// cleaned after the call (stdcall pops them); and records ESP on both sides.
    __declspec(noinline) void CallLikeOpen(void* detour, const char* name, void** result)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            mov  eax, result
            push eax
            push 0
            push name
            push 0
            call detour
            mov  dword ptr [g_espAfter], esp
        }
    }

    using OpenGateSeam = wxl::core::seams::GateOf<wxl::seam::Storage::OpenFile::Convention,
                                                  wxl::seam::Storage::OpenFile::Signature>;
    using OpenGateLink = OpenGateSeam::Link<&wxl::seam::Storage::OpenFile::Gate>;
    using OpenedLink = wxl::core::seams::DetourOf<wxl::seam::Storage::OpenEx::Convention,
                                                  wxl::seam::Storage::OpenEx::Signature>::
        Link<nullptr, &wxl::seam::Storage::OpenEx::After>;

    uint32_t g_mapObjLoads = 0;
    __declspec(noinline) void __fastcall FakeMapObjLoad(void*, void*) { ++g_mapObjLoads; }

    /// Calls a thiscall detour that takes nothing on the stack, as PostloadCallback calls
    /// CMapObj::Load at 0x007D7EB4: the object in ECX.
    __declspec(noinline) void CallLikeMapObjLoad(void* detour, void* self)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            mov  ecx, self
            call detour
            mov  dword ptr [g_espAfter], esp
        }
    }

    using MapObjSeam = wxl::core::seams::DetourOf<wxl::seam::MapObj::Load::Convention,
                                                  wxl::seam::MapObj::Load::Signature>;
    using MapObjLink = MapObjSeam::Link<&wxl::seam::MapObj::Load::Before, nullptr>;

    void CheckAssetDetours()
    {
        const wxl_subscription sub = g_host->Events().Subscribe(WXL_EV_FileOpen, &OnFileOpen, nullptr, 0);
        OpenGateLink::original = Opaque(reinterpret_cast<OpenGateSeam::Fn>(&OpenedLink::Detour));
        OpenedLink::original   = Opaque(&FakeOpenEx);
        void* const gate       = Opaque(reinterpret_cast<void*>(&OpenGateLink::Detour));

        g_answerWith = "Mods\\example\\Fireball.blp";
        void* file   = reinterpret_cast<void*>(1);
        g_received.clear();
        CallLikeOpen(gate, "Interface\\Icons\\Fireball.blp", &file);
        Check(g_espBefore == g_espAfter, "the OpenFile gate and the OpenEx observer pop their four as stdcall does");
        Check(g_openedName == g_answerWith, "the client is handed the path the FileOpen handler wrote");
        const Received* opened = Only(WXL_EV_FileOpened);
        Check(opened && opened->As<wxl_file_opened_args>().found == WXL_FALSE &&
                  opened->As<wxl_file_opened_args>().path.size == strlen(g_answerWith),
              "and the observer, nearer the function, reports the open of that path");
        g_answerWith = nullptr;
        g_host->Events().Unsubscribe(sub);
        OpenGateLink::original = nullptr;
        OpenedLink::original   = nullptr;

        FakeMapObj keep;
        strcpy(keep.name, "World\\wmo\\Test.wmo");
        keep.bytes = "REVM";
        MapObjLink::original = Opaque(reinterpret_cast<MapObjSeam::Fn>(&FakeMapObjLoad));
        g_received.clear();
        CallLikeMapObjLoad(Opaque(reinterpret_cast<void*>(&MapObjLink::Detour)), &keep);
        Check(g_espBefore == g_espAfter && g_mapObjLoads == 1,
              "the MapObj.Load detour takes the object in ECX, runs the original, cleans nothing");
        Check(Only(WXL_EV_MapObjectLoad) != nullptr, "and Before raised MapObjectLoad for it");
        MapObjLink::original = nullptr;
    }

    // --- System_SFile2::FindFile: the gate, called as SFile::FileExistsEx calls it ---------------

    int g_findCalls = 0;

    /// A cdecl function of FindFile's shape that finds nothing.
    __declspec(noinline) int32_t __cdecl FakeFindFile(const char*, char*, uint32_t, uint32_t, uint32_t*, void**)
    {
        ++g_findCalls;
        return 0;
    }

    uint32_t g_eaxAfter = 0;

    /// Six dwords pushed, `add esp, 0x18` after the call, as FileExistsEx at 0x00424B33 does.
    __declspec(noinline) void CallLikeFindFile(void* detour, const char* name, char* path, uint32_t* kind, void** found)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            push found
            push kind
            push 0
            push 0x104
            push path
            push name
            call detour
            add  esp, 0x18
            mov  dword ptr [g_espAfter], esp
            mov  dword ptr [g_eaxAfter], eax
        }
    }

    using FindGateSeam = wxl::core::seams::GateOf<wxl::seam::Storage::FindFile::Convention,
                                                  wxl::seam::Storage::FindFile::Signature>;
    using FindGateLink = FindGateSeam::Link<&wxl::seam::Storage::FindFile::Gate>;

    void CheckFindFileDetour()
    {
        BuildFakeStack();
        const wxl_storage_api& api = wxl::core::services::StorageApi();
        api.serve(Str("Interface\\Served.txt"), "x", 1, 1);
        FindGateLink::original = Opaque(reinterpret_cast<FindGateSeam::Fn>(&FakeFindFile));
        void* const detour     = Opaque(reinterpret_cast<void*>(&FindGateLink::Detour));

        char     path[260] = {};
        uint32_t type      = 3;
        void*    archive   = reinterpret_cast<void*>(1);
        g_findCalls        = 0;
        CallLikeFindFile(detour, "Interface\\Served.txt", path, &type, &archive);
        Check(g_espBefore == g_espAfter && g_eaxAfter == 1 && g_findCalls == 0,
              "the FindFile detour takes a served name: eax 1, the caller cleans its six dwords");
        Check(std::strncmp(path, "|wxl|", 5) == 0 && type == 0 && archive == nullptr,
              "with the file layer's path, a plain file, no archive");
        CallLikeFindFile(detour, "Interface\\Missing.txt", path, &type, &archive);
        Check(g_espBefore == g_espAfter && g_eaxAfter == 0 && g_findCalls == 1,
              "and hands any other name to the client's FindFile");
        FindGateLink::original = nullptr;
        wxl::core::services::assets::ReleaseAll();
        wxl::core::services::assets::ForgetInstall();
    }

    // --- The Vulkan renderer behind the stock client's device (gx/x86/Adapter.cpp) ---------------

    namespace gx86 = wxl::core::gx::x86;

    uint32_t g_stockCalls = 0; ///< calls into the stand-ins of Wow.exe's functions

    /// A stand-in for one function of calls_x86.def, in the row's convention: it answers zero.
    template <auto Member, class Fn>
    struct StockStandIn;

    template <auto Member, class R, class Self, class... A>
    struct StockStandIn<Member, R(__fastcall*)(Self, void*, A...)>
    {
        static R __fastcall Call(Self, void*, A...)
        {
            ++g_stockCalls;
            if constexpr (!std::is_void_v<R>) return R{};
        }
    };

    template <auto Member, class R, class... A>
    struct StockStandIn<Member, R(__stdcall*)(A...)>
    {
        static R __stdcall Call(A...)
        {
            ++g_stockCalls;
            if constexpr (!std::is_void_v<R>) return R{};
        }
    };

    template <auto Member, class R, class... A>
    struct StockStandIn<Member, R(__cdecl*)(A...)>
    {
        static R __cdecl Call(A...)
        {
            ++g_stockCalls;
            if constexpr (!std::is_void_v<R>) return R{};
        }
    };

    /// Blocks a slot's pointer parameters point at: every dword of one points at a zeroed block, so
    /// a reference to a pointer (CGxShader*&, CGxQuery*&) and an object's pointer members (a
    /// buffer's pool) read a zeroed object.
    struct alignas(16) ScratchBlock
    {
        uint8_t bytes[0x4000];
    };
    std::vector<std::unique_ptr<ScratchBlock>> g_scratch;

    void* Scratch()
    {
        g_scratch.push_back(std::make_unique<ScratchBlock>());
        std::memset(g_scratch.back()->bytes, 0, sizeof(ScratchBlock));
        void* inner = nullptr;
        g_scratch.push_back(std::make_unique<ScratchBlock>());
        std::memset(g_scratch.back()->bytes, 0, sizeof(ScratchBlock));
        inner = g_scratch.back()->bytes;
        void* outer = g_scratch[g_scratch.size() - 2]->bytes;
        for (size_t at = 0; at < sizeof(ScratchBlock); at += sizeof inner) std::memcpy(static_cast<uint8_t*>(outer) + at, &inner, sizeof inner);
        return outer;
    }

    template <class T>
    uint32_t SlotArg()
    {
        if constexpr (std::is_pointer_v<T>) return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(Scratch()));
        else return 0;
    }

    template <class Sig>
    struct SlotArgs;

    template <class R, class Self, class... A>
    struct SlotArgs<R(Self, A...)>
    {
        static std::vector<uint32_t> Make() { return {SlotArg<A>()...}; }
        static constexpr bool kFloat = std::is_floating_point_v<R>;
    };

    uint32_t g_slotEspBefore = 0, g_slotEspAfter = 0;

    /// Calls a slot as the engine calls a vtable entry: the arguments pushed right to left, `this`
    /// in ECX, the callee popping them; ESP is read on both sides. A float answer is left on the
    /// x87 stack by the callee and taken off here.
    __declspec(noinline) void CallSlot(void* fn, void* self, const uint32_t* args, uint32_t count, int32_t floating)
    {
        __asm {
            mov  dword ptr [g_slotEspBefore], esp
            mov  ecx, count
            mov  edx, args
        push_next:
            test ecx, ecx
            jz   pushed
            mov  eax, dword ptr [edx + ecx * 4 - 4]
            push eax
            dec  ecx
            jmp  push_next
        pushed:
            mov  ecx, self
            xor  edx, edx
            call fn
            mov  dword ptr [g_slotEspAfter], esp
            cmp  floating, 0
            je   done
            fstp st(0)
        done:
        }
    }

    // The stock vtable stand-in: its D3D9 DeviceCreate and deleting destructor record their calls.
    uint32_t g_d3d9Created = 0, g_d3d9Destroyed = 0;
    int32_t __fastcall StandInD3d9Create(void*, void*, void*, const void*)
    {
        ++g_d3d9Created;
        return 1;
    }
    void* __fastcall StandInD3d9Destructor(void* self, void*, uint32_t)
    {
        ++g_d3d9Destroyed;
        return self;
    }
    void __fastcall StandInKept(void*, void*) {}
    /// The client's window where there is no display: a handle no window has, which the system's
    /// calls on it refuse.
    void* __stdcall StandInCreateWindow(uint32_t, const char*, const char*, uint32_t, int32_t, int32_t, int32_t, int32_t, void*, void*,
                                        void*, void*)
    {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(0x00F00F00));
    }
    intptr_t __cdecl StandInWindowProc(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam)
    {
        return DefWindowProcA(static_cast<HWND>(window), message, wparam, lparam);
    }

    // CGxDevice::ITexWHDStartEnd as Wow.exe's (0x006A5EF0): a mipmapped texture's first level is the
    // device's base mip level, read through ECX (`mov ecx,[ecx+0x350]` at 0x006A5FAE).
    void* g_whdThis = nullptr;
    void __fastcall StandInWHDStartEnd(void* self, void*, void* texture, uint32_t* width, uint32_t* height, uint32_t* base, uint32_t* count)
    {
        g_whdThis       = self;
        const auto* tex = static_cast<const wxl::engine::CGxTex*>(texture);
        *count          = 7; // a 64x64 texture
        *base           = self ? std::min<uint32_t>(static_cast<const wxl::engine::CGxDevice*>(self)->m_baseMipLevel, *count - 1) : 99;
        *width          = tex->m_width >> *base;
        *height         = tex->m_height >> *base;
    }
    void __cdecl StandInTexUser(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*, uint32_t* stride, const void** texels)
    {
        *stride = 0;
        *texels = nullptr;
    }

    // CGxDevice::DeviceCreate as Wow.exe's: the window procedure kept, then DeviceSetFormat through the vtable.
    int32_t __fastcall StandInBaseDeviceCreate(void* self, void*, void* proc, const void* format)
    {
        static_cast<wxl::engine::CGxDevice*>(self)->m_windowProc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(proc));
        using Fn = int32_t(__fastcall*)(void*, void*, const void*);
        return Opaque(reinterpret_cast<Fn>((*static_cast<void* const* const*>(self))[13]))(self, nullptr, format);
    }
    int32_t __fastcall StandInBaseSetFormat(void* self, void*, const void* format)
    {
        std::memcpy(&static_cast<wxl::engine::CGxDevice*>(self)->m_format, format, sizeof(wxl::engine::CGxFormat));
        return 1;
    }

    // The backend table's stand-in: what the adapter asks of the renderer.
    struct BackendCalls
    {
        wxl_result create = WXL_E_FAILED;
        uint32_t   created = 0, destroyed = 0, scenes = 0, presents = 0, draws = 0, caps = 0;
        uint32_t   textures = 0, tex_width = 0, tex_levels = 0;
    } g_backendCalls;

    wxl_gx_vk_backend MakeBackendStandIn()
    {
        wxl_gx_vk_backend b{};
        b.struct_size        = sizeof b;
        b.device_layout_hash = wxl::engine::CGxDevice::kLayoutHash;
        b.select_api         = [](int32_t api) -> int32_t { return api; };
        b.device_create      = [](void*, void*, char* why, uint32_t size) -> wxl_result {
            ++g_backendCalls.created;
            if (g_backendCalls.create != WXL_OK) std::snprintf(why, size, "no Vulkan loader (vulkan-1.dll is not installed: the graphics driver brings it)");
            return g_backendCalls.create;
        };
        b.device_destroy     = [](void*) { ++g_backendCalls.destroyed; };
        b.device_set_window  = [](void*, void*, char*, uint32_t) -> wxl_result { return WXL_OK; };
        b.device_caps        = [](void*) { ++g_backendCalls.caps; };
        b.window_size        = [](void*, uint32_t, uint32_t, int32_t) {};
        b.scene_begin        = [](void*) -> int32_t { return ++g_backendCalls.scenes, WXL_GX_VK_FRAME_OK; };
        b.scene_clear        = [](void*, uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t) {};
        b.draw               = [](void*, const wxl_gx_batch*, int32_t) { ++g_backendCalls.draws; };
        b.scene_present      = [](void*) -> int32_t { return ++g_backendCalls.presents, WXL_GX_VK_FRAME_OK; };
        b.device_restore     = [](void*, char*, uint32_t) -> wxl_result { return WXL_OK; };
        b.gamma_changed      = [](void*) {};
        b.cursor_changed     = [](void*) {};
        b.cursor_set         = [](void*) {};
        b.pool_memory        = [](void*, void*) -> char* { return nullptr; };
        b.pool_resize        = [](void*, void*) {};
        b.pool_destroy       = [](void*, void*) {};
        b.tex_create         = [](void*, void* tex, uint32_t width, uint32_t height, uint32_t levels) -> int32_t {
            ++g_backendCalls.textures;
            g_backendCalls.tex_width  = width;
            g_backendCalls.tex_levels = levels;
            static int record;
            static_cast<wxl::engine::CGxTex*>(tex)->m_apiSpecificData = &record;
            return 1;
        };
        b.tex_destroy        = [](void*, void*) {};
        b.simulate_loss      = [](void*) {};
        b.describe           = [](void*, char* out, uint32_t size) { std::snprintf(out, size, "stand-in"); };
        b.shader_constants   = [](void*, int32_t, uint32_t, uint32_t, const float*) {};
        b.shader_create      = [](void*, void*, int32_t, const char*, const void*, uint32_t) -> int32_t { return 1; };
        b.shader_destroy     = [](void*, void*) {};
        b.tex_lock           = [](void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*) -> char* { return nullptr; };
        b.tex_unlock         = [](void*, void*) {};
        b.pool_discard       = [](void*, void*) {};
        b.pool_written       = [](void*, void*, uint32_t, uint32_t) {};
        b.shader_prewarm     = [](void*, const void* const*, const uint32_t*, uint32_t) {};
        b.target_set         = [](void*) {};
        b.tex_copy           = [](void*, void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t) -> int32_t { return 0; };
        b.tex_stretch        = [](void*, void*, const int32_t*, const int32_t*, uint32_t, uint32_t) -> int32_t { return 0; };
        b.query_create       = [](void*) -> uint32_t { return 0; };
        b.query_destroy      = [](void*, uint32_t) {};
        b.query_begin        = [](void*, uint32_t) -> int32_t { return 0; };
        b.query_end          = [](void*, uint32_t) -> int32_t { return 0; };
        b.query_result       = [](void*, uint32_t, uint32_t*) -> int32_t { return 1; };
        b.read_pixels        = [](void*, int32_t, int32_t, uint32_t, uint32_t, void*) -> int32_t { return 0; };
        b.sample_counts      = [](void*) -> uint32_t { return 1; };
        b.bone_palette       = [](void*, const float*, uint32_t) {};
        return b;
    }

    /// The x86 adapter's vtable copy and its thunks, over stand-ins of Wow.exe's functions and of
    /// the renderer's table: every slot slots.def gives the front pops what the row says, the
    /// kept ones stay the device's own, a device whose Vulkan cannot start gets its vtable back and
    /// makes itself as the D3D9 device, and one whose Vulkan starts draws through the table.
    void CheckGxAdapterX86()
    {
        static void* fakeVtable[84];
        static void* fakeVtableEx[84];
        for (int i = 0; i < 84; ++i) fakeVtable[i] = fakeVtableEx[i] = reinterpret_cast<void*>(&StandInKept);
        fakeVtable[8] = fakeVtableEx[8] = reinterpret_cast<void*>(&StandInD3d9Destructor);
        fakeVtable[10] = fakeVtableEx[10] = reinterpret_cast<void*>(&StandInD3d9Create);

        static gx86::Stock stock;
#define WXL_BINDING_CALL_X86(domain, name, symbol, ret, conv, params) \
    stock.domain##_##name = &StockStandIn<&gx86::Stock::domain##_##name, decltype(stock.domain##_##name)>::Call;
#include "wxl/core/binding/calls_x86.def"
        stock.GxCall_DeviceCreate    = &StandInBaseDeviceCreate;
        stock.GxCall_DeviceSetFormat = &StandInBaseSetFormat;
        stock.GxCall_ITexWHDStartEnd = &StandInWHDStartEnd;
        static void*    devicePtr = nullptr;
        static uint8_t  shadow[2 * 0x1008] = {};
        static void*    uiVertex[2] = {};
        static void*    uiPixel     = nullptr;
        static int32_t  adjust[2]   = {};
        static float    aspect      = 0.0f;
        static int32_t  minSize[2]  = {320, 240};
        static uint32_t perBlock[13] = {}, bits[13] = {};
        stock.device_ptr        = &devicePtr;
        stock.shadow_constants  = shadow;
        stock.ui_vertex_shaders = uiVertex;
        stock.ui_pixel_shader   = &uiPixel;
        stock.client_adjust     = adjust;
        stock.win_aspect        = &aspect;
        stock.win_min_size      = minSize;
        stock.bytes_per_block   = perBlock;
        stock.bit_depth         = bits;
        stock.vtables[0]        = fakeVtable;
        stock.vtables[1]        = fakeVtableEx;
        stock.create_window     = &StandInCreateWindow;
        static const wxl_gx_vk_backend backend = MakeBackendStandIn();
        gx86::UseForTests(&stock, &backend);

        // The device: a D3D9 device's bytes, its own vtable, render states to read.
        auto  deviceBlock = std::make_unique<ScratchBlock>();
        std::memset(deviceBlock->bytes, 0, sizeof(ScratchBlock));
        void* device = deviceBlock->bytes;
        auto* engine = static_cast<wxl::engine::CGxDevice*>(device);
        *static_cast<void***>(device) = fakeVtable;
        engine->m_appRenderStates.m_data = Scratch();
        devicePtr = device;

        Check(!gx86::Adopt(Scratch()), "a device whose vtable is neither D3D9's nor D3D9Ex's is not adopted");
        Check(gx86::Adopt(device) && gx86::Adopted(device) && gx86::VtableOf(device) != fakeVtable,
              "a D3D9 device is given the adapter's copy of its vtable");
        void* const* table = gx86::VtableOf(device);
        uint32_t     kept = 0, front = 0, balanced = 0, unbalanced = 0;
        std::string  wrong;
#define WXL_GX_SLOT(index, name, ret, params, pops, kind, d3d9, d3d9_at, d3d9ex, d3d9ex_at) \
        WXL_GX_SLOT_HARNESS_##kind(index, name, ret, params, pops)
#define WXL_GX_SLOT_HARNESS_keep(index, name, ret, params, pops) \
        if (table[index] == fakeVtable[index]) ++kept; else wrong += " kept:" #name;
#define WXL_GX_SLOT_HARNESS_front(index, name, ret, params, pops)                                                       \
        if (table[index] != reinterpret_cast<void*>(&gx86::slots::name)) wrong += " front:" #name;                      \
        else ++front;                                                                                                    \
        if (index != 8 && index != 10)                                                                                   \
        {                                                                                                                \
            const std::vector<uint32_t> args = SlotArgs<ret params>::Make();                                            \
            if (std::getenv("WXL_VK_TRACE")) std::printf("     slot %s\n", #name);                                     \
            CallSlot(table[index], device, args.data(), static_cast<uint32_t>(args.size()), SlotArgs<ret params>::kFloat); \
            if (g_slotEspBefore == g_slotEspAfter && args.size() * 4 == pops) ++balanced;                                \
            else ++unbalanced, wrong += " esp:" #name;                                                                   \
        }
#include "wxl/gx/slots.def"
#undef WXL_GX_SLOT_HARNESS_keep
#undef WXL_GX_SLOT_HARNESS_front
        std::printf("     x86 adapter: %u slot(s) kept, %u taken by the front, %u called and balanced%s\n", kept, front, balanced,
                    wrong.c_str());
        Check(kept == 27 && front == 57 && wrong.empty(), "the copy holds the device's own entries where slots.def keeps them, the "
                                                          "adapter's where the front takes them");
        Check(balanced == 55 && unbalanced == 0, "each slot taken, called as a vtable entry, pops what its row says and leaves ESP as found");

        // Vulkan cannot start: the device is the D3D9 device it was made as again.
        g_backendCalls = BackendCalls{};
        g_d3d9Created  = 0;
        auto* format   = static_cast<wxl::engine::CGxFormat*>(Scratch());
        std::memset(format, 0, sizeof *format);
        format->m_window = 1;
        format->m_size   = {640, 480};
        uint32_t args[2] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&StandInWindowProc)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(format))};
        CallSlot(table[10], device, args, 2, 0);
        Check(g_slotEspBefore == g_slotEspAfter && gx86::VtableOf(device) == fakeVtable && !gx86::AdoptedDevice() && g_d3d9Created == 1,
              "Vulkan that cannot start gives the device its own vtable back and its D3D9 DeviceCreate makes it, with the same format");
        Check(g_backendCalls.created == 1, "after the renderer was asked for the device once, on the client's window");

        // Vulkan starts: the device draws through the table, and goes with it.
        Check(gx86::Adopt(device), "the device is adopted again");
        g_backendCalls        = BackendCalls{};
        g_backendCalls.create = WXL_OK;
        CallSlot(table[10], device, args, 2, 0);
        if (g_backendCalls.created && gx86::AdoptedDevice() == device)
        {
            Check(g_slotEspBefore == g_slotEspAfter && g_backendCalls.caps >= 1 && g_backendCalls.scenes == 1 && engine->m_context == 1,
                  "a device whose Vulkan starts opens its first frame through the table");
            wxl_gx_batch batch{};
            uint32_t     drawArgs[2] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&batch)), 3};
            CallSlot(table[42], device, drawArgs, 2, 0);
            CallSlot(table[38], device, nullptr, 0, 0);
            Check(g_backendCalls.draws == 1 && g_backendCalls.presents == 1 && g_backendCalls.scenes == 2,
                  "its draws and presents reach the table, and the next frame opens");

            // A mipmapped texture bound for the first time: made with every level from the device's
            // base mip, which CGxDevice::ITexWHDStartEnd reads through `this`.
            auto* tex                 = static_cast<wxl::engine::CGxTex*>(Scratch());
            std::memset(tex, 0, sizeof *tex);
            tex->m_width              = 64;
            tex->m_height             = 64;
            tex->m_flags.m_filter     = 4; // trilinear: mips
            tex->m_needsUpdate        = 1;
            tex->m_needsCreation      = 1;
            tex->m_userFunc           = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&StandInTexUser));
            engine->m_baseMipLevel    = 0;
            g_whdThis                 = nullptr;
            uint32_t texArg           = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tex));
            CallSlot(table[0], device, &texArg, 1, 0);
            Check(g_slotEspBefore == g_slotEspAfter && g_whdThis == device && g_backendCalls.textures == 1 &&
                      g_backendCalls.tex_levels == 7 && g_backendCalls.tex_width == 64 && !tex->m_needsCreation,
                  "a texture bound the first time is made from the device's base mip with all its levels (ITexWHDStartEnd given `this`)");
            uint32_t flags = 0;
            g_d3d9Destroyed = 0;
            CallSlot(table[8], device, &flags, 1, 0);
            Check(g_slotEspBefore == g_slotEspAfter && g_backendCalls.destroyed == 1 && g_d3d9Destroyed == 1 &&
                      gx86::VtableOf(device) == fakeVtable && !gx86::AdoptedDevice(),
                  "deleting it releases the Vulkan device, gives back its vtable and runs its own destructor");
        }
        else
        {
            std::printf("     (no window could be made here: the Vulkan path past DeviceSetFormat is the owner's to see)\n");
            Check(gx86::VtableOf(device) == fakeVtable, "and without a window the device falls back as above");
        }
        gx86::UseForTests(nullptr, nullptr);
        g_scratch.clear();
    }
#endif
}


// --- Gameplay: auras, loot, items, quests, groups, the people around -------------------------

namespace
{
    std::string Said(wxl_str text) { return std::string(text.data ? text.data : "", text.size); }

    /// A player-kind object with a descriptor block, tracked as the client would build it.
    void Build(FakeObject& object, uint64_t guid, uint32_t type, uint32_t dwords)
    {
        object.guid   = guid;
        object.typeId = type;
        object.block.assign(dwords, 0u);
        object.block[0] = static_cast<uint32_t>(guid);
        object.block[1] = static_cast<uint32_t>(guid >> 32);
        wxl::seam::ObjectMgr::InitObject::After(&object, nullptr, 9300u);
    }

    void CheckGameplayAuras()
    {
        printf("\ngameplay, auras:\n");
        FakeObject wolf, shaman;
        Build(wolf, 0xF130000000001111ull, WXL_OBJECT_UNIT, 0x94);
        Build(shaman, 0x0000000000000077ull, WXL_OBJECT_PLAYER, 0x500);
        g_held      = {&wolf, &shaman};
        g_auraOwner = &wolf;
        g_timeMs    = 100000;
        g_auraSlots[2] = FakeAura{ shaman.guid, 8050, 0xA0, 70, 0, 18000, 112000 };

        g_received.clear();
        wxl::seam::Unit::OnAuraAdded::After(&wolf, 2u, 1, &g_auraSlots[2], 8050);
        const Received* applied = Only(WXL_EV_UnitAuraApplied);
        Check(applied != nullptr && Count(WXL_EV_UnitAuraChanged) == 1,
              "Unit.OnAuraAdded raises UnitAuraApplied beside UnitAuraChanged");
        if (applied)
        {
            const wxl_aura_args a = applied->As<wxl_aura_args>();
            Check(a.unit == wxl::core::objects::HandleOf(&wolf) && a.unit_guid == wolf.guid && a.slot == 2 &&
                      a.spell_id == 8050 && a.caster_guid == shaman.guid &&
                      a.caster == wxl::core::objects::HandleOf(&shaman) && a.level == 70 && a.flags == 0xA0 &&
                      a.harmful == WXL_TRUE && a.duration == 18000 && a.remaining == 12000,
                  "with the slot, the spell, the caster, the level, harmful, the duration and what is left");
        }

        // A removal hands the copy kept from before the update.
        FakeAura gone{ wolf.guid, 774, 0x18, 60, 3, 0, 0 };
        g_received.clear();
        wxl::seam::Unit::OnAuraRemoved::After(&wolf, 5u, 0, &gone, 774);
        const Received* removed = Only(WXL_EV_UnitAuraRemoved);
        Check(removed && removed->As<wxl_aura_args>().slot == 5 && removed->As<wxl_aura_args>().stacks == 3 &&
                  removed->As<wxl_aura_args>().harmful == WXL_FALSE && removed->As<wxl_aura_args>().duration == 0 &&
                  removed->As<wxl_aura_args>().caster_guid == wolf.guid,
              "Unit.OnAuraRemoved raises UnitAuraRemoved with what the aura was, self-cast, no duration");

        g_auraSlots[2].stacks = 4;
        g_received.clear();
        wxl::seam::Combat::UnitCombatLogAuraDose::Before(&wolf, &g_auraSlots[2], 8050, 3, 4, 1);
        const Received* dose = Only(WXL_EV_UnitAuraRefreshed);
        Check(dose && dose->As<wxl_aura_args>().slot == 2 && dose->As<wxl_aura_args>().stacks == 4 &&
                  dose->As<wxl_aura_args>().previous_stacks == 3,
              "Combat.UnitCombatLogAuraDose raises UnitAuraRefreshed with the slot and both counts");
        g_received.clear();
        wxl::seam::Combat::UnitCombatLogAuraRefreshed::Before(&wolf, &g_auraSlots[2], 8050, 1);
        dose = Only(WXL_EV_UnitAuraRefreshed);
        Check(dose && dose->As<wxl_aura_args>().previous_stacks == 4,
              "Combat.UnitCombatLogAuraRefreshed raises it with the count unchanged");
        g_received.clear();
        wxl::seam::Combat::UnitCombatLogAuraRefreshed::Before(&wolf, &gone, 774, 0);
        Check(g_received.empty(), "an aura that is none of the unit's slots raises nothing");

        Mute(WXL_EV_UnitAuraApplied);
        g_received.clear();
        wxl::seam::Unit::OnAuraAdded::After(&wolf, 2u, 1, &g_auraSlots[2], 8050);
        Check(Count(WXL_EV_UnitAuraApplied) == 0 && Count(WXL_EV_UnitAuraChanged) == 1,
              "nobody listening to UnitAuraApplied: only UnitAuraChanged");
        Unmute(WXL_EV_UnitAuraApplied);

        // The mount display's mirror handler: the field already written, the old value by address.
        wolf.block[0x45]       = 2410;
        const uint32_t walking = 0;
        g_received.clear();
        wxl::seam::Unit::MountDisplayIDUpdateHandler::Before(wolf.guid, 0x45 * 4 - 0x18, 4, &walking, nullptr);
        const Received* mount = Only(WXL_EV_UnitMountChanged);
        Check(mount && mount->As<wxl_mount_args>().display_id == 2410 &&
                  mount->As<wxl_mount_args>().previous_display_id == 0 &&
                  mount->As<wxl_mount_args>().unit_guid == wolf.guid,
              "Unit.MountDisplayIDUpdateHandler raises UnitMountChanged with both display ids");

        g_auraOwner = nullptr;
        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
        wxl::seam::ObjectMgr::ObjDelete::Before(&shaman);
        g_held.clear();
    }

    void CheckGameplayLoot()
    {
        printf("\ngameplay, loot:\n");
        FakeObject boar;
        Build(boar, 0xF130000000002222ull, WXL_OBJECT_UNIT, 0x94);
        g_held = {&boar};

        // OpenLoot: SetObject on the boar, which the client fills before After runs.
        g_lootGuid = 0;
        g_received.clear();
        wxl::seam::Loot::SetObject::Before(&boar, 130, 1);
        g_lootGuid  = boar.guid;
        g_lootKind  = 1;
        g_lootMoney = 130;
        int32_t first[7]  = { 2589, 3, 1, 0, 0, 0, 0 };
        int32_t second[7] = { 4306, 1, 2, 7, -12, 2, 1 };
        memcpy(g_lootSlots[0], first, sizeof first);
        memcpy(g_lootSlots[1], second, sizeof second);
        wxl::seam::Loot::SetObject::After(&boar, 130, 1);
        const Received* opened = Only(WXL_EV_LootOpened);
        Check(opened && opened->As<wxl_loot_args>().source == wxl::core::objects::HandleOf(&boar) &&
                  opened->As<wxl_loot_args>().money == 130 && opened->As<wxl_loot_args>().count == 2 &&
                  opened->As<wxl_loot_args>().kind == WXL_LOOT_CORPSE && Count(WXL_EV_LootClosed) == 0,
              "Loot.SetObject raises LootOpened with the source, the kind, the money and the count");
        Check(Count(WXL_EV_LootSlot) == 2 && g_received.back().As<wxl_loot_slot_args>().item_id == 4306 &&
                  g_received.back().As<wxl_loot_slot_args>().slot == 2 &&
                  g_received.back().As<wxl_loot_slot_args>().random_property_id == -12 &&
                  g_received.back().As<wxl_loot_slot_args>().slot_type == WXL_LOOT_SLOT_ROLL &&
                  g_received.back().As<wxl_loot_slot_args>().index == 1,
              "then a LootSlot per item, in the window's order");

        g_received.clear();
        wxl::seam::Loot::ClearSlot::Before(2);
        const Received* cleared = Only(WXL_EV_LootSlotCleared);
        Check(cleared && cleared->As<wxl_loot_slot_args>().item_id == 4306 && cleared->As<wxl_loot_slot_args>().index == 1,
              "Loot.ClearSlot raises LootSlotCleared with the entry it empties");
        g_received.clear();
        wxl::seam::Loot::ClearSlot::Before(9);
        Check(g_received.empty(), "a slot the window does not hold raises nothing");

        // CloseLoot: SetObject on nothing.
        g_received.clear();
        wxl::seam::Loot::SetObject::Before(nullptr, 0, 0);
        g_lootGuid = 0;
        memset(g_lootSlots, 0, sizeof g_lootSlots);
        wxl::seam::Loot::SetObject::After(nullptr, 0, 0);
        const Received* closed = Only(WXL_EV_LootClosed);
        Check(closed && closed->As<wxl_loot_args>().source_guid == boar.guid && Count(WXL_EV_LootOpened) == 0,
              "and LootClosed for the window it closes");

        Body money;
        money.U32(57).U8(0);
        FakeStore store = money.Store();
        g_received.clear();
        wxl::seam::Player::PH_LOOT_OPCODES::Before(nullptr, 0x163, 0, &store);
        Check(Only(WXL_EV_LootMoney) && Only(WXL_EV_LootMoney)->As<wxl_loot_money_args>().amount == 57 &&
                  Only(WXL_EV_LootMoney)->As<wxl_loot_money_args>().alone == WXL_FALSE,
              "Player.PH_LOOT_OPCODES raises LootMoney for SMSG_LOOT_MONEY_NOTIFY");

        Body start;
        start.U64(boar.guid).U32(530).U32(1).U32(4306).U32(7).U32(0xFFFFFFF4u).U32(1).U32(60000).U8(0x0F);
        g_uiMap    = 530;
        g_nextRoll = 12;
        store      = start.Store();
        g_received.clear();
        wxl::seam::Player::PH_LOOT_OPCODES::Before(nullptr, 0x2A1, 0, &store);
        const Received* roll = Only(WXL_EV_LootRollStart);
        Check(roll && roll->As<wxl_loot_roll_start_args>().roll_id == 12 &&
                  roll->As<wxl_loot_roll_start_args>().duration == 60000 &&
                  roll->As<wxl_loot_roll_start_args>().random_property_id == -12 &&
                  roll->As<wxl_loot_roll_start_args>().vote_mask == 0x0F,
              "LootRollStart for SMSG_LOOT_START_ROLL with the id the client will give the roll");
        g_uiMap = 571;
        store   = start.Store();
        g_received.clear();
        wxl::seam::Player::PH_LOOT_OPCODES::Before(nullptr, 0x2A1, 0, &store);
        Check(g_received.empty(), "and nothing for a roll on another map, which the client drops");

        Body rolled;
        rolled.U64(boar.guid).U32(1).U64(0x77).U32(4306).U32(7).U32(0).U8(88).U8(2).U8(0);
        store = rolled.Store();
        g_received.clear();
        wxl::seam::Player::PH_LOOT_OPCODES::Before(nullptr, 0x2A2, 0, &store);
        Check(Only(WXL_EV_LootRoll) && Only(WXL_EV_LootRoll)->As<wxl_loot_roll_args>().roll == 88 &&
                  Only(WXL_EV_LootRoll)->As<wxl_loot_roll_args>().roll_type == WXL_ROLL_GREED &&
                  Only(WXL_EV_LootRoll)->As<wxl_loot_roll_args>().player_guid == 0x77,
              "LootRoll for SMSG_LOOT_ROLL");
        Body won;
        won.U64(boar.guid).U32(1).U32(4306).U32(7).U32(0).U64(0x77).U8(88).U8(2);
        store = won.Store();
        g_received.clear();
        wxl::seam::Player::PH_LOOT_OPCODES::Before(nullptr, 0x29F, 0, &store);
        Check(Only(WXL_EV_LootRollWon) && Only(WXL_EV_LootRollWon)->As<wxl_loot_roll_args>().random_suffix == 7,
              "LootRollWon for SMSG_LOOT_ROLL_WON");
        Body passed;
        passed.U64(boar.guid).U32(1).U32(4306).U32(0xFFFFFFF4u).U32(7);
        store = passed.Store();
        g_received.clear();
        wxl::seam::Player::PH_LOOT_OPCODES::Before(nullptr, 0x29E, 0, &store);
        Check(Only(WXL_EV_LootAllPassed) && Only(WXL_EV_LootAllPassed)->As<wxl_loot_roll_args>().random_property_id == -12 &&
                  Only(WXL_EV_LootAllPassed)->As<wxl_loot_roll_args>().random_suffix == 7,
              "LootAllPassed for SMSG_LOOT_ALL_PASSED, its property read before its suffix");
        Check(store.read == 0u, "the loot handler's reads leave the cursor where it was");

        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
        g_held.clear();
    }

    void CheckGameplayItems()
    {
        printf("\ngameplay, items:\n");
        FakeObject me, bag, sword;
        Build(me, 0x0000000000000042ull, WXL_OBJECT_PLAYER, 0x500);
        Build(bag, 0x4000000000000100ull, WXL_OBJECT_CONTAINER, 0x8A);
        Build(sword, 0x4000000000000200ull, WXL_OBJECT_ITEM, 0x40);
        g_held = {&me, &bag, &sword};

        Body push;
        push.U64(me.guid).U32(0).U32(0).U32(1).U8(255).U32(23).U32(2589).U32(0).U32(0).U32(3).U32(10);
        FakeStore store = push.Store();
        g_received.clear();
        wxl::seam::Player::Packet_Group_9::Before(nullptr, 0x166, 0, &store);
        const Received* pushed = Only(WXL_EV_ItemPushed);
        Check(pushed && pushed->As<wxl_item_push_args>().item_id == 2589 && pushed->As<wxl_item_push_args>().bag == 255 &&
                  pushed->As<wxl_item_push_args>().slot == 23 && pushed->As<wxl_item_push_args>().total == 10 &&
                  pushed->As<wxl_item_push_args>().received == WXL_FALSE &&
                  pushed->As<wxl_item_push_args>().announced == WXL_TRUE,
              "Player.Packet_Group_9 raises ItemPushed for SMSG_ITEM_PUSH_RESULT");
        Body failed;
        failed.U8(1).U64(sword.guid).U64(0).U8(0).U32(40);
        store = failed.Store();
        g_received.clear();
        wxl::seam::Player::Packet_Group_9::Before(nullptr, 0x112, 0, &store);
        Check(Only(WXL_EV_InventoryError) && Only(WXL_EV_InventoryError)->As<wxl_inventory_error_args>().required_level == 40 &&
                  Only(WXL_EV_InventoryError)->As<wxl_inventory_error_args>().item_guid == sword.guid,
              "InventoryError for SMSG_INVENTORY_CHANGE_FAILURE, with the level a level refusal needs");
        Body fine;
        fine.U8(0);
        store = fine.Store();
        g_received.clear();
        wxl::seam::Player::Packet_Group_9::Before(nullptr, 0x112, 0, &store);
        Check(g_received.empty(), "and nothing for a result of 0, which is no failure");

        // The backpack's first slot (23) gets the sword: PLAYER_FIELD_INV_SLOT_HEAD + 46.
        const uint32_t at = 0x144 + 23 * 2;
        me.block[at]      = static_cast<uint32_t>(sword.guid);
        me.block[at + 1]  = static_cast<uint32_t>(sword.guid >> 32);
        const uint64_t none = 0;
        g_received.clear();
        wxl::seam::Container::UpdateInvContents::Before(me.guid, (at - 0x94) * 4, 8, &none, nullptr);
        const Received* slot = Only(WXL_EV_InventorySlotChanged);
        Check(slot && slot->As<wxl_inventory_slot_args>().slot == 23 &&
                  slot->As<wxl_inventory_slot_args>().item_guid == sword.guid &&
                  slot->As<wxl_inventory_slot_args>().item == wxl::core::objects::HandleOf(&sword) &&
                  slot->As<wxl_inventory_slot_args>().container == wxl::core::objects::HandleOf(&me) &&
                  slot->As<wxl_inventory_slot_args>().previous_guid == 0,
              "Container.UpdateInvContents raises InventorySlotChanged for the player's slot");
        g_received.clear();
        wxl::seam::Container::InvUpdateHandler::Before(me.guid, (at - 0x94) * 4, 8, &sword.guid, nullptr);
        Check(g_received.empty(), "a slot that holds what it held raises nothing");

        bag.block[0x42 + 4]     = static_cast<uint32_t>(sword.guid);
        bag.block[0x42 + 5]     = static_cast<uint32_t>(sword.guid >> 32);
        g_received.clear();
        wxl::seam::Container::UpdateContainerContents::Before(bag.guid, 8 + 2 * 8, 8, &none, nullptr);
        slot = Only(WXL_EV_InventorySlotChanged);
        Check(slot && slot->As<wxl_inventory_slot_args>().slot == 2 &&
                  slot->As<wxl_inventory_slot_args>().container_guid == bag.guid,
              "Container.UpdateContainerContents raises it for the bag's slot");

        sword.block[0x3C]   = 40;
        sword.block[0x3D]   = 75;
        const uint32_t was  = 41;
        g_received.clear();
        wxl::seam::Item::OnUpdateDurability::Before(sword.guid, (0x3C - 6) * 4, 4, &was, nullptr);
        Check(Only(WXL_EV_ItemDurabilityChanged) &&
                  Only(WXL_EV_ItemDurabilityChanged)->As<wxl_item_durability_args>().current == 40 &&
                  Only(WXL_EV_ItemDurabilityChanged)->As<wxl_item_durability_args>().previous == 41 &&
                  Only(WXL_EV_ItemDurabilityChanged)->As<wxl_item_durability_args>().maximum == 75,
              "Item.OnUpdateDurability raises ItemDurabilityChanged with the maximum");
        sword.block[0x0E]  = 5;
        const uint32_t two = 2;
        g_received.clear();
        wxl::seam::Item::OnUpdateStackCount::Before(sword.guid, (0x0E - 6) * 4, 4, &two, nullptr);
        Check(Only(WXL_EV_ItemStackChanged) && Only(WXL_EV_ItemStackChanged)->As<wxl_item_stack_args>().current == 5 &&
                  Only(WXL_EV_ItemStackChanged)->As<wxl_item_stack_args>().previous == 2,
              "Item.OnUpdateStackCount raises ItemStackChanged");

        wxl::seam::ObjectMgr::ObjDelete::Before(&me);
        wxl::seam::ObjectMgr::ObjDelete::Before(&bag);
        wxl::seam::ObjectMgr::ObjDelete::Before(&sword);
        g_held.clear();
    }

    void CheckGameplayQuests()
    {
        printf("\ngameplay, quests:\n");
        FakeObject me, giver;
        Build(me, 0x0000000000000042ull, WXL_OBJECT_PLAYER, 0x500);
        Build(giver, 0xF130000000003333ull, WXL_OBJECT_UNIT, 0x94);
        g_held = {&me, &giver};

        // Slot 1 of the log: PLAYER_QUEST_LOG_2_1, the player block's dword 15.
        const uint32_t entry = 0x94 + 0x0A + 5;
        uint32_t       before[5]{};
        me.block[entry] = 783;
        g_received.clear();
        wxl::seam::Player::OnUpdateQuest::Before(me.guid, (0x0A + 5) * 4, 20, before, nullptr);
        Check(Only(WXL_EV_QuestAccepted) && Only(WXL_EV_QuestAccepted)->As<wxl_quest_args>().quest_id == 783 &&
                  Only(WXL_EV_QuestAccepted)->As<wxl_quest_args>().log_slot == 1,
              "Player.OnUpdateQuest raises QuestAccepted for a quest new in the log");

        uint32_t accepted[5] = { 783, 0, 0, 0, 0 };
        me.block[entry + 2] = 0x00030000u; // the second objective at 3
        me.block[entry + 1] = 1;           // complete
        g_received.clear();
        wxl::seam::Player::OnUpdateQuest::Before(me.guid, (0x0A + 5) * 4, 20, accepted, nullptr);
        const Received* progress = Only(WXL_EV_QuestObjectiveChanged);
        Check(progress && progress->As<wxl_quest_objective_args>().objective == 1 &&
                  progress->As<wxl_quest_objective_args>().current == 3 &&
                  progress->As<wxl_quest_objective_args>().previous == 0 && Only(WXL_EV_QuestCompleted) &&
                  Count(WXL_EV_QuestAccepted) == 0,
              "QuestObjectiveChanged for the count that moved, and QuestCompleted as its state completes");

        uint32_t done[5] = { 783, 1, 0x00030000u, 0, 0 };
        for (uint32_t i = 0; i < 5; ++i) me.block[entry + i] = 0;
        g_received.clear();
        wxl::seam::Player::OnUpdateQuest::Before(me.guid, (0x0A + 5) * 4, 20, done, nullptr);
        Check(Only(WXL_EV_QuestRemoved) && Only(WXL_EV_QuestRemoved)->As<wxl_quest_args>().quest_id == 783 &&
                  g_received.size() == 1,
              "QuestRemoved when the entry empties");

        Body complete;
        complete.U32(783).U32(1250).U32(0xFFFFFF9Cu).U32(0).U32(0).U32(0);
        FakeStore store = complete.Store();
        g_received.clear();
        wxl::seam::Player::Packet_Group_12::Before(nullptr, 0x191, 0, &store);
        Check(Only(WXL_EV_QuestTurnedIn) && Only(WXL_EV_QuestTurnedIn)->As<wxl_quest_reward_args>().xp == 1250 &&
                  Only(WXL_EV_QuestTurnedIn)->As<wxl_quest_reward_args>().money == -100,
              "Player.Packet_Group_12 raises QuestTurnedIn with the rewards");

        Body details;
        details.U64(giver.guid).U64(0x99).U32(783).Str("The Defias Brotherhood").Str("Go west.").Str("Kill them.");
        store = details.Store();
        g_received.clear();
        wxl::seam::Player::Packet_Group_12::Before(nullptr, 0x188, 0, &store);
        const Received* detail = Only(WXL_EV_QuestGiverDetail);
        Check(detail && Said(detail->As<wxl_quest_giver_args>().title) == "The Defias Brotherhood" &&
                  Said(detail->As<wxl_quest_giver_args>().text) == "Go west." &&
                  detail->As<wxl_quest_giver_args>().shared_by == 0x99 &&
                  detail->As<wxl_quest_giver_args>().giver == wxl::core::objects::HandleOf(&giver),
              "QuestGiverDetail with the giver, the title, the text and who shared it");
        Body list;
        list.U64(giver.guid).Str("Greetings.").U32(0).U32(1).U8(2);
        store = list.Store();
        g_received.clear();
        wxl::seam::Player::Packet_Group_12::Before(nullptr, 0x185, 0, &store);
        Check(Only(WXL_EV_QuestGiverGreeting) && Only(WXL_EV_QuestGiverGreeting)->As<wxl_quest_giver_args>().count == 2 &&
                  Said(Only(WXL_EV_QuestGiverGreeting)->As<wxl_quest_giver_args>().text) == "Greetings.",
              "QuestGiverGreeting with the greeting and how many quests it lists");
        Body request;
        request.U64(giver.guid).U32(783).Str("Title").Str("Bring them.");
        store = request.Store();
        g_received.clear();
        wxl::seam::Player::Packet_Group_12::Before(nullptr, 0x18B, 0, &store);
        Check(Only(WXL_EV_QuestGiverProgress) != nullptr, "QuestGiverProgress for SMSG_QUESTGIVER_REQUEST_ITEMS");
        store = request.Store();
        g_received.clear();
        wxl::seam::Player::Packet_Group_12::Before(nullptr, 0x18D, 0, &store);
        Check(Only(WXL_EV_QuestGiverReward) != nullptr, "QuestGiverReward for SMSG_QUESTGIVER_OFFER_REWARD");

        wxl::seam::ObjectMgr::ObjDelete::Before(&me);
        wxl::seam::ObjectMgr::ObjDelete::Before(&giver);
        g_held.clear();
    }

    /// Answers every GroupInvite, DuelRequested and ResurrectRequest with what it holds.
    struct RequestAnswer
    {
        bool accept  = false;
        bool decline = false;
    };

    RequestAnswer g_requestAnswer;

    void WXL_CALL OnRequest(void* user, const void* args)
    {
        const auto kind = reinterpret_cast<uintptr_t>(user);
        wxl_bool* accept  = nullptr;
        wxl_bool* decline = nullptr;
        if (kind == 0) accept = static_cast<const wxl_group_invite_args*>(args)->accept, decline = static_cast<const wxl_group_invite_args*>(args)->decline;
        if (kind == 1) accept = static_cast<const wxl_duel_request_args*>(args)->accept, decline = static_cast<const wxl_duel_request_args*>(args)->decline;
        if (kind == 2) accept = static_cast<const wxl_resurrect_request_args*>(args)->accept, decline = static_cast<const wxl_resurrect_request_args*>(args)->decline;
        if (g_requestAnswer.accept && accept) *accept = WXL_TRUE;
        if (g_requestAnswer.decline && decline) *decline = WXL_TRUE;
    }

    void CheckGameplayGroup()
    {
        printf("\ngameplay, group:\n");
        const wxl_subscription invite = g_host->Events().Subscribe(WXL_EV_GroupInvite, &OnRequest, reinterpret_cast<void*>(0), 0);

        Body body;
        body.U8(1).Str("Jaina").U32(0).U8(0).U32(0);
        FakeStore store = body.Store();
        int32_t   result = 0;
        void*     param = nullptr;
        uint32_t  id = 0x6F, time = 0;
        void*     msg = &store;
        g_answers.clear();
        g_requestAnswer = RequestAnswer{};
        g_received.clear();
        bool taken = wxl::seam::Player::Packet_SMSG_GROUP_INVITE::Gate(&result, &param, &id, &time, &msg);
        Check(!taken && Only(WXL_EV_GroupInvite) && Said(Only(WXL_EV_GroupInvite)->As<wxl_group_invite_args>().inviter) == "Jaina" &&
                  g_answers.empty(),
              "Player.Packet_SMSG_GROUP_INVITE raises GroupInvite and leaves an unanswered one to the dialog");
        g_requestAnswer.accept = true;
        taken = wxl::seam::Player::Packet_SMSG_GROUP_INVITE::Gate(&result, &param, &id, &time, &msg);
        Check(taken && result == 1 && g_answers.size() == 1 && g_answers[0] == "group accept",
              "an accepted one joins as the dialog's Accept does, and the dialog never shows");
        g_requestAnswer.decline = true;
        taken = wxl::seam::Player::Packet_SMSG_GROUP_INVITE::Gate(&result, &param, &id, &time, &msg);
        Check(taken && g_answers.size() == 2 && g_answers[1] == "group decline", "decline wins over accept");
        Body grouped;
        grouped.U8(0).Str("Jaina").U32(0).U8(0).U32(0);
        store = grouped.Store();
        taken = wxl::seam::Player::Packet_SMSG_GROUP_INVITE::Gate(&result, &param, &id, &time, &msg);
        Check(!taken && g_answers.size() == 2, "an invitation the player cannot take is only told");
        g_host->Events().Unsubscribe(invite);

        // A party of two, the leader the other member, group loot.
        Body list;
        list.U8(0).U8(0).U8(0).U8(0).U64(0x1F00000000000001ull).U32(1).U32(1)
            .Str("Thrall").U64(0x51).U8(1).U8(0).U8(1).U8(2)
            .U64(0x51).U8(3).U64(0).U8(2).U8(0).U8(0).U8(0);
        store = list.Store();
        g_received.clear();
        wxl::seam::Player::Packet_SMSG_GROUP_LIST::Before(nullptr, 0x7D, 0, &store);
        const Received* group = Only(WXL_EV_GroupUpdated);
        Check(group && group->As<wxl_group_args>().count == 1 && group->As<wxl_group_args>().leader_guid == 0x51 &&
                  group->As<wxl_group_args>().loot_method == WXL_LOOT_GROUP &&
                  group->As<wxl_group_args>().loot_threshold == 2 && group->As<wxl_group_args>().raid == WXL_FALSE,
              "Player.Packet_SMSG_GROUP_LIST raises GroupUpdated");
        Check(Only(WXL_EV_GroupMember) && Said(Only(WXL_EV_GroupMember)->As<wxl_group_member_args>().name) == "Thrall" &&
                  Only(WXL_EV_GroupMember)->As<wxl_group_member_args>().roles == 2 &&
                  Only(WXL_EV_GroupMember)->As<wxl_group_member_args>().online == WXL_TRUE,
              "a GroupMember per member");
        Check(Only(WXL_EV_GroupLootMethodChanged) != nullptr, "and GroupLootMethodChanged for the first rules it sees");
        store = list.Store();
        g_received.clear();
        wxl::seam::Player::Packet_SMSG_GROUP_LIST::Before(nullptr, 0x7D, 0, &store);
        Check(g_received.empty(), "the same sequence again within a minute is dropped, as the client drops it");

        store = Body{}.Store();
        g_received.clear();
        wxl::seam::Player::Packet_SMSG_GROUP_DESTROYED::Before(nullptr, 0x37F, 0, &store);
        Check(Only(WXL_EV_GroupLeft) && Only(WXL_EV_GroupLeft)->As<wxl_group_left_args>().reason == WXL_GROUP_DISBANDED,
              "Player.Packet_SMSG_GROUP_DESTROYED raises GroupLeft, disbanded");
        g_received.clear();
        wxl::seam::Player::Packet_SMSG_GROUP_UNINVITE::Before(nullptr, 0x77, 0, &store);
        Check(Only(WXL_EV_GroupLeft) && Only(WXL_EV_GroupLeft)->As<wxl_group_left_args>().reason == WXL_GROUP_REMOVED,
              "Player.Packet_SMSG_GROUP_UNINVITE raises GroupLeft, removed");

        Body check;
        check.U64(0x51);
        store = check.Store();
        g_received.clear();
        wxl::seam::Raid::Packet_MSG_RAID_READY_CHECK::Before(nullptr, 0x322, 0, &store);
        Check(Only(WXL_EV_ReadyCheck) && Only(WXL_EV_ReadyCheck)->As<wxl_ready_check_args>().duration == 30000 &&
                  Only(WXL_EV_ReadyCheck)->As<wxl_ready_check_args>().initiator_guid == 0x51,
              "Raid.Packet_MSG_RAID_READY_CHECK raises ReadyCheck");
        Body confirm;
        confirm.U64(0x52).U8(1);
        store = confirm.Store();
        g_received.clear();
        wxl::seam::Raid::Packet_MSG_RAID_READY_CHECK_CONFIRM::Before(nullptr, 0x3AE, 0, &store);
        Check(Only(WXL_EV_ReadyCheckResponse) && Only(WXL_EV_ReadyCheckResponse)->As<wxl_ready_check_response_args>().ready == WXL_TRUE,
              "Raid.Packet_MSG_RAID_READY_CHECK_CONFIRM raises ReadyCheckResponse");
        g_received.clear();
        wxl::seam::Raid::Packet_MSG_RAID_READY_CHECK_FINISHED::Before(nullptr, 0x3C6, 0, nullptr);
        Check(Only(WXL_EV_ReadyCheckFinished) &&
                  Only(WXL_EV_ReadyCheckFinished)->As<wxl_ready_check_end_args>().initiator_guid == 0x51,
              "Raid.Packet_MSG_RAID_READY_CHECK_FINISHED raises ReadyCheckFinished with the check's initiator");
    }

    void CheckGameplaySocial()
    {
        printf("\ngameplay, social:\n");
        FakeObject me, rival, priest;
        Build(me, 0x0000000000000042ull, WXL_OBJECT_PLAYER, 0x500);
        Build(rival, 0x0000000000000043ull, WXL_OBJECT_PLAYER, 0x500);
        Build(priest, 0x0000000000000044ull, WXL_OBJECT_PLAYER, 0x500);
        g_held        = {&me, &rival, &priest};
        g_localPlayer = &me;

        const wxl_subscription duel = g_host->Events().Subscribe(WXL_EV_DuelRequested, &OnRequest, reinterpret_cast<void*>(1), 0);
        const wxl_subscription res  = g_host->Events().Subscribe(WXL_EV_ResurrectRequest, &OnRequest, reinterpret_cast<void*>(2), 0);
        Body challenge;
        challenge.U64(0xF110000000000009ull).U64(rival.guid);
        FakeStore store = challenge.Store();
        int32_t   result = 0;
        void*     param = nullptr;
        uint32_t  id = 0x167, time = 0;
        void*     msg = &store;
        g_answers.clear();
        g_requestAnswer = RequestAnswer{};
        g_received.clear();
        bool taken = wxl::seam::Duel::Packet_SMSG_DUEL_REQUESTED::Gate(&result, &param, &id, &time, &msg);
        Check(!taken && Only(WXL_EV_DuelRequested) &&
                  Only(WXL_EV_DuelRequested)->As<wxl_duel_request_args>().challenger == wxl::core::objects::HandleOf(&rival),
              "Duel.Packet_SMSG_DUEL_REQUESTED raises DuelRequested and leaves an unanswered one to the dialog");
        g_requestAnswer.decline = true;
        taken = wxl::seam::Duel::Packet_SMSG_DUEL_REQUESTED::Gate(&result, &param, &id, &time, &msg);
        Check(taken && result == 1 && g_answers.size() == 1 &&
                  g_answers[0] == "duel decline " + std::to_string(0xF110000000000009ull) + " " + std::to_string(rival.guid),
              "a declined one is cancelled with its arbiter, and the dialog never shows");
        Body own;
        own.U64(0xF110000000000009ull).U64(me.guid);
        store = own.Store();
        g_received.clear();
        taken = wxl::seam::Duel::Packet_SMSG_DUEL_REQUESTED::Gate(&result, &param, &id, &time, &msg);
        Check(!taken && g_received.empty(), "the player's own challenge, which the client accepts itself, is no request");

        Body offer;
        offer.U64(priest.guid).Sized("Anduin").U8(1).U8(0);
        store = offer.Store();
        g_requestAnswer = RequestAnswer{};
        g_requestAnswer.accept = true;
        g_received.clear();
        taken = wxl::seam::Player::Packet_SMSG_RESURRECT_REQUEST::Gate(&result, &param, &id, &time, &msg);
        Check(taken && Only(WXL_EV_ResurrectRequest) &&
                  Said(Only(WXL_EV_ResurrectRequest)->As<wxl_resurrect_request_args>().name) == "Anduin" &&
                  g_answers.size() == 2 && g_answers[1] == "resurrect accept " + std::to_string(priest.guid) + " sick",
              "Player.Packet_SMSG_RESURRECT_REQUEST raises ResurrectRequest and accepts the offer it kept");
        g_host->Events().Unsubscribe(duel);
        g_host->Events().Unsubscribe(res);

        Body countdown;
        countdown.U32(3000);
        store = countdown.Store();
        g_received.clear();
        wxl::seam::Duel::Packet_SMSG_DUEL_COUNTDOWN::Before(nullptr, 0x2B7, 0, &store);
        Check(Only(WXL_EV_DuelCountdown) && Only(WXL_EV_DuelCountdown)->As<wxl_duel_countdown_args>().duration == 3000,
              "Duel.Packet_SMSG_DUEL_COUNTDOWN raises DuelCountdown");
        Body winner;
        winner.U8(1).Str("Garrosh").Str("Varian");
        store = winner.Store();
        g_received.clear();
        wxl::seam::Duel::Packet_SMSG_DUEL_WINNER::Before(nullptr, 0x16B, 0, &store);
        Check(Only(WXL_EV_DuelWon) && Said(Only(WXL_EV_DuelWon)->As<wxl_duel_won_args>().loser) == "Varian" &&
                  Only(WXL_EV_DuelWon)->As<wxl_duel_won_args>().fled == WXL_TRUE,
              "Duel.Packet_SMSG_DUEL_WINNER raises DuelWon");
        Body over;
        over.U8(0);
        store = over.Store();
        g_received.clear();
        wxl::seam::Duel::Packet_SMSG_DUEL_COMPLETE::Before(nullptr, 0x16A, 0, &store);
        Check(Only(WXL_EV_DuelFinished) && Only(WXL_EV_DuelFinished)->As<wxl_duel_finished_args>().completed == WXL_FALSE,
              "Duel.Packet_SMSG_DUEL_COMPLETE raises DuelFinished");

        Body trade;
        trade.U32(1).U64(rival.guid);
        store = trade.Store();
        g_received.clear();
        wxl::seam::Trade::Packet_SMSG_TRADE_STATUS::Before(nullptr, 0x120, 0, &store);
        Check(Only(WXL_EV_TradeStatus) && Only(WXL_EV_TradeStatus)->As<wxl_trade_args>().status == WXL_TRADE_PROPOSED &&
                  Only(WXL_EV_TradeStatus)->As<wxl_trade_args>().partner_guid == rival.guid,
              "Trade.Packet_SMSG_TRADE_STATUS raises TradeStatus, a proposal with its proposer");

        Body emote;
        emote.U32(10).U64(rival.guid);
        store = emote.Store();
        g_received.clear();
        wxl::seam::Player::Packet_SMSG_EMOTE::Before(nullptr, 0x103, 0, &store);
        Check(Only(WXL_EV_Emote) && Only(WXL_EV_Emote)->As<wxl_emote_args>().emote_id == 10 &&
                  Only(WXL_EV_Emote)->As<wxl_emote_args>().unit == wxl::core::objects::HandleOf(&rival),
              "Player.Packet_SMSG_EMOTE raises Emote");
        Body text;
        text.U64(rival.guid).U32(101).U32(0).Sized("Thrall");
        store = text.Store();
        g_received.clear();
        wxl::seam::Chat::OnTextEmote::Before(nullptr, 0x105, 0, &store);
        Check(Only(WXL_EV_TextEmote) && Only(WXL_EV_TextEmote)->As<wxl_text_emote_args>().text_emote == 101 &&
                  Said(Only(WXL_EV_TextEmote)->As<wxl_text_emote_args>().target_name) == "Thrall",
              "Chat.OnTextEmote raises TextEmote");

        // Windows, by the names of the game's event table.
        const char* names[] = { "PLAYER_LOGIN", "MERCHANT_SHOW", "MERCHANT_CLOSED", "QUEST_DETAIL", "QUEST_FINISHED" };
        wxl::core::seams::gameplay::Install(names, 5);
        g_interact = rival.guid;
        g_received.clear();
        wxl::seam::FrameScript::vSignalEvent::Before(1, nullptr, nullptr);
        wxl::seam::FrameScript::vSignalEvent::Before(1, nullptr, nullptr);
        const Received* window = Only(WXL_EV_WindowOpened);
        Check(window && window->As<wxl_window_args>().kind == WXL_WINDOW_MERCHANT &&
                  window->As<wxl_window_args>().npc == wxl::core::objects::HandleOf(&rival),
              "FrameScript.vSignalEvent raises WindowOpened once for MERCHANT_SHOW, on the interface's npc");
        g_interact = 0;
        g_received.clear();
        wxl::seam::FrameScript::vSignalEvent::Before(2, nullptr, nullptr);
        wxl::seam::FrameScript::vSignalEvent::Before(4, nullptr, nullptr);
        window = Only(WXL_EV_WindowClosed);
        Check(window && window->As<wxl_window_args>().npc_guid == rival.guid && Count(WXL_EV_WindowOpened) == 0,
              "WindowClosed for MERCHANT_CLOSED on whom it opened, and nothing for a window never opened");
        g_received.clear();
        wxl::seam::FrameScript::vSignalEvent::Before(0, nullptr, nullptr);
        Check(Count(WXL_EV_WindowOpened) == 0 && Count(WXL_EV_WindowClosed) == 0, "any other event opens no window");
        wxl::core::seams::gameplay::Install(nullptr, 0);

        // Death, release, resurrection, from the client's re-tellings of the player's state.
        wxl::core::seams::gameplay::Forget();
        me.health = 100;
        g_received.clear();
        wxl::seam::GameUI::UpdateActivePlayer::After();
        Check(g_received.empty(), "GameUI.UpdateActivePlayer: the first state seen raises nothing");
        me.health = 0;
        wxl::seam::GameUI::UpdateActivePlayer::After();
        Check(Only(WXL_EV_PlayerDied) && Only(WXL_EV_PlayerDied)->As<wxl_unit_args>().unit == wxl::core::objects::HandleOf(&me),
              "PlayerDied when the health falls to nothing");
        me.health         = 1;
        me.block[0x96]    = 0x10;
        g_received.clear();
        wxl::seam::Player::UpdateGhostStatus::After(&me);
        Check(Only(WXL_EV_PlayerReleased) != nullptr, "Player.UpdateGhostStatus raises PlayerReleased for a new ghost");
        me.block[0x96] = 0;
        g_received.clear();
        wxl::seam::Player::UpdateGhostStatus::After(&me);
        wxl::seam::GameUI::UpdateActivePlayer::After();
        Check(Only(WXL_EV_PlayerResurrected) != nullptr, "and PlayerResurrected once alive again, once");

        // Reputation: list id 3 is faction 72 at base 0, standing 1000, raised to 1250.
        g_factionId = 72, g_factionBase = 0, g_factionStanding = 1000;
        Body rep;
        rep.F32(0.0f).U8(0).U32(2).U32(3).U32(1250).U32(4).U32(0);
        store = rep.Store();
        g_received.clear();
        wxl::seam::Reputation::Packet_SMSG_SET_FACTION_STANDING::Before(nullptr, 0x124, 0, &store);
        Check(Only(WXL_EV_ReputationChanged) && Only(WXL_EV_ReputationChanged)->As<wxl_reputation_args>().faction_id == 72 &&
                  Only(WXL_EV_ReputationChanged)->As<wxl_reputation_args>().previous == 1000 &&
                  Only(WXL_EV_ReputationChanged)->As<wxl_reputation_args>().current == 1250,
              "Reputation.Packet_SMSG_SET_FACTION_STANDING raises ReputationChanged for the faction that moved only");

        Body earned;
        earned.Packed(rival.guid).U32(6).U32(0x0A3B1C2Du).U32(0);
        store = earned.Store();
        g_received.clear();
        wxl::seam::Achievement::Packet_SMSG_ACHIEVEMENT_EARNED::Before(nullptr, 0x468, 0, &store);
        Check(Only(WXL_EV_AchievementEarned) && Only(WXL_EV_AchievementEarned)->As<wxl_achievement_args>().achievement_id == 6 &&
                  Only(WXL_EV_AchievementEarned)->As<wxl_achievement_args>().player_guid == rival.guid &&
                  Only(WXL_EV_AchievementEarned)->As<wxl_achievement_args>().date == 0x0A3B1C2Du,
              "Achievement.Packet_SMSG_ACHIEVEMENT_EARNED raises AchievementEarned, the guid unpacked");

        g_localPlayer = nullptr;
        wxl::core::seams::gameplay::Forget();
        wxl::seam::ObjectMgr::ObjDelete::Before(&me);
        wxl::seam::ObjectMgr::ObjDelete::Before(&rival);
        wxl::seam::ObjectMgr::ObjDelete::Before(&priest);
        g_held.clear();
    }

#if defined(_M_IX86)
    // The gameplay rows' detours, called as the client calls the functions: the handler table's
    // four dwords, the mirror dispatcher's six, a gate that takes the call, a member with
    // nothing on the stack.

    int32_t g_gameplayOriginal = 0;
    __declspec(noinline) int32_t __cdecl FakeHandler(void*, uint32_t, uint32_t, void*) { return ++g_gameplayOriginal; }
    __declspec(noinline) int32_t __cdecl FakeMirrorHandler(uint64_t, uint32_t, uint32_t, const void*, void*)
    {
        return ++g_gameplayOriginal;
    }
    __declspec(noinline) void __fastcall FakeGhostUpdate(void*, void*) { ++g_gameplayOriginal; }

    /// Calls a handler as NetClient::ProcessMessage does at 0x0063202C: four pushed, 0x10 cleaned.
    __declspec(noinline) void CallLikeDispatch(void* handler, uint32_t id, void* msg)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            push msg
            push 0
            push id
            push 0
            mov  ecx, handler
            call ecx
            add  esp, 10h
            mov  dword ptr [g_espAfter], esp
        }
    }

    /// Calls a member that takes nothing on the stack: `this` in ECX.
    __declspec(noinline) void CallMember(void* detour, void* self)
    {
        __asm {
            mov  dword ptr [g_espBefore], esp
            mov  ecx, self
            call detour
            mov  dword ptr [g_espAfter], esp
        }
    }

    void CheckGameplayDetours()
    {
        using wxl::core::seams::DetourOf;
        using wxl::core::seams::GateOf;
        namespace P = wxl::seam::Player;
        using Emote  = DetourOf<P::Packet_SMSG_EMOTE::Convention, P::Packet_SMSG_EMOTE::Signature>::Link<&P::Packet_SMSG_EMOTE::Before, nullptr>;
        using Invite = GateOf<P::Packet_SMSG_GROUP_INVITE::Convention, P::Packet_SMSG_GROUP_INVITE::Signature>::Link<&P::Packet_SMSG_GROUP_INVITE::Gate>;
        using Stack  = DetourOf<wxl::seam::Item::OnUpdateStackCount::Convention, wxl::seam::Item::OnUpdateStackCount::Signature>::Link<&wxl::seam::Item::OnUpdateStackCount::Before, nullptr>;
        using GhostSeam = DetourOf<P::UpdateGhostStatus::Convention, P::UpdateGhostStatus::Signature>;
        using Ghost  = GhostSeam::Link<nullptr, &P::UpdateGhostStatus::After>;

        FakeObject rival, sword;
        Build(rival, 0x0000000000000043ull, WXL_OBJECT_PLAYER, 0x500);
        Build(sword, 0x4000000000000200ull, WXL_OBJECT_ITEM, 0x40);
        g_held = {&rival, &sword};

        Emote::original = Opaque(&FakeHandler);
        Body emote;
        emote.U32(10).U64(rival.guid);
        FakeStore store = emote.Store();
        g_gameplayOriginal = 0;
        g_received.clear();
        CallLikeDispatch(Opaque(reinterpret_cast<void*>(&Emote::Detour)), 0x103, &store);
        Check(g_espBefore == g_espAfter && g_gameplayOriginal == 1 && Only(WXL_EV_Emote),
              "x86: a handler's detour takes the handler table's four dwords, runs the handler and raises Emote");
        Emote::original = nullptr;

        const wxl_subscription invite = g_host->Events().Subscribe(WXL_EV_GroupInvite, &OnRequest, reinterpret_cast<void*>(0), 0);
        Invite::original = Opaque(&FakeHandler);
        Body body;
        body.U8(1).Str("Jaina").U32(0).U8(0).U32(0);
        store = body.Store();
        g_requestAnswer = RequestAnswer{};
        g_requestAnswer.decline = true;
        g_gameplayOriginal = 0;
        g_answers.clear();
        CallLikeDispatch(Opaque(reinterpret_cast<void*>(&Invite::Detour)), 0x6F, &store);
        Check(g_espBefore == g_espAfter && g_gameplayOriginal == 0 && g_answers.size() == 1,
              "x86: the invitation gate takes a declined invitation, the handler not run, the stack as it was");
        g_requestAnswer = RequestAnswer{};
        store = body.Store();
        CallLikeDispatch(Opaque(reinterpret_cast<void*>(&Invite::Detour)), 0x6F, &store);
        Check(g_espBefore == g_espAfter && g_gameplayOriginal == 1, "x86: and hands an unanswered one to the handler");
        Invite::original = nullptr;
        g_host->Events().Unsubscribe(invite);

        Stack::original = Opaque(&FakeMirrorHandler);
        sword.block[0x0E]  = 5;
        const uint32_t two = 2;
        g_gameplayOriginal = 0;
        g_received.clear();
        CallLikeMirror(Opaque(reinterpret_cast<void*>(&Stack::Detour)), static_cast<uint32_t>(sword.guid),
                       static_cast<uint32_t>(sword.guid >> 32), (0x0E - 6) * 4, 4, &two, nullptr);
        Check(g_espBefore == g_espAfter && g_gameplayOriginal == 1 && Only(WXL_EV_ItemStackChanged),
              "x86: a mirror handler's detour takes the six dwords, the guid's two halves in place");
        Stack::original = nullptr;

        FakeObject ghost;
        Ghost::original = Opaque(reinterpret_cast<GhostSeam::Fn>(&FakeGhostUpdate));
        g_gameplayOriginal = 0;
        CallMember(Opaque(reinterpret_cast<void*>(&Ghost::Detour)), &ghost);
        Check(g_espBefore == g_espAfter && g_gameplayOriginal == 1,
              "x86: CGPlayer_C::UpdateGhostStatus's detour takes `this` in ECX and nothing on the stack");
        Ghost::original = nullptr;

        wxl::seam::ObjectMgr::ObjDelete::Before(&rival);
        wxl::seam::ObjectMgr::ObjDelete::Before(&sword);
        g_held.clear();
    }
#endif
}

// --- The profiler, before it starts -----------------------------------------------------------
// What can be checked without a viewer: locations are kept and shared, nothing opens a zone
// before the profiler is up, and the service and the host's zones answer nothing then. The
// profiler is started only by --bench, which then measures a zone with no viewer connected.

#if defined(WXL_CORE_PROFILE) && WXL_CORE_PROFILE
void CheckProfile()
{
    printf("\nprofile:\n");
    namespace profile = wxl::core::profile;
    const void* a = profile::Location("Journal::Flush", "Flush", "Journal.cpp", 12, 0);
    const void* b = profile::Location("Journal::Flush", "Flush", "Journal.cpp", 12, 0);
    const void* c = profile::Location("Journal::Flush", "Flush", "Journal.cpp", 13, 0);
    Check(a && a == b && a != c, "the same five values answer the same location, another line another one");
    Check(!profile::Location("", "f", "file", 1, 0), "an empty name has no location");
    const auto* site = static_cast<const wxl_core_zone_site*>(a);
    Check(std::string(site->name) == "Journal::Flush" && std::string(site->file) == "Journal.cpp" && site->line == 12,
          "a location is laid out as the client's zone sites are, its strings kept");
    Check(!profile::Started() && profile::Begin(a) == 0 && WarcraftXLZoneBegin(site) == 0,
          "no zone opens before the profiler is up");
    profile::End(0);
    WarcraftXLZoneEnd(0);

    const wxl_profile_api& api = profile::Api();
    Check(api.struct_size == sizeof(wxl_profile_api) && api.connected && api.location && api.zone_begin &&
              api.zone_end && api.zone_text && api.plot && api.message && api.frame_mark,
          "wxl.profile fills every member");
    const uintptr_t location = api.location(wxl_str{"Tick", 4}, wxl_str{"", 0}, wxl_str{"", 0}, 0, 0);
    Check(location != 0 && api.zone_begin(location) == 0 && api.connected() == WXL_FALSE,
          "the service answers a location, and no zone while nobody is connected");
    api.zone_end(0);
    api.plot(wxl_str{"lines", 5}, 1.0);
    api.message(wxl_str{"hello", 5});
    api.frame_mark(wxl_str{"mod", 3});
    profile::Present();
    Check(!profile::Connected(), "a present before the profiler is up does nothing");
}
#endif

#if defined(WXL_CORE_PROFILE) && WXL_CORE_PROFILE
// --profile-live: the profiler started as WarcraftXL.ini's [profile] enabled = 1 starts it, on
// 127.0.0.1:8086 (--profile-live-any: every interface), and waited on for up to a minute, so that
// a viewer (or Tracy's capture tool) can connect; then a hundred frames of the host's zones, a
// mod's zone, a client zone, a plot and a message, each frame ended by a present. What a viewer
// then shows is the runtime's profiler end to end, on this architecture, without a client.
void ProfileLive(bool any)
{
    namespace profile = wxl::core::profile;
    wxl::core::settings::ProfileChoice choice;
    choice.enabled = true;
    choice.any     = any;
    profile::Start(choice);
    printf("\nprofile-live: waiting up to 60 s for a viewer on %s port 8086\n", any ? "any interface," : "127.0.0.1");
    for (int i = 0; i < 6000 && !profile::Connected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!profile::Connected())
    {
        printf("profile-live: no viewer came\n");
        ++g_failures;
        return;
    }
    wxl::host::Profiler hooks;
    hooks.location = [](void*, std::string_view name, std::string_view function, uint32_t color) {
        return profile::Location(name, function, "", 0, color);
    };
    hooks.begin = [](void*, const void* location) { return profile::Begin(location); };
    hooks.end   = [](void*, uint64_t zone) { profile::End(zone); };
    wxl::host::EventBus bus({WXL_EV_Update}, nullptr);
    bus.SetProfiler(&hooks, [](void*, uint32_t) { return std::string("harness"); }, nullptr);
    bus.Subscribe(WXL_EV_Update, [](void*, const void*) { std::this_thread::sleep_for(std::chrono::microseconds(200)); },
                  nullptr, 0, 1);
    bus.SetProfiling(true);
    const wxl_profile_api& api      = profile::Api();
    const uintptr_t        location = api.location(wxl_str{"Harness.Work", 12}, wxl_str{"", 0}, wxl_str{"", 0}, 0, 0);
    static const wxl_core_zone_site kSite{"Harness.Client", "ProfileLive", __FILE__, __LINE__, 0};
    wxl_update_args args{};
    for (int frame = 0; frame < 100; ++frame)
    {
        const uint64_t client = WarcraftXLZoneBegin(&kSite);
        bus.Emit(WXL_EV_Update, &args);
        const uintptr_t zone = api.zone_begin(location);
        api.zone_text(zone, wxl_str{"frame", 5});
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        api.zone_end(zone);
        WarcraftXLZoneEnd(client);
        api.plot(wxl_str{"frame", 5}, frame);
        if (frame % 25 == 0) api.message(wxl_str{"quarter", 7});
        profile::Present();
    }
    printf("profile-live: 100 frames sent\n");
    std::this_thread::sleep_for(std::chrono::seconds(2));
}
#endif

// --- wxl.point: the table, the dispatch, and on x86 the detours SafetyHook writes -----------------

namespace
{
    namespace pts = wxl::core::points;

    const wxl_point_api& PointApi() { return pts::Api(); }

    /// What the handlers saw, in the order they ran.
    std::string g_pointTrace;

    struct Pair
    {
        void* data;
        void* param;
    };

    /// A stand-in original of CGCamera::UpdateCallback's generic shape: answers data + param.
    int g_originalRuns = 0;
    void WXL_CALL FakeCameraOriginal(void* const* args, void* result)
    {
        ++g_originalRuns;
        const uintptr_t data  = reinterpret_cast<uintptr_t>(*static_cast<void**>(args[0]));
        const uintptr_t param = reinterpret_cast<uintptr_t>(*static_cast<void**>(args[1]));
        *static_cast<int32_t*>(result) = static_cast<int32_t>(data + param);
        g_pointTrace += "O";
    }

    int32_t EnterCamera(uintptr_t data, uintptr_t param)
    {
        void*       a     = reinterpret_cast<void*>(data);
        void*       b     = reinterpret_cast<void*>(param);
        void* const args[] = {&a, &b};
        int32_t     result = -1;
        pts::Enter(pts::kCGCamera_UpdateCallback, args, &result, &FakeCameraOriginal);
        return result;
    }

    void WXL_CALL PointBefore(wxl_point_call* call, void*)
    {
        g_pointTrace += "B";
        *static_cast<void**>(call->args[0]) = reinterpret_cast<void*>(uintptr_t{100}); // data = 100
    }

    void WXL_CALL PointReplaceCalls(wxl_point_call* call, void* user)
    {
        g_pointTrace += static_cast<const char*>(user);
        PointApi().call_original(call);
        *static_cast<int32_t*>(call->result) += 1;
    }

    void WXL_CALL PointReplaceAnswers(wxl_point_call* call, void*)
    {
        g_pointTrace += "R";
        *static_cast<int32_t*>(call->result) = 7;
    }

    void WXL_CALL PointAfter(wxl_point_call* call, void* user)
    {
        g_pointTrace += static_cast<const char*>(user);
        *static_cast<int32_t*>(call->result) *= 2;
    }

    void WXL_CALL PointFaultsFirst(wxl_point_call*, void*)
    {
        g_pointTrace += "F";
        *static_cast<volatile int*>(nullptr) = 1;
    }

    uint32_t g_seenDepth = 0;
    void WXL_CALL PointDepth(wxl_point_call* call, void*)
    {
        if (call->depth > g_seenDepth) g_seenDepth = call->depth;
        if (call->depth == 1) EnterCamera(1, 2); // a nested call of the same point
    }

    void WXL_CALL PointCallsOriginalFromBefore(wxl_point_call* call, void*) { PointApi().call_original(call); }

    uint32_t Attach(uint32_t index, uint32_t side, wxl_point_fn fn, void* user, int32_t priority = 0, uint32_t hash = 0)
    {
        return PointApi().attach(index + 1, side, fn, user, priority, hash);
    }

#if !defined(_M_IX86)
    volatile uint8_t g_fakeArmed[pts::kCount] = {};
#else
    /// Puts each x86 point on a stand-in of its shape in this image, and the camera's site on an
    /// inlined copy of it.
    void PlaceStandIns();
#endif

    /// Every point of the table through the dispatch, as its realisation calls it: its arguments
    /// by address, its answer's slot, Before, Replace and After once each.
    int g_everyOriginal = 0;
    void WXL_CALL EveryOriginal(void* const*, void* result)
    {
        ++g_everyOriginal;
        if (result) memset(result, 0, 8);
    }
    int g_everySides = 0;
    void WXL_CALL EveryHandler(wxl_point_call* call, void*)
    {
        g_everySides += 1 << (call->side * 4);
        if (call->side == WXL_POINT_REPLACE) PointApi().call_original(call);
    }

    void CheckEngineService()
    {
        const wxl_engine_api& engine = wxl::core::services::EngineApi();
        uint64_t              hash   = 0;
        Check(engine.struct_size == sizeof(wxl_engine_api) && engine.layout("CM2Shared", &hash) == WXL_OK && hash != 0,
              "wxl.engine answers a type's layout hash");
        uint64_t again = 0;
        Check(engine.layout("CGxDevice::TextureTarget", &again) == WXL_OK &&
                  engine.layout("CGxDevice_TextureTarget", &hash) == WXL_OK && again == hash,
              "by the fork's name or the headers' one");
        Check(engine.layout("CNoSuchType", &hash) == WXL_E_NOT_FOUND && engine.layout(nullptr, &hash) == WXL_E_INVALID,
              "and refuses a type it does not describe or no name");
    }

    void CheckEveryPoint()
    {
        const wxl_point_api& api = PointApi();
        int dispatched = 0, here = 0;
        for (uint32_t index = 0; index < pts::kCount; ++index)
        {
            const char* why = "";
            if (!pts::Has(index, &why)) continue;
            ++here;
            uint32_t ids[3];
            for (uint32_t side = 0; side < 3; ++side)
                ids[side] = Attach(index, side, &EveryHandler, nullptr, 0, pts::RowOf(index).layout_hash);
            uint64_t    values[8] = {};
            void*       args[8];
            for (int i = 0; i < 8; ++i) args[i] = &values[i];
            uint64_t    result = 0;
            g_everyOriginal = g_everySides = 0;
            pts::Enter(index, args, pts::RowOf(index).returns ? &result : nullptr, &EveryOriginal);
            if (ids[0] && ids[1] && ids[2] && g_everyOriginal == 1 && g_everySides == 0x111) ++dispatched;
            for (uint32_t id : ids) api.detach(id);
        }
        Check(here > 0 && dispatched == here, "every point this runtime has runs its Before, Replace (calling "
                                              "the original once) and After");
    }

    void CheckPointTable()
    {
        const wxl_point_api& api = PointApi();
        std::vector<wxl_point_info> infos(api.list(nullptr, 0));
        for (wxl_point_info& info : infos) info.struct_size = sizeof info;
        api.list(infos.data(), static_cast<uint32_t>(infos.size()));
        Check(infos.size() == pts::kCount && pts::kCount >= 6, "list names every point of wxl/points.def");
        const wxl_point_info& camera = infos[pts::kCGCamera_UpdateCallback];
        Check(std::string_view(camera.name.data, camera.name.size) == "CGCamera::UpdateCallback" &&
                  std::string_view(camera.signature.data, camera.signature.size) == "i32(ptr,ptr)" &&
                  camera.arg_count == 2 && (camera.flags & WXL_POINT_STABLE) && (camera.flags & WXL_POINT_REPLACEABLE) &&
                  (camera.flags & WXL_POINT_ON_X86) && (camera.flags & WXL_POINT_ON_X64),
              "the camera point: its name, lowered signature, two arguments, stable, replaceable, on both runtimes");
        const wxl_point_info& finish = infos[pts::kCM2Shared_FinishLoading];
        Check((finish.flags & WXL_POINT_ON_X86) && (finish.flags & WXL_POINT_ON_X64) && !(finish.flags & WXL_POINT_STABLE),
              "CM2Shared::FinishLoading is provisional and on both runtimes (Wow.exe has it at 0x0083CF00)");
        const wxl_point_info& init = infos[pts::kM2Init_uint8_t_uint32_t_M2Data];
        Check(std::string_view(init.name.data, init.name.size) == "M2Init(uint8_t*,uint32_t,M2Data&)" &&
                  std::string_view(init.signature.data, init.signature.size) == "i32(ptr,u32,ptr)" &&
                  (init.flags & WXL_POINT_ON_X86) && (init.flags & WXL_POINT_ON_X64),
              "M2Init(uint8_t*,uint32_t,M2Data&): an overload named by its types, its reference an address, on both "
              "runtimes (de-inlined on x86)");

        uint32_t id = 0;
        Check(api.find("No::SuchPoint", &id) == WXL_E_NOT_FOUND, "find: a name the table does not hold is not_found");
#if defined(_M_IX86)
        Check(api.find("CGCamera::UpdateCallback", &id) == WXL_E_UNSUPPORTED &&
                  api.find("M2Init(uint8_t*,uint32_t,M2Data&)", &id) == WXL_E_UNSUPPORTED,
              "find: outside Wow.exe 12340 no x86 point is there (the harness is not the client), a de-inlined "
              "one included");
        PlaceStandIns();
        Check(api.find("CGCamera::UpdateCallback", &id) == WXL_OK, "find: placed on stand-ins, the x86 points are there");
        Check(api.find("M2Init(uint8_t*,uint32_t,M2Data&)", &id) == WXL_OK && id == pts::kM2Init_uint8_t_uint32_t_M2Data + 1,
              "find: a de-inlined point is there once its parent is");
#else
        Check(api.find("CGCamera::UpdateCallback", &id) == WXL_E_UNSUPPORTED,
              "find: before the client hands its table, no x64 point is there");
        std::vector<std::string> rows;
        std::vector<const char*> pointers;
        for (uint32_t i = 0; i < pts::kCount; ++i)
            rows.push_back(std::string(pts::RowOf(i).name) + ": " + pts::RowOf(i).lowered);
        for (const std::string& row : rows) pointers.push_back(row.c_str());
        std::vector<const char*> wrong = pointers;
        wrong[1] = "CM2Cache::CreateShared: ptr(ptr)";
        wxl_core_points table{sizeof(wxl_core_points), pts::kCount, wrong.data(), g_fakeArmed};
        Check(WarcraftXLPoints(&table) == WXL_E_INVALID, "a client table of another signature is refused");
        table.rows = pointers.data();
        Check(WarcraftXLPoints(&table) == WXL_OK, "the client's own table is taken");
        Check(api.find("CM2Shared::FinishLoading", &id) == WXL_OK && id == pts::kCM2Shared_FinishLoading + 1,
              "find: the inlined function is a point of the recompiled client");
#endif
        Check(api.find("CGCamera::UpdateCallback", &id) == WXL_OK && id == pts::kCGCamera_UpdateCallback + 1,
              "find: the camera point's id is its row's position, from 1");
    }

    void CheckPointDispatch()
    {
        const wxl_point_api& api   = PointApi();
        const uint32_t       index = pts::kCGCamera_UpdateCallback;

        g_pointTrace.clear();
        g_originalRuns = 0;
        Check(EnterCamera(3, 4) == 7 && g_originalRuns == 1 && g_pointTrace == "O",
              "with nothing attached a point is its original alone");

        Check(Attach(index, WXL_POINT_REPLACE, &PointReplaceCalls, const_cast<char*>("x"), 0,
                     pts::RowOf(index).layout_hash ^ 1) == WXL_POINT_ATTACHMENT_NONE,
              "a handler built against another layout of the point is refused");
        Check(Attach(index, 7, &PointBefore, nullptr) == WXL_POINT_ATTACHMENT_NONE, "an unknown side is refused");
        Check(api.attach(0, WXL_POINT_BEFORE, &PointBefore, nullptr, 0, 0) == WXL_POINT_ATTACHMENT_NONE &&
                  api.attach(pts::kCount + 1, WXL_POINT_BEFORE, &PointBefore, nullptr, 0, 0) == WXL_POINT_ATTACHMENT_NONE,
              "point ids outside the table are refused");

        const uint32_t before = Attach(index, WXL_POINT_BEFORE, &PointBefore, nullptr, 0, pts::RowOf(index).layout_hash);
#if !defined(_M_IX86)
        Check(g_fakeArmed[index] == 1, "the first handler sets the point's byte in the client");
#endif
        const uint32_t outer = Attach(index, WXL_POINT_REPLACE, &PointReplaceCalls, const_cast<char*>("1"), -5);
        const uint32_t inner = Attach(index, WXL_POINT_REPLACE, &PointReplaceCalls, const_cast<char*>("2"), 5);
        const uint32_t late  = Attach(index, WXL_POINT_AFTER, &PointAfter, const_cast<char*>("a"), 5);
        const uint32_t early = Attach(index, WXL_POINT_AFTER, &PointAfter, const_cast<char*>("z"), -5);
        Check(before && outer && inner && late && early, "Before, two Replaces and two Afters attach");

        g_pointTrace.clear();
        g_originalRuns = 0;
        int32_t result = EnterCamera(3, 4);
        Check(g_pointTrace == "B12Oaz", "Before, then the Replaces outermost first, the original, then the Afters "
                                        "with the lower priority last (nearest the caller)");
        Check(result == ((100 + 4) + 1 + 1) * 4, "Before rewrote an argument, each Replace and each After the answer");

        api.detach(inner);
        const uint32_t answers = Attach(index, WXL_POINT_REPLACE, &PointReplaceAnswers, nullptr, 10);
        g_pointTrace.clear();
        g_originalRuns = 0;
        result = EnterCamera(3, 4);
        Check(g_pointTrace == "B1Raz" && g_originalRuns == 0 && result == (7 + 1) * 4,
              "a Replace that answers without calling the original takes the call: the function does not run");
        api.detach(answers);
        api.detach(outer);
        api.detach(late);
        api.detach(early);

        // A faulting Replace before the original: the call goes on with the original, the handler stops.
        const uint32_t faulty = Attach(index, WXL_POINT_REPLACE, &PointFaultsFirst, nullptr);
        g_pointTrace.clear();
        g_originalRuns = 0;
        result = EnterCamera(3, 4);
        Check(g_pointTrace == "BFO" && result == 104, "a Replace that faults before the original: the original runs and answers");
        g_pointTrace.clear();
        EnterCamera(3, 4);
        Check(g_pointTrace == "BO", "and the faulting handler is not called again");
        api.detach(faulty);

        const uint32_t depth = Attach(index, WXL_POINT_BEFORE, &PointDepth, nullptr);
        g_seenDepth = 0;
        EnterCamera(1, 1);
        Check(g_seenDepth == 2, "a call of the point inside its own handler is depth 2");
        api.detach(depth);

        const uint32_t misplaced = Attach(index, WXL_POINT_BEFORE, &PointCallsOriginalFromBefore, nullptr);
        g_originalRuns = 0;
        EnterCamera(1, 1);
        Check(g_originalRuns == 1, "call_original outside a Replace does nothing: the original runs once");
        api.detach(misplaced);

        Check(api.detach(before) == WXL_OK && api.detach(before) == WXL_E_NOT_FOUND, "detach, then a second detach is not_found");
#if !defined(_M_IX86)
        Check(g_fakeArmed[index] == 0, "the last handler's detach clears the point's byte");
#endif

        // A mod's handlers go with it.
        const uint32_t saved = g_host->SwapCurrentMod(42);
        Attach(index, WXL_POINT_BEFORE, &PointBefore, nullptr);
        Attach(index, WXL_POINT_AFTER, &PointAfter, const_cast<char*>("a"));
        g_host->SwapCurrentMod(saved);
        pts::ReleaseOf(42);
        g_pointTrace.clear();
        EnterCamera(3, 4);
        Check(g_pointTrace == "O", "ReleaseOf takes a mod's handlers away");
    }
}

#if defined(_M_IX86)
namespace
{
    // Stand-ins of the points' shapes in this image, which the checks place the points on.
    volatile int32_t g_sink = 0;

    __declspec(noinline) int32_t __cdecl StandInCamera(void* data, void* param)
    {
        g_sink = g_sink + 1;
        return static_cast<int32_t>(reinterpret_cast<uintptr_t>(data) + reinterpret_cast<uintptr_t>(param));
    }

    /// An inlined copy of the camera's function, as the 2008 compiler leaves one in a caller.
    __declspec(noinline) int32_t __cdecl InlinedCamera(void* data, void* param)
    {
        g_sink = g_sink + 2;
        return static_cast<int32_t>(reinterpret_cast<uintptr_t>(data) * 1000 + reinterpret_cast<uintptr_t>(param));
    }

    __declspec(noinline) void* __fastcall StandInShared(void* self, void*, void* name, uint32_t flags)
    {
        g_sink = g_sink + 3;
        return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(self) + reinterpret_cast<uintptr_t>(name) + flags);
    }

    std::string g_worldTrace;
    __declspec(noinline) void __cdecl StandInWorld(void* param)
    {
        g_sink = g_sink + 4;
        g_worldTrace += param ? "W" : "w";
    }

    __declspec(noinline) void __cdecl StandInSignal(uint32_t index, void* format, void* args)
    {
        g_sink = g_sink + static_cast<int32_t>(index) + (format ? 1 : 0) + (args ? 1 : 0);
    }

    __declspec(noinline) int32_t __fastcall StandInProcess(void* self, void*, uint32_t time, void* msg, int32_t unused)
    {
        g_sink = g_sink + 5;
        return static_cast<int32_t>(reinterpret_cast<uintptr_t>(self) + time * 10 + reinterpret_cast<uintptr_t>(msg) + unused);
    }

    template <class Fn>
    uintptr_t Address(Fn fn)
    {
        return reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(fn));
    }

    uint32_t g_espIn = 0, g_espOut = 0;
    int32_t  g_pointAnswered = 0;

    /// Calls NetClient::ProcessMessage as the client does at 0x006323F7: `this` in ECX, three
    /// dwords pushed, the callee pops them; ESP recorded on both sides.
    __declspec(noinline) void CallLikeHandleData(void* fn, void* self, uint32_t time, void* msg, int32_t unused)
    {
        __asm {
            mov  dword ptr [g_espIn], esp
            push unused
            push msg
            push time
            mov  ecx, self
            call fn
            mov  dword ptr [g_pointAnswered], eax
            mov  dword ptr [g_espOut], esp
        }
    }

    std::string g_seamTrace;
    using WorldFn = void(__cdecl*)(void*);
    WorldFn g_seamOriginal = nullptr;
    void __cdecl FakeSeamDetour(void* param)
    {
        g_seamTrace += "S";
        g_seamOriginal(param);
    }

    uint32_t g_inlinedFlags = 0;
    void WXL_CALL PointSeesInlined(wxl_point_call* call, void*)
    {
        g_inlinedFlags = call->flags;
        *static_cast<void**>(call->args[0]) = reinterpret_cast<void*>(uintptr_t{9});
    }

    void WXL_CALL PointWorld(wxl_point_call*, void*) { g_seamTrace += "P"; }

    void WXL_CALL PointProcess(wxl_point_call* call, void*)
    {
        *static_cast<uint32_t*>(call->args[1]) = 2; // time = 2
        PointApi().call_original(call);
        *static_cast<int32_t*>(call->result) += 1000;
    }

    int g_sides[3] = {};
    void WXL_CALL CountSide(wxl_point_call* call, void*) { ++g_sides[call->side]; }
    void WXL_CALL ReplaceThrough(wxl_point_call* call, void*)
    {
        ++g_sides[WXL_POINT_REPLACE];
        PointApi().call_original(call);
    }

    /// Before, Replace and After on the two showcase shapes the checks above leave out: a
    /// thiscall that answers a pointer (CM2Cache::CreateShared) and a cdecl of three that answers
    /// nothing (vFrameScript_SignalEvent).
    void CheckPointSides()
    {
        const wxl_point_api& api = PointApi();
        for (uint32_t index : {static_cast<uint32_t>(pts::kCM2Cache_CreateShared), static_cast<uint32_t>(pts::kvFrameScript_SignalEvent)})
        {
            const uint32_t a = Attach(index, WXL_POINT_BEFORE, &CountSide, nullptr);
            const uint32_t b = Attach(index, WXL_POINT_REPLACE, &ReplaceThrough, nullptr);
            const uint32_t c = Attach(index, WXL_POINT_AFTER, &CountSide, nullptr);
            g_sides[0] = g_sides[1] = g_sides[2] = 0;
            const int32_t sinkBefore = g_sink;
            if (index == pts::kCM2Cache_CreateShared)
            {
                using SharedFn = void*(__thiscall*)(void*, void*, uint32_t);
                const SharedFn shared = Opaque(reinterpret_cast<SharedFn>(&StandInShared));
                void* const    answer = shared(reinterpret_cast<void*>(uintptr_t{0x100}), reinterpret_cast<void*>(uintptr_t{0x20}), 3);
                Check(a && b && c && answer == reinterpret_cast<void*>(uintptr_t{0x123}) && g_sink == sinkBefore + 3,
                      "x86: CM2Cache::CreateShared's thiscall detour answers the original's pointer through the chain");
            }
            else
            {
                using SignalFn = void(__cdecl*)(uint32_t, void*, void*);
                Opaque(reinterpret_cast<SignalFn>(&StandInSignal))(4, const_cast<int32_t*>(&g_sink), nullptr);
                Check(a && b && c && g_sink == sinkBefore + 5, "x86: vFrameScript_SignalEvent's detour runs the original once");
            }
            Check(g_sides[0] == 1 && g_sides[1] == 1 && g_sides[2] == 1, "x86: and its Before, Replace and After ran once each");
            api.detach(a);
            api.detach(b);
            api.detach(c);
        }
    }

    void CheckPointPrologues()
    {
        char why[256];
        const uint8_t jumped[16] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        Check(!wxl::hook::safety::CheckPrologue(jumped, 8, why, sizeof why) && strstr(why, "already a jump"),
              "a function whose first instruction is a jump is refused: another tool detoured it");
        printf("     %s\n", why);
        const uint8_t tiny[16] = {0x33, 0xC0, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
        Check(!wxl::hook::safety::CheckPrologue(tiny, 3, why, sizeof why) && strstr(why, "ends after 3"),
              "a function that returns within five bytes is refused");
        printf("     %s\n", why);
        // push ebp; mov ebp, esp; nop; nop; nop; jmp short -7 (lands at +1)
        const uint8_t looping[16] = {0x55, 0x89, 0xE5, 0x90, 0x90, 0x90, 0xEB, 0xF9, 0xC3};
        Check(!wxl::hook::safety::CheckPrologue(looping, 9, why, sizeof why) && strstr(why, "lands at +1"),
              "a function with a branch into its first five bytes is refused");
        printf("     %s\n", why);
        Check(wxl::hook::safety::CheckPrologue(reinterpret_cast<const void*>(&StandInCamera), 0, why, sizeof why),
              "a compiled function's prologue passes");
    }

    // --- De-inlining: a parent's recompiled body, in which an inlined function is real --------

    /// The parent as Wow.exe has it: the child's work copied into it, no call to see.
    __declspec(noinline) int32_t __fastcall StandInFinish(void* self, void*)
    {
        g_sink = g_sink + 6;
        return static_cast<int32_t>(reinterpret_cast<uintptr_t>(self) & 0xFF) + 5;
    }

    struct StandInModel
    {
        uint32_t flags;
        uint32_t size;
        uint8_t  bytes[16];
    };

    int g_childBodies = 0;

    /// The child as the fork writes it, and as extract.py --runtime writes its head.
    __declspec(noinline) int32_t __cdecl TestChildBody(uint8_t* base, uint32_t size, StandInModel& data)
    {
        ++g_childBodies;
        data.flags |= 0x100;
        return base == data.bytes && size == data.size ? 1 : 0;
    }

    __declspec(noinline) int32_t __cdecl TestChild(uint8_t* base, uint32_t size, StandInModel& data)
    {
        if (!wxl::core::deinlined::Armed(pts::kM2Init_uint8_t_uint32_t_M2Data)) return TestChildBody(base, size, data);
        return wxl::core::deinlined::Enter<pts::kM2Init_uint8_t_uint32_t_M2Data>(&TestChildBody, base, size, &data);
    }

    /// The parent recompiled: the same work, the child called.
    __declspec(noinline) int32_t __fastcall TestFinishBody(void* self, void*)
    {
        StandInModel* model = static_cast<StandInModel*>(self);
        g_sink           = g_sink + 60;
        return TestChild(model->bytes, model->size, *model) ? 1000 + static_cast<int32_t>(model->size) : 0;
    }

    /// The second parent as Wow.exe has it (a low-priority sequence read): the child copied in too.
    __declspec(noinline) int32_t __fastcall StandInSequence(void* self, void*, uint32_t index, uint8_t*, uint32_t)
    {
        g_sink = g_sink + 7;
        return static_cast<int32_t>(reinterpret_cast<uintptr_t>(self) & 0xFF) + static_cast<int32_t>(index);
    }

    /// The second parent recompiled: the child called, and the sequence's own work after it.
    __declspec(noinline) int32_t __fastcall TestSequenceBody(void* self, void*, uint32_t index, uint8_t*, uint32_t)
    {
        StandInModel* model = static_cast<StandInModel*>(self);
        g_sink           = g_sink + 70;
        if (!TestChild(model->bytes, model->size, *model)) return 0;
        model->flags |= 0x200;
        return 2000 + static_cast<int32_t>(index);
    }

    /// A function too short to detour: where the second parent is put to make its body refused.
    const uint8_t kTinyParent[16] = {0x33, 0xC0, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

    int g_childSeen = 0;
    uint32_t g_childSize = 0;
    void WXL_CALL ChildBefore(wxl_point_call* call, void*)
    {
        ++g_childSeen;
        g_childSize = *static_cast<uint32_t*>(call->args[1]);
        StandInModel* model = *static_cast<StandInModel**>(call->args[2]);
        model->flags |= 0x1; // a write through the reference reaches the model
    }

    void WXL_CALL ChildReplace(wxl_point_call* call, void*)
    {
        PointApi().call_original(call);
        *static_cast<int32_t*>(call->result) = 0; // refuse the file
    }

    int g_parentSeen = 0;
    void WXL_CALL ParentBefore(wxl_point_call*, void*) { ++g_parentSeen; }

    void CheckDeinlined()
    {
        namespace dei               = wxl::core::deinlined;
        const wxl_point_api& api    = PointApi();
        const uint32_t       child  = pts::kM2Init_uint8_t_uint32_t_M2Data;
        const uint32_t       parent = pts::kCM2Shared_FinishLoading;
        const uint32_t       body   = dei::kParent_CM2Shared_FinishLoading;
        const uint32_t       second = dei::kParent_CM2Shared_FinishLoadingLowPrioritySequence_uint32_t_uint8_t_uint32_t;
        using FinishFn              = int32_t(__thiscall*)(void*);
        using SequenceFn            = int32_t(__thiscall*)(void*, uint32_t, uint8_t*, uint32_t);
        const FinishFn   finish     = Opaque(reinterpret_cast<FinishFn>(&StandInFinish));
        const SequenceFn sequence   = Opaque(reinterpret_cast<SequenceFn>(&StandInSequence));
        StandInModel      model{0, 9, {}};
        uint8_t           buffer[8] = {};
        const int32_t     self      = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&model) & 0xFF);

        g_childBodies = g_childSeen = g_parentSeen = 0;
        Check(finish(&model) == self + 5 && sequence(&model, 3, buffer, sizeof buffer) == self + 3 &&
                  !pts::x86::BodyInPlace(body) && !pts::x86::BodyInPlace(second) && g_childBodies == 0,
              "x86: with no handler on the de-inlined point, both parents run Wow.exe's own bodies");

        // Both parents or none: one that cannot be detoured gives the other back.
        pts::x86::PlaceParent(second, reinterpret_cast<uintptr_t>(kTinyParent), 3);
        const uint32_t refused = Attach(child, WXL_POINT_BEFORE, &ChildBefore, nullptr);
        Check(!refused && !pts::x86::BodyInPlace(body) && !wxl::hook::Armed(reinterpret_cast<void*>(&StandInFinish)),
              "x86: when one parent's body cannot be put in place, the point is not attached and the other "
              "parent is given back");
        pts::x86::PlaceParent(second, Address(&StandInSequence), 0);

        const uint32_t before = Attach(child, WXL_POINT_BEFORE, &ChildBefore, nullptr);
        Check(before && pts::x86::BodyInPlace(body) && pts::x86::BodyInPlace(second),
              "x86: the de-inlined point's first handler puts the recompiled body of each of its parents in place");
        Check(finish(&model) == 1009 && g_childBodies == 1 && g_childSeen == 1 && g_childSize == 9 &&
                  model.flags == 0x101,
              "x86: the parent now runs its recompiled body, in which the child is a call: its Before sees the "
              "arguments, writes through the reference, and the child's body runs");
        model.flags = 0x100;
        Check(sequence(&model, 3, buffer, sizeof buffer) == 2003 && g_childBodies == 2 && g_childSeen == 2 &&
                  model.flags == 0x301,
              "x86: the second parent, a function that is no point, runs its recompiled body too: the child fires "
              "from it as well, with the stack as its caller left it");
        g_childBodies = 1;
        g_childSeen   = 1;
        model.flags   = 0x101;

        const uint32_t onParent = Attach(parent, WXL_POINT_BEFORE, &ParentBefore, nullptr);
        Check(onParent && finish(&model) == 1009 && g_parentSeen == 1 && g_childSeen == 2,
              "x86: the parent's own point still runs in front of the recompiled body");

        const uint32_t replace = Attach(child, WXL_POINT_REPLACE, &ChildReplace, nullptr);
        Check(replace && finish(&model) == 0 && g_childBodies == 3,
              "x86: a Replace on the de-inlined point changes what the parent sees (the file refused)");
        api.detach(replace);

        api.detach(before);
        Check(!pts::x86::BodyInPlace(body) && !pts::x86::BodyInPlace(second),
              "x86: its last handler gone, both recompiled bodies leave");
        g_childBodies = 0;
        Check(finish(&model) == self + 5 && sequence(&model, 3, buffer, sizeof buffer) == self + 3 &&
                  g_childBodies == 0 && g_parentSeen == 3,
              "x86: both parents run Wow.exe's bodies again, the first one's own point still attached");
        api.detach(onParent);
        Check(!wxl::hook::Armed(reinterpret_cast<void*>(&StandInFinish)) &&
                  !wxl::hook::Armed(reinterpret_cast<void*>(&StandInSequence)),
              "x86: and with nothing left neither function is detoured");
    }

    void CheckPointDetours()
    {
        const wxl_point_api& api = PointApi();
        using CameraFn           = int32_t(__cdecl*)(void*, void*);
        const CameraFn camera    = Opaque(&StandInCamera);
        const CameraFn inlined   = Opaque(&InlinedCamera);
        uint8_t        bytes[8];
        memcpy(bytes, reinterpret_cast<const void*>(&StandInCamera), sizeof bytes);

        Check(camera(reinterpret_cast<void*>(3), reinterpret_cast<void*>(4)) == 7 && !wxl::hook::Armed(reinterpret_cast<void*>(&StandInCamera)),
              "x86: an unattached point is not detoured: the function runs as it is");

        g_pointTrace.clear();
        const uint32_t before = Attach(pts::kCGCamera_UpdateCallback, WXL_POINT_BEFORE, &PointBefore, nullptr);
        const uint32_t seen   = Attach(pts::kCGCamera_UpdateCallback, WXL_POINT_BEFORE, &PointSeesInlined, nullptr, 10);
        Check(before && seen && reinterpret_cast<const uint8_t*>(&StandInCamera)[0] == 0xE9,
              "x86: the first handler arms the detour: SafetyHook's jump at the function's head");
        g_pointTrace.clear();
        Check(camera(reinterpret_cast<void*>(3), reinterpret_cast<void*>(4)) == 9 + 4 && g_pointTrace == "B",
              "x86: the detour runs the Before handlers, which rewrite what the function is given");
        g_inlinedFlags = 0;
        Check(inlined(reinterpret_cast<void*>(3), reinterpret_cast<void*>(4)) == 9 * 1000 + 4 &&
                  (g_inlinedFlags & WXL_POINT_CALL_INLINED),
              "x86: a site (an inlined copy) runs the Before handlers, flagged inlined, and their rewrites reach the copy");

        api.detach(before);
        api.detach(seen);
        Check(memcmp(bytes, reinterpret_cast<const void*>(&StandInCamera), sizeof bytes) == 0 &&
                  !wxl::hook::Armed(reinterpret_cast<void*>(&StandInCamera)),
              "x86: the last detach disarms it: the function's own bytes are back");
        g_pointTrace.clear();
        Check(camera(reinterpret_cast<void*>(3), reinterpret_cast<void*>(4)) == 7 && g_pointTrace.empty() &&
                  inlined(reinterpret_cast<void*>(3), reinterpret_cast<void*>(4)) == 3004,
              "x86: and nothing runs any more, at the function or at its site");

        // thiscall, called as the client calls it.
        const uint32_t process = Attach(pts::kNetClient_ProcessMessage, WXL_POINT_REPLACE, &PointProcess, nullptr);
        void* const    self    = reinterpret_cast<void*>(uintptr_t{0x1000});
        CallLikeHandleData(reinterpret_cast<void*>(&StandInProcess), self, 5, reinterpret_cast<void*>(uintptr_t{0x20}), 3);
        Check(process && g_pointAnswered == 0x1000 + 2 * 10 + 0x20 + 3 + 1000 && g_espIn == g_espOut,
              "x86: a thiscall point called as the client calls it: `this` in ECX, the Replace rewrote an argument "
              "and the answer, and the stack is as the caller left it");
        api.detach(process);
        CallLikeHandleData(reinterpret_cast<void*>(&StandInProcess), self, 5, reinterpret_cast<void*>(uintptr_t{0x20}), 3);
        Check(g_pointAnswered == 0x1000 + 50 + 0x20 + 3 && g_espIn == g_espOut, "x86: and as itself once detached");

        // A point on a function a seam already detours: one chain, the point in front.
        const wxl::hook::Id seam = wxl::hook::Attach("Harness.World", Address(&StandInWorld), &FakeSeamDetour,
                                                     &g_seamOriginal);
        const WorldFn world = Opaque(&StandInWorld);
        const uint32_t point = Attach(pts::kCGWorldFrame_RenderWorld, WXL_POINT_BEFORE, &PointWorld, nullptr);
        g_seamTrace.clear();
        g_worldTrace.clear();
        world(reinterpret_cast<void*>(1));
        Check(seam && point && g_seamTrace == "PS" && g_worldTrace == "W",
              "x86: a point joins a seam's chain on its function, in front of it, and the function still runs once");
        api.detach(point);
        g_seamTrace.clear();
        world(reinterpret_cast<void*>(1));
        Check(g_seamTrace == "S" && wxl::hook::Armed(reinterpret_cast<void*>(&StandInWorld)),
              "x86: detaching the point leaves the seam's chain as it was");
        wxl::hook::Detach(seam);
    }
}
#endif

#if defined(_M_IX86)
namespace
{
    void PlaceStandIns()
    {
        pts::x86::Place(pts::kCGCamera_UpdateCallback, Address(&StandInCamera), 0);
        pts::x86::Place(pts::kCM2Cache_CreateShared, Address(&StandInShared), 0);
        pts::x86::Place(pts::kCGWorldFrame_RenderWorld, Address(&StandInWorld), 0);
        pts::x86::Place(pts::kvFrameScript_SignalEvent, Address(&StandInSignal), 0);
        pts::x86::Place(pts::kNetClient_ProcessMessage, Address(&StandInProcess), 0);
        pts::x86::AddSite(pts::kCGCamera_UpdateCallback, Address(&InlinedCamera), "[esp+4],[esp+8]");
        pts::x86::Place(pts::kCM2Shared_FinishLoading, Address(&StandInFinish), 0);
        namespace dei = wxl::core::deinlined;
        pts::x86::PlaceBody(dei::kParent_CM2Shared_FinishLoading, reinterpret_cast<void*>(&TestFinishBody));
        pts::x86::PlaceParent(dei::kParent_CM2Shared_FinishLoadingLowPrioritySequence_uint32_t_uint8_t_uint32_t,
                              Address(&StandInSequence), 0);
        pts::x86::PlaceBody(dei::kParent_CM2Shared_FinishLoadingLowPrioritySequence_uint32_t_uint8_t_uint32_t,
                            reinterpret_cast<void*>(&TestSequenceBody));
    }
}
#endif

// --- A measurement, not a check ---------------------------------------------------------------
// Two paths carry work at a rate nothing else here does: the file-open gate, which the client
// reaches once per file it opens, and the event bus, which every seam goes through. These are
// not frame times -- there is no client and no archive behind them -- but they are the real code
// on the real path, so a change to either shows up here before anyone notices it in game.

void WXL_CALL BenchHandler(void*, const void*) {}

/// Storm's error display told to the log: a recoverable error in one line, a fatal one as a
/// report with the session's end, once. Before Client.Destroy, which would end the session first.
void CheckClientError()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path path =
        std::filesystem::path(temp) / ("wxl-harness-fatal-" + std::to_string(GetCurrentProcessId()) + ".log");
    wxl::log::Open(path.c_str());
    char message[64];
    snprintf(message, sizeof message, "Invalid function pointer: %p", reinterpret_cast<void*>(&CodeProbe));
    wxl::core::crash::ClientError(0x8510007F, "leaked handle", -3, "1 handle not released", true);
    wxl::core::crash::ClientError(0x85100086, nullptr, 0, message, false);
    wxl::core::crash::ClientError(0x85100084, "0xC0000005 (ACCESS_VIOLATION)", -5, "second", false);
    wxl::log::Close();

    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    printf("     the log it wrote:\n");
    for (size_t at = 0; at < text.size();)
    {
        const size_t eol = text.find('\n', at);
        printf("     | %s\n", text.substr(at, eol == std::string::npos ? std::string::npos : eol - at).c_str());
        at = eol == std::string::npos ? text.size() : eol + 1;
    }
    Check(text.find("[client] the client shows error #127 (0x8510007F) (leaked handle) and goes on: 1 handle not released") !=
              std::string::npos,
          "a recoverable error is one warning");
    Check(text.find("[fatal] fatal error #134 (0x85100086) shown by the client") != std::string::npos &&
              text.find(message) != std::string::npos,
          "a fatal one is reported with its number, code and message");
    Check(text.find("the client was handed a function at wxl-core-seam-harness.exe+") != std::string::npos,
          "an invalid function pointer is named by module and offset");
    Check(text.find("[fatal] #0 ") != std::string::npos && text.find("the client writes Errors\\ and ends") != std::string::npos,
          "with a stack, then what the client does next");
    Check(text.find("session end: fatal error (#134 0x85100086: Invalid function pointer: ") != std::string::npos,
          "the session ends on it");
    Check(text.find("second") == std::string::npos, "a second fatal error writes nothing: one report");
}

/// [engine] animate_threads: a scene's models animated as the client does it (its main thread
/// every other model, its one worker the others, woken by an event and waited for by another)
/// and by the gates on 2, 4 and 8 threads; each model's work a chain of 4x4 products per bone.
void BenchAnimate()
{
    using Clock = std::chrono::steady_clock;
    namespace animate = wxl::core::tuning::animate;
    printf("\nanimate_threads (%s; main-thread ms per frame, median of 200):\n", sizeof(void*) == 8 ? "x64" : "x86");

    struct Stock
    {
        HANDLE         start = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE         done  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        FakeAnimScene* scene = nullptr;
        static unsigned long __stdcall Main(void* p)
        {
            auto& self = *static_cast<Stock*>(p);
            for (;;)
            {
                WaitForSingleObject(self.start, INFINITE);
                // CM2Scene::AnimateThread: the second, fourth... model.
                for (FakeAnimModel* m = self.scene->head ? self.scene->head->next : nullptr; m; m = m->next ? m->next->next : nullptr)
                    if (!m->parent) FakeAnimateModel(static_cast<FakeM2Model*>(m), self.scene);
                SetEvent(self.done);
            }
        }
    };
    static Stock stock;
    CloseHandle(CreateThread(nullptr, 0, &Stock::Main, &stock, 0, nullptr));

    int   cacheObject = 0;
    void* cache       = &cacheObject;
    auto  median      = [](std::vector<double>& v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    for (const auto& load : {std::pair<uint32_t, uint32_t>{0, 400}, {40, 150}, {80, 300}, {80, 600}, {400, 300}})
    {
        const uint32_t bones = load.first, models = load.second;
        AnimScene      a(models, 6, bones);
        std::vector<double> single, client, gated[3];
        for (int frame = 0; frame < 220; ++frame)
        {
            Clock::time_point t0 = Clock::now();
            for (FakeAnimModel& m : a.models)
                if (!m.parent) FakeAnimateModel(static_cast<FakeM2Model*>(&m), &a.scene);
            if (frame >= 20) single.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
            t0          = Clock::now();
            stock.scene = &a.scene;
            SetEvent(stock.start);
            for (FakeAnimModel* m = a.scene.head; m; m = m->next ? m->next->next : nullptr)
                if (!m->parent) FakeAnimateModel(static_cast<FakeM2Model*>(m), &a.scene);
            WaitForSingleObject(stock.done, INFINITE);
            if (frame >= 20) client.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
            const uint32_t counts[3] = {2, 4, 8};
            for (int k = 0; k < 3; ++k)
            {
                animate::SetThreadsForTests(counts[k]);
                t0 = Clock::now();
                a.Frame(cache);
                if (frame >= 20) gated[k].push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
            }
        }
        printf("  %3u models, %2u bones: one thread %.3f ms, the client's two %.3f; the gates on 2 threads %.3f, 4 %.3f, 8 %.3f\n", models,
               bones, median(single), median(client), median(gated[0]), median(gated[1]), median(gated[2]));
    }
    animate::SetThreadsForTests(0);
}

/// [engine] trim_alloc: SMemAlloc and SMemFree's heap work, a block allocated and one freed, with
/// the CRT heap's HeapSize after each (as Wow.exe) and without (the call taken out).
void BenchTrimAlloc()
{
    using Clock = std::chrono::steady_clock;
    HANDLE   heap = HeapCreate(0, 0x1000, 0); // _heap_init's
    void*    live[4096] = {};
    uint32_t seed = 1;
    volatile SIZE_T sink = 0;
    printf("\ntrim_alloc (%s; ns per block allocated and freed, 2,000,000 of 8 to 520 bytes):\n", sizeof(void*) == 8 ? "x64" : "x86");
    for (int with = 1; with >= 0; --with)
    {
        const Clock::time_point t0 = Clock::now();
        const int               n  = 2000000;
        for (int i = 0; i < n; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const uint32_t slot = (seed >> 8) & 4095;
            const SIZE_T   size = ((((seed >> 20) & 0x1FF) + 8) + 7) & ~SIZE_T{7};
            if (live[slot])
            {
                if (with) sink = HeapSize(heap, 0, live[slot]);
                HeapFree(heap, 0, live[slot]);
            }
            live[slot] = HeapAlloc(heap, 0, size);
            if (with) sink = HeapSize(heap, 0, live[slot]);
        }
        printf("  %-46s %8.1f ns\n", with ? "with the size query (stock)" : "without it (trim_alloc)",
               std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / n);
    }
    (void)sink;
    HeapDestroy(heap);
}

void Bench()
{
    using Clock = std::chrono::steady_clock;

    const char* texture = "World\\Expansion01\\Doodads\\Generic\\BloodElf\\Banner01.blp";
    const char* table   = "DBFilesClient\\Spell.dbc";
    char        out[512];

    auto time = [](const char* what, int runs, auto&& body) {
        body();                                 // once, so nothing measured is a cold page
        const Clock::time_point start = Clock::now();
        for (int i = 0; i < runs; ++i) body();
        const double ns =
            std::chrono::duration<double, std::nano>(Clock::now() - start).count() / runs;
        printf("  %-46s %8.1f ns/call\n", what, ns);
    };

    printf("\nbench (%s):\n", sizeof(void*) == 8 ? "x64" : "x86");

    {
        const void* client = reinterpret_cast<const void*>(uintptr_t{0x00856370});
        const void* own    = reinterpret_cast<const void*>(&CodeProbe);
        time("function pointer gate, the client's own", 2000000, [&] {
            const void* p = client;
            wxl::seam::Os::ValidateFunctionPointer::Gate(&p);
        });
        time("function pointer gate, the runtime's", 2000000, [&] {
            const void* p = own;
            wxl::seam::Os::ValidateFunctionPointer::Gate(&p);
        });
    }

    wxl::core::services::data::Clear();
    time("file-open gate, a path that is not a database", 200000, [&] {
        wxl::core::services::data::ServedPath(texture, strlen(texture), out, sizeof out);
    });
    time("file-open gate, a database nobody serves", 200000, [&] {
        wxl::core::services::data::ServedPath(table, strlen(table), out, sizeof out);
    });

    // A bus of its own: the harness's bus carries the recording handlers of the checks.
    wxl::host::EventBus bus({WXL_EV_Update}, nullptr);
    wxl_update_args     args{};
    args.delta = 0.016f;
    time("event bus, no subscriber", 200000, [&] { bus.Emit(WXL_EV_Update, &args); });

    bus.Subscribe(WXL_EV_Update, &BenchHandler, nullptr, 0);
    time("event bus, one subscriber", 200000, [&] { bus.Emit(WXL_EV_Update, &args); });
    bus.Subscribe(WXL_EV_Update, &BenchHandler, nullptr, 0);
    time("event bus, two subscribers", 200000, [&] { bus.Emit(WXL_EV_Update, &args); });

    {
        // The lookups behind every object event (the runtime's handle table: an object to its
        // handle, a guid to its handle) and behind a mod's per-object data (ObjectMap's table),
        // with as many objects as a busy city shows.
        constexpr int        kObjects = 2000;
        std::vector<uint8_t> objects(kObjects * 16);
        wxl::runtime::HandleTable handles;
        std::vector<wxl_handle>   minted;
        for (int i = 0; i < kObjects; ++i)
            minted.push_back(handles.Track(&objects[i * 16], 0x0F00000000000000ull + i * 7919u, 1));
        int at = 0;
        time("handle table, object to handle", 2000000, [&] {
            at = (at + 613) % kObjects;
            volatile wxl_handle h = handles.HandleOf(&objects[at * 16]);
            (void)h;
        });
        time("handle table, guid to handle", 2000000, [&] {
            at = (at + 613) % kObjects;
            volatile wxl_handle h = handles.FindByKey(0x0F00000000000000ull + at * 7919u);
            (void)h;
        });
        time("handle table, track and untrack", 200000, [&] {
            at = (at + 613) % kObjects;
            handles.Untrack(&objects[at * 16]);
            handles.Track(&objects[at * 16], 0x0F00000000000000ull + at * 7919u, 1);
        });

        wxl::ObjectMap<uint32_t>::map_type data;
        for (wxl_handle h : minted) data[wxl::Object(h)] = 1;
        time("ObjectMap table, find", 2000000, [&] {
            at = (at + 613) % kObjects;
            auto found = data.find(wxl::Object(minted[at]));
            volatile uint32_t v = found == data.end() ? 0 : found->second;
            (void)v;
        });
        time("ObjectMap table, erase and insert", 200000, [&] {
            at = (at + 613) % kObjects;
            data.erase(wxl::Object(minted[at]));
            data[wxl::Object(minted[at])] = 2;
        });
    }

#if defined(WXL_CORE_PROFILE) && WXL_CORE_PROFILE
    {
        // The profiler up and no viewer, as in a game nobody profiles: the host's zones behind
        // their flag, which a present keeps off; then the flag forced on, every zone asked of a
        // profiler that answers 0 -- the frame a viewer connects or leaves; then a mod's zone
        // through wxl.profile and one of the client's own.
        namespace profile = wxl::core::profile;
        wxl::host::Profiler hooks;
        hooks.location = [](void*, std::string_view name, std::string_view function, uint32_t color) {
            return profile::Location(name, function, "", 0, color);
        };
        hooks.begin = [](void*, const void* location) { return profile::Begin(location); };
        hooks.end   = [](void*, uint64_t zone) { profile::End(zone); };
        wxl::host::EventBus traced({WXL_EV_Update}, nullptr);
        traced.SetProfiler(&hooks, [](void*, uint32_t) { return std::string("bench"); }, nullptr);
        traced.Subscribe(WXL_EV_Update, &BenchHandler, nullptr, 0, 1);
        time("event bus, one subscriber, profiler set", 200000, [&] { traced.Emit(WXL_EV_Update, &args); });
        traced.Subscribe(WXL_EV_Update, &BenchHandler, nullptr, 0, 1);
        time("event bus, two subscribers, profiler set", 200000, [&] { traced.Emit(WXL_EV_Update, &args); });

        wxl::core::settings::ProfileChoice choice;
        choice.enabled = true;
        profile::Start(choice);
        for (int i = 0; i < 500 && !profile::Started(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        printf("  (profiler %s, no viewer)\n", profile::Started() ? "up" : "NOT up");
        time("event bus, two subscribers, profiler up", 200000, [&] { traced.Emit(WXL_EV_Update, &args); });
        time("event bus, two subscribers, no profiler", 200000, [&] { bus.Emit(WXL_EV_Update, &args); });
        traced.SetProfiling(true);
        time("event bus, two subscribers, zones asked", 200000, [&] { traced.Emit(WXL_EV_Update, &args); });

        const wxl_profile_api& api      = profile::Api();
        const uintptr_t        location = api.location(wxl_str{"Bench", 5}, wxl_str{"", 0}, wxl_str{"", 0}, 0, 0);
        time("wxl.profile zone_begin + zone_end, no viewer", 200000, [&] { api.zone_end(api.zone_begin(location)); });
        static const wxl_core_zone_site kSite{"Bench.Client", "Bench", __FILE__, __LINE__, 0};
        time("client zone (WarcraftXLZoneBegin/End), no viewer", 200000,
             [&] { WarcraftXLZoneEnd(WarcraftXLZoneBegin(&kSite)); });
        time("present (frame mark, flag, plots), no viewer", 200000, [&] { profile::Present(); });
    }
#endif

    {
        // A point: the dispatch with nothing attached (a call that reached it as one was
        // detached), then with one handler of each side. An unattached point never reaches it:
        // on x86 it is not detoured, on x64 the client tests its byte.
        namespace pts = wxl::core::points;
        void*       a = nullptr;
        void*       b = nullptr;
        void* const pargs[] = {&a, &b};
        int32_t     presult = 0;
        const auto  original = [](void* const*, void* r) { *static_cast<int32_t*>(r) = 1; };
        time("point, nothing attached", 2000000, [&] { pts::Enter(pts::kCGCamera_UpdateCallback, pargs, &presult, original); });
        const wxl_point_api& api = pts::Api();
        const auto nothing = [](wxl_point_call*, void*) {};
        const uint32_t before = api.attach(pts::kCGCamera_UpdateCallback + 1, WXL_POINT_BEFORE, nothing, nullptr, 0, 0);
        time("point, one Before", 2000000, [&] { pts::Enter(pts::kCGCamera_UpdateCallback, pargs, &presult, original); });
        const uint32_t replace = api.attach(pts::kCGCamera_UpdateCallback + 1, WXL_POINT_REPLACE,
                                            [](wxl_point_call* call, void*) { wxl::core::points::Api().call_original(call); },
                                            nullptr, 0, 0);
        time("point, one Before and a Replace calling the original", 2000000,
             [&] { pts::Enter(pts::kCGCamera_UpdateCallback, pargs, &presult, original); });
        const uint32_t after = api.attach(pts::kCGCamera_UpdateCallback + 1, WXL_POINT_AFTER, nothing, nullptr, 0, 0);
        time("point, Before, Replace and After", 2000000,
             [&] { pts::Enter(pts::kCGCamera_UpdateCallback, pargs, &presult, original); });
        api.detach(before);
        api.detach(replace);
        api.detach(after);
#if defined(_M_IX86)
        using CameraFn = int32_t(__cdecl*)(void*, void*);
        const CameraFn camera = Opaque(&StandInCamera);
        time("x86 point function, unattached (not detoured)", 2000000, [&] { camera(nullptr, nullptr); });
        const uint32_t armed = api.attach(pts::kCGCamera_UpdateCallback + 1, WXL_POINT_BEFORE, nothing, nullptr, 0, 0);
        time("x86 point function, detoured, one Before", 2000000, [&] { camera(nullptr, nullptr); });
        api.detach(armed);
#endif
    }

    wxl::core::services::data::Clear();

    // Storm's seams: an allocation and its free, the client's commonest pair of calls, with
    // accounting off and on, from one thread and from four at once.
    const wxl_footprint_api& footprint = wxl::core::services::FootprintApi();
    static const char        kFile[]   = ".\\Bench.cpp";
    constexpr int            kPairs    = 1000000;
    const auto pairs = [&](uintptr_t base, int count) {
        for (int i = 0; i < count; ++i)
        {
            void* const block = reinterpret_cast<void*>(base + static_cast<uintptr_t>(i) * 16);
            wxl::seam::Storm::SMemAlloc::After(block, 48, kFile, 1, 0);
            wxl::seam::Storm::SMemFree::Before(block, kFile, 2, 0);
        }
    };
    const auto rate = [&](const char* what, int threads) {
        const Clock::time_point start = Clock::now();
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; ++t)
            workers.emplace_back([&, t] { pairs(0x10000000u + static_cast<uintptr_t>(t) * 0x01000000u, kPairs / threads); });
        for (std::thread& worker : workers) worker.join();
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        printf("  %-46s %8.1f ns/pair, %6.2f M alloc+free/s\n", what, seconds * 1e9 / kPairs, kPairs / seconds / 1e6);
    };
    wxl::core::services::footprint::Reset();
    rate("storm seams, accounting off", 1);
    footprint.start();
    rate("storm seams, accounting on", 1);
    rate("storm seams, accounting on, 4 threads", 4);
    // A live set the size of a client's: 1,000,000 blocks kept while the pairs run.
    for (int i = 0; i < 1000000; ++i)
        wxl::seam::Storm::SMemAlloc::After(reinterpret_cast<void*>(0x60000000u + static_cast<uintptr_t>(i) * 32), 64,
                                           kFile, 3, 0);
    rate("storm seams, on, 1,000,000 blocks live", 1);
    footprint.snapshot();
    std::vector<wxl_footprint_category> categories(footprint.categories(nullptr, 0));
    for (wxl_footprint_category& c : categories) c.struct_size = sizeof c;
    footprint.categories(categories.data(), static_cast<uint32_t>(categories.size()));
    for (const wxl_footprint_category& c : categories)
        if (std::string_view(c.name.data, c.name.size) == "accounting")
            printf("  %-46s %8.1f MB for 1,000,000 live blocks\n", "accounting's own tables", c.bytes / 1048576.0);
    time("snapshot, 1,000,000 blocks live", 20, [&] { footprint.snapshot(); });
    wxl::core::services::footprint::Reset();

    BenchAnimate();
    BenchTrimAlloc();
}

namespace
{
    // --- M2.DrawBatch: the batch the device drew, as ModelBatchDrawn -------------------------------

    void CheckModelBatch()
    {
        int render = 0; // CM2SceneRender's `this`; the fake binding only tests it for null
        int model  = 0;
        int root   = 0;

        g_gxDraw                 = wxl_model_batch_args{};
        g_gxDraw.model           = &model;
        g_gxDraw.root_model      = &root;
        g_gxDraw.primitive_type  = 4;
        g_gxDraw.base_vertex     = 12;
        g_gxDraw.start_index     = 300;
        g_gxDraw.primitive_count = 40;
        g_gxDraw.viewport_max_z  = 1.0f;

        Mute(WXL_EV_ModelBatchDrawn);
        g_gxDrew = true;
        g_received.clear();
        wxl::seam::M2::DrawBatch::After(&render);
        Check(g_gxDrew && g_received.empty(), "with nobody listening the seam does not even ask the client for the batch");
        Unmute(WXL_EV_ModelBatchDrawn);

        wxl::seam::M2::DrawBatch::After(&render);
        const Received* drawn = Only(WXL_EV_ModelBatchDrawn);
        Check(drawn && drawn->As<wxl_model_batch_args>().model == &model &&
                  drawn->As<wxl_model_batch_args>().root_model == &root &&
                  drawn->As<wxl_model_batch_args>().base_vertex == 12 &&
                  drawn->As<wxl_model_batch_args>().start_index == 300 &&
                  drawn->As<wxl_model_batch_args>().primitive_count == 40 &&
                  drawn->As<wxl_model_batch_args>().viewport_max_z == 1.0f &&
                  drawn->As<wxl_model_batch_args>().batch == nullptr,
              "a drawn batch raises ModelBatchDrawn with the numbers of the device's draw, and no batch for wxl.render on D3D9");

        g_received.clear();
        wxl::seam::M2::DrawBatch::After(&render);
        Check(g_received.empty(), "a batch the device did not draw raises nothing");
        wxl::seam::M2::DrawBatch::After(nullptr);
        Check(g_received.empty(), "nor does one from no renderer");

        const wxl_gx_api& gx = wxl::core::services::GxApi();
        Check(gx.struct_size == sizeof(wxl_gx_api) && gx.d3d9_device() == nullptr,
              "wxl.gx answers no device where the client hands none over");
        gx.invalidate_state(); // nothing to invalidate: it must not fault
#if !defined(_M_IX86)
        Check(gx.api() == WXL_GX_BACKEND_D3D9, "wxl.gx 2 answers the API the client's device runs on: Direct3D 9 here");
#else
        Check(gx.api() <= WXL_GX_BACKEND_VULKAN, "wxl.gx 2 answers an API the contract names");
#endif
    }

    // --- wxl.render, wxl.vk and the points of the frame on a device the renderer does not drive -------

    void CheckRenderService()
    {
        const wxl_render_api& r = wxl::core::services::RenderApi();
        const wxl_vk_api&     v = wxl::core::services::VkApi();
        const wxl_str         why = r.unsupported_reason();
        Check(r.struct_size == sizeof(wxl_render_api) && r.supported() == WXL_FALSE &&
                  std::string_view(why.data, why.size).find("Direct3D 9") != std::string_view::npos,
              "wxl.render says it does not drive Direct3D 9, and why");
        wxl_render_pipeline_desc desc{};
        desc.struct_size = sizeof desc;
        wxl_render_pass pass{};
        pass.struct_size = sizeof pass;
        uint64_t address = 0;
        Check(r.create_image(8, 8, WXL_RENDER_FORMAT_RGBA8, 1) == WXL_HANDLE_NONE && r.create_screen_image(2, WXL_RENDER_FORMAT_RGBA8, 0) == WXL_HANDLE_NONE &&
                  r.create_buffer(64) == WXL_HANDLE_NONE && r.create_pipeline(&desc) == WXL_HANDLE_NONE &&
                  r.image_slot(16) == WXL_RENDER_NO_SLOT && r.buffer_address(16, &address) == WXL_E_UNSUPPORTED,
              "no resource is made on it");
        int batch = 0;
        Check(r.begin(&pass) == WXL_E_UNSUPPORTED && r.draw(16, 3, 1, nullptr, 0) == WXL_E_UNSUPPORTED &&
                  r.draw_batch(&batch, 16, nullptr, 0) == WXL_E_UNSUPPORTED && r.keep_batch(&batch) == nullptr &&
                  r.scene_color_slot() == WXL_RENDER_NO_SLOT && r.scene_depth_slot() == WXL_RENDER_NO_SLOT,
              "no pass, no draw, no batch kept, no copy of the scene");
        const uint32_t word = 0x07230203u;
        Check(r.replace_shader(wxl_str{"Diffuse", 7}, 1, &word, 4) == WXL_E_UNSUPPORTED &&
                  r.restore_shader(wxl_str{"Diffuse", 7}, 1) == WXL_E_UNSUPPORTED,
              "no shader of the client is replaced");
        desc.kind = WXL_RENDER_PIPELINE_COMPUTE;
        desc.compute = &word;
        desc.compute_size = 4;
        const float matrix[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        uint64_t    palette    = 1;
        Check(r.create_pipeline(&desc) == WXL_HANDLE_NONE && r.dispatch(16, 1, 1, 1, nullptr, 0) == WXL_E_UNSUPPORTED &&
                  r.bone_palette(matrix, sizeof matrix, &palette) == WXL_E_UNSUPPORTED && palette == 0,
              "no compute pipeline, no dispatch, no bone palette");
        r.end();
        r.destroy_image(16);
        uint64_t out = 0;
        Check(v.struct_size == sizeof(wxl_vk_api) && !v.instance() && !v.device() && !v.queue() && !v.command_buffer() &&
                  v.api_version() == 0 && v.frame(&out) == WXL_E_UNSUPPORTED && v.image(16, WXL_FALSE, &out) == WXL_E_UNSUPPORTED,
              "wxl.vk hands out nothing of a Vulkan device there is not");
#if defined(WXL_CORE_PROFILE) && WXL_CORE_PROFILE
        const wxl_profile_api& profile = wxl::core::profile::Api();
        Check(profile.gpu_zone_begin(wxl_str{"zone", 4}) == WXL_E_UNSUPPORTED, "a GPU zone is refused where wxl.render does not draw");
        profile.gpu_zone_end();
#endif

        // The points: inside a frame the client paints, each where its seam is, with the frame's index.
        g_gxContext = g_gxVisible = true;
        g_received.clear();
        wxl::seam::WorldScene::RenderDetailDoodads::After();
        wxl::seam::WorldScene::RenderLiquidPass::After();
        Check(g_received.empty(), "outside a frame no point is raised");
        wxl::seam::Scrn::OnPaint::Before(nullptr, nullptr);
        g_received.clear();
        wxl::seam::WorldScene::RenderDetailDoodads::After();
        wxl::seam::WorldScene::RenderLiquidPass::After();
        Check(g_received.size() == 2 && g_received[0].event == WXL_EV_RenderPoint &&
                  g_received[0].As<wxl_render_point_args>().point == WXL_RENDER_POINT_AFTER_OPAQUE_WORLD &&
                  g_received[1].As<wxl_render_point_args>().point == WXL_RENDER_POINT_AFTER_LIQUIDS &&
                  g_received[0].As<wxl_render_point_args>().frame != 0 && g_received[0].As<wxl_render_point_args>().samples == 0,
              "after the opaque world and after the liquids, RenderPoint with the frame's index (no samples: D3D9 is not drawn by the runtime)");
        Mute(WXL_EV_RenderPoint);
        g_received.clear();
        wxl::seam::WorldScene::RenderDetailDoodads::After();
        Check(g_received.empty(), "a point nobody listens to costs a flag");
        Unmute(WXL_EV_RenderPoint);
        wxl::seam::Gx::ScenePresent::Before();
    }

    // --- Component: AppearanceChanged, and models kept on the copies the client strips ------------

    wxl_str M2Str(const char* text) { return wxl_str{text, static_cast<uint32_t>(std::strlen(text))}; }

    void CheckAppearance()
    {
        g_component            = FakeComponent{};
        g_component.model      = &g_charModel;
        g_component.race       = 1;
        g_component.sex        = 1;
        g_component.display[4] = 416036;
        g_wearerAsked          = 0;

        Mute(WXL_EV_AppearanceChanged);
        g_received.clear();
        wxl::seam::Component::AddItem::After(&g_component, 4, nullptr, 0);
        Check(g_wearerAsked == 0 && g_received.empty(), "with nobody listening the Component seams do not even ask the client");
        Unmute(WXL_EV_AppearanceChanged);

        wxl::seam::Component::AddItem::After(&g_component, 4, nullptr, 0);
        const Received* worn = Only(WXL_EV_AppearanceChanged);
        Check(worn && worn->As<wxl_appearance_args>().change == WXL_APPEARANCE_WORN &&
                  worn->As<wxl_appearance_args>().wearer == &g_component &&
                  worn->As<wxl_appearance_args>().model == &g_charModel &&
                  worn->As<wxl_appearance_args>().item_slot == 4 &&
                  worn->As<wxl_appearance_args>().display_id == 416036 && worn->As<wxl_appearance_args>().race_id == 1 &&
                  worn->As<wxl_appearance_args>().sex_id == 1,
              "an item put on raises AppearanceChanged worn, with its slot, its display and the wearer");

        g_received.clear();
        wxl::seam::Component::AddItem::After(&g_component, 5, nullptr, 0);
        wxl::seam::Component::AddItem::After(&g_component, -1, nullptr, 0);
        Check(g_received.empty(), "an empty slot raises nothing, nor does a slot out of range");

        wxl::seam::Component::RemoveItem::Before(&g_component, 4);
        const Received* removed = Only(WXL_EV_AppearanceChanged);
        Check(removed && removed->As<wxl_appearance_args>().change == WXL_APPEARANCE_REMOVED &&
                  removed->As<wxl_appearance_args>().display_id == 416036,
              "an item about to come off raises removed, with the display it was");

        g_received.clear();
        wxl::seam::Component::FreeComponent::Before(&g_component);
        const Received* freed = Only(WXL_EV_AppearanceChanged);
        Check(freed && freed->As<wxl_appearance_args>().change == WXL_APPEARANCE_FREED &&
                  freed->As<wxl_appearance_args>().item_slot == 0 && freed->As<wxl_appearance_args>().display_id == 0,
              "a wearer about to be freed raises freed, with no slot");
        g_received.clear();
        wxl::seam::Component::FreeComponent::Before(nullptr);
        Check(g_received.empty(), "no component raises nothing");
    }

    void CheckAnimateGates()
    {
        namespace animate = wxl::core::tuning::animate;
        using wxl::core::tuning::FloatState;
        int cacheObject = 0;
        void* cache = &cacheObject;

        animate::SetThreadsForTests(0);
        {
            AnimScene a(9, 0);
            void* self = cache; void* fn = reinterpret_cast<void*>(&FakeAnimateThread); void* arg = &a.scene;
            Check(!wxl::seam::M2::BeginThread::Gate(&self, &fn, &arg) && a.scene.head == &a.models[0],
                  "animate_threads 0: BeginThread runs as the client's, the list untouched");
            void* wait = cache;
            Check(!wxl::seam::M2::WaitThread::Gate(&wait), "and WaitThread too");
        }

        animate::SetThreadsForTests(4);
        {
            AnimScene a(9, 0);
            void* self = cache; void* fn = reinterpret_cast<void*>(&CheckAnimateGates); void* arg = &a.scene;
            Check(!wxl::seam::M2::BeginThread::Gate(&self, &fn, &arg) && a.scene.head == &a.models[0],
                  "a BeginThread for another function than AnimateThread is the client's");
        }

        // The main thread's state made unlike a new thread's: x87 single precision (as D3D9 leaves
        // it) and MXCSR with flush to zero.
        const FloatState before = wxl::core::tuning::CaptureFloat();
        FloatState mainState = before;
#if defined(_M_IX86)
        mainState.control = 0x007F;
#endif
        mainState.mxcsr = 0x9F80;
        wxl::core::tuning::LoadFloat(mainState);
        {
            AnimScene a(301, 7);
            void* self = cache; void* fn = reinterpret_cast<void*>(&FakeAnimateThread); void* arg = &a.scene;
            const bool taken = wxl::seam::M2::BeginThread::Gate(&self, &fn, &arg);
            Check(taken && a.scene.head == nullptr, "animate_threads 4: BeginThread taken, the list hidden from the client's loop");
            void* other = &self;
            Check(!wxl::seam::M2::WaitThread::Gate(&other), "a WaitThread of another cache is the client's");
            void* wait = cache;
            Check(wxl::seam::M2::WaitThread::Gate(&wait) && a.scene.head == &a.models[0], "WaitThread taken, the list back");
            Check(!wxl::seam::M2::WaitThread::Gate(&wait), "and only once");
            bool once = true, attachedLeft = true, mainSide = true, workerSide = true;
            std::set<DWORD> threads;
            for (uint32_t i = 0; i < a.models.size(); ++i)
            {
                const FakeAnimModel& m = a.models[i];
                if (m.parent)
                {
                    attachedLeft = attachedLeft && m.animated == 0;
                    continue;
                }
                once = once && m.animated == 1;
                threads.insert(m.thread);
                if (i % 2 == 0) mainSide = mainSide && m.state == mainState;
                else workerSide = workerSide && !(m.state == mainState);
            }
            Check(once, "every model of the list hung on nothing is animated once");
            Check(attachedLeft, "a model attached to another is left to its parent");
            Check(mainSide, "the first, third... model with the main thread's floating point state");
            Check(workerSide, "the second, fourth... with a new thread's, as the client's worker has");
            Check(wxl::core::tuning::CaptureFloat() == mainState, "the main thread's own state is back after");
            printf("    (%zu thread(s) animated the 301 models)\n", threads.size());
        }
        wxl::core::tuning::LoadFloat(before);
        {
            AnimScene a(1000, 5);
            bool all = true;
            for (int frame = 0; frame < 50; ++frame)
            {
                all = all && a.Frame(cache);
                for (uint32_t i = 0; i < a.models.size(); ++i)
                    all = all && a.models[i].animated == (a.models[i].parent ? 0 : frame + 1);
            }
            Check(all, "50 frames of 1000 models: each animated once a frame, the client's loop finding nothing");
            AnimScene empty(0, 0);
            Check(empty.Frame(cache) && empty.scene.head == nullptr, "an empty list is taken and left empty");
        }
        const animate::Counters totals = animate::Totals();
        Check(totals.frames >= 52 && totals.models >= 50 * 800, "the pool counts its frames and models");
        animate::SetThreadsForTests(0);
    }

    void CheckDrops()
    {
        using wxl::core::tuning::drops::Drop;
        using wxl::core::tuning::drops::Outcome;
        // SMemAlloc's `call _msize` at 0x0076E58A: E8 and 0x004112F8 - 0x0076E58F.
        uint8_t code[5] = {0xE8, 0x69, 0x2D, 0xCA, 0xFF};
        Check(Drop(code, 0x0076E58A, 0x004112F8) == Outcome::dropped && code[0] == 0x0F && code[1] == 0x1F && code[4] == 0x00,
              "the stock call is replaced by a five-byte NOP");
        Check(Drop(code, 0x0076E58A, 0x004112F8) == Outcome::already, "a second time, it is already gone");
        uint8_t hooked[5] = {0xE9, 0x00, 0x00, 0x00, 0x00};
        Check(Drop(hooked, 0x0076E58A, 0x004112F8) == Outcome::foreign && hooked[0] == 0xE9, "other bytes are left as they are");
        uint8_t other[5] = {0xE8, 0x69, 0x2D, 0xCA, 0xFF};
        Check(Drop(other, 0x0076E5AC, 0x004112F8) == Outcome::foreign, "a call to another place is left too");
    }

    void CheckM2Service()
    {
        const wxl_m2_api& m2 = wxl::core::services::M2Api();
        g_component.model    = &g_charModel;

        wxl_m2_item_display row{};
        row.struct_size = sizeof row;
        Check(m2.item_display(416036, &row) == WXL_OK && row.id == 416036 && std::strcmp(row.model[0], "belt.mdx") == 0 &&
                  std::strcmp(row.icon[1], "53::96") == 0 && row.particle_color_id == 7,
              "an ItemDisplayInfo row is read through");
        wxl_m2_item_display shorter{};
        shorter.struct_size = static_cast<uint32_t>(offsetof(wxl_m2_item_display, model_texture));
        Check(m2.item_display(416036, &shorter) == WXL_OK && shorter.model[0] && !shorter.model_texture[0] &&
                  shorter.struct_size == offsetof(wxl_m2_item_display, model_texture),
              "and written no longer than the caller's struct_size");
        Check(m2.item_display(1, &row) == WXL_E_NOT_FOUND, "a row the client lacks is not found");
        wxl_str prefix{};
        Check(m2.race_prefix(1, &prefix) == WXL_OK && std::string(prefix.data, prefix.size) == "Hu" &&
                  m2.race_prefix(99, &prefix) == WXL_E_NOT_FOUND,
              "a race's client prefix is read through, not found for a race the client lacks");

        Check(m2.create_model(nullptr, M2Str("a.mdx")) == nullptr, "a model needs a scene to be made in");
        void* piece = m2.create_model(&g_charModel, M2Str("Item\\ObjectComponents\\Collections\\a_HuF.mdx"));
        Check(piece != nullptr, "a model is made in the scene of the one given");
        FakeM2Model stranger;
        Check(m2.attach_model(&stranger, &g_charModel, 19, WXL_M2_ATTACH_ROOT) == WXL_E_INVALID,
              "a model the service did not make is refused");
        Check(m2.attach_model(piece, &g_charModel, 99, 0) == WXL_E_NOT_FOUND && !wxl::core::services::m2::Following(),
              "a point the parent lacks is refused without ATTACH_ROOT, and nothing follows");

        Check(m2.attach_model(piece, &g_charModel, 99, WXL_M2_ATTACH_ROOT | WXL_M2_ATTACH_FOLLOW_BONES) == WXL_OK &&
                  m2.model_parent(piece) == &g_charModel && wxl::core::services::m2::Following(),
              "with ATTACH_ROOT it hangs on the parent anyway, and ATTACH_FOLLOW_BONES makes it follow");

        // The body's spine and hand posed this frame; the piece's bones take them by key bone and
        // by name, its root (neither) the body bone nearest its pivot.
        for (int b = 0; b < 3; ++b)
            for (int k = 0; k < 16; ++k) g_charModel.matrices[b][k] = static_cast<float>(10 * (b + 1) + k);
        wxl::seam::M2::AnimateMT::After(piece, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        const FakeM2Model* posed = AsFake(piece);
        Check(posed->matrices[1][0] == 20.0f && posed->matrices[2][15] == 45.0f && posed->matrices[0][3] == 13.0f,
              "a following model's bones take the parent's of the same key bone, name, or nearest pivot");

        FakeM2Model copy;
        copy.parent = &g_charModel, copy.source = piece;
        wxl::seam::M2::AnimateMT::After(&copy, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(copy.matrices[1][0] == 20.0f, "an interface copy of a following model follows too");

        FakeM2Model loadedCopy;
        loadedCopy.parent = &g_charModel, loadedCopy.file = piece;
        wxl::seam::M2::AnimateMTSimple::After(&loadedCopy, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(loadedCopy.matrices[1][0] == 20.0f,
              "a loaded copy, which no longer names its source, follows by the file it shares");

        FakeM2Model other;
        other.parent = &g_charModel;
        wxl::seam::M2::AnimateMT::After(&other, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(other.matrices[1][0] == 0.0f, "any other model is left as the client posed it");

        m2.set_geosets(piece, 1, 0xFFFF, WXL_FALSE);
        Check(posed->geoFirst == 1 && posed->geoLast == 0xFFFF && !posed->geoVisible, "geosets are shown and hidden through");
        Check(m2.replace_texture(piece, 2, M2Str("Item\\ObjectComponents\\Waist\\belt.blp")) == WXL_OK && posed->texType == 2 &&
                  m2.replace_texture(piece, 2, M2Str("missing.blp")) == WXL_E_NOT_FOUND &&
                  m2.replace_texture(&stranger, 2, M2Str("a.blp")) == WXL_E_INVALID,
              "a texture is replaced through; not found with no file; refused on a model the service did not make");

        m2.detach_model(piece);
        Check(m2.model_parent(piece) == nullptr && !wxl::core::services::m2::Following(),
              "a detached model hangs on nothing and follows nothing");
        const int freed = g_fakeModelsFreed;
        m2.release_model(piece);
        m2.release_model(piece);
        Check(g_fakeModelsFreed == freed + 1, "a released model is freed, once");

        // RemoveVisuals strips what hangs on its points: the service's own models are hung back,
        // and copies of them, which the strip freed, are copied again.
        void* robe = m2.create_model(&g_charModel, M2Str("robe.mdx"));
        void* belt = m2.create_model(&g_charModel, M2Str("belt.mdx"));
        m2.attach_model(robe, &g_charModel, 19, WXL_M2_ATTACH_ROOT | WXL_M2_ATTACH_FOLLOW_BONES);
        m2.attach_model(belt, &g_charModel, 53, WXL_M2_ATTACH_ROOT);
        wxl::seam::Component::RemoveVisuals::Before(&g_charModel);
        FakeDetachModel(robe); // what DetachAllChildrenById(19) does
        wxl::seam::Component::RemoveVisuals::After(&g_charModel);
        Check(m2.model_parent(robe) == &g_charModel && AsFake(robe)->attachment == 19 && AsFake(robe)->toRoot,
              "a stripped model of the service is hung back where it was");
        Check(m2.model_parent(belt) == &g_charModel && g_fakeCopies == 0, "one on a point not stripped is left alone");

        FakeM2Model  panel;
        FakeM2Model* shown = new FakeM2Model{};
        shown->parent = &panel, shown->attachment = 19, shown->toRoot = WXL_TRUE, shown->file = robe;
        g_fakeLive.push_back(shown);
        wxl::seam::Component::RemoveVisuals::Before(&panel);
        std::erase(g_fakeLive, shown); // the strip frees the copy: the panel held its only reference
        delete shown;
        wxl::seam::Component::RemoveVisuals::After(&panel);
        Check(g_fakeCopies == 1 && FakeChildren(&panel, nullptr, 0) == 1,
              "a stripped interface copy is copied again from the model it was made from");
        for (FakeM2Model* fake : std::vector<FakeM2Model*>(g_fakeLive))
            if (fake->parent == &panel)
            {
                std::erase(g_fakeLive, fake);
                delete fake;
            }
        m2.release_model(robe);
        m2.release_model(belt);

        // A point the body lacks, made by hand: posed on one of its key bones, moved by an offset.
        wxl_vec3 hip{};
        Check(m2.attachment_point(&g_charModel, 32, &hip) == WXL_OK && hip.z == 1.1f &&
                  m2.attachment_point(&g_charModel, 53, &hip) == WXL_E_NOT_FOUND,
              "an attachment point is read through; not found for one the body lacks");
        void* buckle = m2.create_model(&g_charModel, M2Str("buckle.mdx"));
        Check(m2.attach_to_bone(buckle, &g_charModel, 53, 7, wxl_vec3{0.0f, 0.0f, 0.0f}) == WXL_E_NOT_FOUND,
              "a key bone the body lacks is refused");
        Check(m2.attach_to_bone(buckle, &g_charModel, 53, 5, wxl_vec3{1.0f, 0.0f, 0.0f}) == WXL_OK &&
                  m2.model_parent(buckle) == &g_charModel && AsFake(buckle)->attachment == 53 && AsFake(buckle)->toRoot,
              "a model is hung on a key bone, as the point it stands for");
        wxl::seam::M2::AnimateMT::After(buckle, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        // The spine's matrix is 20..35: the translation row (32, 33, 34) gains the first row (20, 21, 22).
        Check(AsFake(buckle)->matrices[0][12] == 52.0f && AsFake(buckle)->matrices[2][14] == 56.0f &&
                  AsFake(buckle)->matrices[1][0] == 20.0f,
              "and every bone is posed on that bone, moved by the offset through it");
        m2.release_model(buckle);
        Check(!wxl::core::services::m2::Following(), "and nothing is posed once it is released");

        void* kept = m2.create_model(&g_charModel, M2Str("b.mdx"));
        m2.attach_model(kept, &g_charModel, 53, WXL_M2_ATTACH_ROOT | WXL_M2_ATTACH_FOLLOW_BONES);
        Check(AsFake(kept)->refs == 2, "an attached model is held by the mod and the parent");
        const int before = g_fakeModelsFreed;
        wxl::core::services::m2::ForgetMod(wxl::core::Host() ? wxl::core::Host()->CurrentMod() : 0);
        Check(g_fakeModelsFreed == before + 1 && !wxl::core::services::m2::Following() &&
                  !wxl::core::services::m2::HasModels(),
              "a disabled mod's models are detached and freed");

        int unit = 0;
        Check(m2.object_model(WXL_HANDLE_NONE) == nullptr, "no object, no model");
        (void)unit;
    }

    // --- wxl.m2: models of the world and bone watchers ------------------------------------------

    struct BoneCounts
    {
        std::atomic<int> asked{0};
        std::atomic<int> called{0};
        std::atomic<int> faulted{0};
    };
    BoneCounts g_boneCounts;

    wxl_bool WXL_CALL OnlyA(void*, wxl_str path, uint32_t count, void*)
    {
        ++g_boneCounts.asked;
        return count == 3 && path.size == 4 && path.data[0] == 'a' ? WXL_TRUE : WXL_FALSE;
    }
    void WXL_CALL MoveFirstBone(const wxl_m2_bones* bones, void* user)
    {
        ++g_boneCounts.called;
        bones->matrices[0] = static_cast<float>(bones->time_ms) + (user ? 1.0f : 0.0f);
    }
    void WXL_CALL FaultingWatcher(const wxl_m2_bones*, void*)
    {
        ++g_boneCounts.faulted;
        *static_cast<volatile int*>(nullptr) = 1;
    }

    void CheckM2World()
    {
        const wxl_m2_api& m2 = wxl::core::services::M2Api();
        g_world              = FakeWorldState{};
        const int32_t map    = g_mapId;

        Check(m2.create_world_model(M2Str("")) == nullptr, "a world model needs a file");
        void* effect = m2.create_world_model(M2Str("Spells\\Fire_Base.m2"));
        Check(effect && g_world.made == 1, "a model is made in the world's scene");

        wxl_m2_placement at{};
        at.struct_size = sizeof at;
        at.position    = wxl_vec3{1.0f, 2.0f, 3.0f};
        at.yaw         = 0.5f;
        g_mapId        = -1;
        Check(m2.place_model(effect, &at) == WXL_E_FAILED && g_world.placed == 0, "nothing is placed with no map loaded");
        g_mapId = 0;
        FakeM2Model stranger;
        Check(m2.place_model(&stranger, &at) == WXL_E_INVALID && m2.place_model(effect, nullptr) == WXL_E_INVALID,
              "a model the mod did not make, or no placement, is refused");
        Check(m2.place_model(effect, &at) == WXL_OK && g_world.placed == 1 && g_world.placement.yaw == 0.5f &&
                  g_world.placement.scale == 1.0f && g_world.placement.position.z == 3.0f,
              "a placement reaches the client, a scale of 0 read as 1");
        wxl_m2_placement older{};
        older.struct_size = static_cast<uint32_t>(offsetof(wxl_m2_placement, yaw));
        older.position    = wxl_vec3{4.0f, 5.0f, 6.0f};
        older.yaw         = 9.0f;
        Check(m2.place_model(effect, &older) == WXL_OK && g_world.placement.position.x == 4.0f &&
                  g_world.placement.yaw == 0.0f && g_world.placement.scale == 1.0f,
              "a shorter placement is read no further than its struct_size");

        at.scale = 2.0f;
        Check(m2.set_model_transform(effect, &at) == WXL_OK && g_world.transform.scale == 2.0f,
              "a model's transform on its point reaches the client");
        Check(m2.play_animation(effect, 158, 0.0f) == WXL_OK && g_world.anim == 158 && g_world.speed == 1.0f &&
                  m2.play_animation(&stranger, 1, 1.0f) == WXL_E_INVALID,
              "an animation plays at speed 1 unless asked, on the mod's models only");
        m2.set_alpha(effect, 2.0f);
        m2.set_emitting(effect, WXL_FALSE);
        Check(g_world.alpha == 1.0f && g_world.emitting == 0, "alpha is held to [0, 1]; emitters are switched");

        uint32_t length = 0;
        Check(m2.model_loaded(effect) && m2.animation_length(effect, 0, &length) == WXL_OK && length == 1333,
              "a loaded model answers how long an animation lasts");
        AsFake(effect)->loaded = false;
        Check(!m2.model_loaded(effect) && m2.animation_length(effect, 0, &length) == WXL_E_FAILED,
              "and fails while it loads");
        AsFake(effect)->loaded = true;
        wxl_vec3 hand{};
        Check(m2.attachment_world(&g_charModel, 32, &hand) == WXL_OK && hand.z == 31.5f &&
                  m2.attachment_world(&g_charModel, 1, &hand) == WXL_E_NOT_FOUND,
              "a point of a loaded model is read in the world; not found for one it lacks");

        float          ground = -1.0f;
        const wxl_vec3 above{5.0f, 5.0f, 10.0f};
        Check(m2.ground_height(&above, 2.0f, 50.0f, &ground) == WXL_OK && ground == 0.0f &&
                  g_traceFlags == 0x120111,
              "the ground is traced straight down, with the mask the client's spell effects use");
        const wxl_vec3 below{5.0f, 5.0f, -5.0f};
        Check(m2.ground_height(&below, 1.0f, 1.0f, &ground) == WXL_E_NOT_FOUND &&
                  m2.ground_height(&above, -1.0f, 1.0f, &ground) == WXL_E_INVALID,
              "no ground in reach is not found; a negative reach is refused");

        // Leaving the world takes the placed models out of it, once.
        g_uiReloading = 1;
        wxl::seam::World::LeaveWorld::Before();
        Check(g_world.unplaced == 0, "a /reload leaves the placed models where they are");
        g_uiReloading = 0;
        wxl::seam::World::LeaveWorld::Before();
        Check(g_world.unplaced == 1, "leaving the world takes them out of it");
        const int freed = g_fakeModelsFreed;
        m2.release_model(effect);
        Check(g_world.unplaced == 1 && g_fakeModelsFreed == freed + 1, "a model taken out is only released after");
        void* again = m2.create_world_model(M2Str("Spells\\Fire_Base.m2"));
        m2.place_model(again, &at);
        m2.release_model(again);
        Check(g_world.unplaced == 2 && g_fakeModelsFreed == freed + 2, "a placed model released is taken out first");

        // Bone watchers.
        uint32_t token = 0;
        Check(m2.watch_bones(nullptr, nullptr, nullptr, &token) == WXL_E_INVALID && !wxl::core::services::m2::WatchingBones(),
              "a watcher needs a function");
        int  user = 0;
        Check(m2.watch_bones(&OnlyA, &MoveFirstBone, &user, &token) == WXL_OK && token != 0 &&
                  wxl::core::services::m2::WatchingBones(),
              "a watcher is taken");
        FakeM2Model a, b;
        b.texType = 2; // the fake names it b.m2
        wxl::seam::M2::AnimateMT::After(&a, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        wxl::seam::M2::AnimateMT::After(&a, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        wxl::seam::M2::AnimateMT::After(&b, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(g_boneCounts.asked == 2 && g_boneCounts.called == 2 && a.matrices[0][0] == 43.0f && b.matrices[0][0] == 0.0f,
              "the filter is asked once per file, the watcher called each frame for the files it took");
        int16_t  parent = 0;
        wxl_vec3 pivot{};
        Check(m2.bone_info(&a, 1, &parent, &pivot) == WXL_OK && parent == 0 && pivot.z == 1.0f &&
                  m2.bone_info(&a, 3, &parent, &pivot) == WXL_E_NOT_FOUND,
              "a file's bone: its parent and pivot; not found past the last");

        uint32_t faulty = 0;
        Check(m2.watch_bones(nullptr, &FaultingWatcher, nullptr, &faulty) == WXL_OK, "a second watcher is taken");
        wxl::seam::M2::AnimateMT::After(&a, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        wxl::seam::M2::AnimateMT::After(&a, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(g_boneCounts.faulted == 1 && g_boneCounts.called == 4 && g_boneCounts.asked == 4,
              "a watcher that faults is dropped; the others go on, the files asked again of the new set");

        m2.unwatch_bones(token);
        Check(!wxl::core::services::m2::WatchingBones(), "unwatched, no watcher is left");
        const int asked = g_bonesAsked;
        wxl::seam::M2::AnimateMT::After(&a, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(g_bonesAsked == asked && g_boneCounts.called == 4, "with no watcher the client is not even asked");

        std::vector<uint32_t> tokens;
        for (uint32_t i = 0; i < 16; ++i)
        {
            uint32_t t = 0;
            if (m2.watch_bones(nullptr, &MoveFirstBone, nullptr, &t) == WXL_OK) tokens.push_back(t);
        }
        uint32_t past = 0;
        Check(tokens.size() == 16 && m2.watch_bones(nullptr, &MoveFirstBone, nullptr, &past) == WXL_E_FAILED,
              "sixteen watchers at most");
        wxl::core::services::m2::ForgetMod(wxl::core::Host() ? wxl::core::Host()->CurrentMod() : 0);
        Check(!wxl::core::services::m2::WatchingBones(), "a disabled mod's watchers go with it");
        g_mapId = map;
    }

    // --- M2Shared: the loader's stages as events, and wxl.m2file ----------------------------------

    void CheckModelStages()
    {
        g_m2File = FakeM2File{};

        Mute(WXL_EV_ModelParsed);
        g_received.clear();
        wxl::seam::M2Shared::Initialize::Before(&g_m2File);
        Check(g_m2File.asked == 0 && g_received.empty(), "with nobody listening the loader seams do not even ask the client");
        Unmute(WXL_EV_ModelParsed);

        wxl::seam::M2Shared::Initialize::Before(&g_m2File);
        const Received* parsed = Only(WXL_EV_ModelParsed);
        Check(parsed && parsed->As<wxl_model_parsed_args>().file == &g_m2File &&
                  parsed->As<wxl_model_parsed_args>().header == g_m2File.bytes &&
                  parsed->As<wxl_model_parsed_args>().size == sizeof g_m2File.bytes &&
                  parsed->As<wxl_model_parsed_args>().path.size == strlen("creature\\fake\\fake.m2"),
              "M2Shared.Initialize raises ModelParsed with the file, its header, its size and its path");

        g_received.clear();
        wxl::seam::M2Shared::InitializeSkinProfile::Before(&g_m2File);
        Check(g_received.empty(), "no skin profile read, no SkinProfileParsed");
        g_m2File.hasSkin = true;
        wxl::seam::M2Shared::InitializeSkinProfile::Before(&g_m2File);
        const Received* skinned = Only(WXL_EV_SkinProfileParsed);
        Check(skinned && skinned->As<wxl_skin_profile_parsed_args>().skin == g_m2File.skin &&
                  skinned->As<wxl_skin_profile_parsed_args>().header == g_m2File.bytes,
              "M2Shared.InitializeSkinProfile raises SkinProfileParsed with the skin profile's header");

        uint8_t anim[32]{};
        g_received.clear();
        wxl::seam::M2Shared::FinishLoadingLowPrioritySequence::Before(&g_m2File, 7, anim, sizeof anim);
        const Received* sequence = Only(WXL_EV_ModelSequenceLoad);
        Check(sequence && sequence->As<wxl_model_sequence_args>().data == anim &&
                  sequence->As<wxl_model_sequence_args>().size == sizeof anim &&
                  sequence->As<wxl_model_sequence_args>().sequence_index == 7,
              "M2Shared.FinishLoadingLowPrioritySequence raises ModelSequenceLoad with its bytes and index");
        g_received.clear();
        wxl::seam::M2Shared::FinishLoadingLowPrioritySequence::Before(&g_m2File, 7, nullptr, 0);
        wxl::seam::M2Shared::Initialize::Before(nullptr);
        Check(g_received.empty(), "no buffer, or no file, raises nothing");

        const wxl_m2file_api& api = wxl::core::services::M2FileApi();
        api.animate_bones(&g_m2File);
        Check(g_m2File.animated == 1, "animate_bones reaches the client's file");

        uint8_t* first  = static_cast<uint8_t*>(api.alloc_near(&g_m2File, 100));
        uint8_t* second = static_cast<uint8_t*>(api.alloc_near(&g_m2File, 20));
        bool     zero   = first != nullptr;
        for (int i = 0; first && i < 100; ++i) zero = zero && first[i] == 0;
        const auto reach = [](const void* a, const void* b) {
            const int64_t d = static_cast<int64_t>(reinterpret_cast<uintptr_t>(a)) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(b));
            return d > -0x40000000LL && d < 0x40000000LL;
        };
        Check(first && second && zero && (reinterpret_cast<uintptr_t>(first) & 15) == 0 &&
                  (reinterpret_cast<uintptr_t>(second) & 15) == 0 && second >= first + 100 && reach(first, &g_m2File) &&
                  reach(second, &g_m2File),
              "alloc_near hands out zeroed, aligned, distinct memory within reach of the anchor");
        Check(api.alloc_near(nullptr, 8) == nullptr && api.alloc_near(&g_m2File, 0) == nullptr,
              "alloc_near refuses no anchor and no size");

        // An array of the header, pointed at that memory and read back the way this client keeps it.
        uint8_t* array = g_m2File.bytes + 8;
        Check(api.set_array(array, first, 25) == WXL_OK && api.array_data(array) == first &&
                  *reinterpret_cast<uint32_t*>(array) == 25,
              "set_array and array_data agree on this client's way of keeping an array");
        uint32_t word = 0;
        std::memcpy(&word, array + 4, sizeof word);
#if defined(_M_X64)
        Check(static_cast<int32_t>(word) == static_cast<int32_t>(first - array), "on x64 the array keeps an offset from itself");
        static char far[1];
        Check(reach(far, array) || api.set_array(array, far, 1) == WXL_E_INVALID,
              "memory out of a 32-bit offset's reach is refused");
#else
        Check(word == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(first)), "on x86 the array keeps the address");
#endif
        Check(api.set_array(array, nullptr, 0) == WXL_OK && api.array_data(array) == nullptr &&
                  api.set_array(array, nullptr, 3) == WXL_E_INVALID,
              "an empty array has no elements; a count with nothing is refused");
    }

    // --- The x86 binding's model calls: the shapes they take, against stand-ins -----------------

#if defined(_M_IX86)
    struct ModelStandIn
    {
        int   released = 0;
        void* parent   = nullptr;
        int   root     = -1;

        __declspec(noinline) void __thiscall Release() { ++released; }
        __declspec(noinline) void __thiscall Attach(void* to, uint32_t id, const void* position, int32_t toRoot)
        {
            parent = position ? nullptr : to;
            root   = toRoot + static_cast<int32_t>(id);
        }
        __declspec(noinline) void __thiscall Geosets(uint32_t first, uint32_t last, int32_t visible)
        {
            root = static_cast<int32_t>(first + last) * (visible ? 1 : -1);
        }
        __declspec(noinline) void* __thiscall Create(const char* path, uint32_t flags)
        {
            return path && !flags ? this : nullptr;
        }
        __declspec(noinline) int32_t __thiscall GetRecord(int32_t id, void* rec)
        {
            if (id != 7) return 0;
            std::memset(rec, 0x5A, 100);
            return 1;
        }
    };

    int g_closed = 0;
    __declspec(noinline) void* __cdecl CreateTextureStandIn(const char* path, void* status)
    {
        return path && status ? reinterpret_cast<void*>(0x1000) : nullptr;
    }
    __declspec(noinline) void __cdecl HandleCloseStandIn(void* handle) { g_closed += handle ? 1 : 0; }

    void CheckModelCallsX86()
    {
        using wxl::core::binding::x86::CallFree;
        using wxl::core::binding::x86::CallMember;

        ModelStandIn      model;
        volatile uint32_t canary = 0xC0FFEE;
        CallMember<void>(AddressOf(&ModelStandIn::Release), &model);
        Check(model.released == 1, "a __thiscall member taking nothing (Release, DetachFromParent, IRsForceUpdate)");
        int parent = 0;
        CallMember<void>(AddressOf(&ModelStandIn::Attach), &model, static_cast<void*>(&parent), uint32_t(19),
                         static_cast<const void*>(nullptr), int32_t(1));
        Check(model.parent == &parent && model.root == 20, "AttachToParent's four, a null position among them");
        CallMember<void>(AddressOf(&ModelStandIn::Geosets), &model, uint32_t(1), uint32_t(2), int32_t(0));
        Check(model.root == -3, "SetGeometryVisible's three");
        Check(CallMember<void*>(AddressOf(&ModelStandIn::Create), &model, "a.m2", uint32_t(0)) == &model,
              "CreateModel and DuplicateModel's two, a pointer answered in EAX");
        uint8_t rec[100] = {};
        Check(CallMember<int32_t>(AddressOf(&ModelStandIn::GetRecord), &model, int32_t(7), static_cast<void*>(rec)) == 1 &&
                  rec[99] == 0x5A,
              "a record store's GetRecord(id, row)");
        uint8_t status[16] = {};
        void* texture = CallFree<void*>(reinterpret_cast<std::uintptr_t>(Opaque(&CreateTextureStandIn)), "a.blp",
                                        static_cast<void*>(status));
        CallFree<void>(reinterpret_cast<std::uintptr_t>(Opaque(&HandleCloseStandIn)), texture);
        Check(texture && g_closed == 1, "CreateTexture's two and HandleClose's one, cdecl");
        Check(canary == 0xC0FFEE, "and the stack is the caller's again after each");
    }
#endif

    /// The seams the stock client cannot take: not attached there, their events not announced.
    /// No row needs it today: both clients take every seam and announce every event.
    void CheckX64Only()
    {
        Check(!wxl::core::seams::X64Only("M2.DrawBatch") && !wxl::core::seams::X64Only("M2.AnimateMT") &&
                  !wxl::core::seams::X64Only("Component.AddItem") && !wxl::core::seams::X64Only(nullptr),
              "every model seam is on both clients");
        const std::vector<wxl_event> all   = wxl::core::seams::RaisedEvents(false);
        const std::vector<wxl_event> stock = wxl::core::seams::RaisedEvents(true);
        Check(stock == all && std::find(stock.begin(), stock.end(), WXL_EV_ModelBatchDrawn) != stock.end(),
              "the stock client announces every event, ModelBatchDrawn with them");
    }

#if defined(_M_IX86)
    // --- The x86 rebuild of a drawn batch's numbers, over stand-ins laid out as Wow.exe's --------

    /// Zeroed bytes the size of one of the client's structs, written at measured offsets.
    struct Block
    {
        std::vector<uint8_t> bytes;
        explicit Block(std::size_t size) : bytes(size, 0) {}
        template <class T>
        void Put(std::size_t offset, T value) { std::memcpy(bytes.data() + offset, &value, sizeof value); }
        void* At() { return bytes.data(); }
    };

    void CheckModelDrawX86()
    {
        namespace md = wxl::core::binding::x86::model_draw;
        Block render(WXL_TYPE_SIZE("CM2SceneRender")), device(WXL_TYPE_SIZE("CGxDeviceD3d9Ex"));
        Block section(WXL_TYPE_SIZE("M2SkinSection")), material(WXL_TYPE_SIZE("M2Material"));
        Block vertices(WXL_TYPE_SIZE("CGxBuf")), indices(WXL_TYPE_SIZE("CGxBuf"));
        Block model(WXL_TYPE_SIZE("CM2Model")), parent(WXL_TYPE_SIZE("CM2Model")), shared(WXL_TYPE_SIZE("CM2Shared"));
        Block header(WXL_TYPE_SIZE("M2Data")), cache(WXL_TYPE_SIZE("CM2Cache"));

        // A section of 600 indices from 1200 over 150 vertices from 40, alpha-blended.
        section.Put<uint16_t>(WXL_FIELD("M2SkinSection", "m_vertexStart"), 40);
        section.Put<uint16_t>(WXL_FIELD("M2SkinSection", "m_vertexCount"), 150);
        section.Put<uint16_t>(WXL_FIELD("M2SkinSection", "m_indexStart"), 1200);
        section.Put<uint16_t>(WXL_FIELD("M2SkinSection", "m_indexCount"), 600);
        material.Put<uint16_t>(WXL_FIELD("M2Material", "m_blendMode"), 2);
        model.Put<void*>(WXL_FIELD("CM2Model", "m_attachParent"), parent.At());
        model.Put<void*>(WXL_FIELD("CM2Model", "m_shared"), shared.At());
        shared.Put<void*>(WXL_FIELD("CM2Shared", "m_data"), header.At());
        header.Put<uint32_t>(WXL_FIELD("M2Data", "m_bones.m_count"), 1);
        render.Put<void*>(WXL_FIELD("CM2SceneRender", "m_curModel"), model.At());
        render.Put<void*>(WXL_FIELD("CM2SceneRender", "m_curSkinSection"), section.At());
        render.Put<void*>(WXL_FIELD("CM2SceneRender", "m_curMaterial"), material.At());
        render.Put<void*>(WXL_FIELD("CM2SceneRender", "m_cache"), cache.At());

        // A D3D9Ex device with a context, its vertex stream 32 bytes a vertex at byte 3200 and
        // its index buffer at byte 512, a full 1280x720 viewport over depth 0-1.
        device.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_api"), md::kApiD3d9Ex);
        device.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_context"), 1);
        device.Put<void*>(WXL_FIELD("CGxDevice", "m_primVertexFormatBuf"), vertices.At());
        device.Put<void*>(WXL_FIELD("CGxDevice", "m_primIndexBuf"), indices.At());
        vertices.Put<uint32_t>(WXL_FIELD("CGxBuf", "m_itemSize"), 32);
        vertices.Put<uint32_t>(WXL_FIELD("CGxBuf", "m_index"), 3200);
        indices.Put<uint32_t>(WXL_FIELD("CGxBuf", "m_index"), 512);
        device.Put<float>(WXL_FIELD("CGxDevice", "m_curWindowRect.maxX"), 1280.0f);
        device.Put<float>(WXL_FIELD("CGxDevice", "m_curWindowRect.maxY"), 720.0f);
        device.Put<float>(WXL_FIELD("CGxDevice", "m_viewport.m_x.m_h"), 1.0f);
        device.Put<float>(WXL_FIELD("CGxDevice", "m_viewport.m_y.m_h"), 1.0f);
        device.Put<float>(WXL_FIELD("CGxDevice", "m_viewport.m_z.m_h"), 1.0f);

        const uint32_t conversion[] = {1, 2, 3, 4, 5, 6, 0}; // as Wow.exe's tables: D3DPT_TRIANGLELIST at 3
        md::Globals    globals{device.At(), 1, conversion, conversion};
        wxl_model_batch_args out{};
        Check(md::Rebuild(render.At(), globals, &out), "a batch drawn on a D3D9 device is rebuilt");
        Check(out.model == model.At() && out.root_model == parent.At() && out.primitive_type == 4 &&
                  out.blend_mode == 2,
              "with its model, the top of what it hangs on, a triangle list and its blend");
        Check(out.min_index == 40 && out.vertex_count == 150 && out.start_index == 1200 + 256 &&
                  out.primitive_count == 200 && out.base_vertex == 100,
              "with shaders, the section's vertices, its indices after the index buffer's own, and "
              "the vertex stream's offset as the base vertex");
        Check(out.viewport_x == 0 && out.viewport_y == 0 && out.viewport_width == 1280 && out.viewport_height == 720 &&
                  out.viewport_min_z == 0.0f && out.viewport_max_z == 1.0f,
              "and the viewport the device set");

        device.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_caps.m_streamOffset"), 1);
        globals.shaders = 0;
        out = {};
        Check(md::Rebuild(render.At(), globals, &out) && out.base_vertex == 0 && out.min_index == 0 && out.vertex_count == 150,
              "without shaders, a model skinned on the CPU draws from vertex 0; a device that offsets "
              "streams has no base vertex");
        cache.Put<uint32_t>(WXL_FIELD("CM2Cache", "m_flags"), md::kSharedVertices);
        out = {};
        Check(md::Rebuild(render.At(), globals, &out) && out.min_index == 40 && out.vertex_count == 150,
              "a one-bone model sharing its vertices draws from the section's own");

        device.Put<float>(WXL_FIELD("CGxDevice", "m_viewport.m_y.m_l"), 0.25f);
        device.Put<void*>(WXL_FIELD("CGxDevice", "m_textureTarget[1].m_apiSpecific"), device.At());
        out = {};
        Check(md::Rebuild(render.At(), globals, &out) && out.viewport_y == 180 && out.viewport_height == 540,
              "drawing into a texture, the viewport's Y counts from the other edge");

        device.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_intF5C"), 1);
        Check(!md::Rebuild(render.At(), globals, &out), "a lost device drew nothing");
        device.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_intF5C"), 0);
        device.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_api"), 0);
        Check(!md::Rebuild(render.At(), globals, &out), "nor is an OpenGL device's draw rebuilt");
        device.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_api"), md::kApiD3d9);
        render.Put<void*>(WXL_FIELD("CM2SceneRender", "m_curSkinSection"), nullptr);
        Check(!md::Rebuild(render.At(), globals, &out) && !md::Rebuild(nullptr, globals, &out),
              "nor a renderer with no section, nor none");
    }

    // --- wxl.highlight on x86: what a batch's draw had bound, read from the engine ----------------

    void CheckHighlightDrawX86()
    {
        namespace hd = wxl::core::binding::x86::highlight_draw;
        Block device(WXL_TYPE_SIZE("CGxDeviceD3d9Ex")), shader(WXL_TYPE_SIZE("CGxShader"));
        Block states(WXL_TYPE_SIZE("CGxStateBom") * 100);
        int   decl = 0, index = 0, stream0 = 0, stream2 = 0, program = 0;

        device.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_api"), hd::kApiD3d9Ex);
        device.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_context"), 1);
        device.Put<void*>(WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dCurrentVertexDecl"), &decl);
        device.Put<void*>(WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dCurrentIndexBuf"), &index);
        device.Put<void*>(WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dVertexStreamBuf") + 0, &stream0);
        device.Put<void*>(WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dVertexStreamBuf") + 2 * sizeof(void*), &stream2);
        device.Put<uint32_t>(WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dVertexStreamOfs") + 2 * 4, 640);
        device.Put<uint32_t>(WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dVertexStreamStride") + 0, 48);
        device.Put<uint32_t>(WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dVertexStreamStride") + 2 * 4, 8);
        device.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_hwRenderStates.m_count"), 100);
        device.Put<void*>(WXL_FIELD("CGxDevice", "m_hwRenderStates.m_data"), states.At());
        states.Put<void*>(hd::kRsVertexShader * WXL_TYPE_SIZE("CGxStateBom"), shader.At());
        shader.Put<void*>(WXL_FIELD("CGxShader", "m_apiSpecific"), &program);
        Block texture(WXL_TYPE_SIZE("CGxTex"));
        int   d3dTexture = 0;
        texture.Put<void*>(WXL_FIELD("CGxTex", "m_apiSpecificData"), &d3dTexture);
        states.Put<void*>(hd::kRsTexture0 * WXL_TYPE_SIZE("CGxStateBom"), texture.At());
        states.Put<int32_t>(hd::kRsAlphaRef * WXL_TYPE_SIZE("CGxStateBom"), 224);

        hd::State state;
        Check(hd::ReadState(device.At(), state) && state.declaration == &decl && state.indices == &index &&
                  state.streams[0] == &stream0 && state.streams[1] == nullptr && state.streams[2] == &stream2 &&
                  state.offsets[2] == 640 && state.strides[0] == 48 && state.strides[2] == 8 && state.vertexShader == &program,
              "a D3D9Ex device's declaration, index buffer, streams and vertex program are read from its copy");
        Check(state.texture0 == &d3dTexture && state.alphaRef == 224, "with its first texture and its alpha test's reference");
        states.Put<int32_t>(hd::kRsAlphaRef * WXL_TYPE_SIZE("CGxStateBom"), 0);
        states.Put<void*>(hd::kRsTexture0 * WXL_TYPE_SIZE("CGxStateBom"), nullptr);
        Check(hd::ReadState(device.At(), state) && state.texture0 == nullptr && state.alphaRef == 0,
              "a draw without texture or alpha test has neither");
        states.Put<void*>(hd::kRsVertexShader * WXL_TYPE_SIZE("CGxStateBom"), nullptr);
        Check(hd::ReadState(device.At(), state) && state.vertexShader == nullptr, "no vertex program: the fixed pipeline");

        Block d3d(WXL_TYPE_SIZE("CGxDeviceD3d"));
        d3d.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_api"), hd::kApiD3d9);
        d3d.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_context"), 1);
        d3d.Put<void*>(WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentVertexDecl"), &decl);
        d3d.Put<void*>(WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentIndexBuf"), &index);
        d3d.Put<uint32_t>(WXL_FIELD("CGxDeviceD3d", "m_d3dVertexStreamOfs") + 4, 96);
        Check(hd::ReadState(d3d.At(), state) && state.indices == &index && state.offsets[1] == 96,
              "and a D3D9 device's at its own offsets");
        d3d.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_intF5C"), 1);
        Check(!hd::ReadState(d3d.At(), state), "a lost device has nothing bound");
        d3d.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_intF5C"), 0);
        d3d.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_api"), 0);
        Check(!hd::ReadState(d3d.At(), state) && !hd::ReadState(nullptr, state), "nor an OpenGL one, nor none");
        d3d.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_api"), hd::kApiD3d9);
        d3d.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_context"), 0);
        Check(hd::ReadBindings(d3d.At(), state) && state.indices == &index,
              "what the engine believes bound is read whatever the device's state, to be put back");

        Check(!hd::IntoTexture(device.At()), "the device draws to the screen");
        device.Put<void*>(WXL_FIELD("CGxDevice", "m_textureTarget[0].m_apiSpecific"), &decl);
        Check(hd::IntoTexture(device.At()), "or into a texture (a portrait)");

        // The engine's shadow: FLT_MAX where nothing was set this frame.
        std::vector<uint8_t> shadow(hd::kShadowSize, 0);
        float* rows = reinterpret_cast<float*>(shadow.data());
        for (uint32_t i = 0; i < hd::kRows * 4; ++i) rows[i] = FLT_MAX;
        rows[4 * 31] = 1.0f;
        Check(!hd::RowSet(rows) && hd::RowSet(rows + 4 * 31), "a register set this frame is told from one left unset");
    }

    // --- wxl.highlight on x86: the passes, over a device that records them -----------------------

    /// A Direct3D 9 device that records what is set on it and hands out made-up objects.
    struct RecordingDevice final : wxl::core::gx::d3d9::Device
    {
        D3DCAPS9            caps{};
        char                objects[64]{};          ///< the addresses of what it makes
        int                 made = 0, released = 0;
        IDirect3DSurface9*  scene = reinterpret_cast<IDirect3DSurface9*>(&objects[62]);
        IDirect3DSurface9*  depth = reinterpret_cast<IDirect3DSurface9*>(&objects[63]);
        D3DMULTISAMPLE_TYPE samples = D3DMULTISAMPLE_NONE;
        IDirect3DSurface9*  targets[2]{};
        IDirect3DSurface9*  boundDepth = nullptr;
        IDirect3DBaseTexture9* textures[3]{};
        std::map<int, DWORD> states;
        IDirect3DVertexShader9*      vs = nullptr;
        IDirect3DPixelShader9*       ps = nullptr;
        IDirect3DVertexDeclaration9* declaration = nullptr;
        IDirect3DVertexBuffer9*      streams[8]{};
        IDirect3DIndexBuffer9*       indices = nullptr;
        float                        vertexConstants[256][4]{};
        float                        pixelConstants[256][4]{};
        DWORD                        programVersion = 0xFFFE0300u;
        struct Drawn
        {
            IDirect3DSurface9* target0, *target1, *depth;
            IDirect3DVertexShader9* vs;
            IDirect3DPixelShader9*  ps;
            bool   indexed;
            INT    baseVertex;
            UINT   startIndex, primitives;
            DWORD  zEnable, zWrite, blend;
            float  bone;  ///< c31.x as the draw saw it
            float  color; ///< the mask's red, c200.x or c30.x
            float  alpha; ///< the mask's alpha reference, c202.x or c29.x
            IDirect3DBaseTexture9* texture0;
        };
        std::vector<Drawn> draws;
        std::vector<std::pair<IDirect3DSurface9*, IDirect3DSurface9*>> resolves;

        RecordingDevice()
        {
            caps.PixelShaderVersion               = D3DPS_VERSION(3, 0);
            caps.VertexShaderVersion              = D3DVS_VERSION(3, 0);
            caps.NumSimultaneousRTs               = 4;
            caps.MaxPixelShader30InstructionSlots = 32768;
            targets[0]                            = scene;
            boundDepth                            = depth;
        }
        template <class T>
        T* Make()
        {
            return reinterpret_cast<T*>(&objects[made++ % 60]);
        }
        HRESULT Caps(D3DCAPS9& out) override { out = caps; return S_OK; }
        HRESULT GetRenderTarget(DWORD i, IDirect3DSurface9** out) override { *out = targets[i]; return S_OK; }
        HRESULT SetRenderTarget(DWORD i, IDirect3DSurface9* s) override { targets[i] = s; return S_OK; }
        HRESULT GetDepthStencil(IDirect3DSurface9** out) override { *out = boundDepth; return S_OK; }
        HRESULT SetDepthStencil(IDirect3DSurface9* s) override { boundDepth = s; return S_OK; }
        HRESULT SurfaceDesc(IDirect3DSurface9* s, D3DSURFACE_DESC& out) override
        {
            out                 = D3DSURFACE_DESC{};
            out.Width           = 1600;
            out.Height          = 900;
            out.MultiSampleType = s == depth ? samples : D3DMULTISAMPLE_NONE;
            return S_OK;
        }
        HRESULT TextureSurface(IDirect3DTexture9*, IDirect3DSurface9** out) override { *out = Make<IDirect3DSurface9>(); return S_OK; }
        HRESULT Clear(DWORD, D3DCOLOR) override { return S_OK; }
        HRESULT StretchRect(IDirect3DSurface9* from, IDirect3DSurface9* to) override
        {
            resolves.emplace_back(from, to);
            return S_OK;
        }
        HRESULT CreateRenderTarget(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, IDirect3DSurface9** out) override
        {
            *out = Make<IDirect3DSurface9>();
            return S_OK;
        }
        HRESULT CreateTargetTexture(UINT, UINT, D3DFORMAT, IDirect3DTexture9** out) override { *out = Make<IDirect3DTexture9>(); return S_OK; }
        HRESULT CreateVertexShader(const DWORD* code, IDirect3DVertexShader9** out) override
        {
            *out = code && code[0] == 0xFFFE0300u ? Make<IDirect3DVertexShader9>() : nullptr;
            return *out ? S_OK : E_FAIL;
        }
        HRESULT CreatePixelShader(const DWORD* code, IDirect3DPixelShader9** out) override
        {
            *out = code && (code[0] == 0xFFFF0300u || code[0] == 0xFFFF0200u) ? Make<IDirect3DPixelShader9>() : nullptr;
            return *out ? S_OK : E_FAIL;
        }
        HRESULT CreateDeclaration(const D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9** out) override
        {
            *out = Make<IDirect3DVertexDeclaration9>();
            return S_OK;
        }
        void  Release(IUnknown* object) override { released += object ? 1 : 0; }
        DWORD VertexShaderVersion(IDirect3DVertexShader9*) override { return programVersion; }
        void  SetViewport(const D3DVIEWPORT9&) override {}
        void  SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override { states[state] = value; }
        void  SetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD) override {}
        void  SetTexture(DWORD i, IDirect3DBaseTexture9* t) override
        {
            if (i < 3) textures[i] = t;
        }
        void SetDeclaration(IDirect3DVertexDeclaration9* d) override { declaration = d; }
        void SetStreamSource(UINT i, IDirect3DVertexBuffer9* b, UINT, UINT) override { streams[i] = b; }
        void SetIndices(IDirect3DIndexBuffer9* i) override { indices = i; }
        void SetVertexShader(IDirect3DVertexShader9* s) override { vs = s; }
        void SetPixelShader(IDirect3DPixelShader9* s) override { ps = s; }
        void SetVertexConstants(UINT first, const float* values, UINT count) override
        {
            std::memcpy(vertexConstants[first], values, count * 16);
        }
        void SetPixelConstants(UINT first, const float* values, UINT count) override
        {
            std::memcpy(pixelConstants[first], values, count * 16);
        }
        DWORD State(D3DRENDERSTATETYPE s) { return states.count(s) ? states[s] : 0xDEADu; }
        void  Record(bool indexed, INT baseVertex, UINT startIndex, UINT primitives)
        {
            const bool model3 = programVersion >= 0xFFFE0300u;
            draws.push_back(Drawn{targets[0], targets[1], boundDepth, vs, ps, indexed, baseVertex, startIndex, primitives,
                                  State(D3DRS_ZENABLE), State(D3DRS_ZWRITEENABLE), State(D3DRS_ALPHABLENDENABLE),
                                  vertexConstants[31][0], model3 ? pixelConstants[200][0] : pixelConstants[30][0],
                                  model3 ? pixelConstants[202][0] : pixelConstants[29][0], textures[0]});
        }
        void DrawIndexed(D3DPRIMITIVETYPE, INT baseVertex, UINT, UINT, UINT startIndex, UINT primitives) override
        {
            Record(true, baseVertex, startIndex, primitives);
        }
        void DrawUp(D3DPRIMITIVETYPE, UINT primitives, const void*, UINT) override { Record(false, 0, 0, primitives); }
    };
#endif

    // --- wxl.highlight: the highlights mods set, and on x86 the passes that draw them -------------

    void CheckHighlight()
    {
        const wxl_highlight_api& api = wxl::core::services::HighlightApi();
        FakeObject boar;
        boar.guid   = 0x791;
        boar.typeId = WXL_OBJECT_UNIT;
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 100u);
        const wxl_handle target = wxl::core::objects::HandleOf(&boar);

        // The client's flag of an event: up while a mod listens or while the runtime needs the seam.
        {
            namespace listening = wxl::core::listening;
            const wxl_event event   = WXL_EV_ModelBatchDrawn;
            const bool      mods    = g_host->Listening(event);
            listening::ModsListen(event, false);
            listening::RuntimeWants(event, true);
            const bool runtimeAlone = wxl_core_listening[event] != 0;
            listening::ModsListen(event, true);
            listening::RuntimeWants(event, false);
            const bool modsAlone = wxl_core_listening[event] != 0;
            listening::ModsListen(event, false);
            Check(runtimeAlone && modsAlone && wxl_core_listening[event] == 0,
                  "the client's flag of an event is up while a mod listens or the runtime needs its seam, down when neither does");
            listening::ModsListen(event, mods);
        }
        const bool modsListen = g_host->Listening(WXL_EV_ModelBatchDrawn);

#if !defined(_M_IX86)
        const wxl_str why = api.unsupported_reason();
        Check(api.supported() == WXL_FALSE && std::string_view(why.data, why.size).find("Vulkan") != std::string_view::npos,
              "the recompiled client on Direct3D 9 draws no highlight, and says why: its Vulkan renderer draws them");
        Check(api.set(target, 0xFF0000u, 2.0f, 8.0f, 0.5f) == WXL_E_UNSUPPORTED && api.clear(target) == WXL_E_NOT_FOUND &&
                  api.drawn() == 0,
              "a highlight is refused as unsupported, and nothing is drawn");
        int world = 0;
        wxl::seam::WorldFrame::RenderWorld::Before(&world);
        wxl::seam::WorldFrame::RenderWorld::After(&world);
        Check(api.drawn() == 0, "a world pass draws none");

        // The service's path on the Vulkan renderer, here a wxl.render without a device and no
        // engine renderer: the drawing itself is checked on lavapipe (core/tests/vk/render_tests.cpp).
        static wxl::vk::render::Render                    vkRender;
        static wxl::core::services::highlight::VulkanTarget vkTarget{&vkRender, nullptr};
        wxl::core::services::highlight::UseVulkanTarget(&vkTarget);
        Check(api.supported() == WXL_TRUE && wxl_core_listening[WXL_EV_ModelBatchDrawn] == (modsListen ? 1 : 0),
              "with a Vulkan renderer highlights are supported; nothing is highlighted yet");
        Check(api.set(target, 0xFFE03020u, 4.0f, 8.0f, 0.5f) == WXL_OK && wxl_core_listening[WXL_EV_ModelBatchDrawn] != 0,
              "a highlight set, the client calls M2.DrawBatch even with no mod listening to ModelBatchDrawn");
        namespace hs = wxl::core::services::highlight;
        wxl::seam::WorldFrame::RenderWorld::Before(&world);
        wxl::seam::WorldFrame::RenderWorld::After(&world);
        Check(!hs::LastPass().resolved && hs::LastPass().refusal.find("M2.DrawBatch") != std::string::npos && api.drawn() == 0,
              "a world pass in which no model batch reached the runtime says so");
        int vkModel = 0, other = 0;
        auto vkBatch = [&](void* root, uint32_t blend) {
            g_gxDraw                 = wxl_model_batch_args{};
            g_gxDraw.root_model      = root;
            g_gxDraw.primitive_type  = 4;
            g_gxDraw.primitive_count = 40;
            g_gxDraw.blend_mode      = blend;
            g_gxDrew                 = true;
            wxl::seam::M2::DrawBatch::After(&vkModel);
        };
        wxl::seam::WorldFrame::RenderWorld::Before(&world);
        vkBatch(&g_charModel, 0);
        vkBatch(&other, 0);
        vkBatch(&g_charModel, 2);
        wxl::seam::WorldFrame::RenderWorld::After(&world);
        const hs::PassCounts& pass = hs::LastPass();
        Check(pass.resolved && pass.entries == 1 && pass.roots == 1 && pass.models == 3 && pass.seen == 2 && pass.kept == 0 &&
                  pass.drawn == 0 && !pass.composited,
              "the pass counts the object's model found, the batches drawn, the object's two, and none kept");
        Check(pass.refusal.find("kept no record") != std::string::npos && api.drawn() == 0,
              "a batch the renderer kept no record of is refused with its reason, and drawn() says none was drawn");
        hs::Present(); // the summary's second begins; its line comes a second later
        hs::Present();
        Check(api.clear(target) == WXL_OK && wxl_core_listening[WXL_EV_ModelBatchDrawn] == (modsListen ? 1 : 0),
              "the last highlight cleared, the client's flag is the mods' again");
        hs::UseVulkanTarget(nullptr);
        hs::Clear();
#else
        namespace hl = wxl::core::gx::d3d9::highlight;
        namespace hd = wxl::core::binding::x86::highlight_draw;

        // The engine the renderer reads: a D3D9 device with the batch's bindings and its vertex
        // program, and the shadow of the vertex constants with a bone in c31.
        static Block engine(WXL_TYPE_SIZE("CGxDeviceD3d"));
        static Block shader(WXL_TYPE_SIZE("CGxShader"));
        static Block states(WXL_TYPE_SIZE("CGxStateBom") * 100);
        static int   decl = 0, index = 0, stream = 0, program = 0;
        engine.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_api"), hd::kApiD3d9);
        engine.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_context"), 1);
        engine.Put<void*>(WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentVertexDecl"), &decl);
        engine.Put<void*>(WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentIndexBuf"), &index);
        engine.Put<void*>(WXL_FIELD("CGxDeviceD3d", "m_d3dVertexStreamBuf"), &stream);
        engine.Put<uint32_t>(WXL_FIELD("CGxDevice", "m_hwRenderStates.m_count"), 100);
        engine.Put<void*>(WXL_FIELD("CGxDevice", "m_hwRenderStates.m_data"), states.At());
        states.Put<void*>(hd::kRsVertexShader * WXL_TYPE_SIZE("CGxStateBom"), shader.At());
        shader.Put<void*>(WXL_FIELD("CGxShader", "m_apiSpecific"), &program);
        static std::vector<uint8_t> shadow(hd::kShadowSize, 0);
        float* rows = reinterpret_cast<float*>(shadow.data());
        for (uint32_t i = 0; i < hd::kRows * 4; ++i) rows[i] = FLT_MAX;
        for (uint32_t i = 28; i < 34; ++i) rows[4 * i] = static_cast<float>(i);
        static int32_t shaders = 1;
        static Block   texture(WXL_TYPE_SIZE("CGxTex"));
        static int     d3dTexture = 0;
        texture.Put<void*>(WXL_FIELD("CGxTex", "m_apiSpecificData"), &d3dTexture);
        states.Put<void*>(hd::kRsTexture0 * WXL_TYPE_SIZE("CGxStateBom"), texture.At());

        Check(api.set(target, 0xFF0000u, 2.0f, 8.0f, 0.5f) == WXL_E_UNSUPPORTED && api.supported() == WXL_FALSE,
              "with no device to draw on, a highlight is refused as unsupported");

        RecordingDevice device;
        hl::Source      source;
        source.device        = [] { return static_cast<const void*>(engine.At()); };
        source.vertexShadow  = shadow.data();
        source.enableShaders = &shaders;
        hl::SetSource(source);
        hl::UseDevice(&device);
        Check(api.supported() == WXL_TRUE && api.unsupported_reason().size == 0, "a device of shader model 3 draws them");
        Check(api.set(0xDEAD, 0xFF0000u, 2.0f, 8.0f, 0.5f) == WXL_E_NOT_FOUND &&
                  api.set(target, 0xFF0000u, -1.0f, 0.0f, 0.0f) == WXL_E_INVALID &&
                  api.set(target, 0xFF0000u, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f) == WXL_E_INVALID,
              "an object gone is not found; a width below 0 or no number is refused");
        Check(api.set(target, 0xFFE03020u, 40.0f, 8.0f, 0.5f) == WXL_OK, "an object is highlighted");
        Check(wxl_core_listening[WXL_EV_ModelBatchDrawn] != 0, "and the seam that keeps its batches is wanted, no mod listening or not");

        // A world pass: one batch of the object's model and one of another, then the world's end.
        int world = 0, render = 0, other = 0;
        auto drawBatch = [&](void* root, uint32_t blend) {
            g_gxDraw                 = wxl_model_batch_args{};
            g_gxDraw.root_model      = root;
            g_gxDraw.primitive_type  = 4;
            g_gxDraw.base_vertex     = 12;
            g_gxDraw.start_index     = 300;
            g_gxDraw.primitive_count = 40;
            g_gxDraw.blend_mode      = blend;
            g_gxDraw.viewport_width  = 1600;
            g_gxDraw.viewport_height = 900;
            g_gxDraw.viewport_max_z  = 1.0f;
            g_gxDrew                 = true;
            wxl::seam::M2::DrawBatch::After(&render);
        };
        // What the game has bound when the world ends, which the interface goes on drawing with:
        // the engine's copy says another declaration, index buffer and stream than the batch's, and
        // the device holds them; c0-c3 hold a matrix the engine set in an earlier frame (its copy
        // has FLT_MAX there), c28-c33 what it set this frame.
        static int laterDecl = 0, laterIndex = 0, laterStream = 0;
        auto gameBindings = [&] {
            engine.Put<void*>(WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentVertexDecl"), &laterDecl);
            engine.Put<void*>(WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentIndexBuf"), &laterIndex);
            engine.Put<void*>(WXL_FIELD("CGxDeviceD3d", "m_d3dVertexStreamBuf"), &laterStream);
            engine.Put<uint32_t>(WXL_FIELD("CGxDeviceD3d", "m_d3dVertexStreamStride"), 24);
            device.declaration = reinterpret_cast<IDirect3DVertexDeclaration9*>(&laterDecl);
            device.indices     = reinterpret_cast<IDirect3DIndexBuffer9*>(&laterIndex);
            device.streams[0]  = reinterpret_cast<IDirect3DVertexBuffer9*>(&laterStream);
            for (int r = 0; r < 4; ++r) device.vertexConstants[r][0] = 0.5f + static_cast<float>(r);
            for (uint32_t r = 28; r < 34; ++r) device.vertexConstants[r][0] = rows[4 * r];
            device.vertexConstants[250][0] = 0.0f;
        };
        auto bindingsBack = [&] {
            engine.Put<void*>(WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentVertexDecl"), &decl);
            engine.Put<void*>(WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentIndexBuf"), &index);
            engine.Put<void*>(WXL_FIELD("CGxDeviceD3d", "m_d3dVertexStreamBuf"), &stream);
        };
        const int invalidated = g_gxInvalidated;
        wxl::seam::WorldFrame::RenderWorld::Before(&world);
        drawBatch(&g_charModel, 0);
        drawBatch(&other, 0);
        drawBatch(&g_charModel, 2); // translucent: not the silhouette
        rows[4 * 31] = 999.0f;      // the engine moves on to other models' bones
        gameBindings();
        wxl::seam::WorldFrame::RenderWorld::After(&world);

        Check(api.drawn() == 1 && device.draws.size() == 4, "the object's one opaque batch is drawn again, then three passes");
        {
            const wxl::core::services::highlight::PassCounts& pass = wxl::core::services::highlight::LastPass();
            Check(pass.resolved && pass.roots == 1 && pass.models == 3 && pass.seen == 2 && pass.kept == 1 && pass.drawn == 1 &&
                      pass.composited && pass.refusal.empty(),
                  "the pass counts the batches drawn, the object's two, the opaque one kept and drawn, and the composite");
        }
        if (device.draws.size() == 4)
        {
            const RecordingDevice::Drawn& mask = device.draws[0];
            Check(mask.indexed && mask.baseVertex == 12 && mask.startIndex == 300 && mask.primitives == 40 &&
                      mask.vs == reinterpret_cast<IDirect3DVertexShader9*>(&program) && mask.bone == 31.0f,
                  "with the game's numbers, its vertex program and the constants it had at its draw");
            Check(mask.target0 != device.scene && mask.target1 != nullptr && mask.depth == device.depth &&
                      mask.zEnable == D3DZB_TRUE && mask.zWrite == FALSE && mask.blend == FALSE,
                  "into two masks, tested against the scene's depth and never writing it");
            Check(std::fabs(mask.color - 0xE0 / 255.0f) < 1e-5f, "in the highlight's colour");
            Check(mask.alpha < 0.0f && mask.texture0 == reinterpret_cast<IDirect3DBaseTexture9*>(&d3dTexture),
                  "an opaque batch keeps every texel of its first texture");
            Check(device.draws[1].target0 != device.scene && device.draws[2].target0 != device.scene &&
                      device.draws[1].depth == nullptr && !device.draws[1].indexed,
                  "the glow is blurred across and down, with no depth buffer");
            const RecordingDevice::Drawn& composite = device.draws[3];
            Check(composite.target0 == device.scene && composite.target1 == nullptr && composite.blend == TRUE &&
                      composite.zEnable == D3DZB_FALSE,
                  "and the outline and the glow are blended onto the scene");
            Check(std::fabs(device.pixelConstants[200][2] - WXL_HIGHLIGHT_MAX_OUTLINE * 900.0f / 1080.0f) < 1e-3f,
                  "an outline wider than 16 pixels is 16 at 1080 lines, scaled to the render's 900");
        }
        Check(device.declaration == reinterpret_cast<IDirect3DVertexDeclaration9*>(&laterDecl) &&
                  device.indices == reinterpret_cast<IDirect3DIndexBuffer9*>(&laterIndex) &&
                  device.streams[0] == reinterpret_cast<IDirect3DVertexBuffer9*>(&laterStream) && device.streams[1] == nullptr,
              "the declaration, index buffer and streams the engine believes bound are bound again (the interface "
              "draws with them without binding them anew)");
        bool constantsKept = device.vertexConstants[250][0] != 0.0f;
        for (int r = 0; r < 4; ++r) constantsKept = constantsKept && device.vertexConstants[r][0] == 0.5f + static_cast<float>(r);
        for (uint32_t r = 28; r < 34; ++r) constantsKept = constantsKept && device.vertexConstants[r][0] == rows[4 * r];
        Check(constantsKept, "the vertex registers the batches overwrote hold the engine's values again, those it set in an "
                             "earlier frame were never touched, the quad's own register is above the game's");
        Check(device.targets[0] == device.scene && device.targets[1] == nullptr && device.boundDepth == device.depth &&
                  device.textures[0] == nullptr && device.textures[2] == nullptr && device.State(D3DRS_ZENABLE) == D3DZB_TRUE &&
                  device.State(D3DRS_COLORWRITEENABLE1) == 0xF && g_gxInvalidated == static_cast<int>(invalidated) + 1,
              "the scene's target and depth are put back, the passes' textures let go, and the game told to send its state");
        Check(device.released == 2, "and the scene's target and depth are let go");
        Check(device.resolves.empty(), "a scene without multisampling needs no resolve");

        // Multisampling: masks of the depth's kind, resolved into the textures the passes read.
        device.samples = D3DMULTISAMPLE_4_SAMPLES;
        device.draws.clear();
        bindingsBack();
        states.Put<int32_t>(hd::kRsAlphaRef * WXL_TYPE_SIZE("CGxStateBom"), 224);
        wxl::seam::WorldFrame::RenderWorld::Before(&world);
        drawBatch(&g_charModel, 1);
        states.Put<int32_t>(hd::kRsAlphaRef * WXL_TYPE_SIZE("CGxStateBom"), 0);
        gameBindings();
        wxl::seam::WorldFrame::RenderWorld::After(&world);
        Check(device.resolves.size() == 2 && device.draws.size() == 4 && device.draws[0].target0 == device.resolves[0].first,
              "a multisampled depth gets multisampled masks, resolved after the batches");
        Check(device.draws.size() == 4 && std::fabs(device.draws[0].alpha - 224.0f / 255.0f) < 1e-5f,
              "an alpha-tested batch (hair, a mane, leaves) keeps only the texels its test keeps");

        // A vertex program of shader model 2 is paired with the mask program of model 2.
        static int program2 = 0;
        shader.Put<void*>(WXL_FIELD("CGxShader", "m_apiSpecific"), &program2);
        device.programVersion = 0xFFFE0200u;
        device.draws.clear();
        wxl::seam::WorldFrame::RenderWorld::Before(&world);
        drawBatch(&g_charModel, 0);
        wxl::seam::WorldFrame::RenderWorld::After(&world);
        Check(device.draws.size() == 4 && device.draws[0].ps != device.draws[3].ps &&
                  std::fabs(device.draws[0].color - 0xE0 / 255.0f) < 1e-5f,
              "a vertex program of model 2 is drawn with the mask of model 2, its colour in c30");
        device.programVersion = 0xFFFE0300u;
        shader.Put<void*>(WXL_FIELD("CGxShader", "m_apiSpecific"), &program);

        // Nothing highlighted in view: nothing is kept or drawn.
        device.draws.clear();
        wxl::seam::WorldFrame::RenderWorld::Before(&world);
        drawBatch(&other, 0);
        wxl::seam::WorldFrame::RenderWorld::After(&world);
        Check(device.draws.empty() && api.drawn() == 0, "a pass without the object in view draws nothing");

        // The device lost: what was made on it goes.
        const int before = device.released;
        wxl::seam::Gx::ReleaseD3dResources::Before(&device, 1);
        Check(device.released > before + 6, "the device lost takes the masks, the glow's targets and the programs with it");

        // Ownership and limits.
        Check(api.clear(target) == WXL_OK && api.clear(target) == WXL_E_NOT_FOUND, "a highlight is cleared once");
        api.set(target, 0x00FF00u, 2.0f, 0.0f, 0.0f);
        wxl::core::services::highlight::ForgetMod(0);
        Check(api.clear(target) == WXL_E_NOT_FOUND, "a mod's highlights go with it");
        api.set(target, 0x00FF00u, 2.0f, 0.0f, 0.0f);
        api.clear_all();
        Check(api.clear(target) == WXL_E_NOT_FOUND, "ClearAll takes off the calling mod's");

        // A device without shader model 3 says so.
        device.caps.PixelShaderVersion = D3DPS_VERSION(2, 0);
        hl::UseDevice(&device);
        const wxl_str why = api.unsupported_reason();
        Check(api.supported() == WXL_FALSE && std::string_view(why.data, why.size).find("shader model 3.0") != std::string_view::npos &&
                  api.set(target, 0xFF0000u, 2.0f, 0.0f, 0.0f) == WXL_E_UNSUPPORTED,
              "a device of shader model 2 draws none, and says why");
        shaders = 0;
        device.caps.PixelShaderVersion = D3DPS_VERSION(3, 0);
        hl::UseDevice(&device);
        Check(api.supported() == WXL_FALSE, "nor does a client drawing without its shaders");
        shaders = 1;

        wxl_result fromThread = WXL_OK;
        std::thread([&] { fromThread = api.set(target, 0xFF0000u, 2.0f, 0.0f, 0.0f); }).join();
        Check(fromThread == WXL_E_INVALID, "off the main thread a highlight is refused");
        wxl::core::services::highlight::Clear();
        Check(wxl_core_listening[WXL_EV_ModelBatchDrawn] == (modsListen ? 1 : 0), "no highlight left, the client's flag is the mods' again");
        hl::UseDevice(nullptr);
        hl::SetSource(hl::Source{});
#endif
        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
    }

    // --- wxl.draw: shapes kept and drawn, through a device that records them ----------------------

    /// A Direct3D 9 device that records wxl.draw's pass: its states, its constants, its vertices.
    struct DrawDevice final : wxl::core::gx::d3d9::Device
    {
        D3DCAPS9                              caps{};
        char                                  objects[8]{};
        int                                   made = 0, released = 0;
        std::map<int, DWORD>                  states;
        float                                 constants[256][4]{};
        D3DVIEWPORT9                          viewport{};
        IDirect3DVertexDeclaration9*          declaration = nullptr;
        std::vector<wxl::gx::shapes::Vertex>  vertices;
        struct Run
        {
            UINT  vertices;
            DWORD zEnable, colorWrite, blend;
            DWORD source = 0, destination = 0;
            const void* pixel = nullptr;
            const void* texture = nullptr;
            float       surface[6][4] = {};
            size_t      first = 0;
            DWORD       zFunc = 0;
            float       alpha = 1.0f; ///< c245.z: the run's opacity multiplier
        };
        std::vector<Run>             runs;
        const void*                  pixel = nullptr;
        const void*                  texture0 = nullptr;
        float                        pixelConstants[256][4]{};
        std::map<int, DWORD>         sampler0;

        DrawDevice()
        {
            caps.PixelShaderVersion  = D3DPS_VERSION(3, 0);
            caps.VertexShaderVersion = D3DVS_VERSION(3, 0);
            caps.DeclTypes           = D3DDTCAPS_UBYTE4;
            caps.MaxPrimitiveCount   = 0xFFFFF;
        }
        template <class T>
        T* Make()
        {
            ++made;
            return reinterpret_cast<T*>(&objects[made % 8]);
        }
        HRESULT Caps(D3DCAPS9& out) override { out = caps; return S_OK; }
        HRESULT GetRenderTarget(DWORD, IDirect3DSurface9** out) override { *out = nullptr; return S_OK; }
        HRESULT SetRenderTarget(DWORD, IDirect3DSurface9*) override { return S_OK; }
        HRESULT GetDepthStencil(IDirect3DSurface9** out) override { *out = nullptr; return S_OK; }
        HRESULT SetDepthStencil(IDirect3DSurface9*) override { return S_OK; }
        HRESULT SurfaceDesc(IDirect3DSurface9*, D3DSURFACE_DESC& out) override { out = D3DSURFACE_DESC{}; return S_OK; }
        HRESULT TextureSurface(IDirect3DTexture9*, IDirect3DSurface9** out) override { *out = nullptr; return S_OK; }
        HRESULT Clear(DWORD, D3DCOLOR) override { return S_OK; }
        HRESULT StretchRect(IDirect3DSurface9*, IDirect3DSurface9*) override { return S_OK; }
        HRESULT CreateRenderTarget(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, IDirect3DSurface9** out) override
        {
            *out = nullptr;
            return E_FAIL;
        }
        HRESULT CreateTargetTexture(UINT, UINT, D3DFORMAT, IDirect3DTexture9** out) override { *out = nullptr; return E_FAIL; }
        HRESULT CreateVertexShader(const DWORD* code, IDirect3DVertexShader9** out) override
        {
            *out = code && code[0] == 0xFFFE0300u ? Make<IDirect3DVertexShader9>() : nullptr;
            return *out ? S_OK : E_FAIL;
        }
        HRESULT CreatePixelShader(const DWORD* code, IDirect3DPixelShader9** out) override
        {
            *out = code && code[0] == 0xFFFF0300u ? Make<IDirect3DPixelShader9>() : nullptr;
            return *out ? S_OK : E_FAIL;
        }
        HRESULT CreateDeclaration(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** out) override
        {
            // Stride 32: position, the other end, the colour, the corner's bytes.
            const bool shape = elements && elements[0].Offset == 0 && elements[1].Offset == 12 && elements[2].Type == D3DDECLTYPE_D3DCOLOR &&
                               elements[3].Offset == 28 && elements[3].Type == D3DDECLTYPE_UBYTE4 && elements[4].Stream == 0xFF;
            *out = shape ? Make<IDirect3DVertexDeclaration9>() : nullptr;
            return *out ? S_OK : E_FAIL;
        }
        void  Release(IUnknown* object) override { released += object ? 1 : 0; }
        DWORD VertexShaderVersion(IDirect3DVertexShader9*) override { return 0xFFFE0300u; }
        void  SetViewport(const D3DVIEWPORT9& v) override { viewport = v; }
        void  SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override { states[state] = value; }
        void  SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE state, DWORD value) override
        {
            if (s == 0) sampler0[state] = value;
        }
        void  SetTexture(DWORD s, IDirect3DBaseTexture9* t) override
        {
            if (s == 0) texture0 = t;
        }
        void  SetDeclaration(IDirect3DVertexDeclaration9* d) override { declaration = d; }
        void  SetStreamSource(UINT, IDirect3DVertexBuffer9*, UINT, UINT) override {}
        void  SetIndices(IDirect3DIndexBuffer9*) override {}
        void  SetVertexShader(IDirect3DVertexShader9*) override {}
        void  SetPixelShader(IDirect3DPixelShader9* p) override { pixel = p; }
        void  SetVertexConstants(UINT first, const float* values, UINT count) override { std::memcpy(constants[first], values, count * 16); }
        void  SetPixelConstants(UINT first, const float* values, UINT count) override { std::memcpy(pixelConstants[first], values, count * 16); }
        DWORD State(D3DRENDERSTATETYPE s) { return states.count(s) ? states[s] : 0xDEADu; }
        void  DrawIndexed(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) override {}
        void  DrawUp(D3DPRIMITIVETYPE type, UINT primitives, const void* data, UINT stride) override
        {
            if (type != D3DPT_TRIANGLELIST || stride != sizeof(wxl::gx::shapes::Vertex)) return;
            const auto* v = static_cast<const wxl::gx::shapes::Vertex*>(data);
            Run run{primitives * 3, State(D3DRS_ZENABLE), State(D3DRS_COLORWRITEENABLE), State(D3DRS_ALPHABLENDENABLE),
                    State(D3DRS_SRCBLEND), State(D3DRS_DESTBLEND), pixel, texture0};
            std::memcpy(run.surface, pixelConstants[200], sizeof run.surface);
            run.zFunc = State(D3DRS_ZFUNC);
            run.alpha = constants[245][2];
            run.first = vertices.size();
            vertices.insert(vertices.end(), v, v + primitives * 3);
            runs.push_back(run);
        }
    };

    /// A flat ground at z 5 everywhere, every chunk loaded.
    struct FlatGround final : wxl::gx::shapes::Ground
    {
        float heights[145];
        FlatGround() { std::fill(std::begin(heights), std::end(heights), 5.0f); }
        bool Present() override { return true; }
        bool Chunk(int32_t, int32_t, const float*& h, uint16_t& holes) override
        {
            h     = heights;
            holes = 0;
            return true;
        }
    };

    void CheckDraw()
    {
        namespace draw = wxl::core::services::draw;
        namespace d9   = wxl::core::gx::d3d9::shapes;
        const wxl_draw_api& api = wxl::core::services::DrawApi();
        DrawDevice device;
        FlatGround ground;
        d9::UseDevice(&device);
        draw::UseGround(&ground);
        g_fakeWorldView = true;
        Check(api.supported() == WXL_TRUE && api.unsupported_reason().size == 0, "a device of shader model 3 draws shapes");

        wxl_draw_shape circle{};
        circle.struct_size = sizeof circle;
        circle.kind        = WXL_DRAW_CIRCLE;
        circle.center      = wxl_vec3{110.0f, 210.0f, 0.0f};
        circle.radius      = 3.0f;
        wxl_draw_style style{};
        style.struct_size = sizeof style;
        style.color       = 0xFF8000u;
        style.alpha       = 1.0f;
        style.fill_alpha  = 0.5f;
        style.width       = 2.0f;
        Check(api.draw(&circle, &style) == WXL_OK, "an immediate circle is taken");

        wxl_draw_shape box{};
        box.struct_size = sizeof box;
        box.kind        = WXL_DRAW_BOX;
        box.center      = wxl_vec3{120.0f, 200.0f, 2.0f};
        box.size        = wxl_vec3{2.0f, 2.0f, 2.0f};
        wxl_draw_style top = style;
        top.depth          = WXL_DRAW_DEPTH_ON_TOP;
        top.fill_alpha     = 0.0f;
        const wxl_handle kept = api.add(&box, &top, 0.0f);
        Check(kept != WXL_HANDLE_NONE, "a box is kept");

        const uint32_t invalidated = g_gxInvalidated;
        wxl::seam::WorldScene::RenderLiquidPass::After();
        wxl_draw_stats stats{};
        stats.struct_size = sizeof stats;
        Check(api.stats(&stats) == WXL_OK && stats.drawn == 2 && stats.immediate == 1 && stats.shapes == 1,
              "the world's liquids drawn: both shapes go onto it");
        Check(stats.vertices == device.vertices.size() && stats.vertices > 0, "every vertex the frame wrote reached the device");
        Check(device.runs.size() == 2 && device.runs[0].zEnable == D3DZB_TRUE && device.runs[1].zEnable == D3DZB_FALSE,
              "two runs: against the depth, then on top");
        Check(device.runs[0].colorWrite == 7u && device.runs[0].blend == TRUE, "blended, the scene's alpha left as it is");
        Check(device.State(D3DRS_ZENABLE) == D3DZB_TRUE && g_gxInvalidated == static_cast<int>(invalidated) + 1,
              "what the engine leaves set is put back, and its state sent again");
        Check(device.viewport.Width == 1600 && device.viewport.Height == 900 && device.viewport.MaxZ == 1.0f,
              "the viewport is the world's, from its box and the window");
        Check(std::fabs(device.constants[244][0] - 1600.0f) < 1e-3f && std::fabs(device.constants[244][2] - (900.0f / 1080.0f) / 16.0f) < 1e-6f &&
                  std::fabs(device.constants[240][0] - 2.0f / 1600.0f) < 1e-9f,
              "c240 to c245: the view's columns, the screen and the width's scale");
        bool laid = !device.vertices.empty(), fromEye = true;
        for (const auto& v : device.vertices)
            if (v.side == 1 && v.flags & 1) laid = laid && std::fabs(v.pos[2] - 5.0f) < 1e-4f;
        fromEye = std::fabs(device.vertices[0].pos[0] - 10.0f) < 3.5f && std::fabs(device.vertices[0].pos[1] - 10.0f) < 3.5f;
        Check(laid && fromEye, "the circle's fill lies on the ground, its vertices taken from the eye");
        uint32_t fills = 0, fillAlpha = 0;
        for (const auto& v : device.vertices)
            if (v.side == 1 && (v.color & 0xFFFFFFu) == 0xFF8000u)
            {
                ++fills;
                fillAlpha = v.color >> 24;
            }
        Check(fills > 0 && fillAlpha == 128, "the fill is drawn in the colour at its own opacity");

        // The immediate shape is gone after its frame; the kept one stays.
        device.vertices.clear();
        device.runs.clear();
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        Check(stats.immediate == 0 && stats.drawn == 1 && device.runs.size() == 1, "an immediate shape is drawn by one frame only");

        // Changing a kept shape.
        Check(api.move(kept, wxl_vec3{130.0f, 200.0f, 2.0f}, 0.5f) == WXL_OK && api.set_style(kept, &style) == WXL_OK,
              "a kept shape moves and takes a new look");
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        Check(stats.rebuilt == 1, "a moved shape is built again once");
        Check(api.set_duration(kept, 0.0f) == WXL_OK && api.update(kept, &circle) == WXL_OK, "it becomes a circle");
        api.remove(kept);
        Check(api.move(kept, circle.center, 0.0f) == WXL_E_NOT_FOUND, "a removed shape is not found");

        // Refusals.
        wxl_draw_shape bad = circle;
        bad.radius         = 0.0f;
        Check(api.draw(&bad, nullptr) == WXL_E_INVALID, "a circle of no radius is refused");
        bad.radius = 300.0f;
        Check(api.add(&bad, nullptr, 0.0f) == WXL_HANDLE_NONE, "a circle past the extent is not kept");
        wxl_draw_style unsized = style;
        unsized.struct_size    = 0;
        Check(api.draw(&circle, &unsized) == WXL_E_INVALID, "a style without its struct_size is refused");
        wxl_draw_style weird = style;
        weird.depth          = 7;
        Check(api.draw(&circle, &weird) == WXL_E_INVALID, "a depth that is none is refused");
        Check(api.add(&circle, nullptr, -1.0f) == WXL_HANDLE_NONE, "a negative duration is refused");
        uint32_t taken = 0;
        wxl_draw_shape small = circle;
        small.flags          = WXL_DRAW_FLAT;
        small.radius         = 0.5f;
        for (uint32_t i = 0; i < WXL_DRAW_MAX_IMMEDIATE + 1; ++i) taken += api.draw(&small, nullptr) == WXL_OK ? 1 : 0;
        Check(taken == WXL_DRAW_MAX_IMMEDIATE, "a frame takes WXL_DRAW_MAX_IMMEDIATE immediate shapes of a mod, no more");
        wxl::seam::WorldScene::RenderLiquidPass::After();
        Check(api.draw(&small, nullptr) == WXL_OK, "the next frame takes them again");

        // A duration runs out.
        const wxl_handle brief = api.add(&circle, &style, 0.001f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        wxl::seam::WorldScene::RenderLiquidPass::After();
        Check(api.set_style(brief, &style) == WXL_E_NOT_FOUND, "a shape whose duration ran out is gone");

        // Attached to an object: follows it, goes when it leaves view.
        FakeObject boar;
        boar.guid     = 0x7D1;
        boar.typeId   = WXL_OBJECT_UNIT;
        boar.position = wxl_vec3{140.0f, 220.0f, 5.0f};
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 100u);
        const wxl_handle target = wxl::core::objects::HandleOf(&boar);
        wxl_draw_shape   ring   = circle;
        ring.kind               = WXL_DRAW_RING;
        ring.center             = wxl_vec3{0.0f, 0.0f, 0.0f};
        ring.inner              = 2.0f;
        const wxl_handle mark   = api.add(&ring, &style, 0.0f);
        Check(api.attach(mark, target, WXL_DRAW_ATTACH_TURN) == WXL_OK, "a ring is attached to a unit");
        Check(api.attach(mark, target, 0x80) == WXL_E_INVALID, "an unknown flag is refused");
        api.attach(mark, target, WXL_DRAW_ATTACH_TURN);
        device.vertices.clear();
        wxl::seam::WorldScene::RenderLiquidPass::After();
        double cx = 0.0, cy = 0.0;
        uint32_t n = 0;
        for (const auto& v : device.vertices)
            if (v.side == 1)
            {
                cx += v.pos[0];
                cy += v.pos[1];
                ++n;
            }
        Check(n > 0 && std::fabs(cx / n - 40.0) < 0.5 && std::fabs(cy / n - 20.0) < 0.5, "it is drawn around the unit");
        boar.position.x += 3.0f;
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        Check(stats.rebuilt == 1, "it is built again where the unit went");
        Check(api.detach(mark) == WXL_OK && api.attach(mark, target, 0) == WXL_OK, "it is let go of, and attached again");
        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
        wxl::seam::WorldScene::RenderLiquidPass::After();
        Check(api.detach(mark) == WXL_E_NOT_FOUND, "the unit gone, its shape is gone");

        // The frame's budget: shapes past it are left out, counted.
        wxl_draw_shape ball{};
        ball.struct_size = sizeof ball;
        ball.kind        = WXL_DRAW_SPHERE;
        ball.center      = wxl_vec3{600.0f, 200.0f, 2.0f};
        ball.radius      = 200.0f;
        wxl_draw_style solid = style;
        solid.alpha          = 0.0f;
        solid.fill_alpha     = 0.3f;
        uint32_t balls = 0;
        while (balls < 200 && api.draw(&ball, &solid) == WXL_OK) ++balls;
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        std::printf("  %u spheres of 200 yd: %u drawn, %u left out, %u vertices, %.0f us\n", balls, stats.drawn, stats.left_out,
                    stats.vertices, stats.build_microseconds);
        Check(stats.left_out > 0 && stats.vertices > WXL_DRAW_MAX_FRAME_VERTICES / 8 && stats.vertices <= WXL_DRAW_MAX_FRAME_VERTICES,
              "past the frame's vertices, shapes are left out; the others drawn, their faces turned to the eye");
        ball.center = wxl_vec3{110.0f, 200.0f, 2.0f};
        device.vertices.clear();
        Check(api.draw(&ball, &solid) == WXL_OK, "a sphere around the eye is taken");
        wxl::seam::WorldScene::RenderLiquidPass::After();
        Check(!device.vertices.empty(), "from inside a volume, its inside is drawn");

        // Ownership: a mod's shapes go with it; off the main thread nothing is taken.
        const wxl_handle mine = api.add(&circle, &style, 0.0f);
        draw::ForgetMod(0);
        Check(api.remove != nullptr && api.set_duration(mine, 1.0f) == WXL_E_NOT_FOUND, "a mod's shapes go with it");
        wxl_result fromThread = WXL_OK;
        std::thread([&] { fromThread = api.draw(&circle, &style); }).join();
        Check(fromThread == WXL_E_INVALID, "off the main thread a shape is refused");

        // abi 0.1.38: a textured inside multiplied into the ground, feathered; its texture opened
        // once, readied on the device each frame, let go of after the shapes that used it.
        draw::Clear();
        device.vertices.clear();
        device.runs.clear();
        const size_t   openedBefore = g_openedTextures.size();
        const int      closedBefore = g_texturesClosed;
        wxl_draw_style stain        = style;
        stain.alpha          = 0.0f;
        stain.fill_alpha     = 0.8f;
        stain.texture        = "SPELLS\\CrackedGround.blp";
        stain.blend          = WXL_DRAW_BLEND_MODULATE;
        stain.uv             = WXL_DRAW_UV_WORLD;
        stain.uv_size_x      = 4.0f;
        stain.feather        = 0.5f;
        stain.flags          = WXL_DRAW_STYLE_FILL_COLOR;
        stain.fill_color     = 0x806040u;
        Check(api.draw(&circle, &stain) == WXL_OK, "a textured circle is taken");
        const wxl_handle kept2 = api.add(&circle, &stain, 0.0f);
        Check(kept2 != WXL_HANDLE_NONE && g_openedTextures.size() == openedBefore + 1, "its texture is opened once for both shapes");
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        Check(stats.textured == 2 && stats.textures == 1 && stats.textures_loading == 0, "both insides drawn as surfaces of one texture");
        const DrawDevice::Run* surfaceRun = nullptr;
        for (const auto& run : device.runs)
            if (run.texture == g_fakeD3dTexture) surfaceRun = &run;
        Check(surfaceRun && surfaceRun->source == D3DBLEND_DESTCOLOR && surfaceRun->destination == D3DBLEND_ZERO,
              "MODULATE multiplies the scene (DESTCOLOR, ZERO) with the device's texture on sampler 0");
        Check(surfaceRun && surfaceRun->surface[3][3] == 0.5f && surfaceRun->surface[5][1] == 1.0f && surfaceRun->surface[2][2] == 1.0f &&
                  surfaceRun->surface[2][3] == 1.0f && surfaceRun->surface[3][1] == 3.0f,
              "c200 to c205 carry its feather, its blend, its repeat and its radius");
        Check(device.sampler0[D3DSAMP_ADDRESSU] == D3DTADDRESS_WRAP && device.sampler0[D3DSAMP_MIPFILTER] == D3DTEXF_LINEAR,
              "WORLD repeats the picture, mipmapped");
        Check(device.texture0 == nullptr, "the texture is unbound after the shapes");
        bool inPlane = surfaceRun != nullptr, tinted = surfaceRun != nullptr;
        for (size_t i = 0; surfaceRun && i < surfaceRun->vertices; ++i)
        {
            const auto& v = device.vertices[surfaceRun->first + i];
            inPlane = inPlane && v.other[0] * v.other[0] + v.other[1] * v.other[1] <= 9.0f + 1e-3f && (v.side & 3) == 1;
            tinted  = tinted && (v.color & 0xFFFFFFu) == 0x806040u && (v.color >> 24) == 204u;
        }
        Check(inPlane && tinted, "its corners carry the circle's plane, in the fill's own colour and opacity");
        // A texture still loading: the inside waits, the lines do not.
        wxl_draw_style loading = stain;
        loading.texture        = "Textures\\Loading.blp";
        loading.alpha          = 1.0f;
        device.runs.clear();
        Check(api.draw(&circle, &loading) == WXL_OK, "a texture not yet loaded is taken");
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        Check(stats.textures == 2 && stats.textured == 1 && stats.drawn == 2, "its inside is left out of the frame, its outline drawn");
        // Let go of: the immediate one at once, the kept one when removed; closed a while later.
        api.remove(kept2);
        wxl::seam::Gx::ScenePresent::Before();
        Check(g_texturesClosed == closedBefore, "a texture lingers a few seconds after its last shape");
        // Refusals of abi 0.1.38's fields.
        wxl_draw_style badStyle = stain;
        badStyle.blend          = 7;
        Check(api.draw(&circle, &badStyle) == WXL_E_INVALID, "a blend that is none is refused");
        badStyle         = stain;
        badStyle.texture = "Textures\\Missing.blp";
        Check(api.draw(&circle, &badStyle) == WXL_E_INVALID, "a texture the client makes none of is refused");
        badStyle             = stain;
        badStyle.radial_fade = 2.0f;
        Check(api.draw(&circle, &badStyle) == WXL_E_INVALID, "a radial fade past 1 is refused");
        // The sizes of abi 0.1.36 are still taken, as the look they had.
        wxl_draw_style old = style;
        old.struct_size    = WXL_DRAW_STYLE_SIZE_0136;
        old.texture        = "Textures\\Missing.blp"; // past its size: not read
        wxl_draw_shape oldShape = circle;
        oldShape.struct_size    = WXL_DRAW_SHAPE_SIZE_0136;
        Check(api.draw(&oldShape, &old) == WXL_OK, "a shape and a style of abi 0.1.36 are taken, the fields past them not read");

        // Lines along, soft, shadowed: one more copy of the segments, under the lines, moved.
        wxl::seam::WorldScene::RenderLiquidPass::After();
        device.vertices.clear();
        device.runs.clear();
        const std::vector<wxl_vec3> path = {{100, 200, 0}, {110, 200, 0}, {110, 210, 0}};
        const std::vector<float>    fades = {0.0f, 1.0f, 1.0f};
        wxl_draw_shape              line{};
        line.struct_size  = sizeof line;
        line.kind         = WXL_DRAW_LINE;
        line.points       = path.data();
        line.point_count  = 3;
        line.point_alphas = fades.data();
        wxl_draw_style gold = style;
        gold.color           = 0xE0B040u;
        gold.softness        = 1.0f;
        gold.shadow_alpha    = 0.5f;
        gold.shadow_x        = 1.5f;
        gold.shadow_y        = 1.5f;
        gold.shadow_softness = 1.0f;
        Check(api.draw(&line, &gold) == WXL_OK, "a soft gold line with a shadow is taken");
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        size_t shadows = 0, lines = 0, faded = 0;
        for (const auto& v : device.vertices)
        {
            const bool moved = (v.side >> 2) == 3 && (v.end >> 1) == 3;
            shadows += moved && (v.color & 0xFFFFFFu) == 0 ? 1 : 0;
            lines += !moved && (v.color & 0xFFFFFFu) == 0xE0B040u && (v.flags >> 2) == 4 ? 1 : 0;
            faded += (v.color & 0xFFFFFFu) == 0xE0B040u && (v.color >> 24) == 0 ? 1 : 0;
        }
        Check(stats.shadows == 1 && shadows == 12 && lines == 12, "its shadow is its segments once more, 1.5 pixels right and down, black");
        Check(faded == 3, "the line starts transparent at its first point");
        Check(!device.vertices.empty() && (device.vertices[0].color & 0xFFFFFFu) == 0, "the shadow is drawn before the line");

        // abi 0.1.39: a textured box seen through the ground, its faces each a surface; the same
        // immediate shape given again is not built again; abi 0.1.38's style still taken.
        wxl::seam::WorldScene::RenderLiquidPass::After();
        device.vertices.clear();
        device.runs.clear();
        wxl_draw_shape crate{};
        crate.struct_size = sizeof crate;
        crate.kind        = WXL_DRAW_BOX;
        crate.center      = wxl_vec3{112.0f, 204.0f, 4.0f};
        crate.size        = wxl_vec3{2.0f, 2.0f, 4.0f};
        wxl_draw_style xray = stain;
        xray.alpha          = 1.0f;
        xray.blend          = WXL_DRAW_BLEND_ALPHA;
        xray.uv             = WXL_DRAW_UV_TILE;
        xray.uv_offset_x    = 0.25f;
        xray.occluded_alpha = 0.35f;
        Check(api.draw(&crate, &xray) == WXL_OK && api.draw(&crate, &xray) == WXL_OK, "a textured box seen through the ground, twice");
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        Check(stats.occluded == 2 && stats.textured == 2, "both drawn where the scene hides them too, their faces as surfaces");
        size_t faceRuns = 0, behindRuns = 0, behindFaces = 0;
        bool   order = true;
        for (size_t i = 0; i < device.runs.size(); ++i)
        {
            const auto& run = device.runs[i];
            const bool  face = run.texture == g_fakeD3dTexture && (device.vertices[run.first].side & 3) == 1; // a surface: textured triangles
            faceRuns += face && run.zFunc == D3DCMP_LESSEQUAL ? 1 : 0;
            if (run.zFunc == D3DCMP_GREATER)
            {
                ++behindRuns;
                behindFaces += face ? 1 : 0;
                order = order && std::fabs(run.alpha - 0.35f) < 1e-6f;
            }
            else
                order = order && run.alpha == 1.0f;
        }
        Check(faceRuns >= 2 && faceRuns <= 6 && behindFaces == faceRuns, "a box shows at most three faces each, every one drawn behind the scene too");
        Check(behindRuns > 0 && order, "the hidden part at 0.35 of its opacity (GREATER), the seen part as usual");
        Check(device.State(D3DRS_ZFUNC) == D3DCMP_LESSEQUAL, "the depth test is put back");
        bool facePlane = false;
        for (const auto& run : device.runs)
            if (run.texture == g_fakeD3dTexture && run.vertices && (device.vertices[run.first].side & 3) == 1)
            {
                const auto& v = device.vertices[run.first];
                facePlane = std::fabs(v.other[0]) <= 2.0f + 1e-3f && std::fabs(v.other[1]) <= 1.0f + 1e-3f && (v.side & 3) == 1 &&
                            run.surface[0][2] != 0.0f && run.surface[3][0] == 1.0f;
            }
        Check(facePlane, "a face's corners carry its frame, its numbers its offset and its box");
        // The same immediate shapes again: not built again; a moved one is.
        Check(api.draw(&crate, &xray) == WXL_OK && api.draw(&circle, &xray) == WXL_OK, "the box again, and a circle where the other box was");
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        Check(stats.reused == 1, "a shape given as the frame before is drawn without being built again");
        wxl_draw_style older = xray;
        older.struct_size    = WXL_DRAW_STYLE_SIZE_0138;
        Check(api.draw(&crate, &older) == WXL_OK, "a style of abi 0.1.38 is taken, occluded_alpha not read");
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        Check(stats.occluded == 0 && stats.textured == 1, "and draws only what is seen");
        wxl_draw_style badXray = xray;
        badXray.occluded_alpha = 1.5f;
        Check(api.draw(&crate, &badXray) == WXL_E_INVALID, "an occluded opacity past 1 is refused");

        // abi 0.1.40: particles. A burst thrown and forgotten, a kept emitter at a rate, both
        // simulated each frame and drawn in a run of their own; budgets, refusals, ownership.
        wxl::seam::WorldScene::RenderLiquidPass::After();
        device.vertices.clear();
        device.runs.clear();
        wxl_draw_emitter clods{};
        clods.struct_size = sizeof clods;
        clods.spawn       = WXL_DRAW_SPAWN_POINT;
        clods.center      = wxl_vec3{110.0f, 210.0f, 5.5f};
        clods.burst       = 20;
        clods.spread      = 0.6f;
        clods.speed_min   = 3.0f;
        clods.speed_max   = 5.0f;
        clods.life_min    = clods.life_max = 0.5f;
        clods.size_min    = clods.size_max = 0.2f;
        clods.gravity     = 19.29f;
        clods.ground      = WXL_DRAW_GROUND_BOUNCE;
        clods.restitution = 0.4f;
        wxl_draw_particles earth{};
        earth.struct_size = sizeof earth;
        earth.texture     = "SPELLS\\CrackedGround.blp";
        earth.flags       = WXL_DRAW_PARTICLES_MASK;
        earth.softness    = 0.2f;
        earth.key_count   = 2;
        earth.keys[0]     = wxl_draw_particle_key{0.0f, 1.0f, 0x806040u, 1.0f};
        earth.keys[1]     = wxl_draw_particle_key{1.0f, 0.5f, 0x806040u, 0.0f};
        Check(api.emit(&clods, &earth) == WXL_OK, "a burst of clods is thrown");
        wxl_draw_emitter puff = clods;
        puff.burst            = 0;
        puff.rate             = 120.0f;
        puff.ground           = WXL_DRAW_GROUND_NONE;
        puff.gravity          = 0.0f;
        wxl_draw_particles dust{};
        dust.struct_size    = sizeof dust;
        dust.blend          = WXL_DRAW_BLEND_ADD;
        dust.occluded_alpha = 0.3f;
        const wxl_handle fountain = api.add_emitter(&puff, &dust);
        Check(fountain != WXL_HANDLE_NONE, "a kept emitter at a rate");
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        Check(stats.particles == 20 && stats.emitters == 2 && stats.particles_culled == 0, "the burst alive, both emitters held");
        size_t particleRuns = 0, particleBehind = 0, sixes = 0;
        for (const auto& run : device.runs)
            if (run.vertices && device.vertices[run.first].side == 1 && run.vertices % 6 == 0 && run.surface[2][3] == 1.0f && run.surface[5][2] == 1.0f)
            {
                ++particleRuns;
                sixes += run.vertices / 6;
            }
        Check(particleRuns == 1 && sixes == 20 && stats.particle_draws >= 1, "the clods drawn as one run of masked pictures, six corners each");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        device.runs.clear();
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        for (const auto& run : device.runs) particleBehind += run.zFunc == D3DCMP_GREATER && run.alpha > 0.29f && run.alpha < 0.31f ? 1 : 0;
        Check(stats.particles > 20 && stats.particles_emitted > 0 && particleBehind == 1, "the rate throws more, and its dust is drawn behind the scene too");
        Check(api.burst(fountain, 10) == WXL_OK && api.place(fountain, wxl_vec3{120.0f, 200.0f, 6.0f}, 0.0f) == WXL_OK &&
                  api.update_emitter(fountain, &puff) == WXL_OK && api.set_emitter_style(fountain, &earth) == WXL_OK,
              "Burst, Place, UpdateEmitter and SetEmitterStyle reach the emitter");
        // Their lives run out: the thrown burst goes with them; the kept one stays until stopped.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        for (int i = 0; i < 8; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wxl::seam::WorldScene::RenderLiquidPass::After();
        }
        api.stats(&stats);
        Check(stats.emitters == 1, "a thrown burst goes once its particles have lived");
        api.stop(fountain);
        Check(api.burst(fountain, 1) == WXL_E_NOT_FOUND, "a stopped emitter is not found");
        for (int i = 0; i < 8; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wxl::seam::WorldScene::RenderLiquidPass::After();
        }
        api.stats(&stats);
        Check(stats.emitters == 0 && stats.particles == 0, "its particles live out their lives, then it goes");
        // Budgets and refusals.
        wxl_draw_emitter big = clods;
        big.burst            = WXL_DRAW_MAX_BURST + 1;
        Check(api.emit(&big, nullptr) == WXL_E_INVALID, "a burst past WXL_DRAW_MAX_BURST is refused");
        big.burst = 0;
        Check(api.emit(&big, nullptr) == WXL_E_INVALID, "a burst of nothing is refused");
        big.burst    = 10;
        big.life_min = 0.0f;
        Check(api.emit(&big, nullptr) == WXL_E_INVALID, "a life of 0 is refused");
        wxl_draw_particles badLook = earth;
        badLook.key_count          = 2;
        badLook.keys[1].t          = -1.0f;
        Check(api.emit(&clods, &badLook) == WXL_E_INVALID, "keys out of order are refused");
        big          = clods;
        big.burst    = WXL_DRAW_MAX_BURST;
        big.life_min = big.life_max = 30.0f;
        for (int i = 0; i < 5; ++i) api.emit(&big, nullptr);
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        Check(stats.particles == WXL_DRAW_MAX_MOD_PARTICLES && stats.particles_culled == 5 * WXL_DRAW_MAX_BURST - WXL_DRAW_MAX_MOD_PARTICLES,
              "past a mod's particles, the rest are not thrown and are counted");
        draw::ForgetMod(0);
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        Check(stats.particles == 0 && stats.emitters == 0, "a mod's emitters and particles go with it");

        // Beams: a quad strip turned to the eye each frame; soft or textured, a surface.
        device.vertices.clear();
        device.runs.clear();
        const std::vector<wxl_vec3> ends = {{108.0f, 205.0f, 5.0f}, {110.0f, 207.0f, 15.0f}};
        wxl_draw_shape ray{};
        ray.struct_size = sizeof ray;
        ray.kind        = WXL_DRAW_BEAM;
        ray.points      = ends.data();
        ray.point_count = 2;
        ray.size        = wxl_vec3{1.5f, 0.5f, 0.0f};
        wxl_draw_style light = style;
        light.alpha          = 0.0f;
        light.fill_alpha     = 0.8f;
        light.feather        = 1.0f;
        light.blend          = WXL_DRAW_BLEND_ADD;
        light.occluded_alpha = 0.25f;
        const wxl_handle kept3 = api.add(&ray, &light, 0.0f);
        Check(kept3 != WXL_HANDLE_NONE && api.draw(&ray, &light) == WXL_OK, "a soft beam, kept and immediate");
        wxl::seam::WorldScene::RenderLiquidPass::After();
        api.stats(&stats);
        size_t beamRuns = 0, beamBehind = 0;
        for (const auto& run : device.runs)
            if (run.vertices == 48 && run.surface[4][2] == 1.0f && run.surface[3][3] == 1.0f)
            {
                beamRuns += run.zFunc == D3DCMP_LESSEQUAL ? 1 : 0;
                beamBehind += run.zFunc == D3DCMP_GREATER ? 1 : 0;
            }
        Check(beamRuns == 2 && beamBehind == 2 && stats.textured == 2, "each a surface of 48 corners, soft to its sides, drawn behind the scene too");
        wxl_draw_shape oneEnd = ray;
        oneEnd.point_count    = 1;
        Check(api.draw(&oneEnd, &light) == WXL_E_INVALID, "a beam of one point is refused");
        api.remove(kept3);

        // A device without shader model 3 says so.
        device.caps.VertexShaderVersion = D3DVS_VERSION(2, 0);
        d9::UseDevice(&device);
        const wxl_str why = api.unsupported_reason();
        Check(api.supported() == WXL_FALSE && std::string_view(why.data, why.size).find("shader model 3.0") != std::string_view::npos &&
                  api.draw(&circle, &style) == WXL_E_UNSUPPORTED,
              "a device of shader model 2 draws none, and says why");

        draw::Clear();
        d9::UseDevice(nullptr);
        draw::UseGround(nullptr);
        g_fakeWorldView = false;
    }
}

// Core.cpp's flag per event, which the harness keeps as the runtime does: a hot seam's body reads
// it before anything else.
extern "C" WXL_CORE_API volatile uint8_t wxl_core_listening[WXL_EV_COUNT] = {};

int main(int argc, char** argv)
{
    // Unbuffered, so the last line printed before a crash is never held back in a buffer.
    setvbuf(stdout, nullptr, _IONBF, 0);


    wxl::core::binding::Bind(FakeInit());

    wxl::host::HostConfig config;
    config.runtime_id   = "harness";
    config.runtime_name = "seam harness";
    config.events       = wxl::core::seams::RaisedEvents();
    config.services.push_back(wxl::host::PublishedService{
        WXL_SCRIPT_SERVICE, WXL_SCRIPT_SERVICE_VERSION, &wxl::core::services::ScriptApi()});
    g_host              = std::make_unique<wxl::host::ModHost>(std::move(config));
    g_host->SetListenHandler([](void*, wxl_event event, bool listening) { wxl::core::listening::ModsListen(event, listening); }, nullptr);

    printf("seam harness: %zu seam(s), %zu raisable event(s), %zu binding row(s) left unbound by the fake\n",
           wxl::core::seams::Table().size(), wxl::core::seams::RaisedEvents().size(),
           wxl::core::binding::Unbound().size());

    CheckStages();
    CheckInitRows();
    CheckEveryRaisedEventIsSubscribable();
    CheckEveryRowDeclaresItsAbi();
    CheckEventDispatchUpdate();
    CheckWorld();
    CheckObjectMgr();
    CheckRevivedObjects();
    CheckFields();
    CheckUnit();
    CheckPlayer();
    CheckGameUI();
    CheckTypedReads();
    CheckActions();
    CheckMapArea();
    CheckListening();
    CheckAssets();
    CheckAssetEvents();
    CheckDoodads();
    CheckFileOpened();
    CheckObjectUpdate();
    CheckCode();
    CheckFrameScript();
    CheckNetClient();
    CheckNetOpcodes();
    CheckNetConnection();
    CheckClientError();
    CheckClientDestroy();
    CheckInput();
    CheckInputService();
    CheckCamera();
    CheckWorldTrace();
    CheckMouseLookBlock();
    CheckPose();
    CheckAnimation();
    CheckSelectionCircle();
    CheckTexture();
    CheckHighlight();
    CheckImGuiService();
    CheckStorage();
    CheckSound();
    CheckStorageService();
    CheckAssetPipeline();
    CheckAssetBricks();
    CheckDataService();
    CheckRender();
    CheckSpell();
    CheckSpellRequest();
    CheckSpellProgress();
    CheckSpellCooldown();
    CheckSpellCancel();
    CheckCombat();
    CheckUiEvent();
    CheckChatAddon();
    CheckScriptError();
    CheckWorldZoneMouseoverClick();
    CheckConsoleVar();
    CheckCVarService();
    CheckTerrainService();
    CheckTerrainLayers();
    CheckTerrainLayerExtras();
    CheckTerrainLayerLimit();
    CheckSplitTiles();
    CheckTerrainExtraLayers();
    CheckShaderCode();
    CheckDraw();
    CheckInterfaceSignature();
    CheckPlayerMoveMoneyLevel();
    CheckGlue();
    CheckChat();
    CheckEnergize();
    CheckFootprint();
    CheckArchivesAndDatabases();
    CheckGameplayAuras();
    CheckGameplayLoot();
    CheckGameplayItems();
    CheckGameplayQuests();
    CheckGameplayGroup();
    CheckGameplaySocial();
#if defined(WXL_CORE_PROFILE) && WXL_CORE_PROFILE
    CheckProfile();
#endif
    CheckModelBatch();
    CheckRenderService();
    CheckAppearance();
    CheckM2Service();
    CheckAnimateGates();
    CheckDrops();
    CheckM2World();
    CheckModelStages();
    CheckX64Only();
#if defined(_M_IX86)
    CheckThiscallShim();
    CheckGxAdapterX86();
    CheckCallX86();
    CheckModelCallsX86();
    CheckModelDrawX86();
    CheckHighlightDrawX86();
    CheckOneSidedShim();
    CheckThiscallGate();
    CheckGetUpdateObjectDetour();
    CheckMirrorDetours();
    CheckRegparmThunks();
    CheckChatDetours();
    CheckStormDetours();
    CheckAssetDetours();
    CheckFindFileDetour();
    CheckValidatorDetour();
    CheckGameplayDetours();
    CheckInterfaceSignatureDetour();
#endif
    CheckEngineService();
    CheckPointTable();
    CheckPointDispatch();
    CheckEveryPoint();
#if defined(_M_IX86)
    CheckPointPrologues();
    CheckPointDetours();
    CheckPointSides();
    CheckDeinlined();
#endif

    printf("\n%d failure(s)\n", g_failures);
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--bench") == 0) Bench();
#if defined(WXL_CORE_PROFILE) && WXL_CORE_PROFILE
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--profile-live") == 0) ProfileLive(false);
        if (strcmp(argv[i], "--profile-live-any") == 0) ProfileLive(true);
    }
#endif

    g_host.reset();
    return g_failures ? 1 : 0;
}
