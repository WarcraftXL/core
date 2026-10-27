// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.render on the device: images, buffers and pipelines made beside the engine's, and passes that
// draw with them into the open frame between the engine's own renderings. What the service
// (src/services/RenderService.cpp) hands mods, and what wxl.highlight is drawn with
// (Highlight.hpp). Portable: no runtime, no Windows; built natively for the tests on lavapipe.
//
//   Image     a render target and a texture: its image (multisampled, with a one-sample image it
//             is resolved into at the end of a pass), its bindless slot; a screen image follows the
//             scene's size, made again when it changes. Between passes every one-sample image is in
//             SHADER_READ_ONLY_OPTIMAL, a multisampled one in its attachment layout.
//   Buffer    device memory read by address (and as 32-bit indices); written through the frame's
//             upload pass, which waits for the frames before it to have read it
//   Pipeline  the mod's SPIR-V and its state; a variant per target (formats, samples) and, for a
//             batch program, per engine vertex program and layout, made on first use; a compute
//             pipeline is one program, dispatched between passes
//   a palette bones (or any rows) of the caller's in the frame's ring, read by address until the
//             frame completes
//   a pass    a rendering of the caller's own (Display::BeginOwnRendering): the engine's rendering
//             ends, the pass draws, the engine's next draw opens its own again
//   a batch   draw::Batch: an engine draw kept by the renderer, drawn again with a batch program
//
// Main thread only. Every resource given back goes once the GPU is done with the frames that used it.

#pragma once

