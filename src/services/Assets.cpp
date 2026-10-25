// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The asset pipeline: providers, served bytes, transforms, and the file layer the client reads
// them through.
//
// The client opens every file through SFile::OpenEx, which finds the name with FindFile and, for a
// plain file, opens the path FindFile answered with Blizzard::File::Open; every read, seek, size
// and close of such a file then goes through the verbs of the client's file stack
// (wxl/core/filestack.hpp). The runtime answers a name it serves at the FindFile gate with a path
// of its own, "|wxl|<id>|<name>" -- a bar cannot be in a Windows file name, so no file of the disk
// is ever taken for one --, and puts its verbs into the top node of that stack: a stream of its own
// is answered by the runtime, anything else goes on to the verb the node held. The client then
// reads a provider's file exactly as it reads a loose file, on whichever thread it reads it.

#include "services/Assets.hpp"
#include "services/StorageService.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/core/filestack.hpp"
#include "wxl/host/guard.hpp"
#include "wxl/host/mod_host.hpp"
#include "wxl/scoped_name.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fsk = wxl::core::filestack;

namespace
{
    constexpr char   kTag[]       = "assets";
    constexpr char   kPrefix[]    = "|wxl|";
    constexpr size_t kPrefixSize  = sizeof kPrefix - 1;
    constexpr size_t kIdDigits    = 8;
    constexpr size_t kNameMax     = 240; ///< the longest name served: the client's path buffer is 260
    constexpr size_t kExtensionMax = 15;

    thread_local wxl::core::services::assets::Mode t_mode = wxl::core::services::assets::Mode::Normal;

    /// Whether a provider answered "not yet" on this thread since the last ClearPending.
    thread_local bool t_pending = false;

    /// Whether the thread is inside a mod's wxl.storage call.
    thread_local bool t_asking = false;

    /// Where the thread's last lookup found its name.
    thread_local wxl::core::services::assets::Resolution t_resolution;

    char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

    /// A name as the client compares it: any case, backslashes.
    std::string Key(std::string_view name)
    {
        std::string key(name);
        for (char& c : key) c = c == '/' ? '\\' : Lower(c);
        return key;
    }

    /// A name with backslashes, as a provider is handed it.
    std::string Backslashed(std::string_view name)
    {
        std::string out(name);
        for (char& c : out)
            if (c == '/') c = '\\';
        return out;
    }

    wxl_str Str(const std::string& text) { return wxl_str{text.data(), static_cast<uint32_t>(text.size())}; }

    // --- sources --------------------------------------------------------------------------------

    struct Stream;

    /// A provider or a name a mod serves.
    struct Source
    {
        uint32_t           id       = 0;
        uint32_t           mod      = 0; ///< the host's token for the mod that added it
        int32_t            priority = 0;
        uint64_t           order    = 0; ///< later is larger
        bool               served   = false;
        wxl_asset_provider provider{};   ///< a provider's functions
        std::string        key;          ///< a served name, as Key() spells it
        std::shared_ptr<const std::vector<uint8_t>> bytes; ///< what is served

        std::atomic<uint32_t> calls{0};     ///< calls into the provider in progress
        std::atomic<bool>     gone{false};  ///< taken back: no call starts any more
        std::atomic<bool>     faulted{false};
        std::mutex            streamsLock;
        std::vector<Stream*>  streams;      ///< the provider's files the client holds open
    };

    using SourcePtr = std::shared_ptr<Source>;
    using Sources   = std::vector<SourcePtr>;

    std::mutex                          g_lock;          ///< guards everything below but the counts
    std::shared_ptr<const Sources>      g_providers;     ///< highest priority first, equal ones in order added
    std::unordered_map<std::string, std::vector<SourcePtr>> g_served; ///< by key
    std::unordered_map<uint32_t, SourcePtr> g_byId;
    std::atomic<uint32_t>               g_sourceCount{0};
    std::atomic<uint32_t>               g_nextId{1};
    std::atomic<uint64_t>               g_order{0};

