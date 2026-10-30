// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A frame capture: one frame of the recompiled client's D3D9 device, as its device calls saw it,
// with the D3D9 back buffer of the same frame, so that the same frame can be drawn again through
// another device (the Vulkan renderer's front, on lavapipe in the tests) and compared pixel by
// pixel. The client writes it (WarcraftXL/GxCapture.cpp, the console command gxCapture); the
// runtime's tests read it (src/gx/capture/). Captures carry the game's own textures, buffers and
// shaders: they stay on the machine that made them, never in a repository.
//
// One capture is a folder <client>\WarcraftXL\captures\<date>-<time>\ holding:
//   frame.wxlcap  the frame, in this format
//   frame.tga     the D3D9 back buffer at the end of the frame, before its present (32-bit BGRA)
//
// The file is little-endian: a wxl_gx_capture_header, then chunks, each a wxl_gx_capture_chunk
// followed by `size` bytes and padded with zeros to a multiple of 4. The chunks come in the order
// the device saw the calls. Pointers of the client (ids of textures, shaders, pools, buffers) are
// written as 64-bit values: identities, not addresses to follow.
//
// The device's state travels as deltas of memory regions (WXL_GX_CAPTURE_MEM): the first MEM of a
// region holds it whole, each later one the runs that changed since. A region is read as raw bytes
// of the engine's layout for the architecture and the CGxDevice layout hash of the header (the
// SDK's wxl/engine/gx.hpp). Before every event (clear, draw, target, copy) the regions are brought
// up to date, so the state a reader holds when it meets an event is the state the device had.
//
// What is not in a capture: the D3D9 shader registers set in frames before the captured one (the
// constants the frame itself flushes are in it, as WXL_GX_CAPTURE_CONSTANTS), and the bytes of a
// dynamic pool the frame reads but did not write (written as unknown, `known` 0).

#ifndef WXL_CORE_GX_CAPTURE_H
#define WXL_CORE_GX_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_GX_CAPTURE_MAGIC   0x50414358u /* "XCAP" */
#define WXL_GX_CAPTURE_VERSION 1u

typedef struct wxl_gx_capture_header
{
    uint32_t magic;              ///< WXL_GX_CAPTURE_MAGIC
    uint32_t version;            ///< WXL_GX_CAPTURE_VERSION
    uint32_t header_size;        ///< sizeof(wxl_gx_capture_header): chunks start here
    uint32_t pointer_size;       ///< 8 on the recompiled client
    uint64_t device_layout_hash; ///< wxl::engine::CGxDevice::kLayoutHash of the writer
    uint32_t device_size;        ///< bytes of a DEVICE region: sizeof(CGxDevice)
    uint32_t tex_size;           ///< bytes of a texture's object: sizeof(CGxTex)
    uint32_t buf_size;           ///< sizeof(CGxBuf)
    uint32_t state_size;         ///< bytes of one application render state: sizeof(CGxAppRenderState)
    uint32_t width;              ///< the back buffer's size
    uint32_t height;
    uint32_t api;                ///< the client's EGxApi (1: D3D9)
    uint32_t frame;              ///< CGxDevice::m_frameCount when the capture began
} wxl_gx_capture_header;

typedef struct wxl_gx_capture_chunk
{
    uint32_t id;   ///< a WXL_GX_CAPTURE_* value
    uint32_t size; ///< bytes that follow, before the padding
} wxl_gx_capture_chunk;

/// The chunks. Their bodies, field by field, in the order they are written.
enum
{
    /// uint32 region (wxl_gx_capture_region), uint32 total size of the region, uint32 run count,
    /// then per run: uint32 offset, uint32 length, `length` bytes.
    WXL_GX_CAPTURE_MEM = 0x204D454Du, /* "MEM " */

