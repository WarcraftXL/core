// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's particles, simulated on the CPU and written as soft sprites. Portable: no runtime, no
// device, no Windows; the native tests build it as it is.
//
//   Emitter  where particles come from and how they move (wxl_draw_emitter, repeated so that this
//            module needs no SDK header)
//   Look     how they look (wxl_draw_particles), its texture as the device holds it set apart
//   Pool     an emitter's particles as a structure of arrays: Spawn throws them, Step moves them
//            (gravity, drag, an attractor, one bounce or a rest on the ground the runtime reads),
//            Write turns each into two triangles facing the camera, lying flat or stretched along
//            its velocity, its size, colour and opacity from the look's keys at its age, its
//            sprite-sheet frame, and its height above the ground for the soft edge
//   ParticleShade  the pixel programs' arithmetic (ParticlePs, draw_particle.frag) on the CPU

#pragma once

#include "gx/shapes/Geometry.hpp"

#include <cstdint>
#include <vector>

namespace wxl::gx::shapes
{
    enum Spawn : uint32_t
    {
        kSpawnPoint = 0, kSpawnDisc, kSpawnBox, kSpawnSegment,
    };
    enum GroundMode : uint32_t
    {
        kGroundNone = 0, kGroundBounce, kGroundSettle,
    };
    enum Billboard : uint32_t
    {
        kBillboardCamera = 0, kBillboardGround, kBillboardStretched,
    };

    constexpr uint32_t kMaxKeys = 4;

    struct Emitter
    {
        uint32_t spawn = kSpawnPoint;
        V3       center;
        V3       extent;
        float    facing = 0.0f;
        V3       direction; ///< (0, 0, 0): up
        float    spread = 0.0f;
        float    speed_min = 1.0f, speed_max = 2.0f;
        float    life_min = 1.0f, life_max = 1.0f;
        float    size_min = 0.3f, size_max = 0.3f;
        float    spin_min = 0.0f, spin_max = 0.0f;
        float    gravity = 0.0f;
        float    drag = 0.0f;
        uint32_t ground = kGroundNone;
        float    restitution = 0.0f;
        float    friction = 0.0f;
        V3       attractor;
        float    attraction = 0.0f;
        float    attract_radius = 0.0f;
    };

    struct Key
    {
        float    t = 0.0f;
        float    size = 1.0f;
        uint32_t color = 0xFFFFFFu;
        float    alpha = 1.0f;
    };

    struct Look
    {
        uint32_t mode = kBillboardCamera;
        bool     textured = false;
        float    right = 1.0f, bottom = 1.0f;
        uint32_t frames_x = 1, frames_y = 1;
        float    frame_rate = 0.0f;
        float    stretch = 0.0f;
        float    softness = 0.0f;
        bool     mask = false;
        float    blur = 0.0f;
        uint32_t key_count = 0;
        Key      keys[kMaxKeys];
    };

    /// The size multiplier, colour and opacity of a look at @p t (0 to 1) of a life: its keys, in
    /// lines between them; with none, white, its own size, fading out.
    void Sample(const Look& look, float t, float& size, uint32_t& rgb, float& alpha);

    /// The camera's axes in the world: what faces it.
    struct Camera
    {
        V3 right{1, 0, 0}, up{0, 0, 1}, forward{0, 1, 0};
    };

    /// A small, fast generator (xorshift32): the same seed, the same particles.
    struct Random
    {
        uint32_t state = 0x9E3779B9u;
        explicit Random(uint32_t seed = 0x9E3779B9u) : state(seed ? seed : 0x9E3779B9u) {}
        uint32_t Next()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        }
        float Unit() { return static_cast<float>(Next() >> 8) * (1.0f / 16777216.0f); } ///< 0 to 1
        float Between(float a, float b) { return a + (b - a) * Unit(); }
    };

    class Pool
    {
    public:
        /// Particles alive.
        size_t Size() const { return x.size(); }
        void   Clear();

        /// Throws @p count particles of @p e from @p at (its center, moved and turned by what it follows).
        void Spawn(const Emitter& e, V3 at, float facing, uint32_t count, Random& random);

        /// Moves them @p dt seconds on @p ground (null: none; then the ground is @p floor); those
        /// whose life ran out, or that reached the attractor, go. @p heights: whether the ground's
        /// height is wanted for the soft edge too.
        void Step(const Emitter& e, float dt, Ground* ground, float floor, bool heights);

        /// Writes each particle's two triangles (six vertices) into @p out, from @p eye, for
        /// @p look; far to near when @p sort. @p softness_from_ground: the corners carry their
        /// height above the ground over the look's softness (else 1000, no fade).
        void Write(std::vector<Vertex>& out, const Look& look, const Camera& camera, V3 eye, bool sort) const;

        // The arrays, one entry per particle (public for the tests).
        std::vector<float>   x, y, z, vx, vy, vz, age, life, size, spin, angle, gz, gx, gy;
        std::vector<uint8_t> state; ///< 0 flying, 1 bounced once, 2 resting on the ground

    private:
        void Kill(size_t i);
        mutable std::vector<uint32_t> order_;
        mutable std::vector<float>    depth_;
    };

    /// The particle pixel programs' arithmetic: @p uv the picture's coordinate (or the sprite's 0 to 1
    /// without a texture), @p height the corner's height over the softness (interpolated),
    /// @p color the vertex colour (0 to 1), @p texel what the texture holds at @p uv. Writes the
    /// colour the blend is given.
    void ParticleShade(bool textured, bool modulate, bool mask, float u, float v, float height, const float color[4], const float texel[4],
                       float out[4]);

    /// A particle run's numbers, in a SurfaceConstants: row 2.w textured, row 5 MODULATE, a mask and
    /// the derivatives' scale (2^blur), as a surface's.
    SurfaceConstants ParticleConstants(bool textured, bool modulate, bool mask = false, float blur = 0.0f);
}
