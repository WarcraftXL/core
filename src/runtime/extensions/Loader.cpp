// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/extensions/Loader.hpp"

#include "runtime/extensions/Manifest.hpp"

#include "common/Log.hpp"

#include <cstdarg>
#include <cstdio>

namespace wxl::runtime::loader
{
    namespace
    {
        constexpr uint32_t kSdkMajorShift = 16; ///< WXL_PluginInfo::sdkVersion is major << 16 | minor
        constexpr uint32_t kSdkMinorMask  = 0xFFFFu;
        constexpr size_t   kMaxReason     = 512;

        bool Refuse(const std::string& folder, std::string* reason, const char* fmt, ...)
        {
            char text[kMaxReason];
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(text, sizeof text, fmt, args);
            va_end(args);
            WLOG_ERROR("extensions: '%s' refused: %s", folder.c_str(), text);
            if (reason) *reason = text;
            return false;
        }

        template <class Fn>
        Fn Export(HMODULE module, const char* name)
        {
            return reinterpret_cast<Fn>(GetProcAddress(module, name));
        }
    }

    bool Open(const std::string& folder, const std::string& dir, Opened& out, std::string* reason)
    {
        out = Opened{};
        const manifest::Version core = manifest::Core();

        const manifest::Manifest m = manifest::Read(dir + "\\wxl.json");
        if (m.status == manifest::Status::Absent)
            return Refuse(folder, reason, "no wxl.json beside the DLL (wxl_add_extension writes it; ship it in "
                          "the extension's folder)");
        if (m.status == manifest::Status::Invalid)
            return Refuse(folder, reason, "wxl.json %s", m.error.c_str());
        if (!manifest::Compatible(m.abi, core))
            return Refuse(folder, reason, "wxl.json says SDK %u.%u; this core loads SDK %u.0 to %u.%u "
                          "(update WarcraftXL or the extension)",
                          m.abi.major, m.abi.minor, core.major, core.major, core.minor);
        if (!m.id.empty() && m.id != folder)
            WLOG_WARN("extensions: '%s' is named '%s' in its wxl.json", folder.c_str(), m.id.c_str());

        const std::string path = dir + "\\" + folder + ".dll";
        HMODULE module = LoadLibraryA(path.c_str());
        if (!module)
            return Refuse(folder, reason, "%s could not be loaded (win32 %lu)", path.c_str(), GetLastError());

        // Formatted before FreeLibrary: the arguments may point into the DLL (info->name).
        auto unload = [&](const char* fmt, auto... args) {
            Refuse(folder, reason, fmt, args...);
            FreeLibrary(module);
            return false;
        };

        const auto query = Export<WXL_QueryFn>(module, "WXL_Query");
        const auto load  = Export<WXL_LoadFn>(module, "WXL_Load");
        if (!query || !load) return unload("exports no WXL_Query/WXL_Load");

        // The version first: an extension of another API is told that, not that its block is short.
        const WXL_PluginInfo* info = query();
        if (!info) return unload("returned no info block");
        if (info->apiVersion != WXL_API_VERSION)
            return unload("was built against API v%u, this core serves v%u (rebuild it against SDK %u.%u)",
                          info->apiVersion, uint32_t(WXL_API_VERSION), core.major, core.minor);
        if (info->structSize < sizeof(WXL_PluginInfo)) return unload("returned an info block too short to be SDK 1.x's");

        const char* name = info->name ? info->name : folder.c_str();
        if (info->clientBuild != WXL_CLIENT_BUILD)
            return unload("'%s' targets client build %u, this one is %u", name, info->clientBuild,
                          uint32_t(WXL_CLIENT_BUILD));

        const manifest::Version built{ info->sdkVersion >> kSdkMajorShift, info->sdkVersion & kSdkMinorMask };
        if (!manifest::Compatible(built, core))
            return unload("'%s' was built against SDK %u.%u; this core loads SDK %u.0 to %u.%u "
                          "(update WarcraftXL or the extension)",
                          name, built.major, built.minor, core.major, core.major, core.minor);
        if (built.major != m.abi.major || built.minor != m.abi.minor)
            WLOG_WARN("extensions: '%s' says SDK %u.%u in wxl.json but was built against %u.%u",
                      folder.c_str(), m.abi.major, m.abi.minor, built.major, built.minor);

        out.module = module;
        out.load   = load;
        out.info   = info;
        out.name   = name;
        out.version  = m.version;
        out.lowLevel = m.lowLevel;
        return true;
    }
}
