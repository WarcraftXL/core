// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.terrain: the map's ground read and reshaped.
//
// The ground is the loaded chunks' own heights (MCVT) and normals (MCNR), inside each tile's file
// buffer, which every reader takes live: the drawing fills its vertex buffers from them, and the
// collision facets, the ray tests and the height query (CMapChunk::Intersect, CMap::GetHeightTerrain)
// read them at each call. An edit writes them in place, then has the client make again what it
// derived from them (binding terrain_chunk_rebuild) and forgets the movement's cached facets.
//
// Edits are kept by chunk (its column and row on the map), per vertex, each with its owner (mod
// and layer) and a serial: the newest shows. A chunk's original heights and normals are taken the
// first time the service writes into it and kept until the map unloads, so that a reset writes
// them back and a chunk read again from the disk gets its edits again. A vertex's normal is the
// file's own while no vertex sharing a triangle with it is edited, and the service's otherwise
// (wxl/terrain/grid.hpp, VertexNormal), so untouched ground keeps the map's normals exactly.

#include "services/TerrainService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/mod_host.hpp"
#include "wxl/terrain/grid.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    using namespace wxl::core;
    namespace grid = wxl::terrain;

    constexpr const char* kTag = "terrain";
    constexpr uint32_t    kVertices = grid::kVertexCount;

    /// Movement flags under which the player is not standing on the ground on its own: on a
    /// transport, without gravity, rooted, falling, swimming, flying, hovering.
    constexpr uint32_t kNotStanding = 0x200u | 0x400u | 0x800u | 0x1000u | 0x2000u | 0x200000u | 0x2000000u | 0x40000000u;
    /// The movement flags of a move the player is making (forward to pitch).
    constexpr uint32_t kMoving = 0xFFu;

    using Owner = uint64_t; ///< the mod's token in the high half, its layer in the low

    struct Edit
    {
        Owner    owner;
        uint64_t serial;
        float    height;
    };

    /// What the service keeps of a chunk.
    struct Chunk
    {
        bool               hasOriginal = false;
        float              original[kVertices]{};            ///< absolute heights
        int8_t             originalNormals[kVertices * 3]{};
        std::vector<Edit>  edits[kVertices];
        uint32_t           edited = 0;                        ///< vertices with at least one edit
    };

    /// A loaded chunk, as the binding shows it.
    struct Live
    {
        void*                  chunk = nullptr;
        wxl_core_terrain_chunk view{};
    };

    std::unordered_map<uint32_t, std::unique_ptr<Chunk>> g_chunks;
    uint64_t g_serial = 0;
    bool     g_inWorld = false;
    std::vector<uint32_t> g_deferredClutter; ///< chunks reshaped while the world was drawn
    wxl_terrain_stats     g_stats{};
    uint64_t              g_generation = 1; ///< grows each time the ground may have changed (wxl.draw drapes on it)

    uint32_t Key(int32_t cx, int32_t cy) { return static_cast<uint32_t>(cy) * grid::kChunksPerSide + static_cast<uint32_t>(cx); }
    int32_t  KeyX(uint32_t key) { return static_cast<int32_t>(key % grid::kChunksPerSide); }
    int32_t  KeyY(uint32_t key) { return static_cast<int32_t>(key / grid::kChunksPerSide); }

    wxl::host::ModHost* HostOf() { return Host(); }

    uint32_t Caller()
    {
        const wxl::host::ModHost* host = HostOf();
        return host ? host->CurrentMod() : 0u;
    }

    /// A refusal, logged once per mod and call.
    void Refuse(const char* call, const char* format, ...)
    {
        static std::set<std::pair<uint32_t, std::string>> said;
        const uint32_t mod = Caller();
        if (!said.insert({mod, call}).second) return;
        char    text[512];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof text, format, args);
        va_end(args);
        const wxl::host::ModHost* host = HostOf();
        const std::string         who  = host && mod && mod <= host->Loaded().size() ? host->Loaded()[mod - 1].id
                                                                                       : std::string("a mod");
        WLOG_WARN(kTag, "wxl.terrain.%s from %s was refused: %s", call, who.c_str(), text);
    }

    bool OnMainThread(const char* call)
    {
        const wxl::host::ModHost* host = HostOf();
        if (!host || host->OnMainThread()) return true;
        Refuse(call, "it was called off the main thread, where the client keeps its ground");
        return false;
    }

    Chunk* Find(uint32_t key)
    {
        const auto it = g_chunks.find(key);
        return it == g_chunks.end() ? nullptr : it->second.get();
    }

    Chunk& Ensure(uint32_t key)
    {
        std::unique_ptr<Chunk>& slot = g_chunks[key];
        if (!slot) slot = std::make_unique<Chunk>();
        return *slot;
    }

    bool HasEdit(int32_t cx, int32_t cy, uint32_t index)
    {
        const Chunk* chunk = Find(Key(cx, cy));
        return chunk && !chunk->edits[index].empty();
    }

    /// The newest edit of a vertex, null when it has none.
    const Edit* Newest(const Chunk& chunk, uint32_t index)
    {
        const Edit* newest = nullptr;
        for (const Edit& edit : chunk.edits[index])
            if (!newest || edit.serial > newest->serial) newest = &edit;
        return newest;
    }

    bool LiveOf(int32_t cx, int32_t cy, Live& out)
    {
        if (!grid::ChunkOnMap(cx, cy)) return false;
        out.chunk = binding::terrain_chunk(cx, cy);
        if (!out.chunk) return false;
        return binding::terrain_chunk_view(out.chunk, &out.view) == WXL_TRUE;
    }

    void TakeOriginal(Chunk& chunk, const Live& live)
    {
        if (chunk.hasOriginal) return;
        for (uint32_t i = 0; i < kVertices; ++i) chunk.original[i] = live.view.heights[i] + live.view.base_z;
        std::memcpy(chunk.originalNormals, live.view.normals, sizeof chunk.originalNormals);
        chunk.hasOriginal = true;
    }

    /// The absolute heights of a loaded chunk: as they are, or its original ones.
    void Heights(const Live& live, const Chunk* chunk, bool original, float out[kVertices])
    {
        if (original && chunk && chunk->hasOriginal)
        {
            std::memcpy(out, chunk->original, sizeof(float) * kVertices);
            return;
        }
        for (uint32_t i = 0; i < kVertices; ++i) out[i] = live.view.heights[i] + live.view.base_z;
    }

    /// One reshaping: the loaded chunks it touched, cached, and what changed in each.
    struct Pass
    {
        std::unordered_map<uint32_t, Live> live;
        std::unordered_set<uint32_t>       absent;
        std::unordered_set<uint32_t>       heights;  ///< chunks whose heights changed
        std::unordered_set<uint32_t>       normals;  ///< chunks whose normals changed
        std::unordered_set<uint64_t>       targets;  ///< vertices whose normal is made again
        uint32_t                           written = 0;

        Live* Get(int32_t cx, int32_t cy)
        {
            if (!grid::ChunkOnMap(cx, cy)) return nullptr;
            const uint32_t key = Key(cx, cy);
            const auto     it  = live.find(key);
            if (it != live.end()) return &it->second;
            if (absent.count(key)) return nullptr;
            Live entry;
            if (!LiveOf(cx, cy, entry))
            {
                absent.insert(key);
                return nullptr;
            }
            return &live.emplace(key, entry).first->second;
        }

        void Target(int32_t cx, int32_t cy, uint32_t index)
        {
            grid::ForNormalNeighbours(cx, cy, index, [&](int32_t x, int32_t y, uint32_t i) {
                targets.insert((static_cast<uint64_t>(Key(x, y)) << 8) | i);
            });
        }

        /// Writes a loaded chunk's heights as its edits and originals say; each vertex changed
        /// marks the normals around it.
        void WriteHeights(uint32_t key)
        {
            Chunk* chunk = Find(key);
            Live*  at    = Get(KeyX(key), KeyY(key));
            if (!chunk || !at) return;
            TakeOriginal(*chunk, *at);
            for (uint32_t i = 0; i < kVertices; ++i)
            {
                const Edit* edit   = Newest(*chunk, i);
                const float target = (edit ? edit->height : chunk->original[i]) - at->view.base_z;
                if (at->view.heights[i] == target) continue;
                at->view.heights[i] = target;
                ++g_generation;
                heights.insert(key);
                ++written;
                Target(KeyX(key), KeyY(key), i);
            }
        }

        /// Makes again the normal of every target vertex in a loaded chunk.
        void WriteNormals()
        {
            for (const uint64_t target : targets)
            {
                const uint32_t key   = static_cast<uint32_t>(target >> 8);
                const uint32_t index = static_cast<uint32_t>(target & 0xFFu);
                const int32_t  cx = KeyX(key), cy = KeyY(key);
                Live*          at = Get(cx, cy);
                if (!at) continue;
                bool near = false;
                grid::ForNormalNeighbours(cx, cy, index, [&](int32_t x, int32_t y, uint32_t i) {
                    if (!near && HasEdit(x, y, i)) near = true;
                });
                int8_t packed[3];
                if (near)
                {
                    Chunk& chunk = Ensure(key);
                    TakeOriginal(chunk, *at);
                    const grid::Point3 n = grid::VertexNormal(cx, cy, index, [&](int32_t x, int32_t y, uint32_t i, float& z) {
                        const Live* other = Get(x, y);
                        if (!other) return false;
                        z = other->view.heights[i] + other->view.base_z;
                        return true;
                    });
                    grid::PackNormal(n, packed);
                }
                else
                {
                    const Chunk* chunk = Find(key);
                    if (!chunk || !chunk->hasOriginal) continue;
                    std::memcpy(packed, chunk->originalNormals + 3 * index, 3);
                }
                int8_t* to = at->view.normals + 3 * index;
                if (std::memcmp(to, packed, 3) == 0) continue;
                std::memcpy(to, packed, 3);
                normals.insert(key);
            }
        }

        /// Has the client make again what it derives from the chunks that changed.
        void Rebuild()
        {
            std::unordered_set<uint32_t> all = normals;
            all.insert(heights.begin(), heights.end());
            for (const uint32_t key : all)
            {
                Live* at = Get(KeyX(key), KeyY(key));
                if (!at) continue;
                const bool reshaped = heights.count(key) != 0;
                // The ground clutter is freed outside the world's drawing only: the frame being
                // drawn may hold it in its sort list.
                binding::terrain_chunk_rebuild(at->chunk, reshaped && !g_inWorld ? WXL_TRUE : WXL_FALSE);
                if (reshaped && g_inWorld &&
                    std::find(g_deferredClutter.begin(), g_deferredClutter.end(), key) == g_deferredClutter.end())
                    g_deferredClutter.push_back(key);
            }
            if (!heights.empty()) binding::terrain_forget_collision();
        }
    };

    /// The ground's height at a point from what is loaded; false where there is none.
    bool GroundAt(float x, float y, bool original, float& out)
    {
        const grid::Locus at = grid::Locate(x, y);
        if (!at.on_map) return false;
        Live live;
        if (!LiveOf(at.chunk_x, at.chunk_y, live)) return false;
        if (grid::CellIsHole(static_cast<uint16_t>(live.view.holes), at.cell_row, at.cell_col)) return false;
        float heights[kVertices];
        Heights(live, Find(Key(at.chunk_x, at.chunk_y)), original, heights);
        out = grid::HeightInCell(at.chunk_x, at.chunk_y, heights, at.cell_row, at.cell_col, x, y);
        return true;
    }

    /// Where the player stands before a reshaping, to tell after it whether the ground left them.
    struct Stand
    {
        const void* unit = nullptr;
        wxl_vec3    position{};
        float       ground = 0.0f;
        bool        onGround = false;
    };

    Stand Before()
    {
        Stand stand;
        stand.unit = binding::local_player_object();
        if (!stand.unit || binding::object_position(stand.unit, &stand.position) != WXL_TRUE) return stand;
        const uint32_t flags = binding::unit_movement_flags(stand.unit);
        if ((flags & (kNotStanding | kMoving)) != 0) return stand;
        // Standing on the terrain itself, not on a building or a rock above it.
        stand.onGround = GroundAt(stand.position.x, stand.position.y, false, stand.ground) &&
                         std::fabs(stand.position.z - stand.ground) < 0.5f;
        return stand;
    }

    /// A player standing still on ground that went down falls, as off a transport that left; a
    /// moving one meets the new ground at its next step.
    void After(const Stand& stand)
    {
        if (!stand.onGround) return;
        float ground = 0.0f;
        const bool there = GroundAt(stand.position.x, stand.position.y, false, ground);
        if (there && ground >= stand.position.z - 0.05f) return;
        binding::unit_fall_local(stand.unit);
    }

    /// The vertices a chunk's loading may change the normals of: its own, and those of its
    /// neighbours' edges, when an edit lies within one chunk of it.
    bool EditsNear(int32_t cx, int32_t cy)
    {
        for (int32_t dy = -1; dy <= 1; ++dy)
            for (int32_t dx = -1; dx <= 1; ++dx)
            {
                const Chunk* chunk = grid::ChunkOnMap(cx + dx, cy + dy) ? Find(Key(cx + dx, cy + dy)) : nullptr;
                if (chunk && chunk->edited) return true;
            }
        return false;
    }

    void CountStats()
    {
        uint32_t chunks = 0, vertices = 0;
        for (const auto& [key, chunk] : g_chunks)
        {
            (void)key;
            if (!chunk->edited) continue;
            ++chunks;
            vertices += chunk->edited;
        }
        g_stats.edited_chunks   = chunks;
        g_stats.edited_vertices = vertices;
    }

    /// Takes edits off where `drop` says, then writes back what shows instead. The chunks given
    /// are the ones to look at; the others are left alone.
    template <class Drop>
    bool TakeOff(const std::vector<uint32_t>& keys, Drop&& drop)
    {
        std::vector<uint32_t> touched;
        for (const uint32_t key : keys)
        {
            Chunk* chunk = Find(key);
            if (!chunk) continue;
            bool changed = false;
            for (uint32_t i = 0; i < kVertices; ++i)
            {
                std::vector<Edit>& list   = chunk->edits[i];
                const size_t       before = list.size();
                list.erase(std::remove_if(list.begin(), list.end(), [&](const Edit& e) { return drop(key, i, e); }), list.end());
                if (list.size() == before) continue;
                changed = true;
                if (list.empty()) --chunk->edited;
            }
            if (changed) touched.push_back(key);
        }
        if (touched.empty()) return false;
        const Stand stand = Before();
        Pass        pass;
        for (const uint32_t key : touched) pass.WriteHeights(key);
        pass.WriteNormals();
        pass.Rebuild();
        After(stand);
        CountStats();
        return true;
    }

    // --- the table ------------------------------------------------------------------------

    wxl_bool WXL_CALL TerrainHeight(double x, double y, wxl_bool original, double* out)
    {
        if (!OnMainThread("height") || !out || !std::isfinite(x) || !std::isfinite(y)) return WXL_FALSE;
        if (binding::terrain_present() != WXL_TRUE) return WXL_FALSE;
        float z = 0.0f;
        if (!GroundAt(static_cast<float>(x), static_cast<float>(y), original == WXL_TRUE, z)) return WXL_FALSE;
        *out = z;
        return WXL_TRUE;
    }

    uint32_t WXL_CALL TerrainPresence(double x, double y)
    {
        if (!OnMainThread("presence") || !std::isfinite(x) || !std::isfinite(y)) return WXL_TERRAIN_NONE;
        if (binding::terrain_present() != WXL_TRUE) return WXL_TERRAIN_NONE;
        const grid::Locus at = grid::Locate(static_cast<float>(x), static_cast<float>(y));
        if (!at.on_map) return WXL_TERRAIN_NONE;
        Live live;
        if (!LiveOf(at.chunk_x, at.chunk_y, live)) return WXL_TERRAIN_NOT_LOADED;
        if (grid::CellIsHole(static_cast<uint16_t>(live.view.holes), at.cell_row, at.cell_col)) return WXL_TERRAIN_HOLE;
        return WXL_TERRAIN_GROUND;
    }

    wxl_bool WXL_CALL TerrainTrace(wxl_vec3 start, wxl_vec3 finish, wxl_terrain_hit* out)
    {
        if (!OnMainThread("trace")) return WXL_FALSE;
        if (!out || out->struct_size < offsetof(wxl_terrain_hit, fraction) + sizeof(float))
        {
            Refuse("trace", "the answer's struct_size is not set: set it to sizeof(wxl_terrain_hit)");
            return WXL_FALSE;
        }
        if (!std::isfinite(start.x) || !std::isfinite(start.y) || !std::isfinite(start.z) || !std::isfinite(finish.x) ||
            !std::isfinite(finish.y) || !std::isfinite(finish.z))
            return WXL_FALSE;
        if (binding::terrain_present() != WXL_TRUE) return WXL_FALSE;

        const grid::Point3 from{start.x, start.y, start.z};
        const grid::Point3 delta{finish.x - start.x, finish.y - start.y, finish.z - start.z};
        Pass               cache;
        float              best = -1.0f;
        grid::Point3       normal{};
        int32_t            hitX = 0, hitY = 0;
        const int64_t      last = static_cast<int64_t>(grid::kChunksPerSide) * grid::kCellsPerChunk;
        grid::WalkCells(grid::Point2{start.x, start.y}, grid::Point2{finish.x, finish.y}, 16384, [&](int64_t R, int64_t C) {
            if (R < 0 || C < 0 || R >= last || C >= last) return true;
            const int32_t cx = static_cast<int32_t>(C / grid::kCellsPerChunk), cy = static_cast<int32_t>(R / grid::kCellsPerChunk);
            const int32_t row = static_cast<int32_t>(R % grid::kCellsPerChunk), col = static_cast<int32_t>(C % grid::kCellsPerChunk);
            const Live*   at  = cache.Get(cx, cy);
            if (!at || grid::CellIsHole(static_cast<uint16_t>(at->view.holes), row, col)) return true;
            // In the chunk's own coordinates, from its corner, as the client tests its triangles.
            const grid::Point2 corner = grid::ChunkCorner(cx, cy);
            const grid::Point3 local{from.x - corner.x, from.y - corner.y, from.z};
            for (int32_t tri = 0; tri < 4; ++tri)
            {
                uint32_t idx[3];
                grid::CellTriangle(row, col, tri, idx);
                grid::Point3 v[3];
                for (int k = 0; k < 3; ++k)
                {
                    const grid::Point2 p = grid::VertexLocal(idx[k]);
                    v[k]                 = grid::Point3{p.x, p.y, at->view.heights[idx[k]] + at->view.base_z};
                }
                const float t = grid::SegmentTriangle(local, delta, v[0], v[1], v[2]);
                if (t < 0.0f || (best >= 0.0f && t >= best)) continue;
                best   = t;
                normal = grid::Unit(grid::UpNormal(v[0], v[1], v[2]));
                hitX   = cx;
                hitY   = cy;
            }
            // Cells come in the segment's order: the first that is met holds the nearest point.
            return best < 0.0f;
        });
        if (best < 0.0f) return WXL_FALSE;
        out->point    = wxl_vec3{start.x + delta.x * best, start.y + delta.y * best, start.z + delta.z * best};
        out->normal   = wxl_vec3{normal.x, normal.y, normal.z};
        out->chunk_x  = hitX;
        out->chunk_y  = hitY;
        out->fraction = best;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL TerrainLocate(double x, double y, wxl_terrain_place* out)
    {
        if (!OnMainThread("locate")) return WXL_FALSE;
        if (!out || out->struct_size < sizeof(wxl_terrain_place))
        {
            Refuse("locate", "the answer's struct_size is not set: set it to sizeof(wxl_terrain_place)");
            return WXL_FALSE;
        }
        if (!std::isfinite(x) || !std::isfinite(y)) return WXL_FALSE;
        const grid::Locus at = grid::Locate(static_cast<float>(x), static_cast<float>(y));
        if (!at.on_map) return WXL_FALSE;
        out->chunk_x  = at.chunk_x;
        out->chunk_y  = at.chunk_y;
        out->cell_row = at.cell_row;
        out->cell_col = at.cell_col;
        out->nearest  = at.nearest;
        out->loaded   = binding::terrain_present() == WXL_TRUE && binding::terrain_chunk(at.chunk_x, at.chunk_y) ? WXL_TRUE
                                                                                                                : WXL_FALSE;
        return WXL_TRUE;
    }

    uint32_t WXL_CALL TerrainVertices(int32_t chunk_x, int32_t chunk_y, wxl_bool original, wxl_vec3* out, uint32_t capacity)
    {
        if (!OnMainThread("vertices") || !grid::ChunkOnMap(chunk_x, chunk_y)) return 0;
        if (binding::terrain_present() != WXL_TRUE) return 0;
        Live live;
        if (!LiveOf(chunk_x, chunk_y, live)) return 0;
        float heights[kVertices];
        Heights(live, Find(Key(chunk_x, chunk_y)), original == WXL_TRUE, heights);
        for (uint32_t i = 0; out && i < kVertices && i < capacity; ++i)
        {
            const grid::Point2 p = grid::VertexPlane(chunk_x, chunk_y, i);
            out[i]               = wxl_vec3{p.x, p.y, heights[i]};
        }
        return kVertices;
    }

    uint32_t WXL_CALL TerrainVerticesIn(double x, double y, double radius, wxl_terrain_vertex* out, uint32_t capacity)
    {
        if (!OnMainThread("vertices_in") || !std::isfinite(x) || !std::isfinite(y)) return 0;
        if (!(radius >= 0.0 && radius <= WXL_TERRAIN_RADIUS_LIMIT))
        {
            Refuse("vertices_in", "the radius %.2f is outside 0 to %.0f yards", radius, static_cast<double>(WXL_TERRAIN_RADIUS_LIMIT));
            return 0;
        }
        if (binding::terrain_present() != WXL_TRUE) return 0;
        Pass     cache;
        uint32_t count = 0;
        grid::ForVerticesInCircle(static_cast<float>(x), static_cast<float>(y), static_cast<float>(radius),
                                  [&](int32_t cx, int32_t cy, uint32_t index, grid::Point2 place) {
            if (out && count < capacity)
            {
                wxl_terrain_vertex& v = out[count];
                v                     = wxl_terrain_vertex{};
                v.chunk_x             = cx;
                v.chunk_y             = cy;
                v.index               = index;
                v.x                   = place.x;
                v.y                   = place.y;
                v.inner               = grid::IsInner(index) ? WXL_TRUE : WXL_FALSE;
                // An edge vertex is read from any loaded chunk holding it: the same ground in each.
                grid::ForCopies(cx, cy, index, [&](int32_t qx, int32_t qy, uint32_t qi) {
                    if (v.loaded == WXL_TRUE) return;
                    const Live* at = cache.Get(qx, qy);
                    if (!at) return;
                    v.loaded             = WXL_TRUE;
                    v.z                  = at->view.heights[qi] + at->view.base_z;
                    const Chunk* state   = Find(Key(qx, qy));
                    v.original_z         = state && state->hasOriginal ? state->original[qi] : v.z;
                });
            }
            ++count;
        });
        return count;
    }

    wxl_result WXL_CALL TerrainSetHeights(const wxl_terrain_height* heights, uint32_t count, uint32_t layer)
    {
        const auto begun = std::chrono::steady_clock::now();
        if (!OnMainThread("set_heights")) return WXL_E_INVALID;
        if (count && !heights)
        {
            Refuse("set_heights", "%u entries were announced and none given", count);
            return WXL_E_INVALID;
        }
        for (uint32_t n = 0; n < count; ++n)
        {
            const wxl_terrain_height& e = heights[n];
            if (!grid::ChunkOnMap(e.chunk_x, e.chunk_y) || e.index >= kVertices)
            {
                Refuse("set_heights", "entry %u names chunk (%d, %d) vertex %u; chunks go 0-1023 and vertices 0-144, nothing was set",
                       n, e.chunk_x, e.chunk_y, e.index);
                return WXL_E_INVALID;
            }
            if (!std::isfinite(e.height) || std::fabs(e.height) > WXL_TERRAIN_HEIGHT_LIMIT)
            {
                Refuse("set_heights", "entry %u's height is not a number within +-%.0f yards, nothing was set", n,
                       static_cast<double>(WXL_TERRAIN_HEIGHT_LIMIT));
                return WXL_E_INVALID;
            }
        }
        if (binding::terrain_present() != WXL_TRUE)
        {
            Refuse("set_heights", "there is no ground: the player is not in the world, or the map has no terrain");
            return WXL_E_UNSUPPORTED;
        }
        if (!count) return WXL_OK;

        const Owner           owner = (static_cast<Owner>(Caller()) << 32) | layer;
        std::vector<uint32_t> touched;
        for (uint32_t n = 0; n < count; ++n)
        {
            const wxl_terrain_height& e      = heights[n];
            const uint64_t            serial = ++g_serial;
            grid::ForCopies(e.chunk_x, e.chunk_y, e.index, [&](int32_t cx, int32_t cy, uint32_t i) {
                const uint32_t     key   = Key(cx, cy);
                Chunk&             chunk = Ensure(key);
                std::vector<Edit>& list  = chunk.edits[i];
                auto mine = std::find_if(list.begin(), list.end(), [&](const Edit& edit) { return edit.owner == owner; });
                if (mine == list.end())
                {
                    if (list.empty()) ++chunk.edited;
                    list.push_back(Edit{owner, serial, e.height});
                }
                else
                {
                    mine->serial = serial;
                    mine->height = e.height;
                }
                if (std::find(touched.begin(), touched.end(), key) == touched.end()) touched.push_back(key);
            });
        }

        const Stand stand = Before();
        Pass        pass;
        for (const uint32_t key : touched) pass.WriteHeights(key);
        pass.WriteNormals();
        pass.Rebuild();
        After(stand);
        CountStats();

        if (pass.written) ++g_stats.edits;
        const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begun).count();
        g_stats.last_vertices     = pass.written;
        g_stats.last_chunks       = static_cast<uint32_t>(pass.heights.size());
        g_stats.last_microseconds = static_cast<uint32_t>(spent);
        g_stats.max_microseconds  = std::max(g_stats.max_microseconds, g_stats.last_microseconds);
        return WXL_OK;
    }

    wxl_result WXL_CALL TerrainResetChunk(int32_t chunk_x, int32_t chunk_y, uint32_t layer)
    {
        if (!OnMainThread("reset_chunk")) return WXL_E_INVALID;
        if (!grid::ChunkOnMap(chunk_x, chunk_y))
        {
            Refuse("reset_chunk", "chunk (%d, %d) is off the map: chunks go 0-1023", chunk_x, chunk_y);
            return WXL_E_INVALID;
        }
        const Owner  owner = (static_cast<Owner>(Caller()) << 32) | layer;
        const Chunk* chunk = Find(Key(chunk_x, chunk_y));
        if (!chunk) return WXL_E_NOT_FOUND;
        // The vertices of the chunk the owner edited, and their copies across its edges, so that
        // the edge stays one ground.
        std::set<std::pair<uint32_t, uint32_t>> vertices;
        std::vector<uint32_t>                   keys;
        for (uint32_t i = 0; i < kVertices; ++i)
        {
            const auto& list = chunk->edits[i];
            if (std::none_of(list.begin(), list.end(), [&](const Edit& e) { return e.owner == owner; })) continue;
            grid::ForCopies(chunk_x, chunk_y, i, [&](int32_t cx, int32_t cy, uint32_t j) {
                vertices.insert({Key(cx, cy), j});
                if (std::find(keys.begin(), keys.end(), Key(cx, cy)) == keys.end()) keys.push_back(Key(cx, cy));
            });
        }
        if (vertices.empty()) return WXL_E_NOT_FOUND;
        TakeOff(keys, [&](uint32_t key, uint32_t index, const Edit& e) {
            return e.owner == owner && vertices.count({key, index});
        });
        return WXL_OK;
    }

    wxl_result WXL_CALL TerrainResetAll(uint32_t layer)
    {
        if (!OnMainThread("reset_all")) return WXL_E_INVALID;
        const Owner           owner = (static_cast<Owner>(Caller()) << 32) | layer;
        std::vector<uint32_t> keys;
        for (const auto& [key, chunk] : g_chunks)
            if (chunk->edited) keys.push_back(key);
        return TakeOff(keys, [&](uint32_t, uint32_t, const Edit& e) { return e.owner == owner; }) ? WXL_OK : WXL_E_NOT_FOUND;
    }

    wxl_result WXL_CALL TerrainStats(wxl_terrain_stats* out)
    {
        if (!out || out->struct_size < sizeof(uint32_t))
        {
            Refuse("stats", "the answer's struct_size is not set: set it to sizeof(wxl_terrain_stats)");
            return WXL_E_INVALID;
        }
        wxl_terrain_stats copy = g_stats;
        const uint32_t    size = std::min<uint32_t>(out->struct_size, sizeof copy);
        copy.struct_size       = size;
        std::memcpy(out, &copy, size);
        return WXL_OK;
    }
}

