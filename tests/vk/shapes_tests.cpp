// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's geometry (src/gx/shapes/) and its Vulkan passes (src/gx/vk/render/Shapes.cpp): ground
// shapes laid on a made-up terrain exactly (every vertex and every edge's middle at the height of
// the ground under it), sectors, rings and polygons of the right area, polygons cut into triangles,
// lines widened on the screen as the vertex programs widen them and cut at the near plane; then on
// the driver: a triangle and a line drawn into the scene against its depth and on top, their
// widths, colours and blending, held to the CPU's copy of the vertex program.

#include "gx/shapes/Geometry.hpp"
#include "gx/vk/render/Shapes.hpp"

#include "Check.hpp"
#include "Rig.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(WXL_VK_SPIRV_VAL)
#include <spirv-tools/libspirv.hpp>
#endif

namespace
{
    namespace sh     = wxl::gx::shapes;
    namespace render = wxl::vk::render;
    using namespace wxl::vk::test;
    using namespace wxl::vk::test::rig;

    constexpr double kOrigin = 17066.666, kCell = 4.1666665, kChunk = 33.333332, kPi = 3.14159265358979323846;

    /// A made-up ground: z = f(x, y) at the grid's vertices, over the chunks around the map's middle;
    /// the cells of one 2 x 2 hole square of chunk (512, 512) are holes, chunk (514, 512) is not loaded.
    class TestGround final : public sh::Ground
    {
    public:
        static double F(double x, double y) { return 20.0 + 3.0 * std::sin(x * 0.13) + 2.0 * std::cos(y * 0.21) + 0.05 * x; }

        bool Present() override { return present; }
        bool Chunk(int32_t cx, int32_t cy, const float*& heights, uint16_t& holes) override
        {
            if (cx < 509 || cx > 515 || cy < 509 || cy > 515 || (cx == 514 && cy == 512)) return false;
            const int slot = (cy - 509) * 7 + (cx - 509);
            float*    h    = cache[slot];
            heights        = h;
            holes          = (cx == 512 && cy == 512) ? 0x0001 : 0;
            if (made[slot]) return true;
            made[slot]     = true;
            const double cornerX = kOrigin - cy * kChunk, cornerY = kOrigin - cx * kChunk;
            for (uint32_t i = 0; i < 145; ++i)
            {
                const int  row = static_cast<int>(i / 17), col = static_cast<int>(i % 17);
                const bool inner = col >= 9;
                const double half = inner ? 0.5 * kCell : 0.0;
                const double x = cornerX - row * kCell - half, y = cornerY - (inner ? col - 9 : col) * kCell - half;
                h[i]           = static_cast<float>(F(x, y));
            }
            heights = h;
            holes   = (cx == 512 && cy == 512) ? 0x0001 : 0;
            return true;
        }
        bool  present = true;
        float cache[49][145];
        bool  made[49] = {};
    };

    /// A point in the middle of chunk (cx, cy).
    sh::V3 ChunkMiddle(int cx, int cy) { return sh::V3{static_cast<float>(kOrigin - (cy + 0.5) * kChunk), static_cast<float>(kOrigin - (cx + 0.5) * kChunk), 20.0f}; }

    double Area(const std::vector<sh::V3>& t)
    {
        double a = 0.0;
        for (size_t i = 0; i + 2 < t.size(); i += 3)
            a += 0.5 * std::fabs((t[i + 1].x - t[i].x) * (t[i + 2].y - t[i].y) - (t[i + 1].y - t[i].y) * (t[i + 2].x - t[i].x));
        return a;
    }

    /// The worst gap between a ground mesh and the ground: at its triangles' corners and the middles
    /// of their edges (a triangle crossing a terrain edge shows at its edges' middles).
    double WorstGap(TestGround& ground, const std::vector<sh::V3>& t, float flat)
    {
        double worst = 0.0;
        auto at = [&](double x, double y, double z) {
            float h = flat;
            if (!sh::HeightAt(ground, x, y, h)) h = flat;
            worst = std::max(worst, std::fabs(z - h));
        };
        for (size_t i = 0; i + 2 < t.size(); i += 3)
            for (int k = 0; k < 3; ++k)
            {
                const sh::V3& a = t[i + k];
                const sh::V3& b = t[i + (k + 1) % 3];
                at(a.x, a.y, a.z);
                at(0.5 * (a.x + b.x), 0.5 * (a.y + b.y), 0.5 * (a.z + b.z));
            }
        return worst;
    }

