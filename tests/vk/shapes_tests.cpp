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
        // The plain vertices are what they were before shadows existed.
        v.clear();
        sh::PushSegment(v, sh::Mesh::Segment{{10, 20, 0.5f}, {50, 20, 0.5f}, 0, 0}, sh::V3{}, 0xFFFFFFFFu, sh::WidthCode(4.0f), false);
        Check(v[2].side == 2 && v[2].end == 1 && v[2].flags == 0, "a plain line's corner bytes are unchanged");
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
        Setup big;
        if (!big.Make(128, 96)) return false;
        LiveSurfaces(big);
        return true;
    }
}
