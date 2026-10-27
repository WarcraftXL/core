// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The renderer inside the runtime: the table the recompiled client's CGxDeviceVk calls
// (wxl/core/gx_vk.h), the Vulkan device made from WarcraftXL.ini's [gx] section, and on the stock
// client, which has no Vulkan adapter, the same device made once its own D3D9 device is up.
//
// On the recompiled client the device renders: frames are cleared to the engine's colour, the
// engine's interface is drawn into them (its textures, pools and programs on the device), Dear
// ImGui over it, and they are shown through the present pass. The world is not drawn yet: its
// draws are counted and left out (gxfront/Engine.hpp says which). The stock client's device is
// made, checked and released: it keeps rendering through D3D9.
//
// The runtime's part around the device lives here: the log, the events a device's resources going
// raises (SwapchainLost; SwapchainRestored comes from the engine's own NotifyOnDeviceRestored), how
// many losses are too many, the frame's GPU zone in the profiler.

#include "gx/Vulkan.hpp"

#include "common/Log.hpp"
#include "gx/Events.hpp"
#include "gx/Select.hpp"
#include "gx/vk/Log.hpp"
#include "gx/vk/gxfront/Front.hpp"
#include "profile/Profile.hpp"
#include "runtime/Session.hpp"
#include "services/HighlightService.hpp"
#include "services/RenderService.hpp"

#include "wxl/core/gx_vk.h"
#include "wxl/engine/gx.hpp"

#include <windows.h>

#if defined(WXL_CORE_PROFILE) && WXL_CORE_PROFILE
#include <tracy/TracyC.h>
#endif

#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

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

    vk::gxfront::Front*  g_front   = nullptr;
    bool                 g_tried   = false; ///< a device was asked for in this launch
    bool                 g_world   = false; ///< inside CGWorldFrame::RenderWorld
    bool                 g_renders = false; ///< the client renders through it (the recompiled client)
    std::deque<uint64_t> g_losses;          ///< when the last losses came, in milliseconds

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
            {c.features.graphicsPipelineLibrary, "pipeline libraries"},
            {c.extensions.extended_dynamic_state3, "extended dynamic state 3"},
            {c.extensions.vertex_input_dynamic_state, "dynamic vertex input"},
            {c.extensions.unified_image_layouts, "unified image layouts"},
            {c.extensions.calibrated_timestamps, "calibrated timestamps"},
            {c.centre_samples != 0, "sample locations (multisampling off per draw)"},
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
    void Report(vk::gxfront::Front& front, double milliseconds)
    {
        const vk::device::Candidate& c = front.Context().Chosen();
        char api[32], driver[32], loader[32];
        vk::FormatVersion(c.api, api, sizeof api);
        vk::device::FormatDriver(c.vendor, c.driver, driver, sizeof driver);
        vk::FormatVersion(front.Context().InstanceApi(), loader, sizeof loader);
        const vk::resources::Budget budget = front.Allocator().CurrentBudget();
        WLOG_INFO(kTag, "Vulkan: device %u, %s (%s, Vulkan %s, driver %s, %04x:%04x, %llu MiB device memory, budget %llu MiB), "
                        "queue family %u, loader %s, validation %s, made in %.0f ms",
                  c.index, c.name, vk::device::TypeName(c.type), api, driver, c.vendor, c.device_id,
                  static_cast<unsigned long long>(c.device_local >> 20), static_cast<unsigned long long>(budget.budget >> 20),
                  c.family, loader, front.Context().Validation() ? "on" : "off", milliseconds);
        char has[512], lacks[512];
        Optional(c, true, has, sizeof has);
        Optional(c, false, lacks, sizeof lacks);
        WLOG_INFO(kTag, "Vulkan: beyond the 1.3 floor it offers %s; it lacks %s", has, lacks);
    }

    double Milliseconds(const LARGE_INTEGER& begin)
    {
        LARGE_INTEGER frequency{}, end{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&end);
        return 1000.0 * static_cast<double>(end.QuadPart - begin.QuadPart) / static_cast<double>(frequency.QuadPart);
    }

    /// <client>\WarcraftXL\cache\vk\, made when missing (outside the client's Cache\, which it
    /// wipes); empty when it cannot be.
    std::string CacheDirectory()
    {
        char path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (!n || n >= MAX_PATH) return {};
        std::string dir(path, n);
        const size_t slash = dir.find_last_of("\\/");
        if (slash == std::string::npos) return {};
        dir.resize(slash + 1);
        for (const char* part : {"WarcraftXL\\", "cache\\", "vk\\"})
        {
            dir += part;
            if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
        }
        return dir;
    }

    vk::gxfront::Options OptionsOf(const wxl::core::settings::Settings& settings)
    {
        vk::gxfront::Options options;
        options.gpu        = settings.gx_gpu[0] ? settings.gx_gpu : nullptr;
        options.validation = settings.gx_validation;
        options.frames     = settings.gx_frames;
        options.filter     = settings.gx_nearest ? vk::present::Filter::Nearest : vk::present::Filter::Linear;
        options.cache_dir  = CacheDirectory();
        options.async_pipelines = settings.gx_async;
        return options;
    }

    // --- The frame's GPU zone in the profiler ------------------------------------------------------
