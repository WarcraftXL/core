// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The facing rows of the client's camera (CSimpleCamera::m_facing: forward, the client's right,
// up), as both runtimes build them for a view a mod sets. The client draws the world looking
// along the first row with the third as its up (CSimpleCamera::SetGxProjectionAndView,
// 0x00607CB0: GxuXformCreateLookAtSgCompat(0, Forward(), Up())), so a roll is the second and
// third rows turned around the first. Plain arithmetic, shared by the x86 binding, the client's
// and the camera service.

#ifndef WXL_CORE_CAMERA_FACING_HPP
#define WXL_CORE_CAMERA_FACING_HPP

#include <cmath>

#include "wxl/abi.h"

namespace wxl::core::camera_facing
{
    inline float Dot(const wxl_vec3& a, const wxl_vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    /// The rows CSimpleCamera::SetFacing(forward) builds (FaceDirection, 0x00607DF0), from the
    /// direction normalised: the client's right a quarter turn to the left of it in the ground
    /// plane (+x when the direction is vertical), and up from the two. False for a zero direction.
    inline bool FaceDirection(wxl_vec3 direction, wxl_vec3 (&rows)[3])
    {
        const float length = std::sqrt(Dot(direction, direction));
        if (!(length > 1e-6f)) return false;
        const wxl_vec3 f{direction.x / length, direction.y / length, direction.z / length};
        wxl_vec3       r{1.0f, 0.0f, 0.0f};
        const float    flat = f.x * f.x + f.y * f.y;
        if (flat >= 0.00000023841858f)
        {
            const float scale = 1.0f / std::sqrt(flat);
            r                 = wxl_vec3{-f.y * scale, f.x * scale, 0.0f};
        }
        rows[0] = f;
        rows[1] = r;
        rows[2] = wxl_vec3{-(r.y * f.z), f.z * r.x, r.y * f.x - f.y * r.x};
        return true;
    }

    /// Turns the rows by `roll` radians around the first, positive to the camera's right: the up
    /// row leans towards the screen's right, which is the opposite of the client's "right" row
    /// (that row points to the screen's left: +y for a camera looking down +x). 0 leaves the rows
    /// as they are, bit for bit.
    inline void Roll(wxl_vec3 (&rows)[3], double roll)
    {
        if (roll == 0.0) return;
        const float    c = static_cast<float>(std::cos(roll));
        const float    s = static_cast<float>(std::sin(roll));
        const wxl_vec3 l = rows[1];
        const wxl_vec3 u = rows[2];
        rows[1]          = wxl_vec3{l.x * c + u.x * s, l.y * c + u.y * s, l.z * c + u.z * s};
        rows[2]          = wxl_vec3{u.x * c - l.x * s, u.y * c - l.y * s, u.z * c - l.z * s};
    }

    /// The rows of a view looking along `direction` and rolled by `roll`. False for a zero
    /// direction.
    inline bool Facing(wxl_vec3 direction, double roll, wxl_vec3 (&rows)[3])
    {
        if (!FaceDirection(direction, rows)) return false;
        Roll(rows, roll);
        return true;
    }

    /// The roll of a camera from its rows in world coordinates: the angle its up is turned around
    /// its forward from the up FaceDirection builds for that forward, -pi to pi, positive to the
    /// camera's right. 0 for a zero forward.
    inline double RollOf(const wxl_vec3& forward, const wxl_vec3& up)
    {
        wxl_vec3 level[3];
        if (!FaceDirection(forward, level)) return 0.0;
        // The screen's right is the opposite of the level "right" row.
        const double along = Dot(up, level[2]);
        const double right = -static_cast<double>(Dot(up, level[1]));
        if (along == 0.0 && right == 0.0) return 0.0;
        return std::atan2(right, along);
    }
} // namespace wxl::core::camera_facing

#endif // WXL_CORE_CAMERA_FACING_HPP