    void TriangulateCases()
    {
        std::vector<uint32_t> out;
        Check(sh::Triangulate({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, out) && out.size() == 6, "a square is two triangles");
        out.clear();
        const std::vector<sh::P2> l = {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
        Check(sh::Triangulate(l, out) && out.size() == 12, "a concave L of six corners is four triangles");
        double area = 0.0;
        for (size_t i = 0; i < out.size(); i += 3)
        {
            const sh::P2 &a = l[out[i]], &b = l[out[i + 1]], &c = l[out[i + 2]];
            area += 0.5 * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
        }
        Check(std::fabs(area - 3.0) < 1e-9, "the L's triangles cover its area, each counter-clockwise");
        out.clear();
        std::vector<sh::P2> cw(l.rbegin(), l.rend());
        Check(sh::Triangulate(cw, out) && out.size() == 12, "a clockwise polygon is cut as well");
        out.clear();
        Check(!sh::Triangulate({{0, 0}, {1, 1}, {1, 0}, {0, 1}}, out), "a polygon crossing itself is refused");
        out.clear();
        Check(sh::Triangulate({{0, 0}, {1, 0}, {2, 0}, {2, 2}, {0, 2}}, out) && out.size() >= 6, "a point on an edge is no trouble");
    }

    void DrapeCases()
    {
        TestGround ground;
        const sh::V3 c = ChunkMiddle(511, 511);
        sh::Desc     circle;
        circle.kind   = sh::kCircle;
        circle.center = c;
        circle.radius = 12.0f;
        sh::Mesh    mesh;
        std::string why;
        Check(sh::Build(circle, sh::Parts{true, true}, &ground, mesh, &why), "a circle on the ground is built");
        Check(mesh.ground && !mesh.triangles.empty() && !mesh.lines.empty(), "it has a fill and an outline, and lies on the ground");
        const double gap = WorstGap(ground, mesh.triangles, c.z);
        std::printf("  circle of 12 yd: %zu triangles, %zu segments, worst gap to the ground %.5f yd\n", mesh.triangles.size() / 3,
                    mesh.lines.size(), gap);
        Check(gap < 2e-3, "every corner and every edge's middle of its fill lies on the ground");
        const double area = Area(mesh.triangles);
        Check(std::fabs(area - kPi * 144.0) / (kPi * 144.0) < 0.01, "its fill covers the circle's area (within 1%)");
        double lineGap = 0.0;
        for (const sh::Mesh::Segment& s : mesh.lines)
        {
            float h = 0.0f;
            sh::HeightAt(ground, 0.5 * (s.a.x + s.b.x), 0.5 * (s.a.y + s.b.y), h);
            lineGap = std::max(lineGap, std::fabs(0.5 * (s.a.z + s.b.z) - h));
        }
        Check(lineGap < 2e-3, "every segment of its outline lies on the ground at its middle");

        // A shape over a hole and over a chunk not loaded: flat at its height there.
        sh::Desc rect;
        rect.kind   = sh::kRectangle;
        rect.center = sh::V3{static_cast<float>(kOrigin - (512 * 8 + 2) * kCell), static_cast<float>(kOrigin - (512 * 8 + 2) * kCell), 7.0f};
        rect.size   = sh::V3{6.0f, 6.0f, 0.0f};
        Check(sh::Build(rect, sh::Parts{false, true}, &ground, mesh, &why), "a rectangle over a hole is built");
        bool flat = false, laid = false;
        for (const sh::V3& v : mesh.triangles) (std::fabs(v.z - 7.0f) < 1e-4f ? flat : laid) = true;
        Check(flat && laid, "over the hole it lies flat at its height, around it on the ground");
        Check(std::fabs(Area(mesh.triangles) - 36.0) < 0.05, "and covers its whole area");
        rect.center = ChunkMiddle(514, 512);
        rect.center.z = 3.0f;
        Check(sh::Build(rect, sh::Parts{false, true}, &ground, mesh, &why), "a rectangle over a chunk not loaded is built");
        bool allFlat = !mesh.triangles.empty();
        for (const sh::V3& v : mesh.triangles) allFlat = allFlat && std::fabs(v.z - 3.0f) < 1e-4f;
        Check(allFlat, "it lies flat at its height");

        // Flat on request, and on a map without terrain.
        circle.flags = sh::kFlat;
        Check(sh::Build(circle, sh::Parts{true, true}, &ground, mesh, &why), "a flat circle is built");
        bool level = true;
        for (const sh::V3& v : mesh.triangles) level = level && v.z == c.z;
        Check(level && !mesh.triangles.empty() && mesh.triangles.size() < 400, "flat: at its height, a fan and no cut along the ground");
        circle.flags    = 0;
        ground.present  = false;
        Check(sh::Build(circle, sh::Parts{true, true}, &ground, mesh, &why) && mesh.triangles.size() < 400,
              "on a map without terrain it lies flat");
        ground.present = true;

        // A segment laid on the ground, cut where it crosses the terrain's triangles.
        std::vector<sh::Mesh::Segment> segs;
        const sh::V3 a = ChunkMiddle(510, 510), b = ChunkMiddle(511, 511);
        sh::DrapeSegment(a, b, &ground, segs, true, true);
        double worst = 0.0;
        for (const sh::Mesh::Segment& s : segs)
            for (double t : {0.0, 0.25, 0.5, 0.75, 1.0})
            {
                float h = 0.0f;
                const double x = s.a.x + (s.b.x - s.a.x) * t, y = s.a.y + (s.b.y - s.a.y) * t;
                sh::HeightAt(ground, x, y, h);
                worst = std::max(worst, std::fabs(s.a.z + (s.b.z - s.a.z) * t - h));
            }
        std::printf("  a diagonal across a chunk: %zu pieces, worst gap %.5f yd\n", segs.size(), worst);
        Check(segs.size() > 16 && worst < 2e-3, "a line laid on the ground follows each triangle it crosses");
        Check(segs.front().cap_a && segs.back().cap_b && !segs.front().cap_b, "its ends are capped, its cuts are not");
    }

    void ShapeCases()
    {
        TestGround ground;
        const sh::V3 c = ChunkMiddle(511, 511);
        sh::Mesh    mesh;
        std::string why;
        sh::Desc    d;
        d.center = c;
        d.flags  = sh::kFlat;

        d.kind   = sh::kSector;
        d.radius = 10.0f;
        d.angle  = static_cast<float>(kPi / 2.0);
        d.facing = 0.3f;
        Check(sh::Build(d, sh::Parts{true, true}, nullptr, mesh, &why), "a cone is built");
        Check(std::fabs(Area(mesh.triangles) - 0.25 * kPi * 100.0) / (0.25 * kPi * 100.0) < 0.01, "a quarter cone covers a quarter disc");
        d.angle = static_cast<float>(1.5 * kPi);
        Check(sh::Build(d, sh::Parts{true, true}, nullptr, mesh, &why) &&
                  std::fabs(Area(mesh.triangles) - 0.75 * kPi * 100.0) / (0.75 * kPi * 100.0) < 0.01,
              "a cone wider than a half turn (pieces of a quarter at most) covers its area");
        d.inner = 4.0f;
        Check(sh::Build(d, sh::Parts{true, true}, nullptr, mesh, &why) &&
                  std::fabs(Area(mesh.triangles) - 0.75 * kPi * 84.0) / (0.75 * kPi * 84.0) < 0.01,
              "an arc of a ring covers its band");
        d.kind = sh::kRing;
        Check(sh::Build(d, sh::Parts{true, true}, &ground, mesh, &why) &&
                  std::fabs(Area(mesh.triangles) - kPi * 84.0) / (kPi * 84.0) < 0.01,
              "a ring on the ground covers its band");

        d.kind   = sh::kPolygon;
        d.flags  = 0;
        d.points = {{0, 0, 0}, {8, 0, 0}, {8, 8, 0}, {4, 3, 0}, {0, 8, 0}};
        Check(sh::Build(d, sh::Parts{true, true}, &ground, mesh, &why), "a concave polygon on the ground is built");
        Check(std::fabs(Area(mesh.triangles) - 44.0) < 0.05 && WorstGap(ground, mesh.triangles, c.z) < 2e-3,
              "it covers its area, on the ground");
        d.points = {{0, 0, 0}, {8, 8, 0}, {8, 0, 0}, {0, 8, 0}};
        Check(!sh::Build(d, sh::Parts{false, true}, &ground, mesh, &why) && why.find("crosses") != std::string::npos,
              "a polygon crossing itself is refused, saying so");

        d = sh::Desc{};
        d.kind    = sh::kGrid;
        d.center  = c;
        d.size    = sh::V3{12.0f, 8.0f, 0.0f};
        d.spacing = 1.0f;
        Check(sh::Build(d, sh::Parts{true, false}, &ground, mesh, &why), "a grid on the ground is built");
        sh::Mesh flatGrid;
        d.flags = sh::kFlat;
        sh::Build(d, sh::Parts{true, false}, nullptr, flatGrid, &why);
        Check(flatGrid.lines.size() == 4 + 11 + 7, "a 12 x 8 grid every yard: its border and 11 + 7 lines inside");
        d.spacing = 0.01f;
        Check(!sh::Build(d, sh::Parts{true, false}, nullptr, mesh, &why) && why.find("lines") != std::string::npos,
              "a grid of too many lines is refused, saying how many");

        d = sh::Desc{};
        d.kind   = sh::kBox;
        d.center = c;
        d.size   = sh::V3{2, 4, 6};
        d.facing = 0.7f;
        d.pitch  = 0.2f;
        Check(sh::Build(d, sh::Parts{true, true}, nullptr, mesh, &why) && mesh.lines.size() == 12 && mesh.triangles.size() == 36 &&
                  mesh.normals.size() == 12,
              "a turned box: 12 edges, 12 triangles with their normals");
        bool outward = true;
        for (size_t f = 0; f < mesh.normals.size(); ++f)
        {
            const sh::V3& a = mesh.triangles[3 * f];
            outward = outward && (mesh.normals[f].x * (a.x - c.x) + mesh.normals[f].y * (a.y - c.y) + mesh.normals[f].z * (a.z - c.z)) > 0.0f;
        }
        Check(outward, "its normals point outwards");
        d.kind   = sh::kSphere;
        d.radius = 3.0f;
        Check(sh::Build(d, sh::Parts{true, true}, nullptr, mesh, &why) && !mesh.ground, "a sphere is built, off the ground");
        double worst = 0.0;
        for (const sh::V3& v : mesh.triangles)
            worst = std::max(worst, std::fabs(std::sqrt((v.x - c.x) * (v.x - c.x) + (v.y - c.y) * (v.y - c.y) + (v.z - c.z) * (v.z - c.z)) - 3.0));
        Check(worst < 1e-3, "on its radius");
        d.kind   = sh::kCylinder;
        d.size.z = 5.0f;
        Check(sh::Build(d, sh::Parts{true, true}, nullptr, mesh, &why) && !mesh.triangles.empty(), "a cylinder is built");

        // Refusals, each naming what is wrong.
        sh::Desc bad;
        bad.kind   = sh::kCircle;
        Check(sh::Validate(bad).find("radius") != std::string::npos, "a circle without a radius is refused");
        bad.radius = 400.0f;
        Check(sh::Validate(bad).find("250") != std::string::npos, "a circle past the extent is refused, saying the limit");
        bad.kind = sh::kLine;
        Check(sh::Validate(bad).find("2 points") != std::string::npos, "a line of no points is refused");
        bad.kind = 42;
        Check(!sh::Validate(bad).empty(), "a kind that is none is refused");
        bad      = sh::Desc{};
        bad.kind = sh::kCircle;
        bad.radius = std::nanf("");
        Check(sh::Validate(bad).find("finite") != std::string::npos, "a number not finite is refused");
    }

    /// A view putting pixels on the clip space: x' = 2x/w - 1, y' = 1 - 2y/h, z as given.
    sh::View PixelView(uint32_t w, uint32_t h)
    {
        sh::View v;
        std::memset(v.vp, 0, sizeof v.vp);
        v.vp[0]  = 2.0f / static_cast<float>(w);
        v.vp[5]  = -2.0f / static_cast<float>(h);
        v.vp[10] = 1.0f;
        v.vp[12] = -1.0f;
        v.vp[13] = 1.0f;
        v.vp[15] = 1.0f;
        v.viewport[2] = static_cast<float>(w);
        v.viewport[3] = static_cast<float>(h);
        v.viewport[5] = 1.0f;
        v.scale = 1.0f;
        return v;
    }

    /// What building costs: an immediate grid under the aim each frame, a telegraph, a volume.
    void CostCases()
    {
        TestGround ground;
        const sh::V3 c = ChunkMiddle(511, 511);
        auto time = [&](const char* what, const sh::Desc& d, sh::Parts parts) {
            sh::Mesh    mesh;
            std::string why;
            const int   rounds = 200;
            const auto  from   = std::chrono::steady_clock::now();
            for (int i = 0; i < rounds; ++i) sh::Build(d, parts, &ground, mesh, &why);
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - from).count() / rounds;
            std::vector<sh::Vertex> stream;
            const auto              w0 = std::chrono::steady_clock::now();
            for (int i = 0; i < rounds; ++i)
            {
                stream.clear();
                for (const sh::Mesh::Segment& seg : mesh.lines) sh::PushSegment(stream, seg, c, 0xFFFFFFFFu, 16, true);
                for (size_t t = 0; t + 2 < mesh.triangles.size(); t += 3)
                    sh::PushTriangle(stream, mesh.triangles[t], mesh.triangles[t + 1], mesh.triangles[t + 2], c, 0x80FFFFFFu, true);
            }
            const double ws = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - w0).count() / rounds;
            std::printf("  cost: %s: %zu vertices, built in %.1f us, written in %.1f us\n", what, mesh.Vertices(), us, ws);
        };
        sh::Desc grid;
        grid.kind    = sh::kGrid;
        grid.center  = c;
        grid.size    = sh::V3{32.0f, 32.0f, 0.0f};
        grid.spacing = 1.0f;
        time("a 32 x 32 yd grid every yard on the ground", grid, sh::Parts{true, false});
        grid.size    = sh::V3{16.0f, 16.0f, 0.0f};
        grid.spacing = 4.1666665f;
        time("a 16 x 16 yd grid on the terrain's cells", grid, sh::Parts{true, true});
        sh::Desc cone;
        cone.kind   = sh::kSector;
        cone.center = c;
        cone.radius = 20.0f;
        cone.angle  = 1.2f;
        time("a 20 yd cone, filled and outlined, on the ground", cone, sh::Parts{true, true});
        sh::Desc circle = cone;
        circle.kind     = sh::kCircle;
        circle.radius   = 8.0f;
        time("an 8 yd circle, filled and outlined, on the ground", circle, sh::Parts{true, true});
        sh::Desc ball = circle;
        ball.kind     = sh::kSphere;
        ball.radius   = 3.0f;
        time("a 3 yd sphere, wireframe and fill", ball, sh::Parts{true, true});
    }

