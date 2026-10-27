// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The x86 attachment: the hook engine, the detours, and the services only a shipped binary has.

#include "runtime/Attachment.hpp"

#include "binding/BindingX86.hpp"
#include "common/Log.hpp"
#include "gx/d3d9/Highlight.hpp"
#include "gx/x86/Adapter.hpp"
#include "hook/Engine.hpp"
#include "objects/Objects.hpp"
#include "points/Points.hpp"
#include "runtime/Core.hpp"
#include "runtime/Image.hpp"
#include "seams/AttachX86.hpp"
#include "seams/Table.hpp"
#include "services/HookService.hpp"
#include "services/PatchService.hpp"
#include "services/DataService.hpp"
#include "profile/Profile.hpp"
#include "services/ActionService.hpp"
#include "services/CameraService.hpp"
#include "services/InputService.hpp"
#include "services/WorldService.hpp"
#include "services/PoseService.hpp"
#include "services/HighlightService.hpp"
#include "services/RenderService.hpp"
#include "services/SelectionCircleService.hpp"
#include "services/TextureService.hpp"
#include "services/CVarService.hpp"
#include "services/TerrainService.hpp"
#include "services/ImGuiService.hpp"
#include "services/FootprintService.hpp"
#include "services/EngineService.hpp"
#include "services/GxService.hpp"
#include "services/M2FileService.hpp"
#include "services/M2Service.hpp"
#include "services/ScriptService.hpp"
#include "services/StorageService.hpp"
#include "services/RelayService.hpp"

#include "wxl/services/hook.h"
#include "wxl/services/object.h"
#include "wxl/services/point.h"
#include "wxl/services/patch.h"
#include "wxl/services/data.h"
#include "wxl/services/engine.h"
#include "wxl/services/gx.h"
#include "wxl/services/m2.h"
#include "wxl/services/m2file.h"
#include "wxl/services/script.h"
#include "wxl/services/storage.h"

namespace wxl::core::attachment
{
    void Prepare()
    {
        binding::x86::Prepare();
        // wxl.highlight's renderer reads the stock client's device and its shader constants.
        if (image::IsClient()) gx::d3d9::highlight::SetSource(binding::x86::HighlightSource());
        if (!wxl::hook::Init())
            WLOG_ERROR(kTag, "hook engine unavailable; detours will be refused");
    }

    std::vector<wxl_event> Events()
    {
        // The seam bodies are the same code on both architectures, so the events they raise are
        // the list the seam table derives, less those of the seams this client cannot take.
        return seams::RaisedEvents(true);
    }

    std::vector<wxl::host::PublishedService> Services()
    {
        return {
            { WXL_HOOK_SERVICE, WXL_HOOK_SERVICE_VERSION, &services::HookApi() },
            { WXL_HOOK_RAW_SERVICE, WXL_HOOK_RAW_SERVICE_VERSION, &services::HookRawApi() },
            { WXL_PATCH_SERVICE, WXL_PATCH_SERVICE_VERSION, &services::PatchApi() },
            { WXL_OBJECT_SERVICE, WXL_OBJECT_SERVICE_VERSION, objects::Api() },
            { WXL_SCRIPT_SERVICE, WXL_SCRIPT_SERVICE_VERSION, &services::ScriptApi() },
            { WXL_STORAGE_SERVICE, WXL_STORAGE_SERVICE_VERSION, &services::StorageApi() },
            { WXL_DATA_SERVICE, WXL_DATA_SERVICE_VERSION, &services::DataApi() },
            { WXL_FOOTPRINT_SERVICE, WXL_FOOTPRINT_SERVICE_VERSION, &services::FootprintApi() },
            { WXL_ACTION_SERVICE, WXL_ACTION_SERVICE_VERSION, &services::ActionApi() },
            { WXL_POINT_SERVICE, WXL_POINT_SERVICE_VERSION, &points::Api() },
            { WXL_M2FILE_SERVICE, WXL_M2FILE_SERVICE_VERSION, &services::M2FileApi() },
            { WXL_M2_SERVICE, WXL_M2_SERVICE_VERSION, &services::M2Api() },
            { WXL_GX_SERVICE, WXL_GX_SERVICE_VERSION, &services::GxApi() },
            { WXL_ENGINE_SERVICE, WXL_ENGINE_SERVICE_VERSION, &services::EngineApi() },
            { WXL_RELAY_SERVICE, WXL_RELAY_SERVICE_VERSION, &services::RelayApi() },
            { WXL_CAMERA_SERVICE, WXL_CAMERA_SERVICE_VERSION, &services::CameraApi() },
            { WXL_INPUT_SERVICE, WXL_INPUT_SERVICE_VERSION, &services::InputApi() },
            { WXL_WORLD_SERVICE, WXL_WORLD_SERVICE_VERSION, &services::WorldApi() },
            { WXL_POSE_SERVICE, WXL_POSE_SERVICE_VERSION, &services::PoseApi() },
            { WXL_IMGUI_SERVICE, WXL_IMGUI_SERVICE_VERSION, &services::ImGuiApi() },
            { WXL_HIGHLIGHT_SERVICE, WXL_HIGHLIGHT_SERVICE_VERSION, &services::HighlightApi() },
            { WXL_RENDER_SERVICE, WXL_RENDER_SERVICE_VERSION, &services::RenderApi() },
            { WXL_VK_SERVICE, WXL_VK_SERVICE_VERSION, &services::VkApi() },
            { WXL_SELECTIONCIRCLE_SERVICE, WXL_SELECTIONCIRCLE_SERVICE_VERSION, &services::SelectionCircleApi() },
            { WXL_TEXTURE_SERVICE, WXL_TEXTURE_SERVICE_VERSION, &services::TextureApi() },
            { WXL_CVAR_SERVICE, WXL_CVAR_SERVICE_VERSION, &services::CVarApi() },
            { WXL_TERRAIN_SERVICE, WXL_TERRAIN_SERVICE_VERSION, &services::TerrainApi() },
#if defined(WXL_CORE_PROFILE) && WXL_CORE_PROFILE
            { WXL_PROFILE_SERVICE, WXL_PROFILE_SERVICE_VERSION, &profile::Api() },
#endif
        };
    }

    void Release(uint32_t mod) { services::ReleaseHooksOf(mod); }

    void ReleaseAll() { services::ReleaseEveryHook(); }

    void Install()
    {
        // The core's own detours go in before any mod can attach behind them, and only in the
        // image whose addresses they are.
        if (!image::IsClient())
            WLOG_WARN(kTag, "the executable is not Wow.exe build %u: named hook points and the "
                            "seams are unavailable; no world event will be raised and wxl.object "
                            "sees no object",
                      static_cast<unsigned>(image::kBuild));
        else
        {
            if (!seams::Install()) WLOG_ERROR(kTag, "a seam is not attached; its events will not be raised");
            // [gx] api = vulkan: the devices the client makes are the Vulkan renderer's to draw with.
            gx::x86::Install();
        }
    }
}
