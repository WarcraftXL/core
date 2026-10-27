// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.texture service.
//
// A request names a source file, a size, restyled groups and a tint. The source is read (a mod's
// folder from the disk, anything else as the client reads its files) and hashed; with the rest of
// the request that hash makes the texture's name, "Interface\\WXL\\<origin>\\<stem>_<w>x<h>_<hash>.blp".
// A name already known is answered at once; a new one is drawn (src/imaging/), written as a BLP2
// and kept. The client reaches the bytes through one provider of the runtime's own (wxl.storage's
// sources, src/services/Assets.cpp), asked before the archives, which answers only names under
// "Interface\\WXL\\": every thread, every loader, both clients, D3D9 and Vulkan alike read them as
// any loose file.
//
// What is kept: each name's recipe (the source's bytes, when small, and the request), and the
// texture's bytes while they fit the budget. Past the budget the least used bytes are let go and
// drawn again from the recipe if the client asks for the name again; past the count of names the
// least used name is forgotten.

#include "services/TextureService.hpp"

#include "common/Log.hpp"
#include "imaging/Imaging.hpp"
#include "runtime/Core.hpp"
#include "services/Assets.hpp"
#include "services/ScriptService.hpp"
#include "services/StorageService.hpp"

#include "wxl/host/mod_host.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace img = wxl::core::imaging;

namespace
{
    constexpr char kTag[] = "texture";

    /// The provider's priority: above the client's archives (0) and above what mods usually use,
    /// though no other source has a reason to hold a name under Interface\WXL\.
    constexpr int32_t kPriority = 1000;

    /// Changes whenever a texture drawn from the same request would come out different: it is in
    /// every name, so a new runtime never answers a texture an older one drew.
    constexpr uint32_t kRecipeVersion = 1;

    constexpr char     kPrefix[]        = "interface\\wxl\\";
    constexpr size_t   kSourceMax       = 16u << 20; ///< the largest source file read
    constexpr size_t   kSourceKeptMax   = 1u << 20;  ///< the largest kept to draw a texture again
    constexpr uint64_t kDefaultBytes    = 64ull << 20;
    constexpr uint32_t kDefaultTextures = 2048;

    struct Recipe
    {
        std::string                                 path;   ///< as answered
        std::string                                 source; ///< as resolved, for the log
        img::Kind                                   kind = img::Kind::Unknown;
        uint64_t                                    content = 0;
        uint32_t                                    width = 0, height = 0, textureWidth = 0, textureHeight = 0;
        std::vector<img::GroupStyle>                groups;
        uint32_t                                    tint = 0;
        std::shared_ptr<const std::vector<uint8_t>> bytes; ///< the source, when small enough to keep
    };

    struct Entry
    {
        Recipe                                      recipe;
        std::shared_ptr<const std::vector<uint8_t>> blp; ///< null once let go
        uint64_t                                    used = 0;
        bool                                        pinned = false; ///< data served by the runtime: never let go
    };

    std::mutex                             g_lock;
    std::unordered_map<std::string, Entry> g_entries; ///< by the path in lower case
    uint64_t                               g_tick     = 0;
    uint64_t                               g_bytes    = 0;
    uint64_t                               g_limit    = kDefaultBytes;
    uint32_t                               g_maxNames = kDefaultTextures;
    uint64_t                               g_made = 0, g_reused = 0, g_released = 0;
    uint64_t                               g_provider = 0; ///< the provider's handle, 0 before the first texture
    std::filesystem::path                  g_modsOverride;

