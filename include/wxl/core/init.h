// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// WarcraftXL.dll on x64: the entry point the client calls to bring the runtime up.

#ifndef WXL_CORE_INIT_H
#define WXL_CORE_INIT_H

// The recompiled client links WarcraftXL.dll and calls WarcraftXLInit once its process is up.
// The stock client never sees this header: there the DLL is started by the entry trampoline the
// patcher adds (WarcraftXLEntry), with nothing to hand over.
//
// C, not C++, like the SDK contract: the client includes it from whatever language level it is
// built at, and the struct is the only thing both sides have to agree on.

#include <stdint.h>

#include "wxl/abi.h"
#include "wxl/events.h"
#include "wxl/services/m2.h"

#if defined(WXL_CORE_BUILD)
    #define WXL_CORE_API __declspec(dllexport)
#else
    #define WXL_CORE_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// A function the client's script engine calls directly, as its lua_CFunction: the native side
/// of a global set through the script rows of the binding. Answers how many results it pushed.
typedef int32_t(WXL_CALL* wxl_script_native_fn)(void* state);

/// Where the client's script state stands, as script_mark keeps it and script_unwind brings it
/// back: what the client's protected call keeps before it calls. Its words are the client's; the
/// runtime only hands the mark back.
typedef struct wxl_script_mark
{
    uintptr_t words[12];
} wxl_script_mark;

/// A function told one name, as the client's SFile::List calls its callback: cdecl on x86
/// (`call [ebp+0xc]; add esp, 8` at 0x0042509D). Answers nonzero to be told the next.
typedef int32_t(WXL_CALL* wxl_core_name_fn)(const char* name, void* user);

/// A function the client's object manager calls when an update changed a field it was set on,
/// as its MIRRORHANDLERFUNC: the object's guid, the field's byte offset from its kind's first
/// field and its size in bytes, the copy of its old bytes the client kept, and the param it was
/// set with. On x86 the client calls it cdecl, the guid as two dwords (CallMirrorFunctions,
/// `call ecx; add esp, 0x18` at 0x004D5254). What it answers is not read.
typedef int32_t(WXL_CALL* wxl_core_mirror_fn)(uint64_t guid, uint32_t offset, uint32_t size,
                                              const void* old_value, void* param);

/// What a graphics resource is, as gx_resources tells it.
#define WXL_CORE_GPU_TEXTURE 0u ///< a texture: 2D, cube or volume, every level
#define WXL_CORE_GPU_BUFFER  1u ///< a vertex or index buffer

/// One texture or buffer of the graphics device. `pool` is its D3DPOOL: 0 default, 1 managed,
/// 2 system memory, 3 scratch. `bytes` is what its format and dimensions take, every level.
typedef struct wxl_core_gpu_resource
{
    uint32_t kind;
    uint32_t pool;
    uint64_t bytes;
} wxl_core_gpu_resource;

typedef void(WXL_CALL* wxl_core_gpu_fn)(const wxl_core_gpu_resource* resource, void* user);

/// One mounted archive: what its header says of its tables, and what they hold in memory.
typedef struct wxl_core_archive
{
    const char* path;
    int32_t     priority;
    uint32_t    hash_entries;
    uint32_t    block_entries;
    uint64_t    bytes; ///< its hash and block tables, unpacked or packed, and its listfile's names
} wxl_core_archive;

typedef void(WXL_CALL* wxl_core_archive_fn)(const wxl_core_archive* archive, void* user);

/// How many blocks a client database holds at most: its records, its strings, its index.
#define WXL_CORE_DB_BLOCKS 3

/// One client database. Each block is an allocation of the client's, null when it has none, with
/// what it holds as far as the store's own counts tell (0 for the strings, which no count
/// gives); the runtime knows the exact size of a block Storm handed out while it was counting.
typedef struct wxl_core_db
{
    const char* file;
    wxl_bool    loaded;
    uint32_t    records;
    uint32_t    record_size; ///< bytes per record in memory
    const void* blocks[WXL_CORE_DB_BLOCKS];
    uint64_t    estimates[WXL_CORE_DB_BLOCKS];
} wxl_core_db;

typedef void(WXL_CALL* wxl_core_db_fn)(const wxl_core_db* db, void* user);

