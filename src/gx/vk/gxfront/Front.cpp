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

        /// What a texture holds until it has an image: its size and format, for the next step to make it.
        struct TexRecord
        {
            uint32_t width;
            uint32_t height;
            uint32_t format;
        };

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
        if (!display_.Create(context_, allocator_, timeline_, display, why, why_size)) return false;
        const VkExtent2D extent = ClientExtent(window_);
        display_.SetWindowExtent(extent);
        display_.SetSceneExtent(extent);
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
        cursor_pixels_ = false;
        ++generation_;
        return true;
    }

    void Front::ReleaseDevice()
    {
        display_.Destroy();
        timeline_.Destroy();
        allocator_.Destroy();
        context_.Destroy();
    }

    void Front::Destroy()
    {
        ReleaseDevice();
        ReleaseCursor();
        for (void* pool : pools_)
        {
            engine::CGxPool* p = static_cast<engine::CGxPool*>(pool);
            std::free(p->m_apiSpecific);
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
        for (int i = 1; i < 13; ++i) caps.m_texFmt[i] = 1;
        if (!f.textureCompressionBC) caps.m_texFmt[6] = caps.m_texFmt[7] = caps.m_texFmt[8] = 0;
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
        // Occlusion queries come with the step that draws the world; until then the engine does without.
        caps.m_occlusionQuery        = 0;
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
        if (display_.BeginFrame(wait)) return true;
        if (!context_.Lost()) context_.MarkLost("a frame could not begin");
        return false;
    }

    void Front::Clear(uint32_t mask, uint32_t argb, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        if (!display_.InFrame() || width <= 0 || height <= 0) return;
        ++stats_.clears;
        const float rgba[4] = {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                               ((argb >> 24) & 0xFF) / 255.0f};
        display_.Clear((mask & 1) != 0, (mask & 2) != 0, rgba, 1.0f,
                       VkRect2D{{x, y}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}});
    }

    void Front::Draw(const void*, int32_t) { ++stats_.draws; }

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

    void Front::UpdateSoftwareCursor()
    {
        engine::CGxDevice*  dev = Engine(device_);
        present::CursorDraw draw;
        POINT               at{};
        if (!dev->m_hwCursor && dev->m_cursorVisible && PointerInClient(window_, at))
        {
            if (!cursor_pixels_)
            {
                display_.PresentPass().SetCursor(dev->m_cursor);
                cursor_pixels_ = true;
            }
            // The pointer is in window pixels; the image shown may be another size than the scene.
            const VkExtent2D scene = display_.SceneExtent();
            const VkExtent2D shown = display_.Chain().Ready() ? display_.Chain().Extent() : scene;
            const float      sx    = scene.width ? static_cast<float>(shown.width) / static_cast<float>(scene.width) : 1.0f;
            draw.visible           = true;
            draw.x                 = at.x - static_cast<int32_t>(static_cast<float>(dev->m_cursorHotspotX) * sx);
            draw.y                 = at.y - static_cast<int32_t>(static_cast<float>(dev->m_cursorHotspotY) * sx);
            draw.size              = static_cast<uint32_t>(32.0f * sx + 0.5f);
        }
        display_.SetCursor(draw);
    }

    bool Front::Present()
    {
        if (!context_.Ready() || !display_.InFrame()) return !context_.Lost();
        UpdateSync();
        UpdateSoftwareCursor();
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
        cursor_pixels_         = false; // the software cursor takes the new pixels at the next frame
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

    char* Front::PoolMemory(void* pool)
    {
        engine::CGxPool* p = static_cast<engine::CGxPool*>(pool);
        if (p->m_apiSpecific) return pools_.count(pool) ? static_cast<char*>(p->m_apiSpecific) : nullptr;
        if (p->m_size <= 0) return nullptr;
        void* block = std::malloc(static_cast<size_t>(p->m_size));
        if (!block) return nullptr;
        p->m_apiSpecific = block;
        pools_.insert(pool);
        return static_cast<char*>(block);
    }

    void Front::PoolResize(void* pool) { PoolDestroy(pool); }

    void Front::PoolDestroy(void* pool)
    {
        if (!pools_.erase(pool)) return;
        engine::CGxPool* p = static_cast<engine::CGxPool*>(pool);
        std::free(p->m_apiSpecific);
        p->m_apiSpecific = nullptr;
    }

    bool Front::TexCreate(void* tex)
    {
        engine::CGxTex* t = static_cast<engine::CGxTex*>(tex);
        if (textures_.count(tex)) return true;
        t->m_apiSpecificData = new TexRecord{t->m_width, t->m_height, t->m_format};
        textures_.insert(tex);
        return true;
    }

    void Front::TexDestroy(void* tex)
    {
        if (!textures_.erase(tex)) return;
        engine::CGxTex* t = static_cast<engine::CGxTex*>(tex);
        delete static_cast<TexRecord*>(t->m_apiSpecificData);
        t->m_apiSpecificData = nullptr;
    }

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
        std::snprintf(out, size, "Vulkan: %s, %llu frames (%llu shown, %llu dropped), %llu swapchain builds, %llu draws not drawn yet, %llu losses%s",
                      context_.Chosen().name, static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.presented),
                      static_cast<unsigned long long>(s.dropped), static_cast<unsigned long long>(s.rebuilds),
                      static_cast<unsigned long long>(stats_.draws), static_cast<unsigned long long>(stats_.losses),
                      context_.Lost() ? ", the device is lost" : "");
    }
}