    void ExpandCases()
    {
        const sh::View view = PixelView(64, 48);
        float          k[24];
        sh::Constants(view, k);
        std::vector<sh::Vertex> v;
        sh::PushSegment(v, sh::Mesh::Segment{{10, 20, 0.5f}, {50, 20, 0.5f}, 0, 0}, sh::V3{}, 0xFFFFFFFFu, sh::WidthCode(4.0f), false);
        Check(v.size() == 6, "a segment is six vertices");
        float c0[4], c2[4];
        sh::Project(k, v[0], c0); // first end, left
        sh::Project(k, v[2], c2); // second end, right
        const float y0 = (1.0f - c0[1] / c0[3]) * 24.0f, y2 = (1.0f - c2[1] / c2[3]) * 24.0f;
        const float x0 = (c0[0] / c0[3] + 1.0f) * 32.0f;
        Check(std::fabs(std::fabs(y0 - y2) - 4.0f) < 1e-3f, "a 4-pixel line's sides are 4 pixels apart on the screen");
        Check(std::fabs(x0 - 10.0f) < 1e-3f, "without a cap, it starts at its end");
        sh::Mesh::Segment capped{{10, 20, 0.5f}, {50, 20, 0.5f}, 1, 0};
        v.clear();
        sh::PushSegment(v, capped, sh::V3{}, 0xFFFFFFFFu, sh::WidthCode(4.0f), false);
        sh::Project(k, v[0], c0);
        Check(std::fabs((c0[0] / c0[3] + 1.0f) * 32.0f - 8.0f) < 1e-3f, "with a cap, it starts half its width before its end");

        // An end behind the near plane: the segment cut there.
        sh::View persp;
        // Looking down +x: clip = (y, z, (x - 1) * 1.01, x), D3D's depth with the near plane at 1.
        std::memset(persp.vp, 0, sizeof persp.vp);
        persp.vp[0 * 4 + 2] = 1.01f; // x -> z
        persp.vp[0 * 4 + 3] = 1.0f;  // x -> w
        persp.vp[1 * 4 + 0] = 1.0f;  // y -> clip x
        persp.vp[2 * 4 + 1] = 1.0f;  // z -> clip y
        persp.vp[3 * 4 + 2] = -1.01f;
        persp.viewport[2] = 64.0f;
        persp.viewport[3] = 48.0f;
        persp.scale       = 1.0f;
        sh::Constants(persp, k);
        v.clear();
        sh::PushSegment(v, sh::Mesh::Segment{{-5, 0, 0}, {10, 0, 0}, 0, 0}, sh::V3{}, 0xFFFFFFFFu, sh::WidthCode(2.0f), false);
        sh::Project(k, v[0], c0);
        Check(c0[2] >= -1e-4f && c0[3] > 0.0f, "an end behind the eye is moved onto the near plane");
        v.clear();
        sh::PushTriangle(v, {3, 0, 0}, {3, 1, 0}, {3, 0, 1}, sh::V3{}, 0xFF00FF00u, true);
        sh::Project(k, v[0], c0);
        Check(c0[3] < 3.0f && c0[3] > 2.9f, "a ground vertex is pulled towards the eye a little");
        Check(std::fabs(c0[0] / c0[3]) < 1e-6f, "and stays where it was on the screen");
    }

