// SPDX-License-Identifier: GPL-3.0-or-later
// The leveled log of each binary (the DLL and the patcher each have their own).

#pragma once

#include <cstdint>

/// Runtime threshold: WXL_LOG_LEVEL (trace|debug|info|warn|error|off, or 0-5), Info by default; a
/// line below it costs one atomic load. Every line is flushed as it is written, so a crash loses none;
/// WXL_LOG_FLUSH=0 flushes Warn and above at once and the rest every 64 lines.
namespace wxl::log
{
    enum class Level : uint8_t
    {
        Trace = 0, ///< per item, for one bug hunt
        Debug = 1, ///< per event (per model, per open)
        Info  = 2, ///< startup summaries, one-shot confirmations
        Warn  = 3, ///< unexpected but survivable
        Error = 4, ///< something does not work
        Off   = 5, ///< a threshold only: nothing is written
    };

    /// Opens the file sink (truncated: one file per session), once.
    void Open(const char* path);

    /// Writes to the console too (command-line tools): Warn and above to stderr, the rest to stdout.
    void EnableConsole(const char* prefix);

    /// Whether a line at `level` would be written; the WLOG_* macros format nothing otherwise.
    bool Enabled(Level level);

    /// One line; thread-safe.
    void Write(Level level, const char* fmt, ...);

    /// Writes `last` as an Info line, flushes and closes the file sink (process exit). Skipped when a
    /// thread ended while holding the log, so it never blocks the exit.
    void Close(const char* last);
}

// Levels below the compile-time floor compile to nothing: Release strips Trace.
#ifndef WXL_LOG_COMPILE_MIN
#  ifdef NDEBUG
#    define WXL_LOG_COMPILE_MIN 1
#  else
#    define WXL_LOG_COMPILE_MIN 0
#  endif
#endif

#define WXL_LOG_AT(lvl, ...) \
    do { if (::wxl::log::Enabled(lvl)) ::wxl::log::Write(lvl, __VA_ARGS__); } while (0)

#if WXL_LOG_COMPILE_MIN <= 0
#  define WLOG_TRACE(...) WXL_LOG_AT(::wxl::log::Level::Trace, __VA_ARGS__)
#else
#  define WLOG_TRACE(...) ((void)0)
#endif

#if WXL_LOG_COMPILE_MIN <= 1
#  define WLOG_DEBUG(...) WXL_LOG_AT(::wxl::log::Level::Debug, __VA_ARGS__)
#else
#  define WLOG_DEBUG(...) ((void)0)
#endif

#define WLOG_INFO(...)  WXL_LOG_AT(::wxl::log::Level::Info,  __VA_ARGS__)
#define WLOG_WARN(...)  WXL_LOG_AT(::wxl::log::Level::Warn,  __VA_ARGS__)
#define WLOG_ERROR(...) WXL_LOG_AT(::wxl::log::Level::Error, __VA_ARGS__)

/// An info line the first time this call site runs.
#define WLOG_INFO_ONCE(...)                                                                        \
    do                                                                                             \
    {                                                                                              \
        static bool wxl_logged = false;                                                            \
        if (!wxl_logged)                                                                           \
        {                                                                                          \
            wxl_logged = true;                                                                     \
            WLOG_INFO(__VA_ARGS__);                                                                \
        }                                                                                          \
    } while (0)