namespace wxl::core::services
{
    const wxl_terrain_api& TerrainApi()
    {
        static const wxl_terrain_api api = [] {
            wxl_terrain_api table{};
            table.struct_size = sizeof table;
            table.height      = &TerrainHeight;
            table.presence    = &TerrainPresence;
            table.trace       = &TerrainTrace;
            table.locate      = &TerrainLocate;
            table.vertices    = &TerrainVertices;
            table.set_heights = &TerrainSetHeights;
            table.reset_chunk = &TerrainResetChunk;
            table.reset_all   = &TerrainResetAll;
            table.stats       = &TerrainStats;
            table.vertices_in = &TerrainVerticesIn;
            return table;
        }();
        return api;
    }

    namespace terrain
    {
        bool ChunkLoaded(void* chunk, wxl_terrain_chunk_state_args& args)
        {
            wxl_core_terrain_chunk view{};
            if (!chunk || binding::terrain_chunk_view(chunk, &view) != WXL_TRUE) return false;
            const uint32_t key    = Key(view.chunk_x, view.chunk_y);
            Chunk*         state  = Find(key);
            const bool     edited = state && state->edited;
            ++g_generation;
            if (EditsNear(view.chunk_x, view.chunk_y))
            {
                // Its own heights as the edits say, then the normals of its vertices and of its
                // neighbours' edges, which counted it missing while it was not loaded.
                Pass pass;
                if (state) pass.WriteHeights(key);
                for (uint32_t i = 0; i < kVertices; ++i) pass.Target(view.chunk_x, view.chunk_y, i);
                pass.WriteNormals();
                pass.Rebuild();
                if (edited) ++g_stats.reapplied;
            }
            args.map_id  = static_cast<uint32_t>(binding::map_id());
            args.chunk_x = view.chunk_x;
            args.chunk_y = view.chunk_y;
            args.edited  = edited ? WXL_TRUE : WXL_FALSE;
            return true;
        }