#include "gx/vk/device/Device.hpp"
#include "gx/vk/draw/Draw.hpp"
#include "gx/vk/frame/Frame.hpp"
#include "gx/vk/present/Display.hpp"
#include "gx/vk/resources/Resources.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::vk::render
{
    /// wxl.render's numbers (wxl/services/render.h), repeated so that this module needs no SDK header.
    enum Format : uint32_t
    {
        kRgba8 = 1, kBgra8, kR8, kRg8, kR16f, kRg16f, kRgba16f, kR32f, kRgba32f, kDepth32f,
    };
    enum PipelineKind : uint32_t
    {
        kGraphics   = 0,
        kFullscreen = 1,
        kBatch      = 2,
        kCompute    = 3,
    };
    enum BlendMode : uint32_t
    {
        kBlendNone = 0, kBlendAlpha, kBlendAdd, kBlendPremultiplied, kBlendMultiply,
    };
    enum DepthTest : uint32_t
    {
        kDepthOff = 0, kDepthLess, kDepthLessEqual, kDepthEqual, kDepthGreater, kDepthGreaterEqual, kDepthAlways,
    };
    enum Cull : uint32_t
    {
        kCullNone = 0, kCullBack, kCullFront, kCullBatch,
    };
    enum Topology : uint32_t
    {
        kTriangles = 0, kTriangleStrip, kLines, kPoints,
    };
    enum SamplerFlags : uint32_t
    {
        kSamplerLinear = 1, kSamplerWrap = 2, kSamplerMips = 4,
    };
    constexpr uint32_t kMaxColors  = 4;
    constexpr uint32_t kPushBytes  = 128;
    constexpr uint32_t kBatchBytes = 4096;
    constexpr uint32_t kNoSlot     = 0xFFFFFFFFu;
    /// The bytes of a bone palette a mod hands over at once (BonePalette).
    constexpr uint32_t kPaletteBytes = 16u << 20;

    /// The VkFormat of a Format; UNDEFINED for a number that is none.
    VkFormat FormatOf(uint32_t format);

    /// The runtime's own programs of this module (its full-screen triangle, wxl.highlight's), for
    /// the tests' validation.
    struct OwnProgram
    {
        const char*     name;
        const uint32_t* code;
        size_t          words;
    };
    std::vector<OwnProgram> OwnPrograms();

    struct ImageSpec
    {
        uint32_t width = 0;   ///< a fixed size, when divisor is 0
        uint32_t height = 0;
        uint32_t divisor = 0; ///< a screen image: the scene's size divided by this (rounded up)
        uint32_t format = kRgba8;
        uint32_t samples = 1; ///< 0: the scene's
    };

    struct Image
    {
        ImageSpec             spec;
        bool                  depth = false;
        VkFormat              format = VK_FORMAT_UNDEFINED;
        VkExtent2D            extent{0, 0};
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        resources::Image      drawn;    ///< what a pass draws into (multisampled when samples > 1)
        resources::Image      resolved; ///< its one-sample image, when multisampled
        VkImageView           sampled = VK_NULL_HANDLE; ///< the one-sample image's view a program reads (depth aspect for depth)
        uint32_t              slot = kNoSlot;
        bool                  ready = false;  ///< made and in its layouts between passes
        bool                  queued = false; ///< waits for a frame to be made in
    };

    struct Buffer
    {
        resources::Buffer buffer;
        uint32_t          size = 0;
        bool              cleared = false; ///< its zeros written
    };

    struct PipelineSpec
    {
        uint32_t              kind = kGraphics;
        std::string           name;
        std::vector<uint32_t> vertex;   ///< SPIR-V words (GRAPHICS)
        std::vector<uint32_t> fragment; ///< (GRAPHICS, FULLSCREEN, BATCH)
        std::vector<uint32_t> compute;  ///< (COMPUTE)
        uint32_t              color_count = 1;
        uint32_t              color_formats[kMaxColors] = {kBgra8};
        uint32_t              depth_format = 0;
        uint32_t              samples = 0; ///< 0: the scene's
        uint32_t              blend[kMaxColors] = {};
        uint32_t              depth_test = kDepthOff;
        bool                  depth_write = false;
        uint32_t              cull = kCullNone;
        uint32_t              topology = kTriangles;
    };

    struct Pipeline
    {
        PipelineSpec   spec;
        VkShaderModule vertex = VK_NULL_HANDLE;
        VkShaderModule fragment = VK_NULL_HANDLE;
        VkShaderModule compute = VK_NULL_HANDLE;
        std::unordered_map<uint64_t, VkPipeline> variants; ///< by target, and engine program for a batch; a compute one's at 0
        bool           said = false; ///< a refusal was logged
    };

    /// What a pass draws into: up to four colour attachments and a depth one, each an image of the
    /// caller's or the scene's.
    struct PassSpec
    {
        uint32_t color_count = 0;
        Image*   colors[kMaxColors] = {};
        bool     scene_color[kMaxColors] = {}; ///< colour i is the scene's colour
        Image*   depth = nullptr;
        bool     scene_depth = false;
        uint32_t clear = 0;                    ///< bit i clears colour i; bit 4 the depth
        float    clear_colors[kMaxColors][4] = {};
        float    clear_depth = 1.0f;
    };

    /// GPU zones' times, delivered once the frame they were recorded in has completed.
    class ZoneSink
    {
    public:
        /// A zone of @p name took the GPU from @p begin to @p end (device ticks).
        virtual void Zone(const char* name, int64_t begin, int64_t end) = 0;

    protected:
        ~ZoneSink() = default;
    };

    class Render
    {
    public:
        struct Stats
        {
            uint64_t passes = 0;        ///< passes opened
            uint64_t draws = 0;         ///< the mods' draws recorded, batches included
            uint64_t batches = 0;       ///< of those, batches drawn again
            uint64_t dispatches = 0;    ///< compute dispatches recorded
            uint64_t palettes = 0;      ///< bone palettes written into frames
            uint64_t refused = 0;       ///< draws refused (no pass, a wrong target, no pipeline, a stale batch)
            uint64_t variants = 0;      ///< pipelines made
            uint64_t stalls = 0;        ///< variants made at a draw
            double   stall_ms = 0.0;
            uint64_t scene_copies = 0;  ///< copies of the scene's colour or depth for sampling
            uint64_t uploaded = 0;      ///< bytes written into buffers
            uint64_t images = 0;        ///< alive
            uint64_t buffers = 0;
            uint64_t pipelines = 0;
        };

        Render() = default;
        Render(const Render&)            = delete;
        Render& operator=(const Render&) = delete;
        ~Render() { Destroy(); }

        /// Beside @p renderer, which draws the engine into @p display's frames.
        bool Create(device::Context& context, resources::Allocator& allocator, frame::Timeline& timeline, present::Display& display,
                    draw::Renderer& renderer);
        /// The device goes (it is idle or lost): what was made on it goes now. The images, buffers
        /// and pipelines stay, made again on the next device at their next use (a buffer's
        /// contents are lost: zeros again).
        void Release();
        /// Release, then every image, buffer and pipeline is deleted.
        void Destroy();
        bool Ready() const { return renderer_ != nullptr; }

        // --- Images --------------------------------------------------------------------------------
        /// Null with @p why when the spec is refused or the device cannot make it.
        Image*     CreateImage(const ImageSpec& spec, std::string* why = nullptr);
        void       DestroyImage(Image* image);
        /// Its bindless slot, the image made (again, at the scene's size) when needed; kNoSlot without it.
        uint32_t   Slot(Image* image);
        VkExtent2D Extent(Image* image);

        // --- Buffers -------------------------------------------------------------------------------
        Buffer*         CreateBuffer(uint32_t size);
        void            DestroyBuffer(Buffer* buffer);
        /// The bytes go to the buffer in the next (or the open) frame's upload pass. False past its end.
        bool            WriteBuffer(Buffer* buffer, uint32_t offset, const void* data, uint32_t size);
        VkDeviceAddress Address(const Buffer* buffer) const { return buffer ? buffer->buffer.address : 0; }

        /// The bindless index of the sampler of SamplerFlags @p flags.
        uint32_t Sampler(uint32_t flags);

        // --- Pipelines -----------------------------------------------------------------------------
        /// Its programs made and its first variant compiled (the target its spec names, at the
        /// scene's samples); null with @p why.
        Pipeline* CreatePipeline(const PipelineSpec& spec, std::string& why);
        void      DestroyPipeline(Pipeline* pipeline);

        // --- In a frame ----------------------------------------------------------------------------
        /// The frame began (its upload pass open): what waited for a frame is made, written, cleared.
        void FrameBegun();
        /// Opens a pass; false with @p why outside a frame, with a pass open, or for attachments that
        /// do not fit together.
        bool Begin(const PassSpec& pass, std::string* why = nullptr);
        /// Closes the pass: multisampled attachments resolved, images back to be sampled.
        void End();
        bool InPass() const { return pass_open_; }
        /// The pass's size.
        VkExtent2D PassExtent() const { return pass_extent_; }

        bool Draw(Pipeline* pipeline, uint32_t vertices, uint32_t instances, const void* push, uint32_t size);
        bool DrawIndexed(Pipeline* pipeline, Buffer* indices, uint32_t first, uint32_t count, uint32_t instances, const void* push,
                         uint32_t size);
        bool Fullscreen(Pipeline* pipeline, const void* push, uint32_t size);
        /// @p batch drawn again with a batch pipeline, @p constants at pc.pixelRegs. False for a
        /// batch of another frame, a pipeline of another kind, or without a pass.
        bool DrawBatch(const draw::Batch& batch, Pipeline* pipeline, const void* constants, uint32_t size);
        /**
         * @brief A compute pipeline over @p x x @p y x @p z workgroups, @p push (up to 128 bytes) as
         *        its push constants, outside a pass: the engine's rendering ends for it. What it
         *        writes through buffer addresses is seen by every draw and dispatch after it. False
         *        with @p why outside a frame, inside a pass, or for another kind of pipeline.
         */
        bool Dispatch(Pipeline* pipeline, uint32_t x, uint32_t y, uint32_t z, const void* push, uint32_t size, std::string* why = nullptr);
        /// Whether the device's queue runs compute programs.
        bool Computes() const { return compute_layout_ != VK_NULL_HANDLE; }
        /**
         * @brief @p count matrices of 12 floats (three vec4 rows each, as the caller's programs read
         *        them) written into the open frame: their @p address, read until the frame
         *        completes. False outside a frame, for none, or past kPaletteBytes.
         */
        bool BonePalette(const float* rows, uint32_t count, VkDeviceAddress& address);

        /// The scene's colour (resolved) and depth (its first sample) as drawn so far, copied for
        /// sampling: their slots, kNoSlot outside a frame or inside a pass. One copy per @p epoch
        /// (the caller's point): asked again within it, the same copy.
        uint32_t SceneColorSlot(uint64_t epoch);
        uint32_t SceneDepthSlot(uint64_t epoch);

        // --- GPU zones -------------------------------------------------------------------------------
        /// Whether timestamps can be written (the queue has them).
        bool Timestamps() const { return query_pool_ != VK_NULL_HANDLE; }
        /// Times go to @p sink (null: no zone is recorded).
        void SetZoneSink(ZoneSink* sink) { sink_ = sink; }
        /// Opens a zone named @p name in the frame's commands; false when none can be recorded.
        bool ZoneBegin(const char* name);
        void ZoneEnd();
        /// Zones left open are closed (the end of a handler).
        void CloseZones();

        const Stats&           Counters() const { return stats_; }
        present::Display&      Screen() { return *display_; }
        draw::Renderer&        Engine() { return *renderer_; }
        device::Context&       Context() { return *context_; }

    private:
        struct Variant;
        bool       MakeImage(Image* image, VkExtent2D extent, std::string* why);
        void       DropImage(Image* image);
        void       InitImage(Image* image, VkCommandBuffer cmd);
        bool       EnsureImage(Image* image);
        VkExtent2D WantedExtent(const Image* image) const;
        VkPipeline VariantOf(Pipeline* pipeline, const draw::Batch* batch);
        VkPipeline Make(const Pipeline& pipeline, const draw::Batch* batch, const VkFormat* colors, uint32_t color_count, VkFormat depth,
                        VkSampleCountFlagBits samples) const;
        Pipeline*  CreateCompute(const PipelineSpec& spec, std::string& why);
        VkPipeline MakeCompute(const Pipeline& pipeline) const;
        void       Bury(std::function<void()> release);
        void       Collect();
        bool       PreparePush(Pipeline* pipeline, VkPipeline& bound);
        void       PushOwn(const void* push, uint32_t size);
        void       ReadZones(uint32_t slot);
        uint32_t   CopySceneDepth();
        /// A small id of the target a variant is made for: its formats and samples.
        uint32_t   TargetId(const VkFormat* colors, uint32_t count, VkFormat depth, VkSampleCountFlagBits samples);

        device::Context*      context_ = nullptr;
        resources::Allocator* allocator_ = nullptr;
        frame::Timeline*      timeline_ = nullptr;
        present::Display*     display_ = nullptr;
        draw::Renderer*       renderer_ = nullptr;
        VkShaderModule        fullscreen_ = VK_NULL_HANDLE;
        VkPipelineLayout      compute_layout_ = VK_NULL_HANDLE; ///< the bindless set and 128 bytes of push constants
        bool                  releasing_ = false; ///< Release runs: what is given back goes at once
        std::unordered_map<std::string, uint32_t> targets_;

        std::vector<Image*>    images_;
        std::vector<Buffer*>   buffers_;
        std::vector<Pipeline*> pipelines_;
        struct Write
        {
            Buffer*              buffer;
            uint32_t             offset;
            std::vector<uint8_t> bytes;
        };
        std::vector<Write>     writes_;  ///< waiting for a frame
        struct Dead
        {
            uint64_t              serial;  ///< 0: once the frame it was given back in completes
            uint64_t              frame;
            std::function<void()> release;
        };
        std::vector<Dead>      dead_;

        // The open pass.
        bool             pass_open_ = false;
        PassSpec         pass_;
        VkExtent2D       pass_extent_{0, 0};
        VkFormat         pass_colors_[kMaxColors] = {};
        uint32_t         pass_color_count_ = 0;
        VkFormat         pass_depth_ = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits pass_samples_ = VK_SAMPLE_COUNT_1_BIT;
        uint64_t         pass_key_ = 0;
        VkPipeline       bound_ = VK_NULL_HANDLE;
        VkBuffer         bound_index_ = VK_NULL_HANDLE;
        bool             bound_index32_ = false;

        // Copies of the scene for sampling.
        Image            scene_color_{};
        Image            scene_depth_{};
        uint64_t         color_epoch_ = 0;
        uint64_t         depth_epoch_ = 0;

        // GPU zones.
        static constexpr uint32_t kZonesPerFrame = 64;
        VkQueryPool      query_pool_ = VK_NULL_HANDLE;
        ZoneSink*        sink_ = nullptr;
        struct Zone
        {
            std::string name;
            uint32_t    begin = 0; ///< query indices in the frame slot's range
            uint32_t    end = 0;
            bool        closed = false;
        };
        std::vector<Zone> zones_[frame::Frames::kMax]; ///< by frame slot
        uint32_t          zone_next_ = 0;
        std::vector<size_t> open_zones_;
        uint32_t          slot_ = 0;
        uint64_t          seen_frame_ = 0; ///< the last frame FrameBegun saw: its slot's queries are reset

        Stats            stats_;
    };
}
