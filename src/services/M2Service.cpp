// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The wxl.m2 service: character appearances and the models mods hang on them.
//
// Every model a mod makes is kept here with the mod that made it, so a call can refuse a model
// it was never given, and a mod that is disabled leaves nothing hanging on a character. The set
// of models that follow their parent's bones is read on the client's animate threads; it only
// ever changes from mod calls, on the main thread, which never run while the scene animates.

#include "services/M2Service.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/guard.hpp"
#include "wxl/host/mod_host.hpp"

#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr char kTag[] = "m2";

    namespace binding = wxl::core::binding;

    struct Watcher
    {
        wxl_m2_equipment_fn fn;
        void*               user;
        uint32_t            owner;
        uint32_t            token;
        bool                live;
    };

    struct Model
    {
        uint32_t owner;
        bool     follow;
        void*    file; // its CM2Shared: what its interface copies are known by
        wxl::core::services::m2::Pose pose{};
    };

    std::vector<Watcher>              g_watchers;
    uint32_t                          g_live      = 0;
    uint32_t                          g_nextToken = 1;
    int                               g_depth     = 0; // dispatches in progress: no compaction under them
    std::unordered_map<void*, Model>  g_models;
    uint32_t                          g_following = 0;
    struct FilePose
    {
        uint32_t                      count; // how many posed models are made of the file
        wxl::core::services::m2::Pose pose;  // the last one's: copies take it
    };
    std::unordered_map<void*, FilePose> g_followFiles;

    struct WatcherCall
    {
        const Watcher*          watcher;
        const wxl_m2_equipment* change;
    };

    void InvokeWatcher(void* context)
    {
        const WatcherCall* call = static_cast<const WatcherCall*>(context);
        try
        {
            call->watcher->fn(call->change, call->watcher->user);
        }
        catch (const std::exception& e)
        {
            WLOG_ERROR(kTag, "an equipment watcher threw: %s", e.what());
        }
        catch (...)
        {
            WLOG_ERROR(kTag, "an equipment watcher threw");
        }
    }

    void Compact()
    {
        if (g_depth != 0) return;
        std::erase_if(g_watchers, [](const Watcher& watcher) { return !watcher.live; });
    }

    void Drop(Watcher& watcher)
    {
        if (!watcher.live) return;
        watcher.live = false;
        --g_live;
    }

    uint32_t CurrentMod()
    {
        wxl::host::ModHost* host = wxl::core::Host();
        return host ? host->CurrentMod() : 0;
    }

    Model* Known(void* model)
    {
        if (!model) return nullptr;
        auto it = g_models.find(model);
        return it == g_models.end() ? nullptr : &it->second;
    }

    /// Sets how a model is posed each frame; a pose of kind None leaves it as the client poses it.
    void SetPose(Model& model, const wxl::core::services::m2::Pose& pose)
    {
        using wxl::core::services::m2::Pose;
        const bool follow = pose.kind != Pose::None;
        if (model.follow)
        {
            --g_following;
            if (model.file && --g_followFiles[model.file].count == 0) g_followFiles.erase(model.file);
        }
        model.follow = follow;
        model.pose   = pose;
        if (!follow) return;
        ++g_following;
        if (!model.file) return;
        FilePose& file = g_followFiles[model.file];
        ++file.count;
        file.pose = pose;
    }

    void SetFollow(Model& model, bool follow)
    {
        wxl::core::services::m2::Pose pose{};
        pose.kind = follow ? wxl::core::services::m2::Pose::Bones : wxl::core::services::m2::Pose::None;
        SetPose(model, pose);
    }

    /// Detaches and lets go of one model this service made, and forgets it.
    void Forget(std::unordered_map<void*, Model>::iterator it)
    {
        void* model = it->first;
        SetFollow(it->second, false);
        g_models.erase(it);
        binding::m2_detach_model(model);
        binding::m2_release_model(model);
    }

    std::string Text(wxl_str text) { return text.data ? std::string(text.data, text.size) : std::string(); }

    // --- The table ----------------------------------------------------------------------------

    wxl_result WXL_CALL WatchEquipment(wxl_m2_equipment_fn fn, void* user, uint32_t* out_token)
    {
        if (!fn) return WXL_E_INVALID;
        const uint32_t token = g_nextToken++;
        g_watchers.push_back(Watcher{fn, user, CurrentMod(), token, true});
        ++g_live;
        if (out_token) *out_token = token;
        return WXL_OK;
    }

    void WXL_CALL UnwatchEquipment(uint32_t token)
    {
        for (Watcher& watcher : g_watchers)
            if (watcher.token == token) Drop(watcher);
        Compact();
    }

    wxl_result WXL_CALL ItemDisplay(uint32_t display_id, wxl_m2_item_display* out)
    {
        if (!out || display_id == 0) return WXL_E_INVALID;
        wxl_m2_item_display row{};
        row.struct_size = sizeof row;
        if (!binding::m2_item_display(display_id, &row)) return WXL_E_NOT_FOUND;
        *out = row;
        return WXL_OK;
    }

    wxl_result WXL_CALL RacePrefix(uint32_t race_id, wxl_str* out)
    {
        if (!out) return WXL_E_INVALID;
        const char* prefix = binding::m2_race_prefix(race_id);
        if (!prefix) return WXL_E_NOT_FOUND;
        out->data = prefix;
        out->size = static_cast<uint32_t>(std::char_traits<char>::length(prefix));
        return WXL_OK;
    }

    void* WXL_CALL CreateModel(void* scene_model, wxl_str path)
    {
        const std::string file = Text(path);
        if (!scene_model || file.empty()) return nullptr;
        void* model = binding::m2_create_model(scene_model, file.c_str());
        if (model) g_models[model] = Model{CurrentMod(), false, binding::m2_model_file(model)};
        return model;
    }

    void WXL_CALL ReleaseModel(void* model)
    {
        auto it = g_models.find(model);
        if (it != g_models.end()) Forget(it);
    }

    wxl_result WXL_CALL AttachModel(void* model, void* parent, uint32_t attachment, uint32_t flags)
    {
        Model* known = Known(model);
        if (!known || !parent || parent == model) return WXL_E_INVALID;
        const wxl_bool to_root = (flags & WXL_M2_ATTACH_ROOT) ? WXL_TRUE : WXL_FALSE;
        if (!binding::m2_attach_model(model, parent, attachment, to_root)) return WXL_E_NOT_FOUND;
        SetFollow(*known, (flags & WXL_M2_ATTACH_FOLLOW_BONES) != 0);
        return WXL_OK;
    }

    void WXL_CALL DetachModel(void* model)
    {
        Model* known = Known(model);
        if (!known) return;
        SetFollow(*known, false);
        binding::m2_detach_model(model);
    }

    void* WXL_CALL ModelParent(void* model)
    {
        return Known(model) ? binding::m2_model_parent(model) : nullptr;
    }

    void WXL_CALL SetGeosets(void* model, uint32_t first, uint32_t last, wxl_bool visible)
    {
        if (Known(model) && first <= last) binding::m2_set_geosets(model, first, last, visible);
    }

    wxl_result WXL_CALL ReplaceTexture(void* model, uint32_t type, wxl_str path)
    {
        const std::string file = Text(path);
        if (!Known(model) || file.empty()) return WXL_E_INVALID;
        return binding::m2_replace_texture(model, type, file.c_str()) ? WXL_OK : WXL_E_NOT_FOUND;
    }

    wxl_result WXL_CALL AttachmentPoint(void* model, uint32_t attachment, wxl_vec3* out)
    {
        if (!model || !out) return WXL_E_INVALID;
        return binding::m2_attachment_point(model, attachment, out) ? WXL_OK : WXL_E_NOT_FOUND;
    }

    wxl_result WXL_CALL AttachToBone(void* model, void* parent, uint32_t attachment, uint32_t key_bone,
                                     const wxl_vec3* offset)
    {
        Model* known = Known(model);
        if (!known || !parent || parent == model || !offset) return WXL_E_INVALID;
        if (!binding::m2_has_key_bone(parent, key_bone)) return WXL_E_NOT_FOUND;
        if (!binding::m2_attach_model(model, parent, attachment, WXL_TRUE)) return WXL_E_NOT_FOUND;
        wxl::core::services::m2::Pose pose{};
        pose.kind    = wxl::core::services::m2::Pose::OnBone;
        pose.keyBone = key_bone;
        pose.offset  = *offset;
        SetPose(*known, pose);
        return WXL_OK;
    }

    const wxl_m2_api kApi = {
        sizeof(wxl_m2_api), &WatchEquipment, &UnwatchEquipment, &ItemDisplay,     &RacePrefix,
        &CreateModel,       &ReleaseModel,   &AttachModel,      &DetachModel,     &ModelParent,
        &SetGeosets,        &ReplaceTexture, &AttachmentPoint,  &AttachToBone,
    };
}

