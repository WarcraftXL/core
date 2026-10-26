// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The engine's draws turned into Vulkan: what CGxDevice's work becomes once the engine's state has
// been read (gxfront/ reads it, from the live engine or from a frame capture). This module knows
// the engine's numbers (its texture formats, pool usages, primitives, blend modes, texture ops)
// but none of its objects: it builds and tests natively, on lavapipe.
//
//   Texture   a CGxTex on the device: its image, its bindless slot, its uploads (Lock/Unlock)
//   Pool      a CGxPool on the device: a stream pool's mapped buffers, renamed when the engine
//             discards it; a static or dynamic pool's CPU copy and device buffer, the ranges the
//             engine wrote copied over before the first draw that reads them
//   Packet    one draw, every state it depends on resolved
//   Renderer  records packets into the open frame of a present::Display: the programs, the
//             pipelines (made at first use, no pipeline libraries yet), the bindless table, the
//             frame's rings, the uploads (in the frame's upload pass when nothing drew with the
//             resource yet this frame, inline otherwise: the rendering is ended, copied into,
//             resumed)
//
// Main thread only. The programs are the renderer's own (shaders/glsl/): D3D9's fixed function
// for stages 0 and 1, and the client's interface programs "UI" and "Desaturate" by name; the
// client's other programs are not translated at this step (V3) and a draw that binds one is not
// recorded.

#pragma once

#include "gx/vk/draw/Formats.hpp"
#include "gx/vk/pipelines/Pipelines.hpp"
#include "gx/vk/present/Display.hpp"
#include "gx/vk/resources/Bindless.hpp"
#include "gx/vk/resources/Resources.hpp"
#include "gx/vk/resources/Ring.hpp"

#include <cstdint>
#include <vector>

namespace wxl::vk::draw
{
    // --- The engine's numbers this module speaks in ------------------------------------------------

    /// EGxPrim.
    enum Primitive : uint32_t
    {
        kPoints        = 0,
        kLines         = 1,
        kLineStrip     = 2,
        kTriangles     = 3,
        kTriangleStrip = 4,
        kTriangleFan   = 5,
    };

    /// EGxPoolTarget, EGxPoolUsage.
    enum PoolTarget : uint32_t
    {
        kVertexPool = 0,
        kIndexPool  = 1,
    };
    enum PoolUsage : uint32_t
    {
        kStatic  = 0,
        kDynamic = 1,
        kStream  = 2,
    };

    /// The engine's vertex attribute types (CGxVertexAttrib::m_type).
    enum AttributeType : uint8_t
    {
        kD3dColor = 0,
        kUbyte4   = 1,
        kUbyte4N  = 2,
        kFloat2   = 3,
        kFloat3   = 4,
        kShort2   = 5,
        kFloat1   = 6,
    };

    /// The engine's blend modes (EGxBlend), the fork's BlendAdd included.
    constexpr uint32_t kBlendModes = 13;

    /// The data block of a draw, as the programs read it (shaders/glsl/engine.glsl, DrawData).
    struct DrawData
    {
        float   worldView[16];
        float   projection[16];
        float   texMatrix[2][16];
        float   constants[4][4];
        float   material[4];
        float   fog[4];
        float   fogColor[4];
        int32_t ops[4];
        int32_t texgen[4];
        int32_t inputs[4];
    };
    static_assert(sizeof(DrawData) == 416, "DrawData is the shaders' block, std430");

    // --- Resources ---------------------------------------------------------------------------------

    struct TextureDesc
    {
        uint32_t format = kTexArgb8888; ///< EGxTexFormat
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t levels = 1;
        bool     cube = false;
        bool     target = false;        ///< a render target (drawn into from V3; cleared until then)
        bool     generate_mips = false; ///< the engine's AUTOGENMIPMAP: levels made from the first
    };

    struct Texture
    {
        TextureDesc         desc;
        FormatInfo          info;
        resources::Image    image;
        VkImageView         flat = VK_NULL_HANDLE; ///< a cube's first face as a 2D view, what a 2D sampler reads
        uint32_t            slot = resources::Bindless::kNoSlot;
        uint64_t            used = 0;     ///< the last frame a draw read it in
        uint64_t            opened = 0;   ///< the frame whose upload pass holds it open (TRANSFER_DST)
        bool                defined = false; ///< it went through an upload pass once: SHADER_READ_ONLY since
        // A lock in progress.
        bool                locked = false;
        uint32_t            lock_face = 0;
        uint32_t            lock_level = 0;
        VkOffset2D          lock_at = {};
        VkExtent2D          lock_size = {};
        resources::Ring::Span lock_span;
    };