#if defined(WXL_CORE_PROFILE) && WXL_CORE_PROFILE
    /// Each frame as a GPU zone of a "Vulkan" context, from its first command to the end of the
    /// present pass, timed by the frame's own timestamps once it has completed (nothing waits).
    class GpuZones final : public vk::present::TimestampSink, public vk::render::ZoneSink
    {
    public:
        /// The context of the device of the front's generation, made once the profiler is up.
        void Attach(vk::gxfront::Front& front)
        {
            if (generation_ == front.Generation() || !wxl::core::profile::Started()) return;
            generation_                     = front.Generation();
            vk::present::Display& display = front.Screen();
            int64_t               ticks   = 0;
            if (!display.SetTimestamps(this) || !display.SampleGpuTime(ticks))
            {
                display.SetTimestamps(nullptr);
                return;
            }
            // One context per device made: a device made again starts its clock again.
            context_ = next_++;
            ___tracy_emit_gpu_new_context(___tracy_gpu_new_context_data{ticks, display.TimestampPeriod(), context_, 0, 2 /* Vulkan */});
            static const char kName[] = "Vulkan";
            ___tracy_emit_gpu_context_name(___tracy_gpu_context_name_data{context_, kName, static_cast<uint16_t>(sizeof kName - 1)});
            std::memset(open_, 0, sizeof open_);
            // The mods' zones (wxl.profile's gpu_zone_begin) on the same timeline.
            front.Mods().SetZoneSink(this);
        }

        /// A mod's zone, once its frame completed: begun, ended and timed at once.
        void Zone(const char* name, int64_t begin, int64_t end) override
        {
            if (!___tracy_connected()) return;
            static const char kSource[] = "wxl.render";
            const size_t      length    = std::strlen(name);
            const uint64_t    location  = ___tracy_alloc_srcloc_name(0, kSource, sizeof kSource - 1, kSource, sizeof kSource - 1, name, length, 0x9B59B6);
            const uint16_t    first     = NextQuery();
            const uint16_t    second    = NextQuery();
            ___tracy_emit_gpu_zone_begin_alloc(___tracy_gpu_zone_begin_data{location, first, context_});
            ___tracy_emit_gpu_zone_end(___tracy_gpu_zone_end_data{second, context_});
            ___tracy_emit_gpu_time(___tracy_gpu_time_data{begin, first, context_});
            ___tracy_emit_gpu_time(___tracy_gpu_time_data{end, second, context_});
        }

        void Begin(uint16_t query) override
        {
            open_[query & 15] = ___tracy_connected() != 0;
            if (open_[query & 15])
                ___tracy_emit_gpu_zone_begin(___tracy_gpu_zone_begin_data{reinterpret_cast<uint64_t>(&kFrame), query, context_});
        }

        void End(uint16_t query) override
        {
            // A zone ends only where it began: one that began unwatched is not recorded at all.
            open_[query & 15] = open_[(query - 1) & 15];
            if (open_[query & 15]) ___tracy_emit_gpu_zone_end(___tracy_gpu_zone_end_data{query, context_});
        }

        void Time(uint16_t query, int64_t ticks) override
        {
            if (!open_[query & 15]) return;
            ___tracy_emit_gpu_time(___tracy_gpu_time_data{ticks, query, context_});
            open_[query & 15] = false;
        }

    private:
        static inline const ___tracy_source_location_data kFrame = {"frame", "ScenePresent", __FILE__, __LINE__, 0x4A90D9};
        uint64_t generation_ = 0;
        uint8_t  context_    = 0;
        bool     open_[16]   = {};
        /// Context ids above those Tracy's own GPU contexts count up from 0; none is used in this process.
        static inline uint8_t next_ = 200;
        /// The mods' zones' query ids, above the frame's own (0 to 15).
        uint16_t query_ = 0x4000;
        uint16_t NextQuery()
        {
            if (query_ == 0xFFFF) query_ = 0x4000;
            return query_++;
        }
    };

    GpuZones g_zones;
