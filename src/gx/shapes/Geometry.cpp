// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's geometry (see Geometry.hpp).

#include "gx/shapes/Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wxl::gx::shapes
{
    namespace
    {
        // The terrain's grid (wxl/terrain/grid.hpp, repeated: this module reads no SDK header).
        constexpr double  kOrigin        = 17066.666;
        constexpr double  kCellSize      = 4.1666665;
        constexpr int64_t kCellsPerSide  = 1024 * 8;
        constexpr int32_t kCellsPerChunk = 8;
        constexpr double  kPi            = 3.14159265358979323846;

        uint32_t OuterIndex(int32_t row, int32_t col) { return static_cast<uint32_t>(17 * row + col); }
        uint32_t InnerIndex(int32_t row, int32_t col) { return static_cast<uint32_t>(17 * row + 9 + col); }

        /// A point of grid space with a height.
        struct G
        {
            double u, v;
            double z;
        };

        /// A cell's five heights (its corners (u, v), (u, v + 1), (u + 1, v + 1), (u + 1, v), then its
        /// middle), read once per chunk while a shape is built.
        class Cells
        {
        public:
            explicit Cells(Ground* ground) : ground_(ground) {}

            bool Heights(int64_t r, int64_t c, double out[5])
            {
                if (!ground_ || r < 0 || c < 0 || r >= kCellsPerSide || c >= kCellsPerSide) return false;
                const int32_t cy = static_cast<int32_t>(r / kCellsPerChunk), cx = static_cast<int32_t>(c / kCellsPerChunk);
                if (!found_ || cx != cx_ || cy != cy_)
                {
                    found_   = true;
                    cx_      = cx;
                    cy_      = cy;
                    heights_ = nullptr;
                    holes_   = 0;
                    loaded_  = ground_->Chunk(cx, cy, heights_, holes_) && heights_;
                }
                if (!loaded_) return false;
                const int32_t row = static_cast<int32_t>(r % kCellsPerChunk), col = static_cast<int32_t>(c % kCellsPerChunk);
                if ((holes_ >> (4 * (row >> 1) + (col >> 1))) & 1u) return false;
                out[0] = heights_[OuterIndex(row, col)];
                out[1] = heights_[OuterIndex(row, col + 1)];
                out[2] = heights_[OuterIndex(row + 1, col + 1)];
                out[3] = heights_[OuterIndex(row + 1, col)];
                out[4] = heights_[InnerIndex(row, col)];
                return true;
            }

        private:
            Ground*      ground_;
            bool         found_ = false, loaded_ = false;
            int32_t      cx_ = 0, cy_ = 0;
            const float* heights_ = nullptr;
            uint16_t     holes_ = 0;
        };

        /// Triangle @p tri (0-3) of cell (r, c): its middle, then two corners going round the cell,
        /// as the client cuts it (grid.hpp, CellTriangle).
        void CellTriangle(int64_t r, int64_t c, int tri, const double h[5], G out[3])
        {
            const double u = static_cast<double>(r), v = static_cast<double>(c);
            const G corners[4] = {{u, v, h[0]}, {u, v + 1.0, h[1]}, {u + 1.0, v + 1.0, h[2]}, {u + 1.0, v, h[3]}};
            out[0] = G{u + 0.5, v + 0.5, h[4]};
            out[1] = corners[tri & 3];
            out[2] = corners[(tri + 1) & 3];
        }

        /// Which of a cell's triangles a point of it (0 to 1 on both) lies over (grid.hpp, TriangleAt).
        int TriangleAt(double along_row, double along_col)
        {
            const bool before_main = along_row <= along_col;
            const bool before_anti = along_row + along_col <= 1.0;
            if (before_main) return before_anti ? 0 : 1;
            return before_anti ? 3 : 2;
        }

        /// The height at (u, v) of the plane through a triangle.
        double PlaneZ(const G t[3], double u, double v)
        {
            const double ax = t[1].u - t[0].u, ay = t[1].v - t[0].v, az = t[1].z - t[0].z;
            const double bx = t[2].u - t[0].u, by = t[2].v - t[0].v, bz = t[2].z - t[0].z;
            const double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
            if (std::fabs(nz) < 1e-12) return t[0].z;
            return t[0].z - (nx * (u - t[0].u) + ny * (v - t[0].v)) / nz;
        }

        double Cross(const G& o, const G& a, const G& b) { return (a.u - o.u) * (b.v - o.v) - (a.v - o.v) * (b.u - o.u); }
        double Cross2(const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

        /// @p subject clipped by the convex polygon @p clip (counter-clockwise in grid space).
        void ClipConvex(std::vector<G>& subject, const std::vector<G>& clip, std::vector<G>& scratch)
        {
            const size_t n = clip.size();
            for (size_t e = 0; e < n && !subject.empty(); ++e)
            {
                const G& a = clip[e];
                const G& b = clip[(e + 1) % n];
                scratch.clear();
                const size_t m = subject.size();
                for (size_t i = 0; i < m; ++i)
                {
                    const G&     p  = subject[i];
                    const G&     q  = subject[(i + 1) % m];
                    const double dp = Cross(a, b, p), dq = Cross(a, b, q);
                    if (dp >= 0.0) scratch.push_back(p);
                    if ((dp >= 0.0) != (dq >= 0.0))
                    {
                        const double t = dp / (dp - dq);
                        scratch.push_back(G{p.u + (q.u - p.u) * t, p.v + (q.v - p.v) * t, p.z + (q.z - p.z) * t});
                    }
                }
                subject.swap(scratch);
            }
        }

        bool Inside(const std::vector<G>& convex, double u, double v)
        {
            const G      p{u, v, 0.0};
            const size_t n = convex.size();
            for (size_t e = 0; e < n; ++e)
                if (Cross(convex[e], convex[(e + 1) % n], p) < 0.0) return false;
            return true;
        }

        V3 World(double u, double v, double z)
        {
            const P2 w = FromGrid(u, v);
            return V3{static_cast<float>(w.x), static_cast<float>(w.y), static_cast<float>(z)};
        }

        /// A convex polygon of grid space, its heights already set, as a fan of world triangles.
        void Fan(const std::vector<G>& poly, std::vector<V3>& out)
        {
            for (size_t i = 1; i + 1 < poly.size(); ++i)
            {
                out.push_back(World(poly[0].u, poly[0].v, poly[0].z));
                out.push_back(World(poly[i].u, poly[i].v, poly[i].z));
                out.push_back(World(poly[i + 1].u, poly[i + 1].v, poly[i + 1].z));
            }
        }

        V3 Add(V3 a, V3 b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
        V3 Sub(V3 a, V3 b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
        V3 Scale(V3 a, float s) { return V3{a.x * s, a.y * s, a.z * s}; }
        float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 CrossV(V3 a, V3 b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        float Length(V3 a) { return std::sqrt(Dot(a, a)); }

        /// The turn of a volume: roll around x, pitch nose up, then facing around z.
        struct Turn
        {
            float m[9];
            explicit Turn(float facing, float pitch, float roll)
            {
                const float cf = std::cos(facing), sf = std::sin(facing);
                const float cp = std::cos(pitch), sp = std::sin(pitch);
                const float cr = std::cos(roll), sr = std::sin(roll);
                // Rz(facing) * Ry(-pitch: +x towards +z) * Rx(roll)
                const float ry[9] = {cp, 0.0f, -sp, 0.0f, 1.0f, 0.0f, sp, 0.0f, cp};
                const float rx[9] = {1.0f, 0.0f, 0.0f, 0.0f, cr, -sr, 0.0f, sr, cr};
                const float rz[9] = {cf, -sf, 0.0f, sf, cf, 0.0f, 0.0f, 0.0f, 1.0f};
                float       t[9];
                Mul(ry, rx, t);
                Mul(rz, t, m);
            }
            static void Mul(const float* a, const float* b, float* out)
            {
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) out[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
            }
            V3 operator()(V3 p) const
            {
                return V3{m[0] * p.x + m[1] * p.y + m[2] * p.z, m[3] * p.x + m[4] * p.y + m[5] * p.z, m[6] * p.x + m[7] * p.y + m[8] * p.z};
            }
        };

        /// Segments of an arc of @p angle radians at @p radius yards: about one a yard, 16 for a
        /// small full circle, 96 at most for a full one.
        uint32_t ArcSegments(float radius, float angle)
        {
            const double turn  = std::min(1.0, std::fabs(angle) / (2.0 * kPi));
            const double least = std::max(3.0, std::ceil(16.0 * turn)), most = std::max(least, std::ceil(96.0 * turn));
            return static_cast<uint32_t>(std::clamp(std::ceil(static_cast<double>(std::fabs(angle) * std::max(radius, 1.0f))), least, most));
        }

        /// Whether a polyline turns at a corner enough for its ends to need square caps there.
        bool Turns(V3 before, V3 at, V3 after)
        {
            const V3    a = Sub(at, before), b = Sub(after, at);
            const float la = Length(a), lb = Length(b);
            if (la < 1e-6f || lb < 1e-6f) return false;
            return Dot(a, b) / (la * lb) < 0.906f; // more than 25 degrees
        }

        /// A polyline into segments, laid on the ground when @p ground is given. @p alphas (one per
        /// point, or empty) are the points' opacities; @p along marks each end with how far along the
        /// line it is (a line's, for its opacity from start to end).
        void Polyline(const std::vector<V3>& pts, bool closed, Ground* ground, std::vector<Mesh::Segment>& out,
                      const std::vector<float>* alphas = nullptr, bool along = false)
        {
            const size_t n = pts.size();
            if (n < 2) return;
            const size_t count = closed ? n : n - 1;
            auto capAt = [&](size_t i) {
                if (!closed && (i == 0 || i == n - 1)) return true;
                return Turns(pts[(i + n - 1) % n], pts[i], pts[(i + 1) % n]);
            };
            std::vector<double> at(count + 1, 0.0);
            for (size_t i = 0; i < count; ++i)
            {
                const V3 a = pts[i], b = pts[(i + 1) % n];
                at[i + 1]  = at[i] + std::sqrt(static_cast<double>((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z)));
            }
            const double total = at[count] > 0.0 ? at[count] : 1.0;
            const bool   per   = alphas && alphas->size() == n;
            for (size_t i = 0; i < count; ++i)
            {
                const V3    a = pts[i], b = pts[(i + 1) % n];
                const bool  ca = capAt(i), cb = capAt((i + 1) % n);
                const float aa = per ? (*alphas)[i] : 1.0f, ab = per ? (*alphas)[(i + 1) % n] : 1.0f;
                const float ta = along ? static_cast<float>(at[i] / total) : -1.0f, tb = along ? static_cast<float>(at[i + 1] / total) : -1.0f;
                if (ground) DrapeSegment(a, b, ground, out, ca, cb, aa, ab, ta, tb);
                else out.push_back(Mesh::Segment{a, b, static_cast<uint8_t>(ca), static_cast<uint8_t>(cb), aa, ab, ta, tb});
            }
        }

        /// The distance of a point of the plane to a polygon's border (its edges), the polygon given
        /// in the same plane.
        double BorderDistance(const std::vector<P2>& poly, double x, double y)
        {
            double best = 1e30;
            for (size_t i = 0, n = poly.size(); i < n; ++i)
            {
                const P2     a = poly[i], b = poly[(i + 1) % n];
                const double dx = b.x - a.x, dy = b.y - a.y, len = dx * dx + dy * dy;
                double       t  = len > 0.0 ? ((x - a.x) * dx + (y - a.y) * dy) / len : 0.0;
                t               = std::clamp(t, 0.0, 1.0);
                const double ex = a.x + dx * t - x, ey = a.y + dy * t - y;
                best            = std::min(best, ex * ex + ey * ey);
            }
            return std::sqrt(best);
        }

        /// The points of an arc around a centre, from @p from radians over @p span.
        void Arc(V3 c, float radius, float from, float span, uint32_t segments, std::vector<V3>& out)
        {
            for (uint32_t i = 0; i <= segments; ++i)
            {
                const double t = from + span * static_cast<double>(i) / segments;
                out.push_back(V3{c.x + radius * static_cast<float>(std::cos(t)), c.y + radius * static_cast<float>(std::sin(t)), c.z});
            }
        }

        std::vector<P2> Plane(const std::vector<V3>& pts)
        {
            std::vector<P2> out(pts.size());
            for (size_t i = 0; i < pts.size(); ++i) out[i] = P2{pts[i].x, pts[i].y};
            return out;
        }

        /// A volume's triangle and its outward normal.
        void VolumeTriangle(Mesh& out, V3 a, V3 b, V3 c, V3 inside)
        {
            V3 n = CrossV(Sub(b, a), Sub(c, a));
            const V3 mid = Scale(Add(Add(a, b), c), 1.0f / 3.0f);
            if (Dot(n, Sub(mid, inside)) < 0.0f) n = Scale(n, -1.0f);
            out.triangles.push_back(a);
            out.triangles.push_back(b);
            out.triangles.push_back(c);
            out.normals.push_back(n);
        }

        std::string Say(const char* format, double a = 0.0, double b = 0.0)
        {
            char text[256];
            std::snprintf(text, sizeof text, format, a, b);
            return text;
        }

        bool Finite(V3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    }

    // --- Grid space ----------------------------------------------------------------------------------

    P2 ToGrid(double x, double y) { return P2{(kOrigin - x) / kCellSize, (kOrigin - y) / kCellSize}; }
    P2 FromGrid(double u, double v) { return P2{kOrigin - u * kCellSize, kOrigin - v * kCellSize}; }

    bool HeightAt(Ground& ground, double x, double y, float& out)
    {
        if (!ground.Present()) return false;
        const P2      g = ToGrid(x, y);
        const int64_t r = static_cast<int64_t>(std::floor(g.x)), c = static_cast<int64_t>(std::floor(g.y));
        Cells         cells(&ground);
        double        h[5];
        if (!cells.Heights(r, c, h)) return false;
        G tri[3];
        CellTriangle(r, c, TriangleAt(g.x - static_cast<double>(r), g.y - static_cast<double>(c)), h, tri);
        out = static_cast<float>(PlaneZ(tri, g.x, g.y));
        return true;
    }

    // --- Draping -----------------------------------------------------------------------------------

    void DrapeConvex(const std::vector<P2>& convex, float flat, Ground* ground, std::vector<V3>& out)
    {
        if (convex.size() < 3) return;
        std::vector<G> poly(convex.size());
        double         area = 0.0;
        for (size_t i = 0; i < convex.size(); ++i)
        {
            const P2 g = ToGrid(convex[i].x, convex[i].y);
            poly[i]    = G{g.x, g.y, flat};
        }
        for (size_t i = 0; i < poly.size(); ++i) area += Cross(G{0, 0, 0}, poly[i], poly[(i + 1) % poly.size()]);
        if (std::fabs(area) < 1e-12) return;
        if (area < 0.0) std::reverse(poly.begin(), poly.end());

        if (!ground || !ground->Present())
        {
            Fan(poly, out);
            return;
        }
        double umin = poly[0].u, umax = umin, vmin = poly[0].v, vmax = vmin;
        for (const G& p : poly)
        {
            umin = std::min(umin, p.u);
            umax = std::max(umax, p.u);
            vmin = std::min(vmin, p.v);
            vmax = std::max(vmax, p.v);
        }
        Cells          cells(ground);
        std::vector<G> piece, scratch;
        double         h[5];
        for (int64_t r = static_cast<int64_t>(std::floor(umin)); r <= static_cast<int64_t>(std::floor(umax)); ++r)
            for (int64_t c = static_cast<int64_t>(std::floor(vmin)); c <= static_cast<int64_t>(std::floor(vmax)); ++c)
            {
                if (cells.Heights(r, c, h))
                {
                    for (int t = 0; t < 4; ++t)
                    {
                        G tri[3];
                        CellTriangle(r, c, t, h, tri);
                        if (Inside(poly, tri[0].u, tri[0].v) && Inside(poly, tri[1].u, tri[1].v) && Inside(poly, tri[2].u, tri[2].v))
                        {
                            // Wholly inside: the terrain's own triangle.
                            for (const G& g : tri) out.push_back(World(g.u, g.v, g.z));
                            continue;
                        }
                        piece.assign(tri, tri + 3);
                        if (Cross(piece[0], piece[1], piece[2]) < 0.0) std::reverse(piece.begin(), piece.end());
                        ClipConvex(piece, poly, scratch);
                        if (piece.size() < 3) continue;
                        for (G& g : piece) g.z = PlaneZ(tri, g.u, g.v);
                        Fan(piece, out);
                    }
                }
                else
                {
                    // No ground here: the part of the cell inside the shape, flat.
                    const double u = static_cast<double>(r), v = static_cast<double>(c);
                    piece = {G{u, v, flat}, G{u + 1.0, v, flat}, G{u + 1.0, v + 1.0, flat}, G{u, v + 1.0, flat}};
                    if (Cross(piece[0], piece[1], piece[2]) < 0.0) std::reverse(piece.begin(), piece.end());
                    ClipConvex(piece, poly, scratch);
                    if (piece.size() < 3) continue;
                    for (G& g : piece) g.z = flat;
                    Fan(piece, out);
                }
            }
    }

    void DrapeSegment(V3 a, V3 b, Ground* ground, std::vector<Mesh::Segment>& out, bool cap_a, bool cap_b, float alpha_a, float alpha_b,
                      float t_a, float t_b)
    {
        if (!ground || !ground->Present())
        {
            out.push_back(Mesh::Segment{a, b, static_cast<uint8_t>(cap_a), static_cast<uint8_t>(cap_b), alpha_a, alpha_b, t_a, t_b});
            return;
        }
        const P2 ga = ToGrid(a.x, a.y), gb = ToGrid(b.x, b.y);
        // Where it crosses the terrain's triangles' edges: u, v, u + v and u - v whole numbers.
        std::vector<double> cuts{0.0, 1.0};
        const double        f0[4] = {ga.x, ga.y, ga.x + ga.y, ga.x - ga.y};
        const double        f1[4] = {gb.x, gb.y, gb.x + gb.y, gb.x - gb.y};
        for (int k = 0; k < 4; ++k)
        {
            const double d = f1[k] - f0[k];
            if (std::fabs(d) < 1e-12) continue;
            const double lo = std::min(f0[k], f1[k]), hi = std::max(f0[k], f1[k]);
            for (double w = std::ceil(lo); w <= hi && cuts.size() < 65536; w += 1.0)
            {
                const double t = (w - f0[k]) / d;
                if (t > 0.0 && t < 1.0) cuts.push_back(t);
            }
        }
        std::sort(cuts.begin(), cuts.end());
        Cells        cells(ground);
        double       h[5];
        const size_t first = out.size();
        for (size_t i = 0; i + 1 < cuts.size(); ++i)
        {
            const double t0 = cuts[i], t1 = cuts[i + 1];
            if (t1 - t0 < 1e-9) continue;
            const double  u0 = ga.x + (gb.x - ga.x) * t0, v0 = ga.y + (gb.y - ga.y) * t0;
            const double  u1 = ga.x + (gb.x - ga.x) * t1, v1 = ga.y + (gb.y - ga.y) * t1;
            const double  um = 0.5 * (u0 + u1), vm = 0.5 * (v0 + v1);
            const int64_t r = static_cast<int64_t>(std::floor(um)), c = static_cast<int64_t>(std::floor(vm));
            double        z0 = a.z + (b.z - a.z) * t0, z1 = a.z + (b.z - a.z) * t1;
            if (cells.Heights(r, c, h))
            {
                G tri[3];
                CellTriangle(r, c, TriangleAt(um - static_cast<double>(r), vm - static_cast<double>(c)), h, tri);
                z0 = PlaneZ(tri, u0, v0);
                z1 = PlaneZ(tri, u1, v1);
            }
            const auto lerp = [](float x, float y, double t) { return static_cast<float>(x + (y - x) * t); };
            out.push_back(Mesh::Segment{World(u0, v0, z0), World(u1, v1, z1), 0, 0, lerp(alpha_a, alpha_b, t0), lerp(alpha_a, alpha_b, t1),
                                        t_a < 0.0f ? -1.0f : lerp(t_a, t_b, t0), t_a < 0.0f ? -1.0f : lerp(t_a, t_b, t1)});
        }
        if (out.size() > first)
        {
            out[first].cap_a = static_cast<uint8_t>(cap_a);
            out.back().cap_b = static_cast<uint8_t>(cap_b);
        }
    }

    // --- Triangulation -------------------------------------------------------------------------------

    bool Triangulate(const std::vector<P2>& polygon, std::vector<uint32_t>& out)
    {
        const size_t n = polygon.size();
        if (n < 3) return false;
        double area = 0.0;
        for (size_t i = 0; i < n; ++i) area += polygon[i].x * polygon[(i + 1) % n].y - polygon[(i + 1) % n].x * polygon[i].y;
        if (std::fabs(area) < 1e-12) return false;
        std::vector<uint32_t> ring(n);
        for (size_t i = 0; i < n; ++i) ring[i] = static_cast<uint32_t>(area > 0.0 ? i : n - 1 - i); // counter-clockwise
        const auto inTriangle = [](const P2& p, const P2& a, const P2& b, const P2& c) {
            return Cross2(a, b, p) >= 0.0 && Cross2(b, c, p) >= 0.0 && Cross2(c, a, p) >= 0.0;
        };
        size_t guard = 0;
        while (ring.size() > 3)
        {
            bool         cut = false;
            const size_t m   = ring.size();
            for (size_t i = 0; i < m; ++i)
            {
                const uint32_t ia = ring[(i + m - 1) % m], ib = ring[i], ic = ring[(i + 1) % m];
                const P2&      a = polygon[ia];
                const P2&      b = polygon[ib];
                const P2&      c = polygon[ic];
                if (Cross2(a, b, c) <= 0.0) continue; // a reflex corner, or a flat one
                bool ear = true;
                for (size_t j = 0; j < m && ear; ++j)
                {
                    const uint32_t k = ring[j];
                    if (k == ia || k == ib || k == ic) continue;
                    const P2& p = polygon[k];
                    if ((p.x == a.x && p.y == a.y) || (p.x == b.x && p.y == b.y) || (p.x == c.x && p.y == c.y)) continue;
                    if (inTriangle(p, a, b, c)) ear = false;
                }
                if (!ear) continue;
                out.push_back(ia);
                out.push_back(ib);
                out.push_back(ic);
                ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
                cut = true;
                break;
            }
            if (!cut)
            {
                // Only flat corners left (collinear points): drop one, else the polygon crosses itself.
                bool dropped = false;
                for (size_t i = 0; i < ring.size(); ++i)
                {
                    const size_t mm = ring.size();
                    if (std::fabs(Cross2(polygon[ring[(i + mm - 1) % mm]], polygon[ring[i]], polygon[ring[(i + 1) % mm]])) < 1e-12)
                    {
                        ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
                        dropped = true;
                        break;
                    }
                }
                if (!dropped) return false;
            }
            if (++guard > 4 * n + 16) return false;
        }
        if (Cross2(polygon[ring[0]], polygon[ring[1]], polygon[ring[2]]) > 0.0)
        {
            out.push_back(ring[0]);
            out.push_back(ring[1]);
            out.push_back(ring[2]);
        }
        return true;
    }

    // --- Shapes --------------------------------------------------------------------------------------

    std::string Validate(const Desc& d)
    {
        if (!d.alphas.empty())
        {
            if (d.kind != kLine) return "point_alphas are a line's: this kind has none";
            if (d.alphas.size() != d.points.size()) return "point_alphas must hold one opacity per point";
            for (float a : d.alphas)
                if (!std::isfinite(a) || a < 0.0f || a > 1.0f) return "point_alphas must be opacities from 0 to 1";
        }
        if (d.kind >= kKindCount) return Say("its kind is %.0f, which is none (0 line to 9 cylinder)", d.kind);
        if (!Finite(d.center) || !Finite(d.size) || !std::isfinite(d.radius) || !std::isfinite(d.inner) || !std::isfinite(d.angle) ||
            !std::isfinite(d.facing) || !std::isfinite(d.pitch) || !std::isfinite(d.roll) || !std::isfinite(d.spacing))
            return "a number of it is not finite";
        for (const V3& p : d.points)
            if (!Finite(p)) return "a point of it is not finite";
        if (d.points.size() > kMaxPoints) return Say("it has %.0f points, more than a shape may (%.0f)", double(d.points.size()), kMaxPoints);
        const auto extent = [&](double value, const char* what) -> std::string {
            if (value > kMaxExtent) return Say(what, value, kMaxExtent);
            return std::string();
        };
        std::string why;
        switch (d.kind)
        {
        case kLine:
            if (d.points.size() < 2) return "a line needs 2 points or more";
            for (const V3& p : d.points)
                if (Length(p) > 2.0f * kMaxExtent) return Say("a point of the line is %.1f yards from its center; %.0f at most", Length(p), 2.0 * kMaxExtent);
            return std::string();
        case kCircle:
        case kSphere:
            if (!(d.radius > 0.0f)) return "its radius must be above 0";
            return extent(d.radius, "its radius is %.1f yards; %.0f at most");
        case kRing:
        case kSector:
            if (!(d.radius > 0.0f)) return "its radius must be above 0";
            if (d.inner < 0.0f || d.inner >= d.radius) return "its inner radius must be from 0 to below its radius";
            if (d.kind == kRing && !(d.inner > 0.0f)) return "a ring's inner radius must be above 0 (a circle has none)";
            if (d.kind == kSector && !(d.angle > 0.0f && d.angle <= 2.0f * static_cast<float>(kPi) + 1e-4f))
                return "a sector's angle must be above 0 and up to 2 pi radians";
            return extent(d.radius, "its radius is %.1f yards; %.0f at most");
        case kRectangle:
        case kGrid:
            if (!(d.size.x > 0.0f && d.size.y > 0.0f)) return "its size.x (length) and size.y (width) must be above 0";
            why = extent(0.5 * std::max(d.size.x, d.size.y), "it reaches %.1f yards from its center; %.0f at most");
            if (!why.empty()) return why;
            if (d.kind == kGrid)
            {
                if (!(d.spacing > 0.0f)) return "a grid's spacing must be above 0";
                const double lines = 2.0 * std::floor(0.5 * d.size.x / d.spacing) + 2.0 * std::floor(0.5 * d.size.y / d.spacing) + 6.0;
                if (lines > kMaxGridLines) return Say("the grid would draw %.0f lines, more than one may (%.0f)", lines, kMaxGridLines);
            }
            return std::string();
        case kPolygon:
            if (d.points.size() < 3) return "a polygon needs 3 points or more";
            for (const V3& p : d.points)
                if (std::sqrt(p.x * p.x + p.y * p.y) > kMaxExtent)
                    return Say("a point of the polygon is %.1f yards from its center; %.0f at most", std::sqrt(p.x * p.x + p.y * p.y), kMaxExtent);
            return std::string();
        case kBox:
            if (!(d.size.x > 0.0f && d.size.y > 0.0f && d.size.z > 0.0f)) return "a box's size must be above 0 on x, y and z";
            return extent(0.5 * std::max({d.size.x, d.size.y, d.size.z}), "it reaches %.1f yards from its center; %.0f at most");
        case kCylinder:
            if (!(d.radius > 0.0f && d.size.z > 0.0f)) return "a cylinder's radius and size.z (its height) must be above 0";
            return extent(std::max(d.radius, 0.5f * d.size.z), "it reaches %.1f yards from its center; %.0f at most");
        default: return std::string();
        }
    }

    V3 InPlane(const Mesh& mesh, V3 w)
    {
        const double cf = std::cos(mesh.turn), sf = std::sin(mesh.turn);
        const double dx = static_cast<double>(w.x) - mesh.origin.x, dy = static_cast<double>(w.y) - mesh.origin.y;
        return V3{static_cast<float>(dx * cf + dy * sf), static_cast<float>(-dx * sf + dy * cf), 0.0f};
    }

    namespace
    {
        /// A polygon's inside cut finer where it is nearer its border than @p feather yards, so that
        /// the distances kept at its corners (Mesh::edge) fade it out evenly; @p local is the polygon
        /// in the mesh's plane.
        void Feathered(Mesh& out, const std::vector<P2>& local, float feather)
        {
            struct Tri
            {
                V3    w[3];
                float d[3];
                int   depth;
            };
            const auto distance = [&](V3 w) {
                const V3 p = InPlane(out, w);
                return static_cast<float>(BorderDistance(local, p.x, p.y));
            };
            const double finest = std::max(static_cast<double>(feather), 0.5);
            std::vector<V3> source;
            source.swap(out.triangles);
            out.edge.clear();
            std::vector<Tri> work;
            for (size_t i = 0; i + 2 < source.size(); i += 3)
            {
                Tri t{{source[i], source[i + 1], source[i + 2]}, {}, 0};
                for (int k = 0; k < 3; ++k) t.d[k] = distance(t.w[k]);
                work.push_back(t);
            }
            const size_t budget = kMaxShapeVertices / 2;
            while (!work.empty())
            {
                const Tri t = work.back();
                work.pop_back();
                double longest = 0.0;
                for (int k = 0; k < 3; ++k)
                {
                    const V3 a = t.w[k], b = t.w[(k + 1) % 3];
                    longest    = std::max(longest, std::hypot(static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y)));
                }
                const float nearest = std::min({t.d[0], t.d[1], t.d[2]});
                if (t.depth < 6 && longest > finest && nearest < feather && out.triangles.size() + 3 * work.size() + 12 < budget)
                {
                    V3    m[3];
                    float md[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        const V3 a = t.w[k], b = t.w[(k + 1) % 3];
                        m[k]       = V3{0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (a.z + b.z)};
                        md[k]      = distance(m[k]);
                    }
                    work.push_back(Tri{{t.w[0], m[0], m[2]}, {t.d[0], md[0], md[2]}, t.depth + 1});
                    work.push_back(Tri{{m[0], t.w[1], m[1]}, {md[0], t.d[1], md[1]}, t.depth + 1});
                    work.push_back(Tri{{m[2], m[1], t.w[2]}, {md[2], md[1], t.d[2]}, t.depth + 1});
                    work.push_back(Tri{{m[0], m[1], m[2]}, {md[0], md[1], md[2]}, t.depth + 1});
                    continue;
                }
                for (int k = 0; k < 3; ++k)
                {
                    out.triangles.push_back(t.w[k]);
                    out.edge.push_back(t.d[k]);
                }
            }
        }
    }

    bool Build(const Desc& d, Parts parts, Ground* ground, Mesh& out, std::string* why)
    {
        out.Clear();
        out.ground = IsGround(d.kind) || (d.kind == kLine && (d.flags & kOnGround));
        out.middle = d.center;
        out.origin = d.center;
        out.turn   = d.facing;
        out.plane  = Mesh::Plane{};
        if (std::string refusal = Validate(d); !refusal.empty())
        {
            if (why) *why = refusal;
            return false;
        }
        Ground* lay = (IsGround(d.kind) && !(d.flags & kFlat)) || (d.kind == kLine && (d.flags & kOnGround)) ? ground : nullptr;
        if (lay && !lay->Present()) lay = nullptr;
        const V3  c  = d.center;
        const float fz = c.z;
        std::vector<V3> loop;

        // A loop of the plane at the shape's height, as a ground outline.
        auto outline = [&](const std::vector<V3>& pts, bool closed) {
            if (parts.lines) Polyline(pts, closed, lay, out.lines);
        };
        auto fill = [&](const std::vector<P2>& convex) {
            if (parts.fill) DrapeConvex(convex, fz, lay, out.triangles);
        };

        switch (d.kind)
        {
        case kLine:
        {
            std::vector<V3> pts(d.points.size());
            for (size_t i = 0; i < pts.size(); ++i) pts[i] = Add(c, d.points[i]);
            if (parts.lines) Polyline(pts, (d.flags & kClosed) != 0, lay, out.lines, &d.alphas, true);
            break;
        }
        case kCircle:
        {
            out.plane.radius = out.plane.extent = d.radius;
            out.plane.min_x = out.plane.min_y = -d.radius;
            out.plane.max_x = out.plane.max_y = d.radius;
            const float tau = static_cast<float>(2.0 * kPi);
            Arc(c, d.radius, 0.0f, tau, ArcSegments(d.radius, tau), loop);
            loop.pop_back();
            outline(loop, true);
            fill(Plane(loop));
            break;
        }
        case kRing:
        case kSector:
        {
            const bool  whole = d.kind == kRing || d.angle >= static_cast<float>(2.0 * kPi) - 1e-4f;
            const float span  = whole ? static_cast<float>(2.0 * kPi) : d.angle;
            out.plane.radius = out.plane.extent = d.radius;
            out.plane.inner      = d.inner;
            out.plane.half_angle = whole ? 4.0f : 0.5f * span;
            out.plane.min_x = out.plane.min_y = -d.radius;
            out.plane.max_x = out.plane.max_y = d.radius;
            const float from  = whole ? 0.0f : d.facing - 0.5f * span;
            const uint32_t n  = ArcSegments(d.radius, span);
            std::vector<V3> outer, inner;
            Arc(c, d.radius, from, span, n, outer);
            if (d.inner > 0.0f) Arc(c, d.inner, from, span, n, inner);
            if (parts.lines)
            {
                if (whole)
                {
                    outer.pop_back();
                    outline(outer, true);
                    outer.push_back(outer.front());
                    if (!inner.empty())
                    {
                        inner.pop_back();
                        outline(inner, true);
                        inner.push_back(inner.front());
                    }
                }
                else
                {
                    loop = outer;
                    if (inner.empty()) loop.push_back(c);
                    else loop.insert(loop.end(), inner.rbegin(), inner.rend());
                    outline(loop, true);
                }
            }
            if (parts.fill)
            {
                if (inner.empty())
                {
                    // Convex pieces: the centre and up to a quarter turn of the arc.
                    const uint32_t step = std::max<uint32_t>(1, static_cast<uint32_t>(std::floor(n * (0.5 * kPi) / span)));
                    for (uint32_t i = 0; i < n; i += step)
                    {
                        std::vector<P2> piece{P2{c.x, c.y}};
                        for (uint32_t k = i; k <= std::min(n, i + step); ++k) piece.push_back(P2{outer[k].x, outer[k].y});
                        fill(piece);
                    }
                }
                else
                    for (uint32_t i = 0; i < n; ++i)
                        fill({P2{inner[i].x, inner[i].y}, P2{outer[i].x, outer[i].y}, P2{outer[i + 1].x, outer[i + 1].y},
                              P2{inner[i + 1].x, inner[i + 1].y}});
            }
            break;
        }
        case kRectangle:
        case kGrid:
        {
            const float cf = std::cos(d.facing), sf = std::sin(d.facing);
            auto at = [&](float along, float across) {
                return V3{c.x + along * cf - across * sf, c.y + along * sf + across * cf, c.z};
            };
            const float hx = 0.5f * d.size.x, hy = 0.5f * d.size.y;
            out.plane.code   = 1;
            out.plane.hx     = hx;
            out.plane.hy     = hy;
            out.plane.extent = std::sqrt(hx * hx + hy * hy);
            out.plane.min_x  = -hx;
            out.plane.max_x  = hx;
            out.plane.min_y  = -hy;
            out.plane.max_y  = hy;
            loop = {at(hx, hy), at(-hx, hy), at(-hx, -hy), at(hx, -hy)};
            fill(Plane(loop));
            if (!parts.lines) break;
            outline(loop, true);
            if (d.kind == kGrid)
            {
                // Lines every spacing from the centre, inside the border.
                const int kx = static_cast<int>(std::floor(hx / d.spacing)), ky = static_cast<int>(std::floor(hy / d.spacing));
                for (int k = -kx; k <= kx; ++k)
                {
                    const float x = static_cast<float>(k) * d.spacing;
                    if (std::fabs(std::fabs(x) - hx) < 1e-3f) continue; // the border
                    Polyline({at(x, -hy), at(x, hy)}, false, lay, out.lines);
                }
                for (int k = -ky; k <= ky; ++k)
                {
                    const float y = static_cast<float>(k) * d.spacing;
                    if (std::fabs(std::fabs(y) - hy) < 1e-3f) continue;
                    Polyline({at(-hx, y), at(hx, y)}, false, lay, out.lines);
                }
            }
            break;
        }
        case kPolygon:
        {
            for (const V3& p : d.points) loop.push_back(V3{c.x + p.x, c.y + p.y, c.z});
            // Its own plane: the points turned by -facing, its bounds there.
            const double    cf = std::cos(d.facing), sf = std::sin(d.facing);
            std::vector<P2> local(d.points.size());
            out.plane.code = 2;
            for (size_t i = 0; i < d.points.size(); ++i)
            {
                const V3 p = d.points[i];
                local[i]   = P2{p.x * cf + p.y * sf, -p.x * sf + p.y * cf};
                const float lx = static_cast<float>(local[i].x), ly = static_cast<float>(local[i].y);
                out.plane.min_x  = i ? std::min(out.plane.min_x, lx) : lx;
                out.plane.max_x  = i ? std::max(out.plane.max_x, lx) : lx;
                out.plane.min_y  = i ? std::min(out.plane.min_y, ly) : ly;
                out.plane.max_y  = i ? std::max(out.plane.max_y, ly) : ly;
                out.plane.extent = std::max(out.plane.extent, std::sqrt(lx * lx + ly * ly));
            }
            if (parts.fill)
            {
                const std::vector<P2> plane = Plane(loop);
                std::vector<uint32_t> tris;
                if (!Triangulate(plane, tris))
                {
                    if (why) *why = "the polygon crosses itself, or its points are all on one line";
                    out.Clear();
                    return false;
                }
                for (size_t i = 0; i + 2 < tris.size(); i += 3) fill({plane[tris[i]], plane[tris[i + 1]], plane[tris[i + 2]]});
                if (parts.feather > 0.0f) Feathered(out, local, parts.feather);
            }
            outline(loop, true);
            break;
        }
        case kBox:
        {
            const Turn turn(d.facing, d.pitch, d.roll);
            const V3   h{0.5f * d.size.x, 0.5f * d.size.y, 0.5f * d.size.z};
            V3         k[8];
            for (int i = 0; i < 8; ++i)
                k[i] = Add(c, turn(V3{(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z}));
            if (parts.lines)
            {
                static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (const auto& e : edges) out.lines.push_back(Mesh::Segment{k[e[0]], k[e[1]], 1, 1});
            }
            if (parts.fill)
            {
                static const int faces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
                for (const auto& f : faces)
                {
                    VolumeTriangle(out, k[f[0]], k[f[1]], k[f[2]], c);
                    VolumeTriangle(out, k[f[0]], k[f[2]], k[f[3]], c);
                }
            }
            break;
        }
        case kSphere:
        {
            const float tau = static_cast<float>(2.0 * kPi);
            const uint32_t n = ArcSegments(d.radius, tau);
            auto ring = [&](V3 axisA, V3 axisB, V3 centre, float r) {
                std::vector<V3> pts;
                for (uint32_t i = 0; i < n; ++i)
                {
                    const float t = tau * static_cast<float>(i) / static_cast<float>(n);
                    pts.push_back(Add(centre, Add(Scale(axisA, r * std::cos(t)), Scale(axisB, r * std::sin(t)))));
                }
                Polyline(pts, true, nullptr, out.lines);
            };
            if (parts.lines)
            {
                const V3    x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
                const float r45 = d.radius * 0.70710678f;
                ring(x, y, c, d.radius);
                ring(x, y, Add(c, V3{0, 0, r45}), r45);
                ring(x, y, Add(c, V3{0, 0, -r45}), r45);
                ring(x, z, c, d.radius);
                ring(y, z, c, d.radius);
            }
            if (parts.fill)
            {
                const uint32_t slices = std::max<uint32_t>(12, n / 2), stacks = std::max<uint32_t>(8, n / 4);
                auto p = [&](uint32_t i, uint32_t j) {
                    const float th = tau * static_cast<float>(i) / static_cast<float>(slices);
                    const float ph = static_cast<float>(kPi) * (static_cast<float>(j) / static_cast<float>(stacks) - 0.5f);
                    return Add(c, V3{d.radius * std::cos(ph) * std::cos(th), d.radius * std::cos(ph) * std::sin(th), d.radius * std::sin(ph)});
                };
                for (uint32_t j = 0; j < stacks; ++j)
                    for (uint32_t i = 0; i < slices; ++i)
                    {
                        const V3 a = p(i, j), b = p(i + 1, j), e = p(i + 1, j + 1), f = p(i, j + 1);
                        if (j > 0) VolumeTriangle(out, a, b, e, c);
                        if (j + 1 < stacks) VolumeTriangle(out, a, e, f, c);
                    }
            }
            break;
        }
        case kCylinder:
        {
            const Turn     turn(d.facing, d.pitch, d.roll);
            const float    tau = static_cast<float>(2.0 * kPi);
            const uint32_t n   = ArcSegments(d.radius, tau);
            const float    hz  = 0.5f * d.size.z;
            std::vector<V3> top, bottom;
            for (uint32_t i = 0; i < n; ++i)
            {
                const float t = tau * static_cast<float>(i) / static_cast<float>(n);
                const float x = d.radius * std::cos(t), y = d.radius * std::sin(t);
                top.push_back(Add(c, turn(V3{x, y, hz})));
                bottom.push_back(Add(c, turn(V3{x, y, -hz})));
            }
            if (parts.lines)
            {
                Polyline(top, true, nullptr, out.lines);
                Polyline(bottom, true, nullptr, out.lines);
                for (uint32_t i = 0; i < 8; ++i)
                {
                    const uint32_t k = i * n / 8;
                    out.lines.push_back(Mesh::Segment{bottom[k], top[k], 1, 1});
                }
            }
            if (parts.fill)
            {
                const V3 ct = Add(c, turn(V3{0, 0, hz})), cb = Add(c, turn(V3{0, 0, -hz}));
                for (uint32_t i = 0; i < n; ++i)
                {
                    const uint32_t k = (i + 1) % n;
                    VolumeTriangle(out, bottom[i], bottom[k], top[k], c);
                    VolumeTriangle(out, bottom[i], top[k], top[i], c);
                    VolumeTriangle(out, ct, top[i], top[k], c);
                    VolumeTriangle(out, cb, bottom[k], bottom[i], c);
                }
            }
            break;
        }
        default: break;
        }
        if (out.Vertices() > kMaxShapeVertices)
        {
            if (why)
                *why = Say("it would draw %.0f vertices, more than one shape may (%.0f): make it smaller, or its lines fewer",
                           static_cast<double>(out.Vertices()), kMaxShapeVertices);
            out.Clear();
            return false;
        }
        return true;
    }

    // --- The stream ----------------------------------------------------------------------------------

    uint8_t WidthCode(float pixels)
    {
        if (!(pixels > 0.5f)) pixels = 0.5f;
        if (pixels > kMaxWidth) pixels = kMaxWidth;
        return static_cast<uint8_t>(std::lround(pixels * 8.0f));
    }

    uint8_t SoftCode(float pixels)
    {
        if (!(pixels > 0.0f)) return 0;
        return static_cast<uint8_t>(std::min<long>(std::lround(std::min(pixels, kMaxSoftness) * 4.0f), 63));
    }

    int8_t OffsetCode(float pixels)
    {
        if (!std::isfinite(pixels)) return 0;
        return static_cast<int8_t>(std::clamp<long>(std::lround(std::clamp(pixels, -kMaxShadowOffset, kMaxShadowOffset) * 2.0f), -32, 31));
    }

    namespace
    {
        void Set(Vertex& v, V3 p, V3 o, V3 eye, uint32_t color, uint8_t side, uint8_t end, uint8_t width, uint8_t flags)
        {
            v.pos[0]   = p.x - eye.x;
            v.pos[1]   = p.y - eye.y;
            v.pos[2]   = p.z - eye.z;
            v.other[0] = o.x - eye.x;
            v.other[1] = o.y - eye.y;
            v.other[2] = o.z - eye.z;
            v.color    = color;
            v.side     = side;
            v.end      = end;
            v.width    = width;
            v.flags    = flags;
        }
    }

    void PushSegment(std::vector<Vertex>& out, const Mesh::Segment& s, V3 eye, uint32_t color, uint8_t width, bool lift)
    {
        const uint8_t l  = lift ? 1 : 0;
        const uint8_t fa = static_cast<uint8_t>(l + (s.cap_a ? 2 : 0)), fb = static_cast<uint8_t>(l + (s.cap_b ? 2 : 0));
        Vertex        q[4];
        Set(q[0], s.a, s.b, eye, color, 0, 0, width, fa);
        Set(q[1], s.b, s.a, eye, color, 0, 1, width, fb);
        Set(q[2], s.b, s.a, eye, color, 2, 1, width, fb);
        Set(q[3], s.a, s.b, eye, color, 2, 0, width, fa);
        out.push_back(q[0]);
        out.push_back(q[1]);
        out.push_back(q[2]);
        out.push_back(q[0]);
        out.push_back(q[2]);
        out.push_back(q[3]);
    }

    void PushSegment(std::vector<Vertex>& out, const Mesh::Segment& s, V3 eye, const LineLook& look, bool lift)
    {
        // Each end's colour: the look's alpha times the end's opacity and, along a line, the
        // opacity from its start to its end.
        const auto at = [&](float alpha, float t) {
            float m = alpha;
            if (t >= 0.0f) m *= look.alpha_start + (look.alpha_end - look.alpha_start) * t;
            const float    a = static_cast<float>(look.color >> 24) * std::clamp(m, 0.0f, 1.0f);
            return (static_cast<uint32_t>(std::lround(a)) << 24) | (look.color & 0xFFFFFFu);
        };
        const uint32_t ca = at(s.alpha_a, s.t_a), cb = at(s.alpha_b, s.t_b);
        const uint8_t  l  = lift ? 1 : 0;
        const uint8_t  soft = static_cast<uint8_t>(std::min<uint32_t>(look.soft, 63u) << 2);
        const uint8_t  fa = static_cast<uint8_t>(l + (s.cap_a ? 2 : 0) + soft), fb = static_cast<uint8_t>(l + (s.cap_b ? 2 : 0) + soft);
        const uint8_t  dy = static_cast<uint8_t>((static_cast<uint8_t>(look.down) & 63u) << 2);
        const uint8_t  dx = static_cast<uint8_t>((static_cast<uint8_t>(look.right) & 63u) << 1);
        Vertex         q[4];
        Set(q[0], s.a, s.b, eye, ca, static_cast<uint8_t>(0 | dy), static_cast<uint8_t>(0 | dx), look.width, fa);
        Set(q[1], s.b, s.a, eye, cb, static_cast<uint8_t>(0 | dy), static_cast<uint8_t>(1 | dx), look.width, fb);
        Set(q[2], s.b, s.a, eye, cb, static_cast<uint8_t>(2 | dy), static_cast<uint8_t>(1 | dx), look.width, fb);
        Set(q[3], s.a, s.b, eye, ca, static_cast<uint8_t>(2 | dy), static_cast<uint8_t>(0 | dx), look.width, fa);
        out.push_back(q[0]);
        out.push_back(q[1]);
        out.push_back(q[2]);
        out.push_back(q[0]);
        out.push_back(q[2]);
        out.push_back(q[3]);
    }

    void PushSurfaceTriangle(std::vector<Vertex>& out, const V3 world[3], const V3 plane[3], V3 eye, uint32_t color, bool lift)
    {
        const uint8_t l = lift ? 1 : 0;
        for (int k = 0; k < 3; ++k)
        {
            Vertex v;
            Set(v, world[k], eye, eye, color, 1, 0, 0, l);
            v.other[0] = plane[k].x;
            v.other[1] = plane[k].y;
            v.other[2] = plane[k].z;
            out.push_back(v);
        }
    }

    void PushTriangle(std::vector<Vertex>& out, V3 a, V3 b, V3 c, V3 eye, uint32_t color, bool lift)
    {
        const uint8_t l = lift ? 1 : 0;
        Vertex        v;
        Set(v, a, a, eye, color, 1, 0, 0, l);
        out.push_back(v);
        Set(v, b, b, eye, color, 1, 0, 0, l);
        out.push_back(v);
        Set(v, c, c, eye, color, 1, 0, 0, l);
        out.push_back(v);
    }

    void Constants(const View& view, float out[24])
    {
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) out[4 * j + i] = view.vp[4 * i + j];
        out[16] = view.viewport[2];
        out[17] = view.viewport[3];
        out[18] = view.scale / 16.0f;
        out[19] = 0.0f;
        out[20] = kLiftScale;
        out[21] = kLiftYards;
        out[22] = 0.0f;
        out[23] = 0.0f;
    }

    void Project(const float k[24], const Vertex& vx, float out[4])
    {
        // The program's arithmetic, line for line (shaders/draw.hlsl, glsl/draw.vert).
        const float w    = static_cast<float>(vx.flags);
        const float lift = std::fmod(w, 2.0f), cap = std::fmod(std::floor(w * 0.5f), 2.0f), soft = std::floor(w * 0.25f);
        const float sx = static_cast<float>(vx.side), ex = static_cast<float>(vx.end);
        float       downCode = std::floor(sx * 0.25f), rightCode = std::floor(ex * 0.5f);
        downCode  = downCode >= 32.0f ? downCode - 64.0f : downCode;
        rightCode = rightCode >= 32.0f ? rightCode - 64.0f : rightCode;
        auto lifted = [&](const float* p) {
            V3          v{p[0], p[1], p[2]};
            const float d    = Length(v);
            const float pull = std::min(lift * (k[20] * d + k[21]), 0.5f * d);
            return Scale(v, 1.0f - pull / std::max(d, 1e-4f));
        };
        auto clip = [&](V3 p, float r[4]) {
            for (int j = 0; j < 4; ++j) r[j] = p.x * k[4 * j] + p.y * k[4 * j + 1] + p.z * k[4 * j + 2] + k[4 * j + 3];
        };
        float a[4], b[4];
        clip(lifted(vx.pos), a);
        clip(lifted(vx.other), b);
        const float side = std::fmod(sx, 4.0f) - 1.0f;
        if (side != 0.0f)
        {
            float a0[4], b0[4];
            std::memcpy(a0, a, sizeof a0);
            std::memcpy(b0, b, sizeof b0);
            if (a0[2] < 0.0f)
            {
                const float t = std::clamp(a0[2] / (a0[2] - b0[2]), 0.0f, 1.0f);
                for (int j = 0; j < 4; ++j) a[j] = a0[j] + (b0[j] - a0[j]) * t;
            }
            if (b0[2] < 0.0f)
            {
                const float t = std::clamp(b0[2] / (b0[2] - a0[2]), 0.0f, 1.0f);
                for (int j = 0; j < 4; ++j) b[j] = b0[j] + (a0[j] - b0[j]) * t;
            }
            const float hw = 0.5f * k[16], hh = 0.5f * k[17];
            const float wa = std::max(a[3], 1e-6f), wb = std::max(b[3], 1e-6f);
            const float sax = a[0] / wa * hw, say = a[1] / wa * hh;
            const float sbx = b[0] / wb * hw, sby = b[1] / wb * hh;
            float       dx = sbx - sax, dy = sby - say;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-4f)
            {
                dx /= len;
                dy /= len;
            }
            else
            {
                dx = 1.0f;
                dy = 0.0f;
            }
            const float along = 1.0f - 2.0f * std::fmod(ex, 2.0f); // d along the segment, first to second end
            const float nx = -dy * along, ny = dx * along;
            const float half = static_cast<float>(vx.width) * k[18] + 2.0f * soft * k[18];
            const float ox = (nx * side - dx * cap) * half, oy = (ny * side - dy * cap) * half;
            a[0] += ox / hw * wa;
            a[1] += oy / hh * wa;
        }
        // A shadow's move on the screen, in half pixels at 1080 lines (y down).
        const float hw = 0.5f * k[16], hh = 0.5f * k[17];
        a[0] += rightCode * 8.0f * k[18] / hw * a[3];
        a[1] -= downCode * 8.0f * k[18] / hh * a[3];
        std::memcpy(out, a, sizeof a);
    }

    // --- Surfaces ------------------------------------------------------------------------------------

    SurfaceConstants MakeSurface(const Mesh& mesh, const SurfaceLook& look)
    {
        SurfaceConstants out;
        const Mesh::Plane& pl = mesh.plane;
        const double       sx = look.size_x > 0.0f ? look.size_x : 1.0, sy = look.size_y > 0.0f ? look.size_y : 1.0;
        const double       cr = std::cos(look.rotation), sr = std::sin(look.rotation);
        // p = A (x, y) + B: the picture's frame before its turn and size (s to the right, t down).
        double A[2][2], B[2], O[2];
        if (look.uv == kUvWorld)
        {
            // East and south of the world's origin, in yards: s = -world y, t = -world x.
            const double cf = std::cos(mesh.turn), sf = std::sin(mesh.turn);
            A[0][0] = -sf, A[0][1] = -cf, B[0] = -static_cast<double>(mesh.origin.y);
            A[1][0] = -cf, A[1][1] = sf, B[1] = -static_cast<double>(mesh.origin.x);
            O[0] = look.offset_x, O[1] = look.offset_y;
        }
        else
        {
            // Across the bounds from their left (the shape's right is its plane's -y) and top (+x),
            // around their middle.
            const double wx = std::max(1e-6, static_cast<double>(pl.max_x) - pl.min_x), wy = std::max(1e-6, static_cast<double>(pl.max_y) - pl.min_y);
            A[0][0] = 0.0, A[0][1] = -1.0 / wy, B[0] = pl.max_y / wy - 0.5;
            A[1][0] = -1.0 / wx, A[1][1] = 0.0, B[1] = pl.max_x / wx - 0.5;
            O[0] = 0.5 + look.offset_x, O[1] = 0.5 + look.offset_y;
        }
        // q = M p + O: turned counter-clockwise as seen from above, one picture per size.
        const double M[2][2] = {{cr / sx, -sr / sx}, {sr / sy, cr / sy}};
        for (int r = 0; r < 2; ++r)
        {
            double x = M[r][0] * A[0][0] + M[r][1] * A[1][0];
            double y = M[r][0] * A[0][1] + M[r][1] * A[1][1];
            double c = M[r][0] * B[0] + M[r][1] * B[1] + O[r];
            if (Repeats(look.uv)) c -= std::floor(c); // a whole picture changes nothing where it repeats
            out.k[r][0] = static_cast<float>(x);
            out.k[r][1] = static_cast<float>(y);
            out.k[r][2] = static_cast<float>(c);
        }
        out.k[2][0] = look.right > 0.0f ? std::min(look.right, 1.0f) : 1.0f;
        out.k[2][1] = look.bottom > 0.0f ? std::min(look.bottom, 1.0f) : 1.0f;
        out.k[2][2] = Repeats(look.uv) ? 1.0f : 0.0f;
        out.k[2][3] = look.textured ? 1.0f : 0.0f;
        out.k[3][0] = static_cast<float>(pl.code);
        out.k[3][1] = pl.radius;
        out.k[3][2] = pl.inner;
        out.k[3][3] = std::max(look.feather, 0.0f);
        out.k[4][0] = pl.half_angle;
        out.k[4][1] = pl.hx;
        out.k[4][2] = pl.hy;
        out.k[4][3] = look.radial > 0.0f ? 1.0f - std::min(look.radial, 1.0f) : 2.0f;
        out.k[5][0] = 1.0f / std::max(pl.extent, 1e-4f);
        out.k[5][1] = look.blend == kBlendModulate ? 1.0f : 0.0f;
        return out;
    }

    namespace
    {
        float Saturate(float x) { return std::min(std::max(x, 0.0f), 1.0f); }
        float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }
    }

    void Picture(const SurfaceConstants& s, float x, float y, float out[2])
    {
        // The pixel programs' arithmetic (draw.hlsl SurfacePs, draw_surface.frag).
        const float u = s.k[0][0] * x + s.k[0][1] * y + s.k[0][2];
        const float v = s.k[1][0] * x + s.k[1][1] * y + s.k[1][2];
        if (s.k[2][2] > 0.5f)
        {
            out[0] = (u - std::floor(u)) * s.k[2][0];
            out[1] = (v - std::floor(v)) * s.k[2][1];
        }
        else
        {
            out[0] = Saturate(u) * s.k[2][0];
            out[1] = Saturate(v) * s.k[2][1];
        }
    }

    void Shade(const SurfaceConstants& s, V3 plane, const float color[4], const float texel[4], float out[4])
    {
        // The pixel programs' arithmetic, line for line.
        const float x = plane.x, y = plane.y;
        const float r = std::sqrt(x * x + y * y);
        float       d;
        if (s.k[3][0] < 0.5f)
        {
            d = s.k[3][1] - r;
            if (s.k[3][2] > 0.0f) d = std::min(d, r - s.k[3][2]);
            if (s.k[4][0] < 3.14159265f)
            {
                const float diff = s.k[4][0] - std::atan2(std::fabs(y), x);
                d                = std::min(d, diff < 1.57079633f ? r * std::sin(diff) : r);
            }
        }
        else if (s.k[3][0] < 1.5f)
            d = std::min(s.k[4][1] - std::fabs(x), s.k[4][2] - std::fabs(y));
        else
            d = plane.z;
        const float edge = s.k[3][3] > 0.0f ? Smooth(Saturate(d / s.k[3][3])) : 1.0f;
        float       fade = 1.0f;
        if (s.k[4][3] < 1.5f) fade = 1.0f - Smooth(Saturate((r * s.k[5][0] - s.k[4][3]) / std::max(1.0f - s.k[4][3], 1e-4f)));
        const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const float* t     = s.k[2][3] > 0.5f ? texel : one;
        const float  a     = color[3] * t[3] * edge * fade;
        for (int i = 0; i < 3; ++i)
        {
            const float c = color[i] * t[i];
            out[i]        = s.k[5][1] > 0.5f ? 1.0f + (c - 1.0f) * a : c;
        }
        out[3] = a;
    }

    float LineCoverage(float across, float half_width, float soft)
    {
        return soft > 0.0f ? Saturate((half_width - std::fabs(across)) / soft) : 1.0f;
    }
}
