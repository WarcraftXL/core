// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/Log.hpp"

#include "common/Config.hpp"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace
{
    using wxl::log::Level;

    constexpr unsigned kFlushEvery   = 64;
    constexpr size_t   kFileBuffer   = 64 * 1024;
    constexpr size_t   kMaxLine      = 1024;
    constexpr size_t   kMaxLevelText = 16;

    FILE*              g_file = nullptr;
    std::mutex         g_mutex;
    std::atomic<Level> g_minLevel{ Level::Info };
    bool               g_minLevelResolved = false;
    bool               g_console          = false;
    char               g_consolePrefix[32] = {};

    bool FlushEveryLine()
    {
        static const bool enabled = wxl::config::Env("WXL_LOG_FLUSH", true);
        return enabled;
    }

    bool DebugStringEnabled()
    {
        static const bool enabled = wxl::config::Env("WXL_LOG_DEBUGSTRING", IsDebuggerPresent() != 0);
        return enabled;
    }

    /// WXL_LOG_LEVEL: a name (only its first letter counts) or a digit; Info when absent or garbled.
    Level LevelFromEnv()
    {
        char raw[kMaxLevelText] = {};
        if (!wxl::config::Raw("WXL_LOG_LEVEL", raw, sizeof raw)) return Level::Info;
        if (*raw >= '0' && *raw <= '5') return static_cast<Level>(*raw - '0');
        switch (*raw | 0x20)
        {
        case 't': return Level::Trace;
        case 'd': return Level::Debug;
        case 'i': return Level::Info;
        case 'w': return Level::Warn;
        case 'e': return Level::Error;
        case 'o': return Level::Off;
        default: return Level::Info;
        }
    }

    /// Resolved on first use; two threads racing store the same value.
    Level MinLevel()
    {
        if (!g_minLevelResolved)
        {
            g_minLevel.store(LevelFromEnv(), std::memory_order_relaxed);
            g_minLevelResolved = true;
        }
        return g_minLevel.load(std::memory_order_relaxed);
    }

    const char* Tag(Level level)
    {
        switch (level)
        {
        case Level::Trace: return "TRACE: ";
        case Level::Debug: return "DEBUG: ";
        case Level::Warn: return "WARN: ";
        case Level::Error: return "ERROR: ";
        default: return "";
        }
    }

    void Emit(Level level, const char* line)
    {
        static unsigned pending = 0;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (DebugStringEnabled()) OutputDebugStringA(line);
        if (g_console)
        {
            FILE* out = level >= Level::Warn ? stderr : stdout;
            if (g_consolePrefix[0]) fputs(g_consolePrefix, out);
            fputs(line, out);
        }
        if (!g_file) return;
        fputs(line, g_file);
        if (++pending < kFlushEvery && level < Level::Warn && !FlushEveryLine()) return;
        fflush(g_file);
        pending = 0;
    }

    /// The folder of `path`, made if missing: a missing Logs\ would lose every line silently.
    void MakeParent(const char* path)
    {
        char         dir[MAX_PATH];
        const size_t length = strnlen(path, MAX_PATH);
        if (length >= MAX_PATH) return;
        memcpy(dir, path, length + 1);
        for (size_t i = length; i-- > 0;)
        {
            if (dir[i] != '\\' && dir[i] != '/') continue;
            dir[i] = '\0';
            if (dir[0]) CreateDirectoryA(dir, nullptr);
            return;
        }
    }
}

namespace wxl::log
{
    void Open(const char* path)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file || !path) return;
        MakeParent(path);
        fopen_s(&g_file, path, "w");
        if (g_file) setvbuf(g_file, nullptr, _IOFBF, kFileBuffer);
    }

    void EnableConsole(const char* prefix)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_console = true;
        if (prefix) strncpy_s(g_consolePrefix, prefix, _TRUNCATE);
    }

    bool Enabled(Level level) { return level >= MinLevel(); }

    void Close(const char* last)
    {
        if (!g_mutex.try_lock()) return;
        if (g_file)
        {
            if (last && Enabled(Level::Info))
            {
                SYSTEMTIME time;
                GetLocalTime(&time);
                fprintf(g_file, "[%02d:%02d:%02d] %s%s\n", time.wHour, time.wMinute, time.wSecond, Tag(Level::Info), last);
            }
            fclose(g_file);
            g_file = nullptr;
        }
        g_mutex.unlock();
    }

    void Write(Level level, const char* fmt, ...)
    {
        char    body[kMaxLine];
        va_list args;
        va_start(args, fmt);
        vsnprintf(body, sizeof body, fmt, args);
        va_end(args);

        SYSTEMTIME time;
        GetLocalTime(&time);
        char line[kMaxLine + 128];
        snprintf(line, sizeof line, "[%02d:%02d:%02d] %s%s\n", time.wHour, time.wMinute, time.wSecond, Tag(level), body);
        Emit(level, line);
    }
}
