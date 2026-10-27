// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The engine's draws turned into Vulkan: what CGxDevice's work becomes once the engine's state has
// been read (gxfront/ reads it, from the live engine or from a frame capture). This module knows
// the engine's numbers (its texture formats, pool usages, primitives, blend modes, texture ops)
// but none of its objects: it builds and tests natively, on lavapipe.
//
//   Texture   a CGxTex on the device: its image, its bindless slots (a 2D view, and a cube's own),
//             its uploads (Lock/Unlock), its views as a render target
//   Pool      a CGxPool on the device: a stream pool's mapped buffers, renamed when the engine
//             discards it; a static or dynamic pool's CPU copy and device buffer, the ranges the
//             engine wrote copied over before the first draw that reads them
//   Packet    one draw, every state it depends on resolved
//   Renderer  the programs (its own, and the client's translated from their DX9 records through a
//             cache kept on disk), the engine's register files (copied into the frame as the bound
//             programs read them), the render target (the scene, or a texture with its depth),
//             clears, copies from the scene, occlusion queries; records packets into the open frame
//             of a present::Display: the pipelines (made at first use), the bindless table, the
//             frame's rings, the uploads (in the frame's upload pass when nothing drew with the
//             resource yet this frame, inline otherwise: the rendering is ended, copied into,
//             resumed)
//
// Main thread only (the translation cache's worker aside).

#pragma once

