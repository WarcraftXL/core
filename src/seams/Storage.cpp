// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Storage seam: what the runtime does at the client's one file-open function.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"
#include "runtime/Stages.hpp"
#include "services/Assets.hpp"
#include "services/DataService.hpp"
#include "services/StorageService.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstdint>
#include <cctype>
#include <cstring>
#include <string>

#include "wxl/scoped_name.hpp"

namespace
{
    /**
     * @brief Where a redirected path lives between the gate and the open.
     *
     * The gate returns before the client opens anything, so the path a handler writes cannot
     * sit on the gate's stack: the client would read freed memory. It cannot be one buffer for
     * the process either, because files are opened from more than one thread. One per thread,
     * overwritten by that thread's next open, which is as long as the client needs it.
     */
    thread_local char g_redirect[512];

    /// The name a folder of the search path answers a lookup with, held as long as g_redirect.
    thread_local char g_resolved[512];

    /// The name an override puts in place of the one asked, held as long as g_redirect.
    thread_local char g_override[512];

    /// The base name a scoped name nobody serves falls back to, held as long as g_redirect.
    thread_local char g_unscoped[512];

    /// What an existence check looks up in place of the name asked, held as long as the check.
    thread_local char g_findName[512];

    /// The name the client asked for, when the gates had it looked up as another: what
    /// FileMissing reports. Valid while `t_askedFor` is the name FindFile was handed.
    thread_local char        t_asked[512];
    thread_local const char* t_askedFor = nullptr;

    void NoteAsked(const char* asked, const char* lookedUp)
    {
        if (asked == lookedUp || std::strlen(asked) >= sizeof t_asked)
        {
            t_askedFor = nullptr;
            return;
        }
        std::memcpy(t_asked, asked, std::strlen(asked) + 1);
        t_askedFor = lookedUp;
    }

    namespace assets  = wxl::core::services::assets;
    namespace storage = wxl::core::services::storage;

    /// Whether the client itself has a name, with no source of the runtime answering.
    bool ClientHas(const char* name)
    {
        assets::ModeScope only(assets::Mode::ClientOnly);
        return wxl::core::binding::file_exists(name) != WXL_FALSE;
    }

    /// A name the client resolves as its own data: not a path, not climbing out.
    bool IsClientName(const char* name)
    {
        if (!name || !*name) return false;
        if (name[0] == '\\' || name[0] == '/' || name[1] == ':') return false;
        return std::strstr(name, "..") == nullptr && std::strchr(name, '|') == nullptr;
    }
}

namespace wxl::seam::Storage::OpenFile
{
    bool WXL_CALL Gate(int32_t* result, void** archive, const char** name, uint32_t* flags,
                       void*** file)
    {
        (void)result;
        (void)file;
        (void)flags;

        // The client is opening a file, so its archives are mounted: whatever else this
        // seam is for, the first call through it is what storage being up looks like.
        wxl::core::stages::Reach(WXL_STAGE_STORAGE);

        if (!name || !*name) return false;

        // The runtime reading a file to transform it, or asking the client what it has: its
        // own opens, which nothing is told of and nothing redirects again.
        if (wxl::core::services::assets::Current() != wxl::core::services::assets::Mode::Normal) return false;

        // The runtime listing the archives for wxl.storage's find: its own open, of an archive's
        // listfile, which nothing else is told of.
        if (wxl::core::services::storage::Listing()) return false;

        // A client database a mod serves is answered here, before anything is raised: the
        // client reads each of its databases once, so this is the only moment that counts.
        // Mods see the open as the path that was asked for, and may still redirect it again.
        const std::size_t length = std::strlen(*name);
        if (wxl::core::services::data::ServedPath(*name, length, g_redirect, sizeof g_redirect))
        {
            *name = g_redirect;
            return false;
        }

        // A mod's override of the name: what is opened in its place, then redirected as any name.
        const char* const asked   = *name;
        const char*       current = asked;
        if (wxl::core::services::storage::Overridden(asked, g_override, sizeof g_override)) current = g_override;

        // A mod's redirect, from the table: what the client opens unless a handler answers.
        g_redirect[0] = 0;
        wxl::core::services::storage::Redirected(current, g_redirect, sizeof g_redirect);
        if (!g_redirect[0] && current != asked) std::memcpy(g_redirect, current, std::strlen(current) + 1);

        wxl::host::ModHost* host = wxl::core::Host();
        if (host && host->Listening(WXL_EV_FileOpen))
        {
            // Handlers see the name asked for, and in `redirect` what the table answers so far.
            wxl_file_open_args args{};
            args.path              = wxl_str{asked, static_cast<uint32_t>(length)};
            args.redirect          = g_redirect;
            args.redirect_capacity = static_cast<uint32_t>(sizeof g_redirect);
            host->Raise(WXL_EV_FileOpen, &args);
        }

        // A handler that writes past the end of the buffer would leave it unterminated; the
        // last byte is forced rather than trusting what came back.
        g_redirect[sizeof g_redirect - 1] = 0;
        const char* opened = g_redirect[0] ? g_redirect : asked;
        if (g_redirect[0]) WLOG_DEBUG(wxl::core::kTag, "file %s redirected to %s", asked, g_redirect);

        // A name in a mod's scope that nothing serves: the client's own file of its base name.
        if (assets::Unscoped(opened, g_unscoped, sizeof g_unscoped)) opened = g_unscoped;

        // The one thing only a gate can do: what the client opens is not what it asked for.
        NoteAsked(asked, opened);
        *name = opened;
        return false;
    }
}

