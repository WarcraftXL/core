// SPDX-License-Identifier: GPL-3.0-or-later
// The device seam (wxl.graphics.device): an extension may answer GxDevCreate with its own CGxDevice.

#include "common/Log.hpp"
#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "runtime/extensions/Extensions.hpp"
#include "runtime/extensions/Services.hpp"
#include "wxl/abi/GraphicsDevice.h"
#include "wxl/game/Binding.hpp"
#include "wxl/offsets/CGxDevice.hpp"
#include "wxl/offsets/CGxDeviceD3d.hpp"
#include "wxl/offsets/free/C.hpp"
#include "wxl/offsets/free/G.hpp"

#include <cstdint>
#include <cstring>

namespace
{
    namespace off = wxl::offsets;

    using DeviceCreateFn = int(__fastcall*)(void* device, void* edx, void* wndProc, void* format);
    using ScalarDeleteFn = void*(__fastcall*)(void* device, void* edx, uint32_t flags);

    constexpr uint32_t kDeleteSelf = 1;

    off::CreateGxDeviceFn g_origDeviceCreate = nullptr;

    WXL_DeviceCreateFn g_factory = nullptr;
    char               g_backendName[64] = "unknown";

    int __cdecl ApiRegisterFactory(WXL_DeviceCreateFn factory, const char* name)
    {
        if (!factory) return 0;
        g_factory = factory;
        std::strncpy(g_backendName, name && name[0] ? name : "unnamed", sizeof g_backendName - 1);
        g_backendName[sizeof g_backendName - 1] = '\0';
        WLOG_INFO("gx-device: device factory registered by %s", g_backendName);
        return 1;
    }

    void __cdecl ApiUnregisterFactory(WXL_DeviceCreateFn factory)
    {
        if (g_factory != factory) return;
        WLOG_INFO("gx-device: device factory of %s withdrawn", g_backendName);
        g_factory = nullptr;
        std::strcpy(g_backendName, "unknown");
    }

    /// The engine's constructor, the default stream pools and their buffers (the shader system releases
    /// through them at exit), and no D3D9 device: what every stock backend's constructor leaves.
    void __cdecl ApiInitBaseDevice(void* device)
    {
        if (!device) return;
        wxl::game::Native<off::CGxDevice::ctorFn>(off::CGxDevice::ctor)(device, nullptr);
        wxl::game::Native<off::CGxDevice::DeviceCreatePoolsFn>(off::CGxDevice::DeviceCreatePools)(device, nullptr);
        wxl::game::Native<off::CGxDevice::DeviceCreateStreamBufsFn>(off::CGxDevice::DeviceCreateStreamBufs)(device, nullptr);
        wxl::game::At<void*>(device, off::CGxDeviceD3d::fields::m_d3dDevice) = nullptr;
    }

    const void* const* __cdecl ApiGetBaseVTable(void)
    {
        return reinterpret_cast<const void* const*>(off::CGxDevice::vftable);
    }

    const WXL_GraphicsDeviceApi g_api = {
        sizeof(WXL_GraphicsDeviceApi), WXL_GRAPHICS_DEVICE_API_VERSION, &ApiRegisterFactory,
        &ApiUnregisterFactory,         &ApiInitBaseDevice,              &ApiGetBaseVTable,
    };

    /// The engine's constructor links the pool list's terminator to itself: never null once it ran.
    bool BaseRan(void* device)
    {
        return wxl::game::At<uintptr_t>(device, off::CGxDevice::fields::m_poolList_m_terminator_m_prevLink) != 0 &&
               wxl::game::At<uintptr_t>(device, off::CGxDevice::fields::m_poolList_m_terminator_m_next) != 0;
    }

    /// A backend that skipped the base constructor gets it, its own vtable kept.
    void EnsureBase(void* device)
    {
        if (BaseRan(device))
        {
            WLOG_INFO("gx-device: %s's device carries the base constructor (pool list linked), not run again",
                      g_backendName);
            return;
        }
        WLOG_WARN("gx-device: %s returned a device the base constructor had not run on", g_backendName);
        void* vtable = *static_cast<void**>(device);
        ApiInitBaseDevice(device);
        *static_cast<void**>(device) = vtable;
    }

    /// The client builds its device before engine initialisation, so the extensions load here: a
    /// backend answers only once its WXL_Load registered the factory. Then GxDevCreate's own steps, in
    /// its order: the device is published before its create, which looks it up through that pointer.
    void* __cdecl DeviceCreateDetour(int api, void* wndProc, void* context)
    {
        wxl::runtime::extensions::EnsureLoaded();
        if (!g_factory) return g_origDeviceCreate(api, wndProc, context);

        WLOG_INFO("gx-device: GxDevCreate(api %d) offered to %s", api, g_backendName);
        void* device = g_factory(api, wndProc, context);
        if (!device)
        {
            WLOG_WARN("gx-device: %s declined; the engine builds its stock backend", g_backendName);
            return g_origDeviceCreate(api, wndProc, context);
        }
        EnsureBase(device);

        auto** published = reinterpret_cast<void**>(off::g_theGxDevicePtr);
        *published       = device;
        auto** vtable    = *reinterpret_cast<void***>(device);
        if (reinterpret_cast<DeviceCreateFn>(vtable[off::CGxDeviceD3d::vftable_slot::DeviceCreate])(device, nullptr, wndProc, context))
        {
            WLOG_INFO("gx-device: %s is the engine device, at %p", g_backendName, device);
            return device;
        }

        WLOG_WARN("gx-device: %s failed to create; the engine builds its stock backend", g_backendName);
        reinterpret_cast<ScalarDeleteFn>(vtable[off::CGxDeviceD3d::vftable_slot::Destructor])(device, nullptr, kDeleteSelf);
        *published = nullptr;
        return g_origDeviceCreate(api, wndProc, context);
    }

    /// Boot phase: the service is published before any extension asks for it.
    bool InstallDeviceCreate()
    {
        if (!wxl::runtime::hookpoints::Attach("CreateGxDevice", &DeviceCreateDetour, &g_origDeviceCreate))
        {
            WLOG_ERROR("gx-device: Gx.DevCreate did not attach; no extension can supply a device");
            return false;
        }
        wxl::runtime::extensions::PublishInterface(WXL_GRAPHICS_DEVICE_API_NAME, WXL_GRAPHICS_DEVICE_API_VERSION,
                                                   const_cast<WXL_GraphicsDeviceApi*>(&g_api));
        return true;
    }
}

WXL_REGISTER_FEATURE_PHASED("gx-device-create", true, InstallDeviceCreate, ::wxl::hook::Phase::Boot)
