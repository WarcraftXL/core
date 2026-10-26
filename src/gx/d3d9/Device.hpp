// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The calls the runtime's own Direct3D 9 passes make, as an interface: the game's device behind
// it in the client (D3d9Device), a recording one in the seam harness, which checks what a pass
// sets and that it puts back what it found without a GPU. Every object it hands out is the
// device's (a surface, a texture, a program) and goes back through Release.

#pragma once

#include <windows.h>
#include <d3d9.h>

// rpcndr.h, which d3d9.h brings in, defines `small` as `char`: undone, so that code including this
// header may still name a variable so.
#ifdef small
    #undef small
#endif

#include <cstdint>

namespace wxl::core::gx::d3d9
{
    class Device
    {
    public:
        virtual ~Device() = default;

        virtual HRESULT Caps(D3DCAPS9& out) = 0;

        // Targets.
        virtual HRESULT GetRenderTarget(DWORD index, IDirect3DSurface9** out) = 0;
        virtual HRESULT SetRenderTarget(DWORD index, IDirect3DSurface9* surface) = 0;
        virtual HRESULT GetDepthStencil(IDirect3DSurface9** out) = 0;
        virtual HRESULT SetDepthStencil(IDirect3DSurface9* surface) = 0;
        virtual HRESULT SurfaceDesc(IDirect3DSurface9* surface, D3DSURFACE_DESC& out) = 0;
        virtual HRESULT TextureSurface(IDirect3DTexture9* texture, IDirect3DSurface9** out) = 0;
        virtual HRESULT Clear(DWORD flags, D3DCOLOR color) = 0;
        virtual HRESULT StretchRect(IDirect3DSurface9* from, IDirect3DSurface9* to) = 0;

        // Objects of its own.
        virtual HRESULT CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples,
                                           DWORD quality, IDirect3DSurface9** out)                       = 0;
        virtual HRESULT CreateTargetTexture(UINT width, UINT height, D3DFORMAT format, IDirect3DTexture9** out) = 0;
        virtual HRESULT CreateVertexShader(const DWORD* code, IDirect3DVertexShader9** out)            = 0;
        virtual HRESULT CreatePixelShader(const DWORD* code, IDirect3DPixelShader9** out)              = 0;
        virtual HRESULT CreateDeclaration(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** out) = 0;
        virtual void    Release(IUnknown* object)                                                      = 0;
        /// The first token of a vertex program's bytecode: its version (0xFFFE0300 for vs_3_0).
        virtual DWORD VertexShaderVersion(IDirect3DVertexShader9* shader) = 0;