    std::string Lower(std::string_view text)
    {
        std::string out(text);
        for (char& c : out) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    bool StartsWith(const std::string& lower, const char* prefix)
    {
        return lower.compare(0, std::strlen(prefix), prefix) == 0;
    }

    /// A refusal: the result, its reason in @p out and in the log.
    wxl_result Refuse(wxl_texture* out, wxl_result result, const char* format, ...)
    {
        char    reason[256];
        va_list args;
        va_start(args, format);
        std::vsnprintf(reason, sizeof reason, format, args);
        va_end(args);
        if (out) std::snprintf(out->reason, sizeof out->reason, "%s", reason);
        WLOG_WARN(kTag, "%s", reason);
        return result;
    }

    std::filesystem::path ModsDirectory()
    {
        if (!g_modsOverride.empty()) return g_modsOverride;
        wxl::host::ModHost* host = wxl::core::Host();
        return host ? host->Config().mods_directory : std::filesystem::path();
    }

    /// Reads a file of the disk whole, refusing one past kSourceMax.
    wxl_result ReadDisk(const std::filesystem::path& file, std::vector<uint8_t>& bytes)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) return WXL_E_NOT_FOUND;
        const uintmax_t size = std::filesystem::file_size(file, ec);
        if (ec) return WXL_E_FAILED;
        if (size > kSourceMax) return WXL_E_INVALID;
        std::ifstream in(file, std::ios::binary);
        if (!in) return WXL_E_FAILED;
        bytes.resize(static_cast<size_t>(size));
        if (size && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) return WXL_E_FAILED;
        return WXL_OK;
    }

    /// Reads a file the client has, as the client finds it.
    wxl_result ReadClient(const std::string& name, std::vector<uint8_t>& bytes)
    {
        const wxl_storage_api& storage = wxl::core::services::StorageApi();
        const wxl_str          path{name.data(), static_cast<uint32_t>(name.size())};
        uint64_t               size = 0;
        const wxl_result       sized = storage.size(path, &size);
        if (sized != WXL_OK) return sized;
        if (size > kSourceMax) return WXL_E_INVALID;
        bytes.resize(static_cast<size_t>(size));
        uint64_t read = 0;
        const wxl_result result = storage.read(path, bytes.data(), size, &read);
        if (result != WXL_OK) return result;
        bytes.resize(static_cast<size_t>(read));
        return WXL_OK;
    }

    /// A name's part kept for the texture's path: letters, digits, '.', '-' and '_'.
    std::string Tidy(std::string_view text, size_t most)
    {
        std::string out;
        for (char c : text)
        {
            if (out.size() >= most) break;
            const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_';
            out.push_back(ok ? c : '_');
        }
        return out.empty() ? std::string("_") : out;
    }

    /// Where a source is and what it is called in the texture's path.
    struct Source
    {
        std::string           name;   ///< as resolved: "Mods\\<folder>\\..." or a client name
        std::string           origin; ///< the mod's folder, the add-on, or "Client"
        std::string           stem;   ///< the file's name without its extension
        std::vector<uint8_t>  bytes;
    };

    /// Resolves and reads a source. @p mod is the calling mod's token, 0 for the interface.
    wxl_result ReadSource(std::string_view given, uint32_t mod, Source& source, wxl_texture* out)
    {
        std::string name(given);
        for (char& c : name)
            if (c == '/') c = '\\';
        if (name.empty() || name.size() > 259) return Refuse(out, WXL_E_INVALID, "make: the source must be a name of 1 to 259 bytes");
        if (name.find(':') != std::string::npos || name[0] == '\\')
            return Refuse(out, WXL_E_INVALID,
                          "make: '%s' is a path of the disk; name a file of a mod's folder (\"Mods\\<folder>\\...\", "
                          "\".\\...\" for the calling mod's own) or one the client has",
                          name.c_str());
        {
            const std::string padded = "\\" + name + "\\";
            if (padded.find("\\..\\") != std::string::npos)
                return Refuse(out, WXL_E_INVALID, "make: '%s' goes up a folder (\"..\"), which a source may not", name.c_str());
        }

        if (name.size() >= 2 && name[0] == '.' && name[1] == '\\')
        {
            wxl::host::ModHost*         host   = wxl::core::Host();
            const wxl::host::LoadedMod* loaded = host && mod ? host->ModOf(mod) : nullptr;
            if (!loaded)
                return Refuse(out, WXL_E_INVALID,
                              "make: '%s' names the calling mod's folder, and no mod is calling; an add-on names its file "
                              "\"Interface\\AddOns\\<add-on>\\...\"",
                              name.c_str());
            name = "Mods\\" + loaded->directory_name + "\\" + name.substr(2);
        }

        const std::string lower = Lower(name);
        const size_t      leaf  = name.find_last_of('\\');
        std::string       file  = leaf == std::string::npos ? name : name.substr(leaf + 1);
        const size_t      dot   = file.find_last_of('.');
        source.stem             = Tidy(dot == std::string::npos ? file : file.substr(0, dot), 48);
        source.name             = name;

        wxl_result result = WXL_E_NOT_FOUND;
        if (StartsWith(lower, "mods\\"))
        {
            const std::string rest  = name.substr(5);
            const size_t      slash = rest.find('\\');
            if (slash == std::string::npos || slash == 0 || slash + 1 == rest.size())
                return Refuse(out, WXL_E_INVALID, "make: '%s' names no file: write \"Mods\\<folder>\\<path>\"", name.c_str());
            source.origin = Tidy(rest.substr(0, slash), 48);
            const std::filesystem::path mods = ModsDirectory();
            if (mods.empty()) return Refuse(out, WXL_E_FAILED, "make: the runtime knows no Mods folder to read '%s' from", name.c_str());
            std::filesystem::path file_path = mods;
            size_t                start     = 0;
            while (start <= rest.size())
            {
                const size_t next = rest.find('\\', start);
                const std::string part = rest.substr(start, next == std::string::npos ? std::string::npos : next - start);
                if (!part.empty()) file_path /= std::filesystem::u8path(part);
                if (next == std::string::npos) break;
                start = next + 1;
            }
            result = ReadDisk(file_path, source.bytes);
        }
        else
        {
            if (StartsWith(lower, "interface\\addons\\"))
            {
                const std::string rest  = name.substr(17);
                const size_t      slash = rest.find('\\');
                source.origin           = Tidy(slash == std::string::npos ? rest : rest.substr(0, slash), 48);
            }
            else
                source.origin = "Client";
            {
                // A mod's own read, not the client's: nothing is raised for what is not there.
                wxl::core::services::assets::AskingScope asking;
                result = ReadClient(name, source.bytes);
            }
            // An add-on's file the client's lookup did not find: read off the disk beside the
            // client, where the add-ons are.
            if (result == WXL_E_NOT_FOUND && StartsWith(lower, "interface\\addons\\"))
            {
                const std::filesystem::path mods = ModsDirectory();
                if (!mods.empty())
                {
                    std::filesystem::path file_path = mods.parent_path();
                    size_t                start     = 0;
                    while (start <= name.size())
                    {
                        const size_t      next = name.find('\\', start);
                        const std::string part = name.substr(start, next == std::string::npos ? std::string::npos : next - start);
                        if (!part.empty()) file_path /= std::filesystem::u8path(part);
                        if (next == std::string::npos) break;
                        start = next + 1;
                    }
                    result = ReadDisk(file_path, source.bytes);
                }
            }
        }
        if (result == WXL_E_NOT_FOUND) return Refuse(out, WXL_E_NOT_FOUND, "make: no file '%s'", name.c_str());
        if (result == WXL_E_INVALID)
            return Refuse(out, WXL_E_INVALID, "make: '%s' is larger than %u MB", name.c_str(), static_cast<unsigned>(kSourceMax >> 20));
        if (result != WXL_OK) return Refuse(out, WXL_E_FAILED, "make: '%s' could not be read", name.c_str());
        return WXL_OK;
    }

    /// A PNG's size, from its header.
    bool PngSize(const std::vector<uint8_t>& bytes, uint32_t* width, uint32_t* height)
    {
        if (bytes.size() < 24) return false;
        const auto be = [&](size_t at) {
            return (static_cast<uint32_t>(bytes[at]) << 24) | (static_cast<uint32_t>(bytes[at + 1]) << 16) |
                   (static_cast<uint32_t>(bytes[at + 2]) << 8) | static_cast<uint32_t>(bytes[at + 3]);
        };
        *width  = be(16);
        *height = be(20);
        return *width && *height;
    }

    /// Draws a recipe as a BLP.
    bool Draw(const Recipe& recipe, const std::vector<uint8_t>& bytes, std::vector<uint8_t>& blp, std::string* error)
    {
        img::Picture picture;
        if (recipe.kind == img::Kind::Svg)
        {
            if (!img::RenderSvg(bytes.data(), bytes.size(), recipe.width, recipe.height, recipe.groups.data(),
                                recipe.groups.size(), &picture, error))
                return false;
        }
        else
        {
            img::Picture decoded;
            if (!img::DecodePng(bytes.data(), bytes.size(), &decoded, error)) return false;
            picture = img::Resample(decoded, recipe.width, recipe.height);
        }
        if (recipe.tint) img::Tint(picture, recipe.tint);
        blp = img::EncodeBlp(picture, recipe.textureWidth, recipe.textureHeight);
        return true;
    }

    /// Lets go of the least used textures' bytes past the budget, and forgets the least used names
    /// past their count; @p keep is never let go. Under g_lock.
    void Trim(const std::string& keep)
    {
        while (g_bytes > g_limit)
        {
            Entry*      oldest = nullptr;
            std::string key;
            for (auto& [k, entry] : g_entries)
                if (entry.blp && !entry.pinned && k != keep && (!oldest || entry.used < oldest->used))
                {
                    oldest = &entry;
                    key    = k;
                }
            if (!oldest) break;
            g_bytes -= oldest->blp->size();
            oldest->blp.reset();
            ++g_released;
            WLOG_DEBUG(kTag, "%s let go for the budget; drawn again if the client asks for it", oldest->recipe.path.c_str());
            // A texture that cannot be drawn again (its source too large to keep) goes whole.
            if (!oldest->recipe.bytes) g_entries.erase(key);
        }
        while (g_entries.size() > g_maxNames)
        {
            auto oldest = g_entries.end();
            for (auto it = g_entries.begin(); it != g_entries.end(); ++it)
                if (it->first != keep && !it->second.pinned && (oldest == g_entries.end() || it->second.used < oldest->second.used)) oldest = it;
            if (oldest == g_entries.end()) break;
            if (oldest->second.blp) g_bytes -= oldest->second.blp->size();
            WLOG_DEBUG(kTag, "%s forgotten: more than %u textures", oldest->second.recipe.path.c_str(), g_maxNames);
            g_entries.erase(oldest);
        }
    }

    // --- the provider -------------------------------------------------------------------------

    struct Open
    {
        std::shared_ptr<const std::vector<uint8_t>> blp;
    };

    /// Whether a name is under Interface\WXL\, compared in place: asked of every name the client opens.
    bool Ours(wxl_str path)
    {
        constexpr size_t n = sizeof kPrefix - 1;
        if (!path.data || path.size < n) return false;
        for (size_t i = 0; i < n; ++i)
        {
            const char c = path.data[i] == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(path.data[i])));
            if (c != kPrefix[i]) return false;
        }
        return true;
    }

    wxl_result WXL_CALL Lookup(void*, wxl_str path)
    {
        if (!Ours(path)) return WXL_E_NOT_FOUND;
        const std::string key = Lower(std::string_view(path.data, path.size));
        std::lock_guard<std::mutex> lock(g_lock);
        return g_entries.count(key) ? WXL_OK : WXL_E_NOT_FOUND;
    }

    void* WXL_CALL OpenFile(void*, wxl_str path, uint64_t* out_size)
    {
        const std::string key = Lower(std::string_view(path.data, path.size));
        Recipe            recipe;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            const auto it = g_entries.find(key);
            if (it == g_entries.end()) return nullptr;
            it->second.used = ++g_tick;
            if (it->second.blp)
            {
                *out_size = it->second.blp->size();
                return new Open{it->second.blp};
            }
            recipe = it->second.recipe;
        }
        // Let go for the budget: drawn again from what was kept.
        std::vector<uint8_t> blp;
        std::string          error;
        if (!recipe.bytes || !Draw(recipe, *recipe.bytes, blp, &error))
        {
            WLOG_WARN(kTag, "%s could not be drawn again: %s", recipe.path.c_str(), error.c_str());
            return nullptr;
        }
        auto shared = std::make_shared<const std::vector<uint8_t>>(std::move(blp));
        {
            std::lock_guard<std::mutex> lock(g_lock);
            const auto it = g_entries.find(key);
            if (it != g_entries.end() && !it->second.blp)
            {
                it->second.blp = shared;
                g_bytes += shared->size();
                ++g_made;
                Trim(key);
            }
        }
        WLOG_DEBUG(kTag, "%s drawn again for the client", recipe.path.c_str());
        *out_size = shared->size();
        return new Open{shared};
    }

    wxl_result WXL_CALL ReadAt(void*, void* file, uint64_t offset, void* buffer, uint32_t size, uint32_t* out_read)
    {
        const std::vector<uint8_t>& bytes = *static_cast<Open*>(file)->blp;
        const uint64_t              left  = offset < bytes.size() ? bytes.size() - offset : 0;
        const uint32_t              n     = static_cast<uint32_t>(std::min<uint64_t>(left, size));
        if (n) std::memcpy(buffer, bytes.data() + offset, n);
        *out_read = n;
        return WXL_OK;
    }

    void WXL_CALL CloseFile(void*, void* file) { delete static_cast<Open*>(file); }

    /// Puts the provider among the client's sources, once.
    bool EnsureProvider()
    {
        if (g_provider) return true;
        static const wxl_asset_provider provider = [] {
            wxl_asset_provider p{};
            p.struct_size = sizeof p;
            p.lookup      = &Lookup;
            p.open        = &OpenFile;
            p.read_at     = &ReadAt;
            p.close       = &CloseFile;
            return p;
        }();
        uint64_t handle = 0;
        if (wxl::core::services::assets::AddProvider(&provider, kPriority, &handle, 0) != WXL_OK) return false;
        g_provider = handle;
        return true;
    }

    // --- making a texture ---------------------------------------------------------------------

    uint64_t Mix(uint64_t hash, const void* data, size_t size) { return img::Hash64(data, size, hash); }

    wxl_result Make(const wxl_texture_request* request, wxl_texture* out, uint32_t mod)
    {
        if (!out || out->struct_size < sizeof(wxl_texture)) return WXL_E_INVALID;
        const uint32_t outSize = out->struct_size;
        std::memset(out, 0, sizeof(wxl_texture));
        out->struct_size = outSize;
        if (!request || request->struct_size < sizeof(wxl_texture_request))
            return Refuse(out, WXL_E_INVALID, "make: the request is missing or too short");
        if (request->width > WXL_TEXTURE_MAX_EDGE || request->height > WXL_TEXTURE_MAX_EDGE)
            return Refuse(out, WXL_E_INVALID, "make: %u x %u is larger than the client loads: 2048 pixels at most each way",
                          request->width, request->height);
        if (request->group_count > WXL_TEXTURE_MAX_GROUPS || (request->group_count && !request->groups))
            return Refuse(out, WXL_E_INVALID, "make: %u groups; at most %u", request->group_count, WXL_TEXTURE_MAX_GROUPS);

        std::vector<img::GroupStyle> groups;
        for (uint32_t i = 0; i < request->group_count; ++i)
        {
            const wxl_texture_group& g = request->groups[i];
            if (g.struct_size < sizeof(wxl_texture_group))
                return Refuse(out, WXL_E_INVALID, "make: group %u is too short (struct_size)", i + 1);
            if (!g.id.data || g.id.size == 0 || g.id.size > 63)
                return Refuse(out, WXL_E_INVALID, "make: group %u has no id, or one past 63 bytes", i + 1);
            img::GroupStyle style;
            style.id.assign(g.id.data, g.id.size);
            for (const img::GroupStyle& other : groups)
                if (other.id == style.id)
                    return Refuse(out, WXL_E_INVALID, "make: the group '%s' is given twice", style.id.c_str());
            if (g.set & ~(WXL_TEXTURE_SET_COLOR | WXL_TEXTURE_SET_WIDTH | WXL_TEXTURE_SET_VISIBLE))
                return Refuse(out, WXL_E_INVALID, "make: group '%s' sets bits this runtime does not know (0x%X)",
                              style.id.c_str(), g.set);
            style.setColor   = (g.set & WXL_TEXTURE_SET_COLOR) != 0;
            style.color      = g.color;
            style.setWidth   = (g.set & WXL_TEXTURE_SET_WIDTH) != 0;
            style.width      = g.width;
            style.setVisible = (g.set & WXL_TEXTURE_SET_VISIBLE) != 0;
            style.visible    = g.visible != WXL_FALSE;
            if (style.setWidth && !(g.width >= 0.0f && g.width <= 1e6f))
                return Refuse(out, WXL_E_INVALID, "make: group '%s' width must be a number, 0 or more", style.id.c_str());
            groups.push_back(style);
        }
        // The same groups in any order are the same texture.
        std::sort(groups.begin(), groups.end(), [](const img::GroupStyle& a, const img::GroupStyle& b) { return a.id < b.id; });

        Source           source;
        const wxl_result read = ReadSource(std::string_view(request->source.data ? request->source.data : "", request->source.size),
                                           mod, source, out);
        if (read != WXL_OK) return read;

        Recipe recipe;
        recipe.source  = source.name;
        recipe.kind    = img::Sniff(source.bytes.data(), source.bytes.size());
        recipe.content = img::Hash64(source.bytes.data(), source.bytes.size());
        recipe.groups  = groups;
        recipe.tint    = request->tint;
        if (recipe.kind == img::Kind::Unknown)
            return Refuse(out, WXL_E_UNSUPPORTED, "make: '%s' is neither an SVG nor a PNG", source.name.c_str());
        if (recipe.kind == img::Kind::Png && !groups.empty())
            return Refuse(out, WXL_E_INVALID, "make: '%s' is a PNG, which has no groups to restyle; a tint colours it",
                          source.name.c_str());

        // The size: as asked, the source's own for 0, the other edge in proportion when one is given.
        uint32_t    naturalW = 0, naturalH = 0;
        std::string error;
        if (recipe.kind == img::Kind::Svg ? !img::SvgSize(source.bytes.data(), source.bytes.size(), &naturalW, &naturalH, &error)
                                          : !PngSize(source.bytes, &naturalW, &naturalH))
            return Refuse(out, WXL_E_FAILED, "make: '%s': %s", source.name.c_str(),
                          error.empty() ? "the PNG has no header" : error.c_str());
        uint32_t w = request->width, h = request->height;
        if (!w && !h)
        {
            w = naturalW;
            h = naturalH;
        }
        else if (!w)
            w = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(h) * naturalW / naturalH)));
        else if (!h)
            h = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(w) * naturalH / naturalW)));
        if (w > WXL_TEXTURE_MAX_EDGE || h > WXL_TEXTURE_MAX_EDGE)
            return Refuse(out, WXL_E_INVALID, "make: '%s' is %u x %u; ask for a size up to 2048 pixels each way",
                          source.name.c_str(), w, h);
        recipe.width         = w;
        recipe.height        = h;
        recipe.textureWidth  = img::TextureEdge(w);
        recipe.textureHeight = img::TextureEdge(h);

        // The name: everything the texture is drawn from.
        uint64_t hash = img::Hash64(&kRecipeVersion, sizeof kRecipeVersion);
        const uint32_t kind = static_cast<uint32_t>(recipe.kind);
        hash = Mix(hash, &kind, sizeof kind);
        hash = Mix(hash, &recipe.content, sizeof recipe.content);
        hash = Mix(hash, &w, sizeof w);
        hash = Mix(hash, &h, sizeof h);
        hash = Mix(hash, &recipe.tint, sizeof recipe.tint);
        for (const img::GroupStyle& g : groups)
        {
            hash = Mix(hash, g.id.data(), g.id.size() + 1);
            const uint32_t fields[4] = {static_cast<uint32_t>(g.setColor) | (static_cast<uint32_t>(g.setWidth) << 1) |
                                            (static_cast<uint32_t>(g.setVisible) << 2),
                                        g.setColor ? g.color : 0u, 0u, g.setVisible ? static_cast<uint32_t>(g.visible) : 0u};
            float width = g.setWidth ? g.width : 0.0f;
            hash        = Mix(hash, fields, sizeof fields);
            hash        = Mix(hash, &width, sizeof width);
        }
        char path[WXL_TEXTURE_PATH_MAX];
        std::snprintf(path, sizeof path, "Interface\\WXL\\%s\\%s_%ux%u_%016llx.blp", source.origin.c_str(), source.stem.c_str(),
                      w, h, static_cast<unsigned long long>(hash));
        recipe.path           = path;
        const std::string key = Lower(recipe.path);

        std::snprintf(out->path, sizeof out->path, "%s", path);
        out->width          = w;
        out->height         = h;
        out->texture_width  = recipe.textureWidth;
        out->texture_height = recipe.textureHeight;
        out->right          = static_cast<float>(w) / static_cast<float>(recipe.textureWidth);
        out->bottom         = static_cast<float>(h) / static_cast<float>(recipe.textureHeight);

        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (!EnsureProvider())
                return Refuse(out, WXL_E_FAILED, "make: the runtime could not add its source of textures to the client's files");
            const auto it = g_entries.find(key);
            if (it != g_entries.end() && it->second.blp)
            {
                it->second.used = ++g_tick;
                ++g_reused;
                out->reused = WXL_TRUE;
                return WXL_OK;
            }
        }

        const auto           started = std::chrono::steady_clock::now();
        std::vector<uint8_t> blp;
        if (!Draw(recipe, source.bytes, blp, &error))
            return Refuse(out, WXL_E_FAILED, "make: '%s' could not be drawn: %s", source.name.c_str(), error.c_str());
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (source.bytes.size() <= kSourceKeptMax)
            recipe.bytes = std::make_shared<const std::vector<uint8_t>>(std::move(source.bytes));

        auto shared = std::make_shared<const std::vector<uint8_t>>(std::move(blp));
        {
            std::lock_guard<std::mutex> lock(g_lock);
            Entry& entry = g_entries[key];
            if (entry.blp) g_bytes -= entry.blp->size();
            entry.recipe = recipe;
            entry.blp    = shared;
            entry.used   = ++g_tick;
            g_bytes += shared->size();
            ++g_made;
            Trim(key);
        }
        WLOG_INFO(kTag, "%s made from %s at %u x %u (texture %u x %u, %zu bytes, %zu group(s)) in %.1f ms", path,
                  recipe.source.c_str(), w, h, recipe.textureWidth, recipe.textureHeight, shared->size(), groups.size(), ms);
        return WXL_OK;
    }

    // --- the table ----------------------------------------------------------------------------

    uint32_t CallingMod()
    {
        wxl::host::ModHost* host = wxl::core::Host();
        return host ? host->CurrentMod() : 0;
    }

    wxl_result WXL_CALL MakeEntry(const wxl_texture_request* request, wxl_texture* out)
    {
        return Make(request, out, CallingMod());
    }

    wxl_result WXL_CALL Stats(wxl_texture_stats* out)
    {
        if (!out || out->struct_size < sizeof(uint32_t)) return WXL_E_INVALID;
        wxl_texture_stats stats{};
        stats.struct_size = out->struct_size;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            stats.textures      = static_cast<uint32_t>(g_entries.size());
            for (const auto& [key, entry] : g_entries)
                if (entry.blp) ++stats.resident;
            stats.texture_limit = g_maxNames;
            stats.bytes         = g_bytes;
            stats.byte_limit    = g_limit;
            stats.made          = g_made;
            stats.reused        = g_reused;
            stats.released      = g_released;
        }
        std::memcpy(out, &stats, std::min<size_t>(out->struct_size, sizeof stats));
        return WXL_OK;
    }

    const wxl_texture_api g_api = {
        sizeof(wxl_texture_api),
        &MakeEntry,
        &Stats,
    };

    // --- WxlTexture, in the game's interface --------------------------------------------------

    /// What the interface calls: the native half, which the wrapper below keeps as an upvalue
    /// and takes out of the globals.
    constexpr char kNativeName[] = "__wxl_texture";

    /// The interface's half: flattens the groups table (the native half reads no table), then
    /// calls the native one. Run with no taint, so add-ons call it freely.
    constexpr char kWrapper[] = R"lua(
