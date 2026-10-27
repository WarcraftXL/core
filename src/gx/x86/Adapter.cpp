// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Vulkan renderer behind the stock client's device (Adapter.hpp): the vtable copy, the slots
// it points at, the window, and the detours on the two device factories.

#include "gx/x86/Adapter.hpp"

#include "common/Log.hpp"
#include "hook/Engine.hpp"
#include "runtime/Session.hpp"
#include "runtime/Settings.hpp"
#include "runtime/Settings.hpp"

#include "wxl/engine/gx.hpp"
#include "wxl/offsets/layout.hpp"
#include "wxl/offsets/site.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <iterator>
#include <cstdio>
#include <cstring>

namespace wxl::core::gx::x86
{
    // --- Wow.exe's own ---------------------------------------------------------------------------

    const Stock& Live()
    {
        static const Stock stock = [] {
            Stock s;
#define WXL_BINDING_CALL_X86(domain, name, symbol, ret, conv, params) \
    s.domain##_##name = reinterpret_cast<decltype(s.domain##_##name)>(WXL_SEAM_SITE(symbol, 0));
#include "wxl/core/binding/calls_x86.def"
#undef WXL_BINDING_CALL_X86
            s.device_ptr        = reinterpret_cast<void**>(WXL_STATIC("g_theGxDevicePtr"));
            s.shadow_constants  = reinterpret_cast<uint8_t*>(WXL_STATIC("CGxDevice::s_shadowConstants"));
            s.ui_vertex_shaders = reinterpret_cast<void**>(WXL_STATIC("CGxDevice::s_uiVertexShader"));
            s.ui_pixel_shader   = reinterpret_cast<void**>(WXL_STATIC("CGxDevice::s_uiPixelShader"));
            s.client_adjust     = reinterpret_cast<int32_t*>(WXL_STATIC("CGxDevice::s_clientAdjust"));
            s.win_aspect        = reinterpret_cast<const float*>(WXL_STATIC("kWinAspect"));
            s.win_min_size      = reinterpret_cast<const int32_t*>(WXL_STATIC("kWinMinSize"));
            s.bytes_per_block   = reinterpret_cast<const uint32_t*>(WXL_STATIC("CGxDevice::s_texFormatBytesPerBlock"));
            s.bit_depth         = reinterpret_cast<const uint32_t*>(WXL_STATIC("CGxDevice::s_texFormatBitDepth"));
#define WXL_GX_VTABLE(api, address, slots, factory) s.vtables[api##_index] = reinterpret_cast<void* const*>(address);
            constexpr int d3d9_index = 0, d3d9ex_index = 1;
#include "wxl/gx/slots.def"
            s.check_entries = true;
            s.create_window = reinterpret_cast<decltype(s.create_window)>(&CreateWindowExA);
            return s;
        }();
        return stock;
    }
}

namespace
{
    namespace engine = wxl::engine;
    namespace x86    = wxl::core::gx::x86;

    constexpr char     kTag[]     = "gx";
    constexpr uint32_t kSlotCount = 84;

    // The engine's numbers this adapter speaks in (the fork's Types.h).
    constexpr uint32_t kPoolUsageStream     = 2;  // EGxPoolUsage
    constexpr uint32_t kTexDxt1             = 6;  // EGxTexFormat: Dxt1, Dxt3, Dxt5 are 6..8
    constexpr uint32_t kTexCubeMap          = 1;  // EGxTexTarget
    constexpr uint32_t kCubeFaces           = 6;
    constexpr uint32_t kTexLock             = 0;  // EGxTexCommand
    constexpr uint32_t kTexLatch            = 1;
    constexpr uint32_t kTexUnlock           = 2;
    constexpr uint32_t kTexRecreate         = 3;
    constexpr uint32_t kRsTexture0          = 21; // EGxRenderState
    constexpr uint32_t kRsTexture15         = 36;
    constexpr uint32_t kRsTexGen0           = 53;
    constexpr uint32_t kRsTexGen7           = 60;
    constexpr uint32_t kRsVertexShader      = 77;
    constexpr uint32_t kRsPixelShader       = 78;
    constexpr int32_t  kTexGenObjectLinear  = 1;  // EGxTexGen
    constexpr int32_t  kTexGenWorld         = 2;
    constexpr int32_t  kTexGenSphereMap     = 6;
    constexpr uint32_t kXformWorld          = 8;  // EGxXform
    constexpr uint32_t kXformProjection     = 9;
    constexpr uint32_t kXformView           = 10;
    constexpr uint32_t kQueryOcclusion      = 0;  // EGxQueryType
    constexpr uint32_t kQueryParamSize      = 0;  // EGxQueryParam
    constexpr uint32_t kQueryParamAvailable = 1;
    constexpr uint32_t kWmSize              = 0;  // EGxWM
    constexpr uint32_t kWmDisplayChange     = 1;
    constexpr uint32_t kWmDestroy           = 2;
    constexpr uint32_t kWmSetFocus          = 3;
    constexpr uint32_t kWmKillFocus         = 4;
    constexpr uint32_t kSizeMinimized       = 1;  // Gx_Size*
    constexpr uint32_t kSizeMaxHide         = 2;
    constexpr uint32_t kOverridePixelShader = 0;  // EGxOverride
    constexpr uint32_t kOverrideNonPow2     = 7;
    constexpr int32_t  kShVertex            = 0;  // EGxShTarget
    constexpr int32_t  kShPixel             = 4;
    constexpr uint32_t kNormalProjection    = 7;  // EGxMasterEnables
    constexpr uint32_t kMatrixIdentityFlag  = 0x1; // CGxMatrixStack::F_Identity

    // Slots of CGxDevice's vtable the adapter calls through (slots.def).
    constexpr uint32_t kSlotNotifyOnDeviceRestored    = 5;
    constexpr uint32_t kSlotNotifyOnTextureRecreation = 6;
    constexpr uint32_t kSlotDestructor                = 8;
    constexpr uint32_t kSlotDeviceCreate              = 10;

    /// The adapter's one device: the stock client makes one, and makes it again only after
    /// destroying the last.
    struct State
    {
        void*        device = nullptr;    ///< the engine's CGxDevice, holding `table`
        void* const* stock = nullptr;     ///< its own vtable, given back at release
        int          api = 0;             ///< 0 D3D9, 1 D3D9Ex: which stock vtable
        void*        table[kSlotCount] = {};
        const wxl_gx_vk_backend* vk = nullptr;
        HWND         hwnd = nullptr;
        ATOM         window_class = 0;
        bool         made = false;     ///< the Vulkan device exists
        bool         gave_up = false;  ///< the device could not be made again: the client closes
        char         why[512] = {};    ///< why the device could not be made, for the fallback's line
    };

    State                    g;
    const x86::Stock*        g_stock   = nullptr;
    const wxl_gx_vk_backend* g_backend = nullptr;
    void*                    g_factory[2] = {}; ///< the factories' originals (hook chains' next links)

    const x86::Stock& S() { return g_stock ? *g_stock : x86::Live(); }
    const wxl_gx_vk_backend* Backend() { return g_backend ? g_backend : WarcraftXLGxVulkan(); }

    engine::CGxDevice* Dev(void* device) { return static_cast<engine::CGxDevice*>(device); }
    bool Ours(const void* device) { return device && device == g.device; }

    template <class T>
    T& At(void* base, std::size_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
    }

    /// A slot of the device's own vtable, called as the engine calls it.
    template <class R, class... A>
    R Virtual(void* device, uint32_t slot, A... args)
    {
        using Fn = R(__fastcall*)(void*, void*, A...);
        return reinterpret_cast<Fn>((*static_cast<void* const* const*>(device))[slot])(device, nullptr, args...);
    }

    /// A stdcall row (a member of CGxDevice whose own code never reads `this`) called as the engine
    /// calls it, with `this` in ECX all the same: what it calls in turn may read ECX as its own
    /// `this`, which no reading of the function's code alone can rule out.
    template <class R, class... A, class... B>
    R WithThis(R(__stdcall* fn)(A...), void* self, B... args)
    {
        using Fn = R(__fastcall*)(void*, void*, A...);
        return reinterpret_cast<Fn>(fn)(self, nullptr, static_cast<A>(args)...);
    }

