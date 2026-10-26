// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What the stock client's D3D9 device had bound when CM2SceneRender::DrawBatch (0x008203B0) drew a
// batch, read from the engine's own copy of what it set: the device is created pure
// (D3DCREATE_PUREDEVICE, CGxDeviceD3d::DeviceCreate), so Direct3D cannot be asked. wxl.highlight
// keeps these to draw the batch again after the world. Header-only, so the seam harness drives it
// over stand-ins of the structs.
//
// Read in Wow.exe: CGxDeviceD3d::ISetVertexBuffer (0x006A39E0) compares a stream's buffer, offset
// and stride at +0x3B64, +0x3B84 and +0x3BA4 (one dword per stream); IStateSync (0x006A5940) keeps
// the index buffer it set at +0x3B60 and reads the vertex program from m_hwRenderStates.m_data
// (+0x2900) + 0x4D0, the 77th CGxStateBom of 16 bytes; IShaderConstantsFlush (0x006A9FE0) sends
// the vertex registers between dirtyMin (0x00C5FFEC) and dirtyMax (0x00C5FFE8), the end of
// s_shadowConstants[1] (0x00C5DFE0 + 0x1008).

#pragma once

#include "wxl/offsets/layout.hpp"

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wxl::core::binding::x86::highlight_draw
{
    constexpr uint32_t kApiD3d9         = 1;  // EGxApi
    constexpr uint32_t kApiD3d9Ex       = 2;
    constexpr uint32_t kRsVertexShader  = 77; // EGxRenderState GxRs_VertexShader: its value is a CGxShader*
    constexpr uint32_t kStreams         = 8;  // m_d3dVertexStreamBuf[8]
    constexpr uint32_t kRows            = 256; // ShadowConstants::constants: 256 float4 registers
    constexpr std::size_t kShadowSize   = kRows * 16 + 8; // ShadowConstants: constants, dirtyMax, dirtyMin
    constexpr std::size_t kShadowMax    = kRows * 16;
    constexpr std::size_t kShadowMin    = kRows * 16 + 4;

    /// What the device had bound for a draw.
    struct State
    {
        void*    declaration = nullptr;  ///< IDirect3DVertexDeclaration9*
        void*    indices     = nullptr;  ///< IDirect3DIndexBuffer9*
        void*    streams[kStreams]{};    ///< IDirect3DVertexBuffer9*
        uint32_t offsets[kStreams]{};
        uint32_t strides[kStreams]{};
        void*    vertexShader = nullptr; ///< IDirect3DVertexShader9*; null: the fixed pipeline
    };

    template <class T>
    T Read(const void* base, std::size_t offset)
    {
        T value;
        std::memcpy(&value, static_cast<const char*>(base) + offset, sizeof value);
        return value;
    }

    /// Whether the device draws into a texture now (a portrait, a minimap) rather than the screen.
    inline bool IntoTexture(const void* device)
    {
        return Read<void*>(device, WXL_FIELD("CGxDevice", "m_textureTarget[0].m_apiSpecific")) ||
               Read<void*>(device, WXL_FIELD("CGxDevice", "m_textureTarget[1].m_apiSpecific"));
    }

    namespace detail
    {
        template <std::size_t Decl, std::size_t Index, std::size_t Buf, std::size_t Ofs, std::size_t Stride>
        void ReadBindings(const void* device, State& out)
        {
            out.declaration = Read<void*>(device, Decl);
            out.indices     = Read<void*>(device, Index);
            for (uint32_t i = 0; i < kStreams; ++i)
            {
                out.streams[i] = Read<void*>(device, Buf + i * sizeof(void*));
                out.offsets[i] = Read<uint32_t>(device, Ofs + i * 4);
                out.strides[i] = Read<uint32_t>(device, Stride + i * 4);
            }
        }
    }

    /**
     * @brief The bindings of the D3D9 device `device` (a CGxDevice*) as its last draw left them;
     *        false on a device of another API, with no context, or lost.
     *
     * The declaration, the index buffer and the streams are what CGxDeviceD3d::IRsSendToHw last
     * set (CGxDeviceD3d and CGxDeviceD3d9Ex each keep them, at their own offsets); the vertex
     * program is the CGxShader the hardware render state GxRs_VertexShader holds, its Direct3D
     * object in m_apiSpecific.
     */
    inline bool ReadState(const void* device, State& out)
    {
        out = State{};
        if (!device) return false;
        const uint32_t api = Read<uint32_t>(device, WXL_FIELD("CGxDevice", "m_api"));
        if (api != kApiD3d9 && api != kApiD3d9Ex) return false;
        if (!Read<uint32_t>(device, WXL_FIELD("CGxDevice", "m_context")) || Read<uint32_t>(device, WXL_FIELD("CGxDevice", "m_intF5C")))
            return false;
        if (api == kApiD3d9)
            detail::ReadBindings<WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentVertexDecl"), WXL_FIELD("CGxDeviceD3d", "m_d3dCurrentIndexBuf"),
                                 WXL_FIELD("CGxDeviceD3d", "m_d3dVertexStreamBuf"), WXL_FIELD("CGxDeviceD3d", "m_d3dVertexStreamOfs"),
                                 WXL_FIELD("CGxDeviceD3d", "m_d3dVertexStreamStride")>(device, out);
        else
            detail::ReadBindings<WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dCurrentVertexDecl"),
                                 WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dCurrentIndexBuf"), WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dVertexStreamBuf"),
                                 WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dVertexStreamOfs"),
                                 WXL_FIELD("CGxDeviceD3d9Ex", "m_d3dVertexStreamStride")>(device, out);

        const uint32_t count  = Read<uint32_t>(device, WXL_FIELD("CGxDevice", "m_hwRenderStates.m_count"));
        const uint8_t* states = Read<const uint8_t*>(device, WXL_FIELD("CGxDevice", "m_hwRenderStates.m_data"));
        if (states && count > kRsVertexShader)
        {
            // CGxStateBom: the value's first word is the pointer.
            const void* shader = Read<const void*>(states, kRsVertexShader * WXL_TYPE_SIZE("CGxStateBom"));
            if (shader) out.vertexShader = Read<void*>(shader, WXL_FIELD("CGxShader", "m_apiSpecific"));
        }
        return out.declaration && out.indices;
    }

    /// Whether a register of the engine's shadow was set this frame: CGxDevice::ShaderConstantsClear
    /// (at every BeginScene) fills all four floats with FLT_MAX.
    inline bool RowSet(const float* row)
    {
        return !(row[0] == FLT_MAX && row[1] == FLT_MAX && row[2] == FLT_MAX && row[3] == FLT_MAX);
    }

    /**
     * @brief Marks registers [first, last] of the engine's vertex shadow (CGxDevice::
     *        s_shadowConstants[1]) as changed, so that its next flush sends them to the device
     *        again: what CGxDevice::ShaderConstantsUnlock does.
     */
    inline void MarkDirty(uint8_t* shadow, uint32_t first, uint32_t last)
    {
        uint32_t dirtyMax = Read<uint32_t>(shadow, kShadowMax);
        uint32_t dirtyMin = Read<uint32_t>(shadow, kShadowMin);
        if (dirtyMin > first) dirtyMin = first;
        if (dirtyMax < last) dirtyMax = last;
        std::memcpy(shadow + kShadowMax, &dirtyMax, sizeof dirtyMax);
        std::memcpy(shadow + kShadowMin, &dirtyMin, sizeof dirtyMin);
    }
}
