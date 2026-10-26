// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// WarcraftXL.ini, next to the client executable: what the player sets without an environment
// variable.

#pragma once

#include "common/Log.hpp"

#include <cstddef>
#include <cstdint>

namespace wxl::core::settings
{
    /// The file's name, next to the client executable.
    constexpr wchar_t kFileName[] = L"WarcraftXL.ini";

    /// `[gx] api`: which graphics API the client creates, over its own choice.
    enum class GxApi : uint8_t
    {
        client, ///< no line: the client's own choice (gxApi, its command line)
        d3d9,   ///< D3D9, a Vulkan request included
        vulkan, ///< Vulkan, a D3D9 or D3D9Ex request included; D3D9 when the machine cannot
    };

    /// What the file says. Every member has the value the runtime uses without the file.
    struct Settings
    {
        bool            found     = false; ///< the file exists and was read
        bool            has_level = false; ///< [log] level was given
        wxl::log::Level level     = wxl::log::Level::Info;
        unsigned        keep      = 30;    ///< earlier launches' logs kept beside the current one

        struct Tag
        {
            char            name[64];
            wxl::log::Level level;
        };
        Tag      tags[32] = {};
        unsigned tag_count = 0;

        bool     has_profile         = false; ///< a [profile] section was there
        bool     has_profile_enabled = false; ///< [profile] enabled was given
        bool     profile_enabled     = false;
        uint16_t profile_port        = 8086;
        bool     profile_any         = false; ///< listen = any: every interface, not 127.0.0.1 only

        GxApi    gx_api        = GxApi::client;
        bool     gx_validation = false;  ///< [gx] validation = 1: Vulkan's validation layer, when installed
        char     gx_gpu[64]    = {};     ///< [gx] gpu: the device's index, or a part of its name
        unsigned gx_frames     = 2;      ///< [gx] frames_in_flight: 1 to 3
        bool     gx_nearest    = false;  ///< [gx] present_filter = nearest: the scene stretched without blending

        /// Lines of the file that were not understood, as messages for the log, at most 8.
        char     problems[8][160] = {};
        unsigned problem_count    = 0;
    };

    /// Whether the profiler starts, where it listens, and what decided it.
    struct ProfileChoice
    {
        bool        enabled  = false;
        bool        from_env = false;   ///< WXL_PROFILE in the environment decided, over the file
        uint16_t    port     = 8086;
        bool        any      = false;   ///< every interface; otherwise 127.0.0.1 only
        const char* ignored  = nullptr; ///< a WXL_PROFILE value that is neither 0 nor 1, ignored
    };

    /**
     * @brief Reads settings from the text of an ini file.
     *
     * Three sections are read, `[log]`, `[profile]` and `[gx]`:
     *
     *     [log]
     *     level = debug          ; trace, debug, info, warn, error or off
     *     keep = 30              ; earlier launches kept in Logs\, 0 for none
     *     level.wxl.probe = info ; a level for one tag: a mod's id, "host", "core"...
     *
     *     [profile]
     *     enabled = 1            ; 0 or 1: the Tracy profiler, off without this line
     *     port = 8086            ; the TCP port a viewer connects to, 1 to 65535
     *     listen = localhost     ; localhost (127.0.0.1 only) or any (every interface)
     *
     *     [gx]
     *     api = vulkan           ; vulkan or d3d9; without it, the client's own choice
     *     validation = 1         ; 0 or 1: Vulkan's validation layer, when it is installed
     *     gpu = 1                ; the Vulkan device: its index, or a part of its name
     *     frames_in_flight = 2   ; 1 to 3: frames the CPU may record ahead of the GPU
     *     present_filter = linear ; linear or nearest: how the scene is stretched to the window
     *
     * `;` and `#` start a comment. Other sections are left for later uses; an unknown key or a
     * value that does not read is noted in `problems` and otherwise ignored.
     */
    void Parse(const char* text, size_t size, Settings& out);

    /// Reads @p path (kernel32 only, safe under the loader lock); `found` is false without it.
    void Load(const wchar_t* path, Settings& out);

    /**
     * @brief Hands the levels to the log: the file's level unless WXL_LOG_LEVEL is set, which
     *        wins, and every tag's own.
     */
    void ApplyToLog(const Settings& settings);

    /**
     * @brief The profiler's settings: the file's, with WXL_PROFILE in the environment (@p env,
     *        null when unset) deciding whether it starts when it is 0 or 1.
     */
    ProfileChoice ResolveProfile(const Settings& settings, const char* env);

    /// The same, with WXL_PROFILE read from the process's environment.
    ProfileChoice ResolveProfile(const Settings& settings);
}
