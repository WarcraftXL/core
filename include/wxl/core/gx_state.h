// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The engine's small Gx records the Vulkan renderer reads besides the SDK's engine headers
// (wxl/engine/gx.hpp): an element of CGxDevice::m_appRenderStates, a CGxVertexAttrib, the viewport
// (a CBoundingBox) and a CGxBatch. CGxDevice holds the first three as an untyped array and as
// bytes in the engine headers, which keep its published layout hash; their layout is written here
// once, the same declarations as the client fork's, so it is the same on both architectures. The
// recompiled client holds its own types to these (static_assert in CGxDeviceVk.cpp), a capture
// carries the size of a state (wxl_gx_capture_header::state_size).

#ifndef WXL_CORE_GX_STATE_H
#define WXL_CORE_GX_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// CGxStateBom: one render state's value, read as its type says (an int, a float, a pointer).
typedef struct wxl_gx_state_value
{
    union
    {
        int32_t  i[3];
        uint32_t u[3];
        float    f[3];
        void*    p;
    } data;
    int32_t filler;
} wxl_gx_state_value;

/// CGxAppRenderState: what the application set, and the push stack's bookkeeping.
typedef struct wxl_gx_app_state
{
    wxl_gx_state_value value;
    uint32_t           stack_depth;
    int32_t            dirty;
} wxl_gx_app_state;

/// CGxVertexAttrib: an attribute of the vertex the next draw reads.
typedef struct wxl_gx_vertex_attrib
{
    uint32_t attrib;   ///< EGxVertexAttrib
    uint32_t type;     ///< 0 D3DCOLOR, 1 UBYTE4, 2 UBYTE4N, 3 FLOAT2, 4 FLOAT3, 5 SHORT2, 6 FLOAT1
    uint32_t offset;   ///< in the vertex
    uint32_t buf_size; ///< the vertex's size
} wxl_gx_vertex_attrib;

/// CBoundingBox of CGxDevice::m_viewport: x, y and z ranges, each low then high, x and y in [0, 1]
/// of the window with y up.
typedef struct wxl_gx_viewport
{
    float x_low, x_high;
    float y_low, y_high;
    float z_low, z_high;
} wxl_gx_viewport;

/// CGxBatch: one draw's primitive and range.
typedef struct wxl_gx_batch
{
    uint32_t primitive; ///< EGxPrim
    uint32_t start;     ///< the first index
    uint32_t count;     ///< indices (or vertices when the draw is not indexed)
    uint16_t min_index;
    uint16_t max_index;
} wxl_gx_batch;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // WXL_CORE_GX_STATE_H
