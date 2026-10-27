// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The log: a launch's file kept for the next, the oldest pruned, levels by tag, continuation
// lines, WarcraftXL.ini and the graphics API its [gx] section selects. Run with a scratch folder:
//
//     wxl-core-log-tests <scratch>

#include "common/Log.hpp"
#include "gx/Select.hpp"
#include "runtime/Settings.hpp"

#include "wxl/core/gx_vk.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* what)
    {
        std::printf("  %s  %s\n", condition ? "ok  " : "FAIL", what);
        if (!condition) ++g_failures;
    }

    std::wstring Wide(const char* text)
    {
        std::wstring out;
        for (; *text; ++text) out += static_cast<wchar_t>(*text == '/' ? '\\' : *text);
        return out;
    }

    std::string Read(const std::wstring& path)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return std::string();
        std::string text(1 << 16, '\0');
        DWORD       read = 0;
        ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
        CloseHandle(file);
        text.resize(read);
        return text;
    }

    std::vector<std::wstring> Logs(const std::wstring& dir)
    {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW          found;
        HANDLE                    search = FindFirstFileW((dir + L"\\*.log").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) return names;
        do names.push_back(found.cFileName);
        while (FindNextFileW(search, &found));
        FindClose(search);
        return names;
    }

    bool Has(const std::vector<std::wstring>& names, const wchar_t* name)
    {
        for (const std::wstring& one : names)
            if (one == name) return true;
        return false;
    }

    /// One launch: opens the session, writes its first line, closes.
    wxl::log::SessionFiles Launch(const std::wstring& dir, unsigned keep, const char* launch)
    {
        wxl::log::SessionFiles files;
        wxl::log::OpenSession(dir.c_str(), L"WarcraftXL", keep, &files);
        if (launch) wxl::log::WriteUnfiltered(wxl::log::Level::Info, "session", launch);
        wxl::log::Close();
        return files;
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2)
    {
        std::printf("usage: wxl-core-log-tests <scratch>\n");
        return 2;
    }
    const std::wstring scratch = Wide(argv[1]);
    CreateDirectoryW(scratch.c_str(), nullptr);

    std::printf("a launch's log is kept for the next\n");
    {
        const std::wstring dir = scratch + L"\\Logs";
        for (const std::wstring& name : Logs(dir)) DeleteFileW((dir + L"\\" + name).c_str());

        wxl::log::SessionFiles first = Launch(dir, 2, "launch 2026-10-25 14:03:22 local, pid 1");
        Check(!first.kept_previous && Logs(dir).size() == 1 && Has(Logs(dir), L"WarcraftXL.log"),
              "the first launch writes WarcraftXL.log, creating Logs\\");
        Check(Read(dir + L"\\WarcraftXL.log").find("] [INFO ] [session] launch 2026-10-25 14:03:22") != std::string::npos,
              "with the line the next launch reads its time from");

        wxl::log::SessionFiles second = Launch(dir, 2, "launch 2026-10-25 15:00:00 local, pid 2");
        Check(second.kept_previous && std::wstring(second.previous) == L"WarcraftXL-2026-10-25_14-03-22.log" &&
                  Has(Logs(dir), L"WarcraftXL-2026-10-25_14-03-22.log") && second.kept == 1,
              "the second renames it after its launch time");
        Check(Read(dir + L"\\WarcraftXL.log").find("15:00:00") != std::string::npos &&
                  Read(dir + L"\\WarcraftXL-2026-10-25_14-03-22.log").find("14:03:22") != std::string::npos,
              "each file holds its own launch");

        Launch(dir, 2, "launch 2026-10-26 09:00:00 local, pid 3");
        wxl::log::SessionFiles fourth = Launch(dir, 2, "launch 2026-10-26 10:00:00 local, pid 4");
        const auto names = Logs(dir);
        Check(names.size() == 3 && !Has(names, L"WarcraftXL-2026-10-25_14-03-22.log") &&
                  Has(names, L"WarcraftXL-2026-10-25_15-00-00.log") && Has(names, L"WarcraftXL-2026-10-26_09-00-00.log") &&
                  fourth.kept == 2 && fourth.removed == 1,
              "past keep, the oldest go");

        // A launch that wrote the same time (two launches in one second).
        Launch(dir, 2, "launch 2026-10-26 10:00:00 local, pid 5");
        Check(Has(Logs(dir), L"WarcraftXL-2026-10-26_10-00-00.log"), "a name taken already is not overwritten");
        Launch(dir, 5, "launch 2026-10-26 10:00:00 local, pid 6");
        Check(Has(Logs(dir), L"WarcraftXL-2026-10-26_10-00-00-2.log"), "the next one is numbered");

        Launch(dir, 5, nullptr);
        Launch(dir, 5, "launch 2026-10-27 07:00:00 local, pid 8");
        bool dated = false;
        for (const std::wstring& name : Logs(dir))
            dated = dated || (name.size() == std::wstring(L"WarcraftXL-0000-00-00_00-00-00.log").size() &&
                              name.compare(0, 11, L"WarcraftXL-") == 0);
        Check(dated, "a log without a launch line is named after its last write");

        Launch(dir, 0, "launch 2026-10-27 08:00:00 local, pid 7");
        Check(Logs(dir).size() == 1, "keep 0 keeps none");
    }

    std::printf("levels\n");
    {
        using wxl::log::Level;
        const std::wstring dir = scratch + L"\\Levels";
        wxl::log::SessionFiles files;
        wxl::log::OpenSession(dir.c_str(), L"WarcraftXL", 0, &files);
        wxl::log::SetMinLevel(Level::Warn);
        Check(wxl::log::MinLevel() == Level::Warn && !wxl::log::Enabled(Level::Info), "one level for every tag");
        Check(wxl::log::SetTagLevel("chatty", Level::Debug) && wxl::log::SetTagLevel("quiet", Level::Error),
              "a tag gets a level of its own");
        Check(wxl::log::MinLevel() == Level::Debug && wxl::log::Enabled(Level::Debug) &&
                  wxl::log::LevelFor("chatty") == Level::Debug && wxl::log::LevelFor("other") == Level::Warn,
              "the gate opens to the lowest of them");
        wxl::log::WriteLine(Level::Info, "core", "not written: core is at warn");
        wxl::log::WriteLine(Level::Debug, "chatty", "written: chatty is at debug");
        wxl::log::WriteLine(Level::Warn, "quiet", "not written: quiet is at error");
        wxl::log::WriteLine(Level::Error, "core", "first line\nstack traceback:\n\tinit.lua:3");
        wxl::log::WriteUnfiltered(Level::Info, "session", "written whatever the levels");
        wxl::log::WriteUrgent("crash", "urgent", true);
        wxl::log::Close();
        const std::string text = Read(files.path);
        Check(text.find("not written") == std::string::npos && text.find("[DEBUG] [chatty] written") != std::string::npos,
              "each tag is filtered at its own level");
        Check(text.find("[ERROR] [core] first line\n    stack traceback:\n    \tinit.lua:3\n") != std::string::npos,
              "a message's further lines are indented under it");
        Check(text.find("[session] written whatever") != std::string::npos &&
                  text.find("[ERROR] [crash] urgent") != std::string::npos,
              "an unfiltered line and an urgent one are written");
        wxl::log::SetMinLevel(Level::Info);
    }

    std::printf("WarcraftXL.ini\n");
    {
        using wxl::log::Level;
        const char text[] = "\xEF\xBB\xBF; WarcraftXL settings\r\n"
                            "[log]\r\n"
                            "level = Debug   ; everything but trace\r\n"
                            "keep=5\r\n"
                            "level.wxl.probe = trace\r\n"
                            "level.host = warn\r\n"
                            "colour = blue\r\n"
                            "level.core = loud\r\n"
                            "[other]\r\n"
                            "level = error\r\n";
        wxl::core::settings::Settings settings;
        wxl::core::settings::Parse(text, sizeof text - 1, settings);
        Check(settings.found && settings.has_level && settings.level == Level::Debug && settings.keep == 5,
              "[log] level and keep, with comments, CRLF and a byte order mark");
        Check(settings.tag_count == 2 && std::strcmp(settings.tags[0].name, "wxl.probe") == 0 &&
                  settings.tags[0].level == Level::Trace && settings.tags[1].level == Level::Warn,
              "level.<tag>, a tag holding dots included");
        Check(settings.problem_count == 2 && std::strstr(settings.problems[0], "line 7") &&
                  std::strstr(settings.problems[0], "colour") && std::strstr(settings.problems[1], "loud"),
              "an unknown key and a level that does not read are noted with their line");

        wxl::core::settings::Settings none;
        wxl::core::settings::Load((scratch + L"\\no-such.ini").c_str(), none);
        Check(!none.found && !none.has_level && none.keep == 30, "without the file, the defaults");

        using wxl::core::settings::ResolveProfile;
        Check(!settings.has_profile && !none.has_profile && !none.profile_enabled && none.profile_port == 8086 &&
                  !none.profile_any,
              "without a [profile] section the profiler is off, on 127.0.0.1:8086 when started");
        const auto off = ResolveProfile(none, nullptr);
        Check(!off.enabled && !off.from_env && off.port == 8086 && !off.any, "and nothing starts it");
        const auto forced = ResolveProfile(none, "1");
        Check(forced.enabled && forced.from_env && !forced.any, "WXL_PROFILE=1 starts it, still on 127.0.0.1");

        const char profileText[] = "[profile]\n"
                                   "enabled = 1\n"
                                   "port = 9000 ; another\n"
                                   "listen = ANY\n"
                                   "[log]\n"
                                   "level = warn\n";
        wxl::core::settings::Settings profile;
        wxl::core::settings::Parse(profileText, sizeof profileText - 1, profile);
        Check(profile.has_profile && profile.has_profile_enabled && profile.profile_enabled &&
                  profile.profile_port == 9000 && profile.profile_any && profile.problem_count == 0 &&
                  profile.has_level && profile.level == Level::Warn,
              "[profile] enabled, port and listen, in any case, beside [log]");
        const auto fromFile = ResolveProfile(profile, nullptr);
        Check(fromFile.enabled && !fromFile.from_env && fromFile.port == 9000 && fromFile.any, "the file starts it");
        const auto envOff = ResolveProfile(profile, "0");
        Check(!envOff.enabled && envOff.from_env && !envOff.ignored, "WXL_PROFILE=0 wins over the file's enabled = 1");
        const auto garbled = ResolveProfile(profile, "yes");
        Check(garbled.enabled && !garbled.from_env && garbled.ignored && std::strcmp(garbled.ignored, "yes") == 0,
              "a WXL_PROFILE neither 0 nor 1 is ignored, and said");

        const char badText[] = "[profile]\n"
                               "enabled = yes\n"
                               "port = 0\n"
                               "port = 65536\n"
                               "port = 80a\n"
                               "listen = lan\n"
                               "colour = blue\n"
                               "listen = localhost\n";
        wxl::core::settings::Settings bad;
        wxl::core::settings::Parse(badText, sizeof badText - 1, bad);
        Check(!bad.has_profile_enabled && !bad.profile_enabled && bad.profile_port == 8086 && !bad.profile_any,
              "a value that does not read keeps the default");
        Check(bad.problem_count == 6 && std::strstr(bad.problems[0], "line 2") && std::strstr(bad.problems[0], "yes") &&
                  std::strstr(bad.problems[1], "line 3") && std::strstr(bad.problems[1], "1 to 65535") &&
                  std::strstr(bad.problems[2], "65536") && std::strstr(bad.problems[3], "80a") &&
                  std::strstr(bad.problems[4], "lan") && std::strstr(bad.problems[5], "line 7") &&
                  std::strstr(bad.problems[5], "[profile]"),
              "each value that does not read, and an unknown key, is noted with its line");

        // [gx]: the API over the client's request, Vulkan's validation and device.
        using wxl::core::settings::GxApi;
        Check(none.gx_api == GxApi::client && !none.gx_validation && !none.gx_gpu[0] && none.gx_frames == 2 && !none.gx_nearest &&
                  none.gx_async,
              "without a [gx] section the client's own choice stands, two frames in flight, bilinear, pipelines compiled on workers");
        const char gxText[] = "[GX]\n"
                              "api = Vulkan ; tried first\n"
                              "validation = 1\n"
                              "gpu = GeForce RTX\n"
                              "frames_in_flight = 3\n"
                              "present_filter = Nearest\n"
                              "Async_Pipelines = 0\n"
                              "vk_rebar = 0\n"
                              "VK_PREWARM = 0\n"
                              "vk_gpu_times = 0 ; no timestamps\n";
        wxl::core::settings::Settings gx;
        wxl::core::settings::Parse(gxText, sizeof gxText - 1, gx);
        Check(gx.gx_api == GxApi::vulkan && gx.gx_validation && std::strcmp(gx.gx_gpu, "GeForce RTX") == 0 &&
                  gx.gx_frames == 3 && gx.gx_nearest && !gx.gx_async && !gx.gx_rebar && !gx.gx_prewarm && !gx.gx_gpu_times &&
                  gx.problem_count == 0,
              "[gx] api, validation, gpu, frames_in_flight, present_filter, async_pipelines, vk_rebar, vk_prewarm and vk_gpu_times, in "
              "any case, a comment after a value");
        Check(none.gx_rebar && none.gx_prewarm && none.gx_gpu_times, "ReBAR rings, the prewarm and the GPU's pass times are on by default");
        const char gxRange[] = "[gx]\n"
                               "frames_in_flight = 4\n"
                               "present_filter = cubic\n";
        wxl::core::settings::Settings gxr;
        wxl::core::settings::Parse(gxRange, sizeof gxRange - 1, gxr);
        Check(gxr.gx_frames == 2 && !gxr.gx_nearest && gxr.problem_count == 2 && std::strstr(gxr.problems[0], "1, 2 or 3") &&
                  std::strstr(gxr.problems[1], "linear or nearest"),
              "frames in flight outside 1-3 and an unknown filter are noted, the defaults kept");
        const char gxBad[] = "[gx]\n"
                             "api = d3d11\n"
                             "validation = on\n"
                             "gpu =\n"
                             "vsync = 1\n"
                             "api = d3d9\n";
        wxl::core::settings::Settings gxb;
        wxl::core::settings::Parse(gxBad, sizeof gxBad - 1, gxb);
        Check(gxb.gx_api == GxApi::d3d9 && !gxb.gx_validation && !gxb.gx_gpu[0] && gxb.problem_count == 4 &&
                  std::strstr(gxb.problems[0], "d3d11") && std::strstr(gxb.problems[1], "on") &&
                  std::strstr(gxb.problems[2], "line 4") && std::strstr(gxb.problems[3], "[gx]"),
              "each [gx] value that does not read, and an unknown key, is noted; the last api line holds");

        using wxl::core::gx::SelectApi;
        Check(SelectApi(WXL_GX_API_D3D9, GxApi::client).api == WXL_GX_API_D3D9 &&
                  !SelectApi(WXL_GX_API_D3D9, GxApi::client).why &&
                  SelectApi(WXL_GX_API_VULKAN, GxApi::client).api == WXL_GX_API_VULKAN &&
                  SelectApi(WXL_GX_API_D3D9EX, GxApi::client).api == WXL_GX_API_D3D9EX,
              "without [gx] api the client's request stands, D3D9 by default");
        Check(SelectApi(WXL_GX_API_D3D9, GxApi::vulkan).api == WXL_GX_API_VULKAN &&
                  SelectApi(WXL_GX_API_D3D9EX, GxApi::vulkan).api == WXL_GX_API_VULKAN &&
                  std::strstr(SelectApi(WXL_GX_API_D3D9EX, GxApi::vulkan).why, "D3D9Ex") &&
                  SelectApi(WXL_GX_API_OPENGL, GxApi::vulkan).api == WXL_GX_API_OPENGL,
              "[gx] api = vulkan takes D3D9 and D3D9Ex to Vulkan, and leaves OpenGL");
        Check(SelectApi(WXL_GX_API_VULKAN, GxApi::d3d9).api == WXL_GX_API_D3D9 &&
                  SelectApi(WXL_GX_API_D3D9EX, GxApi::d3d9).api == WXL_GX_API_D3D9EX,
              "[gx] api = d3d9 takes a Vulkan request to D3D9 and leaves D3D9Ex");

        Level level;
        Check(wxl::log::ParseLevel(" WARN ", level) && level == Level::Warn && wxl::log::ParseLevel("1", level) &&
                  level == Level::Debug && !wxl::log::ParseLevel("loud", level),
              "level names, in any case, or 0 to 5");
    }

    std::printf(g_failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