/// The world camera, as camera_read gives it and camera_write takes it. Directions are in world
/// coordinates, whatever the camera is relative to. `left` is the camera's second row, which the
/// client calls its right: +y for a camera looking down +x.
typedef struct wxl_core_camera
{
    wxl_vec3 position;
    wxl_vec3 forward;
    wxl_vec3 left;
    wxl_vec3 up;
    float    fov;       ///< what it draws with: CGCamera::FOV(), m_fov plus m_targetFov, clamped
    float    fov_base;  ///< m_fov alone, which camera_write sets
    float    near_z;
    float    far_z;
    float    aspect;
    wxl_vec3 pivot;     ///< m_targetPosition raised by m_height
    uint64_t followed;  ///< m_targetGUID, 0 when it follows nothing
    uint32_t cinematic; ///< nonzero while a model camera (m_model) has the view
    float    roll;      ///< radians around `forward`, positive to the camera's right, which
                        ///< camera_write turns the facing by (wxl/core/camera_facing.hpp);
                        ///< camera_read leaves it as it is
} wxl_core_camera;

/// A model file (CM2Shared) as the loader holds it: the path it was opened by, its parsed header
/// (or its bytes, before the parse), how many bytes that is, and its skin profile once one has
/// been read.
typedef struct wxl_core_m2_file
{
    const char* path;
    void*       header;
    uint32_t    size;
    void*       skin;
} wxl_core_m2_file;

/**
 * @brief A loaded model's skeleton, as the runtime poses one model's bones from another's.
 *
 * `bones` are the file's M2CompBone records, `bone_stride` bytes apart, laid out as the file is
 * on every client (key bone id at 0, parent index at 8, name hash at 12, pivot at 0x4C);
 * `key_bones` maps a key bone id to a bone index, 0xFFFF for none; `matrices` are the model's
 * bone matrices this frame (C44Matrix: 16 floats, the translation in the fourth row). `file` is
 * what the bones belong to, so that a map built between two files can be kept.
 */
typedef struct wxl_core_m2_skeleton
{
    const void*     file;
    const uint8_t*  bones;
    uint32_t        bone_count;
    uint32_t        bone_stride;
    const uint16_t* key_bones;
    uint32_t        key_bone_count;
    float*          matrices;
} wxl_core_m2_skeleton;

/**
 * @brief A loaded terrain chunk (CMapChunk), as terrain_chunk_view gives it. `heights` and
 * `normals` point into the tile's file buffer, which lives while the tile is loaded: 145 heights
 * relative to `base_z` (MCVT) and 145 normals as three signed bytes in 127ths (MCNR), both in
 * the grid's vertex order (wxl/terrain/grid.hpp). Writable; the client reads both live.
 */
typedef struct wxl_core_terrain_chunk
{
    int32_t chunk_x;  ///< m_cOffset.x: the chunk's column on the map
    int32_t chunk_y;  ///< m_cOffset.y: its row
    float*  heights;  ///< m_height
    int8_t* normals;  ///< m_normals
    float   base_z;   ///< m_topLeftCoords.z: what the heights are relative to
    uint32_t holes;   ///< m_header->m_holes: the 16-bit hole mask
} wxl_core_terrain_chunk;

/// A terrain render chunk's layers (binding terrain_render_chunk): CMapRenderChunk.
typedef struct wxl_core_render_chunk
{
    uint32_t    layers;            ///< m_layersCount: 0 to 4
    uint32_t    render_flags;      ///< m_renderFlags: 0x4 the first layer specular (its texture stage a reflection map)
    uint32_t    layer_flags[4];    ///< each layer's m_flags (MCLY's)
    uint32_t    layer_texture[4];  ///< each layer's m_textureId: its MTEX index
    const void* batch;             ///< &m_batch: what every draw of the chunk's layers hands the device
    const void* map_chunk;         ///< m_mapChunks[0]: the CMapChunk it draws
    const void* map_chunk2;        ///< m_mapChunks[1]: the second, for a chunk drawn with its neighbour; null for none
    uint32_t    pair;              ///< m_unkFlags & 3: 1 the second below the first (alpha maps 64 x 128), 2 beside it (128 x 64)
} wxl_core_render_chunk;

/// The map's WDT as the client loaded it (binding map_wdt).
typedef struct wxl_core_wdt
{
    const char* path;  ///< CMap::wdtFilename
    uint32_t*   flags; ///< &CMap::header.m_flags: MPHD's flags, as the client reads them
} wxl_core_wdt;

/// The ADT an area of the map was read from (binding terrain_area_file): CMapArea.
typedef struct wxl_core_area_file
{
    int32_t     tile_x; ///< m_index.x
    int32_t     tile_y; ///< m_index.y
    const void* data;   ///< m_fileBuffer: the whole file, MVER first
    uint32_t    size;   ///< m_fileSize
} wxl_core_area_file;