    /// The client's gx.log (GxLog), when it is open.
    void GxLine(const char* format, ...)
    {
        char    line[768];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof line, format, args);
        va_end(args);
        S().GxCall_GxLog("%s", line);
    }

    /// Storm's intrusive list: each element's link at a fixed offset, the terminator's next first.
    template <class F>
    void EachOf(const engine::TSList<void>& list, F&& visit)
    {
        uintptr_t node = reinterpret_cast<uintptr_t>(list.m_terminator.m_next);
        for (uint32_t guard = 0; node && !(node & 1) && guard < (1u << 20); ++guard)
        {
            void* element = reinterpret_cast<void*>(node);
            const uintptr_t next = At<uintptr_t>(element, static_cast<std::size_t>(list.m_linkoffset) + sizeof(void*));
            visit(element);
            node = next;
        }
    }

    template <class T, int Offset>
    const engine::TSList<void>& AsList(const engine::TSExplicitList<T, Offset>& list)
    {
        return reinterpret_cast<const engine::TSList<void>&>(list);
    }

    // --- C44Matrix, as the engine multiplies and inverts it (row vectors) -------------------------

    struct Matrix
    {
        float m[4][4];
    };

    Matrix Identity()
    {
        Matrix out{};
        for (int i = 0; i < 4; ++i) out.m[i][i] = 1.0f;
        return out;
    }

    Matrix Multiply(const Matrix& a, const Matrix& b)
    {
        Matrix out{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        return out;
    }

    /// The inverse by cofactors; the matrix itself when it has none.
    Matrix Inverse(const Matrix& in)
    {
        const float* a = &in.m[0][0];
        float        inv[16];
        inv[0]  = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        inv[4]  = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        inv[8]  = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        inv[1]  = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        inv[5]  = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        inv[9]  = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        inv[2]  = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        inv[6]  = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        inv[3]  = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        inv[7]  = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        if (det == 0.0f) return in;
        Matrix out;
        for (int i = 0; i < 16; ++i) (&out.m[0][0])[i] = inv[i] / det;
        return out;
    }

    Matrix& AsMatrix(engine::C44Matrix& matrix) { return reinterpret_cast<Matrix&>(matrix); }
    static_assert(sizeof(engine::C44Matrix) == sizeof(Matrix), "C44Matrix is 16 floats");

    // --- What the D3D9 device does around its own work, done as it does it -------------------------

    void SetWindowFocus(void* self, int32_t focus)
    {
        Dev(self)->m_windowFocus = focus;
        if (focus && !Dev(self)->m_format.m_window && g.hwnd)
        {
            RECT rect;
            GetWindowRect(g.hwnd, &rect);
            ClipCursor(&rect);
        }
    }

    void TexCreate(void* self, engine::CGxTex* tex)
    {
        uint32_t width = 0, height = 0, base = 0, count = 0;
        S().GxCall_ITexWHDStartEnd(self, nullptr, tex, &width, &height, &base, &count);
        if (!g.made || !g.vk->tex_create(self, tex, width, height, count - base)) return;
        tex->m_needsCreation = 0;
    }

    using TexUserFn = x86::CallOf<void(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*, uint32_t*, const void**),
                                  x86::conv_cdecl>::Fn;
    static_assert(std::is_same_v<TexUserFn, decltype(x86::Stock{}.GxCall_TexUserFunc)>, "a texture's callback is called as calls_x86.def's row");

    void TexUpload(void* self, engine::CGxTex* tex)
    {
        const TexUserFn user = reinterpret_cast<TexUserFn>(static_cast<uintptr_t>(tex->m_userFunc));
        uint32_t        stride = 0;
        const void*     texels = nullptr;
        user(kTexLock, tex->m_width, tex->m_height, 0, 0, tex->m_userArg, &stride, &texels);

        uint32_t width = 0, height = 0, base = 0, count = 0;
        S().GxCall_ITexWHDStartEnd(self, nullptr, tex, &width, &height, &base, &count);
        const bool     compressed = tex->m_dataFormat >= kTexDxt1 && tex->m_dataFormat <= kTexDxt1 + 2;
        const uint32_t faces      = tex->m_target == kTexCubeMap ? kCubeFaces : 1;

        // As the D3D9 device: every level latched, its updated rectangle copied by the engine's own
        // Blit into the texture's format, here into what the renderer hands over.
        for (uint32_t face = 0; face < faces; ++face)
        {
            uint32_t mipWidth = width, mipHeight = height, level = 0;
            for (uint32_t mip = base; mip != count; ++mip, ++level)
            {
                texels = nullptr;
                user(kTexLatch, mipWidth, mipHeight, face, mip, tex->m_userArg, &stride, &texels);
                if (texels && !tex->m_flags.m_renderTarget)
                {
                    RECT rect;
                    rect.left   = tex->m_updateRect.minX >> mip;
                    rect.top    = tex->m_updateRect.minY >> mip;
                    rect.bottom = rect.top + 1 > (tex->m_updateRect.maxY >> mip) ? rect.top + 1 : tex->m_updateRect.maxY >> mip;
                    rect.right  = rect.left + 1 > (tex->m_updateRect.maxX >> mip) ? rect.left + 1 : tex->m_updateRect.maxX >> mip;
                    if (tex->m_format - kTexDxt1 <= 2)
                    {
                        rect.left &= ~3;
                        rect.top &= ~3;
                        rect.right  = (rect.right + 3) & ~3;
                        rect.bottom = (rect.bottom + 3) & ~3;
                        if (rect.bottom > static_cast<LONG>(mipHeight)) rect.bottom = mipHeight;
                        if (rect.right > static_cast<LONG>(mipWidth)) rect.right = mipWidth;
                    }
                    uint32_t pitch = 0;
                    char*    dst   = g.vk->tex_lock(self, tex, face, level, rect.left, rect.top, rect.right - rect.left,
                                                    rect.bottom - rect.top, &pitch);
                    if (dst)
                    {
                        const uint8_t* src = static_cast<const uint8_t*>(texels);
                        if (!tex->m_flags.m_bit15)
                        {
                            const uint32_t format = tex->m_dataFormat;
                            if (compressed) src += S().bytes_per_block[format] * (rect.left >> 2) + stride * (rect.top >> 2);
                            else src += ((rect.left * S().bit_depth[format]) >> 3) + stride * rect.top;
                        }
                        const engine::C2iVector size{rect.right - rect.left, rect.bottom - rect.top};
                        S().GxCall_Blit(&size, 0 /* BlitAlpha_0 */, src, stride, S().GxCall_GetBlitFormat(tex->m_dataFormat), dst, pitch,
                                 S().GxCall_GetBlitFormat(tex->m_format));
                        g.vk->tex_unlock(self, tex);
                    }
                }
                mipHeight = mipHeight >> 1 ? mipHeight >> 1 : 1;
                mipWidth  = mipWidth >> 1 ? mipWidth >> 1 : 1;
            }
        }
        texels = nullptr;
        user(kTexUnlock, tex->m_width, tex->m_height, 0, 0, tex->m_userArg, &stride, &texels);
    }

    void TexForceRecreation(void* self, bool all)
    {
        if (!g.made) return;
        EachOf(AsList(Dev(self)->m_textures), [&](void* element) {
            auto* tex = static_cast<engine::CGxTex*>(element);
            if (!tex->m_apiSpecificData || !(all || tex->m_flags.m_renderTarget)) return;
            static const engine::CiRect kNone{};
            g.vk->tex_destroy(self, tex);
            tex->m_needsCreation = 1;
            S().GxCall_TexMarkForUpdate(self, nullptr, tex, &kNone, 0);
            if (tex->m_userFunc)
            {
                uint32_t    stride = 0;
                const void* texels = nullptr;
                reinterpret_cast<TexUserFn>(static_cast<uintptr_t>(tex->m_userFunc))(kTexRecreate, tex->m_width, tex->m_height, 0, 0,
                                                                                     tex->m_userArg, &stride, &texels);
            }
        });
        Virtual<void>(self, kSlotNotifyOnTextureRecreation);
    }

    /// As the D3D9 device's IShaderConstantsFlush: the registers the frame changed reach the device.
    void ShaderConstantsFlush(void* self)
    {
        constexpr std::size_t kStride   = WXL_TYPE_SIZE("ShadowConstants");
        constexpr std::size_t kDirtyMax = WXL_FIELD("ShadowConstants", "dirtyMax");
        constexpr std::size_t kDirtyMin = WXL_FIELD("ShadowConstants", "dirtyMin");
        for (int32_t target = 0; target < 2; ++target)
        {
            // s_shadowConstants[1] is the vertex stage's, [0] the pixel stage's.
            uint8_t*  shadow = S().shadow_constants + kStride * (target == 0 ? 1 : 0);
            uint32_t& max    = At<uint32_t>(shadow, kDirtyMax);
            uint32_t& min    = At<uint32_t>(shadow, kDirtyMin);
            if (min <= max)
                g.vk->shader_constants(self, target, min, max - min + 1, reinterpret_cast<const float*>(shadow) + min * 4);
            max = 0;
            min = 255;
        }
    }

    void SetTexGen(void* self, uint32_t tmu, int32_t texGen)
    {
        engine::CGxDevice* dev = Dev(self);
        if (tmu >= static_cast<uint32_t>(dev->m_caps.m_numTmus)) return;
        Matrix matrix = Identity();
        if (texGen == kTexGenObjectLinear || texGen == kTexGenWorld)
        {
            engine::CGxMatrixStack& view = dev->m_xforms[kXformView];
            matrix                       = Inverse(AsMatrix(view.m_mtx[view.m_level]));
            if (texGen == kTexGenObjectLinear)
            {
                engine::CGxMatrixStack& world = dev->m_xforms[kXformWorld];
                matrix                        = Multiply(matrix, Inverse(AsMatrix(world.m_mtx[world.m_level])));
            }
        }
        else if (texGen == kTexGenSphereMap)
        {
            matrix.m[0][0] = 0.5f;
            matrix.m[1][1] = 0.5f;
            matrix.m[3][0] = 0.5f;
            matrix.m[3][1] = 0.5f;
        }
        else
        {
            S().GxCall_MatrixStackIdentity(&dev->m_texGen[tmu], nullptr);
            return;
        }
        engine::CGxMatrixStack& stack = dev->m_texGen[tmu];
        stack.m_dirty                 = 1;
        stack.m_flags[stack.m_level] &= ~kMatrixIdentityFlag;
        AsMatrix(stack.m_mtx[stack.m_level]) = matrix;
    }

    void* Emergency(void* self, uint32_t target)
    {
        return reinterpret_cast<uint8_t*>(Dev(self)->m_emergencyMem) + WXL_TYPE_SIZE("EmergencyMem") * (target ? 1 : 0);
    }

    char* BufferLock(void* self, engine::CGxBuf* buf)
    {
        engine::CGxPool* pool = buf->m_pool;
        if (!Dev(self)->m_context || !g.made) return static_cast<char*>(S().GxCall_EmergencyLock(Emergency(self, pool->m_target), nullptr, buf->m_size));
        if (pool->m_usage == kPoolUsageStream)
        {
            const uint32_t index = (buf->m_itemSize + pool->m_offset - 1) - (buf->m_itemSize + pool->m_offset - 1) % buf->m_itemSize;
            if (index + buf->m_size > static_cast<uint32_t>(pool->m_size))
            {
                // D3D9's DISCARD: the renderer renames the pool's memory, what the frame drew from it stays.
                g.vk->pool_discard(self, pool);
                S().GxCall_PoolDiscard(pool, nullptr);
                buf->m_index   = 0;
                pool->m_offset = static_cast<int32_t>(buf->m_size);
            }
            else
            {
                buf->m_index   = index;
                pool->m_offset = static_cast<int32_t>(index + buf->m_size);
            }
        }
        char* base = g.vk->pool_memory(self, pool);
        if (!base || buf->m_index + buf->m_size > static_cast<uint32_t>(pool->m_size))
            return static_cast<char*>(S().GxCall_EmergencyLock(Emergency(self, pool->m_target), nullptr, buf->m_size));
        return base + buf->m_index;
    }

    void BufferUnlock(void* self, engine::CGxBuf* buf)
    {
        void* emergency = Emergency(self, buf->m_pool->m_target);
        if (At<uint8_t>(emergency, WXL_FIELD("EmergencyMem", "m_lock")))
        {
            S().GxCall_EmergencyUnlock(emergency, nullptr);
            buf->m_valid             = 0;
            Dev(self)->m_intF5C      = 1;
            return;
        }
        if (g.made && buf->m_size) g.vk->pool_written(self, buf->m_pool, buf->m_index, buf->m_size);
        buf->m_valid = 1;
    }

    /// What the device made for its window goes: the interface's shaders and the pointer.
    void DestroyVk(void* self)
    {
        void** vertex = S().ui_vertex_shaders;
        for (uint32_t i = 0; i < 2; ++i)
            if (vertex[i]) Virtual<void>(*S().device_ptr, 69 /* ShaderDestroy */, static_cast<void*>(&vertex[i]));
        if (*S().ui_pixel_shader) Virtual<void>(*S().device_ptr, 69 /* ShaderDestroy */, static_cast<void*>(S().ui_pixel_shader));
        Virtual<void>(self, 3 /* ICursorDestroy */);
    }

    void SayWhatIsMissing(const char* why);

    uint32_t QueryHandle(void* query) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(At<void*>(query, WXL_FIELD("CGxQuery", "m_apiSpecific")))); }
}

