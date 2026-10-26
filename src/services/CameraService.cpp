// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.camera over the binding. The views mods set are kept newest first; the first is written
// over the client's camera at the end of every update, after the seam raised CameraUpdate, so
// that a mod which sets its view from that event moves the camera with the frame. The client goes on updating
// its own camera underneath: only its field of view is touched across frames, and that is put
// back before each update and when the last view goes.

#include "services/CameraService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/core/camera_facing.hpp"
#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <atomic>
#include <cmath>
#include <vector>

namespace
{
    using namespace wxl::core;

    /// A view one mod holds.
    struct View
    {
        uint32_t mod;
        wxl_vec3 position;
        wxl_vec3 forward;
        double   fov;  ///< radians; 0 keeps the client's
        double   roll; ///< radians around forward, positive to the camera's right
    };

    /// Newest first. Main thread only, as every call that reaches it.
    std::vector<View> g_views;

    /// The camera whose field of view a view changed, and the client's own, to put back.
    void* g_restoreCamera = nullptr;
    float g_restoreFov    = 0.0f;

    uint32_t Caller()
    {
        const wxl::host::ModHost* host = Host();
        return host ? host->CurrentMod() : 0u;
    }

    /// Whether the call runs on the main thread; says once per call why not.
    bool OnGameThread(const char* call, std::atomic<bool>& said)
    {
        const wxl::host::ModHost* host = Host();
        if (!host || host->OnMainThread()) return true;
        if (!said.exchange(true))
            WLOG_WARN(kTag, "wxl.camera.%s was called off the main thread, where the camera is updated; it was "
                            "refused",
                      call);
        return false;
    }

    /// The world camera as the binding reads it, or the answer for none.
    wxl_result Read(const char* call, std::atomic<bool>& said, wxl_core_camera& out)
    {
        if (!OnGameThread(call, said)) return WXL_E_INVALID;
        const void* camera = binding::camera_world();
        if (!camera || binding::camera_read(camera, &out) != WXL_TRUE) return WXL_E_UNSUPPORTED;
        return WXL_OK;
    }

    /// Puts back the field of view the client had, when a view changed it.
    void RestoreFov()
    {
        // A camera of a world frame since destroyed (a logout) is gone: nothing to put back.
        if (!g_restoreCamera || g_restoreCamera != binding::camera_world())
        {
            g_restoreCamera = nullptr;
            return;
        }
        wxl_core_camera view{};
        if (binding::camera_read(g_restoreCamera, &view) == WXL_TRUE)
        {
            view.fov_base = g_restoreFov;
            binding::camera_write(g_restoreCamera, &view);
        }
        g_restoreCamera = nullptr;
    }

    // --- the table --------------------------------------------------------------------------

    #define WXL_CAMERA_READ(call)                                  \
        static std::atomic<bool> said{false};                      \
        if (!out) return WXL_E_INVALID;                            \
        wxl_core_camera camera{};                                  \
        const wxl_result read = Read(call, said, camera);          \
        if (read != WXL_OK) return read

    wxl_result WXL_CALL Position(wxl_vec3* out)
    {
        WXL_CAMERA_READ("Position");
        *out = camera.position;
        return WXL_OK;
    }

    wxl_result WXL_CALL Forward(wxl_vec3* out)
    {
        WXL_CAMERA_READ("Forward");
        *out = camera.forward;
        return WXL_OK;
    }

    wxl_result WXL_CALL Up(wxl_vec3* out)
    {
        WXL_CAMERA_READ("Up");
        *out = camera.up;
        return WXL_OK;
    }

    wxl_result WXL_CALL Fov(double* out)
    {
        WXL_CAMERA_READ("Fov");
        *out = camera.fov;
        return WXL_OK;
    }

    wxl_result WXL_CALL Pivot(wxl_vec3* out)
    {
        WXL_CAMERA_READ("Pivot");
        if (!camera.followed) return WXL_E_NOT_FOUND;
        *out = camera.pivot;
        return WXL_OK;
    }

    wxl_result WXL_CALL Roll(double* out)
    {
        WXL_CAMERA_READ("Roll");
        *out = camera_facing::RollOf(camera.forward, camera.up);
        return WXL_OK;
    }

    #undef WXL_CAMERA_READ

