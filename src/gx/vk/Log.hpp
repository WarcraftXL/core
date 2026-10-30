// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What the renderer has to say, handed to whoever hosts it: the runtime writes it to the launch's
// log under the tag "gx.vk", the native tests to their standard output. The renderer itself knows
// no log file and no platform, so its lower modules build for Linux as they are.

#pragma once

#include <cstdarg>

namespace wxl::vk
{
    enum class LogLevel
    {
        Debug,
        Info,
        Warn,
        Error,
    };

    /// Receives one line, without its end of line. Called on the thread that logged.
    using LogSink = void (*)(LogLevel level, const char* text, void* user);

    /// Where the renderer's lines go from now on; null drops them.
    void SetLogSink(LogSink sink, void* user);

    /// A printf-style line; longer than 1 KB is cut.
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    void Log(LogLevel level, const char* format, ...);
}