    // --- On the driver ---------------------------------------------------------------------------------

    struct Setup
    {
        Live           live;
        FakeEngine     fake;
        draw::Pool*    pool = nullptr;
        uint32_t       at = 0;
        render::Render render;

        bool Make(uint32_t w, uint32_t h)
        {
            char why[256] = {};
            if (!live.Make(w, h, why, sizeof why)) return false;
            fake.Reset(w, h);
            pool          = live.renderer.CreatePool(draw::kVertexPool, draw::kStream, 1u << 16);
            fake.pools[0] = pool;
            return render.Create(live.context, live.allocator, live.timeline, live.display, live.renderer);
        }

        bool Begin()
        {
            if (!live.Begin(Argb(0, 0, 0, 0))) return false;
            render.FrameBegun();
            return true;
        }

        /// An engine quad over [x0, x1) x [y0, y1) at depth z: what stands in front.
        bool Wall(float x0, float y0, float x1, float y1, float z)
        {
            Vertex quad[6];
            rig::Quad(quad, x0, y0, x1, y1, Argb(0, 0, 255), 0, 0, 1, 1);
            for (Vertex& v : quad) v.z = z;
            Stream(live, fake, pool, quad, 6, at);
            fake.State(wxl::vk::gxfront::kDepthTest, 1);
            fake.State(wxl::vk::gxfront::kDepthWrite, 1);
            const bool drawn = Record(live, fake, 6);
            fake.State(wxl::vk::gxfront::kDepthTest, 0);
            fake.State(wxl::vk::gxfront::kDepthWrite, 0);
            return drawn;
        }