    struct Pool
    {
        uint32_t target = kVertexPool;
        uint32_t usage = kStatic;
        uint32_t size = 0;
        // A stream pool: the buffer written now, and those it was renamed from.
        resources::Buffer current;
        struct Retired
        {
            uint64_t          frame;
            resources::Buffer buffer;
        };
        std::vector<Retired>           retired;
        // A static or dynamic pool: what the engine writes, and the device's copy of it.
        std::vector<uint8_t> shadow;
        resources::Buffer    device;
        uint32_t             dirty_from = 0; ///< the bytes written since the last copy: [from, to)
        uint32_t             dirty_to = 0;
        uint64_t             used = 0;       ///< the last frame a draw read it in
    };

    // --- A draw ----------------------------------------------------------------------------------

    struct Attribute
    {
        uint8_t  location = 0; ///< EGxVertexAttrib
        uint8_t  type = kFloat3;
        uint8_t  stream = 0;
        uint32_t offset = 0;
    };

    struct Stream
    {
        Pool*    pool = nullptr;
        uint32_t offset = 0;
        uint32_t stride = 0;
    };

    struct Packet
    {
        pipelines::VertexProgram vertex = pipelines::VertexProgram::Fixed;
        pipelines::PixelProgram  pixel = pipelines::PixelProgram::Fixed;

        uint32_t  primitive = kTriangles;
        bool      indexed = false;
        Pool*     indices = nullptr;
        uint32_t  first_index = 0;   ///< indexed: into the index pool, in 16-bit indices
        uint32_t  count = 0;         ///< indices when indexed, vertices otherwise
        uint32_t  attribute_count = 0;
        Attribute attributes[14];
        uint32_t  stream_count = 0;
        Stream    streams[14];

        uint32_t blend = 0;          ///< EGxBlend
        uint32_t color_mask = 0xF;   ///< VkColorComponentFlags
        float    blend_factor = 1.0f;
        float    alpha_ref = 0.0f;   ///< 1..255; 0: no alpha test
        bool     depth_test = false;
        bool     depth_write = false;
        VkCompareOp depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
        float    depth_bias = 0.0f;  ///< D3D9's DEPTHBIAS, in depth units
        VkCullModeFlags cull = VK_CULL_MODE_NONE;
        bool     wireframe = false;

        float    viewport[6] = {};   ///< x, y, width, height in pixels (D3D9's), min and max depth
        int32_t  scissor[4] = {};    ///< x, y, width, height in pixels

        Texture*                  textures[2] = {};
        resources::SamplerKey     samplers[2];

        DrawData data = {};
    };

    /// The renderer's program for a client shader, by the name its record has ("UI:0", "UI",
    /// "Desaturate"); false for the client's others, which the renderer does not draw yet.
    bool ProgramByName(bool vertex, const char* name, uint8_t& program);

    // --- The renderer -----------------------------------------------------------------------------

    class Renderer final : public present::FrameHook
    {
    public:
        struct Stats
        {
            uint64_t draws = 0;        ///< recorded
            uint64_t pipelines = 0;    ///< made
            uint64_t pass_breaks = 0;  ///< renderings ended for an inline copy
            uint64_t upload_bytes = 0; ///< copied to the device
            uint64_t textures = 0;     ///< alive
            uint64_t pools = 0;        ///< alive
            uint64_t refused = 0;      ///< draws dropped for want of memory or a pipeline
        };

        Renderer() = default;
        Renderer(const Renderer&)            = delete;
        Renderer& operator=(const Renderer&) = delete;
        ~Renderer() { Destroy(); }

        /// Draws into @p display's scene (its colour and depth formats). Registers itself as the
        /// display's frame hook.
        bool Create(device::Context& context, resources::Allocator& allocator, present::Display& display, char* why,
                    uint32_t why_size);
        /// Its textures and pools must be destroyed first (or the device lost); waits for nothing.
        void Destroy();
        bool Ready() const { return pipelines_.Ready(); }

        // --- Textures ---------------------------------------------------------------------------
        /// Null when the device cannot make it (its format, its size, the bindless table full).
        Texture* CreateTexture(const TextureDesc& desc);
        /// Released once the frames that may read it have completed.
        void     DestroyTexture(Texture* texture);
        /**
         * @brief Where the bytes of @p width x @p height texels at (@p x, @p y) of @p level of @p face
         *        go (D3D9's layout of the format; rows of 4x4 blocks when compressed, the rectangle on
         *        block edges), @p pitch bytes per row. Null outside a frame or without memory.
         */
        uint8_t* LockTexture(Texture* texture, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t width,
                             uint32_t height, uint32_t& pitch);
        /// The bytes written since LockTexture go to the image.
        void     UnlockTexture(Texture* texture);

