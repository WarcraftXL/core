// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.storage service: the client's files, over the binding.

#include "services/StorageService.hpp"

#include "services/Assets.hpp"
#include "services/Hashes.hpp"
#include "services/Reload.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/mod_host.hpp"
#include "wxl/runtime/handles.hpp"
#include "wxl/scoped_name.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr char kTag[] = "storage";

    /// What the client handed back for an open file, and who asked for it.
    struct Open
    {
        void*    file = nullptr;
        uint32_t mod  = 0; ///< the host's token for the mod that opened it
    };

    /// Handles are generational, so one kept past its close answers "gone" rather than reading
    /// whatever the client put at that address next.
    wxl::runtime::HandleTable g_handles;
    std::vector<Open>         g_open;

    /// A boundary string as a NUL-terminated path the client's own calls can take.
    std::string Path(wxl_str text)
    {
        return text.data ? std::string(text.data, text.size) : std::string();
    }

    uint32_t Asking()
    {
        const wxl::host::ModHost* host = wxl::core::Host();
        return host ? host->CurrentMod() : 0u;
    }

    void Forget(void* file)
    {
        g_handles.Untrack(file);
        for (std::size_t i = 0; i < g_open.size(); ++i)
        {
            if (g_open[i].file != file) continue;
            g_open.erase(g_open.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }

    // --- the search path ----------------------------------------------------------------------
    //
    // The client resolves a name in its archives. A folder joins the search here, in the
    // runtime: at each open, a folder that holds the name and wins it turns the name into one the
    // client opens straight from the disk. A name that starts with two backslashes is one the
    // client takes as a path (FindFile hands it to OldFindFile, which reads the disk at its base
    // path followed by the name), so the file's path relative to the client's folder, behind two
    // backslashes, is opened where it lies; the doubled separator after the base path is one to
    // Windows. Without that, the client would look the name up in its table of the loose files
    // under its folder, which it builds by walking the whole folder at the first lookup and again
    // after each RebuildHash. A folder must be inside the client's folder; a mod's, in Mods\, is.

    struct SearchFolder
    {
        std::string directory; ///< absolute, backslashes, no trailing separator
        int32_t     priority;
        uint32_t    mod;       ///< the host's token for the mod that added it
        bool        outside_warned = false;
    };

    std::mutex              g_pathsLock;
    std::vector<SearchFolder> g_paths; ///< highest priority first; equal ones in the order added
    std::atomic<uint32_t>   g_pathCount{0};
    std::atomic<uint32_t>   g_generation{0}; ///< changes when what find answers may have
    thread_local bool       t_listing = false;

    std::string Backslashed(std::string path)
    {
        std::replace(path.begin(), path.end(), '/', '\\');
        return path;
    }

    bool IsFile(const std::string& path)
    {
        const DWORD attributes = ::GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool IsDirectory(const std::string& path)
    {
        const DWORD attributes = ::GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    /// A name the client resolves itself: absolute, a share, or climbing out.
    bool IsPlainName(const char* name)
    {
        if (!name || !*name) return false;
        if (name[0] == '\\' || name[0] == '/' || name[1] == ':') return false;
        return std::strstr(name, "..") == nullptr;
    }

    /// The client's folder, with its separator: its base path, or the current directory while it
    /// has none, which is what its own scan falls back to.
    std::string ClientFolder()
    {
        const char* base = wxl::core::binding::storage_base_path();
        std::string folder;
        if (base && *base)
        {
            folder = Backslashed(base);
        }
        else
        {
            char        current[MAX_PATH];
            const DWORD length = ::GetCurrentDirectoryA(MAX_PATH, current);
            if (length && length < MAX_PATH) folder.assign(current, length);
        }
        if (!folder.empty() && folder.back() != '\\') folder += '\\';
        return folder;
    }

    /// Whether a folder is inside the client's folder, where the client can open its files.
    bool Inside(const std::string& directory, const std::string& folder)
    {
        return !folder.empty() && directory.size() + 1 >= folder.size() &&
               _strnicmp((directory + "\\").c_str(), folder.c_str(), folder.size()) == 0;
    }

    // --- the table ----------------------------------------------------------------------------

    /// What an open that found nothing answers: not yet, when a provider said so, else not found.
    wxl_result Missing()
    {
        return wxl::core::services::assets::PendingSeen() ? WXL_E_PENDING : WXL_E_NOT_FOUND;
    }

    wxl_bool WXL_CALL Exists(wxl_str path)
    {
        const std::string name = Path(path);
        if (name.empty()) return WXL_FALSE;
        const wxl::core::services::assets::AskingScope asking;
        return wxl::core::services::storage::Exists(name.c_str()) ? WXL_TRUE : WXL_FALSE;
    }

    /**
     * @brief The length of a file, which costs an open and a close.
     *
     * The client has no way to ask a name how long it is: only an open file answers that. A mod
     * that then reads the file pays for a second open, which is why the facade's whole-file read
     * exists and why this is not the way to read one.
     */
    wxl_result WXL_CALL Size(wxl_str path, uint64_t* out)
    {
        if (!out) return WXL_E_INVALID;
        const std::string name = Path(path);
        if (name.empty()) return WXL_E_INVALID;

        const wxl::core::services::assets::AskingScope asking;
        wxl::core::services::assets::ClearPending();
        void* file = wxl::core::binding::file_open(name.c_str());
        if (!file) return Missing();
        *out = wxl::core::binding::file_size(file);
        wxl::core::binding::file_close(file);
        return WXL_OK;
    }

    wxl_result WXL_CALL Read(wxl_str path, void* buffer, uint64_t capacity, uint64_t* out_read)
    {
        if (!out_read) return WXL_E_INVALID;
        *out_read = 0;

        const std::string name = Path(path);
        if (name.empty()) return WXL_E_INVALID;

        const wxl::core::services::assets::AskingScope asking;
        wxl::core::services::assets::ClearPending();
        void* file = wxl::core::binding::file_open(name.c_str());
        if (!file) return Missing();

        const uint64_t size = wxl::core::binding::file_size(file);
        if (!buffer || capacity < size)
        {
            // What it would have taken, so the caller can size a buffer and come back.
            *out_read = size;
            wxl::core::binding::file_close(file);
            return WXL_E_INVALID;
        }

        uint64_t read = 0;
        while (read < size)
        {
            const uint64_t left = size - read;
            const uint32_t want = left > 0x10000000u ? 0x10000000u : static_cast<uint32_t>(left);
            const uint32_t got  = wxl::core::binding::file_read(
                file, static_cast<char*>(buffer) + read, want);
            if (!got) break;
            read += got;
        }
        wxl::core::binding::file_close(file);

        *out_read = read;
        return read == size ? WXL_OK : WXL_E_FAILED;
    }

    wxl_handle WXL_CALL OpenFile(wxl_str path)
    {
        const std::string name = Path(path);
        if (name.empty()) return WXL_HANDLE_NONE;

        const wxl::core::services::assets::AskingScope asking;
        void* file = wxl::core::binding::file_open(name.c_str());
        if (!file) return WXL_HANDLE_NONE;

        g_open.push_back(Open{file, Asking()});
        return g_handles.Track(file);
    }

    wxl_result WXL_CALL ReadAt(wxl_handle handle, uint64_t offset, void* buffer, uint64_t size,
                               uint64_t* out_read)
    {
        if (!out_read) return WXL_E_INVALID;
        *out_read = 0;

        // The handle table answers with the client's file pointer, which is everything a read
        // needs; the mod that opened it only matters when its files are closed for it.
        void* file = g_handles.Resolve(handle);
        if (!file) return WXL_E_NOT_FOUND;
        if (!buffer && size) return WXL_E_INVALID;
        if (!size) return WXL_OK;

        if (!wxl::core::binding::file_seek(file, offset)) return WXL_E_FAILED;

        uint64_t read = 0;
        while (read < size)
        {
            const uint64_t left = size - read;
            const uint32_t want = left > 0x10000000u ? 0x10000000u : static_cast<uint32_t>(left);
            const uint32_t got  = wxl::core::binding::file_read(
                file, static_cast<char*>(buffer) + read, want);
            if (!got) break;
            read += got;
        }

        *out_read = read;
        return WXL_OK;
    }

    void WXL_CALL Close(wxl_handle handle)
    {
        void* file = g_handles.Resolve(handle);
        if (!file) return;
        wxl::core::binding::file_close(file);
        Forget(file);
    }

    wxl_result WXL_CALL MountArchive(wxl_str path, int32_t priority)
    {
        const std::string name = Path(path);
        if (name.empty()) return WXL_E_INVALID;
        if (!wxl::core::binding::archive_mount(name.c_str(), priority)) return WXL_E_FAILED;

        g_generation.fetch_add(1, std::memory_order_relaxed);
        WLOG_INFO(kTag, "archive %s mounted at priority %d", name.c_str(), priority);
        return WXL_OK;
    }

    /// A folder as the search path keeps it: absolute against the current directory, which is
    /// the client's folder once the client has set its paths; backslashes, no trailing one.
    bool Absolute(wxl_str directory, std::string& out)
    {
        std::string asked = Backslashed(Path(directory));
        while (asked.size() > 1 && asked.back() == '\\') asked.pop_back();
        if (asked.empty()) return false;

        char        full[MAX_PATH * 2];
        const DWORD length = ::GetFullPathNameA(asked.c_str(), sizeof full, full, nullptr);
        if (!length || length >= sizeof full) return false;
        out.assign(full, length);
        while (out.size() > 3 && out.back() == '\\') out.pop_back();
        return true;
    }

    /**
     * @brief A folder joins the search, for the mod that asks.
     *
     * Adding a folder again changes its priority; another mod's entry for the same folder is
     * its own. Each open looks at the folder again, so a file put in it since is found too.
     */
    wxl_result WXL_CALL AddSearchPath(wxl_str directory, int32_t priority)
    {
        std::string absolute;
        if (!Absolute(directory, absolute)) return WXL_E_INVALID;
        if (!IsDirectory(absolute))
        {
            WLOG_WARN(kTag, "search path %s: no such folder", absolute.c_str());
            return WXL_E_NOT_FOUND;
        }

        const uint32_t mod = Asking();
        {
            std::lock_guard<std::mutex> lock(g_pathsLock);
            const auto same = std::find_if(g_paths.begin(), g_paths.end(), [&](const SearchFolder& known) {
                return known.mod == mod && _stricmp(known.directory.c_str(), absolute.c_str()) == 0;
            });
            if (same != g_paths.end()) g_paths.erase(same);
            const auto at = std::find_if(g_paths.begin(), g_paths.end(),
                                         [&](const SearchFolder& known) { return known.priority < priority; });
            g_paths.insert(at, SearchFolder{absolute, priority, mod});
            g_pathCount.store(static_cast<uint32_t>(g_paths.size()), std::memory_order_release);
        }
        g_generation.fetch_add(1, std::memory_order_relaxed);
        WLOG_INFO(kTag, "folder %s searched at priority %d", absolute.c_str(), priority);
        return WXL_OK;
    }

    /// Drops the entries a predicate picks and says so; the count of what went.
    template <class Pick>
    size_t DropFolders(Pick pick)
    {
        std::vector<std::string> gone;
        {
            std::lock_guard<std::mutex> lock(g_pathsLock);
            for (size_t i = g_paths.size(); i-- > 0;)
            {
                if (!pick(g_paths[i])) continue;
                gone.push_back(g_paths[i].directory);
                g_paths.erase(g_paths.begin() + static_cast<std::ptrdiff_t>(i));
            }
            g_pathCount.store(static_cast<uint32_t>(g_paths.size()), std::memory_order_release);
        }
        if (gone.empty()) return 0;
        // Nothing to tell the client: it reaches a folder's files only through the names an open
        // is rewritten to, and no open is rewritten into a folder that left.
        g_generation.fetch_add(1, std::memory_order_relaxed);
        for (const std::string& directory : gone) WLOG_INFO(kTag, "folder %s no longer searched", directory.c_str());
        return gone.size();
    }

    /// Takes the asking mod's entry for a folder back out of the search.
    wxl_result WXL_CALL RemoveSearchPath(wxl_str directory)
    {
        std::string absolute;
        if (!Absolute(directory, absolute)) return WXL_E_INVALID;
        const uint32_t mod     = Asking();
        bool           removed = false;
        DropFolders([&](const SearchFolder& known) {
            if (removed || known.mod != mod || _stricmp(known.directory.c_str(), absolute.c_str()) != 0) return false;
            removed = true;
            return true;
        });
        return removed ? WXL_OK : WXL_E_NOT_FOUND;
    }

    // --- find ---------------------------------------------------------------------------------

    char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

    /// Whether a name matches a pattern, `*` any run of characters, case and separators aside.
    bool Matches(const char* name, const char* pattern)
    {
        const char* star  = nullptr;
        const char* after = nullptr;
        while (*name)
        {
            const char p = *pattern == '/' ? '\\' : *pattern;
            const char n = *name == '/' ? '\\' : *name;
            if (p == '*')
            {
                star  = pattern++;
                after = name;
            }
            else if (p && Lower(p) == Lower(n))
            {
                ++pattern;
                ++name;
            }
            else if (star)
            {
                pattern = star + 1;
                name    = ++after;
            }
            else
            {
                return false;
            }
        }
        while (*pattern == '*') ++pattern;
        return *pattern == 0;
    }

    struct Found
    {
        const char*              pattern;
        std::vector<std::string> names;
        std::vector<std::string> keys; ///< lowercased, to report each name once
    };

    void Keep(Found& found, const std::string& name)
    {
        if (!Matches(name.c_str(), found.pattern)) return;
        std::string key = Backslashed(name);
        std::transform(key.begin(), key.end(), key.begin(), Lower);
        found.keys.push_back(std::move(key));
        found.names.push_back(Backslashed(name));
    }

    int32_t WXL_CALL KeepListed(const char* name, void* user)
    {
        if (name && *name) Keep(*static_cast<Found*>(user), name);
        return 1;
    }

    void KeepFolder(Found& found, const std::string& root, const std::string& prefix)
    {
        WIN32_FIND_DATAA entry;
        const HANDLE     search = ::FindFirstFileA((root + "\\" + prefix + "*").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) return;
        do
        {
            const std::string leaf = entry.cFileName;
            if (leaf == "." || leaf == "..") continue;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                KeepFolder(found, root, prefix + leaf + "\\");
            else
                Keep(found, prefix + leaf);
        } while (::FindNextFileA(search, &entry));
        ::FindClose(search);
    }

    /**
     * @brief The names matching a pattern, across the archives' listfiles and the folders.
     *
     * Listing every archive reads every listfile, so the answer is kept for the thread's next
     * call with the same pattern -- the count, then the fill -- until an archive or a folder
     * joins.
     */
    uint32_t WXL_CALL Find(wxl_str pattern, wxl_str* out, uint32_t capacity)
    {
        thread_local std::string              lastPattern;
        thread_local uint32_t                 lastGeneration = ~0u;
        thread_local std::vector<std::string> answer;

        const std::string wanted     = Backslashed(Path(pattern));
        const uint32_t    generation = g_generation.load(std::memory_order_relaxed);
        if (wanted.empty()) return 0;
        if (wanted != lastPattern || generation != lastGeneration || wxl::core::services::assets::Active())
        {
            Found found{wanted.c_str(), {}, {}};
            t_listing = true;
            wxl::core::binding::archive_list(&KeepListed, &found);
            t_listing = false;

            std::vector<std::string> folders;
            {
                const std::string           client = ClientFolder();
                std::lock_guard<std::mutex> lock(g_pathsLock);
                for (const SearchFolder& path : g_paths)
                    if (Inside(path.directory, client)) folders.push_back(path.directory);
            }
            for (const std::string& folder : folders) KeepFolder(found, folder, std::string());
            wxl::core::services::assets::ListMatching(
                wanted.c_str(), [&](const std::string& name) { Keep(found, name); },
                [](const char* name, const char* p) { return Matches(name, p); });

            // Once each: the first source to name it, sorted by name.
            std::vector<size_t> order(found.names.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(),
                             [&](size_t a, size_t b) { return found.keys[a] < found.keys[b]; });
            answer.clear();
            for (size_t i = 0; i < order.size(); ++i)
                if (i == 0 || found.keys[order[i]] != found.keys[order[i - 1]])
                    answer.push_back(found.names[order[i]]);
            lastPattern    = wanted;
            lastGeneration = generation;
        }

        for (uint32_t i = 0; out && i < capacity && i < answer.size(); ++i)
            out[i] = wxl_str{answer[i].c_str(), static_cast<uint32_t>(answer[i].size())};
        return static_cast<uint32_t>(answer.size());
    }

    // --- redirects ----------------------------------------------------------------------------
    //
    // A name the client opens in place of another, held per mod and looked at by the open gate
    // on whichever thread opens. Each name keeps its redirects in the order they were made; the
    // latest answers, and taking it back uncovers the one before.

    constexpr size_t kNameMax = 511; ///< what the gate's buffer holds, its NUL aside

    struct Redirect
    {
        uint32_t    mod; ///< the host's token for the mod that made it
        std::string to;
    };

    SRWLOCK                                                    g_redirectLock = SRWLOCK_INIT;
    std::unordered_map<std::string, std::vector<Redirect>>     g_redirects; ///< by Key(from)
    std::atomic<uint32_t>                                      g_redirectCount{0};

    /// A name as the client compares it: any case, either slash.
    std::string Key(std::string_view name)
    {
        std::string key(name);
        for (char& c : key) c = c == '/' ? '\\' : Lower(c);
        return key;
    }

    /// A name a redirect may hold: not empty, short enough for the gate's buffer.
    bool Nameable(wxl_str name) { return name.data && name.size && name.size <= kNameMax; }

    wxl_result WXL_CALL RedirectFile(wxl_str from, wxl_str to)
    {
        if (!Nameable(from) || !Nameable(to)) return WXL_E_INVALID;
        const uint32_t    mod = Asking();
        const std::string key = Key(std::string_view(from.data, from.size));
        AcquireSRWLockExclusive(&g_redirectLock);
        std::vector<Redirect>& stack = g_redirects[key];
        stack.erase(std::remove_if(stack.begin(), stack.end(), [&](const Redirect& r) { return r.mod == mod; }),
                    stack.end());
        stack.push_back(Redirect{mod, Backslashed(Path(to))});
        g_redirectCount.store(static_cast<uint32_t>(g_redirects.size()), std::memory_order_release);
        ReleaseSRWLockExclusive(&g_redirectLock);
        WLOG_INFO(kTag, "%.*s opens %.*s instead", static_cast<int>(from.size), from.data, static_cast<int>(to.size),
                  to.data);
        return WXL_OK;
    }

    /// Takes one mod's redirects back, of one name or of every name.
    size_t DropRedirects(uint32_t mod, const std::string* key)
    {
        size_t dropped = 0;
        AcquireSRWLockExclusive(&g_redirectLock);
        for (auto it = g_redirects.begin(); it != g_redirects.end();)
        {
            if (key && it->first != *key)
            {
                ++it;
                continue;
            }
            std::vector<Redirect>& stack  = it->second;
            const size_t           before = stack.size();
            stack.erase(std::remove_if(stack.begin(), stack.end(), [&](const Redirect& r) { return r.mod == mod; }),
                        stack.end());
            dropped += before - stack.size();
            it = stack.empty() ? g_redirects.erase(it) : std::next(it);
        }
        g_redirectCount.store(static_cast<uint32_t>(g_redirects.size()), std::memory_order_release);
        ReleaseSRWLockExclusive(&g_redirectLock);
        return dropped;
    }

    wxl_result WXL_CALL Unredirect(wxl_str from)
    {
        if (!Nameable(from)) return WXL_E_INVALID;
        const std::string key = Key(std::string_view(from.data, from.size));
        if (!DropRedirects(Asking(), &key)) return WXL_E_NOT_FOUND;
        WLOG_INFO(kTag, "%.*s opens as itself again", static_cast<int>(from.size), from.data);
        return WXL_OK;
    }

    // --- providers, transforms and served bytes: the asset pipeline (services/Assets.cpp) -----

    wxl_result WXL_CALL AddProvider(const wxl_asset_provider* provider, int32_t priority, uint64_t* out)
    {
        return wxl::core::services::assets::AddProvider(provider, priority, out, Asking());
    }

    wxl_result WXL_CALL RemoveProvider(uint64_t provider)
    {
        return wxl::core::services::assets::RemoveProvider(provider, Asking());
    }

    wxl_result WXL_CALL AddTransform(const wxl_asset_transform* transform, int32_t priority, uint64_t* out)
    {
        return wxl::core::services::assets::AddTransform(transform, priority, out, Asking());
    }

    wxl_result WXL_CALL RemoveTransform(uint64_t transform)
    {
        return wxl::core::services::assets::RemoveTransform(transform, Asking());
    }

    wxl_result WXL_CALL ServeBytes(wxl_str path, const void* data, uint32_t size, int32_t priority)
    {
        return wxl::core::services::assets::Serve(path, data, size, priority, Asking());
    }

    wxl_result WXL_CALL Unserve(wxl_str path) { return wxl::core::services::assets::Unserve(path, Asking()); }

    // --- overrides ----------------------------------------------------------------------------
    //
    // A file of the client a mod replaces with another name, for opens and existence checks,
    // held per mod and looked at by both gates on whichever thread asks. Kept as the redirects
    // are: the latest answers, and taking it back uncovers the one before.

    constexpr size_t kOverrideMax = 259; ///< the client's paths are 260 bytes

    SRWLOCK                                                g_overrideLock = SRWLOCK_INIT;
    std::unordered_map<std::string, std::vector<Redirect>> g_overrides; ///< by Key(path)
    std::atomic<uint32_t>                                  g_overrideCount{0};

    const char* ModId(uint32_t mod)
    {
        const wxl::host::ModHost*   host   = wxl::core::Host();
        const wxl::host::LoadedMod* loaded = host ? host->ModOf(mod) : nullptr;
        return loaded ? loaded->id.c_str() : "a mod";
    }

    wxl_result WXL_CALL OverrideFile(wxl_str path, wxl_str with)
    {
        if (!path.data || !path.size || path.size > kOverrideMax || !with.data || !with.size || with.size > kOverrideMax)
            return WXL_E_INVALID;
        const uint32_t    mod = Asking();
        const std::string key = Key(std::string_view(path.data, path.size));
        AcquireSRWLockExclusive(&g_overrideLock);
        std::vector<Redirect>& stack = g_overrides[key];
        stack.erase(std::remove_if(stack.begin(), stack.end(), [&](const Redirect& r) { return r.mod == mod; }),
                    stack.end());
        stack.push_back(Redirect{mod, Backslashed(Path(with))});
        g_overrideCount.store(static_cast<uint32_t>(g_overrides.size()), std::memory_order_release);
        ReleaseSRWLockExclusive(&g_overrideLock);
        WLOG_INFO(kTag, "mod %s overrides %.*s with %.*s", ModId(mod), static_cast<int>(path.size), path.data,
                  static_cast<int>(with.size), with.data);
        return WXL_OK;
    }

    /// Takes one mod's overrides back, of one name or of every name; each is logged.
    size_t DropOverrides(uint32_t mod, const std::string* key)
    {
        std::vector<std::string> restored;
        AcquireSRWLockExclusive(&g_overrideLock);
        for (auto it = g_overrides.begin(); it != g_overrides.end();)
        {
            if (key && it->first != *key)
            {
                ++it;
                continue;
            }
            std::vector<Redirect>& stack  = it->second;
            const size_t           before = stack.size();
            stack.erase(std::remove_if(stack.begin(), stack.end(), [&](const Redirect& r) { return r.mod == mod; }),
                        stack.end());
            if (stack.size() != before) restored.push_back(it->first);
            it = stack.empty() ? g_overrides.erase(it) : std::next(it);
        }
        g_overrideCount.store(static_cast<uint32_t>(g_overrides.size()), std::memory_order_release);
        ReleaseSRWLockExclusive(&g_overrideLock);
        for (const std::string& name : restored) WLOG_INFO(kTag, "mod %s restores %s", ModId(mod), name.c_str());
        return restored.size();
    }

    wxl_result WXL_CALL RestoreFile(wxl_str path)
    {
        if (!path.data || !path.size || path.size > kOverrideMax) return WXL_E_INVALID;
        const std::string key = Key(std::string_view(path.data, path.size));
        return DropOverrides(Asking(), &key) ? WXL_OK : WXL_E_NOT_FOUND;
    }

    // --- scoped names, hashes, reloads -------------------------------------------------------

    wxl_str WXL_CALL ScopedNameOf(wxl_str mod, wxl_str path)
    {
        thread_local std::string scoped;
        const std::string_view   owner = mod.data && mod.size ? std::string_view(mod.data, mod.size)
                                                              : std::string_view(ModId(Asking()));
        scoped = wxl::ScopedName(owner, path.data ? std::string_view(path.data, path.size) : std::string_view());
        return wxl_str{scoped.c_str(), static_cast<uint32_t>(scoped.size())};
    }

    wxl_result WXL_CALL HashFile(wxl_str path, uint32_t flags, wxl_file_hash* out)
    {
        const std::string name = Path(path);
        if (name.empty() || !out) return WXL_E_INVALID;
        return wxl::core::services::hashes::Hash(name.c_str(), flags, out);
    }

    wxl_result WXL_CALL HashFileAsync(wxl_str path, uint32_t flags, wxl_file_hash_fn fn, void* user, uint64_t* out)
    {
        const std::string name = Path(path);
        if (name.empty() || !fn || !out) return WXL_E_INVALID;
        return wxl::core::services::hashes::HashAsync(name.c_str(), flags, fn, user, out, Asking());
    }

    wxl_result WXL_CALL ReloadAsset(wxl_str path, wxl_asset_reload* out)
    {
        const std::string name = Path(path);
        if (name.empty()) return WXL_E_INVALID;
        return wxl::core::services::reload::Reload(name.c_str(), out);
    }

    const wxl_storage_api g_storage = {
        sizeof(wxl_storage_api),
        &Exists,
        &Size,
        &Read,
        &OpenFile,
        &ReadAt,
        &Close,
        &AddSearchPath,
        &MountArchive,
        &Find,
        &RemoveSearchPath,
        &RedirectFile,
        &Unredirect,
        &AddProvider,
        &RemoveProvider,
        &AddTransform,
        &RemoveTransform,
        &ServeBytes,
        &Unserve,
        &HashFile,
        &HashFileAsync,
        &OverrideFile,
        &RestoreFile,
        &ScopedNameOf,
        &ReloadAsset,
    };
}

namespace wxl::core::services
{
    const wxl_storage_api& StorageApi() { return g_storage; }

    namespace storage
    {
        void CloseEverything()
        {
            if (!g_open.empty())
                WLOG_WARN(kTag, "closing %zu file(s) a mod left open", g_open.size());
            for (const Open& open : g_open) wxl::core::binding::file_close(open.file);
            g_open.clear();
            g_handles.Clear();
        }

        bool Listing() { return t_listing; }

        void RemoveSearchPathsOf(uint32_t mod)
        {
            DropFolders([mod](const SearchFolder& known) { return known.mod == mod; });
        }

        void ForgetSearchPaths()
        {
            std::lock_guard<std::mutex> lock(g_pathsLock);
            g_paths.clear();
            g_pathCount.store(0, std::memory_order_release);
            g_generation.fetch_add(1, std::memory_order_relaxed);
        }

        bool Exists(const char* name)
        {
            // The client's own lookup, which the FindFile gate answers for the folders and the
            // providers as it does for the client.
            return name && *name && wxl::core::binding::file_exists(name);
        }

        bool AnyFolder() { return g_pathCount.load(std::memory_order_acquire) != 0; }

        bool BestFolder(const char* name, char* out, size_t capacity, int32_t* priority)
        {
            if (!out || !capacity) return false;
            if (g_pathCount.load(std::memory_order_acquire) == 0 || !IsPlainName(name)) return false;

            const std::string relative = Backslashed(name);
            const char*       base     = wxl::core::binding::storage_base_path();
            std::string       file;
            int32_t           won = 0;
            {
                std::lock_guard<std::mutex> lock(g_pathsLock);
                for (SearchFolder& path : g_paths)
                {
                    const std::string candidate = path.directory + "\\" + relative;
                    if (!IsFile(candidate)) continue;
                    const std::string folder = ClientFolder();
                    if (!Inside(path.directory, folder))
                    {
                        if (!path.outside_warned)
                            WLOG_WARN(kTag, "folder %s is outside the client's folder %s; the client "
                                            "cannot open its files",
                                      path.directory.c_str(), folder.c_str());
                        path.outside_warned = true;
                        continue;
                    }
                    // Behind its base path, the client reads "\\<relative>"; with none, the path
                    // itself, which is absolute.
                    file = base && *base ? "\\\\" + candidate.substr(folder.size()) : candidate;
                    won  = path.priority;
                    break;
                }
            }
            if (file.empty() || file.size() + 1 > capacity) return false;
            std::memcpy(out, file.c_str(), file.size() + 1);
            if (priority) *priority = won;
            return true;
        }

        bool Redirected(const char* name, char* out, size_t capacity)
        {
            if (!name || !out || !capacity || g_redirectCount.load(std::memory_order_acquire) == 0) return false;
            const std::string key   = Key(name);
            bool              found = false;
            AcquireSRWLockShared(&g_redirectLock);
            const auto it = g_redirects.find(key);
            if (it != g_redirects.end() && !it->second.empty() && it->second.back().to.size() < capacity)
            {
                const std::string& to = it->second.back().to;
                std::memcpy(out, to.c_str(), to.size() + 1);
                found = true;
            }
            ReleaseSRWLockShared(&g_redirectLock);
            return found;
        }

        void RemoveRedirectsOf(uint32_t mod) { DropRedirects(mod, nullptr); }

        bool Overridden(const char* name, char* out, size_t capacity)
        {
            if (!name || !out || !capacity || g_overrideCount.load(std::memory_order_acquire) == 0) return false;
            const std::string key   = Key(name);
            bool              found = false;
            AcquireSRWLockShared(&g_overrideLock);
            const auto it = g_overrides.find(key);
            if (it != g_overrides.end() && !it->second.empty() && it->second.back().to.size() < capacity)
            {
                const std::string& to = it->second.back().to;
                std::memcpy(out, to.c_str(), to.size() + 1);
                found = true;
            }
            ReleaseSRWLockShared(&g_overrideLock);
            return found;
        }

        void RemoveOverridesOf(uint32_t mod) { DropOverrides(mod, nullptr); }

        void CloseFilesOf(uint32_t mod)
        {
            for (std::size_t i = g_open.size(); i > 0; --i)
            {
                const Open& open = g_open[i - 1];
                if (open.mod != mod) continue;
                wxl::core::binding::file_close(open.file);
                g_handles.Untrack(open.file);
                g_open.erase(g_open.begin() + static_cast<std::ptrdiff_t>(i - 1));
            }
        }
    }
}