    std::shared_ptr<const Sources> Providers()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_providers;
    }

    SourcePtr ById(uint32_t id)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        const auto it = g_byId.find(id);
        return it == g_byId.end() ? SourcePtr() : it->second;
    }

    void CountSources()
    {
        size_t count = g_providers ? g_providers->size() : 0;
        count += g_served.size();
        g_sourceCount.store(static_cast<uint32_t>(count), std::memory_order_release);
    }

    // --- calls into a mod -----------------------------------------------------------------------

    /// The source whose function the calling thread is inside, so it cannot take itself back.
    thread_local const Source* t_inside = nullptr;

    const char* ModName(uint32_t mod)
    {
        const wxl::host::ModHost* host = wxl::core::Host();
        const wxl::host::LoadedMod* loaded = host ? host->ModOf(mod) : nullptr;
        return loaded ? loaded->id.c_str() : "a mod";
    }

    template <class F>
    void Invoke(void* context)
    {
        (*static_cast<F*>(context))();
    }

    /**
     * @brief Calls into a mod's provider or transform, under the crash guard, as that mod.
     *
     * False when the call did not happen (the source is taken back or faulted) or faulted; a
     * fault disables the mod, on whichever thread it happened.
     */
    template <class F>
    bool Call(uint32_t mod, std::atomic<uint32_t>& calls, const std::atomic<bool>& gone, std::atomic<bool>& faulted,
              const Source* source, const char* what, F&& f)
    {
        if (gone.load(std::memory_order_acquire) || faulted.load(std::memory_order_acquire)) return false;
        calls.fetch_add(1, std::memory_order_acq_rel);
        if (gone.load(std::memory_order_acquire))
        {
            calls.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }

        wxl::host::ModHost* host     = wxl::core::Host();
        const uint32_t      previous = host ? host->SwapCurrentMod(mod) : 0;
        const Source*       outer    = t_inside;
        t_inside                     = source;
        wxl::host::Fault fault;
        const bool       returned = wxl::host::CallGuarded(&Invoke<std::remove_reference_t<F>>,
                                                            const_cast<void*>(static_cast<const void*>(&f)), &fault);
        t_inside                  = outer;
        if (host) host->SwapCurrentMod(previous);
        calls.fetch_sub(1, std::memory_order_acq_rel);

        if (!returned)
        {
            faulted.store(true, std::memory_order_release);
            char described[96];
            wxl::host::DescribeFault(fault, described, sizeof described);
            WLOG_ERROR(kTag, "%s of mod %s faulted: %s; the mod is disabled", what, ModName(mod), described);
            if (host && mod) host->Disable(mod, std::string("in ") + what);
        }
        return returned;
    }

    template <class F>
    bool CallProvider(Source& source, const char* what, F&& f)
    {
        return Call(source.mod, source.calls, source.gone, source.faulted, &source, what, f);
    }

    /// Waits until no call into a source is in progress; it is gone, so none starts.
    void Drain(std::atomic<uint32_t>& calls)
    {
        while (calls.load(std::memory_order_acquire) != 0) ::Sleep(0);
    }

    // --- transforms -----------------------------------------------------------------------------

    struct Transform
    {
        uint32_t            id       = 0;
        uint32_t            mod      = 0;
        int32_t             priority = 0;
        std::string         extension; ///< lower case, no dot
        uint32_t            magic = 0;
        wxl_asset_transform table{};

        std::atomic<uint32_t> calls{0};
        std::atomic<bool>     gone{false};
        std::atomic<bool>     faulted{false};
    };

    using TransformPtr = std::shared_ptr<Transform>;
    using Transforms   = std::vector<TransformPtr>;

    std::shared_ptr<const Transforms> g_transforms; ///< highest priority first; under g_lock
    std::atomic<uint32_t>             g_transformCount{0};

    std::shared_ptr<const Transforms> TransformList()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_transforms;
    }

    /// The extension of a name, lower case, without its dot; empty for none.
    std::string ExtensionOf(const char* name)
    {
        const char* dot   = std::strrchr(name, '.');
        const char* slash = std::strrchr(name, '\\');
        const char* other = std::strrchr(name, '/');
        if (!dot || (slash && slash > dot) || (other && other > dot)) return std::string();
        std::string extension(dot + 1);
        if (extension.size() > kExtensionMax) return std::string();
        for (char& c : extension) c = Lower(c);
        return extension;
    }

    /// What a transform answers into, between its apply and the next.
    struct Output
    {
        std::vector<uint8_t> bytes;
        bool                 replaced = false;
    };

    void* WXL_CALL Replace(void* context, uint64_t size)
    {
        Output* output = static_cast<Output*>(context);
        if (size > SIZE_MAX) return nullptr;
        try
        {
            output->bytes.assign(static_cast<size_t>(size), 0);
        }
        catch (...)
        {
            output->replaced = false;
            return nullptr;
        }
        output->replaced = true;
        return output->bytes.data();
    }

    /// Runs every transform of a file's kind over its bytes, in priority order.
    void RunTransforms(const char* name, std::vector<uint8_t>& bytes)
    {
        const std::shared_ptr<const Transforms> list = TransformList();
        if (!list) return;
        const std::string extension = ExtensionOf(name);
        const std::string path      = Backslashed(name);
        for (const TransformPtr& transform : *list)
        {
            if (transform->extension != extension) continue;
            if (transform->magic)
            {
                uint32_t magic = 0;
                if (bytes.size() < 4) continue;
                std::memcpy(&magic, bytes.data(), 4);
                if (magic != transform->magic) continue;
            }

            Output           output;
            wxl_asset_output out{sizeof(wxl_asset_output), &output, &Replace};
            wxl_result       result = WXL_E_FAILED;
            const auto       apply  = [&] {
                result = transform->table.apply(transform->table.user, Str(path), bytes.data(), bytes.size(), &out);
            };
            if (!Call(transform->mod, transform->calls, transform->gone, transform->faulted, nullptr,
                      "its asset transform (apply)", apply))
                continue;
            if (result != WXL_OK || !output.replaced) continue;
            WLOG_DEBUG(kTag, "%s transformed by mod %s: %zu bytes instead of %zu", path.c_str(),
                       ModName(transform->mod), output.bytes.size(), bytes.size());
            bytes.swap(output.bytes);
        }
    }

    // --- streams --------------------------------------------------------------------------------

    /// The address that marks a stream as the runtime's: its record's file handle.
    char g_mine;

    /**
     * @brief A file the client opened through the layer.
     *
     * Starts with the client's own record, so that whatever the client reads of it directly is
     * sane, and is told apart from the client's by its file handle. Calls on one stream may
     * come from more than one thread; the lock keeps the cursor whole.
     */
    struct Stream
    {
        fsk::StreamRecord record{};
        SourcePtr         source;   ///< a provider's file: its provider
        void*             file = nullptr; ///< the provider's pointer; null once its provider is gone
        std::shared_ptr<const std::vector<uint8_t>> bytes; ///< a file read whole: served or transformed
        uint64_t          size   = 0;
        uint64_t          cursor = 0;
        std::string       path;     ///< the layer path, which the record names
        std::mutex        lock;
    };

    bool Mine(const fsk::StreamRecord* record) { return record && record->file_handle == &g_mine; }

    Stream* AsStream(fsk::StreamRecord* record) { return reinterpret_cast<Stream*>(record); }

    Stream* NewStream(const char* path, uint64_t size)
    {
        Stream* stream = new (std::nothrow) Stream();
        if (!stream) return nullptr;
        stream->path                      = path;
        stream->size                      = size;
        stream->record.file_handle        = &g_mine;
        stream->record.mode               = 1;
        stream->record.info_valid         = true;
        stream->record.info.size          = static_cast<int64_t>(size);
        stream->record.info.attributes    = 0x20;
        stream->record.info.existence     = 1;
        stream->record.info.regular_file  = true;
        stream->record.pathname           = stream->path.data();
        stream->record.info.path          = stream->path.data();
        return stream;
    }

    /// Reads at an offset; how many bytes through @p read. False on a read error.
    bool ReadStream(Stream& stream, uint64_t offset, void* buffer, uint32_t size, uint32_t* read)
    {
        *read = 0;
        if (offset >= stream.size || !size) return true;
        const uint64_t left = stream.size - offset;
        const uint32_t want = left < size ? static_cast<uint32_t>(left) : size;
        if (stream.bytes)
        {
            std::memcpy(buffer, stream.bytes->data() + offset, want);
            *read = want;
            return true;
        }
        if (!stream.source || !stream.file) return false;

        Source&    source = *stream.source;
        wxl_result result = WXL_E_FAILED;
        uint32_t   got    = 0;
        void*      file   = stream.file;
        CallProvider(source, "its asset provider (read_at)", [&] {
            result = source.provider.read_at(source.provider.user, file, offset, buffer, want, &got);
        });
        if (result != WXL_OK) return false;
        *read = got > want ? want : got;
        return true;
    }

    /// Opens what a layer path names: a provider's file, served bytes, or a file to transform.
    Stream* OpenLayer(const char* path)
    {
        uint32_t    id   = 0;
        const char* name = nullptr;
        if (!wxl::core::services::assets::IsLayerPath(path, &id, &name)) return nullptr;
        const std::string backslashed = Backslashed(name);

        if (id == 0)
        {
            // A file a transform converts: read whole through the client, as it would have been
            // read without the transform, then converted.
            std::vector<uint8_t> bytes;
            {
                wxl::core::services::assets::ModeScope inner(wxl::core::services::assets::Mode::Inner);
                void* file = wxl::core::binding::file_open(backslashed.c_str());
                if (!file) return nullptr;
                const uint64_t size = wxl::core::binding::file_size(file);
                try
                {
                    bytes.resize(static_cast<size_t>(size));
                }
                catch (...)
                {
                    wxl::core::binding::file_close(file);
                    return nullptr;
                }
                uint64_t done = 0;
                while (done < size)
                {
                    const uint64_t left = size - done;
                    const uint32_t want = left > 0x10000000u ? 0x10000000u : static_cast<uint32_t>(left);
                    const uint32_t got  = wxl::core::binding::file_read(file, bytes.data() + done, want);
                    if (!got) break;
                    done += got;
                }
                wxl::core::binding::file_close(file);
                if (done != size) return nullptr;
            }
            RunTransforms(backslashed.c_str(), bytes);
            const uint64_t size   = bytes.size();
            Stream*        stream = NewStream(path, size);
            if (!stream) return nullptr;
            stream->bytes = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
            return stream;
        }

        const SourcePtr source = ById(id);
        if (!source || source->gone.load(std::memory_order_acquire)) return nullptr;
        if (source->served)
        {
            Stream* stream = NewStream(path, source->bytes->size());
            if (stream) stream->bytes = source->bytes;
            return stream;
        }

        void*    file = nullptr;
        uint64_t size = 0;
        CallProvider(*source, "its asset provider (open)", [&] {
            file = source->provider.open(source->provider.user, Str(backslashed), &size);
        });
        if (!file) return nullptr;

        Stream* stream = NewStream(path, size);
        if (!stream)
        {
            CallProvider(*source, "its asset provider (close)",
                         [&] { source->provider.close(source->provider.user, file); });
            return nullptr;
        }
        stream->source = source;
        stream->file   = file;
        std::lock_guard<std::mutex> lock(source->streamsLock);
        if (source->gone.load(std::memory_order_acquire))
        {
            // Taken back while it was opening: the provider is closing everything it had.
            CallProvider(*source, "its asset provider (close)", [&] { source->provider.close(source->provider.user, file); });
            delete stream;
            return nullptr;
        }
        source->streams.push_back(stream);
        return stream;
    }

    void CloseStream(Stream* stream)
    {
        if (stream->source)
        {
            Source& source = *stream->source;
            void*   file   = nullptr;
            {
                std::lock_guard<std::mutex> lock(source.streamsLock);
                source.streams.erase(std::remove(source.streams.begin(), source.streams.end(), stream),
                                     source.streams.end());
                file         = stream->file;
                stream->file = nullptr;
            }
            if (file)
                CallProvider(source, "its asset provider (close)", [&] { source.provider.close(source.provider.user, file); });
        }
        delete stream;
    }

    // --- the verbs ------------------------------------------------------------------------------

    std::mutex         g_installLock;
    std::atomic<bool>  g_installed{false};
    fsk::Filesystem*   g_node = nullptr;   ///< the node the verbs were written into
    fsk::Functions     g_original{};       ///< what it held before

    bool Original(fsk::Verb verb, fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        return verb ? verb(fs, parms) : false;
    }

    bool OpenVerb(fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        if (!parms || !parms->path || std::strncmp(parms->path, kPrefix, kPrefixSize) != 0)
            return Original(g_original.open, fs, parms);
        // Reading only: the layer's files are not written.
        Stream* stream = OpenLayer(parms->path);
        if (!stream) return false;
        parms->stream = &stream->record;
        return true;
    }

    bool CloseVerb(fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        if (!parms || !Mine(parms->stream)) return Original(g_original.close, fs, parms);
        CloseStream(AsStream(parms->stream));
        return true;
    }

    bool ReadVerb(fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        if (!parms || !Mine(parms->stream)) return Original(g_original.read, fs, parms);
        Stream&                     stream = *AsStream(parms->stream);
        std::lock_guard<std::mutex> lock(stream.lock);
        uint32_t                    read = 0;
        const uint32_t              want = parms->amount > 0 ? static_cast<uint32_t>(parms->amount) : 0;
        if (!ReadStream(stream, stream.cursor, parms->buffer, want, &read))
        {
            parms->amount = 0;
            return false;
        }
        stream.cursor += read;
        parms->amount = static_cast<int32_t>(read);
        return true;
    }

    bool ReadAtVerb(fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        if (!parms || !Mine(parms->stream)) return Original(g_original.readp, fs, parms);
        Stream&                     stream = *AsStream(parms->stream);
        std::lock_guard<std::mutex> lock(stream.lock);
        uint32_t                    read = 0;
        const uint32_t              want = parms->amount > 0 ? static_cast<uint32_t>(parms->amount) : 0;
        const uint64_t              at   = parms->pos > 0 ? static_cast<uint64_t>(parms->pos) : 0;
        if (!ReadStream(stream, at, parms->buffer, want, &read))
        {
            parms->amount = 0;
            return false;
        }
        parms->amount = static_cast<int32_t>(read);
        return true;
    }

    bool GetPosVerb(fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        if (!parms || !Mine(parms->stream)) return Original(g_original.getpos, fs, parms);
        Stream&                     stream = *AsStream(parms->stream);
        std::lock_guard<std::mutex> lock(stream.lock);
        parms->pos = static_cast<int64_t>(stream.cursor);
        return true;
    }

    bool SetPosVerb(fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        if (!parms || !Mine(parms->stream)) return Original(g_original.setpos, fs, parms);
        Stream&                     stream = *AsStream(parms->stream);
        std::lock_guard<std::mutex> lock(stream.lock);
        int64_t from = 0;
        if (parms->pos_mode == 1) from = static_cast<int64_t>(stream.cursor);
        if (parms->pos_mode == 2) from = static_cast<int64_t>(stream.size);
        const int64_t to = from + parms->pos;
        if (to < 0) return false;
        stream.cursor = static_cast<uint64_t>(to);
        return true;
    }

    bool ProvidedEntry(const char* path, bool& folder);

    /// The info of an entry a provider lists and the disk has not: a folder or a file
    /// whose length is known once it is open.
    void ProvidedInfo(fsk::FileInfo& info, bool folder)
    {
        info              = fsk::FileInfo{};
        info.attributes   = folder ? 0x40 : 0x20;
        info.existence    = folder ? 2 : 1;
        info.regular_file = !folder;
    }

    bool GetFileInfoVerb(fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        if (!parms) return Original(g_original.getfileinfo, fs, parms);
        if (Mine(parms->stream))
        {
            parms->info = &parms->stream->info;
            return true;
        }
        if (!parms->stream && parms->path && std::strncmp(parms->path, kPrefix, kPrefixSize) == 0)
        {
            // Asked by path: a layer file exists while its source does. Its length is known only
            // once it is open.
            uint32_t    id   = 0;
            const char* name = nullptr;
            const bool  ours = wxl::core::services::assets::IsLayerPath(parms->path, &id, &name);
            const SourcePtr source = ours && id ? ById(id) : SourcePtr();
            if (!ours || (id && (!source || source->gone.load()))) return false;
            fsk::FileInfo* info = parms->info ? parms->info : &parms->info2;
            *info               = fsk::FileInfo{};
            info->attributes    = 0x20;
            info->existence     = 1;
            info->regular_file  = true;
            if (source && source->served) info->size = static_cast<int64_t>(source->bytes->size());
            parms->info = info;
            return true;
        }
        if (Original(g_original.getfileinfo, fs, parms)) return true;
        // Not on the disk: an entry a provider lists in its folder (the client lists a folder and
        // then asks each entry's info, as the add-on list does).
        bool folder = false;
        if (parms->stream || !parms->path || !ProvidedEntry(parms->path, folder)) return false;
        fsk::FileInfo* info = parms->info ? parms->info : &parms->info2;
        ProvidedInfo(*info, folder);
        parms->info = info;
        return true;
    }

    bool ExistsVerb(fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        if (parms && parms->path && std::strncmp(parms->path, kPrefix, kPrefixSize) == 0 && parms->info)
        {
            uint32_t    id   = 0;
            const char* name = nullptr;
            const bool  ours = wxl::core::services::assets::IsLayerPath(parms->path, &id, &name);
            const SourcePtr source = ours && id ? ById(id) : SourcePtr();
            parms->info->existence = ours && (!id || (source && !source->gone.load())) ? 1 : 0;
            return true;
        }
        const bool result = Original(g_original.exists, fs, parms);
        bool       folder = false;
        if (parms && parms->path && parms->info && (!result || parms->info->existence == 0) &&
            ProvidedEntry(parms->path, folder))
        {
            parms->info->existence = folder ? 2 : 1;
            return true;
        }
        return result;
    }

    /// A verb that writes or changes a file: the layer's files are read only.
    template <fsk::Verb fsk::Functions::*Member>
    bool ReadOnlyVerb(fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        if (parms && Mine(parms->stream)) return false;
        return Original(g_original.*Member, fs, parms);
    }

    bool FlushVerb(fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        if (parms && Mine(parms->stream)) return true;
        return Original(g_original.flush, fs, parms);
    }

    // --- a folder's listing ---------------------------------------------------------------------

    /// What a listing of the disk passed on, so that a provider's entry is told once.
    struct Listing
    {
        fsk::DirCallback                callback = nullptr;
        void*                           cookie   = nullptr;
        std::unordered_set<std::string> seen;
        bool                            stopped = false;
    };

    thread_local Listing* t_listing = nullptr;

    bool ListedEntry(const fsk::ProcessDirParms& parms)
    {
        Listing* listing = t_listing;
        if (!listing) return true;
        if (parms.filename) listing->seen.insert(Key(parms.filename));
        fsk::ProcessDirParms forwarded = parms;
        forwarded.cookie               = listing->cookie;
        const bool more                = listing->callback(forwarded);
        if (!more) listing->stopped = true;
        return more;
    }

    /// The folder a disk path is, relative to the client's folder; false when it is outside. A
    /// relative path is the client's folder's already (its working folder).
    bool RelativeFolder(const char* path, std::string& out)
    {
        if (!path || !*path) return false;
        if (path[0] != '\\' && path[0] != '/' && path[1] != ':')
        {
            out = Backslashed(path);
            while (!out.empty() && out.back() == '\\') out.pop_back();
            return out.find("..") == std::string::npos;
        }
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
            if (!length || length >= MAX_PATH) return false;
            folder.assign(current, length);
        }
        if (!folder.empty() && folder.back() != '\\') folder += '\\';
        std::string asked = Backslashed(path);
        while (asked.size() > 1 && asked.back() == '\\') asked.pop_back();
        if ((asked + "\\").size() < folder.size() || _strnicmp((asked + "\\").c_str(), folder.c_str(), folder.size()) != 0)
            return false;
        out = asked.size() + 1 > folder.size() ? asked.substr(folder.size()) : std::string();
        return true;
    }

    struct Entry
    {
        std::string name;
        bool        folder;
    };

    void WXL_CALL CollectEntry(void* context, wxl_str name, wxl_bool is_folder)
    {
        if (!name.data || !name.size) return;
        static_cast<std::vector<Entry>*>(context)->push_back(Entry{std::string(name.data, name.size), is_folder != WXL_FALSE});
    }

    /// What the providers list in a folder, highest priority first; each name once.
    std::vector<Entry> ProvidersList(const std::string& folder)
    {
        std::vector<Entry>                    all;
        const std::shared_ptr<const Sources> list = Providers();
        if (!list) return all;
        for (const SourcePtr& source : *list)
        {
            if (!source->provider.list) continue;
            std::vector<Entry> entries;
            CallProvider(*source, "its asset provider (list)", [&] {
                source->provider.list(source->provider.user, Str(folder), &CollectEntry, &entries);
            });
            all.insert(all.end(), entries.begin(), entries.end());
        }
        return all;
    }

    bool AnyLister()
    {
        const std::shared_ptr<const Sources> list = Providers();
        if (!list) return false;
        return std::any_of(list->begin(), list->end(), [](const SourcePtr& s) { return s->provider.list != nullptr; });
    }

    /// Whether a disk path under the client's folder is an entry some provider lists in its
    /// folder, and whether it is a folder.
    bool ProvidedEntry(const char* path, bool& folder)
    {
        if (!path || g_sourceCount.load(std::memory_order_acquire) == 0 || !AnyLister()) return false;
        std::string relative;
        if (!RelativeFolder(path, relative) || relative.empty()) return false;
        const size_t      cut    = relative.find_last_of('\\');
        const std::string parent = cut == std::string::npos ? std::string() : relative.substr(0, cut);
        const std::string name   = Key(cut == std::string::npos ? relative : relative.substr(cut + 1));
        if (name.empty()) return false;
        for (const Entry& entry : ProvidersList(parent))
        {
            if (Key(entry.name) != name) continue;
            folder = entry.folder;
            return true;
        }
        return false;
    }

    bool DirWalkVerb(fsk::Filesystem* fs, fsk::FileParms* parms)
    {
        if (!parms || !parms->dircallback || !parms->path || g_sourceCount.load(std::memory_order_acquire) == 0 ||
            t_listing || !AnyLister())
            return Original(g_original.dirwalk, fs, parms);
        std::string folder;
        if (!RelativeFolder(parms->path, folder)) return Original(g_original.dirwalk, fs, parms);

        Listing listing;
        listing.callback = parms->dircallback;
        listing.cookie   = parms->cookie;
        t_listing        = &listing;
        fsk::FileParms walked = *parms;
        walked.dircallback    = &ListedEntry;
        const bool result     = Original(g_original.dirwalk, fs, &walked);
        t_listing             = nullptr;
        if (listing.stopped) return result;

        // The providers' entries the disk did not have, told as the disk's are.
        for (const Entry& entry : ProvidersList(folder))
        {
            if (!listing.seen.insert(Key(entry.name)).second) continue;
            fsk::ProcessDirParms told{};
            told.root     = parms->path;
            told.filename = entry.name.c_str();
            told.cookie   = parms->cookie;
            told.is_dir   = entry.folder;
            if (!parms->dircallback(told)) break;
        }
        return true;
    }

    // --- registration ---------------------------------------------------------------------------

    void Rebuild(Sources providers)
    {
        std::stable_sort(providers.begin(), providers.end(),
                         [](const SourcePtr& a, const SourcePtr& b) { return a->priority > b->priority; });
        g_providers = std::make_shared<const Sources>(std::move(providers));
    }

    /// Takes a source out of the lists, then waits for its calls and closes its files.
    void Retire(const SourcePtr& source)
    {
        source->gone.store(true, std::memory_order_release);
        Drain(source->calls);
        std::vector<void*> open;
        {
            std::lock_guard<std::mutex> lock(source->streamsLock);
            for (Stream* stream : source->streams)
            {
                open.push_back(stream->file);
                stream->file = nullptr;
            }
            source->streams.clear();
        }
        if (open.empty() || source->faulted.load(std::memory_order_acquire)) return;

        // The provider closes what the client still holds: no other call starts now, so these
        // are its last. The client's reads of those files fail from here on; its close frees
        // what is left of each.
        std::atomic<bool> stillHere{false};
        for (void* file : open)
            Call(source->mod, source->calls, stillHere, source->faulted, source.get(), "its asset provider (close)",
                 [&] { source->provider.close(source->provider.user, file); });
        WLOG_INFO(kTag, "provider of mod %s taken back with %zu file(s) the client held open; they read as "
                        "errors from now on",
                  ModName(source->mod), open.size());
    }

    wxl_result RemoveSource(uint64_t handle, uint32_t mod, bool served)
    {
        if (handle == 0 || handle > UINT32_MAX) return WXL_E_NOT_FOUND;
        SourcePtr source;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            const auto it = g_byId.find(static_cast<uint32_t>(handle));
            if (it == g_byId.end() || it->second->mod != mod || it->second->served != served) return WXL_E_NOT_FOUND;
            source = it->second;
            if (t_inside == source.get()) return WXL_E_INVALID;
            g_byId.erase(it);
            Sources kept;
            if (g_providers)
                for (const SourcePtr& s : *g_providers)
                    if (s != source) kept.push_back(s);
            Rebuild(std::move(kept));
            CountSources();
        }
        Retire(source);
        return WXL_OK;
    }
}