// --- The window ------------------------------------------------------------------------------------

namespace
{
    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    /// The D3D9 device's own class name, so that a tool finding the client's window by its class
    /// finds it under Vulkan too; its procedure is the adapter's.
    constexpr char kClassName[] = "GxWindowClassD3d";

    ATOM RegisterWindowClass()
    {
        HINSTANCE   instance = GetModuleHandleA(nullptr);
        WNDCLASSEXA windowClass{};
        windowClass.cbSize        = sizeof windowClass;
        windowClass.style         = CS_OWNDC;
        windowClass.lpfnWndProc   = WindowProc;
        windowClass.hInstance     = instance;
        windowClass.lpszClassName = kClassName;
        windowClass.hIcon         = static_cast<HICON>(LoadImageA(instance, "BlizzardIcon.ico", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        windowClass.hCursor       = LoadCursorA(instance, "BlizzardCursor.cur");
        if (!windowClass.hCursor) windowClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        return RegisterClassExA(&windowClass);
    }

    /// As the D3D9 device's window, except fullscreen: a borderless window over the monitor, at its
    /// mode, rather than an exclusive display mode. The scene is then the monitor's size.
    bool CreateGameWindow(void* self, engine::CGxFormat& format)
    {
        HINSTANCE instance = GetModuleHandleA(nullptr);
        DWORD     style;
        if (!format.m_window) style = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_SYSMENU;
        else if (format.m_maximize == 1) style = WS_POPUP | WS_VISIBLE;
        else if (format.m_maximize == 2) style = WS_POPUP;
        else style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;

        float aspect = 0.0f;
        if (format.m_aspect && format.m_size.x && format.m_size.y) aspect = static_cast<float>(format.m_size.x) / format.m_size.y;

        RECT client{0, 0, format.m_size.x, format.m_size.y};
        AdjustWindowRectEx(&client, style, FALSE, 0);
        int32_t* adjust = S().client_adjust;
        adjust[0]       = client.right - format.m_size.x - client.left;
        adjust[1]       = client.bottom - format.m_size.y - client.top;

        int32_t x = format.m_pos.x, y = format.m_pos.y, width = format.m_size.x, height = format.m_size.y;
        if (!format.m_window)
        {
            // The monitor the window would have been on, whole.
            POINT       at{format.m_pos.x, format.m_pos.y};
            MONITORINFO monitor{};
            monitor.cbSize = sizeof monitor;
            if (GetMonitorInfoA(MonitorFromPoint(at, MONITOR_DEFAULTTOPRIMARY), &monitor))
            {
                x      = monitor.rcMonitor.left;
                y      = monitor.rcMonitor.top;
                width  = monitor.rcMonitor.right - monitor.rcMonitor.left;
                height = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
            }
        }
        else if (format.m_maximize == 1)
        {
            format.m_pos    = {0, 0};
            format.m_size.x = GetSystemMetrics(SM_CXSCREEN);
            format.m_size.y = GetSystemMetrics(SM_CYSCREEN);
            x = 0, y = 0, width = format.m_size.x, height = format.m_size.y;
        }
        else
        {
            RECT window{0, 0, format.m_size.x, format.m_size.y};
            if (S().GxCall_AdjustWindowRectToWorkArea(aspect, adjust, &window))
            {
                format.m_pos.x  = window.left;
                format.m_pos.y  = window.top;
                format.m_size.x = window.right - window.left;
                format.m_size.y = window.bottom - window.top;
            }
            else format.m_pos = {0, 0};
            x = format.m_pos.x, y = format.m_pos.y, width = format.m_size.x, height = format.m_size.y;
            if (width && height)
            {
                width += adjust[0];
                height += adjust[1];
            }
        }
        if (!width) width = CW_USEDEFAULT;
        if (!height) height = CW_USEDEFAULT;

        g.hwnd = static_cast<HWND>(S().create_window(WS_EX_APPWINDOW, kClassName, "World of Warcraft", style, x, y, width, height,
                                                     nullptr, nullptr, instance, self));
        if (g.hwnd && format.m_maximize != 2) ShowWindow(g.hwnd, SW_SHOWNORMAL);
        return g.hwnd != nullptr;
    }

    // CGxDevice's clamps of a window being sized: its smallest client size, then its aspect.
    void ClampLeft(engine::CiRect& r)
    {
        const int32_t* adjust = S().client_adjust;
        if (r.maxX - r.minX - adjust[0] < S().win_min_size[0]) r.minX = r.maxX - adjust[0] - S().win_min_size[0];
    }
    void ClampRight(engine::CiRect& r)
    {
        const int32_t* adjust = S().client_adjust;
        if (r.maxX - r.minX - adjust[0] < S().win_min_size[0]) r.maxX = S().win_min_size[0] + adjust[0] + r.minX;
    }
    void ClampTop(engine::CiRect& r)
    {
        const int32_t* adjust = S().client_adjust;
        if (r.maxY - r.minY - adjust[1] < S().win_min_size[1]) r.minY = r.maxY - adjust[1] - S().win_min_size[1];
    }
    void ClampBottom(engine::CiRect& r)
    {
        const int32_t* adjust = S().client_adjust;
        if (r.maxY - r.minY - adjust[1] < S().win_min_size[1]) r.maxY = S().win_min_size[1] + adjust[1] + r.minY;
    }
    int32_t HeightByAspect(const engine::CiRect& r)
    {
        const int32_t* adjust = S().client_adjust;
        return static_cast<int32_t>(static_cast<float>(r.maxX - r.minX - adjust[0]) / *S().win_aspect + 0.5f + adjust[1]);
    }
    int32_t WidthByAspect(const engine::CiRect& r)
    {
        const int32_t* adjust = S().client_adjust;
        return static_cast<int32_t>(*S().win_aspect * static_cast<float>(r.maxY - r.minY - adjust[1]) + 0.5f + adjust[0]);
    }
    void ClampTopByAspect(engine::CiRect& r)
    {
        if (*S().win_aspect != 0.0f) r.minY = r.maxY - HeightByAspect(r);
    }
    void ClampBottomByAspect(engine::CiRect& r)
    {
        if (*S().win_aspect != 0.0f) r.maxY = r.minY + HeightByAspect(r);
    }
    void ClampLeftByAspect(engine::CiRect& r)
    {
        if (*S().win_aspect != 0.0f) r.minX = r.maxX - WidthByAspect(r);
    }
    void ClampRightByAspect(engine::CiRect& r)
    {
        if (*S().win_aspect != 0.0f) r.maxX = r.minX + WidthByAspect(r);
    }

    int32_t DeviceWM(void* self, uint32_t wm, uintptr_t param1, uintptr_t param2);

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        void* device = reinterpret_cast<void*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
        if (device && !Ours(device)) device = nullptr;
        switch (message)
        {
        case WM_CREATE:
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTA*>(lparam)->lpCreateParams));
            return 0;
        case WM_DESTROY:
            if (device) DeviceWM(device, kWmDestroy, 0, 0);
            return 0;
        case WM_SIZE:
        {
            if (!device) break;
            engine::CRect rect{0.0f, 0.0f, static_cast<float>(HIWORD(lparam)), static_cast<float>(LOWORD(lparam))};
            const uint32_t kind = wparam == SIZE_MINIMIZED ? kSizeMinimized : wparam == SIZE_MAXHIDE ? kSizeMaxHide : 0u;
            DeviceWM(device, kWmSize, reinterpret_cast<uintptr_t>(&rect), kind);
            break;
        }
        case WM_ACTIVATE:
            // A borderless fullscreen window is not taken away by the system as an exclusive one is:
            // it minimises itself when it loses the focus, and comes back on any activation, a
            // click included. Its WM_SIZE tells the engine.
            if (device && !Dev(device)->m_format.m_window)
            {
                if (LOWORD(wparam) == WA_INACTIVE) ShowWindowAsync(hwnd, SW_MINIMIZE);
                else if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
            }
            break;
        case WM_SETFOCUS:
            if (device) DeviceWM(device, kWmSetFocus, 0, 0);
            return 0;
        case WM_KILLFOCUS:
            if (device) DeviceWM(device, kWmKillFocus, 0, 0);
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 0;
        case WM_SETCURSOR:
            if (!device) return DefWindowProcA(hwnd, message, wparam, lparam);
            if (g.made && LOWORD(lparam) == HTCLIENT)
            {
                g.vk->cursor_set(device);
                return 1;
            }
            break;
        case WM_DISPLAYCHANGE:
            if (device && !Dev(device)->m_format.m_window)
            {
                // The desktop's mode changed under the borderless window: it covers the monitor again.
                MONITORINFO monitor{};
                monitor.cbSize = sizeof monitor;
                if (GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor))
                    SetWindowPos(hwnd, nullptr, monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right - monitor.rcMonitor.left,
                                 monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
                engine::CRect rect{0.0f, 0.0f, static_cast<float>(HIWORD(lparam)), static_cast<float>(LOWORD(lparam))};
                DeviceWM(device, kWmDisplayChange, reinterpret_cast<uintptr_t>(&rect), 0);
            }
            break;
        case WM_SYSCOMMAND:
            if (wparam == SC_SCREENSAVE || wparam == SC_MONITORPOWER) return 0;
            break;
        case WM_SIZING:
        {
            RECT*          sizing = reinterpret_cast<RECT*>(lparam);
            engine::CiRect rect{sizing->top, sizing->left, sizing->bottom, sizing->right};
            if (rect.maxX - rect.minX <= 0 || rect.maxY - rect.minY <= 0) return 0;
            switch (wparam)
            {
            case WMSZ_LEFT: ClampLeft(rect), ClampTopByAspect(rect); break;
            case WMSZ_RIGHT: ClampRight(rect), ClampBottomByAspect(rect); break;
            case WMSZ_TOP: ClampTop(rect), ClampLeftByAspect(rect); break;
            case WMSZ_TOPLEFT: ClampTop(rect), ClampLeft(rect), ClampTopByAspect(rect), ClampLeftByAspect(rect); break;
            case WMSZ_TOPRIGHT: ClampTop(rect), ClampRight(rect), ClampTopByAspect(rect), ClampRightByAspect(rect); break;
            case WMSZ_BOTTOM: ClampBottom(rect), ClampRightByAspect(rect); break;
            case WMSZ_BOTTOMLEFT: ClampBottom(rect), ClampLeft(rect), ClampLeftByAspect(rect), ClampBottomByAspect(rect); break;
            case WMSZ_BOTTOMRIGHT: ClampBottom(rect), ClampRight(rect), ClampBottomByAspect(rect), ClampRightByAspect(rect); break;
            default: break;
            }
            sizing->left   = rect.minX;
            sizing->bottom = rect.maxY;
            sizing->top    = rect.minY;
            sizing->right  = rect.maxX;
            return 1;
        }
        default:
            break;
        }
        if (device && Dev(device)->m_windowProc)
        {
            using Proc = intptr_t(__cdecl*)(void*, uint32_t, uintptr_t, intptr_t);
            return reinterpret_cast<Proc>(static_cast<uintptr_t>(Dev(device)->m_windowProc))(hwnd, message, wparam, lparam);
        }
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }
}