local native = __wxl_texture
__wxl_texture = nil
local type, pairs, tostring, format, concat = type, pairs, tostring, string.format, table.concat
local function colour(v)
    if type(v) == "number" then return format("@%.0f", v) end
    return v
end
function WxlTexture(source, width, height, groups, tint)
    local flat = ""
    if groups ~= nil then
        if type(groups) ~= "table" then
            return nil, "WxlTexture: groups must be a table {id = colour, false, or {color, width, visible}}"
        end
        local lines = {}
        for id, v in pairs(groups) do
            local c, w, shown = "", "", ""
            local kind = type(v)
            if kind == "string" or kind == "number" then
                c = colour(v)
            elseif kind == "boolean" then
                shown = v and "1" or "0"
            elseif kind == "table" then
                for field, f in pairs(v) do
                    if field == "color" then
                        c = colour(f)
                    elseif field == "width" then
                        if type(f) ~= "number" then return nil, "WxlTexture: group '" .. tostring(id) .. "' width must be a number" end
                        w = format("%.9g", f)
                    elseif field == "visible" then
                        if type(f) ~= "boolean" then return nil, "WxlTexture: group '" .. tostring(id) .. "' visible must be true or false" end
                        shown = f and "1" or "0"
                    else
                        return nil, "WxlTexture: group '" .. tostring(id) .. "' has a field '" .. tostring(field) .. "'; it takes color, width and visible"
                    end
                end
            else
                return nil, "WxlTexture: group '" .. tostring(id) .. "' takes a colour, false, or a table {color, width, visible}"
            end
            if type(id) ~= "string" then return nil, "WxlTexture: groups are named by the SVG's ids, strings" end
            lines[#lines + 1] = id .. "\t" .. tostring(c) .. "\t" .. w .. "\t" .. shown
        end
        flat = concat(lines, "\n")
    end
    if tint ~= nil then tint = colour(tint) end
    return native(source, width, height, flat, tint)
end
)lua";

    int HexDigit(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    /// "rrggbb", "rrggbbaa" (an optional '#'), or "@<decimal 0xAARRGGBB>": 0xAARRGGBB.
    bool ParseColor(std::string text, uint32_t* argb)
    {
        if (!text.empty() && text[0] == '@')
        {
            if (text.size() < 2 || !std::isdigit(static_cast<unsigned char>(text[1]))) return false;
            char*                    end   = nullptr;
            const unsigned long long value = std::strtoull(text.c_str() + 1, &end, 10);
            if (!end || *end || value > 0xFFFFFFFFull) return false;
            *argb = static_cast<uint32_t>(value);
            return true;
        }
        if (!text.empty() && text[0] == '#') text.erase(0, 1);
        if (text.size() != 6 && text.size() != 8) return false;
        uint32_t rgba = 0;
        for (char c : text)
        {
            const int d = HexDigit(c);
            if (d < 0) return false;
            rgba = (rgba << 4) | static_cast<uint32_t>(d);
        }
        *argb = text.size() == 6 ? (0xFF000000u | rgba) : ((rgba & 0xFFu) << 24) | (rgba >> 8);
        return true;
    }

    /// One field of a flattened group line.
    std::vector<std::string> Split(const std::string& text, char by)
    {
        std::vector<std::string> parts;
        size_t                   start = 0;
        while (true)
        {
            const size_t next = text.find(by, start);
            parts.push_back(text.substr(start, next == std::string::npos ? std::string::npos : next - start));
            if (next == std::string::npos) break;
            start = next + 1;
        }
        return parts;
    }

    bool Edge(double value, uint32_t* edge)
    {
        if (!(value >= 0.0 && value <= 4294967295.0) || value != std::floor(value)) return false;
        *edge = static_cast<uint32_t>(value);
        return true;
    }

    void WXL_CALL Native(wxl_script_call* call, void*)
    {
        const wxl_script_api& script = wxl::core::services::ScriptApi();
        const auto string = [&](uint32_t index, std::string& value) {
            wxl_str text{};
            if (!script.arg_string(call, index, &text)) return false;
            value.assign(text.data ? text.data : "", text.size);
            return true;
        };
        const auto fail = [&](const std::string& reason) {
            script.push_nil(call);
            script.push_string(call, wxl_str{reason.data(), static_cast<uint32_t>(reason.size())});
        };

        std::string source, groups, tint;
        if (script.arg_type(call, 0) != WXL_VALUE_STRING || !string(0, source))
            return fail("WxlTexture: 'source' (argument 1) must be a file's name");
        double width = 0.0, height = 0.0;
        const uint32_t wt = script.arg_type(call, 1), ht = script.arg_type(call, 2);
        if ((wt != WXL_VALUE_NIL && (wt != WXL_VALUE_NUMBER || !script.arg_number(call, 1, &width))) ||
            (ht != WXL_VALUE_NIL && (ht != WXL_VALUE_NUMBER || !script.arg_number(call, 2, &height))))
            return fail("WxlTexture: 'width' and 'height' (arguments 2 and 3) must be numbers of pixels, or nil for the "
                        "source's own");
        string(3, groups);
        if (script.arg_type(call, 4) == WXL_VALUE_STRING) string(4, tint);

        wxl_texture made{};
        made.struct_size        = sizeof made;
        const wxl_result result = wxl::core::services::texture::MakeForInterface(source, width, height, groups, tint, &made);
        if (result != WXL_OK) return fail(std::string("WxlTexture: ") + (made.reason[0] ? made.reason : "failed"));
        script.push_string(call, wxl_str{made.path, static_cast<uint32_t>(std::strlen(made.path))});
        script.push_number(call, made.right);
        script.push_number(call, made.bottom);
        script.push_number(call, made.texture_width);
        script.push_number(call, made.texture_height);
    }
}