#endif

    void Stop(bool released_resources)
    {
        if (!g_front) return;
        if (g_renders && released_resources)
        {
            // What the runtime and the mods made on the device goes before it, its frames done.
            g_front->WaitIdle();
            wxl::core::gx::ResourcesLost(true);
        }
        // What the mods and the highlights hold of the renderer goes with it.
        wxl::core::services::render::DeviceGone();
        wxl::core::services::highlight::DeviceGone();
        delete g_front;
        g_front   = nullptr;
        g_renders = false;
        vk::SetLogSink(nullptr, nullptr);
    }

    /// Makes the device from the settings, on @p window when there is one; false with @p why.
    bool Start(void* owner, void* window, const wxl::core::settings::Settings& settings, char* why, uint32_t why_size)
    {
        Stop(false);
        g_tried = true;
        vk::SetLogSink(Sink, nullptr);

        LARGE_INTEGER begin{};
        QueryPerformanceCounter(&begin);
        g_front = new vk::gxfront::Front();
        if (!g_front->Create(owner, window, OptionsOf(settings), why, why_size))
        {
            Stop(false);
            return false;
        }
        Report(*g_front, Milliseconds(begin));
        return true;
    }

    bool Mine(void* device) { return g_front && g_renders && device && g_front->Owner() == device; }

    // --- The table's rows ------------------------------------------------------------------------------

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

    wxl_result WXL_CALL DeviceCreateRow(void* device, void* window, char* why, uint32_t why_size)
    {
        char reason[512] = {};
        if (!device || !window || !Start(device, window, wxl::core::session::CurrentSettings(), reason, sizeof reason))
        {
            if (!reason[0]) std::snprintf(reason, sizeof reason, "the client gave no device or no window");
            WLOG_WARN(kTag, "Vulkan not started: %s; the client stays on D3D9", reason);
            if (why && why_size) std::snprintf(why, why_size, "%s", reason);
            return WXL_E_FAILED;
        }
        g_renders = true;
        g_losses.clear();
        const wxl::core::settings::Settings& s = wxl::core::session::CurrentSettings();
        WLOG_INFO(kTag, "Vulkan: the client renders through it (%u frames in flight, %s scaling): the interface and the world, "
                        "the client's shaders translated as it creates them; the draws it cannot make are left out and counted",
                  s.gx_frames, s.gx_nearest ? "nearest" : "bilinear");
        return WXL_OK;
    }

    void WXL_CALL DeviceDestroyRow(void* device)
    {
        if (!g_front || g_front->Owner() != device) return;
        char line[2048];
        g_front->Describe(line, sizeof line);
        WLOG_INFO(kTag, "%s; released", line);
        Stop(true);
    }

    wxl_result WXL_CALL DeviceSetWindowRow(void* device, void* window, char* why, uint32_t why_size)
    {
        if (!Mine(device)) return WXL_E_INVALID;
        char reason[256] = {};
        if (!window)
        {
            g_front->SetWindow(nullptr, reason, sizeof reason);
            // As the D3D9 device's DeviceSetFormat: what was made for the old window goes.
            wxl::core::gx::ResourcesLost(true);
            return WXL_OK;
        }
        if (!g_front->SetWindow(window, reason, sizeof reason))
        {
            WLOG_WARN(kTag, "Vulkan: the new window cannot be presented to: %s", reason);
            if (why && why_size) std::snprintf(why, why_size, "%s", reason);
            return WXL_E_FAILED;
        }
        return WXL_OK;
    }

    void WXL_CALL DeviceCapsRow(void* device)
    {
        if (Mine(device)) g_front->WriteCaps();
    }

    void WXL_CALL WindowSizeRow(void* device, uint32_t width, uint32_t height, int32_t minimized)
    {
        if (!Mine(device)) return;
        // A resize: the size-bound images go (as at a D3D9 reset), the device and Dear ImGui stay.
        if (!minimized) wxl::core::gx::ResourcesLost(false);
        g_front->WindowSize(width, height, minimized != 0);
    }

    int32_t WXL_CALL SceneBeginRow(void* device)
    {
        if (!Mine(device)) return WXL_GX_VK_FRAME_LOST;
        if (!g_front->BeginScene()) return WXL_GX_VK_FRAME_LOST;
        wxl::core::gx::UpdateBatchKeeping();
        return WXL_GX_VK_FRAME_OK;
    }

    void WXL_CALL SceneClearRow(void* device, uint32_t mask, uint32_t argb, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        if (Mine(device)) g_front->Clear(mask, argb, x, y, width, height);
    }

    void WXL_CALL DrawRow(void* device, const wxl_gx_batch* batch, int32_t count)
    {
        if (Mine(device)) g_front->Draw(batch, count, g_world);
    }

    int32_t WXL_CALL ScenePresentRow(void* device)
    {
        if (!Mine(device)) return WXL_GX_VK_FRAME_LOST;
        if (!g_front->Present())
        {
            WLOG_ERROR(kTag, "Vulkan: the device was lost; the client stops drawing until it is made again");
            return WXL_GX_VK_FRAME_LOST;
        }
#if defined(WXL_CORE_PROFILE) && WXL_CORE_PROFILE
        // Between frames: where the zone's context is made (once per device), its clock sampled.
        g_zones.Attach(*g_front);
#endif
        return WXL_GX_VK_FRAME_OK;
    }

    wxl_result WXL_CALL DeviceRestoreRow(void* device, char* why, uint32_t why_size)
    {
        if (!Mine(device)) return WXL_E_INVALID;
        const uint64_t now = GetTickCount64();
        while (!g_losses.empty() && now - g_losses.front() > 60'000) g_losses.pop_front();
        g_losses.push_back(now);
        char reason[512] = {};
        if (g_losses.size() >= 3)
            std::snprintf(reason, sizeof reason, "the Vulkan device was lost three times within a minute");
        else
        {
            // The runtime's objects and the mods' go with the device, once what it was given is done
            // (a loss the renderer declared leaves a working GPU behind); SwapchainRestored follows
            // from the engine's NotifyOnDeviceRestored once the client has its device back.
            g_front->WaitIdle();
            wxl::core::gx::ResourcesLost(true);
            LARGE_INTEGER begin{};
            QueryPerformanceCounter(&begin);
            if (g_front->Restore(reason, sizeof reason))
            {
                WLOG_WARN(kTag, "Vulkan: the device was lost and is made again (%zu loss(es) in the last minute), in %.0f ms",
                          g_losses.size(), Milliseconds(begin));
                return WXL_OK;
            }
        }
        WLOG_ERROR(kTag, "Vulkan: %s; the client closes. Set [gx] api = d3d9 in WarcraftXL.ini to play on D3D9", reason);
        if (why && why_size) std::snprintf(why, why_size, "%s", reason);
        return WXL_E_FAILED;
    }

    void WXL_CALL GammaChangedRow(void* device)
    {
        if (Mine(device)) g_front->GammaChanged();
    }

    void WXL_CALL CursorChangedRow(void* device)
    {
        if (Mine(device)) g_front->CursorChanged();
    }

    void WXL_CALL CursorSetRow(void* device)
    {
        if (Mine(device)) g_front->SetCursor();
    }

    char* WXL_CALL PoolMemoryRow(void* device, void* pool) { return Mine(device) && pool ? g_front->PoolMemory(pool) : nullptr; }

    void WXL_CALL PoolResizeRow(void* device, void* pool)
    {
        if (Mine(device) && pool) g_front->PoolResize(pool);
    }

    void WXL_CALL PoolDestroyRow(void* device, void* pool)
    {
        if (Mine(device) && pool) g_front->PoolDestroy(pool);
    }

    int32_t WXL_CALL TexCreateRow(void* device, void* tex, uint32_t width, uint32_t height, uint32_t levels)
    {
        return Mine(device) && tex && g_front->TexCreate(tex, width, height, levels) ? 1 : 0;
    }

    void WXL_CALL TexDestroyRow(void* device, void* tex)
    {
        if (Mine(device) && tex) g_front->TexDestroy(tex);
    }

    void WXL_CALL SimulateLossRow(void* device)
    {
        if (!Mine(device)) return;
        WLOG_WARN(kTag, "Vulkan: gxVkSimulateLoss: the device is marked lost, as a real loss would");
        g_front->SimulateLoss();
    }

    void WXL_CALL DescribeRow(void* device, char* out, uint32_t out_size)
    {
        if (!out || !out_size) return;
        if (g_front && g_front->Owner() == device) g_front->Describe(out, out_size);
        else std::snprintf(out, out_size, "Vulkan: no device");
    }

    void WXL_CALL ShaderConstantsRow(void* device, int32_t target, uint32_t first, uint32_t rows, const float* values)
    {
        if (Mine(device)) g_front->ShaderConstants(target, first, rows, values);
    }

    int32_t WXL_CALL ShaderCreateRow(void* device, void* shader, int32_t vertex, const char* name, const void* code, uint32_t size)
    {
        return Mine(device) && g_front->ShaderCreate(shader, vertex != 0, name, code, size) ? 1 : 0;
    }

    void WXL_CALL ShaderDestroyRow(void* device, void* shader)
    {
        if (Mine(device)) g_front->ShaderDestroy(shader);
    }

    char* WXL_CALL TexLockRow(void* device, void* tex, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t width,
                              uint32_t height, uint32_t* pitch)
    {
        uint32_t stride = 0;
        char*    bytes  = Mine(device) && tex ? g_front->TexLock(tex, face, level, x, y, width, height, stride) : nullptr;
        if (pitch) *pitch = stride;
        return bytes;
    }

    void WXL_CALL TexUnlockRow(void* device, void* tex)
    {
        if (Mine(device) && tex) g_front->TexUnlock(tex);
    }

    void WXL_CALL PoolDiscardRow(void* device, void* pool)
    {
        if (Mine(device) && pool) g_front->PoolDiscard(pool);
    }

    void WXL_CALL PoolWrittenRow(void* device, void* pool, uint32_t offset, uint32_t size)
    {
        if (Mine(device) && pool) g_front->PoolWritten(pool, offset, size);
    }

    void WXL_CALL ShaderPrewarmRow(void* device, const void* const* codes, const uint32_t* sizes, uint32_t count)
    {
        if (Mine(device)) g_front->ShaderPrewarm(codes, sizes, count);
    }

    void WXL_CALL TargetSetRow(void* device)
    {
        if (Mine(device)) g_front->TargetChanged();
    }

    int32_t WXL_CALL TexCopyRow(void* device, void* tex, int32_t dest_x, int32_t dest_y, int32_t source_x, int32_t source_y, int32_t width,
                                int32_t height, uint32_t level, uint32_t plane)
    {
        return Mine(device) && tex && g_front->TexCopy(tex, dest_x, dest_y, source_x, source_y, width, height, level, plane) ? 1 : 0;
    }

    int32_t WXL_CALL TexStretchRow(void* device, void* tex, const int32_t* dest, const int32_t* source, uint32_t level, uint32_t face)
    {
        return Mine(device) && tex && g_front->TexStretch(tex, dest, source, level, face) ? 1 : 0;
    }

    uint32_t WXL_CALL QueryCreateRow(void* device) { return Mine(device) ? g_front->QueryCreate() : 0; }

    void WXL_CALL QueryDestroyRow(void* device, uint32_t query)
    {
        if (Mine(device)) g_front->QueryDestroy(query);
    }

    int32_t WXL_CALL QueryBeginRow(void* device, uint32_t query) { return Mine(device) && g_front->QueryBegin(query) ? 1 : 0; }

    int32_t WXL_CALL QueryEndRow(void* device, uint32_t query) { return Mine(device) && g_front->QueryEnd(query) ? 1 : 0; }

    int32_t WXL_CALL QueryResultRow(void* device, uint32_t query, uint32_t* samples)
    {
        uint32_t count = 0;
        const bool known = Mine(device) && g_front->QueryResult(query, count);
        if (samples) *samples = count;
        return known ? 1 : 0;
    }

    int32_t WXL_CALL ReadPixelsRow(void* device, int32_t x, int32_t y, uint32_t width, uint32_t height, void* bgra)
    {
        return Mine(device) && g_front->ReadPixels(x, y, width, height, static_cast<uint32_t*>(bgra)) ? 1 : 0;
    }

    uint32_t WXL_CALL SampleCountsRow(void* device) { return Mine(device) ? g_front->SampleCounts() : 1u; }

    void WXL_CALL BonePaletteRow(void* device, const float* bones, uint32_t count)
    {
        if (Mine(device)) g_front->Renderer().BonePalette(bones, count);
    }

    const wxl_gx_vk_backend kBackend = {
        sizeof(wxl_gx_vk_backend),
        0,
        wxl::engine::CGxDevice::kLayoutHash,
        SelectApiRow,
        DeviceCreateRow,
        DeviceDestroyRow,
        DeviceSetWindowRow,
        DeviceCapsRow,
        WindowSizeRow,
        SceneBeginRow,
        SceneClearRow,
        DrawRow,
        ScenePresentRow,
        DeviceRestoreRow,
        GammaChangedRow,
        CursorChangedRow,
        CursorSetRow,
        PoolMemoryRow,
        PoolResizeRow,
        PoolDestroyRow,
        TexCreateRow,
        TexDestroyRow,
        SimulateLossRow,
        DescribeRow,
        ShaderConstantsRow,
        ShaderCreateRow,
        ShaderDestroyRow,
        TexLockRow,
        TexUnlockRow,
        PoolDiscardRow,
        PoolWrittenRow,
        ShaderPrewarmRow,
        TargetSetRow,
        TexCopyRow,
        TexStretchRow,
        QueryCreateRow,
        QueryDestroyRow,
        QueryBeginRow,
        QueryEndRow,
        QueryResultRow,
        ReadPixelsRow,
        SampleCountsRow,
        BonePaletteRow,
    };
}

