// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Vulkan renderer behind the stock client's device (x86 only). The stock Wow.exe makes its
// device as a CGxDeviceD3d (or a CGxDeviceD3d9Ex); under `[gx] api = vulkan` the runtime gives
// that object, as soon as it is made, a copy of its vtable in which every slot that reaches
// Direct3D 9 calls this adapter instead (include/wxl/gx/slots.def says which, and why the others
// may stay). The adapter is the recompiled client's CGxDeviceVk on the stock client: it keeps the
// engine's own logic of each device call, as the D3D9 device does (the window and its procedure,
// the stream pools' offsets, the textures' callbacks, the lost device's return), calling Wow.exe's
// own functions for it (calls_x86.def, domain GxCall), and hands the Vulkan work to the same table
// the recompiled client calls (wxl/core/gx_vk.h). The device's EGxApi stays D3D9's, so the engine
// takes the paths it takes on D3D9 (its shader profiles, its shadows, the pointer's half pixel).
//
// When Vulkan cannot start (no loader, no driver, no device meeting the floor, a window it cannot
// present to), the device gets its own vtable back and goes on as the D3D9 device it was made as,
// with the same format, and the log says why.
//
// Main thread only, as the engine drives its device.

#pragma once

#include "wxl/core/gx_vk.h"
#include "wxl/events.h"

#include <cstdint>
#include <type_traits>

namespace wxl::core::gx::x86
{
    /// A __thiscall slot as the runtime writes it: __fastcall with a dummy second parameter takes
    /// `this` in ECX, the dummy in EDX, every other parameter on the stack, popped by the callee.
    template <class Signature>
    struct Thiscall;

    template <class R, class Self, class... A>
    struct Thiscall<R(Self, A...)>
    {
        using Fn = R(__fastcall*)(Self, void*, A...);
        /// The bytes of stack the callee pops: four for each parameter of 32 bits or less.
        static constexpr uint32_t kPops = (0u + ... + static_cast<uint32_t>(((sizeof(A) + 3) / 4) * 4));
    };

    template <class Signature>
    struct Stdcall;

    template <class R, class... A>
    struct Stdcall<R(A...)>
    {
        using Fn = R(__stdcall*)(A...);
    };

    template <class Signature>
    struct Cdecl;

    template <class R, class... A>
    struct Cdecl<R(A...)>
    {
        using Fn = R(__cdecl*)(A...);
    };

    template <class Signature, class Conv>
    struct CallOf;
    struct conv_thiscall;
    struct conv_stdcall;
    struct conv_cdecl;
    template <class Signature>
    struct CallOf<Signature, conv_thiscall> : Thiscall<Signature>
    {
    };
    template <class Signature>
    struct CallOf<Signature, conv_stdcall> : Stdcall<Signature>
    {
    };
    template <class Signature>
    struct CallOf<Signature, conv_cdecl> : Cdecl<Signature>
    {
    };

    /**
     * @brief What the adapter reaches of Wow.exe: the functions of calls_x86.def's GxCall rows, in
     *        the convention each row declares (a __thiscall one called with `this` then a null
     *        EDX), and the statics it reads. The live client's (Live()), or the harness's.
     */
    struct Stock
    {
// One member per row of the table, named <domain>_<name>; the adapter calls the GxCall ones.
#define WXL_BINDING_CALL_X86(domain, name, symbol, ret, conv, params) typename CallOf<ret params, conv_##conv>::Fn domain##_##name = nullptr;
#include "wxl/core/binding/calls_x86.def"
#undef WXL_BINDING_CALL_X86

        void**         device_ptr = nullptr;       ///< g_theGxDevicePtr
        uint8_t*       shadow_constants = nullptr; ///< CGxDevice::s_shadowConstants[2]: [0] pixel, [1] vertex
        void**         ui_vertex_shaders = nullptr; ///< CGxDevice::s_uiVertexShader[2]
        void**         ui_pixel_shader = nullptr;  ///< CGxDevice::s_uiPixelShader
        int32_t*       client_adjust = nullptr;    ///< CGxDevice::s_clientAdjust (a C2iVector)
        const float*   win_aspect = nullptr;       ///< kWinAspect
        const int32_t* win_min_size = nullptr;     ///< kWinMinSize (a C2iVector)
        const uint32_t* bytes_per_block = nullptr; ///< CGxDevice::s_texFormatBytesPerBlock
        const uint32_t* bit_depth = nullptr;       ///< CGxDevice::s_texFormatBitDepth
        void* const*   vtables[2] = {};            ///< the stock vtables of D3D9 and D3D9Ex (slots.def)
        bool           check_entries = false;      ///< a device's vtable must hold Wow.exe's entries (slots.def)
        /// What makes the client's window: CreateWindowExA (the harness, which has no display, its own).
        void*(__stdcall* create_window)(uint32_t ex_style, const char* class_name, const char* title, uint32_t style, int32_t x,
                                        int32_t y, int32_t width, int32_t height, void* parent, void* menu, void* instance,
                                        void* param) = nullptr;
    };

    /// Wow.exe's own, resolved from the tables when the runtime is built.
    const Stock& Live();

    /**
     * @brief Puts the adapter between the stock client and its device factories: each D3D9 or
     *        D3D9Ex device made from now on is the Vulkan renderer's to draw with, when
     *        `[gx] api = vulkan` asks for it. Called once the hook engine is up; false when a
     *        factory could not be detoured (logged: the client stays on D3D9).
     */
    bool Install();

    /**
     * @brief Gives the engine's new @p device (made by a stock factory, still holding its own
     *        vtable) the copy whose Direct3D 9 slots call the adapter. Nothing when the device is
     *        not one of the two stock D3D9 devices or its vtable is not the stock one (logged).
     * @return true when the device draws through the adapter from its DeviceCreate on.
     */
    bool Adopt(void* device);

    /// Whether @p device is the adapter's: it holds the copy of its vtable.
    bool Adopted(const void* device);

    /// The device the adapter has given its vtable copy to, null when none.
    void* AdoptedDevice();

    /// What the adapter calls: the live client's and the runtime's own table, or the harness's.
    void UseForTests(const Stock* stock, const wxl_gx_vk_backend* backend);

    /// The vtable @p device holds now.
    void* const* VtableOf(const void* device);

    /// The adapter's slot functions, one per `front` row of slots.def, with the row's parameters
    /// as __thiscall (Thiscall above).
    namespace slots
    {
#define WXL_GX_SLOT(index, name, ret, params, pops, kind, d3d9, d3d9_at, d3d9ex, d3d9ex_at) WXL_GX_SLOT_DECLARE_##kind(name, ret, params)
#define WXL_GX_SLOT_DECLARE_keep(name, ret, params)
#define WXL_GX_SLOT_DECLARE_front(name, ret, params) std::remove_pointer_t<Thiscall<ret params>::Fn> name;
#include "wxl/gx/slots.def"
#undef WXL_GX_SLOT_DECLARE_keep
#undef WXL_GX_SLOT_DECLARE_front
    }
}
