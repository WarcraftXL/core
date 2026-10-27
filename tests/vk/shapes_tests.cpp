// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's geometry (src/gx/shapes/) and its Vulkan passes (src/gx/vk/render/Shapes.cpp): ground
// shapes laid on a made-up terrain exactly (every vertex and every edge's middle at the height of
// the ground under it), sectors, rings and polygons of the right area, polygons cut into triangles,
// lines widened on the screen as the vertex programs widen them and cut at the near plane; then on
// the driver: a triangle and a line drawn into the scene against its depth and on top, their
// widths, colours and blending, held to the CPU's copy of the vertex program. Since abi 0.1.38:
// surfaces' picture frames (fit, tile, world), feathers and fades, a polygon's inside cut finer at
// its border, lines' opacities along them, soft edges and shadows moved on the screen; on the
// driver a textured ring, a modulated stain and a feathered disc held pixel by pixel to the CPU's
// copy of the pixel programs (Shade), and a soft line over its shadow.

#include "gx/shapes/Geometry.hpp"
#include "gx/shapes/Particles.hpp"
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

    // --- Surfaces, soft lines, shadows -------------------------------------------------------------------

    bool Near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

    void SurfaceCases()
    {
        // FIT over a rectangle 8 long (along facing) by 4 wide: its top towards the facing, its
        // right on the shape's right.
        sh::Desc rect;
        rect.kind   = sh::kRectangle;
        rect.flags  = sh::kFlat;
        rect.center = sh::V3{100.0f, 200.0f, 0.0f};
        rect.size   = sh::V3{8.0f, 4.0f, 0.0f};
        rect.facing = 0.0f;
        sh::Mesh mesh;
        Check(sh::Build(rect, sh::Parts{false, true}, nullptr, mesh), "a flat rectangle is built");
        sh::SurfaceLook look;
        look.textured = true;
        sh::SurfaceConstants k = sh::MakeSurface(mesh, look);
        float uv[2];
        sh::Picture(k, 4.0f, 2.0f, uv); // the front left corner (+x ahead, +y to the left)
        Check(Near(uv[0], 0.0f) && Near(uv[1], 0.0f), "FIT: the corner ahead and to the left is the picture's top left");
        sh::Picture(k, -4.0f, -2.0f, uv);
        Check(Near(uv[0], 1.0f) && Near(uv[1], 1.0f), "FIT: the corner behind and to the right is its bottom right");
        sh::Picture(k, 0.0f, 0.0f, uv);
        Check(Near(uv[0], 0.5f) && Near(uv[1], 0.5f), "FIT: the centre is the picture's middle");
        look.right = 0.5f;
        look.bottom = 0.25f;
        k = sh::MakeSurface(mesh, look);
        sh::Picture(k, -4.0f, -2.0f, uv);
        Check(Near(uv[0], 0.5f) && Near(uv[1], 0.25f), "a picture filling part of its texture (wxl.texture's) is read there only");
        // A quarter turn counter-clockwise seen from above: the picture's top goes to the left.
        look = sh::SurfaceLook{};
        look.textured = true;
        look.rotation = static_cast<float>(kPi / 2);
        sh::Desc square = rect;
        square.size     = sh::V3{4.0f, 4.0f, 0.0f};
        sh::Build(square, sh::Parts{false, true}, nullptr, mesh);
        k = sh::MakeSurface(mesh, look);
        sh::Picture(k, 0.0f, 1.9f, uv); // towards the left edge's middle
        Check(Near(uv[0], 0.5f, 1e-3f) && uv[1] < 0.03f, "a turned picture: its top now on the shape's left");
        // TILE at half the bounds repeats twice; the sampler repeats it.
        look          = sh::SurfaceLook{};
        look.textured = true;
        look.uv       = sh::kUvTile;
        look.size_x = look.size_y = 0.5f;
        k = sh::MakeSurface(mesh, look);
        float a[2], b[2];
        sh::Picture(k, 0.0f, 1.0f, a);
        sh::Picture(k, 0.0f, -1.0f, b);
        Check(Near(a[0], b[0], 1e-4f) && Near(a[1], b[1], 1e-4f) && sh::Repeats(look.uv), "TILE: a picture repeats every half of the bounds");
        // WORLD: two shapes side by side read the same picture at the same world point, whatever
        // their facings; one picture is size yards, its top to the north.
        look    = sh::SurfaceLook{};
        look.textured = true;
        look.uv = sh::kUvWorld;
        look.size_x = look.size_y = 4.0f;
        sh::Desc one = rect, two = rect;
        two.center   = sh::V3{106.0f, 203.0f, 0.0f};
        two.facing   = 1.1f;
        sh::Mesh m1, m2;
        sh::Build(one, sh::Parts{false, true}, nullptr, m1);
        sh::Build(two, sh::Parts{false, true}, nullptr, m2);
        const sh::SurfaceConstants k1 = sh::MakeSurface(m1, look), k2 = sh::MakeSurface(m2, look);
        const sh::V3 world{103.3f, 201.7f, 0.0f};
        const sh::V3 p1 = sh::InPlane(m1, world), p2 = sh::InPlane(m2, world);
        sh::Picture(k1, p1.x, p1.y, a);
        sh::Picture(k2, p2.x, p2.y, b);
        Check(Near(a[0], b[0], 1e-3f) && Near(a[1], b[1], 1e-3f), "WORLD: two shapes join, whatever their facing");
        const sh::V3 north = sh::InPlane(m1, sh::V3{world.x + 1.0f, world.y, 0.0f});
        sh::Picture(k1, north.x, north.y, b);
        Check(Near(b[0], a[0], 1e-3f) && Near(std::fmod(a[1] - b[1] + 1.0f, 1.0f), 0.25f, 1e-3f), "WORLD: a yard north is a quarter picture up");

        // The feather and the fade, on a circle of 10 yards.
        sh::Desc disc;
        disc.kind   = sh::kCircle;
        disc.flags  = sh::kFlat;
        disc.center = sh::V3{0.0f, 0.0f, 0.0f};
        disc.radius = 10.0f;
        sh::Build(disc, sh::Parts{false, true}, nullptr, mesh);
        look         = sh::SurfaceLook{};
        look.feather = 2.0f;
        k            = sh::MakeSurface(mesh, look);
        const float white[4] = {1, 1, 1, 1}, none[4] = {1, 1, 1, 1};
        float       out[4];
        sh::Shade(k, sh::V3{0.0f, 7.9f, 0.0f}, white, none, out);
        Check(Near(out[3], 1.0f), "feathered: full inside the feather");
        sh::Shade(k, sh::V3{9.0f, 0.0f, 0.0f}, white, none, out);
        Check(Near(out[3], 0.5f), "feathered: half way through it, half seen");
        sh::Shade(k, sh::V3{0.0f, -10.0f, 0.0f}, white, none, out);
        Check(Near(out[3], 0.0f), "feathered: nothing at the border");
        look.feather = 0.0f;
        look.radial  = 1.0f;
        k            = sh::MakeSurface(mesh, look);
        sh::Shade(k, sh::V3{0.0f, 0.0f, 0.0f}, white, none, out);
        const float centre = out[3];
        sh::Shade(k, sh::V3{5.0f, 0.0f, 0.0f}, white, none, out);
        const float half = out[3];
        sh::Shade(k, sh::V3{0.0f, 9.99f, 0.0f}, white, none, out);
        Check(Near(centre, 1.0f) && Near(half, 0.5f) && out[3] < 0.01f, "a radial fade from the centre out to the border");
        // A ring's feather on both borders; a sector's on its straight sides.
        sh::Desc ring = disc;
        ring.kind     = sh::kRing;
        ring.inner    = 4.0f;
        sh::Build(ring, sh::Parts{false, true}, nullptr, mesh);
        look = sh::SurfaceLook{};
        look.feather = 1.0f;
        k = sh::MakeSurface(mesh, look);
        sh::Shade(k, sh::V3{4.5f, 0.0f, 0.0f}, white, none, out);
        Check(Near(out[3], 0.5f), "a ring fades at its hole too");
        sh::Desc cone = disc;
        cone.kind     = sh::kSector;
        cone.angle    = static_cast<float>(kPi / 2);
        sh::Build(cone, sh::Parts{false, true}, nullptr, mesh);
        k = sh::MakeSurface(mesh, look);
        // On the side at 45 degrees, half a yard in.
        const float c45 = std::cos(static_cast<float>(kPi / 4)), s45 = std::sin(static_cast<float>(kPi / 4));
        sh::Shade(k, sh::V3{5.0f * c45 + 0.5f * s45, 5.0f * s45 - 0.5f * c45, 0.0f}, white, none, out);
        Check(Near(out[3], 0.5f, 1e-3f), "a sector fades along its straight sides");
        // MODULATE gives 1 + (colour - 1) x opacity: white leaves the ground as it is.
        look = sh::SurfaceLook{};
        look.blend = sh::kBlendModulate;
        k = sh::MakeSurface(mesh, look);
        const float grey[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        sh::Shade(k, sh::V3{2.0f, 0.0f, 0.0f}, grey, none, out);
        Check(Near(out[0], 0.75f) && Near(out[3], 0.5f), "MODULATE: half of a half-grey darkens to three quarters");

        // A polygon's inside cut finer near its border, each corner's distance to it kept; even laid
        // flat, where it is a few large triangles.
        sh::Desc poly;
        poly.kind   = sh::kPolygon;
        poly.flags  = sh::kFlat;
        poly.center = sh::V3{0.0f, 0.0f, 0.0f};
        poly.points = {{-6, -4, 0}, {6, -4, 0}, {6, 4, 0}, {0, 1, 0}, {-6, 4, 0}};
        sh::Build(poly, sh::Parts{false, true, 1.0f}, nullptr, mesh);
        bool   kept = mesh.edge.size() == mesh.triangles.size() && !mesh.triangles.empty(), inside = false, exact = true;
        double area = 0.0;
        for (size_t i = 0; kept && i + 2 < mesh.triangles.size(); i += 3)
        {
            const sh::V3 &p = mesh.triangles[i], &q = mesh.triangles[i + 1], &r = mesh.triangles[i + 2];
            area += 0.5 * std::fabs((q.x - p.x) * (r.y - p.y) - (r.x - p.x) * (q.y - p.y));
            for (int c = 0; c < 3; ++c) inside = inside || mesh.edge[i + c] >= 1.0f;
            // The corner's distance: to the nearest of the polygon's edges.
            const sh::V3 w = mesh.triangles[i];
            double       best = 1e9;
            for (size_t e = 0; e < poly.points.size(); ++e)
            {
                const sh::V3 a0 = poly.points[e], b0 = poly.points[(e + 1) % poly.points.size()];
                const double dx = b0.x - a0.x, dy = b0.y - a0.y;
                const double t  = std::clamp(((w.x - a0.x) * dx + (w.y - a0.y) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
                best            = std::min(best, std::hypot(a0.x + dx * t - w.x, a0.y + dy * t - w.y));
            }
            exact = exact && std::fabs(best - mesh.edge[i]) < 1e-3;
        }
        const double expected = 12.0 * 8.0 - 0.5 * 12.0 * 3.0;
        Check(kept && inside && exact && std::fabs(area - expected) < 1e-3, "a feathered polygon: cut finer, its corners' distances to its border kept, its area whole");
        look = sh::SurfaceLook{};
        look.feather = 1.0f;
        k = sh::MakeSurface(mesh, look);
        Check(k.k[3][0] == 2.0f, "a polygon's feather reads the distances its corners carry");

        // A line's opacity along it, and point by point; laid on the ground, cut where it crosses the
        // terrain's edges, each piece's opacities in between.
        TestGround   ground;
        const sh::V3 c = ChunkMiddle(511, 511);
        sh::Desc     line;
        line.kind   = sh::kLine;
        line.flags  = sh::kOnGround;
        line.center = c;
        line.points = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}};
        line.alphas = {1.0f, 0.0f, 1.0f};
        Check(sh::Build(line, sh::Parts{}, &ground, mesh), "a line with an opacity per point is built");
        bool  monotone = true, ends = false;
        float last     = 2.0f;
        for (const sh::Mesh::Segment& seg : mesh.lines)
        {
            if (seg.t_b <= 0.5f + 1e-4f)
            {
                monotone = monotone && seg.alpha_a <= last + 1e-5f && seg.alpha_b <= seg.alpha_a + 1e-5f;
                last     = seg.alpha_b;
            }
            ends = ends || (seg.t_a < 1e-6f && Near(seg.alpha_a, 1.0f));
        }
        Check(mesh.lines.size() > 2 && monotone && ends && Near(mesh.lines.back().t_b, 1.0f) && Near(mesh.lines.back().alpha_b, 1.0f),
              "the opacities and the way along each piece go from the first point to the last");
        sh::LineLook lineLook;
        lineLook.color       = 0xFFFFFFFFu;
        lineLook.alpha_start = 0.0f;
        lineLook.alpha_end   = 1.0f;
        std::vector<sh::Vertex> v;
        sh::Mesh::Segment       midway{{0, 0, 0}, {1, 0, 0}, 0, 0, 1.0f, 1.0f, 0.0f, 0.5f};
        sh::PushSegment(v, midway, sh::V3{}, lineLook, false);
        Check((v[0].color >> 24) == 0 && (v[1].color >> 24) == 128, "from transparent at the start to half way at the middle");
        line.alphas = {1.0f, 2.0f, 1.0f};
        Check(!sh::Validate(line).empty(), "an opacity past 1 is refused");
        line.alphas = {1.0f};
        Check(!sh::Validate(line).empty(), "as many opacities as points, or none");

        // A shadow: moved by its pixels on the screen; a soft line: wider by half its softness.
        const sh::View view = PixelView(64, 48);
        float          kv[24];
        sh::Constants(view, kv);
        v.clear();
        sh::LineLook moved;
        moved.width = sh::WidthCode(4.0f);
        moved.right = sh::OffsetCode(3.0f);
        moved.down  = sh::OffsetCode(-1.5f);
        moved.soft  = sh::SoftCode(2.0f);
        sh::PushSegment(v, sh::Mesh::Segment{{10, 20, 0.5f}, {50, 20, 0.5f}, 0, 0}, sh::V3{}, moved, false);
        float c0[4], c3[4];
        sh::Project(kv, v[0], c0); // first end, left
        sh::Project(kv, v[5], c3); // first end, right
        const float x0 = (c0[0] / c0[3] + 1.0f) * 32.0f, y0 = (1.0f - c0[1] / c0[3]) * 24.0f, y3 = (1.0f - c3[1] / c3[3]) * 24.0f;
        Check(Near(x0, 13.0f, 1e-3f), "a shadow moves 3 pixels to the right");
        Check(Near(0.5f * (y0 + y3), 18.5f, 1e-3f), "and 1.5 up for -1.5 down");
        Check(Near(std::fabs(y0 - y3), 6.0f, 1e-3f), "a 4-pixel line 2 pixels soft is drawn 6 wide, half seen at 4");
        Check(Near(sh::LineCoverage(2.0f, 3.0f, 2.0f), 0.5f) && Near(sh::LineCoverage(0.5f, 3.0f, 2.0f), 1.0f) &&
                  Near(sh::LineCoverage(3.0f, 3.0f, 2.0f), 0.0f),
              "its edges fade over its softness");
        Check(sh::OffsetCode(40.0f) == 30 && sh::OffsetCode(-40.0f) == -30 && sh::SoftCode(99.0f) == 60, "offsets and softness are bounded");
        // Since abi 0.1.39: volumes' faces, each in its own frame.
        sh::Desc box;
        box.kind   = sh::kBox;
        box.center = sh::V3{10.0f, 20.0f, 30.0f};
        box.size   = sh::V3{4.0f, 2.0f, 6.0f};
        box.facing = 0.7f;
        box.pitch  = 0.2f;
        sh::Mesh boxMesh;
        Check(sh::Build(box, sh::Parts{true, true}, nullptr, boxMesh) && boxMesh.faces.size() == 6 &&
                  boxMesh.face_plane.size() == boxMesh.triangles.size(),
              "a box's inside is six faces, every corner in its face's frame");
        bool onFace = true, frames = true;
        for (const sh::Mesh::Face& face : boxMesh.faces)
        {
            const float dot = face.up.x * face.left.x + face.up.y * face.left.y + face.up.z * face.left.z;
            frames = frames && std::fabs(dot) < 1e-5f && face.count == 6;
            for (uint32_t t = face.first; t < face.first + face.count; ++t)
            {
                const sh::V3 p = boxMesh.face_plane[t];
                onFace = onFace && std::fabs(std::fabs(p.x) - face.plane.hx) < 1e-3f && std::fabs(std::fabs(p.y) - face.plane.hy) < 1e-3f;
            }
        }
        Check(frames && onFace, "each face's corners at its half sides, its axes square");
        const sh::Mesh::Face& side = boxMesh.faces[5]; // +x, the facing's side
        Check(std::fabs(side.plane.hx - 3.0f) < 1e-5f && std::fabs(side.plane.hy - 1.0f) < 1e-5f, "a side face's picture stands up the box");
        sh::SurfaceLook faceLook;
        faceLook.textured = true;
        sh::SurfaceConstants fk = sh::MakeFaceSurface(side, faceLook);
        sh::Picture(fk, 3.0f, 1.0f, a);
        Check(Near(a[0], 0.0f) && Near(a[1], 0.0f), "FIT on a face: its top left corner is the picture's");
        // WORLD on two boxes side by side, turned alike: their facing sides join.
        faceLook.uv     = sh::kUvWorld;
        faceLook.size_x = faceLook.size_y = 1.5f;
        sh::Desc other  = box;
        const float cf  = std::cos(box.facing), sf = std::sin(box.facing);
        other.pitch     = 0.0f;
        box.pitch       = 0.0f;
        other.center    = sh::V3{box.center.x + 2.0f * sf, box.center.y - 2.0f * cf, box.center.z}; // 2 yd to the box's right, where its face's left points
        sh::Mesh m1b, m2b;
        sh::Build(box, sh::Parts{false, true}, nullptr, m1b);
        sh::Build(other, sh::Parts{false, true}, nullptr, m2b);
        const sh::Mesh::Face& f1 = m1b.faces[5];
        const sh::Mesh::Face& f2 = m2b.faces[5];
        const sh::SurfaceConstants w1 = sh::MakeFaceSurface(f1, faceLook), w2 = sh::MakeFaceSurface(f2, faceLook);
        // The world point on both faces' plane, 1 yd up, at the boxes' shared edge.
        const sh::V3 edgeWorld{f1.origin.x + f1.left.x * 1.0f + f1.up.x, f1.origin.y + f1.left.y * 1.0f + f1.up.y, f1.origin.z + f1.up.z};
        const auto in = [](const sh::Mesh::Face& f, sh::V3 w) {
            const sh::V3 r{w.x - f.origin.x, w.y - f.origin.y, w.z - f.origin.z};
            return sh::V3{r.x * f.up.x + r.y * f.up.y + r.z * f.up.z, r.x * f.left.x + r.y * f.left.y + r.z * f.left.z, 0.0f};
        };
        const sh::V3 e1 = in(f1, edgeWorld), e2 = in(f2, edgeWorld);
        sh::Picture(w1, e1.x, e1.y, a);
        sh::Picture(w2, e2.x, e2.y, b);
        Check(Near(a[0], b[0], 1e-3f) && Near(a[1], b[1], 1e-3f) && std::fabs(e2.y + 1.0f) < 1e-3f, "WORLD on faces: two boxes side by side join");
        // A cylinder: its side once around, the seam behind it; its ends discs.
        sh::Desc can;
        can.kind   = sh::kCylinder;
        can.center = sh::V3{0, 0, 0};
        can.radius = 2.0f;
        can.size.z = 3.0f;
        sh::Mesh canMesh;
        sh::Build(can, sh::Parts{false, true}, nullptr, canMesh);
        bool seamless = canMesh.faces.size() == 3 && canMesh.faces[0].plane.code == 1 && canMesh.faces[1].plane.code == 0;
        const sh::Mesh::Face& wall = canMesh.faces[0];
        for (uint32_t t = wall.first; seamless && t + 2 < wall.first + wall.count; t += 3)
            for (int k = 0; k < 3; ++k)
                seamless = seamless && std::fabs(canMesh.face_plane[t + k].y - canMesh.face_plane[t + (k + 1) % 3].y) < 2.0f * 3.1416f * 2.0f / 8.0f;
        Check(seamless, "a cylinder's side: one picture around it, no triangle across the seam");
        Check(kv[22] == 1.0f, "a run's opacity multiplier is 1 unless it says otherwise");

        // The plain vertices are what they were before shadows existed.
        v.clear();
        sh::PushSegment(v, sh::Mesh::Segment{{10, 20, 0.5f}, {50, 20, 0.5f}, 0, 0}, sh::V3{}, 0xFFFFFFFFu, sh::WidthCode(4.0f), false);
        Check(v[2].side == 2 && v[2].end == 1 && v[2].flags == 0, "a plain line's corner bytes are unchanged");
    }

    // --- Particles (abi 0.1.40) ------------------------------------------------------------------------

    /// Flat ground at a height, everywhere.
    struct Floor final : sh::Ground
    {
        float heights[145];
        explicit Floor(float z) { std::fill(std::begin(heights), std::end(heights), z); }
        bool Present() override { return true; }
        bool Chunk(int32_t, int32_t, const float*& h, uint16_t& holes) override
        {
            h     = heights;
            holes = 0;
            return true;
        }
    };

    /// PixelView seen from @p eye: positions are written from it, the picture stays where it was.
    sh::View PixelViewAt(uint32_t w, uint32_t h, sh::V3 eye)
    {
        sh::View v = PixelView(w, h);
        v.vp[12]   = -1.0f + 2.0f * eye.x / static_cast<float>(w);
        v.vp[13]   = 1.0f - 2.0f * eye.y / static_cast<float>(h);
        v.vp[14]   = eye.z;
        v.eye      = eye;
        return v;
    }

    void BeamCases()
    {
        sh::Desc beam;
        beam.kind   = sh::kBeam;
        beam.center = sh::V3{0, 0, 0};
        beam.points = {{0, 0, 0}, {0, 10, 0}};
        beam.size   = sh::V3{2.0f, 1.0f, 0.0f};
        sh::Mesh mesh;
        Check(sh::Build(beam, sh::Parts{false, true}, nullptr, mesh) && mesh.beam.on && mesh.triangles.empty(), "a beam is built as its two ends");
        std::vector<sh::Vertex> v;
        sh::WriteBeam(mesh, sh::V3{20, 5, 0}, 0xFFFFFFFFu, false, v);
        Check(v.size() == sh::kBeamSegments * 6, "its quad strip: six corners a segment");
        // Seen from +x: it faces the eye, spread along z; its width at each end; bright at a, gone at b.
        Check(Near(std::fabs(v[0].pos[2] - v[1].pos[2]), 2.0f, 1e-3f) && Near(v[0].pos[0], -20.0f, 1e-3f), "turned around its axis to face the eye, 2 yards wide at a");
        const sh::Vertex& endB = v[v.size() - 4]; // the last segment's b side: corners -4 and -1
        Check(Near(std::fabs(endB.pos[2] - v[v.size() - 1].pos[2]), 1.0f, 1e-3f), "and 1 at b");
        Check((v[0].color >> 24) == 255 && (endB.color >> 24) == 0, "its opacity from 1 at a to 0 at b");
        Check(Near(v[0].other[1], -1.0f) && Near(v[1].other[1], 1.0f) && Near(v[2].other[0], 10.0f / 8.0f), "its plane: along in yards, across -1 to 1");
        // End-on: gone; near the eye: gone.
        v.clear();
        sh::WriteBeam(mesh, sh::V3{0, 30, 0}, 0xFFFFFFFFu, false, v);
        bool none = true;
        for (const auto& x : v) none = none && (x.color >> 24) == 0;
        Check(none, "seen along its axis it fades out instead of flashing");
        Check(sh::BeamSideways(sh::V3{0, 1, 0}, sh::V3{1, 0, 0}) == 1.0f && sh::BeamSideways(sh::V3{0, 1, 0}, sh::V3{0.1f, 1, 0}) < 1.0f,
              "the fade as it turns end-on is smooth");
        v.clear();
        sh::WriteBeam(mesh, sh::V3{0.5f, 0, 0}, 0xFFFFFFFFu, false, v);
        Check((v[0].color >> 24) < 60, "where it nears the eye it fades");
        // Its soft sides: feather is a part of its half width.
        sh::SurfaceLook look;
        look.feather = 1.0f;
        const sh::SurfaceConstants k = sh::MakeBeamSurface(mesh, look);
        float out[4];
        const float white[4] = {1, 1, 1, 1};
        sh::Shade(k, sh::V3{3.0f, 0.5f, 0.0f}, white, white, out);
        const float half = out[3];
        sh::Shade(k, sh::V3{9.0f, 0.0f, 0.0f}, white, white, out);
        Check(Near(half, 0.5f) && Near(out[3], 1.0f), "soft to its sides, whole along its middle, all along it");
        beam.points = {{0, 0, 0}};
        Check(!sh::Validate(beam).empty(), "a beam needs two points");
    }

    void ParticleCases()
    {
        const sh::V3 c = ChunkMiddle(511, 511);
        Floor        floor(c.z);
        // A burst of clods thrown up, falling, bouncing once and settling, never under the ground.
        sh::Emitter clods;
        clods.center      = sh::V3{c.x, c.y, c.z + 0.1f};
        clods.spread      = 0.5f;
        clods.speed_min   = 4.0f;
        clods.speed_max   = 6.0f;
        clods.life_min    = clods.life_max = 5.0f;
        clods.gravity     = 19.29f;
        clods.ground      = sh::kGroundBounce;
        clods.restitution = 0.4f;
        clods.friction    = 0.5f;
        sh::Pool   pool;
        sh::Random random(7);
        pool.Spawn(clods, clods.center, 0.0f, 50, random);
        Check(pool.Size() == 50, "a burst throws its count");
        bool above = true, up = true, bounced = false, settled = true;
        for (size_t i = 0; i < pool.Size(); ++i) up = up && pool.vz[i] > 0.0f;
        for (int frame = 0; frame < 240; ++frame)
        {
            pool.Step(clods, 1.0f / 60.0f, &floor, c.z, false);
            for (size_t i = 0; i < pool.Size(); ++i)
            {
                above   = above && pool.z[i] >= c.z - 1e-4f;
                bounced = bounced || pool.state[i] == 1;
            }
        }
        for (size_t i = 0; i < pool.Size(); ++i) settled = settled && pool.state[i] == 2 && std::fabs(pool.z[i] - c.z) < 1e-4f;
        Check(up && above && bounced && settled, "thrown up within the cone, they fall, bounce once, settle on the ground and never go under it");
        float slid = 0.0f;
        for (size_t i = 0; i < pool.Size(); ++i) slid = std::max(slid, std::fabs(pool.vx[i]) + std::fabs(pool.vy[i]));
        Check(slid < 0.5f, "settled, they slide to a stop");
        // Their lives run out.
        for (int frame = 0; frame < 120; ++frame) pool.Step(clods, 1.0f / 30.0f, &floor, c.z, false);
        Check(pool.Size() == 0, "their lives run out and they go");
        // The same seed, the same particles.
        sh::Pool   a, b;
        sh::Random ra(42), rb(42);
        a.Spawn(clods, clods.center, 0.3f, 20, ra);
        b.Spawn(clods, clods.center, 0.3f, 20, rb);
        Check(a.vx == b.vx && a.life == b.life && a.angle == b.angle, "a seed throws the same particles");
        // Grains on a disc pulled into its middle, going as they reach it.
        sh::Emitter grains;
        grains.spawn          = sh::kSpawnDisc;
        grains.center         = c;
        grains.extent         = sh::V3{3.0f, 0, 0};
        grains.speed_min      = grains.speed_max = 0.0f;
        grains.life_min       = grains.life_max = 10.0f;
        grains.attractor      = c;
        grains.attraction     = 12.0f;
        grains.attract_radius = 0.25f;
        grains.drag           = 1.0f;
        sh::Pool sand;
        sand.Spawn(grains, c, 0.0f, 40, random);
        bool onDisc = true;
        for (size_t i = 0; i < sand.Size(); ++i) onDisc = onDisc && std::hypot(sand.x[i] - c.x, sand.y[i] - c.y) <= 3.0f + 1e-3f && sand.z[i] == c.z;
        for (int frame = 0; frame < 300 && sand.Size(); ++frame) sand.Step(grains, 1.0f / 60.0f, nullptr, c.z, false);
        Check(onDisc && sand.Size() == 0, "grains thrown on a disc converge on the attractor and go there");
        // Keys: size, colour and opacity in lines between them; none: white, fading out.
        sh::Look look;
        float    size, alpha;
        uint32_t rgb;
        sh::Sample(look, 0.25f, size, rgb, alpha);
        Check(size == 1.0f && rgb == 0xFFFFFFu && Near(alpha, 0.75f), "no keys: white, its own size, fading out");
        look.key_count = 3;
        look.keys[0]   = sh::Key{0.0f, 0.5f, 0x000000u, 0.0f};
        look.keys[1]   = sh::Key{0.2f, 1.0f, 0xFF0000u, 1.0f};
        look.keys[2]   = sh::Key{1.0f, 3.0f, 0x0000FFu, 0.0f};
        sh::Sample(look, 0.1f, size, rgb, alpha);
        Check(Near(size, 0.75f) && rgb == 0x800000u && Near(alpha, 0.5f), "half way between two keys");
        sh::Sample(look, 0.6f, size, rgb, alpha);
        const auto close = [](uint32_t a, uint32_t b) {
            for (int shift : {0, 8, 16})
                if (std::abs(static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF)) > 1) return false;
            return true;
        };
        Check(Near(size, 2.0f, 1e-3f) && close(rgb, 0x800080u) && Near(alpha, 0.5f, 1e-3f), "and between the next two");
        // Written: six corners each, far to near, their frame of the sheet, their height over the softness.
        sh::Emitter still;
        still.speed_min = still.speed_max = 0.0f;
        still.life_min = still.life_max = 4.0f;
        still.size_min = still.size_max = 2.0f;
        sh::Pool   two;
        sh::Random r(3);
        two.Spawn(still, sh::V3{0, 10, 0}, 0.0f, 1, r);
        two.Spawn(still, sh::V3{0, 30, 0}, 0.0f, 1, r);
        two.angle[0] = two.angle[1] = 0.0f;
        sh::Camera camera; // right +x, up +z, looking along +y
        sh::Look   sheet;
        sheet.textured  = true;
        sheet.frames_x  = 4;
        sheet.frames_y  = 2;
        sheet.softness  = 1.0f;
        two.gz[0] = two.gz[1] = -0.5f; // the ground half a yard under them
        two.age[0] = two.age[1] = 2.1f; // past half of its life: the fifth frame of eight
        std::vector<sh::Vertex> v;
        two.Write(v, sheet, camera, sh::V3{0, 0, 0}, true);
        Check(v.size() == 12 && Near(v[0].pos[1], 30.0f), "six corners a particle, the far one first");
        Check(Near(v[0].other[0], 0.0f) && Near(v[0].other[1], 0.5f) && Near(v[1].other[0], 0.25f) && Near(v[2].other[1], 1.0f),
              "the frame of the sheet at its age, once over its life");
        Check(Near(v[0].other[2], 1.5f) && Near(v[2].other[2], -0.5f), "each corner's height above the ground over the softness");
        Check(Near(v[1].pos[0] - v[0].pos[0], 2.0f) && Near(v[0].pos[2] - v[2].pos[2], 2.0f), "facing the camera, its size across");
        float out[4];
        const float white[4] = {1, 1, 1, 1}, texel[4] = {0.1f, 0.2f, 0.3f, 0.5f};
        sh::ParticleShade(false, false, false, 0.5f, 0.5f, 1.0f, white, texel, out);
        const float centre = out[3];
        sh::ParticleShade(false, false, false, 0.0f, 0.5f, 1.0f, white, texel, out);
        Check(Near(centre, 1.0f) && Near(out[3], 0.0f), "no texture: a soft round blot, whole in its middle, nothing at its edge");
        sh::ParticleShade(true, false, true, 0.5f, 0.5f, 0.5f, white, texel, out);
        Check(Near(out[0], 1.0f) && Near(out[3], 0.25f), "a mask: the colour its own, the texture's opacity, half faded near the ground");
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

        // Since abi 0.1.38: an inside written as a surface (its plane, its numbers), a line's shadow.
        auto surface = [&](const char* what, const sh::Desc& d, sh::Parts parts, bool shadowed) {
            sh::Mesh    mesh;
            std::string why;
            const int   rounds = 200;
            const auto  from   = std::chrono::steady_clock::now();
            for (int i = 0; i < rounds; ++i) sh::Build(d, parts, &ground, mesh, &why);
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - from).count() / rounds;
            std::vector<sh::Vertex> stream;
            sh::SurfaceLook         look;
            look.textured = true;
            look.feather  = parts.feather;
            look.blend    = sh::kBlendModulate;
            sh::LineLook line, shadow;
            shadow.down = shadow.right = sh::OffsetCode(1.5f);
            shadow.soft = sh::SoftCode(1.0f);
            const auto w0 = std::chrono::steady_clock::now();
            for (int i = 0; i < rounds; ++i)
            {
                stream.clear();
                const sh::SurfaceConstants k = sh::MakeSurface(mesh, look);
                (void)k;
                const bool edged = mesh.edge.size() == mesh.triangles.size();
                for (size_t t = 0; t + 2 < mesh.triangles.size(); t += 3)
                {
                    sh::V3 plane[3];
                    for (int j = 0; j < 3; ++j)
                    {
                        plane[j]   = sh::InPlane(mesh, mesh.triangles[t + j]);
                        plane[j].z = edged ? mesh.edge[t + j] : 0.0f;
                    }
                    sh::PushSurfaceTriangle(stream, &mesh.triangles[t], plane, c, 0xCCFFFFFFu, true);
                }
                for (const sh::Mesh::Segment& seg : mesh.lines)
                {
                    if (shadowed) sh::PushSegment(stream, seg, c, shadow, true);
                    sh::PushSegment(stream, seg, c, line, true);
                }
            }
            const double ws = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - w0).count() / rounds;
            std::printf("  cost: %s: %zu vertices, built in %.1f us, written in %.1f us\n", what, stream.size(), us, ws);
        };
        sh::Desc ring = circle;
        ring.kind     = sh::kRing;
        ring.radius   = 6.0f;
        ring.inner    = 4.0f;
        surface("a 6 yd textured ring on the ground (a surface)", ring, sh::Parts{false, true}, false);
        surface("an 8 yd textured, feathered circle on the ground", circle, sh::Parts{false, true, 1.0f}, false);
        sh::Desc poly;
        poly.kind   = sh::kPolygon;
        poly.center = c;
        poly.points = {{-5, -4, 0}, {5, -4, 0}, {6, 3, 0}, {0, 1, 0}, {-6, 4, 0}};
        surface("a 12 yd polygon, feathered 1 yd (cut finer at its border)", poly, sh::Parts{false, true, 1.0f}, false);
        sh::Desc dig  = grid;
        dig.size      = sh::V3{6.0f, 6.0f, 0.0f};
        dig.spacing   = 1.0f;
        surface("a 6 x 6 yd digging grid: soft lines, their shadow, a modulated feathered inside", dig, sh::Parts{true, true, 0.5f}, true);
        grid.size    = sh::V3{32.0f, 32.0f, 0.0f};
        grid.spacing = 1.0f;
        surface("a 32 x 32 yd grid every yard with its shadow", grid, sh::Parts{true, false}, true);

        // Since abi 0.1.39: a textured box's and cylinder's faces written each frame (their numbers
        // made again: what a uv_offset scrolled each frame costs; nothing is built again).
        auto faces = [&](const char* what, const sh::Desc& d) {
            sh::Mesh    mesh;
            const int   rounds = 200;
            const auto  from   = std::chrono::steady_clock::now();
            for (int i = 0; i < rounds; ++i) sh::Build(d, sh::Parts{true, true}, nullptr, mesh);
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - from).count() / rounds;
            std::vector<sh::Vertex> stream;
            sh::SurfaceLook         look;
            look.textured = true;
            look.uv       = sh::kUvTile;
            const auto w0 = std::chrono::steady_clock::now();
            for (int i = 0; i < rounds; ++i)
            {
                stream.clear();
                look.offset_x = 0.01f * static_cast<float>(i);
                for (const sh::Mesh::Face& face : mesh.faces)
                {
                    const sh::SurfaceConstants k = sh::MakeFaceSurface(face, look);
                    (void)k;
                    for (uint32_t t = face.first; t + 2 < face.first + face.count; t += 3)
                        sh::PushSurfaceTriangle(stream, &mesh.triangles[t], &mesh.face_plane[t], c, 0xFFFFFFFFu, false);
                }
                for (const sh::Mesh::Segment& seg : mesh.lines) sh::PushSegment(stream, seg, c, 0xFFFFFFFFu, 16, false);
            }
            const double ws = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - w0).count() / rounds;
            std::printf("  cost: %s: %zu vertices, %zu faces, built in %.1f us, written with its numbers in %.1f us\n", what, stream.size(),
                        mesh.faces.size(), us, ws);
        };
        sh::Desc crate;
        crate.kind   = sh::kBox;
        crate.center = c;
        crate.size   = sh::V3{4.0f, 4.0f, 6.0f};
        faces("a textured box, outlined (every face written)", crate);
        sh::Desc can = crate;
        can.kind     = sh::kCylinder;
        can.radius   = 3.0f;
        faces("a textured cylinder, outlined (every face written)", can);

        // Particles: a thousand simulated (gravity, drag, a bounce on the terrain's own heights),
        // then written facing the camera, sorted far to near or not.
        sh::Emitter dust;
        dust.spawn     = sh::kSpawnDisc;
        dust.center    = c;
        dust.extent    = sh::V3{2.0f, 0, 0};
        dust.spread    = 1.0f;
        dust.speed_min = 1.0f;
        dust.speed_max = 4.0f;
        dust.life_min  = dust.life_max = 30.0f;
        dust.gravity   = 19.29f;
        dust.drag      = 0.5f;
        dust.ground    = sh::kGroundBounce;
        dust.restitution = 0.3f;
        sh::Pool   thousand;
        sh::Random random(11);
        thousand.Spawn(dust, c, 0.0f, 1000, random);
        const int  rounds = 200;
        const auto s0     = std::chrono::steady_clock::now();
        for (int i = 0; i < rounds; ++i) thousand.Step(dust, 1.0f / 600.0f, &ground, c.z, true);
        const double step = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - s0).count() / rounds;
        sh::Look     soft;
        soft.softness = 0.3f;
        sh::Camera   camera;
        std::vector<sh::Vertex> stream;
        const auto w0 = std::chrono::steady_clock::now();
        for (int i = 0; i < rounds; ++i)
        {
            stream.clear();
            thousand.Write(stream, soft, camera, c, false);
        }
        const double write = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - w0).count() / rounds;
        const auto   o0    = std::chrono::steady_clock::now();
        for (int i = 0; i < rounds; ++i)
        {
            stream.clear();
            thousand.Write(stream, soft, camera, c, true);
        }
        const double sorted = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - o0).count() / rounds;
        {
            sh::Desc ray;
            ray.kind   = sh::kBeam;
            ray.center = c;
            ray.points = {{0, 0, 0}, {2, 3, 12}};
            ray.size   = sh::V3{1.5f, 0.5f, 0.0f};
            sh::Mesh rayMesh;
            sh::Build(ray, sh::Parts{false, true}, nullptr, rayMesh);
            std::vector<sh::Vertex> rays;
            sh::SurfaceLook         rayLook;
            rayLook.textured = true;
            rayLook.feather  = 1.0f;
            const auto b0    = std::chrono::steady_clock::now();
            for (int i = 0; i < rounds; ++i)
            {
                rays.clear();
                for (int k = 0; k < 20; ++k)
                {
                    const sh::SurfaceConstants rk = sh::MakeBeamSurface(rayMesh, rayLook);
                    (void)rk;
                    sh::WriteBeam(rayMesh, sh::V3{c.x + 15.0f, c.y + static_cast<float>(k), c.z + 2.0f}, 0xFFFFE0A0u, false, rays);
                }
            }
            const double beams = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - b0).count() / rounds;
            std::printf("  cost: 20 beams turned to the eye, with their numbers: %.1f us, %zu vertices\n", beams, rays.size());
        }
        std::printf("  cost: 1000 particles: simulated in %.1f us, written in %.1f us (%.1f us sorted far to near), %zu vertices, %zu KiB to upload\n",
                    step, write, sorted, stream.size(), stream.size() * sizeof(sh::Vertex) / 1024);
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

    /// Bilinear at (u, v) of a w x h picture, clamped at its edges or repeated, as the sampler reads level 0.
    void Bilinear(const std::vector<uint32_t>& texels, int w, int h, float u, float v, bool wrap, float out[4])
    {
        const float x = u * static_cast<float>(w) - 0.5f, y = v * static_cast<float>(h) - 0.5f;
        const int   x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
        for (int c = 0; c < 4; ++c) out[c] = 0.0f;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i)
            {
                const int tx = wrap ? ((x0 + i) % w + w) % w : std::clamp(x0 + i, 0, w - 1);
                const int ty = wrap ? ((y0 + j) % h + h) % h : std::clamp(y0 + j, 0, h - 1);
                const uint32_t t  = texels[static_cast<size_t>(ty) * w + tx];
                const float    k  = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
                out[0] += k * static_cast<float>((t >> 16) & 0xFF) / 255.0f;
                out[1] += k * static_cast<float>((t >> 8) & 0xFF) / 255.0f;
                out[2] += k * static_cast<float>(t & 0xFF) / 255.0f;
                out[3] += k * static_cast<float>(t >> 24) / 255.0f;
            }
    }

    /// A surface of @p mesh as the service writes it: its triangles with their plane coordinates.
    void PushSurface(std::vector<sh::Vertex>& v, const sh::Mesh& mesh, uint32_t color)
    {
        const bool edged = mesh.edge.size() == mesh.triangles.size();
        for (size_t t = 0; t + 2 < mesh.triangles.size(); t += 3)
        {
            sh::V3 plane[3];
            for (int k = 0; k < 3; ++k)
            {
                plane[k]   = sh::InPlane(mesh, mesh.triangles[t + k]);
                plane[k].z = edged ? mesh.edge[t + k] : 0.0f;
            }
            sh::PushSurfaceTriangle(v, &mesh.triangles[t], plane, sh::V3{}, color, false);
        }
    }

    /// A textured ring (alpha), a stain (modulate) and a feathered, faded disc on a background, then
    /// a soft line over its shadow: every pixel held to the CPU's copy of the programs.
    void LiveSurfaces(Setup& s)
    {
        const uint32_t W = 128, H = 96;
        const sh::View view = PixelView(W, H);
        float          k[24];
        sh::Constants(view, k);
        render::Shapes shapes;
        shapes.Attach(s.render);

        // A 16 x 16 picture: colours across, an opacity down, and a darker band.
        const int             TW = 16, TH = 16;
        std::vector<uint32_t> texels(TW * TH);
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x)
                texels[static_cast<size_t>(y) * TW + x] = Argb(static_cast<uint32_t>(x * 16 + 8), static_cast<uint32_t>(255 - y * 12),
                                                               (x / 4 + y / 4) % 2 ? 40u : 200u, static_cast<uint32_t>(255 - y * 8));

        Check(s.Begin(), "a frame begins");
        draw::TextureDesc desc;
        desc.format         = draw::kTexArgb8888;
        desc.width          = TW;
        desc.height         = TH;
        draw::Texture* tex  = s.live.renderer.CreateTexture(desc);
        uint32_t       pitch = 0;
        uint8_t*       to    = tex ? s.live.renderer.LockTexture(tex, 0, 0, 0, 0, TW, TH, pitch) : nullptr;
        if (to)
        {
            for (int y = 0; y < TH; ++y) std::memcpy(to + static_cast<size_t>(y) * pitch, &texels[static_cast<size_t>(y) * TW], TW * 4u);
            s.live.renderer.UnlockTexture(tex);
        }
        Check(tex && to, "a picture is made on the device");

        // The background, opaque, then the three surfaces, then the line and its shadow.
        const uint32_t          background = 0xFFC08040u;
        std::vector<sh::Vertex> v;
        sh::PushTriangle(v, {-1, -1, 0.5f}, {200, -1, 0.5f}, {-1, 200, 0.5f}, sh::V3{}, background, false);
        std::vector<sh::Run> runs;
        runs.push_back(sh::Run{0, static_cast<uint32_t>(v.size())});

        struct Case
        {
            sh::Desc        desc;
            sh::SurfaceLook look;
            uint32_t        color;
            sh::Mesh        mesh;
            sh::Surface     surface;
        };
        Case cases[3];
        cases[0].desc.kind   = sh::kRing;
        cases[0].desc.center = sh::V3{30, 28, 0.5f};
        cases[0].desc.radius = 24.0f;
        cases[0].desc.inner  = 10.0f;
        cases[0].desc.facing = 0.4f;
        cases[0].look.textured = true;
        cases[0].color         = 0xE6FFFFFFu;
        cases[1].desc.kind   = sh::kCircle;
        cases[1].desc.center = sh::V3{98, 28, 0.5f};
        cases[1].desc.radius = 24.0f;
        cases[1].look.textured = true;
        cases[1].look.blend    = sh::kBlendModulate;
        cases[1].look.uv       = sh::kUvTile;
        cases[1].look.size_x = cases[1].look.size_y = 0.5f;
        cases[1].color         = 0xCCFFFFFFu;
        cases[2].desc.kind   = sh::kCircle;
        cases[2].desc.center = sh::V3{64, 62, 0.5f};
        cases[2].desc.radius = 16.0f;
        cases[2].look.feather = 5.0f;
        cases[2].look.radial  = 0.5f;
        cases[2].color        = 0xFFFF2010u;
        std::vector<sh::Surface> surfaces;
        for (Case& c : cases)
        {
            c.desc.flags = sh::kFlat;
            sh::Build(c.desc, sh::Parts{false, true}, nullptr, c.mesh);
            c.surface.constants = sh::MakeSurface(c.mesh, c.look);
            c.surface.texture   = c.look.textured ? tex : nullptr;
            c.surface.repeats   = sh::Repeats(c.look.uv);
            sh::Run run;
            run.first   = static_cast<uint32_t>(v.size());
            run.program = sh::kSurface;
            run.blend   = static_cast<uint8_t>(c.look.blend);
            run.surface = static_cast<uint32_t>(surfaces.size());
            PushSurface(v, c.mesh, c.color);
            run.count = static_cast<uint32_t>(v.size()) - run.first;
            runs.push_back(run);
            surfaces.push_back(c.surface);
        }
        // A white line 4 pixels wide, 2 soft, over a black shadow 3 pixels down, at y 88 (on its own
        // row of the background).
        sh::Run lines;
        lines.first = static_cast<uint32_t>(v.size());
        sh::LineLook shadow;
        shadow.color = 0xFF000000u;
        shadow.width = sh::WidthCode(6.0f);
        shadow.soft  = sh::SoftCode(2.0f);
        shadow.down  = sh::OffsetCode(3.0f);
        sh::PushSegment(v, sh::Mesh::Segment{{4, 88, 0.5f}, {124, 88, 0.5f}, 0, 0}, sh::V3{}, shadow, false);
        sh::LineLook white;
        white.color = 0xFFFFFFFFu;
        white.width = sh::WidthCode(4.0f);
        white.soft  = sh::SoftCode(2.0f);
        sh::PushSegment(v, sh::Mesh::Segment{{4, 88, 0.5f}, {124, 88, 0.5f}, 0, 0}, sh::V3{}, white, false);
        lines.count = static_cast<uint32_t>(v.size()) - lines.first;
        runs.push_back(lines);

        std::string    why;
        const uint32_t drawn = shapes.Draw(v.data(), runs.data(), static_cast<uint32_t>(runs.size()), surfaces.data(), k, view.viewport, &why);
        if (!drawn) std::printf("  refused: %s\n", why.c_str());
        Check(drawn == v.size(), "every surface and line is drawn");
        const std::vector<uint32_t> px = s.live.End();
        Check(px.size() == W * H, "the scene is read back");
        if (px.size() != W * H) return;

        const float bg[3] = {0xC0 / 255.0f, 0x80 / 255.0f, 0x40 / 255.0f};
        for (int which = 0; which < 3; ++which)
        {
            const Case& c       = cases[which];
            float       worst   = 0.0f;
            int         checked = 0;
            for (int y = 0; y < static_cast<int>(H); ++y)
                for (int x = 0; x < static_cast<int>(W); ++x)
                {
                    // Pixel centres on whole coordinates (Direct3D 9's); clear of the outline's chords.
                    const float  dx = static_cast<float>(x) - c.desc.center.x, dy = static_cast<float>(y) - c.desc.center.y;
                    const float  r  = std::sqrt(dx * dx + dy * dy);
                    if (r > c.desc.radius - 1.5f || (c.desc.inner > 0.0f && r < c.desc.inner + 1.5f)) continue;
                    const sh::V3 plane = sh::InPlane(c.mesh, sh::V3{static_cast<float>(x), static_cast<float>(y), 0.5f});
                    float        uv[2], texel[4] = {1, 1, 1, 1}, out[4];
                    sh::Picture(c.surface.constants, plane.x, plane.y, uv);
                    if (c.look.textured) Bilinear(texels, TW, TH, uv[0], uv[1], c.surface.repeats, texel);
                    const float color[4] = {static_cast<float>((c.color >> 16) & 0xFF) / 255.0f, static_cast<float>((c.color >> 8) & 0xFF) / 255.0f,
                                            static_cast<float>(c.color & 0xFF) / 255.0f, static_cast<float>(c.color >> 24) / 255.0f};
                    sh::Shade(c.surface.constants, plane, color, texel, out);
                    float expect[3];
                    for (int ch = 0; ch < 3; ++ch)
                        expect[ch] = c.look.blend == sh::kBlendModulate ? bg[ch] * out[ch] : out[ch] * out[3] + bg[ch] * (1.0f - out[3]);
                    const uint32_t got    = px[static_cast<size_t>(y) * W + x];
                    const float    have[3] = {static_cast<float>(R(got)) / 255.0f, static_cast<float>(G(got)) / 255.0f, static_cast<float>(B(got)) / 255.0f};
                    for (int ch = 0; ch < 3; ++ch) worst = std::max(worst, std::fabs(have[ch] - expect[ch]));
                    ++checked;
                }
            static const char* const kWhat[3] = {"a textured ring over the ground matches the CPU's picture (alpha)",
                                                 "a repeated stain multiplied into the ground matches it (modulate)",
                                                 "a feathered disc fading from its centre matches it"};
            if (worst > 4.0f / 255.0f) std::printf("  case %d: worst %.1f/255 over %d pixels\n", which, worst * 255.0f, checked);
            Check(checked > 200 && worst <= 4.0f / 255.0f, kWhat[which]);
        }
        // The line: its rows' coverage from its softness, its shadow 3 rows down.
        auto lum = [&](int x, int y) { return static_cast<float>(G(px[static_cast<size_t>(y) * W + x])) / 255.0f; };
        bool soft = true;
        for (int d = -4; d <= 0; ++d)
        {
            // Above the line nothing of the shadow (it lies 3 lower and 4 wide each side): the line over the background.
            const float cover = sh::LineCoverage(static_cast<float>(d), 3.0f, 2.0f);
            const float want  = cover + bg[1] * (1.0f - cover);
            if (d <= -2) soft = soft && std::fabs(lum(64, 88 + d) - want) <= 4.0f / 255.0f;
        }
        Check(soft, "a soft line's edge fades over its softness");
        Check(lum(64, 88) > 0.98f, "its middle is whole");
        Check(lum(64, 92) < 0.02f, "its shadow lies 3 pixels down, under it");
        Check(R(px[static_cast<size_t>(88) * W + 2]) == 0xC0, "nothing past its ends");
        shapes.Detach();
        if (tex) s.live.renderer.DestroyTexture(tex);
    }

    /// A box's face textured in its own frame and feathered at its edges, and a shape seen through a
    /// wall (occluded_alpha): every pixel held to the CPU's copy.
    void LiveVolumes(Setup& s)
    {
        const uint32_t W = 128, H = 96;
        const sh::View view = PixelView(W, H);
        float          k[24];
        sh::Constants(view, k);
        render::Shapes shapes;
        shapes.Attach(s.render);
        const int             TW = 8, TH = 8;
        std::vector<uint32_t> texels(TW * TH);
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x)
                texels[static_cast<size_t>(y) * TW + x] = Argb(static_cast<uint32_t>(x * 32 + 16), static_cast<uint32_t>(y * 32 + 16), 128u, 255u);
        Check(s.Begin(), "a frame begins");
        Check(s.Wall(0, 0, 64, 96, 0.2f), "a wall stands over the left half");
        draw::TextureDesc desc;
        desc.format         = draw::kTexArgb8888;
        desc.width          = TW;
        desc.height         = TH;
        draw::Texture* tex  = s.live.renderer.CreateTexture(desc);
        uint32_t       pitch = 0;
        uint8_t*       to    = tex ? s.live.renderer.LockTexture(tex, 0, 0, 0, 0, TW, TH, pitch) : nullptr;
        if (to)
        {
            for (int y = 0; y < TH; ++y) std::memcpy(to + static_cast<size_t>(y) * pitch, &texels[static_cast<size_t>(y) * TW], TW * 4u);
            s.live.renderer.UnlockTexture(tex);
        }
        // The box's top face (+z), turned, its picture over it; behind the wall on the left (z 0.5
        // and its top at 0.55, the wall at 0.2), drawn there at 0.3 of its opacity.
        sh::Desc box;
        box.kind   = sh::kBox;
        box.center = sh::V3{64, 48, 0.5f};
        box.size   = sh::V3{60, 40, 0.1f};
        box.facing = 0.25f;
        sh::Mesh mesh;
        sh::Build(box, sh::Parts{false, true}, nullptr, mesh);
        const sh::Mesh::Face& face = mesh.faces[1];
        sh::SurfaceLook look;
        look.textured = true;
        look.feather  = 4.0f;
        sh::Surface surface;
        surface.constants = sh::MakeFaceSurface(face, look);
        surface.texture   = tex;
        std::vector<sh::Vertex> v;
        const uint32_t          color = 0xFFFFFFFFu;
        for (uint32_t t = face.first; t + 2 < face.first + face.count; t += 3)
            sh::PushSurfaceTriangle(v, &mesh.triangles[t], &mesh.face_plane[t], sh::V3{}, color, false);
        sh::Run seen;
        seen.count   = static_cast<uint32_t>(v.size());
        seen.program = sh::kSurface;
        sh::Run hidden = seen;
        hidden.behind  = true;
        hidden.alpha   = 0.3f;
        const sh::Run runs[2] = {hidden, seen};
        std::string    why;
        const uint32_t drawn = shapes.Draw(v.data(), runs, 2, &surface, k, view.viewport, &why);
        if (!drawn) std::printf("  refused: %s\n", why.c_str());
        Check(drawn == 2 * v.size(), "the face is drawn twice: behind the wall, then where it is seen");
        const std::vector<uint32_t> px = s.live.End();
        if (px.size() != W * H) return;
        float worstSeen = 0.0f, worstHidden = 0.0f;
        int   seenCount = 0, hiddenCount = 0;
        const float wall[3] = {0.0f, 0.0f, 1.0f}, floor[3] = {0.0f, 0.0f, 0.0f};
        for (int y = 0; y < static_cast<int>(H); ++y)
            for (int x = 0; x < static_cast<int>(W); ++x)
            {
                const sh::V3 r{static_cast<float>(x) - face.origin.x, static_cast<float>(y) - face.origin.y, 0.0f};
                const sh::V3 p{r.x * face.up.x + r.y * face.up.y, r.x * face.left.x + r.y * face.left.y, 0.0f};
                if (std::fabs(p.x) > face.plane.hx - 1.0f || std::fabs(p.y) > face.plane.hy - 1.0f || std::abs(x - 64) < 2) continue;
                float uv[2], texel[4], out[4];
                sh::Picture(surface.constants, p.x, p.y, uv);
                Bilinear(texels, TW, TH, uv[0], uv[1], false, texel);
                const float c[4] = {1, 1, 1, 1};
                sh::Shade(surface.constants, p, c, texel, out);
                const bool  behind = x < 64;
                const float a      = out[3] * (behind ? 0.3f : 1.0f);
                const float* under = behind ? wall : floor;
                const uint32_t got = px[static_cast<size_t>(y) * W + x];
                const float have[3] = {R(got) / 255.0f, G(got) / 255.0f, B(got) / 255.0f};
                float worst = 0.0f;
                for (int ch = 0; ch < 3; ++ch) worst = std::max(worst, std::fabs(have[ch] - (out[ch] * a + under[ch] * (1.0f - a))));
                if (behind)
                {
                    worstHidden = std::max(worstHidden, worst);
                    ++hiddenCount;
                }
                else
                {
                    worstSeen = std::max(worstSeen, worst);
                    ++seenCount;
                }
            }
        if (worstSeen > 4.0f / 255.0f || worstHidden > 4.0f / 255.0f)
            std::printf("  face: worst %.1f/255 seen (%d), %.1f/255 behind the wall (%d)\n", worstSeen * 255.0f, seenCount, worstHidden * 255.0f, hiddenCount);
        Check(seenCount > 300 && worstSeen <= 4.0f / 255.0f, "a box's face textured in its own frame, feathered at its edges, matches the CPU's picture");
        Check(hiddenCount > 300 && worstHidden <= 4.0f / 255.0f, "behind the wall it is drawn at its occluded opacity over the wall");
        shapes.Detach();
        if (tex) s.live.renderer.DestroyTexture(tex);
    }

    /// Particles on the driver: soft blots blended by their opacity and masked pictures added,
    /// every pixel held to the CPU's copy (ParticleShade).
    void LiveParticles(Setup& s)
    {
        const uint32_t W = 128, H = 96;
        const sh::View view = PixelView(W, H);
        float          k[24];
        sh::Constants(view, k);
        render::Shapes shapes;
        shapes.Attach(s.render);
        const int             TW = 8, TH = 8;
        std::vector<uint32_t> texels(TW * TH);
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x)
                texels[static_cast<size_t>(y) * TW + x] = Argb(20u, 10u, 5u, static_cast<uint32_t>(std::min(255, 64 + 24 * (x + y))));
        Check(s.Begin(), "a frame begins");
        draw::TextureDesc desc;
        desc.format         = draw::kTexArgb8888;
        desc.width          = TW;
        desc.height         = TH;
        draw::Texture* tex  = s.live.renderer.CreateTexture(desc);
        uint32_t       pitch = 0;
        uint8_t*       to    = tex ? s.live.renderer.LockTexture(tex, 0, 0, 0, 0, TW, TH, pitch) : nullptr;
        if (to)
        {
            for (int y = 0; y < TH; ++y) std::memcpy(to + static_cast<size_t>(y) * pitch, &texels[static_cast<size_t>(y) * TW], TW * 4u);
            s.live.renderer.UnlockTexture(tex);
        }
        // The picture's camera: right +x, up -y (the screen's), looking into the depth.
        sh::Camera camera;
        camera.right   = sh::V3{1, 0, 0};
        camera.up      = sh::V3{0, -1, 0};
        camera.forward = sh::V3{0, 0, 1};
        sh::Emitter still;
        still.speed_min = still.speed_max = 0.0f;
        still.life_min = still.life_max = 10.0f;
        still.size_min = still.size_max = 40.0f;
        sh::Pool   blots, glows;
        sh::Random r(5);
        blots.Spawn(still, sh::V3{32, 48, 0.5f}, 0.0f, 1, r);
        glows.Spawn(still, sh::V3{96, 48, 0.5f}, 0.0f, 1, r);
        blots.angle[0] = glows.angle[0] = 0.0f;
        sh::Look plain, masked;
        plain.key_count  = 1;
        plain.keys[0]    = sh::Key{0.0f, 1.0f, 0x40C0FFu, 0.8f};
        masked.textured  = true;
        masked.mask      = true;
        masked.key_count = 1;
        masked.keys[0]   = sh::Key{0.0f, 1.0f, 0xFFE0A0u, 1.0f};
        std::vector<sh::Vertex> v;
        const uint32_t          background = 0xFF204060u;
        sh::PushTriangle(v, {-1, -1, 0.5f}, {300, -1, 0.5f}, {-1, 300, 0.5f}, sh::V3{}, background, false);
        sh::Run back;
        back.count = static_cast<uint32_t>(v.size());
        sh::Run one;
        one.first   = static_cast<uint32_t>(v.size());
        one.program = sh::kParticle;
        one.surface = 0;
        blots.Write(v, plain, camera, sh::V3{}, true);
        one.count = static_cast<uint32_t>(v.size()) - one.first;
        sh::Run two;
        two.first   = static_cast<uint32_t>(v.size());
        two.program = sh::kParticle;
        two.blend   = sh::kBlendAdd;
        two.surface = 1;
        glows.Write(v, masked, camera, sh::V3{}, false);
        two.count = static_cast<uint32_t>(v.size()) - two.first;
        sh::Surface surfaces[2];
        surfaces[0].constants = sh::ParticleConstants(false, false);
        surfaces[1].constants = sh::ParticleConstants(true, false, true);
        surfaces[1].texture   = tex;
        const sh::Run runs[3] = {back, one, two};
        std::string    why;
        const uint32_t drawn = shapes.Draw(v.data(), runs, 3, surfaces, k, view.viewport, &why);
        if (!drawn) std::printf("  refused: %s\n", why.c_str());
        Check(drawn == v.size(), "the particles are drawn");
        const std::vector<uint32_t> px = s.live.End();
        if (px.size() != W * H) return;
        const float bg[3] = {0x20 / 255.0f, 0x40 / 255.0f, 0x60 / 255.0f};
        float worst[2] = {0, 0};
        int   seen[2]  = {0, 0};
        for (int which = 0; which < 2; ++which)
        {
            const float cx = which ? 96.0f : 32.0f;
            const sh::Key& key = which ? masked.keys[0] : plain.keys[0];
            const float color[4] = {((key.color >> 16) & 0xFF) / 255.0f, ((key.color >> 8) & 0xFF) / 255.0f, (key.color & 0xFF) / 255.0f, key.alpha};
            for (int y = 30; y < 66; ++y)
                for (int x = static_cast<int>(cx) - 18; x < static_cast<int>(cx) + 18; ++x)
                {
                    const float u = (static_cast<float>(x) - (cx - 20.0f)) / 40.0f, vv = (static_cast<float>(y) - 28.0f) / 40.0f;
                    float       texel[4] = {1, 1, 1, 1}, out[4];
                    if (which) Bilinear(texels, TW, TH, u, vv, false, texel);
                    sh::ParticleShade(which == 1, false, which == 1, u, vv, 1000.0f, color, texel, out);
                    float expect[3];
                    for (int ch = 0; ch < 3; ++ch) expect[ch] = which ? bg[ch] + out[ch] * out[3] : out[ch] * out[3] + bg[ch] * (1.0f - out[3]);
                    const uint32_t got = px[static_cast<size_t>(y) * W + x];
                    const float have[3] = {R(got) / 255.0f, G(got) / 255.0f, B(got) / 255.0f};
                    for (int ch = 0; ch < 3; ++ch) worst[which] = std::max(worst[which], std::fabs(std::min(have[ch], 1.0f) - std::min(expect[ch], 1.0f)));
                    ++seen[which];
                }
        }
        if (worst[0] > 4.0f / 255.0f || worst[1] > 4.0f / 255.0f) std::printf("  particles: worst %.1f/255 blot, %.1f/255 masked glow\n", worst[0] * 255, worst[1] * 255);
        Check(seen[0] > 1000 && worst[0] <= 4.0f / 255.0f, "a soft round blot blended by its opacity matches the CPU's picture");
        Check(seen[1] > 1000 && worst[1] <= 4.0f / 255.0f, "a masked picture added in its own colour matches it");
        shapes.Detach();
        if (tex) s.live.renderer.DestroyTexture(tex);
    }

    /// Beams on the driver: soft to their sides, one plain blended by its opacity, one a masked
    /// picture repeated along it and added; every pixel held to the CPU's copy.
    void LiveBeams(Setup& s)
    {
        const uint32_t W = 128, H = 96;
        const sh::V3   eye{64, 48, -10000}; // far: the beams lie flat on the picture
        const sh::View view = PixelViewAt(W, H, eye);
        float          k[24];
        sh::Constants(view, k);
        render::Shapes shapes;
        shapes.Attach(s.render);
        const int             TW = 8, TH = 8;
        std::vector<uint32_t> texels(TW * TH);
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x)
                texels[static_cast<size_t>(y) * TW + x] = Argb(10u, 10u, 10u, static_cast<uint32_t>(40 + 27 * ((x * 3 + y * 5) % 8)));
        Check(s.Begin(), "a frame begins");
        draw::TextureDesc desc;
        desc.format         = draw::kTexArgb8888;
        desc.width          = TW;
        desc.height         = TH;
        draw::Texture* tex  = s.live.renderer.CreateTexture(desc);
        uint32_t       pitch = 0;
        uint8_t*       to    = tex ? s.live.renderer.LockTexture(tex, 0, 0, 0, 0, TW, TH, pitch) : nullptr;
        if (to)
        {
            for (int y = 0; y < TH; ++y) std::memcpy(to + static_cast<size_t>(y) * pitch, &texels[static_cast<size_t>(y) * TW], TW * 4u);
            s.live.renderer.UnlockTexture(tex);
        }
        struct Ray
        {
            sh::Desc        desc;
            sh::Mesh        mesh;
            sh::SurfaceLook look;
            uint32_t        color;
            sh::Surface     surface;
        } rays[2];
        rays[0].desc.points = {{14, 24, 0.5f}, {114, 24, 0.5f}};
        rays[0].desc.size   = sh::V3{24.0f, 12.0f, 0.0f};
        rays[0].look.feather = 1.0f;
        rays[0].color        = 0xFFFFD060u;
        rays[1].desc.points  = {{14, 70, 0.5f}, {114, 70, 0.5f}};
        rays[1].desc.size    = sh::V3{20.0f, 20.0f, 0.0f};
        rays[1].look.feather = 0.5f;
        rays[1].look.textured = true;
        rays[1].look.mask     = true;
        rays[1].look.uv       = sh::kUvTile;
        rays[1].look.size_y   = 0.25f;
        rays[1].look.offset_y = 0.1f;
        rays[1].look.blend    = sh::kBlendAdd;
        rays[1].color         = 0xFF80C0FFu;
        std::vector<sh::Vertex> v;
        const uint32_t          background = 0xFF203040u;
        sh::PushTriangle(v, {-1, -1, 0.5f}, {300, -1, 0.5f}, {-1, 300, 0.5f}, eye, background, false);
        std::vector<sh::Run>     runs;
        std::vector<sh::Surface> surfaces;
        runs.push_back(sh::Run{0, static_cast<uint32_t>(v.size())});
        for (Ray& r : rays)
        {
            r.desc.kind = sh::kBeam;
            sh::Build(r.desc, sh::Parts{false, true}, nullptr, r.mesh);
            r.surface.constants = sh::MakeBeamSurface(r.mesh, r.look);
            r.surface.texture   = r.look.textured ? tex : nullptr;
            r.surface.repeats   = true;
            sh::Run run;
            run.first   = static_cast<uint32_t>(v.size());
            run.program = sh::kSurface;
            run.blend   = static_cast<uint8_t>(r.look.blend);
            run.surface = static_cast<uint32_t>(surfaces.size());
            sh::WriteBeam(r.mesh, eye, r.color, false, v);
            run.count = static_cast<uint32_t>(v.size()) - run.first;
            runs.push_back(run);
            surfaces.push_back(r.surface);
        }
        std::string    why;
        const uint32_t drawn = shapes.Draw(v.data(), runs.data(), static_cast<uint32_t>(runs.size()), surfaces.data(), k, view.viewport, &why);
        if (!drawn) std::printf("  refused: %s\n", why.c_str());
        Check(drawn == v.size(), "the beams are drawn");
        const std::vector<uint32_t> px = s.live.End();
        if (px.size() != W * H) return;
        const float bg[3] = {0x20 / 255.0f, 0x30 / 255.0f, 0x40 / 255.0f};
        // The reference: each triangle of the strip projected as the program puts it, its plane and
        // colour interpolated at each pixel centre inside it (as the rasteriser does), then shaded.
        for (int which = 0; which < 2; ++which)
        {
            const Ray&     r      = rays[which];
            const sh::Run& run    = runs[1 + which];
            float          worst  = 0.0f;
            int            seenPx = 0;
            for (uint32_t t = run.first; t + 2 < run.first + run.count; t += 3)
            {
                float sx[3], sy[3];
                for (int c = 0; c < 3; ++c)
                {
                    float clip[4];
                    sh::Project(k, v[t + c], clip);
                    sx[c] = (clip[0] / clip[3] + 1.0f) * 0.5f * static_cast<float>(W);
                    sy[c] = (1.0f - clip[1] / clip[3]) * 0.5f * static_cast<float>(H);
                }
                const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
                if (std::fabs(area) < 1e-6f) continue;
                for (int y = 0; y < static_cast<int>(H); ++y)
                    for (int x = 0; x < static_cast<int>(W); ++x)
                    {
                        // Pixel centres on whole coordinates (Direct3D 9's).
                        const float px0 = static_cast<float>(x), py0 = static_cast<float>(y);
                        const float b0  = ((sx[1] - px0) * (sy[2] - py0) - (sx[2] - px0) * (sy[1] - py0)) / area;
                        const float b1  = ((sx[2] - px0) * (sy[0] - py0) - (sx[0] - px0) * (sy[2] - py0)) / area;
                        const float b2  = 1.0f - b0 - b1;
                        if (b0 < 0.02f || b1 < 0.02f || b2 < 0.02f) continue; // inside, clear of the edges
                        const float wts[3] = {b0, b1, b2};
                        sh::V3      plane{0, 0, 0};
                        float       a = 0.0f;
                        for (int c = 0; c < 3; ++c)
                        {
                            plane.x += wts[c] * v[t + c].other[0];
                            plane.y += wts[c] * v[t + c].other[1];
                            a += wts[c] * static_cast<float>(v[t + c].color >> 24) / 255.0f;
                        }
                        float uv[2], texel[4] = {1, 1, 1, 1}, out[4];
                        sh::Picture(r.surface.constants, plane.x, plane.y, uv);
                        if (r.look.textured) Bilinear(texels, TW, TH, uv[0], uv[1], true, texel);
                        const float color[4] = {((r.color >> 16) & 0xFF) / 255.0f, ((r.color >> 8) & 0xFF) / 255.0f, (r.color & 0xFF) / 255.0f, a};
                        sh::Shade(r.surface.constants, plane, color, texel, out);
                        const uint32_t got     = px[static_cast<size_t>(y) * W + x];
                        const float    have[3] = {R(got) / 255.0f, G(got) / 255.0f, B(got) / 255.0f};
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            const float expect = r.look.blend == sh::kBlendAdd ? std::min(bg[ch] + out[ch] * out[3], 1.0f)
                                                                               : out[ch] * out[3] + bg[ch] * (1.0f - out[3]);
                            worst = std::max(worst, std::fabs(have[ch] - expect));
                        }
                        ++seenPx;
                    }
            }
            if (worst > 4.0f / 255.0f) std::printf("  beam %d: worst %.1f/255 over %d pixels\n", which, worst * 255.0f, seenPx);
            Check(seenPx > 400 && worst <= 4.0f / 255.0f,
                  which ? "a beam of a masked picture repeated along it and added matches the CPU's picture"
                        : "a soft beam fading from its start to its end matches the CPU's picture");
        }
        shapes.Detach();
        if (tex) s.live.renderer.DestroyTexture(tex);
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
        SurfaceCases();
        ParticleCases();
        BeamCases();
        CostCases();
    }

    bool ShapesLiveCases()
    {
        std::printf("wxl.draw on the driver\n");
        ProgramCases();
        {
            Setup s;
            if (!s.Make(64, 48)) return false;
            LiveDraw(s);
        }
        // One device at a time: the device's functions are loaded for one.
        {
            Setup big;
            if (!big.Make(128, 96)) return false;
            LiveSurfaces(big);
        }
        {
            Setup volumes;
            if (!volumes.Make(128, 96)) return false;
            LiveVolumes(volumes);
        }
        {
            Setup particles;
            if (!particles.Make(128, 96)) return false;
            LiveParticles(particles);
        }
        Setup beams;
        if (!beams.Make(128, 96)) return false;
        LiveBeams(beams);
        return true;
    }
}
