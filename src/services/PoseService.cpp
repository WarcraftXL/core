// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.pose over the binding. Each turned head is kept with its target and where it is; at every
// CGUnit_C::PreAnimate of its unit it moves towards the target and is written as the procedural
// transform of the head key bone, after the game wrote its own twist there. A released head goes
// back to 0 the same way, then the bone is handed back to the animation.

#include "services/PoseService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/mod_host.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <vector>

namespace
{
    using namespace wxl::core;

    constexpr uint32_t kHeadBone     = 6;    // the head key bone, which the game twists itself
    constexpr uint32_t kProcedural   = 0x80; // the bone flag that applies its procedural transform
    constexpr double   kMaxYaw       = 100.0 * 3.14159265358979 / 180.0;
    constexpr double   kMaxPitch     = 80.0 * 3.14159265358979 / 180.0;
    constexpr float    kSettled      = 0.0005f;

    struct Head
    {
        uint32_t   mod;
        wxl_handle unit;
        float      yaw, pitch;         ///< where it goes
        float      nowYaw = 0, nowPitch = 0;
        float      smoothing;          ///< seconds for half the way
        bool       releasing = false;
        LARGE_INTEGER last{};          ///< when it last moved
    };

    std::vector<Head> g_heads;

    uint32_t Caller()
    {
        const wxl::host::ModHost* host = Host();
        return host ? host->CurrentMod() : 0u;
    }

    bool OnGameThread(const char* call)
    {
        static std::atomic<bool> said{false};
        const wxl::host::ModHost* host = Host();
        if (!host || host->OnMainThread()) return true;
        if (!said.exchange(true))
            WLOG_WARN(kTag, "wxl.pose.%s was called off the main thread, where the models animate; it was refused", call);
        return false;
    }

    double Clamp(double v, double limit) { return v < -limit ? -limit : (v > limit ? limit : v); }

    /// The client's C44Matrix::Rotation(angle, axis, normalized): rows a, b, c, d.
    void Rotation(float angle, float x, float y, float z, float (&m)[16])
    {
        const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
        const float r[16] = {x * x * t + c,     y * x * t + s * z, z * x * t - y * s, 0.0f,
                             y * x * t - s * z, y * y * t + c,     t * (z * y) + x * s, 0.0f,
                             z * x * t + y * s, t * (z * y) - x * s, c + t * (z * z), 0.0f,
                             0.0f,              0.0f,              0.0f,              1.0f};
        for (int i = 0; i < 16; ++i) m[i] = r[i];
    }

    void Multiply(const float (&a)[16], const float (&b)[16], float (&out)[16])
    {
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += a[row * 4 + k] * b[k * 4 + col];
                out[row * 4 + col] = sum;
            }
    }

    /// The head's turn: a nod about the bone's own y axis (the game pitches a torso so, by minus
    /// the pitch), then a turn about z, as the game twists a turning unit's head.
    void HeadMatrix(float yaw, float pitch, float (&out)[16])
    {
        float nod[16], turn[16];
        Rotation(-pitch, 0.0f, 1.0f, 0.0f, nod);
        Rotation(yaw, 0.0f, 0.0f, 1.0f, turn);
        Multiply(nod, turn, out);
    }

    void* Resolve(wxl_handle unit, uint32_t& kind)
    {
        kind = WXL_OBJECT_UNKNOWN;
        return objects::Service().Handles().Resolve(unit, &kind);
    }

    bool IsUnit(uint32_t kind) { return kind == WXL_OBJECT_UNIT || kind == WXL_OBJECT_PLAYER; }

    wxl_result WXL_CALL HeadTurn(wxl_handle unit, double yaw, double pitch, double smoothing)
    {
        if (!OnGameThread("Head")) return WXL_E_INVALID;
        if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(smoothing) || smoothing < 0.0)
            return WXL_E_INVALID;
        uint32_t kind = 0;
        if (!Resolve(unit, kind)) return WXL_E_NOT_FOUND;
        if (!IsUnit(kind)) return WXL_E_INVALID;
        const float y = static_cast<float>(Clamp(yaw, kMaxYaw));
        const float p = static_cast<float>(Clamp(pitch, kMaxPitch));
        for (Head& head : g_heads)
            if (head.unit == unit)
            {
                head.mod       = Caller();
                head.yaw       = y;
                head.pitch     = p;
                head.smoothing = static_cast<float>(smoothing);
                head.releasing = false;
                return WXL_OK;
            }
        Head head{Caller(), unit, y, p};
        head.smoothing = static_cast<float>(smoothing);
        QueryPerformanceCounter(&head.last);
        g_heads.push_back(head);
        return WXL_OK;
    }

    wxl_result WXL_CALL ReleaseHead(wxl_handle unit)
    {
        if (!OnGameThread("ReleaseHead")) return WXL_E_INVALID;
        const uint32_t mod = Caller();
        for (Head& head : g_heads)
            if (head.unit == unit && head.mod == mod && !head.releasing)
            {
                head.releasing = true;
                head.yaw = head.pitch = 0.0f;
                return WXL_OK;
            }
        return WXL_E_NOT_FOUND;
    }

    float Approach(float now, float target, float dt, float halfLife)
    {
        if (!(halfLife > 0.0f)) return target;
        return target + (now - target) * std::exp2(-dt / halfLife);
    }
}

namespace wxl::core::services
{
    const wxl_pose_api& PoseApi()
    {
        static const wxl_pose_api api = [] {
            wxl_pose_api table{};
            table.struct_size  = sizeof table;
            table.head         = &HeadTurn;
            table.release_head = &ReleaseHead;
            return table;
        }();
        return api;
    }

    namespace pose
    {
        void PreAnimate(void* unit)
        {
            if (g_heads.empty() || !unit) return;
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            for (size_t i = g_heads.size(); i-- > 0;)
            {
                Head&    head = g_heads[i];
                uint32_t kind = 0;
                void*    self = Resolve(head.unit, kind);
                if (!self)
                {
                    g_heads.erase(g_heads.begin() + static_cast<std::ptrdiff_t>(i)); // the unit left view
                    continue;
                }
                if (self != unit) continue;

                float dt = static_cast<float>(now.QuadPart - head.last.QuadPart) / static_cast<float>(frequency.QuadPart);
                if (!(dt >= 0.0f) || dt > 0.25f) dt = 0.25f;
                head.last     = now;
                head.nowYaw   = Approach(head.nowYaw, head.yaw, dt, head.smoothing);
                head.nowPitch = Approach(head.nowPitch, head.pitch, dt, head.smoothing);

                void* model = binding::m2_object_model(unit);
                if (!model) return;
                if (head.releasing && std::fabs(head.nowYaw) < kSettled && std::fabs(head.nowPitch) < kSettled)
                {
                    binding::model_bone_flags(model, kHeadBone, 0, kProcedural);
                    g_heads.erase(g_heads.begin() + static_cast<std::ptrdiff_t>(i));
                    return;
                }
                float matrix[16];
                HeadMatrix(head.nowYaw, head.nowPitch, matrix);
                binding::model_bone_flags(model, kHeadBone, kProcedural, kProcedural);
                binding::model_bone_transform(model, kHeadBone, matrix);
                return;
            }
        }

        void ForgetMod(uint32_t mod)
        {
            for (Head& head : g_heads)
                if (head.mod == mod)
                {
                    head.releasing = true;
                    head.yaw = head.pitch = 0.0f;
                }
        }

        void Clear() { g_heads.clear(); }
    }
}