// --- The frame and the loss --------------------------------------------------------------------------

namespace
{
    /// As the D3D9 device's scene begin: a lost device is made again here, the engine told as after
    /// a reset, then the next frame opens.
    void SceneBegin(void* self)
    {
        if (!g.made || g.gave_up) return;
        engine::CGxDevice* dev = Dev(self);
        if (!dev->m_context)
        {
            char why[512] = {};
            if (g.vk->device_restore(self, why, sizeof why) != WXL_OK)
            {
                GxLine("CGxDeviceVk::ISceneBegin(): %s", why);
                g.gave_up = true;
                char text[768];
                std::snprintf(text, sizeof text,
                              "The Vulkan graphics device stopped working (%s).\n\nWorld of Warcraft will close. To play on Direct3D 9, "
                              "set \"api = d3d9\" under [gx] in WarcraftXL.ini.",
                              why);
                MessageBoxA(g.hwnd, text, "World of Warcraft", MB_OK);
                S().GxCall_PostClose();
                return;
            }
            TexForceRecreation(self, true);
            // The queries were the lost device's: made again at their next begin.
            EachOf(AsList(dev->m_queryList), [](void* query) { At<void*>(query, WXL_FIELD("CGxQuery", "m_apiSpecific")) = nullptr; });
            g.vk->device_caps(self);
            g.vk->gamma_changed(self);
            g.vk->target_set(self);
            g.vk->cursor_changed(self);
            SetWindowFocus(self, 1);
            dev->m_context = 1;
            dev->m_intF5C  = 0;
            Virtual<void>(self, kSlotNotifyOnDeviceRestored);
        }
        S().GxCall_ShaderConstantsClear();
        if (g.vk->scene_begin(self) != WXL_GX_VK_FRAME_OK) dev->m_context = 0;
    }

    int32_t DeviceWM(void* self, uint32_t wm, uintptr_t param1, uintptr_t param2)
    {
        engine::CGxDevice* dev = Dev(self);
        switch (wm)
        {
        case kWmSize:
        {
            const engine::CRect* rect = reinterpret_cast<const engine::CRect*>(param1);
            if (param2 == kSizeMinimized || param2 == kSizeMaxHide)
            {
                dev->m_windowVisible = 0;
                if (g.made) g.vk->window_size(self, 0, 0, 1);
                break;
            }
            dev->m_windowVisible = 1;
            S().GxCall_DeviceSetDefWindow(self, nullptr, rect);
            if (g.made && dev->m_context)
            {
                // Not a reset as on D3D9: the swapchain and the scene follow the window, the device
                // and its resources stay. The engine's size-bound targets are made again as after one.
                g.vk->window_size(self, static_cast<uint32_t>(rect->maxX), static_cast<uint32_t>(rect->maxY), 0);
                TexForceRecreation(self, false);
                SetWindowFocus(self, 1);
                Virtual<void>(self, kSlotNotifyOnDeviceRestored);
            }
            dev->m_viewportDirty = 1;
            break;
        }
        case kWmDisplayChange:
            if (dev->m_windowVisible)
            {
                S().GxCall_DeviceSetDefWindow(self, nullptr, reinterpret_cast<const engine::CRect*>(param1));
                dev->m_viewportDirty = 1;
            }
            break;
        case kWmDestroy:
        case kWmKillFocus:
            dev->m_windowFocus = 0;
            break;
        case kWmSetFocus:
            SetWindowFocus(self, 1);
            if (g.made) g.vk->cursor_changed(self);
            break;
        default:
            break;
        }
        return 0;
    }
}

// --- The slots ------------------------------------------------------------------------------------------
//
// One function per `front` row of slots.def, its parameters the row's, __fastcall with a dummy EDX
// for the row's __thiscall (Adapter.hpp). The table below checks each against its row.

namespace wxl::core::gx::x86::slots
{
    void __fastcall ITexMarkAsUpdated(void* self, void*, void* texture)
    {
        auto* tex = static_cast<engine::CGxTex*>(texture);
        if (!tex->m_needsUpdate || !Dev(self)->m_context) return;
        if (tex->m_needsCreation || !tex->m_apiSpecificData) TexCreate(self, tex);
        if (!tex->m_needsCreation && tex->m_apiSpecificData)
        {
            if (tex->m_userFunc) TexUpload(self, tex);
            WithThis(S().GxCall_ITexMarkAsUpdated, self, tex);
        }
    }

