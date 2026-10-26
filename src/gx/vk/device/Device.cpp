// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The loader, the instance, the physical device and the logical device.

#include "gx/vk/device/Device.hpp"

#include "gx/vk/Log.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    using wxl::vk::Log;
    using wxl::vk::LogLevel;

    constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

    bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name)
    {
        for (const VkExtensionProperties& e : list)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
    {
        const LogLevel level = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT     ? LogLevel::Error
                               : severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ? LogLevel::Warn
                                                                                             : LogLevel::Debug;
        Log(level, "validation: %s", data && data->pMessage ? data->pMessage : "(no message)");
        return VK_FALSE;
    }

    VkDebugUtilsMessengerCreateInfoEXT MessengerInfo()
    {
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = OnValidation;
        return info;
    }

    /// The feature structures, chained: 1.0 in features2, then 1.2 and 1.3.
    struct FeatureChain
    {
        VkPhysicalDeviceFeatures2        features2{};
        VkPhysicalDeviceVulkan12Features v12{};
        VkPhysicalDeviceVulkan13Features v13{};

        FeatureChain()
        {
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            v12.sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            v13.sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            features2.pNext = &v12;
            v12.pNext       = &v13;
        }
        FeatureChain(const FeatureChain&)            = delete;
        FeatureChain& operator=(const FeatureChain&) = delete;
    };
}