namespace wxl::core::services
{
    const wxl_texture_api& TextureApi() { return g_api; }

    namespace texture
    {
        std::string ServeData(const char* stem, const uint8_t* rgba, uint32_t width, uint32_t height)
        {
            if (!rgba || !width || !height || (width & (width - 1)) || (height & (height - 1))) return {};
            const uint64_t hash = img::Hash64(rgba, static_cast<size_t>(width) * height * 4u, 0x5DA7A0ull);
            char           path[WXL_TEXTURE_PATH_MAX];
            std::snprintf(path, sizeof path, "Interface\\WXL\\Data\\%s_%ux%u_%016llx.blp", stem ? stem : "data", width, height,
                          static_cast<unsigned long long>(hash));
            const std::string key = Lower(path);
            auto blp = std::make_shared<const std::vector<uint8_t>>(img::EncodeBlpData(rgba, width, height));
            std::lock_guard<std::mutex> lock(g_lock);
            if (!EnsureProvider()) return {};
            Entry& entry = g_entries[key];
            if (!entry.blp)
            {
                entry.recipe.path   = path;
                entry.recipe.width  = entry.recipe.textureWidth = width;
                entry.recipe.height = entry.recipe.textureHeight = height;
                entry.blp           = blp;
                entry.pinned        = true;
                g_bytes += blp->size();
                ++g_made;
            }
            entry.used = ++g_tick;
            return path;
        }

