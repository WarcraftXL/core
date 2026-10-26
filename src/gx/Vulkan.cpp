// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The renderer inside the runtime: the table the recompiled client's CGxDeviceVk calls
// (wxl/core/gx_vk.h), the Vulkan device made from WarcraftXL.ini's [gx] section, and on the stock
// client, which has no Vulkan adapter, the same device made once its own D3D9 device is up.
//
// This runtime does not draw through Vulkan yet. Selecting it makes the device, logs which one
// and what it offers, checks that a submission comes back, and releases it: the client renders
// through D3D9 as before.

#include "gx/Vulkan.hpp"

#include "common/Log.hpp"
#include "gx/Select.hpp"
#include "gx/vk/Log.hpp"
#include "gx/vk/device/Device.hpp"
#include "gx/vk/frame/Frame.hpp"
#include "gx/vk/present/Present.hpp"
#include "gx/vk/resources/Resources.hpp"
#include "runtime/Session.hpp"

#include "wxl/core/gx_vk.h"
#include "wxl/engine/gx.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace
{
    constexpr char kTag[] = "gx";

    namespace vk = wxl::vk;

    void Sink(vk::LogLevel level, const char* text, void*)
    {
        using wxl::log::Level;
        const Level to = level == vk::LogLevel::Error  ? Level::Error
                         : level == vk::LogLevel::Warn ? Level::Warn
                         : level == vk::LogLevel::Info ? Level::Info
                                                       : Level::Debug;
        wxl::log::Write(to, kTag, "Vulkan: %s", text);
    }

    /// The renderer's objects, from device_create to device_destroy.
    struct Renderer
    {
        vk::device::Context     context;
        vk::resources::Allocator allocator;
        vk::frame::Submitter    submitter;
    };

    Renderer* g_renderer = nullptr;
    void*     g_owner    = nullptr;
    bool      g_tried    = false; ///< a device was asked for in this launch

    /// "anisotropy, BC textures, ..." for the features and extensions @p want says.
    void Optional(const vk::device::Candidate& c, bool want, char* out, size_t size)
    {
        struct Row
        {
            bool        has;
            const char* name;
        };
        const Row rows[] = {
            {c.features.samplerAnisotropy, "anisotropy"},
            {c.features.textureCompressionBC, "BC textures"},
            {c.features.largePoints, "large points"},
            {c.features.fillModeNonSolid, "wireframe"},
            {c.features.shaderClipDistance, "clip distance"},
            {c.features.independentBlend, "independent blend"},
            {c.features.depthBiasClamp, "depth bias clamp"},
            {c.extensions.memory_budget, "memory budget"},
            {c.extensions.graphics_pipeline_library && c.extensions.pipeline_library, "pipeline libraries"},
            {c.extensions.extended_dynamic_state3, "extended dynamic state 3"},
            {c.extensions.vertex_input_dynamic_state, "dynamic vertex input"},
            {c.extensions.unified_image_layouts, "unified image layouts"},
            {c.extensions.calibrated_timestamps, "calibrated timestamps"},
        };
        size_t used = 0;
        out[0]      = '\0';
        for (const Row& row : rows)
        {
            if (row.has != want) continue;
            const int wrote = std::snprintf(out + used, size - used, "%s%s", used ? ", " : "", row.name);
            if (wrote < 0 || static_cast<size_t>(wrote) >= size - used) break;
            used += static_cast<size_t>(wrote);
        }
        if (!used) std::snprintf(out, size, "none");
    }

    /// The device line: what was taken, and what it offers beyond the floor.
    void Report(const Renderer& r, double milliseconds)
    {
        const vk::device::Candidate& c = r.context.Chosen();
        char api[32], driver[32], loader[32];
        vk::FormatVersion(c.api, api, sizeof api);
        vk::device::FormatDriver(c.vendor, c.driver, driver, sizeof driver);
        vk::FormatVersion(r.context.InstanceApi(), loader, sizeof loader);
        const vk::resources::Budget budget = r.allocator.CurrentBudget();
        WLOG_INFO(kTag, "Vulkan: device %u, %s (%s, Vulkan %s, driver %s, %04x:%04x, %llu MiB device memory, budget %llu MiB), "
                        "queue family %u, loader %s, validation %s, made in %.0f ms",
                  c.index, c.name, vk::device::TypeName(c.type), api, driver, c.vendor, c.device_id,
                  static_cast<unsigned long long>(c.device_local >> 20), static_cast<unsigned long long>(budget.budget >> 20),
                  c.family, loader, r.context.Validation() ? "on" : "off", milliseconds);
        char has[512], lacks[512];
        Optional(c, true, has, sizeof has);
        Optional(c, false, lacks, sizeof lacks);
        WLOG_INFO(kTag, "Vulkan: beyond the 1.3 floor it offers %s; it lacks %s", has, lacks);
    }

    void Stop()
    {
        if (!g_renderer) return;
        delete g_renderer;
        g_renderer = nullptr;
        g_owner    = nullptr;
        vk::SetLogSink(nullptr, nullptr);
    }

    /// Makes the device from the settings and checks a submission comes back; false with @p why.
    bool Start(void* owner, const wxl::core::settings::Settings& settings, char* why, uint32_t why_size)
    {
        Stop();
        g_tried = true;
        vk::SetLogSink(Sink, nullptr);

        vk::device::Options options;
        options.gpu        = settings.gx_gpu[0] ? settings.gx_gpu : nullptr;
        options.validation = settings.gx_validation;
        options.instance_extensions = vk::present::InstanceExtensions(options.instance_extension_count);
        options.presents            = vk::present::Presents;

        LARGE_INTEGER frequency{}, begin{}, end{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&begin);

        g_renderer = new Renderer();
        g_owner    = owner;
        if (!g_renderer->context.Create(options, why, why_size) || !g_renderer->allocator.Create(g_renderer->context, why, why_size) ||
            !g_renderer->submitter.Create(g_renderer->context, why, why_size))
        {
            Stop();
            return false;
        }

        // An empty submission, waited for with the bound every wait has: the queue answers.
        vk::frame::Submitter& submitter = g_renderer->submitter;
        uint64_t serial = 0;
        if (submitter.Begin()) serial = submitter.Submit();
        const vk::frame::Wait wait = serial ? submitter.WaitFor(serial) : vk::frame::Wait::Failed;
        if (wait != vk::frame::Wait::Done)
        {
            std::snprintf(why, why_size, "the device took no submission (%s) on %s", vk::frame::WaitName(wait),
                          g_renderer->context.Chosen().name);
            Stop();
            return false;
        }

        QueryPerformanceCounter(&end);
        Report(*g_renderer, 1000.0 * static_cast<double>(end.QuadPart - begin.QuadPart) / static_cast<double>(frequency.QuadPart));
        return true;
    }

    constexpr char kNoDrawing[] = "this runtime does not draw through Vulkan yet";

    int32_t WXL_CALL SelectApiRow(int32_t requested)
    {
        const wxl::core::gx::ApiChoice choice =
            wxl::core::gx::SelectApi(requested, wxl::core::session::CurrentSettings().gx_api);
        if (choice.why)
            WLOG_INFO(kTag, "the client asked for %s; %s, by %s", wxl::core::gx::ApiName(requested),
                      choice.api == WXL_GX_API_VULKAN ? "Vulkan is tried, D3D9 if this machine cannot" : "D3D9 is created",
                      choice.why);
        return choice.api;
    }

    wxl_result WXL_CALL DeviceCreateRow(void* device, void*, char* why, uint32_t why_size)
    {
        char reason[512] = {};
        if (!Start(device, wxl::core::session::CurrentSettings(), reason, sizeof reason))
        {
            WLOG_WARN(kTag, "Vulkan not started: %s; the client stays on D3D9", reason);
            if (why && why_size) std::snprintf(why, why_size, "%s", reason);
            return WXL_E_FAILED;
        }
        WLOG_INFO(kTag, "Vulkan: %s, so the device is released and the client goes on with D3D9", kNoDrawing);
        Stop();
        if (why && why_size) std::snprintf(why, why_size, "%s", kNoDrawing);
        return WXL_E_UNSUPPORTED;
    }

    void WXL_CALL DeviceDestroyRow(void* device)
    {
        if (g_renderer && g_owner == device) Stop();
    }

    const wxl_gx_vk_backend kBackend = {
        sizeof(wxl_gx_vk_backend),
        0,
        wxl::engine::CGxDevice::kLayoutHash,
        SelectApiRow,
        DeviceCreateRow,
        DeviceDestroyRow,
    };
}

namespace wxl::core::gx
{
    Outcome DeviceReady(const settings::Settings& settings, char* why, uint32_t why_size)
    {
        // The recompiled client asked through its adapter already; the stock one has none, so
        // the device is made here, beside its own.
        if (g_tried || settings.gx_api != settings::GxApi::vulkan) return Outcome::not_asked;
        char reason[512] = {};
        if (!Start(nullptr, settings, reason, sizeof reason))
        {
            WLOG_WARN(kTag, "Vulkan not started: %s; the client stays on D3D9", reason);
            if (why && why_size) std::snprintf(why, why_size, "%s", reason);
            return Outcome::failed;
        }
        WLOG_INFO(kTag, "Vulkan: %s, so the device is released and the client keeps its D3D9 device", kNoDrawing);
        Stop();
        return Outcome::released;
    }
}

const wxl_gx_vk_backend* WXL_CALL WarcraftXLGxVulkan(void) { return &kBackend; }