#include "gx/vk/draw/Formats.hpp"
#include "gx/vk/pipelines/Pipelines.hpp"
#include "gx/vk/present/Display.hpp"
#include "gx/vk/resources/Bindless.hpp"
#include "gx/vk/resources/Resources.hpp"
#include "gx/vk/resources/Ring.hpp"
#include "gx/vk/shaders/Cache.hpp"
#include "gx/vk/shaders/Interface.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
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

    /// EGxPoolTarget (the recompiled client's GxPoolTarget_Index32 included), EGxPoolUsage.
    enum PoolTarget : uint32_t
    {
        kVertexPool  = 0,
        kIndexPool   = 1, ///< 16-bit indices
        kIndex32Pool = 2, ///< 32-bit indices
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
    constexpr uint32_t kStages     = shaders::kStages;

    /// The data block of a draw the renderer's own programs read (shaders/glsl/engine.glsl, DrawData).
    struct DrawData
    {
        float   worldView[16];
        float   projection[16];
        float   texMatrix[2][16];
        float   constants[4][4];
        float   fog[4];
        int32_t ops[4];
        int32_t texgen[4];
        int32_t inputs[4];
        float   lightPosition[4][4];
        float   lightDiffuse[4][4];
        float   lightAmbient[4][4];
        float   lightSpecular[4][4];
        float   lightAttenuation[4][4];
        float   matDiffuse[4];
        float   matAmbient[4];
        float   matEmissive[4];
        float   matSpecular[4];
        float   globalAmbient[4];
        int32_t lighting[4];
        int32_t sources[4];
    };
    static_assert(sizeof(DrawData) == 816, "DrawData is the shaders' block, std430");

    // --- Resources ---------------------------------------------------------------------------------

    struct TextureDesc
    {
        uint32_t format = kTexArgb8888; ///< EGxTexFormat
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t levels = 1;
        bool     cube = false;
        bool     target = false;        ///< a render target
        bool     generate_mips = false; ///< the engine's AUTOGENMIPMAP: levels made from the first
    };

    struct Texture
    {
        TextureDesc         desc;
        FormatInfo          info;
        resources::Image    image;
        VkImageView         flat = VK_NULL_HANDLE; ///< a cube's first face as a 2D view, what a 2D sampler reads
        VkImageView         attach[6] = {};        ///< a target's level 0 (each face of a cube), as an attachment
        uint32_t            slot = resources::Bindless::kNoSlot;      ///< sampled as 2D
        uint32_t            cube_slot = resources::Bindless::kNoSlot; ///< a cube sampled as a cube
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
        uint16_t  vertex = 0;        ///< a vertex program's id (pipelines::VertexProgram for the own ones)
        uint16_t  pixel = 0;         ///< a pixel program's id

        uint32_t  primitive = kTriangles;
        bool      indexed = false;
        Pool*     indices = nullptr;
        uint32_t  first_index = 0;   ///< indexed: into the index pool, in its indices (16 or 32 bits)
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
        /// GxRs_Multisample 0 (D3D9's MULTISAMPLEANTIALIAS off): in a multisampled scene, every
        /// sample of a pixel takes what its centre takes (coverage, depth, colour).
        bool     multisample_off = false;

        float    viewport[6] = {};   ///< x, y, width, height in pixels (D3D9's), min and max depth
        int32_t  scissor[4] = {};    ///< x, y, width, height in pixels

        Texture*              textures[kStages] = {};
        resources::SamplerKey samplers[kStages];

        uint32_t flags = 0;          ///< shaders::kFlagFog, kFlagPointSprite, kFlagClip
        float    clip_plane[4] = {}; ///< against D3D9's clip-space position
        uint32_t fog_color = 0;      ///< 0xAARRGGBB
        float    point_size = 1.0f;
        float    point_range[2] = {0.0f, 64.0f}; ///< D3D9's POINTSIZE_MIN and POINTSIZE_MAX

        DrawData data = {};
    };

    /**
     * One draw as the renderer recorded it: what a mod draws again with a fragment program of its
     * own behind the draw's vertex program (wxl.render's batches). Everything it names lives until
     * the frame it was drawn in has completed: the buffers (a stream pool renamed since is retired
     * by frame), the registers and the draw's data (the frame's ring), the textures' slots.
     */
    struct Batch
    {
        uint64_t        frame = 0;      ///< the frame it was drawn in
        uint16_t        vertex = 0;     ///< its vertex program's id
        uint16_t        layout = 0;     ///< its vertex layout's id (pipelines::Pipelines::LayoutId)
        uint32_t        primitive = kTriangles;
        bool            indexed = false;
        bool            scene = false;  ///< drawn into the scene (not a texture)
        uint32_t        first_index = 0;
        uint32_t        count = 0;
        VkBuffer        index = VK_NULL_HANDLE;
        bool            index32 = false;        ///< its indices are 32-bit (else 16-bit)
        uint32_t        stream_count = 0;
        VkBuffer        buffers[14] = {};
        VkDeviceSize    offsets[14] = {};
        float           viewport[6] = {}; ///< D3D9's: x, y, width, height in the scene's pixels, min and max depth
        VkCullModeFlags cull = VK_CULL_MODE_NONE;
        float           depth_bias = 0.0f;
        shaders::Push   push = {};      ///< as the draw pushed it
    };

    /// The renderer's own program for a client shader, by the name its record has ("UI:0", "UI:1",
    /// "UI", "Desaturate"); false for the others, which are translated.
    bool ProgramByName(bool vertex, const char* name, uint16_t& program);

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
            uint64_t textures_made = 0; ///< made, over the device's life (those made again after a recreation counted again)
            uint64_t textures_peak = 0; ///< the most alive at once
            uint64_t pools = 0;        ///< alive
            uint64_t refused = 0;      ///< draws dropped for want of memory, a pipeline or a target
            uint64_t programs = 0;     ///< client programs translated and made (each record once)
            uint64_t programs_refused = 0; ///< client records the translator refused
            uint64_t register_bytes = 0;   ///< register rows copied into the frames
            uint64_t target_renderings = 0; ///< renderings begun into a texture
            uint64_t copies = 0;       ///< copies from the scene into a texture
            uint64_t queries = 0;      ///< occlusion queries issued
            uint64_t skipped = 0;      ///< draws left out while their pipeline compiled on a worker
            uint64_t centred = 0;      ///< draws with multisampling off, their samples on the pixel's centre
            uint64_t antialiased = 0;  ///< draws with multisampling off drawn multisampled (the device has no sample locations)
            uint64_t direct_bytes = 0; ///< pool bytes written straight into mapped device memory (no staging, no copy)
            uint64_t staged_bytes = 0; ///< pool bytes copied through the staging ring
            uint64_t ahead = 0;        ///< programs of earlier launches made from the shader pack before the engine asked
            double   ahead_ms = 0.0;
        };

        /// Where the renderer keeps what outlives a launch (empty: nothing is kept), and how it makes
        /// its pipelines.
        struct Paths
        {
            std::string pipeline_cache;  ///< the VkPipelineCache's file
            std::string shader_pack;     ///< the translated programs' pack (shaders::Cache)
            std::string pipeline_keys;   ///< the pipelines drawn, compiled ahead at the next launch
            bool        async_pipelines = false; ///< a missing pipeline is compiled on a worker, its draws into the scene left out meanwhile
            bool        pipeline_libraries = true; ///< linked from parts where the device has them
            /// The programs and pipelines the earlier launches drew (the keys file, the shader pack)
            /// made at the renderer's start, before the engine binds them ([gx] vk_prewarm).
            bool        prewarm_known = true;
            /// The static and dynamic pools in mapped device memory where the device has a large
            /// BAR heap ([gx] vk_rebar, 64-bit only: a mapped pool costs address space), written in
            /// place when no frame in flight reads them.
            bool        direct_pools = false;
        };

        Renderer() = default;
        Renderer(const Renderer&)            = delete;
        Renderer& operator=(const Renderer&) = delete;
        ~Renderer() { Destroy(); }

        /// Draws into @p display's scene (its colour and depth formats) and into textures.
        /// Registers itself as the display's frame hook.
        bool Create(device::Context& context, resources::Allocator& allocator, present::Display& display, const Paths& paths, char* why,
                    uint32_t why_size);
        /// Its textures and pools must be destroyed first (or the device lost); waits for nothing.
        void Destroy();
        bool Ready() const { return pipelines_.Ready(); }

        // --- Programs ---------------------------------------------------------------------------
        /**
         * @brief The program of a client shader: one of the renderer's own by @p name, or its DX9
         *        record @p code (@p size bytes) translated (once per record, through the cache).
         * @return its id, or -1 with @p why when the record cannot be translated.
         */
        int  Program(bool pixel, const char* name, const void* code, size_t size, std::string* why = nullptr);
        /**
         * @brief A program of the caller's SPIR-V (@p words) in the place of program @p base of the
         *        same stage: it reads what @p base reads (its inputs, its registers, its stages).
         *        One per (@p base, @p identity), made once; -1 when its module cannot be made.
         */
        int  Replacement(bool pixel, int base, const std::vector<uint32_t>& words, uint64_t identity);
        /// The records of a shader's permutations, translated ahead on the cache's worker.
        void Prewarm(std::vector<std::vector<uint8_t>> records);
        shaders::Cache& ShaderCache() { return cache_; }

        // --- The engine's registers --------------------------------------------------------------
        /// Rows @p first.. of the vertex (@p pixel false) or pixel register file changed.
        void Constants(bool pixel, uint32_t first, uint32_t rows, const float* values);
        const float* Registers(bool pixel) const { return pixel ? pixel_regs_.rows : vertex_regs_.rows; }
        /**
         * @brief The engine's bone palette (GxBonePaletteSet): @p count bones (at most
         *        shaders::kPaletteBones), each a C34Matrix of 12 floats (a0 a1 a2 b0 b1 b2 c0 c1 c2 d0
         *        d1 d2), what the skinned vertex programs read from now on in the place of the
         *        registers from c31: bone i's rows (a0 b0 c0 d0), (a1 b1 c1 d1), (a2 b2 c2 d2). It stays
         *        until the next one, from frame to frame, as the registers do.
         */
        void BonePalette(const float* bones, uint32_t count);
        /// The bones of the palette the next draws read; 0 before the engine sets one.
        uint32_t PaletteBones() const { return palette_.bones; }

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

        // --- Where draws go ------------------------------------------------------------------------
        /// The engine's render target: @p color's level 0 (its face @p face when a cube) and @p depth,
        /// or the scene when both are null. A colour target without a depth texture draws with a
        /// depth of the renderer's own, as D3D9 makes one for it.
        void SetTarget(Texture* color, uint32_t face, Texture* depth);
        bool TargetIsScene() const { return !target_.color && !target_.depth; }
        /// The size of what draws go into now.
        VkExtent2D TargetExtent() const;
        /// Clears the target inside @p rect (pixels of the target): its colour to @p rgba when
        /// @p color, its depth to @p z and its stencil to 0 when @p depth.
        void Clear(bool color, bool depth, const float rgba[4], float z, VkRect2D rect);
        /**
         * @brief D3D9's StretchRect from the back buffer: the scene's @p from rectangle into @p to of
         *        @p dest's @p level (@p face of a cube), filtered linearly when @p linear.
         * @return false when it could not be (outside a frame, no such level).
         */
        bool CopyScene(Texture* dest, uint32_t level, uint32_t face, VkRect2D from, VkRect2D to, bool linear);

        // --- Occlusion queries -----------------------------------------------------------------------
        /// Whether the device counts samples (occlusionQueryPrecise).
        bool QueriesSupported() const { return queries_supported_; }
        uint32_t CreateQuery();
        void     DestroyQuery(uint32_t query);
        /// The samples of the draws between Begin and End are counted.
        bool     BeginQuery(uint32_t query);
        bool     EndQuery(uint32_t query);
        /// Whether the query's count is known (true for one never issued, its count 0), and the count.
        bool     QueryResult(uint32_t query, uint32_t& samples);

        // --- Draws ------------------------------------------------------------------------------
        /// Records @p packet into the open frame; false when it was not (no frame, no memory, no pipeline).
        bool Draw(const Packet& packet);

        // --- What draws beside the engine's ------------------------------------------------------
        /// Each draw recorded from now on is kept as LastBatch() (a copy per draw: only while asked).
        void KeepBatches(bool on) { keep_batches_ = on; }
        bool KeepingBatches() const { return keep_batches_; }
        /// The last draw kept, or null when the last draw asked for was not drawn (or none is kept).
        const Batch* LastBatch() const { return last_valid_ ? &last_ : nullptr; }
        /// The next draw is a new one: until it is kept, LastBatch() answers null.
        void ForgetLastBatch() { last_valid_ = false; }
        /// @p size bytes in the frame's ring, aligned to 16, read by the GPU through @p address
        /// until the frame completes; false without memory or outside a frame.
        bool FrameData(const void* bytes, uint32_t size, VkDeviceAddress& address);
        /// @p size bytes of the frame's staging ring (copied from by the frame's upload pass or
        /// later commands of the frame); false without memory.
        bool Stage(VkDeviceSize size, resources::Ring::Span& span) { return staging_.Allocate(size, 16, span); }
        /// What the renderer bound in the open rendering is bound again before its next draw (a
        /// recorder in between changed it).
        void Invalidate() { epoch_ = ~0ull; }
        /// The buffer of zeros the vertex layouts read an input the vertex lacks from, bound at
        /// kZeroBinding by any recorder that draws with them.
        VkBuffer ZeroBuffer() const { return zeros_.buffer; }
        static constexpr uint32_t kZeroBinding = 15;

        const Stats&            Counters() const { return stats_; }
        /// The staging ring's and the draw data ring's chunks: mapped memory, address space on x86.
        const resources::Ring::Stats& StagingCounters() const { return staging_.Counters(); }
        const resources::Ring::Stats& DataCounters() const { return data_.Counters(); }
        pipelines::Pipelines&   PipelineCache() { return pipelines_; }
        resources::Bindless&    Table() { return bindless_; }
        const Formats&          TextureFormats() const { return formats_; }
        present::Display&       Screen() { return *display_; }

        // present::FrameHook
        void FrameBegun(uint32_t slot, VkCommandBuffer upload) override;
        void FrameEnding(VkCommandBuffer upload) override;
        void FrameFlushing(VkCommandBuffer upload) override;
        void RenderingEnding(VkCommandBuffer cmd) override;
        void RenderingEnded(VkCommandBuffer cmd) override;

    private:
        struct RegisterFile
        {
            float           rows[shaders::kRegisterRows * 4] = {};
            bool            dirty = true;
            VkDeviceAddress address = 0;
            uint32_t        copied = 0; ///< rows in the copy at address
            uint64_t        frame = 0;  ///< the frame the copy is in
        };
        struct Target
        {
            Texture* color = nullptr;
            uint32_t face = 0;
            Texture* depth = nullptr;
        };
        struct Query
        {
            enum State : uint8_t
            {
                Idle,
                Pending, ///< begun by the engine, waiting for a draw inside a rendering
                Running,
                Issued,
            } state = Idle;
            bool     alive = false;
            uint32_t slot = 0;
            uint32_t index = 0;
            uint64_t frame = 0;
            uint64_t epoch = 0;
            uint32_t samples = 0;
        };

        bool EnsureDevice(Pool* pool);
        /// The programs and pipelines of earlier launches made now (Paths::prewarm_known).
        void PrepareKnown();
        bool FlushPool(Pool* pool);
        void OpenInUpload(Texture* texture);
        void CopyIntoImage(Texture* texture, VkCommandBuffer cmd, const resources::Ring::Span& span, uint32_t pitch);
        void GenerateMips(Texture* texture, VkCommandBuffer cmd);
        void Retire(resources::Buffer buffer);
        void Retire(resources::Image image, VkImageView flat, uint32_t slot, uint32_t cube_slot, const VkImageView* attach);
        VkCommandBuffer Inline();
        uint16_t LayoutOf(const Packet& packet, uint32_t needed, uint32_t& carried);
        void Collect(bool all);
        /// What the frame's upload pass holds open goes to SHADER_READ_ONLY; the rings' writes reach the GPU.
        void CloseUploads(VkCommandBuffer upload);
        VkDeviceAddress Snapshot(RegisterFile& file, uint32_t rows);
        /// The palette's copy in this frame's ring (made when the palette changed or the frame is new).
        VkDeviceAddress SnapshotPalette();
        /// The rendering draws go into now, begun when it is not open; its pipeline target id.
        VkCommandBuffer Rendering(uint8_t& target_id);
        VkImageView AttachView(Texture* texture, uint32_t face);
        bool EnsureTargetDepth(VkExtent2D extent);
        void EndQueries(VkCommandBuffer cmd);

        struct Dead
        {
            uint64_t frame; ///< released once this frame has completed
            resources::Buffer buffer;
            resources::Image  image;
            VkImageView       views[7];
            uint32_t          slots[2];
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
        shaders::Cache        cache_;
        std::unordered_map<uint64_t, int> programs_; ///< a translated record's id (or -1), by its cache key
        resources::Image      black_;
        resources::Image      black_cube_;
        uint32_t              black_cube_slot_ = 0;
        resources::Buffer     zeros_;   ///< what a vertex input the vertex lacks reads
        bool                  black_ready_ = false;
        bool                  color_rgba_ = false; ///< no B8G8R8A8 vertex input: colours read in RGBA order
        bool                  direct_pools_ = false; ///< Paths::direct_pools, and the device has the memory
        bool                  queries_pending_ = false; ///< a query begun by the engine waits for a draw
        std::vector<Texture*> opened_;  ///< open in the frame's upload pass

        RegisterFile          vertex_regs_;
        RegisterFile          pixel_regs_;
        struct Palette
        {
            float           rows[shaders::kPaletteRows * 4] = {};
            uint32_t        bones = 0;
            bool            dirty = true;
            VkDeviceAddress address = 0;
            uint64_t        frame = 0; ///< the frame the copy is in
        };
        Palette               palette_;

        Target                target_;       ///< what the engine draws into
        Target                open_;         ///< what the open target rendering draws into (color null: none)
        resources::Image      target_depth_; ///< the depth of a colour target without one
        bool                  target_depth_ready_ = false;

        bool                  queries_supported_ = false;
        VkQueryPool           query_pool_ = VK_NULL_HANDLE;
        static constexpr uint32_t kQueriesPerFrame = 64;
        uint32_t              query_next_ = 0; ///< the next free query of the frame's slot
        uint32_t              slot_ = 0;
        std::vector<Query>    queries_;

        // What the current rendering has bound.
        uint64_t   epoch_ = 0;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkBuffer   vertex_buffers_[16] = {};
        VkDeviceSize vertex_offsets_[16] = {};
        VkBuffer   index_buffer_ = VK_NULL_HANDLE;
        bool       index32_ = false; ///< the index type index_buffer_ is bound with
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
        uint32_t   last_needed_ = 0;
        uint16_t   last_layout_ = 0;
        uint32_t   last_carried_ = 0;
        Stats      stats_;
        bool       keep_batches_ = false;
        bool       last_valid_ = false;
        Batch      last_;
    };
}