    wxl_bool WXL_CALL Cinematic()
    {
        static std::atomic<bool> said{false};
        wxl_core_camera          camera{};
        if (Read("Cinematic", said, camera) != WXL_OK) return WXL_FALSE;
        return camera.cinematic ? WXL_TRUE : WXL_FALSE;
    }

    bool Finite(const wxl_vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    /// SetView and SetViewRolled: the caller's view, newest first.
    wxl_result Place(const char* call, std::atomic<bool>& said, wxl_vec3 position, wxl_vec3 forward, double fov,
                     double roll)
    {
        if (!OnGameThread(call, said)) return WXL_E_INVALID;
        const float length = forward.x * forward.x + forward.y * forward.y + forward.z * forward.z;
        if (!Finite(position) || !Finite(forward) || !(length > 1e-12f)) return WXL_E_INVALID;
        if (!std::isfinite(fov) || fov < 0.0 || fov >= 3.14159265358979) return WXL_E_INVALID;
        if (!std::isfinite(roll)) return WXL_E_INVALID;
        // Taken to -pi..pi, where a float keeps its precision.
        if (roll != 0.0) roll = std::remainder(roll, 6.283185307179586);

        const uint32_t mod = Caller();
        for (auto it = g_views.begin(); it != g_views.end(); ++it)
            if (it->mod == mod)
            {
                g_views.erase(it);
                break;
            }
        g_views.insert(g_views.begin(), View{mod, position, forward, fov, roll});
        return WXL_OK;
    }

    wxl_result WXL_CALL SetView(wxl_vec3 position, wxl_vec3 forward, double fov)
    {
        static std::atomic<bool> said{false};
        return Place("SetView", said, position, forward, fov, 0.0);
    }

    wxl_result WXL_CALL SetViewRolled(wxl_vec3 position, wxl_vec3 forward, double fov, double roll)
    {
        static std::atomic<bool> said{false};
        return Place("SetViewRolled", said, position, forward, fov, roll);
    }

    /// Drops a mod's view; with none left, the client's field of view goes back now.
    bool Drop(uint32_t mod)
    {
        for (auto it = g_views.begin(); it != g_views.end(); ++it)
        {
            if (it->mod != mod) continue;
            g_views.erase(it);
            if (g_views.empty()) RestoreFov();
            return true;
        }
        return false;
    }

    wxl_result WXL_CALL ReleaseView()
    {
        static std::atomic<bool> said{false};
        if (!OnGameThread("ReleaseView", said)) return WXL_E_INVALID;
        return Drop(Caller()) ? WXL_OK : WXL_E_NOT_FOUND;
    }
}

namespace wxl::core::services
{
    const wxl_camera_api& CameraApi()
    {
        static const wxl_camera_api api = [] {
            wxl_camera_api table{};
            table.struct_size  = sizeof table;
            table.position     = &Position;
            table.forward      = &Forward;
            table.up           = &Up;
            table.fov          = &Fov;
            table.pivot        = &Pivot;
            table.cinematic    = &Cinematic;
            table.set_view     = &SetView;
            table.release_view    = &ReleaseView;
            table.set_view_rolled = &SetViewRolled;
            table.roll            = &Roll;
            return table;
        }();
        return api;
    }

    namespace camera
    {
        void BeforeUpdate(void* camera)
        {
            if (camera && camera == g_restoreCamera) RestoreFov();
        }

        void AfterUpdate(void* camera)
        {
            if (!camera || g_views.empty()) return;

            wxl_core_camera client{};
            if (binding::camera_read(camera, &client) != WXL_TRUE || client.cinematic) return;

            const View&     view = g_views.front();
            wxl_core_camera placed = client;
            placed.position = view.position;
            placed.forward  = view.forward;
            placed.roll     = static_cast<float>(view.roll);
            // The client draws with m_fov plus the offset its effects add (m_targetFov): m_fov is
            // set so that the two make the view's field of view.
            if (view.fov > 0.0)
                placed.fov_base = static_cast<float>(view.fov) - (client.fov - client.fov_base);
            binding::camera_write(camera, &placed);
            if (view.fov > 0.0)
            {
                g_restoreCamera = camera;
                g_restoreFov    = client.fov_base;
            }
        }

        void ForgetMod(uint32_t mod) { Drop(mod); }

        void Clear()
        {
            g_views.clear();
            RestoreFov();
        }
    }
}
