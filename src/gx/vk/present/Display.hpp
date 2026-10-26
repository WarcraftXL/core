// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A frame from its first command to the image shown. The engine's back buffer is the scene, an
// image of the renderer's own (its colour and its depth) at the size the engine renders at; the
// frame clears it and draws into it, then the present pass draws it onto the swapchain's image and
// the frame is submitted and presented. The window's states (resized, minimised, its surface lost,
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
        void SetSync(Sync sync);
        /// Frames shown at least @p interval_ns apart (0: no limit).
        void SetInterval(uint64_t interval_ns) { limiter_.SetInterval(interval_ns); }
        /// The cursor drawn by the present pass this frame (the software cursor).
        void SetCursor(const CursorDraw& cursor) { cursor_ = cursor; }
        Pass& PresentPass() { return pass_; }

        /// GPU timestamps of each frame to @p sink (null stops them); false when the queue has none.
        bool SetTimestamps(TimestampSink* sink);
        /// One GPU timestamp now, waited for within the bound: what a profiler aligns its clocks with.
        bool SampleGpuTime(int64_t& ticks);
        /// Nanoseconds per GPU tick.
        float TimestampPeriod() const { return context_ ? context_->Properties().limits.timestampPeriod : 1.0f; }

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
        void            EndRendering();
        /// Whether a rendering is open in the frame's command buffer.
        bool            InRendering() const { return rendering_ != Rendering::None; }
        /// Whether the open rendering draws into a target, and its key; 0 for the scene or none.
        uint64_t        TargetKey() const { return rendering_ == Rendering::Target ? target_key_ : 0; }
        /// The size of the open rendering's images (the scene's when none is open).
        VkExtent2D      RenderingExtent() const { return rendering_ == Rendering::Target ? target_extent_ : scene_extent_; }
        /// The scene made (at the size wanted) and in COLOR_ATTACHMENT_OPTIMAL for this frame,
        /// outside any rendering: what a copy from it starts from. False outside a frame.
        bool            PrepareScene();
        const resources::Image& SceneImage() const { return scene_; }
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
        VkFormat   SceneFormat() const { return kSceneFormat; }
        VkFormat   DepthFormat() const { return depth_format_; }
        const Swapchain& Chain() const { return swapchain_; }
        const Stats& Counters() const { return stats_; }
        uint64_t   LastSerial() const { return last_serial_; }
        size_t     Pending() const { return graveyard_.Size(); }

        /// The offscreen image's pixels (B8G8R8A8) after the last frame, waited for within the bound.
        bool ReadOffscreen(std::vector<uint32_t>& pixels);

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

        resources::Image  scene_;
        resources::Image  depth_;
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

        CursorDraw cursor_;
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
    };
}