    // The renderer reads the render states themselves at each draw; what a state's change sets in
    // motion on the D3D9 device is done here as it does it: a texture made and filled at its first
    // use (ISetTexture), a shader made at its first bind (IBindVertexShader, IBindPixelShader), the
    // texture generation's matrix (ISetTexGen).
    void __fastcall IRsSendToHw(void* self, void*, uint32_t which)
    {
        const auto* states = static_cast<const wxl_gx_app_state*>(Dev(self)->m_appRenderStates.m_data);
        const wxl_gx_state_value& value = states[which].value;
        if (which >= kRsTexture0 && which <= kRsTexture15)
        {
            if (value.data.p) ITexMarkAsUpdated(self, nullptr, value.data.p);
        }
        else if (which == kRsVertexShader || which == kRsPixelShader)
        {
            void* shader = value.data.p;
            if (shader && !At<int32_t>(shader, WXL_FIELD("CGxShader", "m_loaded"))) Virtual<void>(self, 72 /* IShaderCreate */, shader);
        }
        else if (which >= kRsTexGen0 && which <= kRsTexGen7)
            SetTexGen(self, which - kRsTexGen0, value.data.i[0]);
    }

    void __fastcall ICursorCreate(void* self, void*, const void* format)
    {
        S().GxCall_ICursorCreate(self, nullptr, format);
        if (g.made) g.vk->cursor_changed(self);
    }

    void __fastcall ICursorDestroy(void* self, void*) { S().GxCall_ICursorDestroy(self, nullptr); }

    // As the D3D9 device: the software cursor is the engine's quad drawn into the scene at present,
    // before a screenshot reads it back (the hardware one is Windows', in no screenshot). The scene
    // stays open between frames here, so no scene begins or ends around it.
    void __fastcall ICursorDraw(void* self, void*) { S().GxCall_ICursorDraw(self, nullptr); }

    void __fastcall DeviceDestroy(void* self, void*);

    void* __fastcall Destructor(void* self, void*, uint32_t flags)
    {
        void* const* stock = Ours(self) ? g.stock : *static_cast<void* const* const*>(self);
        if (Ours(self))
        {
            DeviceDestroy(self, nullptr);
            *static_cast<void* const**>(self) = g.stock;
            g = State{};
        }
        using Fn = void*(__fastcall*)(void*, void*, uint32_t);
        return reinterpret_cast<Fn>(stock[kSlotDestructor])(self, nullptr, flags);
    }

    int32_t __fastcall DeviceCreateOnWindow(void*, void*, void*, const void*)
    {
        // A window of the caller's (a tool embedding the engine): the client itself never asks.
        GxLine("CGxDeviceVk::DeviceCreate(): a window of the caller's is not supported by the Vulkan device");
        return 0;
    }

    /// The recompiled client's CGxDeviceVk::DeviceCreate: the window class, then CGxDevice's own
    /// DeviceCreate, which calls DeviceSetFormat (below) to make the window and the device.
    bool CreateVulkan(void* self, void* windowProc, const void* format)
    {
        g.vk = Backend();
        if (!g.vk)
        {
            std::snprintf(g.why, sizeof g.why, "the runtime has no Vulkan renderer");
            return false;
        }
        HDC dc = GetDC(nullptr);
        if (GetDeviceGammaRamp(dc, &Dev(self)->m_systemGammaRamp)) Dev(self)->m_gammaRamp = Dev(self)->m_systemGammaRamp;
        ReleaseDC(nullptr, dc);

        g.window_class = RegisterWindowClass();
        if (!g.window_class)
        {
            std::snprintf(g.why, sizeof g.why, "the window class could not be registered (error %lu)", GetLastError());
            DeviceDestroy(self, nullptr);
            return false;
        }
        if (S().GxCall_DeviceCreate(self, nullptr, windowProc, format)) return true;
        DeviceDestroy(self, nullptr);
        return false;
    }

    /// The console command gxVkSimulateLoss, as the recompiled client has it: the device is marked
    /// lost, as a real loss would, and made again at the next frame through the same path.
    int32_t __cdecl SimulateLossCommand(const char*, const char*)
    {
        if (!g.made)
        {
            S().GxCall_ConsoleWrite("gxVkSimulateLoss: the graphics device is not Vulkan", 0 /* DEFAULT_COLOR */);
            return 1;
        }
        g.vk->simulate_loss(g.device);
        S().GxCall_ConsoleWrite("gxVkSimulateLoss: the Vulkan device is marked lost; it is made again at the next frame", 0);
        return 1;
    }

    int32_t __fastcall DeviceCreate(void* self, void*, void* windowProc, const void* format)
    {
        if (Ours(self) && CreateVulkan(self, windowProc, format))
        {
            static bool registered = false;
            if (!registered)
                registered = S().GxCall_CommandRegister("gxVkSimulateLoss", reinterpret_cast<void*>(&SimulateLossCommand), 1 /* CATEGORY_GRAPHICS */,
                                                        "Marks the Vulkan device lost, to test its return") != 0;
            return 1;
        }
        // Vulkan could not start: the device is the D3D9 device it was made as again, its own
        // vtable back, and makes itself with the same format.
        void* const* stock = Ours(self) ? g.stock : *static_cast<void* const* const*>(self);
        if (Ours(self))
        {
            SayWhatIsMissing(g.why);
            *static_cast<void* const**>(self) = g.stock;
            g = State{};
        }
        using Fn = int32_t(__fastcall*)(void*, void*, void*, const void*);
        return reinterpret_cast<Fn>(stock[kSlotDeviceCreate])(self, nullptr, windowProc, format);
    }

    void __fastcall DeviceDestroy(void* self, void*)
    {
        S().GxCall_DeviceDestroy(self, nullptr);
        DestroyVk(self);
        if (g.vk && g.made)
        {
            TexForceRecreation(self, true);
            g.vk->device_destroy(self);
            g.made = false;
        }
        if (g.hwnd)
        {
            DestroyWindow(g.hwnd);
            g.hwnd = nullptr;
        }
        if (g.window_class)
        {
            UnregisterClassA(reinterpret_cast<const char*>(static_cast<uintptr_t>(g.window_class)), GetModuleHandleA(nullptr));
            g.window_class = 0;
        }
        Dev(self)->m_context = 0;
    }

    void __fastcall DeviceEvictResources(void*, void*) {}

    int32_t __fastcall DeviceSetFormat(void* self, void*, const void* requested)
    {
        engine::CGxDevice* dev = Dev(self);
        GxLine("CGxDeviceVk::DeviceSetFormat():");
        WithThis(S().GxCall_LogFormat, self, requested);
        if (!g.vk) return 0;

        char why[512] = {};
        if (g.hwnd) ShowWindow(g.hwnd, SW_HIDE);
        // No frame is drawn while the window is made again: its messages meanwhile only note its size.
        dev->m_context = 0;
        DestroyVk(self);
        // The surface on the old window goes before the window does.
        if (g.made) g.vk->device_set_window(self, nullptr, why, sizeof why);
        if (g.hwnd) DestroyWindow(g.hwnd);
        g.hwnd        = nullptr;
        dev->m_format = *static_cast<const engine::CGxFormat*>(requested);

        engine::CGxFormat format = *static_cast<const engine::CGxFormat*>(requested);
        if (CreateGameWindow(self, format))
        {
            wxl_result made;
            if (g.made) made = g.vk->device_set_window(self, g.hwnd, why, sizeof why);
            else
            {
                made   = g.vk->device_create(self, g.hwnd, why, sizeof why);
                g.made = made == WXL_OK;
            }
            if (made == WXL_OK)
            {
                dev->m_context = 1;
                dev->m_intF5C  = 0;
                g.vk->device_caps(self);
                WithThis(S().GxCall_LogCaps, self, &dev->m_caps);
                if (S().GxCall_DeviceSetFormat(self, nullptr, &format))
                {
                    g.vk->gamma_changed(self);
                    Virtual<void>(self, 2 /* ICursorCreate */, static_cast<const void*>(&format));
                    // As the D3D9 device: the interface's shaders, which the renderer has programs for.
                    Virtual<void>(*S().device_ptr, 68 /* ShaderCreate */, static_cast<void*>(S().ui_vertex_shaders), 0u /* GxSh_Vertex */,
                                  static_cast<const void*>("Shaders\\Vertex"), static_cast<const void*>("UI"), 2u);
                    Virtual<void>(*S().device_ptr, 68 /* ShaderCreate */, static_cast<void*>(S().ui_pixel_shader), 4u /* GxSh_Pixel */,
                                  static_cast<const void*>("Shaders\\Pixel"), static_cast<const void*>("UI"), 1u);
                    // The window's first WM_SIZE came before the device was made: the size it has now.
                    RECT client;
                    if (GetClientRect(g.hwnd, &client))
                    {
                        const engine::CRect rect{0.0f, 0.0f, static_cast<float>(client.bottom - client.top),
                                                 static_cast<float>(client.right - client.left)};
                        S().GxCall_DeviceSetDefWindow(self, nullptr, &rect);
                    }
                    SetWindowFocus(self, 1);
                    SceneBegin(self);
                    return 1;
                }
            }
            else
            {
                GxLine("CGxDeviceVk::DeviceSetFormat(): %s", why);
                std::snprintf(g.why, sizeof g.why, "%s", why);
            }
        }
        else std::snprintf(g.why, sizeof g.why, "the client's window could not be made (error %lu)", GetLastError());

        GxLine("CGxDeviceVk::DeviceSetFormat(): unable to set format!");
        DestroyVk(self);
        if (g.made && g.hwnd) g.vk->device_set_window(self, nullptr, why, sizeof why);
        if (g.hwnd) DestroyWindow(g.hwnd);
        g.hwnd         = nullptr;
        dev->m_context = 0;
        return 0;
    }

