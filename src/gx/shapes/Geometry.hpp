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
        };
        std::vector<Segment> lines;
        std::vector<V3>      triangles; ///< three corners each
        std::vector<V3>      normals;   ///< one per triangle of a volume (outwards), to draw its front faces only
        bool                 ground = false; ///< drawn pulled towards the eye, over the ground it lies on
        V3                   middle;         ///< where it is, to draw volumes far to near

        size_t Vertices() const { return lines.size() * 6 + triangles.size(); }
        void   Clear()
        {
            lines.clear();
            triangles.clear();
            normals.clear();
        }
    };

    /// What of a shape is built: its lines (a line, an outline, a grid's lines, a wireframe) and
    /// its inside (a ground shape's or a volume's fill).
    struct Parts
    {
        bool lines = true;
        bool fill = false;
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
    void DrapeSegment(V3 a, V3 b, Ground* ground, std::vector<Mesh::Segment>& out, bool cap_a, bool cap_b);

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
        float    other[3]; ///< a line corner: the segment's other end; a triangle: pos again
        uint32_t color;    ///< 0xAARRGGBB (D3DCOLOR)
        uint8_t  side;     ///< 0: to the left of the segment, 2: to its right, 1: a triangle's corner (not widened)
        uint8_t  end;      ///< 0: at the segment's first end, 1: at its second
        uint8_t  width;    ///< the line's width in eighths of a pixel at 1080 lines
        uint8_t  flags;    ///< 1: pulled towards the eye (a ground shape); + 2: a square cap at this end
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
}