        ~Setup()
        {
            if (live.timeline.Ready() && live.timeline.Last()) live.timeline.WaitFor(live.timeline.Last());
            render.Destroy();
            if (pool) live.renderer.DestroyPool(pool);
        }
    };

    uint32_t R(uint32_t argb) { return (argb >> 16) & 0xFF; }
    uint32_t G(uint32_t argb) { return (argb >> 8) & 0xFF; }
    uint32_t B(uint32_t argb) { return argb & 0xFF; }

    void LiveDraw(Setup& s)
    {
        const uint32_t W = 64, H = 48;
        const sh::View view = PixelView(W, H);
        float          k[24];
        sh::Constants(view, k);
        render::Shapes shapes;
        shapes.Attach(s.render);

        // A wall in front of the left half (z 0.2); a red line at z 0.5 across the picture, tested; a
        // green line on top; a half-transparent white triangle in the bottom right.
        std::vector<sh::Vertex> v;
        sh::PushTriangle(v, {40, 34, 0.5f}, {60, 34, 0.5f}, {60, 46, 0.5f}, sh::V3{}, 0x80FFFFFFu, false);
        sh::PushSegment(v, sh::Mesh::Segment{{2, 10, 0.5f}, {62, 10, 0.5f}, 0, 0}, sh::V3{}, 0xFFFF0000u, sh::WidthCode(4.0f), false);
        const uint32_t tested = static_cast<uint32_t>(v.size());
        sh::PushSegment(v, sh::Mesh::Segment{{2, 24, 0.5f}, {62, 24, 0.5f}, 0, 0}, sh::V3{}, 0xFF00FF00u, sh::WidthCode(2.0f), false);
        const uint32_t top = static_cast<uint32_t>(v.size()) - tested;

        Check(s.Begin(), "a frame begins");
        Check(s.Wall(0, 0, 32, 48, 0.2f), "a wall stands over the left half");
        std::string why;
        const uint32_t drawn = shapes.Draw(v.data(), tested, top, k, view.viewport, &why);
        if (!drawn) std::printf("  refused: %s\n", why.c_str());
        Check(drawn == tested + top, "every vertex is drawn");
        const std::vector<uint32_t> px = s.live.End();
        Check(px.size() == W * H, "the scene is read back");
        if (px.size() != W * H) return;
        auto at = [&](int x, int y) { return px[static_cast<size_t>(y) * W + x]; };
        // The red line covers rows 8 to 11 (centre 10, 4 pixels), right of the wall.
        int rows = 0;
        for (int y = 0; y < static_cast<int>(H); ++y) rows += R(at(48, y)) > 200 && G(at(48, y)) < 50 ? 1 : 0;
        Check(rows == 4, "a 4-pixel line covers 4 rows");
        if (!(R(at(16, 9)) < 50 && B(at(16, 9)) > 200)) std::printf("  behind the wall: %08X, beside it %08X\n", at(16, 9), at(48, 9));
        Check(R(at(16, 9)) < 50 && B(at(16, 9)) > 200, "behind the wall the tested line is hidden");
        Check(G(at(16, 24)) > 200 && G(at(48, 24)) > 200, "the line on top shows over the wall and beside it");
        int greenRows = 0;
        for (int y = 0; y < static_cast<int>(H); ++y) greenRows += G(at(48, y)) > 200 && R(at(48, y)) < 50 ? 1 : 0;
        Check(greenRows == 2, "a 2-pixel line covers 2 rows");
        const uint32_t half = at(56, 42);
        Check(R(half) >= 126 && R(half) <= 130 && G(half) >= 126 && G(half) <= 130, "a white triangle at alpha 128 over black is grey 128");
        Check((at(56, 42) >> 24) == (at(56, 2) >> 24), "the scene's alpha is left as it was under the shapes");

        // The CPU's copy of the program puts the triangle's corner where the driver drew it.
        float c[4];
        sh::Project(k, v[1], c);
        // Direct3D 9 puts pixel centres on whole coordinates: the pixel inside the corner.
        const int cx = static_cast<int>((c[0] / c[3] + 1.0f) * 32.0f) - 1, cy = static_cast<int>((1.0f - c[1] / c[3]) * 24.0f) + 1;
        Check(R(at(cx, cy)) > 100, "the driver drew the triangle's corner where Project puts it");
        shapes.Detach();
    }

    void ProgramCases()
    {
#if defined(WXL_VK_SPIRV_VAL)
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        for (const render::OwnProgram& p : render::ShapePrograms())
        {
            char what[96];
            std::snprintf(what, sizeof what, "spirv-val passes %s", p.name);
            Check(tools.Validate(p.code, p.words), what);
        }
#endif
    }
}

namespace wxl::vk::test
{
    void ShapesPureCases()
    {
        std::printf("wxl.draw's geometry\n");
        TriangulateCases();
        DrapeCases();
        ShapeCases();
        ExpandCases();
        CostCases();
    }

    bool ShapesLiveCases()
    {
        std::printf("wxl.draw on the driver\n");
        ProgramCases();
        Setup s;
        if (!s.Make(64, 48)) return false;
        LiveDraw(s);
        return true;
    }
}