/// A program's record (binding gx_shader_record): CGxShader.
typedef struct wxl_core_shader_record
{
    const char* name;   ///< m_key.m_str: "<program>:<permutation>"
    int32_t     target; ///< m_target: EGxShTarget, 0 vertex, 4 pixel
    const void* code;   ///< m_code.m_data
    uint32_t    size;   ///< m_code.m_count, in bytes
} wxl_core_shader_record;

/// How the world is being drawn (binding gx_world_view): the engine's matrices, rows a..d of
/// C44Matrix, which a point taken from `eye` is multiplied by on the left: clip = [p 1] * view *
/// projection.
typedef struct wxl_core_world_view
{
    float    projection[16]; ///< CGxDevice::m_projNative
    float    view[16];       ///< m_xforms[GxXform_View] at its level
    float    viewport[6];    ///< m_viewport: x min, x max, y min, y max (0 to 1 of the window, y up), z min, z max
    float    window[2];      ///< m_curWindowRect's maxX and maxY: the window's size
    wxl_vec3 eye;            ///< CWorldScene::s_activeWorldView
} wxl_core_world_view;

/// One string per row of wxl/core/binding.def, "name: ret (params)", in table order. The
/// client hands its copy over in wxl_core_init and the runtime compares it with its own, so a
/// client built from a different table is refused by name instead of having its accessors
/// called through the wrong signatures.
static const char* const wxl_core_init_rows[] = {
#define WXL_BINDING(name, ret, params, args, absent) #name ": " #ret " " #params,
#include "wxl/core/binding.def"
#undef WXL_BINDING
};

#define WXL_CORE_INIT_ROW_COUNT ((uint32_t)(sizeof wxl_core_init_rows / sizeof wxl_core_init_rows[0]))

/// What the client hands the runtime: one accessor per row of wxl/core/binding.def, in table
/// order, map_id first. Zero it, set struct_size to sizeof(wxl_core_init), and rows and
/// row_count to wxl_core_init_rows and WXL_CORE_INIT_ROW_COUNT. The runtime reads only what
/// struct_size covers.
///
/// The function pointers are accessors: the runtime is a DLL and reads the client's state only
/// through what the client hands it here. Each may be left NULL, in which case what is built on
/// it answers the row's documented absent value.
typedef struct wxl_core_init
{
    uint32_t           struct_size;
    uint32_t           row_count; ///< how many rows the client was built with
    const char* const* rows;      ///< their strings, as wxl_core_init_rows spells them

#define WXL_BINDING(name, ret, params, args, absent) ret(WXL_CALL* name) params;
#include "wxl/core/binding.def"
#undef WXL_BINDING
} wxl_core_init;

/**
 * @brief One byte per event of wxl/events.h, nonzero while at least one handler is subscribed
 *        to it, or while the runtime's own work rides on the seam that raises it.
 *
 * The runtime writes it whenever an event gains its first handler or loses its last, and holds
 * it up while one of its services needs the seam called (wxl.highlight keeps the model batches of
 * M2.DrawBatch while a highlight is set); the client reads it, through wxl::seam::Listening
 * (wxl/seams/seams.h), before calling a busy seam that raises that event, so a seam nobody needs
 * costs a load and a branch instead of a call into the runtime. All zero until WarcraftXLInit has
 * loaded the mods. Indexed by wxl_event; the client and the runtime come from one tree, so they
 * agree on the count.
 */
WXL_CORE_API extern volatile uint8_t wxl_core_listening[];

/**
 * @brief Brings the runtime up: the mod host, every mod under Mods\ next to the client
 *        executable, then WXL_STAGE_EARLY.
 *
 * Call once, on the main thread, before the client reads anything a mod may want to change, and
 * never under the loader lock: loading a mod is a LoadLibrary. The host lives for the rest of the
 * process. The log is Logs\WarcraftXL.log next to the client executable, open since DllMain.
 *
 * @return WXL_OK when the host is up and its mods are loaded; WXL_E_INVALID when init is null,
 *         its struct_size too small, or its rows disagree with the runtime's (the log names the
 *         first that differs); WXL_E_FAILED when the host could not be stood up, in which case
 *         the reason is in the log and the client goes on without mods. A second call returns
 *         the outcome of the first.
 */
WXL_CORE_API wxl_result WXL_CALL WarcraftXLInit(const wxl_core_init* init);

#ifdef __cplusplus
}
#endif

#endif // WXL_CORE_INIT_H