namespace wxl::core::gx
{
    Outcome DeviceReady(const settings::Settings& settings, char* why, uint32_t why_size)
    {
        // The recompiled client asked through its adapter already; the stock one has none, so
        // the device is made here, beside its own, checked with a frame, and released.
        if (g_tried || settings.gx_api != settings::GxApi::vulkan) return Outcome::not_asked;
        char reason[512] = {};
        if (!Start(nullptr, nullptr, settings, reason, sizeof reason))
        {
            WLOG_WARN(kTag, "Vulkan not started: %s; the client stays on D3D9", reason);
            if (why && why_size) std::snprintf(why, why_size, "%s", reason);
            return Outcome::failed;
        }
        WLOG_INFO(kTag, "Vulkan: this runtime does not draw the stock client through Vulkan yet, so the device is released "
                        "and the client keeps its D3D9 device");
        Stop(false);
        return Outcome::released;
    }

    bool VulkanActive() { return g_front && g_renders; }

    wxl::vk::gxfront::Front* VulkanFront() { return g_front && g_renders && g_front->Ready() ? g_front : nullptr; }

    void UpdateBatchKeeping()
    {
        if (!g_front || !g_renders) return;
        g_front->Renderer().KeepBatches(services::render::WantsBatches() || services::highlight::Capturing());
    }

    void WorldPass(bool inside) { g_world = inside; }

    bool InWorldPass() { return g_world; }

    bool VulkanOpenFrame(VulkanFrame& out)
    {
        if (!g_front || !g_renders || !g_front->Ready() || g_front->Context().Lost() || !g_front->Screen().InFrame()) return false;
        out.context    = &g_front->Context();
        out.allocator  = &g_front->Allocator();
        out.display    = &g_front->Screen();
        out.generation = g_front->Generation();
        out.frames     = g_front->Screen().FrameCount();
        return true;
    }
}

const wxl_gx_vk_backend* WXL_CALL WarcraftXLGxVulkan(void) { return &kBackend; }
