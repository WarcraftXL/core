// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A frame from its first command to the image shown. The engine's back buffer is the scene, an
// image of the renderer's own (its colour and its depth) at the size the engine renders at, with
// as many samples per pixel as the engine's format asks (gxMultisample, D3D9's multisampled back
// buffer); the frame clears it and draws into it, then the present pass draws it (resolved, when
// multisampled) onto the swapchain's image and the frame is submitted and presented. A copy or a
// readback of the scene reads it resolved, as D3D9's StretchRect from a multisampled back buffer
// does; a readback submits the frame so far and waits for it, then the frame goes on. The window's states (resized, minimised, its surface lost,
// out of date) are absorbed here as rebuilds; only a lost device reaches the caller as a loss.
//
// Without a surface the frame is drawn and submitted but not shown; with an offscreen target (the
// tests) the present pass draws into an image read back to the CPU.
//
// Everything here runs on the thread that owns the device (the engine's main thread). Waits are
// bounded: a frame slot that does not come back within the bound marks the device lost.

#pragma once

#include "gx/vk/device/Device.hpp"
#include "gx/vk/frame/Frame.hpp"
#include "gx/vk/present/Limiter.hpp"
#include "gx/vk/present/Pass.hpp"
#include "gx/vk/present/Swapchain.hpp"
#include "gx/vk/resources/Resources.hpp"
#include "gx/vk/Stats.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace wxl::vk::present
{
    /// Makes the surface frames are shown on; null with @p why when it cannot.
    using SurfaceMaker = VkSurfaceKHR (*)(VkInstance instance, void* user, char* why, uint32_t why_size);

    /// Receives the frame's GPU timestamps: a query is begun and ended as the frame is recorded,
    /// and its time (in device ticks) comes once the frame has completed.
    class TimestampSink
    {
    public:
        virtual void Begin(uint16_t query)              = 0;
        virtual void End(uint16_t query)                = 0;
        virtual void Time(uint16_t query, int64_t ticks) = 0;

    protected:
        ~TimestampSink() = default;
    };

    /// Receives each timed pass of a frame once the frame has completed: its name and its first
    /// and last GPU timestamps (device ticks).
    class PassSink
    {
    public:
        virtual void Zone(const char* name, int64_t begin, int64_t end) = 0;

    protected:
        ~PassSink() = default;
    };

    /// The GPU time of one kind of pass, summed over each frame (milliseconds per frame).
    struct PassTime
    {
        const char*         name = nullptr;
        stats::Distribution per_frame;
    };

    /// What records into a frame besides the display: told when a frame's command buffers begin and
    /// before they are submitted, with the frame's upload command buffer (draw::Renderer).
    class FrameHook
    {
    public:
        virtual void FrameBegun(uint32_t slot, VkCommandBuffer upload) = 0;
        virtual void FrameEnding(VkCommandBuffer upload)               = 0;
        /// A rendering of the frame is about to end (its queries end inside it), then has ended.
        virtual void RenderingEnding(VkCommandBuffer cmd) { (void)cmd; }
        virtual void RenderingEnded(VkCommandBuffer cmd) { (void)cmd; }
        /// The frame so far is about to be submitted in the middle of the frame (a readback): what
        /// the upload pass holds open is closed, as before the frame's own submission; the frame
        /// goes on after, in the same slot, its upload pass empty again.
        virtual void FrameFlushing(VkCommandBuffer upload) { (void)upload; }

    protected:
        ~FrameHook() = default;
    };

    enum class FrameEnd
    {
        Presented, ///< shown
        Drawn,     ///< submitted, not shown: no surface, the window minimised, or an offscreen target
        Dropped,   ///< submitted, not shown: no image this time (out of date, timed out, surface lost)
        Lost,      ///< the device was lost
    };

    const char* FrameEndName(FrameEnd end);

    /// A rendering into images other than the scene (a texture the engine draws into).
    struct TargetRendering
    {
        VkImageView color = VK_NULL_HANDLE;
        VkImageView depth = VK_NULL_HANDLE;
        VkFormat    depth_format = VK_FORMAT_UNDEFINED;
        VkExtent2D  extent{0, 0};
        uint64_t    key = 0; ///< the caller's identity of the target: the open rendering is kept while it is the same
    };

    class Display
    {
    public:
        struct Options
        {
            uint32_t frames = 2;               ///< frames in flight, 1 to 3
            Filter   filter = Filter::Linear;  ///< how the scene is stretched to the image shown
        };

        struct Stats
        {
            uint64_t frames = 0;
            uint64_t presented = 0;
            uint64_t dropped = 0;
            uint64_t rebuilds = 0;
            uint64_t surfaces_lost = 0;
            uint64_t acquire_timeouts = 0;
            uint64_t scenes = 0; ///< scene images made (the first, and each resize)
            uint64_t resolves = 0; ///< multisampled scenes resolved (for a copy, a readback, the image shown)
            uint64_t flushes = 0;  ///< frames submitted and waited for in their middle (readbacks)
            uint64_t readbacks = 0; ///< rectangles of the scene read back to the CPU
        };

        Display() = default;
        Display(const Display&)            = delete;
        Display& operator=(const Display&) = delete;
        ~Display() { Destroy(); }

        bool Create(device::Context& context, resources::Allocator& allocator, frame::Timeline& timeline,
                    const Options& options, char* why, uint32_t why_size);

        /// Waits (bounded) for the last frame unless the device is lost, then releases everything.
        void Destroy();
        bool Ready() const { return frames_.Ready(); }

        // --- Where frames go ----------------------------------------------------------------------
        /// Frames are shown on the surface @p make makes (now; again if the surface is lost); null
        /// shows them nowhere. The previous surface and its swapchain are retired.
        bool SetSurface(SurfaceMaker make, void* user, char* why, uint32_t why_size);

        /// Frames are drawn into an offscreen image of @p extent, read back by ReadOffscreen. For tests.
        bool SetOffscreen(VkExtent2D extent);

        /// The window's client size; zero while it is minimised (nothing is shown then).
        void SetWindowExtent(VkExtent2D extent);
        /// The engine's back buffer size: the scene is made again at the next use when it changes.
        void SetSceneExtent(VkExtent2D extent);
        /// The engine's samples per pixel (its format's multisample count): the scene is made again at
        /// the next use with the most the device draws colour and depth with, at most @p count.
        void SetSamples(uint32_t count);
        /// The scene's samples per pixel (1 when not multisampled).
        uint32_t Samples() const { return static_cast<uint32_t>(samples_); }
        /// Whether a draw into the multisampled scene may put its samples on the pixel's centre
        /// (the device has the sample locations, the scene's depth was made for them).
        bool CentreSamples() const { return centre_samples_; }
        /// The sample counts the device draws the scene's colour and depth with (VkSampleCountFlags).
        uint32_t SampleCounts() const;
        void SetSync(Sync sync);
        /// Frames shown at least @p interval_ns apart (0: no limit).
        void SetInterval(uint64_t interval_ns) { limiter_.SetInterval(interval_ns); }
        Pass& PresentPass() { return pass_; }

        /// GPU timestamps of each frame to @p sink (null stops them); false when the queue has none.
        bool SetTimestamps(TimestampSink* sink);
        /// One GPU timestamp now, waited for within the bound: what a profiler aligns its clocks with.
        bool SampleGpuTime(int64_t& ticks);
        /// Nanoseconds per GPU tick.
        float TimestampPeriod() const { return context_ ? context_->Properties().limits.timestampPeriod : 1.0f; }

        // --- The GPU time of each pass ------------------------------------------------------------
        /**
         * @brief Times the frame's passes on the GPU from the next frame: its upload pass, each
         *        rendering (the scene, a texture target, a pass of the caller's own), the resolve,
         *        the present pass, and what the caller names with PassBegin. A handful of
         *        timestamps a frame, read once the frame has completed (nothing waits).
         * @return false when the queue has no timestamps (nothing is timed then).
         */
        bool SetPassTiming(bool on);
        bool PassTiming() const { return pass_on_; }
        /// A pass of the caller's named @p name (a string that lives as long as the process) begins
        /// in the frame's command buffer; its handle, or kNoPass when nothing is timed.
        uint32_t PassBegin(const char* name);
        void     PassEnd(uint32_t pass);
        static constexpr uint32_t kNoPass = ~0u;
        /// Each pass's milliseconds per frame, over the frames timed so far, and since the segment began.
        const std::vector<PassTime>& PassTimes() const { return pass_times_; }
        const std::vector<PassTime>& PassSegment() const { return pass_segment_; }
        void                         NewPassSegment()
        {
            pass_segment_.clear();
            pass_segment_frames_ = 0;
        }
        /// The frames whose passes were read back (the session's, the segment's).
        uint64_t PassFrames() const { return pass_frames_; }
        uint64_t PassSegmentFrames() const { return pass_segment_frames_; }
        /// Each pass to @p sink as well, once its frame has completed (null stops it).
        void SetPassSink(PassSink* sink) { pass_sink_ = sink; }

        // --- A frame ------------------------------------------------------------------------------
        /// Opens a frame: waits (bounded) for its slot, frees what completed. False with @p wait
        /// saying why (Timeout marks the device lost).
        bool BeginFrame(frame::Wait& wait);
        bool InFrame() const { return frames_.Open(); }
        VkCommandBuffer Cmd() const { return frames_.Current(); }
        /// The open frame's upload command buffer (submitted before Cmd()); null outside a frame.
        VkCommandBuffer Upload() const { return frames_.Upload(); }
        uint32_t Slot() const { return frames_.Index(); }
        uint32_t FrameCount() const { return frames_.Count(); }

        /// The frame's hook (null for none).
        void SetFrameHook(FrameHook* hook) { hook_ = hook; }

        /// The number of the open frame (or of the last one): 1 for the first frame begun, then one more each.
        uint64_t FrameNumber() const { return stats_.frames; }
        /// The last frame number whose submission has completed on the GPU (or was never submitted).
        uint64_t FramesCompleted();

        /**
         * @brief Clears the scene's colour (when @p color, to @p rgba) and its depth (when @p depth,
         *        to @p z, the stencil to 0) inside @p rect, in scene pixels, clamped to the scene.
         */
        void Clear(bool color, bool depth, const float rgba[4], float z, VkRect2D rect);

        /// A rendering into the scene's colour alone (its contents kept), for an overlay; ended by
        /// EndRendering or by whatever comes next. Null outside a frame.
        VkCommandBuffer BeginColorRendering();
        /// A rendering into the scene's colour and depth (their contents kept), the engine's draws;
        /// the one already open when it is. Null outside a frame.
        VkCommandBuffer BeginSceneRendering();
        /// A rendering into @p target (its images in their attachment layouts already, contents
        /// kept); the one already open when it has the same key. Null outside a frame.
        VkCommandBuffer BeginTargetRendering(const TargetRendering& target);
        /**
         * @brief A rendering of the caller's own (@p info: its attachments, already in their
         *        attachment layouts), named by @p key (never 0, never a texture target's), whatever
         *        was open ended first. Null outside a frame. Ended by EndRendering, as any other.
         */
        VkCommandBuffer BeginOwnRendering(const VkRenderingInfo& info, uint64_t key);
        /// The scene's colour was drawn into by a rendering of the caller's own: its resolve is due.
        void SceneDrawn() { if (samples_ > VK_SAMPLE_COUNT_1_BIT) msaa_dirty_ = true; }
        void            EndRendering();
        /// Whether a rendering is open in the frame's command buffer.
        bool            InRendering() const { return rendering_ != Rendering::None; }
        /// Whether the open rendering draws into a target, and its key; 0 for the scene or none.
        uint64_t        TargetKey() const { return rendering_ == Rendering::Target ? target_key_ : 0; }
        /// The size of the open rendering's images (the scene's when none is open).
        VkExtent2D      RenderingExtent() const { return rendering_ == Rendering::Target ? target_extent_ : scene_extent_; }
        /// The scene made (at the size wanted) and in COLOR_ATTACHMENT_OPTIMAL for this frame,
        /// outside any rendering. False outside a frame.
        bool            PrepareScene();
        /**
         * @brief The scene's colour with one sample per pixel, outside any rendering and in
         *        COLOR_ATTACHMENT_OPTIMAL: the scene itself, or its resolved copy when it is
         *        multisampled (resolved now if drawn into since). What a copy or a readback reads.
         *        Null outside a frame.
         */
        const resources::Image* ResolveScene();
        /// The scene's images: its colour (multisampled or not), and the image shown and copied from.
        const resources::Image& SceneImage() const { return scene_; }
        /// The scene's depth (multisampled as its colour), in DEPTH_STENCIL_ATTACHMENT_OPTIMAL during the frame.
        const resources::Image& DepthImage() const { return depth_; }
        const resources::Image& Shown() const { return samples_ > VK_SAMPLE_COUNT_1_BIT ? resolve_ : scene_; }

        /**
         * @brief Submits what the open frame recorded so far and waits for it (bounded), then opens
         *        the same frame again: what D3D9's synchronous readbacks imply. False when the device
         *        did not complete it (a timeout marks it lost).
         */
        bool Flush();

        /**
         * @brief D3D9's readback of the back buffer (GetRenderTargetData, or a StretchRect into a
         *        lockable surface when multisampled): @p rect of the scene, resolved, as B8G8R8A8
         *        rows into @p bgra (rect.extent.width x rect.extent.height pixels), the frame
         *        submitted so far and waited for. False outside a frame or when it could not be read.
         */
        bool ReadScene(VkRect2D rect, uint32_t* bgra);
        /// Grows at each rendering begun: what a recorder bound inside the previous one (its
        /// pipeline, its descriptor set, its dynamic states) is to be set again.
        uint64_t        RenderingEpoch() const { return epoch_; }

        /// Runs @p release once the open frame has completed (at once when there is none).
        void AfterFrame(std::function<void()> release);

        /// Frees now what the completed frames left (a window about to go takes its surface with it).
        void CollectNow();

        /// Draws the scene onto the image shown, submits the frame and presents it.
        FrameEnd EndFrame();

        // --- What it is ---------------------------------------------------------------------------
        VkExtent2D SceneExtent() const { return scene_extent_; }
        /// The size the scene is drawn at from its next use (SetSceneExtent's), the made one's until set.
        VkExtent2D WantedSceneExtent() const { return scene_wanted_.width && scene_wanted_.height ? scene_wanted_ : scene_extent_; }
        VkFormat   SceneFormat() const { return kSceneFormat; }
        VkFormat   DepthFormat() const { return depth_format_; }
        const Swapchain& Chain() const { return swapchain_; }
        const Stats& Counters() const { return stats_; }
        uint64_t   LastSerial() const { return last_serial_; }
        size_t     Pending() const { return graveyard_.Size(); }

        /// The offscreen image's pixels (B8G8R8A8) after the last frame, waited for within the bound.
        bool ReadOffscreen(std::vector<uint32_t>& pixels);
        /// Waits (bounded) until everything submitted so far has completed; false when it did not.
        bool WaitSubmitted();

        static constexpr VkFormat kSceneFormat = VK_FORMAT_B8G8R8A8_UNORM;
        /// How long an acquire may wait for an image before the frame is dropped.
        static constexpr uint64_t kAcquireNs = 1'000'000'000ull;

    private:
        enum class Rendering
        {
            None,
            Full,   ///< colour and depth
            Color,  ///< colour only
            Target, ///< another colour image (and depth)
        };

        bool EnsureScene();
        void Touch();
        void Begin(Rendering kind);
        bool RebuildSwapchain(char* why, uint32_t why_size);
        void DropSurface();
        uint64_t Submit(const frame::Timeline::WaitOn* waits, uint32_t wait_count, const VkSemaphore* signals, uint32_t signal_count);
        void EndTimestamp();
        void Sleep(uint64_t ns);
        uint32_t PassBeginIn(const char* name, VkCommandBuffer cmd);
        void     PassEndIn(uint32_t pass, VkCommandBuffer cmd);
        /// The slot's passes of its previous frame read back; its queries reset in @p upload.
        void     PassFrameBegun(uint32_t slot, VkCommandBuffer upload);

        device::Context*      context_ = nullptr;
        resources::Allocator* allocator_ = nullptr;
        frame::Timeline*      timeline_ = nullptr;
        frame::Frames         frames_;
        resources::Graveyard  graveyard_;
        Pass                  pass_;
        Swapchain             swapchain_;
        Limiter               limiter_;

        SurfaceMaker make_surface_ = nullptr;
        void*        surface_user_ = nullptr;
        VkSurfaceKHR surface_ = VK_NULL_HANDLE;
        bool         rebuild_ = false;
        Sync         sync_ = Sync::On;
        VkExtent2D   window_{0, 0};

        resources::Image  scene_;   ///< the colour drawn into (multisampled when samples_ > 1)
        resources::Image  resolve_; ///< its one-sample copy, when multisampled
        resources::Image  depth_;
        VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
        bool                  centre_samples_ = false; ///< CentreSamples()
        uint32_t          samples_wanted_ = 1;
        bool              msaa_dirty_ = false; ///< drawn into since the last resolve
        resources::Buffer pixels_; ///< what ReadScene copies into
        VkFormat          depth_format_ = VK_FORMAT_UNDEFINED;
        VkExtent2D        scene_extent_{0, 0};
        VkExtent2D        scene_wanted_{0, 0};
        bool              touched_ = false;
        Rendering         rendering_ = Rendering::None;
        uint64_t          target_key_ = 0;
        VkExtent2D        target_extent_{0, 0};

        bool              offscreen_ = false;
        resources::Image  target_;
        resources::Buffer readback_;

        FrameHook* hook_ = nullptr;
        uint64_t   epoch_ = 0;
        struct Submitted
        {
            uint64_t frame;
            uint64_t serial;
        };
        std::vector<Submitted> submitted_;
        uint64_t   completed_frame_ = 0;
        std::vector<std::function<void()>> after_;
        uint64_t   last_serial_ = 0;
        Stats      stats_;

        TimestampSink* sink_ = nullptr;
        VkQueryPool    queries_ = VK_NULL_HANDLE;
        bool           written_[frame::Frames::kMax] = {};

        // The passes timed: per slot, the passes recorded in its frame (two queries each).
        static constexpr uint32_t kPassQueries = 128;
        struct PassRecord
        {
            const char* name;
            uint32_t    query; ///< its first; the second follows
            bool        ended;
        };
        VkQueryPool             pass_pool_ = VK_NULL_HANDLE;
        uint64_t                pass_mask_ = ~0ull; ///< timestampValidBits
        bool                    pass_on_ = false;
        std::vector<PassRecord> pass_records_[frame::Frames::kMax];
        std::vector<PassTime>   pass_times_;
        std::vector<PassTime>   pass_segment_;
        uint64_t                pass_frames_ = 0;
        uint64_t                pass_segment_frames_ = 0;
        PassSink*               pass_sink_ = nullptr;
        uint32_t                upload_pass_ = kNoPass;
        uint32_t                rendering_pass_ = kNoPass;
    };
}