        void Unserve(const std::string& path)
        {
            std::lock_guard<std::mutex> lock(g_lock);
            const auto it = g_entries.find(Lower(path));
            if (it == g_entries.end() || !it->second.pinned) return;
            if (it->second.blp) g_bytes -= it->second.blp->size();
            g_entries.erase(it);
        }
    }

    namespace texture
    {
        void OnScriptReady()
        {
            const wxl_script_api& script = ScriptApi();
            if (script.register_function(kNativeName, &Native, nullptr) != WXL_OK)
            {
                WLOG_WARN(kTag, "WxlTexture could not be put in the interface: %s is taken", kNativeName);
                return;
            }
            script.run(wxl_str{kWrapper, static_cast<uint32_t>(sizeof kWrapper - 1)}, "WarcraftXL\\WxlTexture");
        }

        wxl_result MakeForInterface(const std::string& source, double width, double height, const std::string& groups,
                                    const std::string& tint, wxl_texture* out)
        {
            uint32_t w = 0, h = 0;
            if (!Edge(width, &w) || !Edge(height, &h))
                return Refuse(out, WXL_E_INVALID, "the size must be whole numbers of pixels");

            std::vector<std::string>       ids;
            std::vector<wxl_texture_group> list;
            if (!groups.empty())
            {
                const std::vector<std::string> lines = Split(groups, '\n');
                ids.reserve(lines.size());
                for (const std::string& line : lines)
                {
                    const std::vector<std::string> f = Split(line, '\t');
                    if (f.size() != 4) return Refuse(out, WXL_E_INVALID, "a group could not be read");
                    ids.push_back(f[0]);
                    wxl_texture_group g{};
                    g.struct_size = sizeof g;
                    g.id          = wxl_str{ids.back().data(), static_cast<uint32_t>(ids.back().size())};
                    if (!f[1].empty())
                    {
                        if (!ParseColor(f[1], &g.color))
                            return Refuse(out, WXL_E_INVALID,
                                          "group '%s': \"%s\" is not a colour; write \"rrggbb\" or \"rrggbbaa\" in hex",
                                          f[0].c_str(), f[1].c_str());
                        g.set |= WXL_TEXTURE_SET_COLOR;
                    }
                    if (!f[2].empty())
                    {
                        char*        end   = nullptr;
                        const double value = std::strtod(f[2].c_str(), &end);
                        if (!end || *end || !(value >= 0.0))
                            return Refuse(out, WXL_E_INVALID, "group '%s': the width must be a number, 0 or more", f[0].c_str());
                        g.set |= WXL_TEXTURE_SET_WIDTH;
                        g.width = static_cast<float>(value);
                    }
                    if (!f[3].empty())
                    {
                        g.set |= WXL_TEXTURE_SET_VISIBLE;
                        g.visible = f[3] == "1" ? WXL_TRUE : WXL_FALSE;
                    }
                    list.push_back(g);
                }
                // ids may have moved while they grew: point at them once they are all in.
                for (size_t i = 0; i < list.size(); ++i)
                    list[i].id = wxl_str{ids[i].data(), static_cast<uint32_t>(ids[i].size())};
            }
            uint32_t tintArgb = 0;
            if (!tint.empty() && !ParseColor(tint, &tintArgb))
                return Refuse(out, WXL_E_INVALID, "the tint \"%s\" is not a colour; write \"rrggbb\" or \"rrggbbaa\" in hex",
                              tint.c_str());

            wxl_texture_request request{};
            request.struct_size = sizeof request;
            request.source      = wxl_str{source.data(), static_cast<uint32_t>(source.size())};
            request.width       = w;
            request.height      = h;
            request.groups      = list.empty() ? nullptr : list.data();
            request.group_count = static_cast<uint32_t>(list.size());
            request.tint        = tintArgb;
            return Make(&request, out, 0);
        }

        void UseModsDirectory(const std::filesystem::path& mods) { g_modsOverride = mods; }

        void SetLimits(uint64_t bytes, uint32_t textures)
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_limit    = bytes;
            g_maxNames = textures;
            Trim(std::string());
        }

        void Clear()
        {
            uint64_t provider = 0;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                g_entries.clear();
                g_bytes    = 0;
                g_limit    = kDefaultBytes;
                g_maxNames = kDefaultTextures;
                g_made = g_reused = g_released = 0;
                provider   = g_provider;
                g_provider = 0;
            }
            if (provider) assets::RemoveProvider(provider, 0);
        }
    }
}