namespace wxl::core::services::assets
{
    bool Active()
    {
        return g_sourceCount.load(std::memory_order_acquire) != 0 || g_transformCount.load(std::memory_order_acquire) != 0;
    }

    Mode Current() { return t_mode; }

    bool ModAsking() { return t_asking; }

    AskingScope::AskingScope() : previous_(t_asking) { t_asking = true; }
    AskingScope::~AskingScope() { t_asking = previous_; }

    void ClearResolution() { t_resolution = Resolution{}; }

    void NoteResolution(const Resolution& resolution) { t_resolution = resolution; }

    const Resolution& LastResolution() { return t_resolution; }

    bool AnySource(const char* name, Candidate* out, uint32_t* layer)
    {
        Candidate found{};
        if (!BestSource(name, INT32_MIN, &found)) return false;
        if (out) *out = found;
        if (layer) *layer = SourceLayer(found.id, nullptr);
        return true;
    }

    uint32_t SourceLayer(uint32_t id, std::string* mod)
    {
        const SourcePtr source = ById(id);
        if (!source) return WXL_FILE_LAYER_NONE;
        if (mod) *mod = ModName(source->mod);
        return source->served ? WXL_FILE_LAYER_SERVED : WXL_FILE_LAYER_PROVIDER;
    }

    bool Unscoped(const char* name, char* out, size_t capacity)
    {
        if (!name || !out || !capacity || !std::strchr(name, '[')) return false;
        wxl::ScopedParts parts;
        if (!wxl::SplitScopedName(name, &parts) || parts.base.size() + 1 > capacity) return false;
        // The scoped name itself first: a folder of the search path or a source that has it.
        char folder[512];
        if (wxl::core::services::storage::BestFolder(name, folder, sizeof folder, nullptr)) return false;
        if (AnySource(name, nullptr, nullptr)) return false;
        std::memcpy(out, parts.base.c_str(), parts.base.size() + 1);
        return true;
    }

