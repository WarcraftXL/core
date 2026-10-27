// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's particles (see Particles.hpp).

#include "gx/shapes/Particles.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace wxl::gx::shapes
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        V3    Add(V3 a, V3 b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
        V3    Scale(V3 a, float s) { return V3{a.x * s, a.y * s, a.z * s}; }
        float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3    Cross(V3 a, V3 b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        V3    Normal(V3 a, V3 otherwise)
        {
            const float l = std::sqrt(Dot(a, a));
            return l > 1e-6f ? Scale(a, 1.0f / l) : otherwise;
        }
        V3 Turned(V3 a, float c, float s) { return V3{a.x * c - a.y * s, a.x * s + a.y * c, a.z}; }

        /// How far below a moved particle its ground height is asked again, in yards.
        constexpr float kGroundStep = 0.5f;
        /// A height ratio that never fades.
        constexpr float kNoFade = 1000.0f;
    }

    void Sample(const Look& look, float t, float& size, uint32_t& rgb, float& alpha)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        if (!look.key_count)
        {
            size  = 1.0f;
            rgb   = 0xFFFFFFu;
            alpha = 1.0f - t;
            return;
        }
        const Key* keys = look.keys;
        const uint32_t n = std::min(look.key_count, kMaxKeys);
        if (t <= keys[0].t || n == 1)
        {
            size  = keys[0].size;
            rgb   = keys[0].color;
            alpha = keys[0].alpha;
            return;
        }
        uint32_t i = 1;
        while (i < n - 1 && t > keys[i].t) ++i;
        const Key& a = keys[i - 1];
        const Key& b = keys[i];
        if (t >= b.t)
        {
            size  = b.size;
            rgb   = b.color;
            alpha = b.alpha;
            return;
        }
        const float f = (t - a.t) / std::max(b.t - a.t, 1e-6f);
        size          = a.size + (b.size - a.size) * f;
        alpha         = a.alpha + (b.alpha - a.alpha) * f;
        const auto channel = [&](int shift) {
            const float ca = static_cast<float>((a.color >> shift) & 0xFF), cb = static_cast<float>((b.color >> shift) & 0xFF);
            return static_cast<uint32_t>(std::lround(ca + (cb - ca) * f)) << shift;
        };
        rgb = channel(16) | channel(8) | channel(0);
    }

    void Pool::Clear()
    {
        for (auto* v : {&x, &y, &z, &vx, &vy, &vz, &age, &life, &size, &spin, &angle, &gz, &gx, &gy}) v->clear();
        state.clear();
    }

    void Pool::Spawn(const Emitter& e, V3 at, float facing, uint32_t count, Random& random)
    {
        const float c = std::cos(facing), s = std::sin(facing);
        // The cone: around its axis, turned with the facing; up when none is given.
        const V3 axis = Normal(Turned(e.direction, c, s), V3{0, 0, 1});
        const V3 side = Normal(std::fabs(axis.z) < 0.9f ? Cross(axis, V3{0, 0, 1}) : Cross(axis, V3{1, 0, 0}), V3{1, 0, 0});
        const V3 other = Cross(axis, side);
        const float cosSpread = std::cos(std::clamp(e.spread, 0.0f, kPi));
        for (uint32_t n = 0; n < count; ++n)
        {
            V3 offset;
            switch (e.spawn)
            {
            case kSpawnDisc:
            {
                const float r = e.extent.x * std::sqrt(random.Unit()), a = 2.0f * kPi * random.Unit();
                offset        = V3{r * std::cos(a), r * std::sin(a), 0.0f};
                break;
            }
            case kSpawnBox:
                offset = V3{e.extent.x * (random.Unit() - 0.5f), e.extent.y * (random.Unit() - 0.5f), e.extent.z * (random.Unit() - 0.5f)};
                break;
            case kSpawnSegment: offset = Scale(e.extent, random.Unit()); break;
            default: break;
            }
            if (e.spawn == kSpawnDisc || e.spawn == kSpawnBox) offset = Turned(offset, c, s);
            const V3 p = Add(at, offset);
            // Uniform over the cap of the cone.
            const float cz  = 1.0f - random.Unit() * (1.0f - cosSpread);
            const float sz  = std::sqrt(std::max(0.0f, 1.0f - cz * cz));
            const float phi = 2.0f * kPi * random.Unit();
            const V3    dir = Add(Add(Scale(axis, cz), Scale(side, sz * std::cos(phi))), Scale(other, sz * std::sin(phi)));
            const float speed = random.Between(e.speed_min, e.speed_max);
            x.push_back(p.x);
            y.push_back(p.y);
            z.push_back(p.z);
            vx.push_back(dir.x * speed);
            vy.push_back(dir.y * speed);
            vz.push_back(dir.z * speed);
            age.push_back(0.0f);
            life.push_back(std::max(random.Between(e.life_min, e.life_max), 1e-3f));
            size.push_back(random.Between(e.size_min, e.size_max));
            spin.push_back(random.Between(e.spin_min, e.spin_max));
            angle.push_back(2.0f * kPi * random.Unit());
            gz.push_back(-1e30f);
            gx.push_back(1e30f);
            gy.push_back(1e30f);
            state.push_back(0);
        }
    }

    void Pool::Kill(size_t i)
    {
        const size_t last = x.size() - 1;
        for (auto* v : {&x, &y, &z, &vx, &vy, &vz, &age, &life, &size, &spin, &angle, &gz, &gx, &gy})
        {
            (*v)[i] = (*v)[last];
            v->pop_back();
        }
        state[i] = state[last];
        state.pop_back();
    }

    void Pool::Step(const Emitter& e, float dt, Ground* ground, float floor, bool heights)
    {
        if (dt <= 0.0f) return;
        const bool  collide = e.ground != kGroundNone;
        const bool  pull    = e.attraction != 0.0f;
        const float keep    = std::max(0.0f, 1.0f - e.drag * dt);
        const float slide   = std::max(0.0f, 1.0f - e.friction * dt);
        const float radius2 = e.attract_radius * e.attract_radius;
        for (size_t i = 0; i < x.size();)
        {
            age[i] += dt;
            if (age[i] >= life[i])
            {
                Kill(i);
                continue;
            }
            angle[i] += spin[i] * dt;
            if (state[i] < 2)
            {
                vz[i] -= e.gravity * dt;
                if (pull)
                {
                    const float dx = e.attractor.x - x[i], dy = e.attractor.y - y[i], dz = e.attractor.z - z[i];
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 <= radius2)
                    {
                        Kill(i);
                        continue;
                    }
                    const float k = e.attraction * dt / std::sqrt(std::max(d2, 1e-8f));
                    vx[i] += dx * k;
                    vy[i] += dy * k;
                    vz[i] += dz * k;
                }
                vx[i] *= keep;
                vy[i] *= keep;
                vz[i] *= keep;
                x[i] += vx[i] * dt;
                y[i] += vy[i] * dt;
                z[i] += vz[i] * dt;
            }
            else
            {
                // Resting: it slides to a stop on the ground.
                vx[i] *= slide;
                vy[i] *= slide;
                x[i] += vx[i] * dt;
                y[i] += vy[i] * dt;
            }
            if (collide || heights)
            {
                // The ground's height under it, asked again once it moved half a yard.
                if (std::fabs(x[i] - gx[i]) > kGroundStep || std::fabs(y[i] - gy[i]) > kGroundStep)
                {
                    float h = floor;
                    if (!(ground && HeightAt(*ground, x[i], y[i], h))) h = floor;
                    gz[i] = h;
                    gx[i] = x[i];
                    gy[i] = y[i];
                }
            }
            if (collide && z[i] <= gz[i])
            {
                z[i] = gz[i];
                if (e.ground == kGroundBounce && state[i] == 0 && vz[i] < 0.0f)
                {
                    vz[i] = -vz[i] * std::clamp(e.restitution, 0.0f, 1.0f);
                    const float along = std::max(0.0f, 1.0f - std::clamp(e.friction, 0.0f, 1.0f));
                    vx[i] *= along;
                    vy[i] *= along;
                    state[i] = vz[i] > 0.25f ? 1 : 2;
                }
                else
                {
                    vz[i]    = 0.0f;
                    state[i] = 2;
                }
            }
            ++i;
        }
    }

    void Pool::Write(std::vector<Vertex>& out, const Look& look, const Camera& camera, V3 eye, bool sort) const
    {
        const size_t n = x.size();
        if (!n) return;
        order_.resize(n);
        std::iota(order_.begin(), order_.end(), 0u);
        if (sort)
        {
            // Far to near, for blending over each other.
            depth_.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                const float dx = x[i] - eye.x, dy = y[i] - eye.y, dz = z[i] - eye.z;
                depth_[i]      = dx * dx + dy * dy + dz * dz;
            }
            std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) { return depth_[a] > depth_[b]; });
        }
        const uint32_t fx = std::max(look.frames_x, 1u), fy = std::max(look.frames_y, 1u), frames = fx * fy;
        const float    du = look.right / static_cast<float>(fx), dv = look.bottom / static_cast<float>(fy);
        const bool     flat = look.mode == kBillboardGround;
        const float    soft = look.softness > 0.0f && !flat ? 1.0f / look.softness : 0.0f;
        const size_t   base = out.size();
        out.resize(base + n * 6);
        Vertex* v = out.data() + base;
        for (uint32_t i : order_)
        {
            const float t = age[i] / life[i];
            float       mult, alpha;
            uint32_t    rgb;
            Sample(look, t, mult, rgb, alpha);
            const float half = 0.5f * size[i] * mult;
            const V3    p{x[i], y[i], flat ? gz[i] > -1e29f ? gz[i] : z[i] : z[i]};
            V3          r, u;
            if (flat)
            {
                const float c = std::cos(angle[i]), s = std::sin(angle[i]);
                r = V3{c * half, s * half, 0.0f};
                u = V3{-s * half, c * half, 0.0f};
            }
            else if (look.mode == kBillboardStretched && vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i] > 1e-6f)
            {
                // Along its velocity as the camera sees it, as long as `stretch` seconds of it.
                const V3    vel{vx[i], vy[i], vz[i]};
                const V3    seen = Add(vel, Scale(camera.forward, -Dot(vel, camera.forward)));
                const float len  = std::sqrt(Dot(seen, seen));
                const V3    axis = Normal(seen, camera.up);
                r                = Scale(AxisSide(axis, camera.forward, camera.right), half); // a beam's billboard
                u                = Scale(axis, half + 0.5f * len * look.stretch);
            }
            else
            {
                const float c = std::cos(angle[i]), s = std::sin(angle[i]);
                r = Add(Scale(camera.right, c * half), Scale(camera.up, s * half));
                u = Add(Scale(camera.up, c * half), Scale(camera.right, -s * half));
            }
            // Its frame of the sheet: played at frame_rate, or once over its life.
            uint32_t frame = 0;
            if (frames > 1)
                frame = look.frame_rate > 0.0f ? static_cast<uint32_t>(age[i] * look.frame_rate) % frames
                                               : std::min(static_cast<uint32_t>(t * static_cast<float>(frames)), frames - 1);
            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            if (look.textured)
            {
                u0 = static_cast<float>(frame % fx) * du;
                v0 = static_cast<float>(frame / fx) * dv;
                u1 = u0 + du;
                v1 = v0 + dv;
            }
            const uint32_t color = (static_cast<uint32_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)) << 24) | (rgb & 0xFFFFFFu);
            const V3       corner[4] = {Add(Add(p, Scale(r, -1.0f)), u), Add(Add(p, r), u), Add(Add(p, r), Scale(u, -1.0f)),
                                        Add(Add(p, Scale(r, -1.0f)), Scale(u, -1.0f))};
            const float    cu[4] = {u0, u1, u1, u0}, cv[4] = {v0, v0, v1, v1};
            const float    ground = gz[i] > -1e29f ? gz[i] : -1e30f;
            const auto     set = [&](Vertex& o, int k) {
                o.pos[0]   = corner[k].x - eye.x;
                o.pos[1]   = corner[k].y - eye.y;
                o.pos[2]   = corner[k].z - eye.z;
                o.other[0] = cu[k];
                o.other[1] = cv[k];
                o.other[2] = soft > 0.0f && ground > -1e29f ? (corner[k].z - ground) * soft : kNoFade;
                o.color    = color;
                o.side     = 1;
                o.end      = 0;
                o.width    = 0;
                o.flags    = flat ? 1 : 0;
            };
            set(v[0], 0);
            set(v[1], 1);
            set(v[2], 2);
            v[3] = v[0];
            v[4] = v[2];
            set(v[5], 3);
            v += 6;
        }
    }

    void ParticleShade(bool textured, bool modulate, bool mask, float u, float v, float height, const float color[4], const float texel[4],
                       float out[4])
    {
        // The pixel programs' arithmetic (draw.hlsl ParticlePs, draw_particle.frag), line for line.
        float t[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (textured)
        {
            for (int i = 0; i < 4; ++i) t[i] = texel[i];
            if (mask) t[0] = t[1] = t[2] = 1.0f;
        }
        else
        {
            // A soft round blot: (1 - r^2)^2 of the sprite's middle.
            const float dx = u * 2.0f - 1.0f, dy = v * 2.0f - 1.0f;
            const float a  = std::clamp(1.0f - (dx * dx + dy * dy), 0.0f, 1.0f);
            t[3]           = a * a;
        }
        const float fade = std::clamp(height, 0.0f, 1.0f);
        const float a    = color[3] * t[3] * fade;
        for (int i = 0; i < 3; ++i)
        {
            const float c = color[i] * t[i];
            out[i]        = modulate ? 1.0f + (c - 1.0f) * a : c;
        }
        out[3] = a;
    }

    SurfaceConstants ParticleConstants(bool textured, bool modulate, bool mask, float blur)
    {
        SurfaceConstants k;
        k.k[2][3] = textured ? 1.0f : 0.0f;
        k.k[5][1] = modulate ? 1.0f : 0.0f;
        k.k[5][2] = mask ? 1.0f : 0.0f;
        k.k[5][3] = std::exp2(std::clamp(blur, 0.0f, 8.0f));
        return k;
    }
}
