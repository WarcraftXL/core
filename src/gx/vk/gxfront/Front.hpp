// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Vulkan device behind one engine device: what CGxDevice's work means for the renderer. It
// reads the engine's device through the generated engine layouts (wxl::engine::CGxDevice and its
// siblings) and nothing else of the engine; the engine's own logic around each call stays with
// the caller (the recompiled client's CGxDeviceVk), as it does in the D3D9 device.
//
// What it does: the device, the window's surface and swapchain, the scene cleared to the engine's
// colour and shown through the present pass (stretched, the gamma ramp in fullscreen, the software
// cursor), vsync and its emulated intervals, the hardware cursor, the loss and its recovery, the
// engine's capabilities; the engine's pools, textures, render targets, programs (its own records
// translated), registers and occlusion queries on the device, its draws read out of its state
// (Engine.hpp) and recorded by the renderer (draw/), its copies from the back buffer. The draws
// Engine.hpp leaves out are counted by reason.
//
// Windows only (the window, the cursor), built into WarcraftXL.dll. Main thread only.

#pragma once

#include "gx/vk/device/Device.hpp"
#include "gx/vk/draw/Draw.hpp"
#include "gx/vk/frame/Frame.hpp"
#include "gx/vk/gxfront/Engine.hpp"
#include "gx/vk/present/Display.hpp"
#include "gx/vk/resources/Resources.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wxl::vk::gxfront
{
    struct Options
    {
        const char*     gpu = nullptr;                    ///< [gx] gpu
        bool            validation = false;               ///< [gx] validation
        uint32_t        frames = 2;                       ///< [gx] frames_in_flight
        present::Filter filter = present::Filter::Linear; ///< [gx] present_filter
        std::string     cache_dir;                        ///< where the shader pack and the pipeline cache live (empty: none)
        bool            async_pipelines = true;           ///< [gx] async_pipelines: a draw into the scene whose pipeline compiles is left out
    };

    class Front
    {
    public:
        struct Stats
        {
            uint64_t draws = 0;     ///< the engine's draws
            uint64_t left_out[static_cast<int>(Verdict::Count)] = {}; ///< by Verdict: [0] the ones drawn, the others left out
            uint64_t world = 0;     ///< of the drawn, those inside the world's pass
            uint64_t clears = 0;
            uint64_t losses = 0;
        };

        Front() = default;
        Front(const Front&)            = delete;
        Front& operator=(const Front&) = delete;
        ~Front() { Destroy(); }

        /**
         * @brief Makes the device for the engine's @p device, presenting into @p window (an HWND).
         * @return false with @p why: no loader, no device that meets the floor, a surface or a
         *         swapchain that cannot be made on this window.
         */
        bool Create(void* device, void* window, const Options& options, char* why, uint32_t why_size);

        /// Releases everything; the engine's pools and textures lose their records.
        void Destroy();
        bool Ready() const { return context_.Ready(); }
        void* Owner() const { return device_; }

        /// The engine made its window again (null: it is about to destroy it, the surface goes now).
        bool SetWindow(void* window, char* why, uint32_t why_size);

        /// The engine's capabilities (CGxDevice::m_caps) for this device.
        void WriteCaps();

        /// The window's client size, or that it was minimised.
        void WindowSize(uint32_t width, uint32_t height, bool minimized);

        /// Opens the next frame; false when the device is lost.
        bool BeginScene();
        /// Clears the scene inside (x, y, width, height), the colour 0xAARRGGBB when mask has 1, the depth when 2.
        void Clear(uint32_t mask, uint32_t argb, int32_t x, int32_t y, int32_t width, int32_t height);
        /// A draw of the engine; @p world: inside the world's pass.
        void Draw(const wxl_gx_batch* batch, int32_t count, bool world);
        /// Shows the open frame; false when the device is lost.
        bool Present();

        /// After a loss: everything made again on the same window. False with @p why.
        bool Restore(char* why, uint32_t why_size);

        void GammaChanged();
        void CursorChanged();
        /// WM_SETCURSOR over the client area: the pointer the engine wants.
        void SetCursor();

        char* PoolMemory(void* pool);
        void  PoolResize(void* pool);
        void  PoolDestroy(void* pool);
        void  PoolDiscard(void* pool);
        void  PoolWritten(void* pool, uint32_t offset, uint32_t size);
        bool  TexCreate(void* tex, uint32_t width, uint32_t height, uint32_t levels);
        void  TexDestroy(void* tex);
        char* TexLock(void* tex, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t& pitch);
        void  TexUnlock(void* tex);
        bool  ShaderCreate(void* shader, bool vertex, const char* name, const void* code, uint32_t size);
        void  ShaderDestroy(void* shader);
        void  ShaderConstants(int32_t target, uint32_t first, uint32_t rows, const float* values);
        void  ShaderPrewarm(const void* const* codes, const uint32_t* sizes, uint32_t count);
        /// The engine's render target changed (CGxDevice::m_textureTarget).
        void  TargetChanged();
        /// D3D9's TexCopy and TexStretch from the back buffer; false when nothing was copied.
        bool  TexCopy(void* tex, int32_t dest_x, int32_t dest_y, int32_t source_x, int32_t source_y, int32_t width, int32_t height,
                      uint32_t level, uint32_t plane);
        bool  TexStretch(void* tex, const int32_t* dest, const int32_t* source, uint32_t level, uint32_t face);
        uint32_t QueryCreate();
        void     QueryDestroy(uint32_t query);
        bool     QueryBegin(uint32_t query);
        bool     QueryEnd(uint32_t query);
        bool     QueryResult(uint32_t query, uint32_t& samples);
        /// D3D9's DeviceReadPixels: the back buffer's pixels as the frame has drawn them so far.
        bool     ReadPixels(int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t* bgra);
        /// The samples per pixel the device draws the back buffer with (VkSampleCountFlags).
        uint32_t SampleCounts() const { return display_.SampleCounts(); }

        /// Waits (bounded) for the frames submitted; a GPU that does not finish them is marked lost.
        void WaitIdle();

        void SimulateLoss();
        void Describe(char* out, uint32_t size) const;

        // --- For the runtime around it ---------------------------------------------------------------
        device::Context&      Context() { return context_; }
        resources::Allocator& Allocator() { return allocator_; }
        present::Display&     Screen() { return display_; }
        frame::Timeline&      Timeline() { return timeline_; }
        draw::Renderer&       Renderer() { return renderer_; }
        /// Grows each time the device is made again: what was made on the previous one is gone.
        uint64_t              Generation() const { return generation_; }
        const Stats&          Counters() const { return stats_; }
        void*                 Window() const { return window_; }

    private:
        /// What the engine's pointers name on the live client.
        class Live final : public Resolver
        {
        public:
            explicit Live(Front& front) : front_(front) {}
            const engine::CGxBuf* Buffer(const void* buf) override { return static_cast<const engine::CGxBuf*>(buf); }
            draw::Pool*           Pool(const void* pool) override;
            draw::Texture*        Texture(const void* texture, const engine::CGxTex*& fields) override;
            int                   Program(const void* shader, bool vertex) override;

        private:
            Front& front_;
        };

        /// What a CGxTex's m_apiSpecificData points at: the texture, null while the device is made again.
        struct TexRecord
        {
            draw::Texture* texture = nullptr;
        };

        bool MakeDevice(char* why, uint32_t why_size);
        void ReleaseDevice();
        void ReadRefresh();
        void UpdateSync();
        void UpdateSoftwareCursor();
        void ReleaseCursor();

        Options               options_;
        void*                 device_ = nullptr; ///< the engine's CGxDevice
        void*                 window_ = nullptr; ///< its HWND
        device::Context       context_;
        resources::Allocator  allocator_;
        frame::Timeline       timeline_;
        present::Display      display_;
        draw::Renderer        renderer_;
        uint64_t              generation_ = 0;
        Stats                 stats_;

        uint32_t refresh_hz_ = 60;
        void*    cursor_ = nullptr;       ///< the hardware cursor (HCURSOR) made from the engine's pixels
        bool     cursor_pixels_ = false;  ///< the software cursor's pixels were handed to the present pass

        std::unordered_set<void*> pools_;    ///< pools whose m_apiSpecific is a draw::Pool of ours
        std::unordered_set<void*> textures_; ///< textures whose m_apiSpecificData is a TexRecord of ours
        /// A client shader: its record (kept, so that a device made again after a loss makes its
        /// program again) and its program on the current device.
        struct ShaderRecord
        {
            std::string          name;
            std::vector<uint8_t> code;
            int                  program = -1;
            uint64_t             generation = 0; ///< the device the program was made on
        };
        std::unordered_map<const void*, ShaderRecord> shaders_[2]; ///< [pixel, vertex]
        draw::Texture* Record(void* tex);
        bool           missing_target_ = false; ///< the engine draws into a texture the device has not made
    };
}
