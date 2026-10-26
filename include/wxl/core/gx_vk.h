// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Vulkan renderer as the recompiled client's device reaches it. The renderer is
// WarcraftXL.dll's (src/gx/vk/, one source for both architectures); the client's CGxDeviceVk is a
// thin adapter: it keeps the engine's own logic of each device call (as the D3D9 device does) and
// hands the Vulkan work to this table. The client takes the table once, from WarcraftXLGxVulkan,
// and checks it before it makes a Vulkan device:
//
//     const wxl_gx_vk_backend* vk = WarcraftXLGxVulkan();
//     if (!vk || vk->struct_size < sizeof(wxl_gx_vk_backend) ||
//         vk->device_layout_hash != wxl::engine::CGxDevice::kLayoutHash) -> stay on D3D9
//
// Which API the client creates is decided here too: WarcraftXL.ini's [gx] api, over the client's
// own choice (its gxApi CVar and its command line).
//
// Every row is called on the client's main thread. `device` is always the engine's CGxDevice the
// Vulkan device was made for; a row called with another one does nothing.

#ifndef WXL_CORE_GX_VK_H
#define WXL_CORE_GX_VK_H

#include <stdint.h>

#include "wxl/core/gx_state.h"
#include "wxl/core/init.h"

#ifdef __cplusplus
extern "C" {
#endif

/// The client's EGxApi values this table speaks in.
enum
{
    WXL_GX_API_OPENGL = 0,
    WXL_GX_API_D3D9   = 1,
    WXL_GX_API_D3D9EX = 2,
    WXL_GX_API_VULKAN = 6, ///< appended to the client's EGxApi after GxApi_GLL
};

/// What a frame came to (scene_begin, scene_present).
enum
{
    WXL_GX_VK_FRAME_OK   = 0,
    WXL_GX_VK_FRAME_LOST = 1, ///< the device was lost: the client sets m_context to 0 and restores it at its next scene
};

typedef struct wxl_gx_vk_backend
{
    uint32_t struct_size;
    uint32_t reserved;
    /// The layout hash of CGxDevice this runtime reads the engine's device by
    /// (wxl::engine::CGxDevice::kLayoutHash of its architecture); a client whose own differs
    /// stays on D3D9.
    uint64_t device_layout_hash;

    /**
     * @brief The API the client creates, given the one it asked for (its gxApi CVar, then its
     *        command line): WarcraftXL.ini's `[gx] api = vulkan` turns a D3D9 or D3D9Ex request into
     *        Vulkan, `[gx] api = d3d9` a Vulkan one into D3D9; without the line the request stands.
     *        Logs a change. Called once, before the device is created.
     */
    int32_t(WXL_CALL* select_api)(int32_t requested);

    /**
     * @brief Makes the Vulkan device for the engine's device @p device, presenting into @p window
     *        (an HWND, the client's window, already made), and shows a first frame on it. Logs the
     *        device taken, or why none was.
     * @param why  a sentence when it fails, for the client's own log
     * @return WXL_OK when the client renders through Vulkan; otherwise nothing is kept and the
     *         client creates D3D9 in its place.
     */
    wxl_result(WXL_CALL* device_create)(void* device, void* window, char* why, uint32_t why_size);

    /// Releases what device_create made for @p device; nothing when it made nothing.
    void(WXL_CALL* device_destroy)(void* device);

    /**
     * @brief The client's window changes (DeviceSetFormat makes it again): null before the old one
     *        is destroyed (its surface goes now, the frames on it completed), then the new one.
     * @return WXL_OK, or a failure with @p why (the client treats it as a failed format).
     */
    wxl_result(WXL_CALL* device_set_window)(void* device, void* window, char* why, uint32_t why_size);

    /// Fills the engine's capabilities (CGxDevice::m_caps) for the device made.
    void(WXL_CALL* device_caps)(void* device);

    /// The client area's size (WM_SIZE), or that the window was minimised.
    void(WXL_CALL* window_size)(void* device, uint32_t width, uint32_t height, int32_t minimized);

    /// Opens the next frame, as the D3D9 device's scene begin: WXL_GX_VK_FRAME_OK or _LOST.
    int32_t(WXL_CALL* scene_begin)(void* device);

    /// Clears the render target inside the viewport (as D3D9 does; @p x, @p y, @p width and
    /// @p height are the back buffer's, a texture target's is the runtime's to work out): its
    /// colour (0xAARRGGBB) when @p mask has 1, its depth when it has 2.
    void(WXL_CALL* scene_clear)(void* device, uint32_t mask, uint32_t argb, int32_t x, int32_t y, int32_t width, int32_t height);

    /**
     * @brief A draw of the engine (CGxDevice::Draw, @p batch a CGxBatch, indexed when @p count is
     *        not 0), its render states synced (IRsSync) and its constants flushed
     *        (shader_constants): drawn from the device's state as the D3D9 device's IStateSync
     *        reads it, into the render target target_set last named. A draw binding a client
     *        shader whose record the translator refused is counted and left out.
     */
    void(WXL_CALL* draw)(void* device, const wxl_gx_batch* batch, int32_t count);

    /// Shows the open frame: WXL_GX_VK_FRAME_OK, or _LOST when the device was lost.
    int32_t(WXL_CALL* scene_present)(void* device);

    /**
     * @brief After a loss, at the next scene: the device made again on the same window.
     * @return WXL_OK; WXL_E_FAILED with @p why when it cannot be, or after three losses within a
     *         minute (the client then tells the player and closes).
     */
    wxl_result(WXL_CALL* device_restore)(void* device, char* why, uint32_t why_size);

    /// The engine's gamma ramp (m_gammaRamp) or its window mode changed.
    void(WXL_CALL* gamma_changed)(void* device);

    /// The engine's pointer changed: its pixels and hotspot (m_cursor), or whether it shows.
    void(WXL_CALL* cursor_changed)(void* device);

    /// WM_SETCURSOR over the client area: the runtime sets the pointer the engine wants.
    void(WXL_CALL* cursor_set)(void* device);

    /// The memory of a vertex or index pool (CGxPool, m_size bytes), made at its first lock; null
    /// when none can be (the client then writes into its emergency memory).
    char*(WXL_CALL* pool_memory)(void* device, void* pool);

    /// A pool's size changed (m_size): its memory is made again at its next lock.
    void(WXL_CALL* pool_resize)(void* device, void* pool);

    /// A pool goes: its memory with it.
    void(WXL_CALL* pool_destroy)(void* device, void* pool);

    /**
     * @brief A texture (CGxTex) is made on the device, @p width x @p height at its first level
     *        with @p levels levels (CGxDevice::ITexWHDStartEnd), its format, target and flags read
     *        from it: its m_apiSpecificData set. 1 when made.
     */
    int32_t(WXL_CALL* tex_create)(void* device, void* tex, uint32_t width, uint32_t height, uint32_t levels);

    /// A texture goes, or is to be made again: what tex_create made goes, m_apiSpecificData is null.
    void(WXL_CALL* tex_destroy)(void* device, void* tex);

    /// The console command gxVkSimulateLoss: the device is marked lost, as a real loss would.
    void(WXL_CALL* simulate_loss)(void* device);

    /// One line on the device for the client's crash report.
    void(WXL_CALL* describe)(void* device, char* out, uint32_t out_size);

    /// The rows @p first.. of the vertex (@p target 0) or pixel (1) program registers changed
    /// (CGxDevice::s_shadowConstants' dirty range, as the D3D9 device's IShaderConstantsFlush sends it).
    void(WXL_CALL* shader_constants)(void* device, int32_t target, uint32_t first, uint32_t rows, const float* values);

    /**
     * @brief A client shader (CGxShader) is made on the device: by the name of its record ("UI:0",
     *        "Desaturate") when the renderer has a program of its own for it, otherwise from its DX9
     *        record (@p code, @p size bytes: CGxShader::m_code) translated. 1 when it has a program
     *        (the client marks it valid), 0 when the record is refused (it stays invalid, as one
     *        D3D9 refuses).
     */
    int32_t(WXL_CALL* shader_create)(void* device, void* shader, int32_t vertex, const char* name, const void* code, uint32_t size);

    /// A client shader goes.
    void(WXL_CALL* shader_destroy)(void* device, void* shader);

    /**
     * @brief Where the texels of @p width x @p height at (@p x, @p y) of @p level of @p face of a
     *        texture are written, in the layout of its format (rows of 4x4 blocks when compressed,
     *        the rectangle on block edges), @p pitch bytes per row; null when they cannot be. One
     *        lock at a time per texture, closed by tex_unlock.
     */
    char*(WXL_CALL* tex_lock)(void* device, void* tex, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t width,
                              uint32_t height, uint32_t* pitch);

    /// What was written since tex_lock goes to the texture.
    void(WXL_CALL* tex_unlock)(void* device, void* tex);

    /// A stream pool starts again from 0 (its DISCARD): what the frame drew from it stays.
    void(WXL_CALL* pool_discard)(void* device, void* pool);

    /// @p size bytes at @p offset of a pool's memory were written (its buffer unlocked).
    void(WXL_CALL* pool_written)(void* device, void* pool, uint32_t offset, uint32_t size);

    /// The records of a shader's @p count permutations, just loaded (CGxDevice::ShaderCreate):
    /// translated ahead, off the main thread, before their first bind.
    void(WXL_CALL* shader_prewarm)(void* device, const void* const* codes, const uint32_t* sizes, uint32_t count);

    /// The engine's render target changed (CGxDevice::m_textureTarget, after DeviceSetRenderTarget;
    /// the client makes a target texture first, as the D3D9 device does).
    void(WXL_CALL* target_set)(void* device);

    /// D3D9's TexCopy: @p width x @p height of the back buffer at (@p source_x, @p source_y) into
    /// @p tex's @p level (@p plane of a cube) at (@p dest_x, @p dest_y). 1 when copied.
    int32_t(WXL_CALL* tex_copy)(void* device, void* tex, int32_t dest_x, int32_t dest_y, int32_t source_x, int32_t source_y, int32_t width,
                                int32_t height, uint32_t level, uint32_t plane);

    /// D3D9's TexStretch: the back buffer's @p source rectangle into @p tex's @p dest one, each
    /// (minY, minX, maxY, maxX) as CiRect holds it, or null for the whole surface; filtered when
    /// the sizes differ. 1 when copied.
    int32_t(WXL_CALL* tex_stretch)(void* device, void* tex, const int32_t* dest, const int32_t* source, uint32_t level, uint32_t face);

    /// An occlusion query (CGxQuery): its handle (0 when there are none: the device counts no samples).
    uint32_t(WXL_CALL* query_create)(void* device);
    void(WXL_CALL* query_destroy)(void* device, uint32_t query);
    /// The samples of the draws between begin and end are counted. 1 when the query was issued.
    int32_t(WXL_CALL* query_begin)(void* device, uint32_t query);
    int32_t(WXL_CALL* query_end)(void* device, uint32_t query);
    /// 1 with @p samples when the query's count is known (a query never issued is, with 0), 0 while
    /// the GPU has not finished it: never waits.
    int32_t(WXL_CALL* query_result)(void* device, uint32_t query, uint32_t* samples);

    /**
     * @brief D3D9's readback of the back buffer (DeviceReadPixels: screenshots, the portraits'
     *        check of the target's alpha): @p width x @p height pixels at (@p x, @p y) of the frame
     *        drawn so far, resolved when multisampled, as CImVector rows (B, G, R, A bytes) into
     *        @p bgra; what lies outside the back buffer reads as zero. The frame recorded so far is
     *        submitted and waited for (bounded), then goes on. 1 when read.
     */
    int32_t(WXL_CALL* read_pixels)(void* device, int32_t x, int32_t y, uint32_t width, uint32_t height, void* bgra);

    /// The samples per pixel the device draws the back buffer with: bit n set when it draws 2^n
    /// (bit 0, one sample, always), for the formats the client offers (gxMultisample). A format
    /// asking for a count the device lacks gets the most it has below it.
    uint32_t(WXL_CALL* sample_counts)(void* device);
} wxl_gx_vk_backend;

/// The renderer's table; never null from a runtime that exports it.
WXL_CORE_API const wxl_gx_vk_backend* WXL_CALL WarcraftXLGxVulkan(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // WXL_CORE_GX_VK_H
