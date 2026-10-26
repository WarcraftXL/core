// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The renderer's small shared pieces: the log hand-off and the names of results and versions.

#include "gx/vk/Log.hpp"
#include "gx/vk/Vk.hpp"

#include <cstdio>

namespace
{
    wxl::vk::LogSink g_sink = nullptr;
    void*            g_user = nullptr;
}

namespace wxl::vk
{
    void SetLogSink(LogSink sink, void* user)
    {
        g_sink = sink;
        g_user = user;
    }

    void Log(LogLevel level, const char* format, ...)
    {
        if (!g_sink) return;
        char    text[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof text, format, args);
        va_end(args);
        g_sink(level, text, g_user);
    }

    void FormatVersion(uint32_t version, char* out, uint32_t size)
    {
        std::snprintf(out, size, "%u.%u.%u", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                      VK_API_VERSION_PATCH(version));
    }

    const char* ResultName(VkResult result)
    {
        switch (result)
        {
#define WXL_VK_RESULT(name) \
    case name: return #name;
            WXL_VK_RESULT(VK_SUCCESS)
            WXL_VK_RESULT(VK_NOT_READY)
            WXL_VK_RESULT(VK_TIMEOUT)
            WXL_VK_RESULT(VK_EVENT_SET)
            WXL_VK_RESULT(VK_EVENT_RESET)
            WXL_VK_RESULT(VK_INCOMPLETE)
            WXL_VK_RESULT(VK_ERROR_OUT_OF_HOST_MEMORY)
            WXL_VK_RESULT(VK_ERROR_OUT_OF_DEVICE_MEMORY)
            WXL_VK_RESULT(VK_ERROR_INITIALIZATION_FAILED)
            WXL_VK_RESULT(VK_ERROR_DEVICE_LOST)
            WXL_VK_RESULT(VK_ERROR_MEMORY_MAP_FAILED)
            WXL_VK_RESULT(VK_ERROR_LAYER_NOT_PRESENT)
            WXL_VK_RESULT(VK_ERROR_EXTENSION_NOT_PRESENT)
            WXL_VK_RESULT(VK_ERROR_FEATURE_NOT_PRESENT)
            WXL_VK_RESULT(VK_ERROR_INCOMPATIBLE_DRIVER)
            WXL_VK_RESULT(VK_ERROR_TOO_MANY_OBJECTS)
            WXL_VK_RESULT(VK_ERROR_FORMAT_NOT_SUPPORTED)
            WXL_VK_RESULT(VK_ERROR_FRAGMENTED_POOL)
            WXL_VK_RESULT(VK_ERROR_UNKNOWN)
            WXL_VK_RESULT(VK_ERROR_OUT_OF_POOL_MEMORY)
            WXL_VK_RESULT(VK_ERROR_INVALID_EXTERNAL_HANDLE)
            WXL_VK_RESULT(VK_ERROR_FRAGMENTATION)
            WXL_VK_RESULT(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
            WXL_VK_RESULT(VK_PIPELINE_COMPILE_REQUIRED)
            WXL_VK_RESULT(VK_ERROR_SURFACE_LOST_KHR)
            WXL_VK_RESULT(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
            WXL_VK_RESULT(VK_SUBOPTIMAL_KHR)
            WXL_VK_RESULT(VK_ERROR_OUT_OF_DATE_KHR)
            WXL_VK_RESULT(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR)
            WXL_VK_RESULT(VK_ERROR_VALIDATION_FAILED_EXT)
#undef WXL_VK_RESULT
        default:
        {
            // One line at a time reads it: the text is the caller's until the next unknown result.
            static thread_local char text[32];
            std::snprintf(text, sizeof text, "VkResult %d", static_cast<int>(result));
            return text;
        }
        }
    }
}
