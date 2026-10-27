// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.draw's shapes made into geometry, and the vertices both devices draw them from. Portable: no
// runtime, no device, no Windows; the native tests build it as it is.
//
//   Desc     a shape as wxl_draw_shape describes it, its points owned
//   Build    a Desc into a Mesh in world coordinates: line segments (their ends marked for a square
//            cap where the line ends or turns) and triangles. Ground shapes lie on a Ground: each
//            convex piece of the shape is cut along the terrain's own triangles (cell edges and
//            diagonals: the lines u, v, u + v and u - v = integer in grid units) and every vertex
//            takes the height of the triangle it lies on, so that the mesh lies exactly on the
//            surface the client walks on, whatever its slope; where there is no ground (a hole, a
//            chunk not loaded, off the map) the piece lies flat at center.z.
//   Vertex   32 bytes, what the vertex programs of both devices read (shaders/draw.hlsl for
//            Direct3D 9, gx/vk/render/glsl/draw.vert for Vulkan); a line segment is two triangles
//            whose corners carry both ends and a side, widened on the screen by the program.
//   View     the matrices the world was drawn with, and Project: the vertex program's arithmetic
//            on the CPU, which the tests hold both devices' pictures to.
//   Surface  a ground shape's inside drawn with a texture, a feather, a radial fade or a blend of
//            its own: its triangles carry the shape's own plane coordinates (and a polygon's
//            distance to its border), and SurfaceConstants the per-draw numbers the pixel programs
//            read; Shade is their arithmetic on the CPU.
//   Run      a stretch of the frame's vertices drawn with one program, blend and depth test.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wxl::gx::shapes
{
    struct V3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    struct P2
    {
        double x = 0.0, y = 0.0;
    };

    /// wxl_draw_kind, repeated so that this module needs no SDK header.
    enum Kind : uint32_t
    {
        kLine = 0, kCircle, kRing, kSector, kRectangle, kPolygon, kGrid, kBox, kSphere, kCylinder, kKindCount
    };

    /// wxl_draw_shape.flags.
    enum Flags : uint32_t
    {
        kFlat     = 0x1,
        kOnGround = 0x2,
        kClosed   = 0x4,
    };

    /// The contract's limits (wxl/services/draw.h).
    constexpr uint32_t kMaxPoints         = 4096;
    constexpr float    kMaxExtent         = 250.0f;
    constexpr uint32_t kMaxGridLines      = 1024;
    constexpr uint32_t kMaxShapeVertices  = 131072;
    constexpr uint32_t kMaxFrameVertices  = 524288;
    constexpr float    kMaxWidth          = 31.0f;

    struct Desc
    {
        uint32_t        kind = kLine;
        uint32_t        flags = 0;
        V3              center;
        float           radius = 0.0f;
        float           inner = 0.0f;
        float           angle = 0.0f;
        float           facing = 0.0f;
        float           pitch = 0.0f;
        float           roll = 0.0f;
        V3              size;
        float           spacing = 0.0f;
        std::vector<V3> points; ///< offsets from center
        std::vector<float> alphas; ///< a line's opacity at each point; empty for 1
    };

    /// Whether the kind lies on the ground.
    constexpr bool IsGround(uint32_t kind) { return kind >= kCircle && kind <= kGrid; }
    constexpr bool IsVolume(uint32_t kind) { return kind >= kBox && kind <= kCylinder; }

    /// Empty when the description can be built; otherwise why not, in a sentence naming the field.
    std::string Validate(const Desc& desc);

    /// The ground: the terrain's chunks, as the runtime reads them (or a test's).
    class Ground
    {
    public:
        virtual ~Ground() = default;
        /// Whether the map has terrain at all; false: every ground shape lies flat.
        virtual bool Present() = 0;
        /// Chunk (chunk_x, chunk_y)'s 145 absolute heights and its holes; false when not loaded.
        virtual bool Chunk(int32_t chunk_x, int32_t chunk_y, const float*& heights, uint16_t& holes) = 0;
    };

    /// A shape as built, in world coordinates.
    struct Mesh
    {
        struct Segment
        {
            V3      a, b;
            uint8_t cap_a = 0, cap_b = 0; ///< a square cap half the width long at that end
            float   alpha_a = 1.0f, alpha_b = 1.0f; ///< the opacity of each end (a line's points' opacities)
            float   t_a = -1.0f, t_b = -1.0f;       ///< how far along its line each end is, 0 to 1; -1: not a line's
        };
        /// How a ground shape's inside is drawn as a surface: the shape's plane (yards from its
        /// center, turned by its facing: x along it) and what bounds it.
        struct Plane
        {
            uint32_t code = 0;       ///< 0 round (circle, ring, sector), 1 a box (rectangle, grid), 2 a polygon (distances at the vertices)
            float    radius = 0.0f;  ///< round: the outer radius
            float    inner = 0.0f;   ///< round: the hole's
            float    half_angle = 4.0f; ///< round: half the sector's arc in radians; pi or more for none
            float    hx = 0.0f, hy = 0.0f; ///< a box's half sides
            float    extent = 1.0f;  ///< the farthest the inside reaches from the center (for the radial fade)
            float    min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f; ///< its bounds in its plane
        };
        std::vector<Segment> lines;
        std::vector<V3>      triangles; ///< three corners each
        std::vector<V3>      normals;   ///< one per triangle of a volume (outwards), to draw its front faces only
        bool                 ground = false; ///< drawn pulled towards the eye, over the ground it lies on
        V3                   middle;         ///< where it is, to draw volumes far to near
        V3                   origin;         ///< the center it was built at
        float                turn = 0.0f;    ///< and the facing its plane is turned by
        Plane                plane;          ///< a ground shape's inside, for a surface
        std::vector<float>   edge;           ///< a polygon's inside built for a feather: each triangle corner's distance to its border

        size_t Vertices() const { return lines.size() * 6 + triangles.size(); }
        void   Clear()
        {
            lines.clear();
            triangles.clear();
            normals.clear();
            edge.clear();
        }
    };

    /// What of a shape is built: its lines (a line, an outline, a grid's lines, a wireframe) and
    /// its inside (a ground shape's or a volume's fill).
    struct Parts
    {
        bool  lines = true;
        bool  fill = false;
        float feather = 0.0f; ///< yards: a polygon's inside is cut finer near its border and its distances kept
    };

    /**
     * @brief @p desc built into @p out in world coordinates.
     * @param ground  the terrain under ground shapes; null: they lie flat
     * @return false with @p why past kMaxShapeVertices (or for a shape Validate refuses)
     */
    bool Build(const Desc& desc, Parts parts, Ground* ground, Mesh& out, std::string* why = nullptr);

    // --- The pieces, for the tests -----------------------------------------------------------------

    /// Grid units: u along the terrain's rows (world x falling), v along its columns (world y falling).
    P2 ToGrid(double x, double y);
    P2 FromGrid(double u, double v);

    /// A convex polygon of the plane (world x, y; either winding) laid on the ground as triangles
    /// appended to @p out, at @p flat where there is none.
    void DrapeConvex(const std::vector<P2>& convex, float flat, Ground* ground, std::vector<V3>& out);

    /// The segment a-b laid on the ground: the points where it crosses the terrain's triangles'
    /// edges, appended to @p out as segments (the ends' z where there is no ground). @p cap_a and
    /// @p cap_b cap the first and the last.
    void DrapeSegment(V3 a, V3 b, Ground* ground, std::vector<Mesh::Segment>& out, bool cap_a, bool cap_b, float alpha_a = 1.0f,
                      float alpha_b = 1.0f, float t_a = -1.0f, float t_b = -1.0f);

    /// The height of the ground at a point, on the triangle under it; false where there is none.
    bool HeightAt(Ground& ground, double x, double y, float& out);

    /// A simple polygon (no edge crossing another; either winding) cut into triangles by ear
    /// clipping: indices into @p polygon, three per triangle. False for fewer than three points or
    /// one that cannot be cut (it crosses itself).
    bool Triangulate(const std::vector<P2>& polygon, std::vector<uint32_t>& out);

    // --- What the devices draw ---------------------------------------------------------------------

    /// One vertex of the stream, as both vertex programs read it (stream 0, stride 32).
    struct Vertex
    {
        float    pos[3];   ///< from the eye: world minus View::eye
        float    other[3]; ///< a line corner: the segment's other end; a triangle: pos again, or a surface's plane (x, y, distance)
        uint32_t color;    ///< 0xAARRGGBB (D3DCOLOR)
        uint8_t  side;     ///< bits 0-1: 0 left of the segment, 2 right of it, 1 a triangle's corner (not widened);
                           ///< bits 2-7: the move down on the screen in half pixels at 1080 lines (6-bit two's complement)
        uint8_t  end;      ///< bit 0: 0 at the segment's first end, 1 at its second; bits 1-6: the move right, as side's
        uint8_t  width;    ///< the line's width in eighths of a pixel at 1080 lines
        uint8_t  flags;    ///< bit 0: pulled towards the eye (a ground shape); bit 1: a square cap at this end;
                           ///< bits 2-7: the edges' softness in quarter pixels at 1080 lines
    };
    static_assert(sizeof(Vertex) == 32, "the stream's stride is 32 bytes on both devices");

    /// The world's projection as it was drawn, from the engine's own matrices.
    struct View
    {
        float vp[16] = {};      ///< view times projection, rows (a0..a3, b0.., c0.., d0..): clip = [x y z 1] * vp
        float viewport[6] = {}; ///< D3D9's: x, y, width, height in pixels, min and max depth
        V3    eye;              ///< where positions are taken from (the world's draws' origin)
        float scale = 1.0f;     ///< pixels per pixel of a 1080-line picture: the scene's height / 1080
    };

    /// The pull of a ground shape towards the eye: a fraction of the distance and a constant, which
    /// keeps it over the terrain it lies on without moving it on the screen.
    constexpr float kLiftScale = 0.002f;
    constexpr float kLiftYards = 0.02f;

    /// The constants both vertex programs take: the matrix's four columns, then (viewport width,
    /// height, half a pixel's width at 1080 lines in pixels / 8, 0) and (kLiftScale, kLiftYards, 0, 0).
    void Constants(const View& view, float out[24]);

    /// A vertex where the vertex program puts it, in clip space (D3D9's: y up, z 0 to w): the
    /// arithmetic of shaders/draw.hlsl and glsl/draw.vert on the CPU.
    void Project(const float constants[24], const Vertex& vertex, float out[4]);

    /// The six vertices of a segment (two triangles, cw), from the eye; a triangle's three.
    void PushSegment(std::vector<Vertex>& out, const Mesh::Segment& s, V3 eye, uint32_t color, uint8_t width, bool lift);
    void PushTriangle(std::vector<Vertex>& out, V3 a, V3 b, V3 c, V3 eye, uint32_t color, bool lift);

    /// A width in pixels at 1080 lines as the stream carries it (clamped to 0.5..kMaxWidth).
    uint8_t WidthCode(float pixels);

    constexpr float kMaxSoftness     = 15.0f; ///< pixels at 1080 lines
    constexpr float kMaxShadowOffset = 15.0f;

    /// How a segment is drawn: its colour (the alpha multiplied by its ends' opacities), width,
    /// softness and the move on the screen (a shadow's).
    struct LineLook
    {
        uint32_t color = 0xFFFFFFFFu;
        uint8_t  width = 16;
        uint8_t  soft = 0;    ///< quarter pixels, 0-63 (SoftCode)
        int8_t   right = 0;   ///< half pixels, -32 to 31 (OffsetCode)
        int8_t   down = 0;
        float    alpha_start = 1.0f, alpha_end = 1.0f; ///< along a line (Segment::t), multiplying the colour's alpha
    };
    uint8_t SoftCode(float pixels);
    int8_t  OffsetCode(float pixels);
    /// The segment's six vertices in @p look.
    void PushSegment(std::vector<Vertex>& out, const Mesh::Segment& s, V3 eye, const LineLook& look, bool lift);

    /// A surface's triangle: its corners in world coordinates and in the shape's plane (x, y, and
    /// a polygon's distance to its border).
    void PushSurfaceTriangle(std::vector<Vertex>& out, const V3 world[3], const V3 plane[3], V3 eye, uint32_t color, bool lift);
    /// A world point in a mesh's plane (yards from its origin, turned by its facing).
    V3 InPlane(const Mesh& mesh, V3 world);

    // --- Surfaces -------------------------------------------------------------------------------------

    enum Uv : uint32_t
    {
        kUvFit = 0, kUvTile, kUvWorld,
    };
    enum Blend : uint32_t
    {
        kBlendAlpha = 0, kBlendAdd, kBlendModulate,
    };

    /// What a surface looks like, from wxl_draw_style's fields of abi 0.1.38.
    struct SurfaceLook
    {
        bool     textured = false;
        float    right = 1.0f, bottom = 1.0f; ///< the part of the texture the picture fills
        uint32_t uv = kUvFit;
        float    size_x = 1.0f, size_y = 1.0f;
        float    rotation = 0.0f;
        float    offset_x = 0.0f, offset_y = 0.0f;
        uint32_t blend = kBlendAlpha;
        float    feather = 0.0f;
        float    radial = 0.0f;
    };

    /// The per-draw numbers of a surface: six rows of four, the pixel programs' (c200 to c205 on
    /// Direct3D 9, the frame's memory on Vulkan):
    ///   0, 1  u and v of the picture = row . (x, y, 1), x, y the plane's coordinates
    ///   2     the picture's part of the texture (right, bottom), 1 when it repeats, 1 when textured
    ///   3     plane code, outer radius, inner radius, feather (yards)
    ///   4     half the sector's angle, a box's half sides, where the radial fade starts (2: none)
    ///   5     1 / the extent, 1 for MODULATE, 0, 0
    struct SurfaceConstants
    {
        float k[6][4] = {};
    };
    SurfaceConstants MakeSurface(const Mesh& mesh, const SurfaceLook& look);

    /// Whether the sampler repeats the picture (TILE and WORLD).
    constexpr bool Repeats(uint32_t uv) { return uv == kUvTile || uv == kUvWorld; }

    /// The surface's pixel at a point of its plane: the pixel programs' arithmetic. @p texel is
    /// what the texture holds at the picture's coordinate (Picture), @p color the vertex colour (0-1
    /// each). Writes the colour the blend is given (RGB, alpha).
    void Picture(const SurfaceConstants& s, float x, float y, float out_uv[2]);
    void Shade(const SurfaceConstants& s, V3 plane, const float color[4], const float texel[4], float out[4]);

    /// A line's pixel at @p across pixels from its middle: the opacity its softness leaves
    /// (half_width and soft in pixels as drawn).
    float LineCoverage(float across, float half_width, float soft);

    // --- Runs -----------------------------------------------------------------------------------------

    enum Program : uint8_t
    {
        kPlain = 0,   ///< colour of the vertex, a line's soft edges
        kSurface = 1, ///< a surface: the picture, the feather, the fade
    };

    /// A stretch of the frame's vertices drawn alike.
    struct Run
    {
        uint32_t first = 0, count = 0;
        bool     on_top = false;
        uint8_t  program = kPlain;
        uint8_t  blend = kBlendAlpha;  ///< a surface's
        uint32_t surface = 0;          ///< a surface's: an index into the frame's surfaces
    };

    /// A surface's numbers and its texture as the device holds it (an IDirect3DBaseTexture9*, a
    /// Vulkan draw::Texture*; null when untextured).
    struct Surface
    {
        SurfaceConstants constants;
        void*            texture = nullptr;
        bool             repeats = false;
    };
}