namespace wxl::seam::Storage::FindFile
{
    bool WXL_CALL Gate(int32_t* result, const char** name, char** path, uint32_t* size, uint32_t* flags,
                       uint32_t** type, void*** archive)
    {
        (void)flags;
        if (!name || !IsClientName(*name)) return false;
        const assets::Mode mode = assets::Current();
        if (mode == assets::Mode::ClientOnly) return false;
        assets::ClearPending();

        // An existence check -- no archive asked back, which only SFile::OpenEx asks -- has not
        // been through the open's gate: an override and a scope are looked at here.
        const bool exists = !archive || !*archive;
        if (exists && mode == assets::Mode::Normal && !storage::Listing())
        {
            const char* asked   = *name;
            const char* current = asked;
            if (storage::Overridden(current, g_findName, sizeof g_findName)) current = g_findName;
            if (assets::Unscoped(current, g_unscoped, sizeof g_unscoped)) current = g_unscoped;
            NoteAsked(asked, current);
            *name = current;
        }
        if (!storage::AnyFolder() && !assets::Active()) return false;

        // The sources by priority: the best folder, then a provider or served bytes above it.
        int32_t    folderPriority = 0;
        const bool folder =
            storage::BestFolder(*name, g_resolved, sizeof g_resolved, &folderPriority);
        assets::Candidate source{};
        const bool        sourced = assets::BestSource(*name, folder ? folderPriority : INT32_MIN, &source);
        const int32_t     best    = sourced ? source.priority : folderPriority;
        const bool        any     = folder || sourced;

        // A source of 0 or below only fills a name the client does not have.
        const bool wins = any && (best > 0 || !ClientHas(*name));

        // A kind some mod transforms is read through the file layer, whichever source has it.
        if (mode == assets::Mode::Normal && assets::Transformed(*name) && (wins || ClientHas(*name)))
        {
            if (path && *path && size && assets::LayerPath(0, *name, *path, *size))
            {
                if (type && *type) **type = 0;
                if (archive && *archive) **archive = nullptr;
                *result = 1;
                return true;
            }
        }
        if (!wins) return false;

        if (sourced)
        {
            if (!path || !*path || !size || !assets::LayerPath(source.id, *name, *path, *size)) return false;
            WLOG_DEBUG(wxl::core::kTag, "file %s served by a mod's source", *name);
            assets::Resolution found;
            found.layer  = assets::SourceLayer(source.id, nullptr);
            found.source = source.id;
            assets::NoteResolution(found);
            if (type && *type) **type = 0; // SFILE_PLAIN: opened with Blizzard::File::Open(path)
            if (archive && *archive) **archive = nullptr;
            *result = 1;
            return true;
        }

        WLOG_DEBUG(wxl::core::kTag, "file %s found in a search path folder: %s", *name, g_resolved);
        *name = g_resolved;
        return false;
    }
}