    void ClearPending() { t_pending = false; }

    bool PendingSeen() { return t_pending; }

    ModeScope::ModeScope(Mode mode) : previous_(t_mode) { t_mode = mode; }
    ModeScope::~ModeScope() { t_mode = previous_; }

    bool BestSource(const char* name, int32_t floor, Candidate* out)
    {
        if (!name || !*name || g_sourceCount.load(std::memory_order_acquire) == 0) return false;
        const size_t length = std::strlen(name);
        if (length > kNameMax) return false;

        // Served bytes: one lookup, the best entry of the name.
        SourcePtr served;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (!g_served.empty())
            {
                const auto it = g_served.find(Key(std::string_view(name, length)));
                if (it != g_served.end())
                    for (const SourcePtr& s : it->second)
                        if (!served || s->priority > served->priority ||
                            (s->priority == served->priority && s->order > served->order))
                            served = s;
            }
        }
        if (served && served->priority <= floor) served.reset();

        const std::shared_ptr<const Sources> list = Providers();
        if (list)
        {
            const std::string asked = Backslashed(std::string_view(name, length));
            for (const SourcePtr& source : *list)
            {
                if (source->priority <= floor) break;
                if (served && source->priority <= served->priority) break;
                wxl_result has = WXL_E_NOT_FOUND;
                CallProvider(*source, "its asset provider (lookup)",
                             [&] { has = source->provider.lookup(source->provider.user, Str(asked)); });
                if (has == WXL_E_PENDING) t_pending = true;
                if (has != WXL_OK) continue;
                if (out) *out = Candidate{source->id, source->priority};
                return true;
            }
        }
        if (!served) return false;
        if (out) *out = Candidate{served->id, served->priority};
        return true;
    }

    bool Transformed(const char* name)
    {
        if (g_transformCount.load(std::memory_order_acquire) == 0 || !name) return false;
        const std::string extension = ExtensionOf(name);
        if (extension.empty()) return false;
        const std::shared_ptr<const Transforms> list = TransformList();
        return list && std::any_of(list->begin(), list->end(),
                                   [&](const TransformPtr& t) { return t->extension == extension; });
    }

    bool LayerPath(uint32_t id, const char* name, char* path, size_t capacity)
    {
        if (!name || !path || !Install()) return false;
        const int written = std::snprintf(path, capacity, "%s%08X|%s", kPrefix, id, name);
        return written > 0 && static_cast<size_t>(written) < capacity;
    }

    bool IsLayerPath(const char* path, uint32_t* id, const char** name)
    {
        if (!path || std::strncmp(path, kPrefix, kPrefixSize) != 0) return false;
        const char* digits = path + kPrefixSize;
        uint32_t    value  = 0;
        for (size_t i = 0; i < kIdDigits; ++i)
        {
            const char c = digits[i];
            uint32_t   d = 0;
            if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
            else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
            else return false;
            value = value << 4 | d;
        }
        if (digits[kIdDigits] != '|') return false;
        if (id) *id = value;
        if (name) *name = digits + kIdDigits + 1;
        return true;
    }

    wxl_result AddProvider(const wxl_asset_provider* provider, int32_t priority, uint64_t* out, uint32_t mod)
    {
        if (!provider || !out || provider->struct_size < offsetof(wxl_asset_provider, list) ||
            !provider->lookup || !provider->open || !provider->read_at || !provider->close)
            return WXL_E_INVALID;
        auto source = std::make_shared<Source>();
        std::memcpy(&source->provider, provider,
                    std::min<size_t>(provider->struct_size, sizeof(wxl_asset_provider)));
        source->provider.struct_size = sizeof(wxl_asset_provider);
        source->id       = g_nextId.fetch_add(1, std::memory_order_relaxed);
        source->mod      = mod;
        source->priority = priority;
        source->order    = g_order.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_lock);
            Sources all = g_providers ? *g_providers : Sources();
            all.push_back(source);
            Rebuild(std::move(all));
            g_byId[source->id] = source;
            CountSources();
        }
        *out = source->id;
        WLOG_INFO(kTag, "mod %s added a provider at priority %d", ModName(mod), priority);
        return WXL_OK;
    }

    wxl_result RemoveProvider(uint64_t handle, uint32_t mod) { return RemoveSource(handle, mod, false); }

    wxl_result AddTransform(const wxl_asset_transform* transform, int32_t priority, uint64_t* out, uint32_t mod)
    {
        if (!transform || !out || transform->struct_size < sizeof(wxl_asset_transform) || !transform->apply ||
            !transform->extension.data || !transform->extension.size || transform->extension.size > kExtensionMax)
            return WXL_E_INVALID;
        auto entry       = std::make_shared<Transform>();
        entry->table     = *transform;
        entry->extension = std::string(transform->extension.data, transform->extension.size);
        if (!entry->extension.empty() && entry->extension[0] == '.') entry->extension.erase(0, 1);
        for (char& c : entry->extension) c = Lower(c);
        if (entry->extension.empty()) return WXL_E_INVALID;
        entry->table.extension = wxl_str{nullptr, 0};
        entry->magic           = transform->magic;
        entry->id              = g_nextId.fetch_add(1, std::memory_order_relaxed);
        entry->mod             = mod;
        entry->priority        = priority;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            Transforms all = g_transforms ? *g_transforms : Transforms();
            all.push_back(entry);
            std::stable_sort(all.begin(), all.end(),
                             [](const TransformPtr& a, const TransformPtr& b) { return a->priority > b->priority; });
            g_transforms = std::make_shared<const Transforms>(std::move(all));
            g_transformCount.store(static_cast<uint32_t>(g_transforms->size()), std::memory_order_release);
        }
        Install();
        *out = entry->id;
        WLOG_INFO(kTag, "mod %s added a transform of .%s files at priority %d", ModName(mod), entry->extension.c_str(),
                  priority);
        return WXL_OK;
    }

    wxl_result RemoveTransform(uint64_t handle, uint32_t mod)
    {
        TransformPtr gone;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (!g_transforms) return WXL_E_NOT_FOUND;
            Transforms kept;
            for (const TransformPtr& t : *g_transforms)
            {
                if (!gone && t->id == handle && t->mod == mod) gone = t;
                else kept.push_back(t);
            }
            if (!gone) return WXL_E_NOT_FOUND;
            g_transforms = std::make_shared<const Transforms>(std::move(kept));
            g_transformCount.store(static_cast<uint32_t>(g_transforms->size()), std::memory_order_release);
        }
        gone->gone.store(true, std::memory_order_release);
        Drain(gone->calls);
        return WXL_OK;
    }

    wxl_result Serve(wxl_str path, const void* data, uint32_t size, int32_t priority, uint32_t mod)
    {
        if (!path.data || !path.size || path.size > kNameMax || (size && !data)) return WXL_E_INVALID;
        auto source      = std::make_shared<Source>();
        source->served   = true;
        source->key      = Key(std::string_view(path.data, path.size));
        source->id       = g_nextId.fetch_add(1, std::memory_order_relaxed);
        source->mod      = mod;
        source->priority = priority;
        source->order    = g_order.fetch_add(1, std::memory_order_relaxed);
        try
        {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            source->bytes        = std::make_shared<const std::vector<uint8_t>>(bytes, bytes + size);
        }
        catch (...)
        {
            return WXL_E_FAILED;
        }
        SourcePtr replaced;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            std::vector<SourcePtr>& holders = g_served[source->key];
            for (auto it = holders.begin(); it != holders.end(); ++it)
            {
                if ((*it)->mod != mod) continue;
                replaced = *it;
                g_byId.erase(replaced->id);
                holders.erase(it);
                break;
            }
            holders.push_back(source);
            g_byId[source->id] = source;
            CountSources();
        }
        if (replaced) replaced->gone.store(true, std::memory_order_release);
        Install();
        WLOG_DEBUG(kTag, "mod %s serves %.*s (%u bytes) at priority %d", ModName(mod), static_cast<int>(path.size),
                   path.data, size, priority);
        return WXL_OK;
    }

    wxl_result Unserve(wxl_str path, uint32_t mod)
    {
        if (!path.data || !path.size || path.size > kNameMax) return WXL_E_INVALID;
        const std::string           key = Key(std::string_view(path.data, path.size));
        std::lock_guard<std::mutex> lock(g_lock);
        const auto                  it = g_served.find(key);
        if (it == g_served.end()) return WXL_E_NOT_FOUND;
        std::vector<SourcePtr>& holders = it->second;
        for (auto h = holders.begin(); h != holders.end(); ++h)
        {
            if ((*h)->mod != mod) continue;
            (*h)->gone.store(true, std::memory_order_release);
            g_byId.erase((*h)->id);
            holders.erase(h);
            if (holders.empty()) g_served.erase(it);
            CountSources();
            return WXL_OK;
        }
        return WXL_E_NOT_FOUND;
    }

    void ListMatching(const char* pattern, const std::function<void(const std::string&)>& keep,
                      const std::function<bool(const char*, const char*)>& matches)
    {
        if (!pattern || g_sourceCount.load(std::memory_order_acquire) == 0) return;
        {
            std::vector<std::string> served;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                for (const auto& [key, holders] : g_served)
                    if (!holders.empty()) served.push_back(key);
            }
            for (const std::string& name : served)
                if (matches(name.c_str(), pattern)) keep(name);
        }
        if (!AnyLister()) return;

        // The folder the pattern starts in, walked down from there.
        std::string start = Backslashed(pattern);
        const size_t star = start.find('*');
        if (star != std::string::npos) start.erase(star);
        const size_t slash = start.rfind('\\');
        start              = slash == std::string::npos ? std::string() : start.substr(0, slash);

        std::vector<std::string>        folders{start};
        std::unordered_set<std::string> walked;
        while (!folders.empty() && walked.size() < 100000)
        {
            const std::string folder = folders.back();
            folders.pop_back();
            if (!walked.insert(Key(folder)).second) continue;
            for (const Entry& entry : ProvidersList(folder))
            {
                const std::string full = folder.empty() ? entry.name : folder + "\\" + entry.name;
                if (entry.folder) folders.push_back(full);
                else if (matches(full.c_str(), pattern)) keep(full);
            }
        }
    }

    void ReleaseOf(uint32_t mod)
    {
        std::vector<SourcePtr>    retired;
        std::vector<TransformPtr> gone;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            Sources kept;
            if (g_providers)
                for (const SourcePtr& s : *g_providers)
                    (s->mod == mod ? retired : kept).push_back(s);
            Rebuild(std::move(kept));
            for (auto it = g_served.begin(); it != g_served.end();)
            {
                std::vector<SourcePtr>& holders = it->second;
                for (auto h = holders.begin(); h != holders.end();)
                {
                    if ((*h)->mod != mod)
                    {
                        ++h;
                        continue;
                    }
                    retired.push_back(*h);
                    h = holders.erase(h);
                }
                it = holders.empty() ? g_served.erase(it) : std::next(it);
            }
            for (const SourcePtr& s : retired) g_byId.erase(s->id);
            CountSources();

            if (g_transforms)
            {
                Transforms kept2;
                for (const TransformPtr& t : *g_transforms) (t->mod == mod ? gone : kept2).push_back(t);
                g_transforms = std::make_shared<const Transforms>(std::move(kept2));
                g_transformCount.store(static_cast<uint32_t>(g_transforms->size()), std::memory_order_release);
            }
        }
        for (const SourcePtr& s : retired) Retire(s);
        for (const TransformPtr& t : gone)
        {
            t->gone.store(true, std::memory_order_release);
            Drain(t->calls);
        }
        if (!retired.empty() || !gone.empty())
            WLOG_INFO(kTag, "mod %s: %zu source(s) and %zu transform(s) taken back", ModName(mod), retired.size(),
                      gone.size());
    }

    void ReleaseAll()
    {
        std::vector<uint32_t> mods;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (g_providers)
                for (const SourcePtr& s : *g_providers) mods.push_back(s->mod);
            for (const auto& [key, holders] : g_served)
                for (const SourcePtr& s : holders) mods.push_back(s->mod);
            if (g_transforms)
                for (const TransformPtr& t : *g_transforms) mods.push_back(t->mod);
        }
        std::sort(mods.begin(), mods.end());
        mods.erase(std::unique(mods.begin(), mods.end()), mods.end());
        for (uint32_t mod : mods) ReleaseOf(mod);
    }

    bool Install()
    {
        if (g_installed.load(std::memory_order_acquire)) return true;
        std::lock_guard<std::mutex> lock(g_installLock);
        if (g_installed.load(std::memory_order_relaxed)) return true;

        void** top = wxl::core::binding::filestack_top();
        if (!top || !*top || *top == wxl::core::binding::filestack_initial()) return false;
        fsk::Filesystem* node = static_cast<fsk::Filesystem*>(*top);

        // The node's verbs are kept before any is replaced, so a verb of ours never finds an
        // original it does not have yet; each slot is one aligned pointer, written whole.
        g_original = node->verbs;
        g_node     = node;
        const auto put = [](fsk::Verb* slot, fsk::Verb verb) {
            ::InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(verb));
        };
        put(&node->verbs.open, &OpenVerb);
        put(&node->verbs.close, &CloseVerb);
        put(&node->verbs.read, &ReadVerb);
        put(&node->verbs.readp, &ReadAtVerb);
        put(&node->verbs.getpos, &GetPosVerb);
        put(&node->verbs.setpos, &SetPosVerb);
        put(&node->verbs.getfileinfo, &GetFileInfoVerb);
        put(&node->verbs.exists, &ExistsVerb);
        put(&node->verbs.dirwalk, &DirWalkVerb);
        put(&node->verbs.flush, &FlushVerb);
        put(&node->verbs.write, &ReadOnlyVerb<&fsk::Functions::write>);
        put(&node->verbs.writep, &ReadOnlyVerb<&fsk::Functions::writep>);
        put(&node->verbs.seteof, &ReadOnlyVerb<&fsk::Functions::seteof>);
        put(&node->verbs.setcachemode, &ReadOnlyVerb<&fsk::Functions::setcachemode>);
        g_installed.store(true, std::memory_order_release);
        WLOG_INFO(kTag, "file layer in the client's file stack");
        return true;
    }

    void ForgetInstall()
    {
        std::lock_guard<std::mutex> lock(g_installLock);
        g_installed.store(false, std::memory_order_release);
        g_node     = nullptr;
        g_original = fsk::Functions{};
    }
}