namespace wxl::core::services
{
    const wxl_m2_api& M2Api() { return kApi; }

    namespace m2
    {
        bool Watching() { return g_live != 0; }

        void DispatchEquipment(const wxl_m2_equipment& change)
        {
            wxl::host::ModHost* host = wxl::core::Host();
            ++g_depth;
            // By index, and up to the size at the start: a watcher may watch again.
            const size_t count = g_watchers.size();
            for (size_t i = 0; i < count; ++i)
            {
                if (!g_watchers[i].live) continue;
                const Watcher    watcher  = g_watchers[i];
                WatcherCall      call{&watcher, &change};
                const uint32_t   previous = host ? host->SwapCurrentMod(watcher.owner) : 0;
                wxl::host::Fault fault;
                const bool       returned = wxl::host::CallGuarded(&InvokeWatcher, &call, &fault);
                if (host) host->SwapCurrentMod(previous);
                if (returned) continue;

                char what[96];
                wxl::host::DescribeFault(fault, what, sizeof what);
                WLOG_ERROR(kTag, "an equipment watcher faulted: %s", what);
                if (host && watcher.owner != 0)
                    host->Disable(watcher.owner, "in its equipment watcher");
                else
                    Drop(g_watchers[i]);
            }
            --g_depth;
            Compact();
        }