        // State.
        virtual void SetViewport(const D3DVIEWPORT9& viewport)                          = 0;
        virtual void SetRenderState(D3DRENDERSTATETYPE state, DWORD value)              = 0;
        virtual void SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE state, DWORD value) = 0;
        virtual void SetTexture(DWORD sampler, IDirect3DBaseTexture9* texture)          = 0;
        virtual void SetDeclaration(IDirect3DVertexDeclaration9* declaration)           = 0;
        virtual void SetStreamSource(UINT stream, IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) = 0;
        virtual void SetIndices(IDirect3DIndexBuffer9* indices)                         = 0;
        virtual void SetVertexShader(IDirect3DVertexShader9* shader)                    = 0;
        virtual void SetPixelShader(IDirect3DPixelShader9* shader)                      = 0;
        virtual void SetVertexConstants(UINT first, const float* values, UINT count)    = 0;
        virtual void SetPixelConstants(UINT first, const float* values, UINT count)     = 0;

        // Draws.
        virtual void DrawIndexed(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT vertices, UINT startIndex,
                                 UINT primitives)                                                       = 0;
        virtual void DrawUp(D3DPRIMITIVETYPE type, UINT primitives, const void* vertices, UINT stride) = 0;
    };

    /// The game's IDirect3DDevice9 behind the interface.
    class D3d9Device final : public Device
    {
    public:
        explicit D3d9Device(IDirect3DDevice9* device) : d_(device) {}

        IDirect3DDevice9* Raw() const { return d_; }

        HRESULT Caps(D3DCAPS9& out) override { return d_->GetDeviceCaps(&out); }
        HRESULT GetRenderTarget(DWORD index, IDirect3DSurface9** out) override { return d_->GetRenderTarget(index, out); }
        HRESULT SetRenderTarget(DWORD index, IDirect3DSurface9* surface) override { return d_->SetRenderTarget(index, surface); }
        HRESULT GetDepthStencil(IDirect3DSurface9** out) override { return d_->GetDepthStencilSurface(out); }
        HRESULT SetDepthStencil(IDirect3DSurface9* surface) override { return d_->SetDepthStencilSurface(surface); }
        HRESULT SurfaceDesc(IDirect3DSurface9* surface, D3DSURFACE_DESC& out) override { return surface->GetDesc(&out); }
        HRESULT TextureSurface(IDirect3DTexture9* texture, IDirect3DSurface9** out) override
        {
            return texture->GetSurfaceLevel(0, out);
        }
        HRESULT Clear(DWORD flags, D3DCOLOR color) override { return d_->Clear(0, nullptr, flags, color, 1.0f, 0); }
        HRESULT StretchRect(IDirect3DSurface9* from, IDirect3DSurface9* to) override
        {
            return d_->StretchRect(from, nullptr, to, nullptr, D3DTEXF_NONE);
        }
        HRESULT CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, DWORD quality,
                                   IDirect3DSurface9** out) override
        {
            return d_->CreateRenderTarget(width, height, format, samples, quality, FALSE, out, nullptr);
        }
        HRESULT CreateTargetTexture(UINT width, UINT height, D3DFORMAT format, IDirect3DTexture9** out) override
        {
            return d_->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, out, nullptr);
        }
        HRESULT CreateVertexShader(const DWORD* code, IDirect3DVertexShader9** out) override
        {
            return d_->CreateVertexShader(code, out);
        }
        HRESULT CreatePixelShader(const DWORD* code, IDirect3DPixelShader9** out) override
        {
            return d_->CreatePixelShader(code, out);
        }
        HRESULT CreateDeclaration(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** out) override
        {
            return d_->CreateVertexDeclaration(elements, out);
        }
        void Release(IUnknown* object) override
        {
            if (object) object->Release();
        }
        DWORD VertexShaderVersion(IDirect3DVertexShader9* shader) override
        {
            UINT size = 0;
            if (!shader || FAILED(shader->GetFunction(nullptr, &size)) || size < sizeof(DWORD)) return 0;
            DWORD  local[64];
            DWORD* code = size <= sizeof local ? local : new DWORD[(size + 3) / 4];
            DWORD  version = SUCCEEDED(shader->GetFunction(code, &size)) ? code[0] : 0;
            if (code != local) delete[] code;
            return version;
        }
        void SetViewport(const D3DVIEWPORT9& viewport) override { d_->SetViewport(&viewport); }
        void SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override { d_->SetRenderState(state, value); }
        void SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE state, DWORD value) override
        {
            d_->SetSamplerState(sampler, state, value);
        }
        void SetTexture(DWORD sampler, IDirect3DBaseTexture9* texture) override { d_->SetTexture(sampler, texture); }
        void SetDeclaration(IDirect3DVertexDeclaration9* declaration) override { d_->SetVertexDeclaration(declaration); }
        void SetStreamSource(UINT stream, IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) override
        {
            d_->SetStreamSource(stream, buffer, offset, stride);
        }
        void SetIndices(IDirect3DIndexBuffer9* indices) override { d_->SetIndices(indices); }
        void SetVertexShader(IDirect3DVertexShader9* shader) override { d_->SetVertexShader(shader); }
        void SetPixelShader(IDirect3DPixelShader9* shader) override { d_->SetPixelShader(shader); }
        void SetVertexConstants(UINT first, const float* values, UINT count) override
        {
            d_->SetVertexShaderConstantF(first, values, count);
        }
        void SetPixelConstants(UINT first, const float* values, UINT count) override
        {
            d_->SetPixelShaderConstantF(first, values, count);
        }
        void DrawIndexed(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT vertices, UINT startIndex,
                         UINT primitives) override
        {
            d_->DrawIndexedPrimitive(type, baseVertex, minIndex, vertices, startIndex, primitives);
        }
        void DrawUp(D3DPRIMITIVETYPE type, UINT primitives, const void* vertices, UINT stride) override
        {
            d_->DrawPrimitiveUP(type, primitives, vertices, stride);
        }

    private:
        IDirect3DDevice9* d_;
    };
}