    void __fastcall DeviceSetBaseMipLevel(void* self, void*, uint32_t level)
    {
        TexForceRecreation(self, true);
        S().GxCall_DeviceSetBaseMipLevel(self, nullptr, level);
    }

    void __fastcall DeviceSetGammaRamp(void* self, void*, const void* ramp)
    {
        S().GxCall_DeviceSetGammaRamp(self, nullptr, ramp);
        if (g.made) g.vk->gamma_changed(self);
    }

    void __fastcall DeviceSetGamma(void* self, void*, float gamma)
    {
        S().GxCall_DeviceSetGamma(self, nullptr, gamma);
        if (g.made) g.vk->gamma_changed(self);
    }

    void* __fastcall DeviceWindow(void*, void*) { return g.hwnd; }

    // As the D3D9 device: the back buffer's rectangle, resolved when multisampled, as the frame has
    // drawn it so far (the renderer submits what was recorded and waits for it). A read that fails
    // leaves the pixels black and transparent, so a caller that sizes its work by them stays in bounds.
    void __fastcall DeviceReadPixels(void* self, void*, void* rectangle, void* array)
    {
        auto& rect = *static_cast<engine::CiRect*>(rectangle);
        S().GxCall_ClampRectToWindow(self, nullptr, &rect);
        const uint32_t width  = rect.maxX > rect.minX ? rect.maxX - rect.minX : 0;
        const uint32_t height = rect.maxY > rect.minY ? rect.maxY - rect.minY : 0;
        S().GxCall_PixelsSetCount(array, nullptr, width * height);
        if (!(width * height)) return;
        auto* pixels = static_cast<engine::TSGrowableArray<engine::CImVector>*>(array);
        if (!Dev(self)->m_context || !g.made || !g.vk->read_pixels(self, rect.minX, rect.minY, width, height, pixels->m_data))
            std::memset(pixels->m_data, 0, width * height * sizeof(engine::CImVector));
    }

    int32_t __fastcall DeviceWM(void* self, void*, uint32_t wm, uint32_t param1, uint32_t param2)
    {
        return ::DeviceWM(self, wm, param1, param2);
    }

    void __fastcall DeviceSetRenderTarget(void* self, void*, uint32_t buffer, void* texture, uint32_t plane)
    {
        engine::CGxDevice* dev = Dev(self);
        if (!dev->m_context || !g.made) return;
        engine::CGxDevice_TextureTarget& target = dev->m_textureTarget[buffer];
        if (target.m_texture == texture && target.m_plane == plane) return;
        S().GxCall_DeviceSetRenderTarget(self, nullptr, buffer, texture, plane);
        auto* tex = static_cast<engine::CGxTex*>(texture);
        if (tex && (tex->m_needsCreation || !tex->m_apiSpecificData)) TexCreate(self, tex);
        dev->m_viewportDirty    = 1;
        dev->m_scissorRectDirty = 1;
        g.vk->target_set(self);
    }

    // CGxDevice's own: the D3D9 device's depth surfaces have no counterpart on the renderer.
    void __fastcall DeviceSaveDepthSurface(void*, void*) {}
    void __fastcall DeviceRestoreDepthSurface(void*, void*) {}
    void __fastcall DeviceResolveDepthBuffer(void*, void*, void*) {}
    void __fastcall DeviceCopyTex(void*, void*, void*, uint32_t, void*, uint32_t) {}

    void __fastcall DeviceOverride(void* self, void*, uint32_t which, uint32_t value)
    {
        S().GxCall_DeviceOverride(self, nullptr, which, value);
        if (which == kOverridePixelShader) Dev(self)->m_caps.m_shaderTargets[kShPixel] = static_cast<int32_t>(value);
        else if (which == kOverrideNonPow2) Dev(self)->m_caps.m_texNonPow2 = value != 0;
    }

    void __fastcall CapsWindowSize(void* self, void*, void* rect) { *static_cast<engine::CRect*>(rect) = Dev(self)->m_curWindowRect; }

    void __fastcall CapsWindowSizeInScreenCoords(void* self, void*, void* out)
    {
        engine::CRect&       rect   = *static_cast<engine::CRect*>(out);
        const engine::CRect& window = Dev(self)->m_curWindowRect;
        if (Dev(self)->m_format.m_window && g.hwnd)
        {
            POINT points[2] = {{0, 0}, {static_cast<LONG>(window.maxX), static_cast<LONG>(window.maxY)}};
            MapWindowPoints(g.hwnd, nullptr, points, 2);
            rect = {static_cast<float>(points[0].y), static_cast<float>(points[0].x), static_cast<float>(points[1].y),
                    static_cast<float>(points[1].x)};
        }
        else rect = window;
    }

    // The renderer's line on the device, after what the report holds already: its counts of what
    // was drawn and left out, by reason.
    void __fastcall LogCrashInfo(void* self, void*, void* buffer, uint32_t size)
    {
        if (!g.vk || !buffer || !size) return;
        char line[2048];
        g.vk->describe(self, line, sizeof line);
        char*        out  = static_cast<char*>(buffer);
        const size_t used = strnlen(out, size);
        if (used + 1 < size) std::snprintf(out + used, size - used, "%s\r\n", line);
    }

    void __fastcall ScenePresent(void* self, void*)
    {
        engine::CGxDevice* dev = Dev(self);
        if (dev->m_context && g.made)
        {
            const int32_t screenshot = dev->m_scrShotClick;
            S().GxCall_ScenePresent(self, nullptr);
            Virtual<void>(self, 4 /* ICursorDraw */);
            if (screenshot) S().GxCall_DeviceScreenShot(self, nullptr);
            S().GxCall_WaitForFPSCap(self, nullptr);
            if (g.vk->scene_present(self) != WXL_GX_VK_FRAME_OK) dev->m_context = 0;
        }
        SceneBegin(self);
    }

    void __fastcall SceneClear(void* self, void*, uint32_t mask, uint32_t color)
    {
        engine::CGxDevice* dev = Dev(self);
        if (!dev->m_context || !g.made) return;
        // D3D9 clears the viewport: the same rectangle its IXformSetViewport makes (the renderer
        // works out a texture target's). CImVector by value: B, G, R, A from the low byte up.
        const auto&           viewport = *reinterpret_cast<const wxl_gx_viewport*>(dev->m_viewport);
        const engine::CRect&  window   = dev->m_curWindowRect;
        const int32_t x      = static_cast<int32_t>(viewport.x_low * window.maxX + 0.5f);
        const int32_t y      = static_cast<int32_t>((1.0f - viewport.y_high) * window.maxY + 0.5f);
        const int32_t width  = static_cast<int32_t>(viewport.x_high * window.maxX - x + 0.5f);
        const int32_t height = static_cast<int32_t>((1.0f - viewport.y_low) * window.maxY - y + 0.5f);
        g.vk->scene_clear(self, mask, color, x, y, width, height);
    }

    // As the D3D9 device: the base class only keeps the projection; the native one is D3D's, z in [0, 1].
    void __fastcall XformSetProjection(void* self, void*, const void* projection)
    {
        engine::CGxDevice* dev = Dev(self);
        std::memcpy(&dev->m_projection, projection, sizeof dev->m_projection);
        Matrix native = *static_cast<const Matrix*>(projection);
        float (&p)[4][4] = native.m;
        if (!(std::fabs(p[2][3] - 1.0f) < 0.00000023841858f) && !(std::fabs(p[2][3]) < 0.00000023841858f))
        {
            const float scale = 1.0f / p[2][3];
            for (auto& row : p)
                for (float& v : row) v *= scale;
        }
        if (p[3][3] == 0.0f)
        {
            const float minZ = -(p[3][2] / (p[2][2] + 1.0f));
            const float maxZ = -(p[3][2] / (p[2][2] - 1.0f));
            p[2][2]          = maxZ / (maxZ - minZ);
            p[3][2]          = maxZ * minZ / (minZ - maxZ);
        }
        else
        {
            const float inv  = 1.0f / p[2][2];
            const float minZ = (-1.0f - p[3][2]) * inv;
            const float maxZ = (1.0f - p[3][2]) * inv;
            p[2][2]          = 1.0f / (maxZ - minZ);
            p[3][2]          = minZ / (minZ - maxZ);
        }
        if (!(dev->m_appMasterEnables & (1u << kNormalProjection)) && p[3][3] != 1.0f)
        {
            Matrix scale = Identity();
            scale.m[0][0] = scale.m[1][1] = scale.m[2][2] = 0.2f;
            native        = Multiply(native, scale);
        }
        dev->m_xforms[kXformProjection].m_dirty = 1;
        AsMatrix(dev->m_projNative)             = native;
    }

