// SPDX-License-Identifier: GPL-3.0-or-later
// wxl-patcher [Wow.exe]: adds the WarcraftXL.dll import and the large-address flag, with a .orig backup.

#include "patcher/PeImage.hpp"

#include "common/Log.hpp"

#include <windows.h>
#include <io.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    constexpr char kDllName[]    = "WarcraftXL.dll";
    constexpr char kFuncName[]   = "WarcraftXL";
    constexpr char kTagSection[] = ".wxl";

    /// The whole file; empty on failure.
    std::vector<uint8_t> ReadAll(const char* path)
    {
        std::vector<uint8_t> data;
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f) return data;
        long size = -1;
        if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
        if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return data; }
        data.resize(static_cast<size_t>(size));
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        fclose(f);
        return data;
    }

    /// Written to a temporary file, flushed, then moved over: the target is replaced whole or not at all.
    bool WriteAll(const char* path, const std::vector<uint8_t>& data)
    {
        const std::string tmp = std::string(path) + ".wxltmp";
        FILE* f = nullptr;
        if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) return false;
        const bool written = fwrite(data.data(), 1, data.size(), f) == data.size()
                          && fflush(f) == 0
                          && _commit(_fileno(f)) == 0;
        const bool closed = fclose(f) == 0;
        if (!written || !closed) { remove(tmp.c_str()); return false; }
        if (!MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        { remove(tmp.c_str()); return false; }
        return true;
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    wxl::log::EnableConsole("[WarcraftXL] ");
    const char* target = argc > 1 ? argv[1] : "Wow.exe";
    WLOG_INFO("patcher start, target='%s'", target);

    std::vector<uint8_t> file = ReadAll(target);
    if (file.empty()) { WLOG_ERROR("cannot read '%s'", target); return 1; }

    wxl::patcher::PeImage pe(file);
    if (!pe.Valid()) { WLOG_ERROR("'%s' is not a 32-bit PE", target); return 1; }

    // The flag is set even on a file an older patcher already gave the import.
    pe.SetLargeAddressAware();
    if (pe.HasSection(kTagSection))
    {
        if (!WriteAll(target, file)) { WLOG_ERROR("cannot write '%s'", target); return 1; }
        WLOG_INFO("already patched ('%s' present), ensured large-address-aware", kTagSection);
        return 0;
    }

    if (!pe.AddImport(kDllName, kFuncName, kTagSection))
    { WLOG_ERROR("import injection failed"); return 1; }

    // The backup is the only way back: nothing is written before it is on disk.
    std::string backup = std::string(target) + ".orig";
    if (GetFileAttributesA(backup.c_str()) == INVALID_FILE_ATTRIBUTES
        && !CopyFileA(target, backup.c_str(), TRUE))
    {
        WLOG_ERROR("cannot back up '%s' to '%s' (win32=%lu), aborting before write",
                   target, backup.c_str(), GetLastError());
        return 1;
    }

    if (!WriteAll(target, file)) { WLOG_ERROR("cannot write '%s'", target); return 1; }

    WLOG_INFO("patched '%s' (+import %s!%s, large-address aware, backup '%s')",
              target, kDllName, kFuncName, backup.c_str());
    return 0;
}
