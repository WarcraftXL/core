// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Vulkan renderer as the recompiled client's device reaches it. The renderer is
// WarcraftXL.dll's (src/gx/vk/, one source for both architectures); the client's CGxDeviceVk is a
// thin adapter whose overrides call through this table, one line each. The client takes the
// table once, from WarcraftXLGxVulkan, and checks it before it makes a Vulkan device:
//
//     const wxl_gx_vk_backend* vk = WarcraftXLGxVulkan();
//     if (!vk || vk->struct_size < sizeof(wxl_gx_vk_backend) ||
//         vk->device_layout_hash != wxl::engine::CGxDevice::kLayoutHash) -> stay on D3D9
//
// Which API the client creates is decided here too: WarcraftXL.ini's [gx] api, over the client's
// own choice (its gxApi CVar and its command line).

#ifndef WXL_CORE_GX_VK_H
#define WXL_CORE_GX_VK_H

#include <stdint.h>

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
     *        Logs a change. Called once, on the main thread, before the device is created.
     */
    int32_t(WXL_CALL* select_api)(int32_t requested);

    /**
     * @brief Makes the Vulkan device for the engine's device @p device, which presents into
     *        @p window (an HWND; null before the engine has made it). Logs the device taken, or
     *        why none was.
     * @param why  a sentence when it fails, for the client's own log
     * @return WXL_OK when the device renders through Vulkan; otherwise the client releases its
     *         Vulkan device and creates D3D9 in its place. This runtime does not draw through
     *         Vulkan yet: it makes the device, logs it, releases it and answers WXL_E_UNSUPPORTED.
     */
    wxl_result(WXL_CALL* device_create)(void* device, void* window, char* why, uint32_t why_size);

    /// Releases what device_create made for @p device; nothing when it made nothing.
    void(WXL_CALL* device_destroy)(void* device);
} wxl_gx_vk_backend;

/// The renderer's table; never null from a runtime that exports it.
WXL_CORE_API const wxl_gx_vk_backend* WXL_CALL WarcraftXLGxVulkan(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // WXL_CORE_GX_VK_H