        // --- Pools ------------------------------------------------------------------------------
        Pool*    CreatePool(uint32_t target, uint32_t usage, uint32_t size);
        void     DestroyPool(Pool* pool);
        /// The pool's size changed: its memory is made again (a static pool keeps what fits).
        void     ResizePool(Pool* pool, uint32_t size);
        /// Where the engine writes the pool's bytes: a stream pool's mapped buffer, the CPU copy of
        /// the others. Null without memory.
        uint8_t* PoolMemory(Pool* pool);
        /// A stream pool starts again at 0 (D3D9's DISCARD): its buffer is renamed.
        void     DiscardPool(Pool* pool);
        /// The engine wrote @p size bytes at @p offset (a static or dynamic pool).
        void     PoolWritten(Pool* pool, uint32_t offset, uint32_t size);
        /// The device goes (it is idle or lost): the pool's buffers go now; a static or dynamic
        /// pool keeps its bytes, copied whole to the next device at its next draw.
        void     Forget(Pool* pool);

        // --- Draws ------------------------------------------------------------------------------
        /// Records @p packet into the open frame; false when it was not (no frame, no memory, no pipeline).
        bool Draw(const Packet& packet);

        const Stats&            Counters() const { return stats_; }
        pipelines::Pipelines&   PipelineCache() { return pipelines_; }
        resources::Bindless&    Table() { return bindless_; }
        const Formats&          TextureFormats() const { return formats_; }
        present::Display&       Screen() { return *display_; }

        // present::FrameHook
        void FrameBegun(uint32_t slot, VkCommandBuffer upload) override;
        void FrameEnding(VkCommandBuffer upload) override;

    private:
        bool EnsureDevice(Pool* pool);
        bool FlushPool(Pool* pool);
        void OpenInUpload(Texture* texture);
        void CopyIntoImage(Texture* texture, VkCommandBuffer cmd, const resources::Ring::Span& span, uint32_t pitch);
        void GenerateMips(Texture* texture, VkCommandBuffer cmd);
        void Retire(resources::Buffer buffer);
        void Retire(resources::Image image, VkImageView flat, uint32_t slot);
        VkCommandBuffer Inline();
        uint16_t LayoutOf(const Packet& packet, int32_t& inputs);
        void Collect(bool all);

        struct Dead
        {
            uint64_t frame; ///< released once this frame has completed
            resources::Buffer buffer;
            resources::Image  image;
            VkImageView       flat;
            uint32_t          slot;
        };
        std::vector<Dead>     dead_;

        device::Context*      context_ = nullptr;
        resources::Allocator* allocator_ = nullptr;
        present::Display*     display_ = nullptr;
        Formats               formats_;
        resources::Bindless   bindless_;
        resources::Ring       staging_;
        resources::Ring       data_;
        pipelines::Pipelines  pipelines_;
        resources::Image      black_;
        resources::Buffer     zeros_;   ///< what a vertex input the vertex lacks reads
        bool                  black_ready_ = false;
        bool                  color_rgba_ = false; ///< no B8G8R8A8 vertex input: colours read in RGBA order
        std::vector<Texture*> opened_;  ///< open in the frame's upload pass

        // What the current rendering has bound.
        uint64_t   epoch_ = 0;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkBuffer   vertex_buffers_[16] = {};
        VkDeviceSize vertex_offsets_[16] = {};
        VkBuffer   index_buffer_ = VK_NULL_HANDLE;
        float      viewport_[6] = {};
        int32_t    scissor_[4] = {};
        uint32_t   dynamic_[8] = {}; ///< cull, topology, depth test, depth write, compare, bias enable, bias bits, blend factor bits
        DrawData   last_data_ = {};
        VkDeviceAddress last_address_ = 0;
        // The last vertex layout seen, and its id.
        uint32_t   last_attribute_count_ = UINT32_MAX;
        Attribute  last_attributes_[14] = {};
        uint32_t   last_strides_[14] = {};
        uint32_t   last_stream_count_ = 0;
        uint16_t   last_layout_ = 0;
        int32_t    last_inputs_ = 0;
        Stats      stats_;
    };
}
