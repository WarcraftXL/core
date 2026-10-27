// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Vulkan device behind one engine device.

#include <windows.h>

#include "gx/vk/gxfront/Front.hpp"

#include "gx/vk/Log.hpp"
#include "gx/vk/present/Present.hpp"

#include "wxl/engine/gx.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wxl::vk::gxfront
{
    namespace
    {
        namespace engine = wxl::engine;

        engine::CGxDevice* Engine(void* device) { return static_cast<engine::CGxDevice*>(device); }

        VkSurfaceKHR MakeSurface(VkInstance instance, void* user, char* why, uint32_t why_size)
        {
            return present::CreateWindowSurface(instance, static_cast<Front*>(user)->Window(), why, why_size);
        }

        VkExtent2D ClientExtent(void* window)
        {
            RECT client{};
            if (!window || !GetClientRect(static_cast<HWND>(window), &client)) return {0, 0};
            return {static_cast<uint32_t>(std::max<LONG>(client.right - client.left, 0)),
                    static_cast<uint32_t>(std::max<LONG>(client.bottom - client.top, 0))};
        }

        /// Where the pointer is over the window's client area, if it is.
        bool PointerInClient(void* window, POINT& at)
        {
            if (!window || !GetCursorPos(&at) || !ScreenToClient(static_cast<HWND>(window), &at)) return false;
            RECT client{};
            GetClientRect(static_cast<HWND>(window), &client);
            return at.x >= client.left && at.x < client.right && at.y >= client.top && at.y < client.bottom;
        }

        /// The engine's 32x32 0xAARRGGBB pointer as a Windows cursor, its alpha keyed at 224 as the
        /// engine draws it (a hardware cursor shows a pixel or not).
        HCURSOR MakeCursor(const uint32_t* argb, uint32_t hot_x, uint32_t hot_y)
        {
            BITMAPV5HEADER header{};
            header.bV5Size        = sizeof header;
            header.bV5Width       = 32;
            header.bV5Height      = -32; // top-down
            header.bV5Planes      = 1;
            header.bV5BitCount    = 32;
            header.bV5Compression = BI_BITFIELDS;
            header.bV5RedMask     = 0x00FF0000;
            header.bV5GreenMask   = 0x0000FF00;
            header.bV5BlueMask    = 0x000000FF;
            header.bV5AlphaMask   = 0xFF000000;
            void*   bits          = nullptr;
            HDC     dc            = GetDC(nullptr);
            HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
            ReleaseDC(nullptr, dc);
            static const BYTE kMask[32 * 32 / 8] = {};
            HBITMAP           mask = CreateBitmap(32, 32, 1, 1, kMask);
            HCURSOR           made = nullptr;
            if (color && mask && bits)
            {
                uint32_t* out = static_cast<uint32_t*>(bits);
                for (uint32_t i = 0; i < 32 * 32; ++i) out[i] = (argb[i] >> 24) >= 224 ? (argb[i] | 0xFF000000u) : 0u;
                ICONINFO info{};
                info.fIcon    = FALSE;
                info.xHotspot = std::min<uint32_t>(hot_x, 31);
                info.yHotspot = std::min<uint32_t>(hot_y, 31);
                info.hbmMask  = mask;
                info.hbmColor = color;
                made          = reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
            }
            if (color) DeleteObject(color);
            if (mask) DeleteObject(mask);
            return made;
        }
    }

    bool Front::Create(void* device, void* window, const Options& options, char* why, uint32_t why_size)
    {
        Destroy();
        device_  = device;
        window_  = window;
        options_ = options;
        if (!MakeDevice(why, why_size))
        {
            Destroy();
            return false;
        }
        return true;
    }

    bool Front::MakeDevice(char* why, uint32_t why_size)
    {
        device::Options options;
        options.gpu                 = options_.gpu && options_.gpu[0] ? options_.gpu : nullptr;
        options.validation          = options_.validation;
        options.instance_extensions = present::InstanceExtensions(options.instance_extension_count);
        options.presents            = present::Presents;
        if (!context_.Create(options, why, why_size) || !allocator_.Create(context_, why, why_size) ||
            !timeline_.Create(context_, why, why_size))
            return false;

        present::Display::Options display;
        display.frames = options_.frames;
        display.filter = options_.filter;
        draw::Renderer::Paths paths;
        if (!options_.cache_dir.empty())
        {
            paths.pipeline_cache = options_.cache_dir + "pipelines.bin";
            paths.shader_pack    = options_.cache_dir + "shaders.pack";
            paths.pipeline_keys  = options_.cache_dir + "pipelines.keys";
        }
        paths.async_pipelines = options_.async_pipelines;
        if (!display_.Create(context_, allocator_, timeline_, display, why, why_size) ||
            !renderer_.Create(context_, allocator_, display_, paths, why, why_size))
            return false;
        if (!render_.Create(context_, allocator_, timeline_, display_, renderer_))
        {
            std::snprintf(why, why_size, "wxl.render's own programs could not be made");
            return false;
        }
        const VkExtent2D extent = ClientExtent(window_);
        display_.SetWindowExtent(extent);
        display_.SetSceneExtent(extent);
        if (device_) display_.SetSamples(Engine(device_)->m_format.m_multisampleCount);
        if (window_ && !display_.SetSurface(&MakeSurface, this, why, why_size)) return false;

        // A first frame, waited for within the bound: the queue answers, and with a window the
        // swapchain is made now rather than later, so a window it cannot present to keeps D3D9.
        frame::Wait wait{};
        if (!display_.BeginFrame(wait))
        {
            std::snprintf(why, why_size, "the first frame did not begin (%s)", frame::WaitName(wait));
            return false;
        }
        const present::FrameEnd end = display_.EndFrame();
        if (end == present::FrameEnd::Lost || (window_ && extent.width && extent.height && !display_.Chain().Ready()))
        {
            std::snprintf(why, why_size, "the swapchain could not be made on the client's window (the first frame was %s)",
                          present::FrameEndName(end));
            return false;
        }
        wait = timeline_.WaitFor(display_.LastSerial());
        if (wait != frame::Wait::Done)
        {
            std::snprintf(why, why_size, "the device did not complete its first frame (%s) on %s", frame::WaitName(wait),
                          context_.Chosen().name);
            return false;
        }
        ReadRefresh();
        ++generation_;
        return true;
    }

    void Front::ReleaseDevice()
    {
        if (renderer_.Ready())
        {
            // The textures and the pools lose what they had on this device; their records stay, the
            // engine makes them again (its textures recreated, its pools written again) on the next.
            WaitIdle();
            for (void* tex : textures_)
            {
                TexRecord* record = static_cast<TexRecord*>(static_cast<engine::CGxTex*>(tex)->m_apiSpecificData);
                renderer_.DestroyTexture(record->texture);
                record->texture = nullptr;
            }
            for (void* pool : pools_) renderer_.Forget(static_cast<draw::Pool*>(static_cast<engine::CGxPool*>(pool)->m_apiSpecific));
        }
        render_.Release();
        renderer_.Destroy();
        display_.Destroy();
        timeline_.Destroy();
        allocator_.Destroy();
        context_.Destroy();
    }

    void Front::Destroy()
    {
        ReleaseDevice();
        render_.Destroy();
        replacements_.clear();
        ReleaseCursor();
        for (void* pool : pools_)
        {
            engine::CGxPool* p = static_cast<engine::CGxPool*>(pool);
            delete static_cast<draw::Pool*>(p->m_apiSpecific);
            p->m_apiSpecific = nullptr;
        }
        pools_.clear();
        for (void* tex : textures_)
        {
            engine::CGxTex* t = static_cast<engine::CGxTex*>(tex);
            delete static_cast<TexRecord*>(t->m_apiSpecificData);
            t->m_apiSpecificData = nullptr;
        }
        textures_.clear();
        shaders_[0].clear();
        shaders_[1].clear();
        device_ = nullptr;
        window_ = nullptr;
        stats_  = Stats{};
    }

    bool Front::SetWindow(void* window, char* why, uint32_t why_size)
    {
        window_ = window;
        if (!window)
        {
            // The window goes right after this: its surface must go first, now.
            if (display_.InFrame()) display_.EndRendering();
            const uint64_t last = timeline_.Last();
            if (last && timeline_.WaitFor(last) != frame::Wait::Done && !context_.Lost())
                context_.MarkLost("the frames on the old window did not complete");
            display_.SetSurface(nullptr, nullptr, why, why_size);
            // Nothing in flight names them any more: the swapchain and the surface go now.
            display_.CollectNow();
            return true;
        }
        const VkExtent2D extent = ClientExtent(window);
        display_.SetWindowExtent(extent);
        display_.SetSceneExtent(extent);
        // A new format (DeviceSetFormat makes the window again): its multisample count too.
        display_.SetSamples(Engine(device_)->m_format.m_multisampleCount);
        ReadRefresh();
        GammaChanged();
        return display_.SetSurface(&MakeSurface, this, why, why_size);
    }

    void Front::ReadRefresh()
    {
        refresh_hz_ = 60;
        if (!window_) return;
        HMONITOR       monitor = MonitorFromWindow(static_cast<HWND>(window_), MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW info{};
        info.cbSize = sizeof info;
        if (!monitor || !GetMonitorInfoW(monitor, &info)) return;
        DEVMODEW mode{};
        mode.dmSize = sizeof mode;
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
            refresh_hz_ = mode.dmDisplayFrequency;
    }

    void Front::WriteCaps()
    {
        if (!device_ || !context_.Ready()) return;
        engine::CGxCaps&                  caps   = Engine(device_)->m_caps;
        const VkPhysicalDeviceLimits&     limits = context_.Properties().limits;
        const device::Features&           f      = context_.Chosen().features;
        auto renders = [&](VkFormat format) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(context_.Physical(), format, &props);
            return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0 ? 1 : 0;
        };

        // As the D3D9 device sets them on a current GPU, from this device's limits where they differ.
        caps.m_numTmus           = 8;
        caps.m_pixelCenterOnEdge = 0;
        caps.m_texelCenterOnEdge = 1;
        caps.m_numStreams        = 16;
        caps.m_streamOffset      = 1;
        caps.m_maxIndex          = 0xFFFFFF;
        caps.m_generateMipMaps   = 1;
        const uint32_t texMax    = std::min<uint32_t>(limits.maxImageDimension2D, 16384);
        caps.m_texMaxSize[0]     = texMax;                                                   // 2D
        caps.m_texMaxSize[1]     = std::min<uint32_t>(limits.maxImageDimensionCube, 16384); // cube
        caps.m_texMaxSize[2]     = texMax;
        caps.m_texMaxSize[3]     = texMax;
        caps.m_texTarget[0]      = 1;
        caps.m_texTarget[1]      = 1;
        caps.m_texTarget[2]      = 0;
        caps.m_texTarget[3]      = 1;
        caps.m_texNonPow2        = 0;
        // EGxTexFormat 1..12: Abgr8888, Argb8888, Argb4444, Argb1555, Rgb565, Dxt1, Dxt3, Dxt5, Uv88, Gr1616F, R32F, D24X8.
        caps.m_texFmt[0] = 0;
        for (uint32_t i = 1; i < 13; ++i) caps.m_texFmt[i] = renderer_.TextureFormats().Of(i).format != VK_FORMAT_UNDEFINED ? 1 : 0;
        (void)f.textureCompressionBC;
        for (int& rt : caps.m_rttFormat) rt = 0;
        caps.m_rttFormat[2]  = renders(VK_FORMAT_B8G8R8A8_UNORM);
        caps.m_rttFormat[5]  = renders(VK_FORMAT_R5G6B5_UNORM_PACK16);
        caps.m_rttFormat[10] = renders(VK_FORMAT_R16G16_SFLOAT);
        caps.m_rttFormat[11] = renders(VK_FORMAT_R32_SFLOAT);
        caps.m_rttFormat[12] = 1;
        for (int i = 0; i < 6; ++i)
        {
            caps.m_shaderTargets[i]   = 0;
            caps.m_shaderConstants[i] = 0;
        }
        caps.m_shaderTargets[4]      = 4;   // ps_3_0
        caps.m_shaderTargets[0]      = 3;   // vs_3_0
        caps.m_shaderConstants[0]    = 256;
        caps.m_texFilterTrilinear    = 1;
        caps.m_texFilterAnisotropic  = f.samplerAnisotropy ? 1 : 0;
        caps.m_maxTexAnisotropy      = f.samplerAnisotropy ? static_cast<uint32_t>(std::min(limits.maxSamplerAnisotropy, 16.0f)) : 1;
        caps.m_depthBias             = 1;
        caps.m_colorWrite            = 1;
        caps.m_maxClipPlanes         = f.shaderClipDistance ? static_cast<int32_t>(std::min<uint32_t>(limits.maxClipDistances, 6)) : 0;
        caps.m_hwCursor              = 1;
        // Occlusion queries where the device counts samples (the sun's glare reads them).
        caps.m_occlusionQuery        = renderer_.QueriesSupported() ? 1 : 0;
        caps.m_pointParameters       = f.largePoints ? 1 : 0;
        caps.m_pointScaleMax         = f.largePoints ? limits.pointSizeRange[1] : 1.0f;
        caps.m_pointSprite           = f.largePoints ? 1 : 0;
        caps.m_blendFactor           = 1;
        caps.m_shaderAlphaRef        = 1;
        caps.unk_0134                = 0;
        caps.unk_0138                = 0;
    }

    void Front::WindowSize(uint32_t width, uint32_t height, bool minimized)
    {
        if (minimized)
        {
            display_.SetWindowExtent({0, 0});
            return;
        }
        display_.SetWindowExtent({width, height});
        display_.SetSceneExtent({width, height});
        ReadRefresh();
    }

    bool Front::BeginScene()
    {
        if (!context_.Ready()) return false;
        if (display_.InFrame()) return true;
        frame::Wait wait{};
        if (display_.BeginFrame(wait))
        {
            render_.FrameBegun();
            return true;
        }
        if (!context_.Lost()) context_.MarkLost("a frame could not begin");
        return false;
    }

    void Front::Clear(uint32_t mask, uint32_t argb, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        if (!display_.InFrame() || width <= 0 || height <= 0 || !device_ || missing_target_) return;
        ++stats_.clears;
        // The viewport D3D9 clears, as the engine's device holds it now (a texture target's included).
        (void)x;
        (void)y;
        int32_t rect[4];
        ClearRect(*Engine(device_), rect);
        if (rect[2] <= 0 || rect[3] <= 0) return;
        const float rgba[4] = {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                               ((argb >> 24) & 0xFF) / 255.0f};
        renderer_.Clear((mask & 1) != 0, (mask & 2) != 0, rgba, 1.0f,
                        VkRect2D{{rect[0], rect[1]}, {static_cast<uint32_t>(rect[2]), static_cast<uint32_t>(rect[3])}});
    }

    void Front::Draw(const wxl_gx_batch* batch, int32_t count, bool world)
    {
        ++stats_.draws;
        // The batch a mod may draw again is this draw's, or none: never an earlier draw's, when
        // this one is left out.
        renderer_.ForgetLastBatch();
        if (!batch || !display_.InFrame() || !renderer_.Ready()) return;
        engine::CGxDevice* dev = Engine(device_);
        EngineView         view;
        view.device           = dev;
        view.states           = static_cast<const wxl_gx_app_state*>(dev->m_appRenderStates.m_data);
        view.vertex_constants = renderer_.Registers(false);
        if (!view.states || dev->m_appRenderStates.m_count < kRenderStates) return;
        Live           live(*this);
        draw::Packet   packet;
        const Verdict  verdict = BuildDraw(view, *batch, count, live, packet);
        ++stats_.left_out[static_cast<int>(verdict)];
        if (verdict == Verdict::Draw && missing_target_)
        {
            --stats_.left_out[0];
            ++stats_.left_out[static_cast<int>(Verdict::Target)];
            return;
        }
        if (verdict == Verdict::Draw && renderer_.Draw(packet) && world) ++stats_.world;
    }

    void Front::UpdateSync()
    {
        const engine::CGxFormat& format = Engine(device_)->m_format;
        const uint32_t           vsync  = format.m_vsync;
        display_.SetSync(vsync ? present::Sync::On : present::Sync::Off);
        // D3D9 shows every 2nd to 4th refresh in fullscreen only (a window gets every refresh): a
        // limiter over FIFO does it here.
        const bool fullscreen = !format.m_window;
        display_.SetInterval(fullscreen && vsync >= 2 && vsync <= 4 ? vsync * 1'000'000'000ull / refresh_hz_ : 0);
    }

    bool Front::Present()
    {
        if (!context_.Ready() || !display_.InFrame()) return !context_.Lost();
        UpdateSync();
        const present::FrameEnd end = display_.EndFrame();
        if (end == present::FrameEnd::Lost) return false;
        // gxFixLag: the CPU waits for the frame it just sent, as the D3D9 device's event query does.
        if (Engine(device_)->m_format.m_fixLag && display_.LastSerial())
        {
            const frame::Wait wait = timeline_.WaitFor(display_.LastSerial());
            if (wait == frame::Wait::Timeout) context_.MarkLost("a frame did not complete within the bound (gxFixLag)");
        }
        return !context_.Lost();
    }

    bool Front::Restore(char* why, uint32_t why_size)
    {
        ++stats_.losses;
        ReleaseDevice();
        return MakeDevice(why, why_size);
    }

    void Front::GammaChanged()
    {
        if (!device_ || !display_.Ready()) return;
        engine::CGxDevice* dev = Engine(device_);
        // As the D3D9 device: the ramp in fullscreen only, where D3D9 sets it on the display.
        if (dev->m_format.m_window)
        {
            display_.PresentPass().SetGamma(nullptr);
            return;
        }
        present::GammaRamp ramp;
        std::memcpy(ramp.red, dev->m_gammaRamp.m_red, sizeof ramp.red);
        std::memcpy(ramp.green, dev->m_gammaRamp.m_green, sizeof ramp.green);
        std::memcpy(ramp.blue, dev->m_gammaRamp.m_blue, sizeof ramp.blue);
        bool identity = true;
        for (int i = 0; i < 256 && identity; ++i)
        {
            const int want = i * 257;
            identity = std::abs(ramp.red[i] - want) <= 256 && std::abs(ramp.green[i] - want) <= 256 && std::abs(ramp.blue[i] - want) <= 256;
        }
        display_.PresentPass().SetGamma(identity ? nullptr : &ramp);
    }

    void Front::ReleaseCursor()
    {
        if (cursor_ && GetCursor() != static_cast<HCURSOR>(cursor_)) DestroyCursor(static_cast<HCURSOR>(cursor_));
        cursor_ = nullptr;
    }

    void Front::CursorChanged()
    {
        if (!device_) return;
        engine::CGxDevice* dev = Engine(device_);
        if (!dev->m_hwCursor) return;
        HCURSOR made = MakeCursor(dev->m_cursor, dev->m_cursorHotspotX, dev->m_cursorHotspotY);
        HCURSOR old  = static_cast<HCURSOR>(cursor_);
        cursor_      = made;
        POINT at{};
        if (PointerInClient(window_, at)) ::SetCursor(dev->m_cursorVisible && made ? made : nullptr);
        if (old && old != made && GetCursor() != old) DestroyCursor(old);
    }

    void Front::SetCursor()
    {
        if (!device_) return;
        engine::CGxDevice* dev = Engine(device_);
        if (dev->m_hwCursor && !cursor_) cursor_ = MakeCursor(dev->m_cursor, dev->m_cursorHotspotX, dev->m_cursorHotspotY);
        ::SetCursor(dev->m_hwCursor && dev->m_cursorVisible && cursor_ ? static_cast<HCURSOR>(cursor_) : nullptr);
    }

    draw::Pool* Front::Live::Pool(const void* pool)
    {
        if (!pool || !front_.pools_.count(const_cast<void*>(pool))) return nullptr;
        return static_cast<draw::Pool*>(static_cast<const engine::CGxPool*>(pool)->m_apiSpecific);
    }

    draw::Texture* Front::Live::Texture(const void* texture, const engine::CGxTex*& fields)
    {
        fields = static_cast<const engine::CGxTex*>(texture);
        if (!texture || !front_.textures_.count(const_cast<void*>(texture))) return nullptr;
        return static_cast<TexRecord*>(fields->m_apiSpecificData)->texture;
    }

    int Front::Live::Program(const void* shader, bool vertex)
    {
        auto&      shaders = front_.shaders_[vertex ? 1 : 0];
        const auto found   = shaders.find(shader);
        if (found == shaders.end()) return -1;
        ShaderRecord& record = found->second;
        if (record.generation != front_.generation_)
        {
            // Made on a device since lost: made again on this one.
            record.program    = front_.renderer_.Program(!vertex, record.name.c_str(), record.code.data(), record.code.size());
            record.generation = front_.generation_;
        }
        return front_.replacements_.empty() ? record.program : front_.ProgramOf(record, vertex);
    }

    char* Front::PoolMemory(void* pool)
    {
        engine::CGxPool* p = static_cast<engine::CGxPool*>(pool);
        if (!renderer_.Ready() || p->m_size <= 0) return nullptr;
        if (!pools_.count(pool))
        {
            if (p->m_apiSpecific) return nullptr; // not ours
            p->m_apiSpecific = renderer_.CreatePool(p->m_target, p->m_usage, static_cast<uint32_t>(p->m_size));
            pools_.insert(pool);
        }
        return reinterpret_cast<char*>(renderer_.PoolMemory(static_cast<draw::Pool*>(p->m_apiSpecific)));
    }

    void Front::PoolResize(void* pool)
    {
        if (!pools_.count(pool)) return;
        engine::CGxPool* p = static_cast<engine::CGxPool*>(pool);
        renderer_.ResizePool(static_cast<draw::Pool*>(p->m_apiSpecific), static_cast<uint32_t>(std::max(p->m_size, 0)));
    }

    void Front::PoolDestroy(void* pool)
    {
        if (!pools_.erase(pool)) return;
        engine::CGxPool* p = static_cast<engine::CGxPool*>(pool);
        renderer_.DestroyPool(static_cast<draw::Pool*>(p->m_apiSpecific));
        p->m_apiSpecific = nullptr;
    }

    void Front::PoolDiscard(void* pool)
    {
        if (pools_.count(pool)) renderer_.DiscardPool(static_cast<draw::Pool*>(static_cast<engine::CGxPool*>(pool)->m_apiSpecific));
    }

    void Front::PoolWritten(void* pool, uint32_t offset, uint32_t size)
    {
        if (pools_.count(pool))
            renderer_.PoolWritten(static_cast<draw::Pool*>(static_cast<engine::CGxPool*>(pool)->m_apiSpecific), offset, size);
    }

    bool Front::TexCreate(void* tex, uint32_t width, uint32_t height, uint32_t levels)
    {
        engine::CGxTex* t = static_cast<engine::CGxTex*>(tex);
        if (!renderer_.Ready()) return false;
        TexRecord* record = nullptr;
        if (textures_.count(tex))
        {
            record = static_cast<TexRecord*>(t->m_apiSpecificData);
            if (record->texture) return true;
        }
        draw::TextureDesc desc;
        desc.format        = t->m_format;
        desc.width         = width;
        desc.height        = height;
        desc.levels        = levels;
        desc.cube          = t->m_target == 1; // GxTex_CubeMap
        desc.target        = t->m_flags.m_renderTarget != 0;
        desc.generate_mips = t->m_flags.m_generateMipMaps != 0;
        draw::Texture* made = renderer_.CreateTexture(desc);
        if (!made) return false;
        if (!record)
        {
            record               = new TexRecord();
            t->m_apiSpecificData = record;
            textures_.insert(tex);
        }
        record->texture = made;
        return true;
    }

    void Front::TexDestroy(void* tex)
    {
        if (!textures_.erase(tex)) return;
        engine::CGxTex* t      = static_cast<engine::CGxTex*>(tex);
        TexRecord*      record = static_cast<TexRecord*>(t->m_apiSpecificData);
        renderer_.DestroyTexture(record->texture);
        delete record;
        t->m_apiSpecificData = nullptr;
    }

    char* Front::TexLock(void* tex, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t& pitch)
    {
        if (!textures_.count(tex)) return nullptr;
        TexRecord* record = static_cast<TexRecord*>(static_cast<engine::CGxTex*>(tex)->m_apiSpecificData);
        return reinterpret_cast<char*>(renderer_.LockTexture(record->texture, face, level, x, y, width, height, pitch));
    }

    void Front::TexUnlock(void* tex)
    {
        if (!textures_.count(tex)) return;
        renderer_.UnlockTexture(static_cast<TexRecord*>(static_cast<engine::CGxTex*>(tex)->m_apiSpecificData)->texture);
    }

    bool Front::ShaderCreate(void* shader, bool vertex, const char* name, const void* code, uint32_t size)
    {
        if (!shader || !renderer_.Ready()) return false;
        ShaderRecord record;
        record.name = name ? name : "";
        if (code && size) record.code.assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + size);
        std::string why;
        record.program    = renderer_.Program(!vertex, record.name.c_str(), record.code.data(), record.code.size(), &why);
        record.generation = generation_;
        const bool made   = record.program >= 0;
        shaders_[vertex ? 1 : 0][shader] = std::move(record);
        return made;
    }

    int Front::ProgramOf(const ShaderRecord& record, bool vertex)
    {
        if (record.program < 0) return record.program;
        // The newest replacement whose prefix the shader's name starts with.
        for (auto it = replacements_.rbegin(); it != replacements_.rend(); ++it)
            if (it->vertex == vertex && record.name.compare(0, it->prefix.size(), it->prefix) == 0)
            {
                const int id = renderer_.Replacement(!vertex, record.program, it->words, it->identity);
                return id >= 0 ? id : record.program;
            }
        return record.program;
    }

    bool Front::ReplaceShader(const std::string& prefix, bool vertex, std::vector<uint32_t> words, std::string& why)
    {
        if (!renderer_.Ready())
        {
            why = "no Vulkan device";
            return false;
        }
        if (words.size() < 5 || words[0] != 0x07230203u)
        {
            why = "the program is no SPIR-V module";
            return false;
        }
        VkShaderModuleCreateInfo info{};
        info.sType          = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize       = words.size() * sizeof(uint32_t);
        info.pCode          = words.data();
        VkShaderModule test = VK_NULL_HANDLE;
        if (vkCreateShaderModule(context_.Device(), &info, nullptr, &test) != VK_SUCCESS)
        {
            why = "the device refused the SPIR-V";
            return false;
        }
        vkDestroyShaderModule(context_.Device(), test, nullptr);
        uint64_t identity = 0xCBF29CE484222325ull; // FNV-1a over the words
        for (uint32_t w : words) identity = (identity ^ w) * 0x100000001B3ull;
        RestoreShader(prefix, vertex);
        replacements_.push_back(Replacement{prefix, vertex, std::move(words), identity});
        return true;
    }

    bool Front::RestoreShader(const std::string& prefix, bool vertex)
    {
        const auto at = std::find_if(replacements_.begin(), replacements_.end(),
                                     [&](const Replacement& r) { return r.vertex == vertex && r.prefix == prefix; });
        if (at == replacements_.end()) return false;
        replacements_.erase(at);
        return true;
    }

    void Front::ShaderDestroy(void* shader)
    {
        shaders_[0].erase(shader);
        shaders_[1].erase(shader);
    }

    void Front::ShaderConstants(int32_t target, uint32_t first, uint32_t rows, const float* values)
    {
        renderer_.Constants(target != 0, first, rows, values);
    }

    void Front::ShaderPrewarm(const void* const* codes, const uint32_t* sizes, uint32_t count)
    {
        if (!renderer_.Ready() || !codes || !sizes) return;
        std::vector<std::vector<uint8_t>> records;
        for (uint32_t i = 0; i < count; ++i)
            if (codes[i] && sizes[i])
                records.emplace_back(static_cast<const uint8_t*>(codes[i]), static_cast<const uint8_t*>(codes[i]) + sizes[i]);
        renderer_.Prewarm(std::move(records));
    }

    draw::Texture* Front::Record(void* tex)
    {
        if (!tex || !textures_.count(tex)) return nullptr;
        return static_cast<TexRecord*>(static_cast<engine::CGxTex*>(tex)->m_apiSpecificData)->texture;
    }

    void Front::TargetChanged()
    {
        if (!device_ || !renderer_.Ready()) return;
        const engine::CGxDevice* dev = Engine(device_);
        draw::Texture* color = Record(dev->m_textureTarget[0].m_texture);
        draw::Texture* depth = Record(dev->m_textureTarget[1].m_texture);
        // A target the device could not make draws nowhere rather than into the scene.
        missing_target_ = (dev->m_textureTarget[0].m_texture && !color) || (dev->m_textureTarget[1].m_texture && !depth);
        renderer_.SetTarget(color, dev->m_textureTarget[0].m_plane, depth);
    }

    bool Front::TexCopy(void* tex, int32_t dest_x, int32_t dest_y, int32_t source_x, int32_t source_y, int32_t width, int32_t height,
                        uint32_t level, uint32_t plane)
    {
        draw::Texture* dest = Record(tex);
        if (!dest || width <= 0 || height <= 0) return false;
        const VkRect2D from{{source_x, source_y}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
        const VkRect2D to{{dest_x, dest_y}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
        return renderer_.CopyScene(dest, level, plane, from, to, false);
    }

    bool Front::TexStretch(void* tex, const int32_t* dest, const int32_t* source, uint32_t level, uint32_t face)
    {
        draw::Texture* target = Record(tex);
        if (!target) return false;
        // The rectangles as CiRect holds them (minY, minX, maxY, maxX); null: the whole surface.
        const VkExtent2D scene = display_.SceneExtent();
        const uint32_t   tw = std::max(target->desc.width >> level, 1u), th = std::max(target->desc.height >> level, 1u);
        auto rect = [](const int32_t* r, uint32_t w, uint32_t h) {
            if (!r) return VkRect2D{{0, 0}, {w, h}};
            return VkRect2D{{r[1], r[0]}, {static_cast<uint32_t>(std::max(r[3] - r[1], 0)), static_cast<uint32_t>(std::max(r[2] - r[0], 0))}};
        };
        const VkRect2D from = rect(source, scene.width, scene.height), to = rect(dest, tw, th);
        const bool linear = !(source && dest && from.extent.width == to.extent.width && from.extent.height == to.extent.height);
        return renderer_.CopyScene(target, level, face, from, to, linear);
    }

    bool Front::ReadPixels(int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t* bgra)
    {
        if (!renderer_.Ready() || !display_.InFrame() || !bgra) return false;
        return display_.ReadScene(VkRect2D{{x, y}, {width, height}}, bgra);
    }

    uint32_t Front::QueryCreate() { return renderer_.Ready() ? renderer_.CreateQuery() : 0; }
    void     Front::QueryDestroy(uint32_t query) { renderer_.DestroyQuery(query); }
    bool     Front::QueryBegin(uint32_t query) { return renderer_.BeginQuery(query); }
    bool     Front::QueryEnd(uint32_t query) { return renderer_.EndQuery(query); }
    bool     Front::QueryResult(uint32_t query, uint32_t& samples) { return renderer_.QueryResult(query, samples); }

    void Front::WaitIdle()
    {
        if (!timeline_.Ready() || !timeline_.Last()) return;
        if (timeline_.WaitFor(timeline_.Last()) != frame::Wait::Done && !context_.Lost())
            context_.MarkLost("the frames submitted did not complete within the bound");
    }

    void Front::SimulateLoss()
    {
        if (context_.Ready()) context_.MarkLost("the console command gxVkSimulateLoss");
    }

    void Front::Describe(char* out, uint32_t size) const
    {
        if (!context_.Ready())
        {
            std::snprintf(out, size, "Vulkan: no device");
            return;
        }
        const present::Display::Stats& s = display_.Counters();
        const draw::Renderer::Stats&   r = renderer_.Counters();
        auto n = [](uint64_t v) { return static_cast<unsigned long long>(v); };
        draw::Renderer&                 renderer = const_cast<draw::Renderer&>(renderer_);
        const shaders::Cache::Stats     c        = renderer.ShaderCache().Counters();
        const pipelines::Pipelines::Stats p      = renderer.PipelineCache().Counters();
        std::snprintf(out, size,
                      "Vulkan: %s, %llu frames (%llu shown, %llu dropped), %llu swapchain builds, %u sample(s) per pixel, %llu draws "
                      "(%llu drawn, %llu in the world's pass; left out: %llu untranslated programs, %llu into a depth texture alone, "
                      "%llu not ready, %llu while their pipeline compiled, %llu refused by the renderer), %llu pipelines (%s; %llu linked "
                      "from parts in %.0f ms, %llu compiled on workers in %.0f ms with %llu parts, %llu of %llu drawn before prewarmed, "
                      "%llu linked ones replaced by their whole compile, %llu compiled on the main thread in %.0f ms and %llu parts in "
                      "%.0f ms; %llu hitches of %.0f ms or more, the longest %.1f ms; %llu failed), %llu programs translated (%llu "
                      "refused; %llu translated in %.0f ms, %llu from the cache, %llu ahead), %llu target renderings, %llu copies, %llu "
                      "resolves, %llu readbacks, %llu queries, %llu pass breaks, %llu textures, %llu draws with multisampling off in a "
                      "multisampled scene (%llu with their samples on the pixel's centre, %llu antialiased: no sample locations), "
                      "%llu losses%s",
                      context_.Chosen().name, n(s.frames), n(s.presented), n(s.dropped), n(s.rebuilds), display_.Samples(), n(stats_.draws),
                      n(r.draws), n(stats_.world), n(stats_.left_out[static_cast<int>(Verdict::Program)]),
                      n(stats_.left_out[static_cast<int>(Verdict::Target)]), n(stats_.left_out[static_cast<int>(Verdict::Unready)]),
                      n(r.skipped), n(r.refused), n(r.pipelines),
                      renderer.PipelineCache().Libraries() ? "linked from parts"
                      : renderer.PipelineCache().Async()   ? "compiled on workers, draws into the scene left out meanwhile"
                                                           : "compiled when first drawn",
                      n(p.linked), p.linked_ms, n(p.background), p.background_ms, n(p.parts_background), n(p.prewarmed),
                      n(p.prewarm_known), n(p.optimized), n(p.compiled_main), p.compiled_main_ms, n(p.parts_main), p.parts_main_ms,
                      n(p.hitches), pipelines::Pipelines::kHitchMs, p.longest_ms, n(p.failed), n(r.programs), n(r.programs_refused),
                      n(c.translated), c.milliseconds, n(c.from_disk), n(c.prewarmed), n(r.target_renderings), n(r.copies), n(s.resolves),
                      n(s.readbacks), n(r.queries), n(r.pass_breaks), n(r.textures), n(r.centred + r.antialiased), n(r.centred),
                      n(r.antialiased), n(stats_.losses),
                      context_.Lost() ? ", the device is lost" : "");
    }
}