namespace wxl::vk::device
{
    void Describe(VkInstance instance, VkPhysicalDevice physical, uint32_t index, PresentQuery presents,
                  Candidate& out)
    {
        out       = Candidate{};
        out.index = index;

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::snprintf(out.name, sizeof out.name, "%s", properties.deviceName);
        out.type      = properties.deviceType;
        out.api       = properties.apiVersion;
        out.driver    = properties.driverVersion;
        out.vendor    = properties.vendorID;
        out.device_id = properties.deviceID;

        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        for (uint32_t h = 0; h < memory.memoryHeapCount; ++h)
            if (memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) out.device_local += memory.memoryHeaps[h].size;

        // The first graphics family, or the first that also presents when that is asked.
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
        for (uint32_t f = 0; f < familyCount; ++f)
        {
            if (!families[f].queueCount || !(families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            const bool can = presents ? presents(instance, physical, f) : false;
            if (out.family == UINT32_MAX || (can && !out.presents))
            {
                out.family   = f;
                out.presents = can;
            }
            if (can) break;
        }

        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (extensionCount) vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, extensions.data());
        extensions.resize(extensionCount);
#define WXL_VK_EXTENSION_HAS(member, name, enabled) out.extensions.member = HasExtension(extensions, name);
        WXL_VK_EXTENSIONS(WXL_VK_EXTENSION_HAS)
#undef WXL_VK_EXTENSION_HAS

        // The 1.2 and 1.3 structures may only be chained for a device of that version; below the
        // floor the device is refused on its version anyway.
        if (out.api < kFloorApi) return;
        FeatureChain chain;
        vkGetPhysicalDeviceFeatures2(physical, &chain.features2);
        const VkPhysicalDeviceFeatures&        core10 = chain.features2.features;
        const VkPhysicalDeviceVulkan12Features& v12   = chain.v12;
        const VkPhysicalDeviceVulkan13Features& v13   = chain.v13;
#define WXL_VK_FEATURE_HAS(member, version, required, where) out.features.member = where.member == VK_TRUE;
        WXL_VK_FEATURES(WXL_VK_FEATURE_HAS)
#undef WXL_VK_FEATURE_HAS
    }

    bool Context::Create(const Options& options, char* why, uint32_t why_size)
    {
        Destroy();
        lost_.store(false, std::memory_order_relaxed);
        want_swapchain_ = options.swapchain;
        if (!CreateInstance(options, why, why_size) || !ChoosePhysical(options, why, why_size) ||
            !CreateDevice(why, why_size))
        {
            Destroy();
            return false;
        }
        return true;
    }

    bool Context::CreateInstance(const Options& options, char* why, uint32_t why_size)
    {
        if (volkInitialize() != VK_SUCCESS)
        {
#if defined(_WIN32)
            std::snprintf(why, why_size, "no Vulkan loader (vulkan-1.dll is not installed: the graphics driver brings it)");
#else
            std::snprintf(why, why_size, "no Vulkan loader (libvulkan.so.1 not found)");
#endif
            return false;
        }
        loader_ = true;

        instance_api_ = volkGetInstanceVersion();
        char version[32];
        FormatVersion(instance_api_, version, sizeof version);
        if (instance_api_ < kFloorApi)
        {
            std::snprintf(why, why_size, "the Vulkan loader is %s, below the 1.3 floor (update the graphics driver)", version);
            return false;
        }

        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        if (count) vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        available.resize(count);

        std::vector<const char*> extensions;
        for (uint32_t i = 0; i < options.instance_extension_count; ++i)
        {
            const char* name = options.instance_extensions[i];
            if (!HasExtension(available, name))
            {
                std::snprintf(why, why_size, "the Vulkan loader has no %s", name);
                return false;
            }
            extensions.push_back(name);
        }

        std::vector<const char*> layers;
        bool                     debugUtils = false;
        if (options.validation)
        {
            uint32_t layerCount = 0;
            vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            std::vector<VkLayerProperties> layerList(layerCount);
            if (layerCount) vkEnumerateInstanceLayerProperties(&layerCount, layerList.data());
            bool present = false;
            for (uint32_t i = 0; i < layerCount; ++i)
                present = present || std::strcmp(layerList[i].layerName, kValidationLayer) == 0;
            // Asked for is not required: a missing layer is a line, not a refused device.
            if (present && HasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
            {
                layers.push_back(kValidationLayer);
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                debugUtils = true;
            }
            else
                Log(LogLevel::Warn, "[gx] validation = 1, but %s; going on without it",
                    present ? "the loader has no " VK_EXT_DEBUG_UTILS_EXTENSION_NAME : "VK_LAYER_KHRONOS_validation is not installed");
        }

        VkApplicationInfo app{};
        app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName   = options.application;
        app.applicationVersion = VK_MAKE_API_VERSION(0, 3, 3, 5);
        app.pEngineName        = "WarcraftXL";
        app.engineVersion      = VK_MAKE_API_VERSION(0, 0, 2, 0);
        app.apiVersion         = kFloorApi;

        const VkDebugUtilsMessengerCreateInfoEXT messenger = MessengerInfo();
        VkInstanceCreateInfo                     info{};
        info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pNext                   = debugUtils ? &messenger : nullptr; // the layer's word on vkCreateInstance itself
        info.pApplicationInfo        = &app;
        info.enabledLayerCount       = static_cast<uint32_t>(layers.size());
        info.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();
        info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

        VkResult result = vkCreateInstance(&info, nullptr, &instance_);
        if (result != VK_SUCCESS && !layers.empty())
        {
            // A layer that enumerates may still not load: a 32-bit process finding a 64-bit
            // layer registered is the usual case. Validation is dropped, not the device.
            Log(LogLevel::Warn, "vkCreateInstance with %s failed (%s); again without it", kValidationLayer, ResultName(result));
            extensions.pop_back();
            info.pNext                   = nullptr;
            info.enabledLayerCount       = 0;
            info.ppEnabledLayerNames     = nullptr;
            info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
            info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
            debugUtils                   = false;
            instance_                    = VK_NULL_HANDLE;
            result                       = vkCreateInstance(&info, nullptr, &instance_);
        }
        if (result != VK_SUCCESS)
        {
            instance_ = VK_NULL_HANDLE;
            char asked[256] = {};
            size_t used     = 0;
            for (const char* name : extensions)
            {
                const int wrote = std::snprintf(asked + used, sizeof asked - used, "%s%s", used ? ", " : "", name);
                if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof asked - used) break;
                used += static_cast<size_t>(wrote);
            }
            if (result == VK_ERROR_EXTENSION_NOT_PRESENT)
                std::snprintf(why, why_size, "the Vulkan loader lists but refuses the instance extensions %s (%s)", asked,
                              ResultName(result));
            else
                std::snprintf(why, why_size, "vkCreateInstance failed: %s", ResultName(result));
            return false;
        }
        volkLoadInstanceOnly(instance_);
        validation_ = debugUtils;
        if (debugUtils && vkCreateDebugUtilsMessengerEXT(instance_, &messenger, nullptr, &messenger_) != VK_SUCCESS)
            messenger_ = VK_NULL_HANDLE;

        Log(LogLevel::Debug, "instance: loader %s, Vulkan 1.3 asked, %u extensions, validation %s", version,
            static_cast<unsigned>(extensions.size()), validation_ ? "on" : "off");
        return true;
    }

    bool Context::ChoosePhysical(const Options& options, char* why, uint32_t why_size)
    {
        uint32_t count  = 0;
        VkResult result = vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            std::snprintf(why, why_size, "vkEnumeratePhysicalDevices failed: %s", ResultName(result));
            return false;
        }
        if (!count)
        {
            std::snprintf(why, why_size, "the Vulkan loader finds no device (no driver offers Vulkan)");
            return false;
        }
        std::vector<VkPhysicalDevice> handles(count);
        vkEnumeratePhysicalDevices(instance_, &count, handles.data());
        handles.resize(count);

        std::vector<Candidate> candidates(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            Candidate& c = candidates[i];
            Describe(instance_, handles[i], i, options.presents, c);
            Judge(c, options.floor);
            char api[32];
            FormatVersion(c.api, api, sizeof api);
            Log(LogLevel::Debug, "device %u: %s (%s, Vulkan %s, %04x:%04x, %llu MiB device memory)%s%s", i, c.name,
                TypeName(c.type), api, c.vendor, c.device_id, static_cast<unsigned long long>(c.device_local >> 20),
                c.reject[0] ? ": does not qualify, " : ": qualifies", c.reject);
        }

        const Choice choice = Choose(candidates.data(), count, options.gpu);
        if (choice.note[0]) Log(LogLevel::Warn, "%s; choosing among the others", choice.note);
        if (choice.index < 0)
        {
            char refusals[400];
            Refusals(candidates.data(), count, refusals, sizeof refusals);
            std::snprintf(why, why_size, "no device meets the Vulkan %u.%u floor (%s)", VK_API_VERSION_MAJOR(options.floor.api),
                          VK_API_VERSION_MINOR(options.floor.api), refusals);
            return false;
        }
        physical_ = handles[static_cast<size_t>(choice.index)];
        chosen_   = candidates[static_cast<size_t>(choice.index)];
        vkGetPhysicalDeviceProperties(physical_, &properties_);
        Log(LogLevel::Debug, "device %u taken: %s", chosen_.index, choice.why);
        return true;
    }