        bool Following() { return g_following != 0; }

        bool HasModels() { return !g_models.empty(); }

        bool Owns(void* model) { return model && g_models.count(model) != 0; }

        bool PoseOf(void* model, Pose& out)
        {
            auto it = g_models.find(model);
            if (it == g_models.end())
            {
                // An interface copy of a character duplicates its attachments: the copy is posed
                // as the model it was made from, which stays alive as long as the copy does.
                void* source = binding::m2_model_source(model);
                if (source) it = g_models.find(source);
            }
            if (it != g_models.end())
            {
                out = it->second.pose;
                return it->second.follow;
            }
            // Once loaded, a copy forgets the model it was copied from; the file is what remains.
            void* file = binding::m2_model_file(model);
            auto  by   = file ? g_followFiles.find(file) : g_followFiles.end();
            if (by == g_followFiles.end()) return false;
            out = by->second.pose;
            return true;
        }

        bool Follows(void* model)
        {
            Pose pose{};
            return PoseOf(model, pose);
        }

        void* SourceOf(void* model)
        {
            if (!model) return nullptr;
            if (g_models.count(model)) return model;
            void* source = binding::m2_model_source(model);
            if (source && g_models.count(source)) return source;
            void* file = binding::m2_model_file(model);
            if (!file) return nullptr;
            for (const auto& [owned, record] : g_models)
                if (record.file == file) return owned;
            return nullptr;
        }

        void ForgetMod(uint32_t mod)
        {
            for (Watcher& watcher : g_watchers)
                if (watcher.owner == mod) Drop(watcher);
            Compact();
            for (auto it = g_models.begin(); it != g_models.end();)
            {
                auto next = std::next(it);
                if (it->second.owner == mod) Forget(it);
                it = next;
            }
        }

        void Clear()
        {
            for (Watcher& watcher : g_watchers) Drop(watcher);
            Compact();
            // On the way out the client is going too: forget the models without touching them.
            g_models.clear();
            g_followFiles.clear();
            g_following = 0;
        }
    }
}