    void __fastcall XformSetView(void* self, void*, const void* matrix) { S().GxCall_XformSetView(self, nullptr, matrix); }

    // As the D3D9 device's Draw: the constants and the render states reach the device first (its
    // IStateSync), then the renderer reads the rest of the draw out of the device's state.
    void __fastcall Draw(void* self, void*, void* batch, int32_t count)
    {
        engine::CGxDevice* dev = Dev(self);
        if (!dev->m_context || dev->m_intF5C || !g.made) return;
        ShaderConstantsFlush(self);
        S().GxCall_IRsSync(self, nullptr, 0);
        g.vk->draw(self, static_cast<const wxl_gx_batch*>(batch), count);
    }

    void __fastcall PoolSizeSet(void* self, void*, void* pool, uint32_t size)
    {
        static_cast<engine::CGxPool*>(pool)->m_size = static_cast<int32_t>(size);
        if (g.made) g.vk->pool_resize(self, pool);
    }

    void __fastcall PoolDestroy(void* self, void*, void* pool)
    {
        if (!pool) return;
        if (g.made) g.vk->pool_destroy(self, pool);
        S().GxCall_PoolDestroy(self, nullptr, pool);
    }

    void* __fastcall BufLock(void* self, void*, void* buf)
    {
        S().GxCall_BufLock(self, nullptr, buf);
        return BufferLock(self, static_cast<engine::CGxBuf*>(buf));
    }

    int32_t __fastcall BufUnlock(void* self, void*, void* buf, uint32_t size)
    {
        S().GxCall_BufUnlock(self, nullptr, buf, size);
        BufferUnlock(self, static_cast<engine::CGxBuf*>(buf));
        return 1;
    }

    void __fastcall BufData(void* self, void*, void* buf, const void* data, uint32_t size, uint32_t offset)
    {
        WithThis(S().GxCall_BufData, self, buf, data, size, offset);
        char* dst = BufferLock(self, static_cast<engine::CGxBuf*>(buf));
        if (dst) std::memcpy(dst + offset, data, size);
        BufferUnlock(self, static_cast<engine::CGxBuf*>(buf));
    }

    void __fastcall TexDestroy(void* self, void*, void* texture)
    {
        if (g.made && static_cast<engine::CGxTex*>(texture)->m_apiSpecificData) g.vk->tex_destroy(self, texture);
        WithThis(S().GxCall_TexDestroy, self, texture);
    }

    int32_t __fastcall TexCopy(void* self, void*, void* dest, const void* destPos, const void* sourcePos, const void* size, uint32_t level,
                               uint32_t plane)
    {
        if (!Dev(self)->m_context || !g.made) return 0;
        auto* tex = static_cast<engine::CGxTex*>(dest);
        if (tex->m_needsCreation || !tex->m_apiSpecificData) TexCreate(self, tex);
        if (tex->m_needsCreation || !tex->m_apiSpecificData) return 0;
        const auto& to = *static_cast<const engine::C2iVector*>(destPos);
        const auto& from = *static_cast<const engine::C2iVector*>(sourcePos);
        const auto& extent = *static_cast<const engine::C2iVector*>(size);
        return g.vk->tex_copy(self, dest, to.x, to.y, from.x, from.y, extent.x, extent.y, level, plane);
    }

    int32_t __fastcall TexStretch(void* self, void*, void* dest, const void* destRect, const void* sourceRect, uint32_t level, uint32_t face)
    {
        if (!Dev(self)->m_context || !g.made) return 0;
        auto* tex = static_cast<engine::CGxTex*>(dest);
        if (tex->m_needsCreation || !tex->m_apiSpecificData) TexCreate(self, tex);
        if (tex->m_needsCreation || !tex->m_apiSpecificData) return 0;
        // CiRect holds minY, minX, maxY, maxX: the order the row hands on.
        return g.vk->tex_stretch(self, dest, static_cast<const int32_t*>(destRect), static_cast<const int32_t*>(sourceRect), level, face);
    }

    void __fastcall TexSetCacheSize(void*, void*, uint32_t) {}

    void __fastcall QueryCreate(void* self, void*, void* queryRef, uint32_t type) { S().GxCall_QueryCreate(self, nullptr, queryRef, type); }

    void __fastcall QueryDestroy(void* self, void*, void* queryRef)
    {
        void* query = *static_cast<void**>(queryRef);
        if (g.made && QueryHandle(query)) g.vk->query_destroy(self, QueryHandle(query));
        At<void*>(query, WXL_FIELD("CGxQuery", "m_apiSpecific")) = nullptr;
        WithThis(S().GxCall_QueryDestroy, self, queryRef);
    }

    bool __fastcall QueryBegin(void* self, void*, void* query)
    {
        if (!Dev(self)->m_context || !g.made || At<uint32_t>(query, WXL_FIELD("CGxQuery", "m_type")) != kQueryOcclusion) return false;
        if (!QueryHandle(query))
        {
            At<void*>(query, WXL_FIELD("CGxQuery", "m_apiSpecific")) = reinterpret_cast<void*>(static_cast<uintptr_t>(g.vk->query_create(self)));
            if (!QueryHandle(query)) return false;
        }
        return g.vk->query_begin(self, QueryHandle(query)) != 0;
    }

    bool __fastcall QueryEnd(void* self, void*, void* query)
    {
        if (!Dev(self)->m_context || !g.made || !QueryHandle(query)) return false;
        return g.vk->query_end(self, QueryHandle(query)) != 0;
    }

    // As the D3D9 device: the first parameter is the result's size, the second whether the result
    // is there (answered in data, the call itself answering false).
    bool __fastcall QueryGetParam(void* self, void*, void* query, uint32_t param, void* data)
    {
        if (!Dev(self)->m_context || !g.made) return false;
        uint32_t& out = *static_cast<uint32_t*>(data);
        if (param == kQueryParamSize)
        {
            out = sizeof(uint32_t);
            return true;
        }
        if (param == kQueryParamAvailable)
        {
            uint32_t samples = 0;
            out = !QueryHandle(query) || g.vk->query_result(self, QueryHandle(query), &samples);
        }
        return false;
    }

    bool __fastcall QueryGetData(void* self, void*, void* query, void* data)
    {
        uint32_t& out = *static_cast<uint32_t*>(data);
        out           = 0;
        if (!Dev(self)->m_context || !g.made || !QueryHandle(query)) return false;
        return g.vk->query_result(self, QueryHandle(query), &out) != 0;
    }

    void __fastcall IShaderCreate(void* self, void*, void* shader);

    void __fastcall ShaderCreate(void* self, void*, void* shaders, uint32_t target, const void* path, const void* name, uint32_t permutations)
    {
        S().GxCall_ShaderCreate(self, nullptr, shaders, target, static_cast<const char*>(path), static_cast<const char*>(name), permutations);
        void** list = static_cast<void**>(shaders);
        // Every permutation's record, just loaded: the renderer translates them ahead of their first bind.
        if (g.made && permutations && (static_cast<int32_t>(target) == kShVertex || static_cast<int32_t>(target) == kShPixel))
        {
            const void* codes[64];
            uint32_t    sizes[64];
            uint32_t    count = 0;
            for (uint32_t i = 0; i < permutations && count < 64; ++i)
            {
                if (!list[i] || !At<uint32_t>(list[i], WXL_FIELD("CGxShader", "m_code.m_count"))) continue;
                codes[count] = At<const void*>(list[i], WXL_FIELD("CGxShader", "m_code.m_data"));
                sizes[count] = At<uint32_t>(list[i], WXL_FIELD("CGxShader", "m_code.m_count"));
                ++count;
            }
            if (count) g.vk->shader_prewarm(self, codes, sizes, count);
        }
        if (permutations == 1 && list[0] && !At<int32_t>(list[0], WXL_FIELD("CGxShader", "m_loaded"))) IShaderCreate(self, nullptr, list[0]);
    }

    void __fastcall ShaderDestroy(void* self, void*, void* shaderRef)
    {
        void* shader = *static_cast<void**>(shaderRef);
        if (shader && At<uint32_t>(shader, WXL_FIELD("CGxShader", "m_refCount")) == 1)
        {
            if (g.made) g.vk->shader_destroy(self, shader);
            At<int32_t>(shader, WXL_FIELD("CGxShader", "m_loaded")) = 0;
            At<int32_t>(shader, WXL_FIELD("CGxShader", "m_valid"))  = 0;
        }
        S().GxCall_ShaderDestroy(self, nullptr, shaderRef);
    }

    void __fastcall IShaderReload(void*, void*, void*, const void*, const void*) {}