    bool Context::CreateDevice(char* why, uint32_t why_size)
    {
        // The features enabled are those read, required and optional alike: the chain is the one
        // Describe filled, so a feature is on exactly when the device has it.
        FeatureChain                     chain;
        VkPhysicalDeviceFeatures&        core10 = chain.features2.features;
        VkPhysicalDeviceVulkan12Features& v12   = chain.v12;
        VkPhysicalDeviceVulkan13Features& v13   = chain.v13;
#define WXL_VK_FEATURE_ENABLE(member, version, required, where) where.member = chosen_.features.member ? VK_TRUE : VK_FALSE;
        WXL_VK_FEATURES(WXL_VK_FEATURE_ENABLE)
#undef WXL_VK_FEATURE_ENABLE
        (void)core10;
        (void)v12;
        (void)v13;

        std::vector<const char*> extensions;
#define WXL_VK_EXTENSION_ENABLE(member, name, enabled) \
    if (chosen_.extensions.member && enabled) extensions.push_back(name);
        WXL_VK_EXTENSIONS(WXL_VK_EXTENSION_ENABLE)
#undef WXL_VK_EXTENSION_ENABLE
        // The swapchain is enabled with the window it serves, or when asked for (a headless surface).
        if ((chosen_.presents || want_swapchain_) && chosen_.extensions.swapchain) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        const float             priority = 1.0f;
        VkDeviceQueueCreateInfo queue{};
        queue.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = chosen_.family;
        queue.queueCount       = 1;
        queue.pQueuePriorities = &priority;

        VkDeviceCreateInfo info{};
        info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.pNext                   = &chain.features2;
        info.queueCreateInfoCount    = 1;
        info.pQueueCreateInfos       = &queue;
        info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

        const VkResult result = vkCreateDevice(physical_, &info, nullptr, &device_);
        if (result != VK_SUCCESS)
        {
            device_ = VK_NULL_HANDLE;
            std::snprintf(why, why_size, "vkCreateDevice on %s failed: %s", chosen_.name, ResultName(result));
            return false;
        }
        volkLoadDevice(device_);
        vkGetDeviceQueue(device_, chosen_.family, 0, &queue_);
        return true;
    }

    bool Context::Check(VkResult result, const char* what)
    {
        if (result == VK_SUCCESS) return true;
        if (result == VK_ERROR_DEVICE_LOST) MarkLost(what);
        return false;
    }

    void Context::MarkLost(const char* what)
    {
        if (!lost_.exchange(true, std::memory_order_relaxed))
            Log(LogLevel::Error, "the device was lost (%s); nothing more is submitted to it", what);
    }

    void Context::Destroy()
    {
        if (device_)
        {
            // No vkDeviceWaitIdle: it has no bound. Whoever submitted waits for its own serials
            // (bounded) before the device goes; a lost or hung device is destroyed as it is.
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
            queue_  = VK_NULL_HANDLE;
        }
        if (messenger_)
        {
            vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
            messenger_ = VK_NULL_HANDLE;
        }
        if (instance_)
        {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        if (loader_)
        {
            // The loader goes with the device, so a client that falls back to D3D9 keeps no
            // Vulkan driver mapped.
            volkFinalize();
            loader_ = false;
        }
        physical_   = VK_NULL_HANDLE;
        chosen_     = Candidate{};
        properties_ = {};
        validation_ = false;
    }
}