        bool ChunkUnloading(void* chunk, wxl_terrain_chunk_state_args& args)
        {
            int32_t cx = 0, cy = 0;
            if (!chunk || binding::terrain_chunk_place(chunk, &cx, &cy) != WXL_TRUE) return false;
            const Chunk* state = Find(Key(cx, cy));
            ++g_generation;
            args.map_id  = static_cast<uint32_t>(binding::map_id());
            args.chunk_x = cx;
            args.chunk_y = cy;
            args.edited  = state && state->edited ? WXL_TRUE : WXL_FALSE;
            return true;
        }

        void MapUnloading(wxl_terrain_cleared_args& args)
        {
            uint32_t edited = 0;
            for (const auto& [key, chunk] : g_chunks)
            {
                (void)key;
                if (chunk->edited) ++edited;
            }
            g_chunks.clear();
            g_deferredClutter.clear();
            ++g_generation;
            CountStats();
            if (edited) WLOG_INFO(kTag, "the map is unloaded: the edits of %u chunks are dropped", edited);
            args.map_id = static_cast<uint32_t>(binding::map_id());
            args.chunks = edited;
        }

        bool Present() { return binding::terrain_present() == WXL_TRUE; }

        bool ChunkGround(int32_t chunk_x, int32_t chunk_y, float heights[145], uint16_t& holes)
        {
            Live live;
            if (!LiveOf(chunk_x, chunk_y, live)) return false;
            for (uint32_t i = 0; i < kVertices; ++i) heights[i] = live.view.heights[i] + live.view.base_z;
            holes = static_cast<uint16_t>(live.view.holes);
            return true;
        }

        uint64_t Generation() { return g_generation; }

        void WorldBegin() { g_inWorld = true; }

        void WorldEnd()
        {
            g_inWorld = false;
            if (g_deferredClutter.empty()) return;
            const std::vector<uint32_t> keys = std::move(g_deferredClutter);
            g_deferredClutter.clear();
            for (const uint32_t key : keys)
                if (void* chunk = binding::terrain_chunk(KeyX(key), KeyY(key))) binding::terrain_chunk_rebuild(chunk, WXL_TRUE);
        }

        void ForgetMod(uint32_t mod)
        {
            std::vector<uint32_t> keys;
            for (const auto& [key, chunk] : g_chunks)
                if (chunk->edited) keys.push_back(key);
            if (keys.empty()) return;
            TakeOff(keys, [&](uint32_t, uint32_t, const Edit& e) { return static_cast<uint32_t>(e.owner >> 32) == mod; });
        }

        void Clear()
        {
            g_chunks.clear();
            g_deferredClutter.clear();
            g_inWorld = false;
            g_serial  = 0;
            g_stats   = wxl_terrain_stats{};
        }
    }
}