    // As the D3D9 device's IShaderCreateVertex/Pixel: made when the context is up, from its record
    // (the renderer translates the DX9 bytecode, or uses its own program for the interface's "UI"
    // and "Desaturate"). A record the renderer refuses stays invalid, as one D3D9 refuses.
    void __fastcall IShaderCreate(void* self, void*, void* shader)
    {
        At<int32_t>(shader, WXL_FIELD("CGxShader", "m_valid")) = 0;
        const int32_t target = At<int32_t>(shader, WXL_FIELD("CGxShader", "m_target"));
        if (!Dev(self)->m_context || !g.made || (target != kShVertex && target != kShPixel)) return;
        At<int32_t>(shader, WXL_FIELD("CGxShader", "m_loaded")) = 1;
        const char*    name = At<const char*>(shader, WXL_FIELD("CGxShader", "m_key.m_str"));
        const uint32_t size = At<uint32_t>(shader, WXL_FIELD("CGxShader", "m_code.m_count"));
        const void*    code = size ? At<const void*>(shader, WXL_FIELD("CGxShader", "m_code.m_data")) : nullptr;
        if (g.vk->shader_create(self, shader, target == kShVertex, name ? name : "", code, size))
            At<int32_t>(shader, WXL_FIELD("CGxShader", "m_valid")) = 1;
    }

    void __fastcall CursorSetVisible(void* self, void*, int32_t visible)
    {
        S().GxCall_CursorSetVisible(self, nullptr, visible);
        if (g.made && Dev(self)->m_context) g.vk->cursor_changed(self);
    }

    void __fastcall CursorUnlock(void* self, void*, uint32_t x, uint32_t y)
    {
        S().GxCall_CursorUnlock(self, nullptr, x, y);
        if (g.made) g.vk->cursor_changed(self);
    }

    void __fastcall StereoSetConvergence(void*, void*, float) {}
    float __fastcall StereoGetConvergence(void*, void*) { return 0.0f; }
    void __fastcall StereoSetSeparation(void*, void*, float) {}
    float __fastcall StereoGetSeparation(void*, void*) { return 0.0f; }
    bool __fastcall StereoEnabled(void*, void*) { return false; }
}

// Every `front` row against its function: the C++ type is the row's signature as __thiscall, and
// the bytes it pops are the row's (check_conventions.py holds the row's bytes to Wow.exe's own).
#define WXL_GX_SLOT(index, name, ret, params, pops, kind, d3d9, d3d9_at, d3d9ex, d3d9ex_at) WXL_GX_SLOT_CHECK_##kind(name, ret, params, pops)
#define WXL_GX_SLOT_CHECK_keep(name, ret, params, pops)
#define WXL_GX_SLOT_CHECK_front(name, ret, params, pops)                                                                          \
    static_assert(std::is_same_v<decltype(&wxl::core::gx::x86::slots::name), wxl::core::gx::x86::Thiscall<ret params>::Fn>,     \
                  "slot " #name ": the adapter's function has the row's signature");                                            \
    static_assert(wxl::core::gx::x86::Thiscall<ret params>::kPops == pops, "slot " #name ": the row's parameters pop its bytes");
#include "wxl/gx/slots.def"
#undef WXL_GX_SLOT_CHECK_keep
#undef WXL_GX_SLOT_CHECK_front

// --- The vtable copy and the factories --------------------------------------------------------------------

namespace
{
    struct Row
    {
        uint32_t index;
        bool     front;
        void*    slot;      ///< the adapter's function, for a front row
        uint32_t entry[2];  ///< the stock entries, D3D9 and D3D9Ex
    };

    const Row kRows[] = {
#define WXL_GX_SLOT(index, name, ret, params, pops, kind, d3d9, d3d9_at, d3d9ex, d3d9ex_at) \
    {index, WXL_GX_SLOT_FRONT_##kind, WXL_GX_SLOT_FN_##kind(name), {d3d9_at, d3d9ex_at}},
#define WXL_GX_SLOT_FRONT_keep  false
#define WXL_GX_SLOT_FRONT_front true
#define WXL_GX_SLOT_FN_keep(name)  nullptr
#define WXL_GX_SLOT_FN_front(name) reinterpret_cast<void*>(&wxl::core::gx::x86::slots::name)
#include "wxl/gx/slots.def"
    };
    static_assert(sizeof kRows / sizeof kRows[0] == kSlotCount, "slots.def has a row per slot");

    /// The factories' detours: the stock device, made, then given the adapter's vtable.
    template <int Api>
    void* __cdecl Factory()
    {
        using Fn       = void*(__cdecl*)();
        void* device   = reinterpret_cast<Fn>(g_factory[Api])();
        if (device) x86::Adopt(device);
        return device;
    }

    /// The INFO line that says what an x86 machine lacks to run the renderer, from why it did not start.
    void SayWhatIsMissing(const char* why)
    {
        BOOL wow64 = FALSE;
        IsWow64Process(GetCurrentProcess(), &wow64);
        const bool wine = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != nullptr;
        const char* where = wine ? "under Wine or Proton, the host's 32-bit Vulkan driver (for NVIDIA the 32-bit part of the driver, "
                                   "lib32-nvidia-utils or the driver's i386 libraries; for Mesa lib32-vulkan-radeon / lib32-vulkan-intel "
                                   "or mesa-vulkan-drivers:i386)"
                                 : "the 32-bit part of the graphics driver (installed with the driver on 64-bit Windows; reinstall the "
                                   "driver if it is missing)";
        if (std::strstr(why, "no Vulkan loader"))
            WLOG_INFO(kTag, "Vulkan on the 32-bit client: no 32-bit Vulkan loader (vulkan-1.dll in %s) was found; it comes with %s. "
                            "The client goes on with Direct3D 9",
                      wow64 ? "SysWOW64" : "System32", where);
        else if (std::strstr(why, "finds no device"))
            WLOG_INFO(kTag, "Vulkan on the 32-bit client: the 32-bit Vulkan loader is there, but no 32-bit Vulkan driver (ICD) answers "
                            "it; it comes with %s. The client goes on with Direct3D 9",
                      where);
        else
            WLOG_INFO(kTag, "Vulkan on the 32-bit client did not start (%s). The client goes on with Direct3D 9", why[0] ? why : "no reason given");
    }
}

namespace wxl::core::gx::x86
{
    bool Adopt(void* device)
    {
        if (!device) return false;
        void* const* vtable = *static_cast<void* const* const*>(device);
        const int    api    = vtable == S().vtables[0] ? 0 : vtable == S().vtables[1] ? 1 : -1;
        if (api < 0)
        {
            WLOG_WARN(kTag, "Vulkan: the client made a device whose vtable (0x%p) is neither D3D9's nor D3D9Ex's; it stays on its own",
                      static_cast<const void*>(vtable));
            return false;
        }
        if (g.device)
        {
            WLOG_WARN(kTag, "Vulkan: a second device was made while the first draws through the renderer; it stays on Direct3D 9");
            return false;
        }
        State next{};
        next.device = device;
        next.stock  = vtable;
        next.api    = api;
        for (const Row& row : kRows)
        {
            // The live vtable must hold what slots.def was generated from: anything else patched it.
            if (S().check_entries && reinterpret_cast<uintptr_t>(vtable[row.index]) != row.entry[api])
            {
                WLOG_WARN(kTag, "Vulkan: slot %u of the %s device's vtable holds 0x%p, not Wow.exe's 0x%08X (something patched it); the "
                                "client stays on Direct3D 9",
                          row.index, api ? "D3D9Ex" : "D3D9", vtable[row.index], row.entry[api]);
                return false;
            }
            next.table[row.index] = row.front ? row.slot : vtable[row.index];
        }
        g = next;
        *static_cast<void* const**>(device) = g.table;
        WLOG_INFO(kTag, "Vulkan: the client's %s device is drawn by the Vulkan renderer (%u of its %u slots taken over); Direct3D 9 if "
                        "Vulkan cannot start",
                  api ? "D3D9Ex" : "D3D9", static_cast<uint32_t>(std::count_if(std::begin(kRows), std::end(kRows), [](const Row& r) { return r.front; })),
                  kSlotCount);
        return true;
    }

    bool Adopted(const void* device) { return Ours(device); }

    void* AdoptedDevice() { return g.device; }

    void* const* VtableOf(const void* device) { return device ? *static_cast<void* const* const*>(device) : nullptr; }

    void UseForTests(const Stock* stock, const wxl_gx_vk_backend* backend)
    {
        g_stock   = stock;
        g_backend = backend;
        g         = State{};
    }

    bool Install()
    {
        static bool installed = false;
        if (installed) return true;
        if (session::CurrentSettings().gx_api != settings::GxApi::vulkan) return true;
        installed = true;
        bool ok   = true;
        const char* const names[2] = {"CreateCGxDeviceD3d", "CreateCGxDeviceD3d9Ex"};
        void* const detours[2] = {reinterpret_cast<void*>(&Factory<0>), reinterpret_cast<void*>(&Factory<1>)};
        uintptr_t addresses[2] = {};
#define WXL_GX_VTABLE(api, address, slots, factory) addresses[api##_index] = WXL_SEAM_SITE(factory, 0);
        constexpr int d3d9_index = 0, d3d9ex_index = 1;
#include "wxl/gx/slots.def"
        for (int i = 0; i < 2; ++i)
        {
            if (wxl::hook::Attach(names[i], reinterpret_cast<void*>(addresses[i]), detours[i], &g_factory[i]) == wxl::hook::kNone)
            {
                WLOG_WARN(kTag, "Vulkan: %s could not be detoured; its devices stay on Direct3D 9", names[i]);
                ok = false;
            }
        }
        return ok;
    }
}