namespace
{
    /// What a file is for, by its extension.
    uint32_t KindOf(const std::string& name)
    {
        const size_t slash = name.find_last_of("\\/");
        const size_t dot   = name.rfind('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return WXL_ASSET_OTHER;
        std::string extension;
        for (size_t i = dot + 1; i < name.size(); ++i)
            extension += static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
        static const struct
        {
            const char* extension;
            uint32_t    kind;
        } kKinds[] = {
            {"m2", WXL_ASSET_MODEL},      {"mdx", WXL_ASSET_MODEL},      {"mdl", WXL_ASSET_MODEL},
            {"skin", WXL_ASSET_SKIN},     {"anim", WXL_ASSET_ANIMATION}, {"blp", WXL_ASSET_TEXTURE},
            {"tga", WXL_ASSET_TEXTURE},   {"adt", WXL_ASSET_TERRAIN},    {"wdt", WXL_ASSET_TERRAIN},
            {"wdl", WXL_ASSET_TERRAIN},   {"wmo", WXL_ASSET_MAP_OBJECT}, {"wav", WXL_ASSET_SOUND},
            {"mp3", WXL_ASSET_SOUND},     {"ogg", WXL_ASSET_SOUND},      {"dbc", WXL_ASSET_DATABASE},
            {"db2", WXL_ASSET_DATABASE},  {"lua", WXL_ASSET_INTERFACE},  {"xml", WXL_ASSET_INTERFACE},
            {"toc", WXL_ASSET_INTERFACE}, {"bls", WXL_ASSET_SHADER},     {"ttf", WXL_ASSET_FONT},
            {"avi", WXL_ASSET_MOVIE},
        };
        for (const auto& known : kKinds)
            if (extension == known.extension) return known.kind;
        return WXL_ASSET_OTHER;
    }
}

namespace wxl::seam::Storage::FindFileResult
{
    void WXL_CALL After(int32_t result, const char* name, char* path, uint32_t size, uint32_t flags, uint32_t* type,
                        void** archive)
    {
        (void)size;
        (void)flags;
        const assets::Mode mode = assets::Current();
        if (mode == assets::Mode::ClientOnly || storage::Listing() || !name) return;

        // The answer is in al alone: the rest of eax is whatever the function left there.
        if (static_cast<uint8_t>(result) != 0)
        {
            // Where it was found, for hash_file: a folder of the search path is a name the gate
            // rewrote; an archive's file is SFILE_OLD_SFILE (3); a plain one is on the disk.
            assets::Resolution found;
            if (name == g_resolved) found.layer = WXL_FILE_LAYER_FOLDER;
            else if (type && *type == 3) found.layer = WXL_FILE_LAYER_ARCHIVE;
            else found.layer = WXL_FILE_LAYER_DISK;
            found.archive = found.layer == WXL_FILE_LAYER_ARCHIVE && archive ? *archive : nullptr;
            if (found.layer != WXL_FILE_LAYER_ARCHIVE && path) found.path = path;
            if (found.layer == WXL_FILE_LAYER_FOLDER) found.path = name;
            assets::NoteResolution(found);
            return;
        }

        // Found nowhere. The client asked, not the runtime and not a mod through wxl.storage.
        wxl::host::ModHost* host = wxl::core::Host();
        if (mode != assets::Mode::Normal || assets::ModAsking() || !host || !host->Listening(WXL_EV_FileMissing)) return;

        const std::string asked = t_askedFor == name ? std::string(t_asked) : std::string(name);
        wxl::ScopedParts  parts;
        const bool        scoped = wxl::SplitScopedName(asked, &parts);
        const std::string base   = scoped ? parts.base : asked;

        wxl_file_missing_args args{};
        args.path    = wxl_str{asked.c_str(), static_cast<uint32_t>(asked.size())};
        args.base    = wxl_str{base.c_str(), static_cast<uint32_t>(base.size())};
        args.scope   = wxl_str{parts.mod.c_str(), static_cast<uint32_t>(scoped ? parts.mod.size() : 0)};
        args.kind    = KindOf(base);
        args.open    = archive ? WXL_TRUE : WXL_FALSE;
        args.pending = assets::PendingSeen() ? WXL_TRUE : WXL_FALSE;
        args.loader  = host->OnMainThread() ? WXL_FALSE : WXL_TRUE;
        host->Raise(WXL_EV_FileMissing, &args);
    }
}

namespace wxl::seam::Storage::OpenEx
{
    void WXL_CALL After(int32_t result, void*, const char* name, uint32_t, void** file)
    {
        // Any thread, every open: nothing is read until someone listens.
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !host->Listening(WXL_EV_FileOpened) || !name) return;
        if (wxl::core::services::storage::Listing()) return;
        if (wxl::core::services::assets::Current() != wxl::core::services::assets::Mode::Normal) return;

        const bool found = result != 0 && file && *file;
        const uint64_t size = found ? wxl::core::binding::file_size(*file) : 0;

        wxl_file_opened_args args{};
        args.path  = wxl_str{name, static_cast<uint32_t>(std::strlen(name))};
        args.found = found ? WXL_TRUE : WXL_FALSE;
        args.size  = size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(size);
        host->Raise(WXL_EV_FileOpened, &args);
    }
}

namespace
{
    /// An archive's event, as the client describes the archive; false when it describes none.
    bool ArchiveArgs(void* archive, wxl_archive_args* args)
    {
        wxl_core_archive described{};
        if (!archive || !wxl::core::binding::archive_of(archive, &described)) return false;
        const char* path    = described.path ? described.path : "";
        args->path          = wxl_str{path, static_cast<uint32_t>(std::strlen(path))};
        args->priority      = described.priority;
        args->hash_entries  = described.hash_entries;
        args->block_entries = described.block_entries;
        args->bytes         = described.bytes;
        return true;
    }
}

namespace wxl::seam::Storage::OpenArchive
{
    void WXL_CALL After(int32_t result, const char*, int32_t, uint32_t, void** archive)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        wxl_archive_args    args{};
        if (!host || !result || !archive || !ArchiveArgs(*archive, &args)) return;
        host->Raise(WXL_EV_ArchiveMounted, &args);
    }
}

namespace wxl::seam::Storage::CloseArchive
{
    void WXL_CALL Before(void* archive)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        wxl_archive_args    args{};
        if (!host || !ArchiveArgs(archive, &args)) return;
        host->Raise(WXL_EV_ArchiveUnmounted, &args);
    }
}