    /// uint64 id (CGxTex*), `tex_size` bytes of the CGxTex, uint32 level count, then per level:
    /// uint32 face, uint32 mip, uint32 width, uint32 height, uint32 pitch (bytes per row, or per row
    /// of 4x4 blocks), uint32 format (EGxTexFormat of the bytes), uint32 size, `size` bytes. No level
    /// when the texture cannot be read (a depth texture). Written when a texture is first named, and
    /// again when the frame changed it.
    WXL_GX_CAPTURE_TEXTURE = 0x20584554u, /* "TEX " */

    /// uint64 id (CGxShader*), int32 target (EGxShTarget), uint32 name length, the name (no nul),
    /// uint32 code size, the code (the record's DX9 bytecode, CGxShader::m_code). Once per shader.
    WXL_GX_CAPTURE_SHADER = 0x52444853u, /* "SHDR" */

    /// uint64 id (CGxPool*), uint32 target (EGxPoolTarget), uint32 usage (EGxPoolUsage), uint32
    /// size, uint32 hint. Once per pool.
    WXL_GX_CAPTURE_POOL = 0x4C4F4F50u, /* "POOL" */

    /// uint64 id (CGxBuf*), uint64 pool id, `buf_size` bytes of the CGxBuf. Before each event that
    /// names the buffer, when it changed.
    WXL_GX_CAPTURE_BUFFER = 0x20465542u, /* "BUF " */

    /// uint64 pool id, uint32 offset, uint32 size, uint32 known (1: the bytes the frame wrote or the
    /// pool holds; 0: the pool could not be read back, the bytes are zeros), `size` bytes.
    WXL_GX_CAPTURE_DATA = 0x41544144u, /* "DATA" */

    /// uint32 target (0 vertex, 1 pixel), uint32 first row, uint32 rows, rows * 16 bytes: what the
    /// device sent to the shader registers (its IShaderConstantsFlush).
    WXL_GX_CAPTURE_CONSTANTS = 0x534E4F43u, /* "CONS" */

    /// uint32 mask (EGxBufferBit), uint32 colour (0xAARRGGBB): CGxDevice::SceneClear.
    WXL_GX_CAPTURE_CLEAR = 0x20524C43u, /* "CLR " */

    /// 16 bytes of the CGxBatch (EGxPrim m_primType, uint32 m_start, uint32 m_count, uint16
    /// m_minIndex, uint16 m_maxIndex), int32 count: CGxDevice::Draw.
    WXL_GX_CAPTURE_DRAW = 0x57415244u, /* "DRAW" */

    /// uint32 buffer (EGxBuffer), uint64 texture id (0: the back buffer), uint32 plane:
    /// CGxDevice::DeviceSetRenderTarget.
    WXL_GX_CAPTURE_TARGET = 0x54475452u, /* "RTGT" */

    /// uint32 kind (0 TexCopy, 1 TexStretch), uint64 destination texture id, int32 values: for a
    /// copy dest x, y, source x, y, width, height, level, plane; for a stretch the destination and
    /// source rectangles (minY, minX, maxY, maxX each, all -1 when null), level, face.
    WXL_GX_CAPTURE_COPY = 0x59504F43u, /* "COPY" */

    /// uint32 width, uint32 height, width * height * 4 bytes (BGRA): the back buffer at the present.
    WXL_GX_CAPTURE_SHOT = 0x544F4853u, /* "SHOT" */

    /// Nothing: the frame is whole.
    WXL_GX_CAPTURE_END = 0x20444E45u, /* "END " */
};

/// The memory regions a MEM chunk updates.
typedef enum wxl_gx_capture_region
{
    WXL_GX_CAPTURE_REGION_DEVICE = 1,    ///< the CGxDevice object (`device_size` bytes)
    WXL_GX_CAPTURE_REGION_STATES = 2,    ///< the application render states (m_appRenderStates' elements)
    WXL_GX_CAPTURE_REGION_VS_SHADOW = 3, ///< CGxDevice::s_shadowConstants[1]: the vertex constants set, 256 rows and the dirty range
    WXL_GX_CAPTURE_REGION_PS_SHADOW = 4, ///< CGxDevice::s_shadowConstants[0]: the pixel constants set
} wxl_gx_capture_region;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // WXL_CORE_GX_CAPTURE_H
