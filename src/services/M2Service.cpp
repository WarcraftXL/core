// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The wxl.m2 service: the models objects draw with, and models mods make and hang on them.
//
// Every model a mod makes is kept here with the mod that made it, so a call can refuse a model it
// was never given, and a mod that is disabled leaves nothing hanging on a character. A model hung
// to follow its parent's bones is posed here after the client posed it: its bones take the
// parent's bones of the same key bone id or name (WXL_M2_ATTACH_FOLLOW_BONES), or all of them one
// key bone of the parent moved by an offset (attach_to_bone). The same code poses on both
// clients, from the skeletons the binding describes.
//
// A mod may also place models of its own in the world (spell effects: create_world_model,
// place_model) and watch the bones the client poses (live physics: watch_bones), on the x64
// client, whose binding answers those rows.

#include "services/M2Service.hpp"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/guard.hpp"

#include "wxl/host/mod_host.hpp"

#include <ankerl/unordered_dense.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <shared_mutex>
#include <string_view>
#include <string>
#include <vector>

namespace
{
    constexpr char kTag[] = "m2";

    namespace binding = wxl::core::binding;

    /// How a model wxl.m2 hung is posed each frame, after the client posed it itself.
    struct Pose
    {
        enum Kind : uint32_t
        {
            None,   ///< as the client poses it
            Bones,  ///< each bone as the parent's bone it matches (ATTACH_FOLLOW_BONES)
            OnBone, ///< every bone as one key bone of the parent, moved by an offset (attach_to_bone)
        };
        Kind     kind    = None;
        uint32_t keyBone = 0;
        wxl_vec3 offset{};
    };

    struct Model
    {
        uint32_t owner;
        void*    file; ///< its CM2Shared: what its interface copies are known by once they loaded
        Pose     pose;
        bool     world = false; ///< placed in the world: it has a map entity to take down
    };

    struct FilePose
    {
        uint32_t count; ///< how many posed models are made of the file
        Pose     pose;  ///< the last one's: copies take it
    };

    ankerl::unordered_dense::map<void*, Model>    g_models;
    ankerl::unordered_dense::map<void*, FilePose> g_followFiles;
    std::atomic<uint32_t>                         g_following{0};

    uint32_t CurrentMod()
    {
        wxl::host::ModHost* host = wxl::core::Host();
        return host ? host->CurrentMod() : 0;
    }

    /// One kind of refusal, said once.
    struct Said
    {
        std::atomic<bool> said{false};
    };

    /// Whether the call runs where the client's models live; says once why not.
    bool OnMainThread(const char* call, Said& said)
    {
        const wxl::host::ModHost* host = wxl::core::Host();
        if (!host || host->OnMainThread()) return true;
        if (!said.said.exchange(true))
            WLOG_WARN(kTag, "wxl.m2.%s was called off the main thread, where the client's models are not "
                            "the mod's to touch; it was refused",
                      call);
        return false;
    }

    Model* Known(void* model)
    {
        if (!model) return nullptr;
        auto it = g_models.find(model);
        return it == g_models.end() ? nullptr : &it->second;
    }

    /// Sets how a model is posed each frame; a pose of kind None leaves it as the client poses it.
    void SetPose(Model& model, const Pose& pose)
    {
        if (model.pose.kind != Pose::None)
        {
            g_following.fetch_sub(1, std::memory_order_relaxed);
            if (model.file)
            {
                auto it = g_followFiles.find(model.file);
                if (it != g_followFiles.end() && --it->second.count == 0) g_followFiles.erase(it);
            }
        }
        model.pose = pose;
        if (pose.kind == Pose::None) return;
        g_following.fetch_add(1, std::memory_order_relaxed);
        if (!model.file) return;
        FilePose& file = g_followFiles[model.file];
        ++file.count;
        file.pose = pose;
    }

    /// Detaches and lets go of one model this service made, and forgets it.
    void Forget(void* model)
    {
        auto it = g_models.find(model);
        if (it == g_models.end()) return;
        SetPose(it->second, Pose{});
        const bool world = it->second.world;
        g_models.erase(it);
        if (world) binding::m2_unplace_world(model);
        binding::m2_detach_model(model);
        binding::m2_release_model(model);
    }

    std::string Text(wxl_str text) { return text.data ? std::string(text.data, text.size) : std::string(); }

    // --- Bone watchers: called on the animate threads ----------------------------------------
    //
    // A fixed table, so the animate threads never see it move: a slot is filled on the main thread
    // and only ever switched off after. Which watchers want a file is decided once per file and
    // kept here, under a lock both animate threads take; a change to the watchers starts it over.

    constexpr uint32_t kBoneSlots = 16;

    struct BoneWatcher
    {
        wxl_m2_bones_filter_fn filter = nullptr;
        wxl_m2_bones_fn        fn     = nullptr;
        void*                  user   = nullptr;
        uint32_t               owner  = 0;
        uint32_t               token  = 0;
        std::atomic<bool>      live{false};
    };

    BoneWatcher           g_bones[kBoneSlots];
    std::atomic<uint32_t> g_boneLive{0};
    std::atomic<uint32_t> g_boneEpoch{1};

    uint32_t g_nextBoneToken = 1;

    struct FileWant
    {
        std::string path;
        uint32_t    epoch = 0;
        uint32_t    mask  = 0; // bit per slot of g_bones
    };
    std::shared_mutex                   g_wantLock;
    std::unordered_map<void*, FileWant> g_wants;

    void DropBones(BoneWatcher& watcher)
    {
        if (!watcher.live.exchange(false)) return;
        --g_boneLive;
        ++g_boneEpoch;
    }

    struct BoneCall
    {
        const BoneWatcher*  watcher;
        const wxl_m2_bones* bones;
        wxl_bool            wanted;
    };

    void InvokeBoneFilter(void* context)
    {
        BoneCall* call = static_cast<BoneCall*>(context);
        call->wanted = call->watcher->filter
                           ? call->watcher->filter(call->bones->file, call->bones->path, call->bones->bone_count,
                                                   call->watcher->user)
                           : WXL_TRUE;
    }

    void InvokeBoneWatcher(void* context)
    {
        const BoneCall* call = static_cast<const BoneCall*>(context);
        call->watcher->fn(call->bones, call->watcher->user);
    }

    /// Calls one bone watcher under the guard; a fault switches it off.
    bool GuardedBones(BoneWatcher& watcher, void (*fn)(void*), BoneCall& call)
    {
        wxl::host::Fault fault;
        if (wxl::host::CallGuarded(fn, &call, &fault)) return true;
        char what[96];
        wxl::host::DescribeFault(fault, what, sizeof what);
        WLOG_ERROR(kTag, "a bone watcher faulted and was dropped: %s", what);
        DropBones(watcher);
        return false;
    }

    /// The watchers that want `bones`' file: asked once per file, kept until the watchers change.
    uint32_t WantsOf(const wxl_m2_bones& bones)
    {
        const uint32_t         epoch = g_boneEpoch.load();
        const std::string_view path(bones.path.data ? bones.path.data : "", bones.path.size);
        {
            std::shared_lock lock(g_wantLock);
            auto             it = g_wants.find(bones.file);
            if (it != g_wants.end() && it->second.epoch == epoch && it->second.path == path) return it->second.mask;
        }
        uint32_t mask = 0;
        for (uint32_t i = 0; i < kBoneSlots; ++i)
        {
            BoneWatcher& watcher = g_bones[i];
            if (!watcher.live.load()) continue;
            BoneCall call{&watcher, &bones, WXL_FALSE};
            if (GuardedBones(watcher, &InvokeBoneFilter, call) && call.wanted) mask |= 1u << i;
        }
        std::unique_lock lock(g_wantLock);
        FileWant&        want = g_wants[bones.file];
        want.path.assign(path);
        want.epoch = epoch;
        want.mask  = mask;
        return mask;
    }


    // --- Posing -------------------------------------------------------------------------------

    constexpr uint32_t kBoneKeyId  = 0x00; // int32_t: the key bone id, -1 for none
    constexpr uint32_t kBoneParent = 0x08; // int16_t: the parent bone's index, -1 for the root
    constexpr uint32_t kBoneName   = 0x0C; // uint32_t: the hash of the bone's name, 0 for none
    constexpr uint32_t kBonePivot  = 0x4C; // C3Vector: where it sits in the bind pose
    constexpr uint32_t kBoneSize   = 0x58;
    constexpr uint16_t kNone       = 0xFFFF;
    constexpr size_t   kMatrix     = 16; // floats in a C44Matrix

    template <class T>
    T BoneField(const wxl_core_m2_skeleton& skeleton, uint32_t bone, uint32_t offset)
    {
        T value;
        std::memcpy(&value, skeleton.bones + static_cast<size_t>(bone) * skeleton.bone_stride + offset, sizeof value);
        return value;
    }

    uint16_t KeyBone(const wxl_core_m2_skeleton& skeleton, uint32_t key)
    {
        if (key >= skeleton.key_bone_count) return kNone;
        const uint16_t index = skeleton.key_bones[key];
        return index < skeleton.bone_count ? index : kNone;
    }

    /// Which parent bone each child bone takes its pose from, for one pair of model files: by key
    /// bone id first, then by name; a bone matching neither takes the parent bone nearest its pivot
    /// among the descendants of its nearest matched ancestor's counterpart (a later expansion's
    /// piece names thigh, knee and shin bones by key bones a 3.3.5a body lacks, which the body has
    /// under other ids at the same place). Each animate thread keeps the last few pairs it built;
    /// a pair is rebuilt when either file's bone count no longer agrees, which is all a recycled
    /// file can be told by.
    struct BoneMap
    {
        const void*           child       = nullptr;
        const void*           parent      = nullptr;
        uint32_t              childCount  = 0;
        uint32_t              parentCount = 0;
        bool                  matched     = false;
        std::vector<uint16_t> parentOf;
    };

    const BoneMap& MapBones(const wxl_core_m2_skeleton& child, const wxl_core_m2_skeleton& parent)
    {
        thread_local BoneMap  s_maps[8];
        thread_local uint32_t s_next = 0;

        for (const BoneMap& map : s_maps)
            if (map.child == child.file && map.parent == parent.file && map.childCount == child.bone_count &&
                map.parentCount == parent.bone_count)
                return map;

        BoneMap& map    = s_maps[s_next++ % 8];
        map.child       = child.file;
        map.parent      = parent.file;
        map.childCount  = child.bone_count;
        map.parentCount = parent.bone_count;
        map.matched     = false;
        map.parentOf.assign(child.bone_count, kNone);

        for (uint32_t i = 0; i < child.bone_count; ++i)
        {
            uint16_t      found = kNone;
            const int32_t key   = BoneField<int32_t>(child, i, kBoneKeyId);
            if (key >= 0) found = KeyBone(parent, static_cast<uint32_t>(key));
            const uint32_t name = BoneField<uint32_t>(child, i, kBoneName);
            if (found == kNone && name)
                for (uint32_t j = 0; j < parent.bone_count; ++j)
                    if (BoneField<uint32_t>(parent, j, kBoneName) == name)
                    {
                        found = static_cast<uint16_t>(j);
                        break;
                    }

            if (found != kNone)
            {
                map.matched = true;
            }
            else
            {
                const int16_t  up       = BoneField<int16_t>(child, i, kBoneParent);
                const uint16_t ancestor = up >= 0 && static_cast<uint32_t>(up) < i ? map.parentOf[up] : kNone;
                wxl_vec3       a        = BoneField<wxl_vec3>(child, i, kBonePivot);
                float          best     = 3.4e38f;
                for (uint32_t j = 0; j < parent.bone_count; ++j)
                {
                    if (ancestor != kNone)
                    {
                        uint32_t walk = j;
                        for (uint32_t depth = 0; walk != ancestor && walk < parent.bone_count && depth < parent.bone_count;
                             ++depth)
                        {
                            const int16_t next = BoneField<int16_t>(parent, walk, kBoneParent);
                            walk               = next < 0 ? kNone : static_cast<uint32_t>(next);
                        }
                        if (walk != ancestor) continue;
                    }
                    const wxl_vec3 b = BoneField<wxl_vec3>(parent, j, kBonePivot);
                    const float    d = (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
                    if (d < best)
                    {
                        best  = d;
                        found = static_cast<uint16_t>(j);
                    }
                }
                if (found == kNone) found = ancestor;
            }
            map.parentOf[i] = found;
        }
        return map;
    }

    bool Skeletons(void* model, wxl_core_m2_skeleton& child, wxl_core_m2_skeleton& parent)
    {
        void* to = binding::m2_model_parent(model);
        return to && binding::m2_skeleton(model, &child) && binding::m2_skeleton(to, &parent) && child.matrices &&
               parent.matrices && child.bone_stride >= kBoneSize && parent.bone_stride >= kBoneSize;
    }

    /// Both are in view space: the parent's posed body is the child's, bone for bone.
    void PoseFromParent(void* model)
    {
        wxl_core_m2_skeleton child{}, parent{};
        if (!Skeletons(model, child, parent)) return;
        const BoneMap& map = MapBones(child, parent);
        if (!map.matched) return;
        for (uint32_t i = 0; i < map.childCount; ++i)
            if (map.parentOf[i] != kNone)
                std::memcpy(child.matrices + i * kMatrix, parent.matrices + map.parentOf[i] * kMatrix,
                            kMatrix * sizeof(float));
    }

    /// What CM2Model::AnimateAttachmentsMT hands an attachment: its bone, moved to the point
    /// (C44Matrix::Translate: the translation row gains the offset through the matrix).
    void PoseOnBone(void* model, uint32_t keyBone, const wxl_vec3& offset)
    {
        wxl_core_m2_skeleton child{}, parent{};
        if (!Skeletons(model, child, parent)) return;
        const uint16_t bone = KeyBone(parent, keyBone);
        if (bone == kNone) return;
        float m[kMatrix];
        std::memcpy(m, parent.matrices + bone * kMatrix, sizeof m);
        m[12] += m[0] * offset.x + m[4] * offset.y + m[8] * offset.z;
        m[13] += m[1] * offset.x + m[5] * offset.y + m[9] * offset.z;
        m[14] += m[2] * offset.x + m[6] * offset.y + m[10] * offset.z;
        for (uint32_t i = 0; i < child.bone_count; ++i) std::memcpy(child.matrices + i * kMatrix, m, sizeof m);
    }

    /// How `model`, or the model it is an interface copy of, is posed.
    Pose PoseOf(void* model)
    {
        auto it = g_models.find(model);
        if (it == g_models.end())
        {
            // An interface copy of a character duplicates its attachments: the copy is posed as
            // the model it was made from, which stays alive as long as the copy does.
            if (void* source = binding::m2_model_source(model)) it = g_models.find(source);
        }
        if (it != g_models.end()) return it->second.pose;
        // Once loaded, a copy forgets the model it was copied from; the file is what remains.
        void* file = binding::m2_model_file(model);
        auto  by   = file ? g_followFiles.find(file) : g_followFiles.end();
        return by == g_followFiles.end() ? Pose{} : by->second.pose;
    }

    // --- The table ----------------------------------------------------------------------------

    void* WXL_CALL ObjectModel(wxl_handle object)
    {
        static Said said;
        if (object == WXL_HANDLE_NONE || !OnMainThread("ObjectModel", said)) return nullptr;
        void* native = wxl::core::objects::Service().Handles().Resolve(object);
        return native ? binding::m2_object_model(native) : nullptr;
    }

    wxl_result WXL_CALL ItemDisplay(uint32_t display_id, wxl_m2_item_display* out)
    {
        if (!out || display_id == 0 || out->struct_size < sizeof(uint32_t) * 2) return WXL_E_INVALID;
        wxl_m2_item_display row{};
        row.struct_size = sizeof row;
        if (!binding::m2_item_display(display_id, &row)) return WXL_E_NOT_FOUND;
        // Written no longer than the caller's struct says it is.
        const uint32_t size = out->struct_size < sizeof row ? out->struct_size : static_cast<uint32_t>(sizeof row);
        row.struct_size     = size;
        std::memcpy(out, &row, size);
        return WXL_OK;
    }

    wxl_result WXL_CALL RacePrefix(uint32_t race_id, wxl_str* out)
    {
        if (!out) return WXL_E_INVALID;
        const char* prefix = binding::m2_race_prefix(race_id);
        if (!prefix) return WXL_E_NOT_FOUND;
        out->data = prefix;
        out->size = static_cast<uint32_t>(std::strlen(prefix));
        return WXL_OK;
    }

    void* WXL_CALL CreateModel(void* scene_model, wxl_str path)
    {
        static Said said;
        const std::string file = Text(path);
        if (!scene_model || file.empty() || !OnMainThread("CreateModel", said)) return nullptr;
        void* model = binding::m2_create_model(scene_model, file.c_str());
        if (model) g_models[model] = Model{CurrentMod(), binding::m2_model_file(model), Pose{}};
        return model;
    }

    void WXL_CALL ReleaseModel(void* model)
    {
        static Said said;
        if (OnMainThread("Release", said)) Forget(model);
    }

    wxl_result WXL_CALL AttachModel(void* model, void* parent, uint32_t attachment, uint32_t flags)
    {
        static Said said;
        if (!OnMainThread("Attach", said)) return WXL_E_INVALID;
        Model* known = Known(model);
        if (!known || !parent || parent == model) return WXL_E_INVALID;
        const wxl_bool to_root = (flags & WXL_M2_ATTACH_ROOT) ? WXL_TRUE : WXL_FALSE;
        if (!binding::m2_attach_model(model, parent, attachment, to_root)) return WXL_E_NOT_FOUND;
        Pose pose{};
        pose.kind = (flags & WXL_M2_ATTACH_FOLLOW_BONES) ? Pose::Bones : Pose::None;
        SetPose(*known, pose);
        return WXL_OK;
    }

    void WXL_CALL DetachModel(void* model)
    {
        static Said said;
        Model*      known = OnMainThread("Detach", said) ? Known(model) : nullptr;
        if (!known) return;
        SetPose(*known, Pose{});
        binding::m2_detach_model(model);
    }

    void* WXL_CALL ModelParent(void* model) { return Known(model) ? binding::m2_model_parent(model) : nullptr; }

    void WXL_CALL SetGeosets(void* model, uint32_t first, uint32_t last, wxl_bool visible)
    {
        static Said said;
        if (first <= last && OnMainThread("SetGeosets", said) && Known(model))
            binding::m2_set_geosets(model, first, last, visible);
    }

    wxl_result WXL_CALL ReplaceTexture(void* model, uint32_t type, wxl_str path)
    {
        static Said       said;
        const std::string file = Text(path);
        if (!OnMainThread("ReplaceTexture", said) || !Known(model) || file.empty()) return WXL_E_INVALID;
        return binding::m2_replace_texture(model, type, file.c_str()) ? WXL_OK : WXL_E_NOT_FOUND;
    }

    wxl_result WXL_CALL AttachmentPoint(const void* model, uint32_t attachment, wxl_vec3* out)
    {
        if (!model || !out) return WXL_E_INVALID;
        return binding::m2_attachment_point(model, attachment, out) ? WXL_OK : WXL_E_NOT_FOUND;
    }

    wxl_result WXL_CALL AttachToBone(void* model, void* parent, uint32_t attachment, uint32_t key_bone, wxl_vec3 offset)
    {
        static Said said;
        if (!OnMainThread("AttachToBone", said)) return WXL_E_INVALID;
        Model* known = Known(model);
        if (!known || !parent || parent == model) return WXL_E_INVALID;
        // A parent that has not loaded cannot be asked; it is taken at its word.
        wxl_core_m2_skeleton skeleton{};
        if (binding::m2_skeleton(parent, &skeleton) && KeyBone(skeleton, key_bone) == kNone) return WXL_E_NOT_FOUND;
        if (!binding::m2_attach_model(model, parent, attachment, WXL_TRUE)) return WXL_E_NOT_FOUND;
        Pose pose{};
        pose.kind    = Pose::OnBone;
        pose.keyBone = key_bone;
        pose.offset  = offset;
        SetPose(*known, pose);
        return WXL_OK;
    }

    // --- Bone watchers ----------------------------------------------------------------------

    wxl_result WXL_CALL WatchBones(wxl_m2_bones_filter_fn filter, wxl_m2_bones_fn fn, void* user, uint32_t* out_token)
    {
        if (!fn) return WXL_E_INVALID;
        for (BoneWatcher& watcher : g_bones)
        {
            if (watcher.live.load()) continue;
            watcher.filter = filter;
            watcher.fn     = fn;
            watcher.user   = user;
            watcher.owner  = CurrentMod();
            watcher.token  = g_nextBoneToken++;
            watcher.live.store(true);
            ++g_boneLive;
            ++g_boneEpoch;
            if (out_token) *out_token = watcher.token;
            return WXL_OK;
        }
        return WXL_E_FAILED;
    }

    void WXL_CALL UnwatchBones(uint32_t token)
    {
        for (BoneWatcher& watcher : g_bones)
            if (watcher.token == token) DropBones(watcher);
    }

    wxl_result WXL_CALL BoneInfo(void* file, uint32_t index, int16_t* out_parent, wxl_vec3* out_pivot)
    {
        if (!file) return WXL_E_INVALID;
        return binding::m2_file_bone(file, index, out_parent, out_pivot) ? WXL_OK : WXL_E_NOT_FOUND;
    }

    // --- Models of the world --------------------------------------------------------------------

    wxl_m2_placement Placement(const wxl_m2_placement* in)
    {
        wxl_m2_placement out{};
        out.struct_size = sizeof out;
        out.scale       = 1.0f;
        if (!in) return out;
        const uint32_t size = in->struct_size ? in->struct_size : sizeof out;
        if (size >= offsetof(wxl_m2_placement, position) + sizeof(wxl_vec3)) out.position = in->position;
        if (size >= offsetof(wxl_m2_placement, roll) + sizeof(float))
        {
            out.yaw   = in->yaw;
            out.pitch = in->pitch;
            out.roll  = in->roll;
        }
        if (size >= offsetof(wxl_m2_placement, scale) + sizeof(float) && in->scale > 0.0f) out.scale = in->scale;
        return out;
    }

    void* WXL_CALL CreateWorldModel(wxl_str path)
    {
        const std::string file = Text(path);
        if (file.empty()) return nullptr;
        void* model = binding::m2_create_world_model(file.c_str());
        if (!model) return nullptr;
        Model record{};
        record.owner = CurrentMod();
        record.file  = binding::m2_model_file(model);
        g_models[model] = record;
        return model;
    }

    wxl_result WXL_CALL PlaceModel(void* model, const wxl_m2_placement* placement)
    {
        Model* known = Known(model);
        if (!known || !placement) return WXL_E_INVALID;
        SetPose(*known, Pose{});
        const wxl_m2_placement at = Placement(placement);
        if (!binding::m2_place_world(model, &at)) return WXL_E_FAILED;
        known->world = true;
        return WXL_OK;
    }

    wxl_result WXL_CALL SetModelTransform(void* model, const wxl_m2_placement* placement)
    {
        if (!Known(model) || !placement) return WXL_E_INVALID;
        const wxl_m2_placement at = Placement(placement);
        binding::m2_set_transform(model, &at);
        return WXL_OK;
    }

    wxl_result WXL_CALL PlayAnimation(void* model, uint32_t anim_id, float speed)
    {
        if (!Known(model)) return WXL_E_INVALID;
        binding::m2_play_animation(model, anim_id, speed > 0.0f ? speed : 1.0f);
        return WXL_OK;
    }

    wxl_bool WXL_CALL ModelLoaded(void* model)
    {
        return model ? binding::m2_loaded(model) : WXL_FALSE;
    }

    wxl_result WXL_CALL AnimationLength(void* model, uint32_t anim_id, uint32_t* out_ms)
    {
        if (!model || !out_ms) return WXL_E_INVALID;
        if (!binding::m2_loaded(model)) return WXL_E_FAILED;
        *out_ms = binding::m2_animation_length(model, anim_id);
        return WXL_OK;
    }

    void WXL_CALL SetAlpha(void* model, float alpha)
    {
        if (Known(model)) binding::m2_set_alpha(model, alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha);
    }

    void WXL_CALL SetEmitting(void* model, wxl_bool emitting)
    {
        if (Known(model)) binding::m2_set_emitting(model, emitting);
    }

    wxl_result WXL_CALL AttachmentWorld(void* model, uint32_t attachment, wxl_vec3* out)
    {
        if (!model || !out) return WXL_E_INVALID;
        if (!binding::m2_loaded(model)) return WXL_E_FAILED;
        return binding::m2_attachment_world(model, attachment, out) ? WXL_OK : WXL_E_NOT_FOUND;
    }

    wxl_result WXL_CALL GroundHeight(const wxl_vec3* at, float up, float down, float* out_z)
    {
        if (!at || !out_z || up < 0.0f || down < 0.0f) return WXL_E_INVALID;
        return binding::m2_ground_height(at, up, down, out_z) ? WXL_OK : WXL_E_NOT_FOUND;
    }

    const wxl_m2_api kApi = {
        sizeof(wxl_m2_api), &ObjectModel,  &ItemDisplay,     &RacePrefix,      &CreateModel,
        &ReleaseModel,      &AttachModel,  &DetachModel,     &ModelParent,     &SetGeosets,
        &ReplaceTexture,    &AttachmentPoint, &AttachToBone,
        &CreateWorldModel,  &PlaceModel,   &SetModelTransform, &PlayAnimation, &ModelLoaded,
        &AnimationLength,   &SetAlpha,     &SetEmitting,     &AttachmentWorld, &GroundHeight,
        &WatchBones,        &UnwatchBones, &BoneInfo,
    };
}

namespace wxl::core::services
{
    const wxl_m2_api& M2Api() { return kApi; }

    namespace m2
    {
        bool Following() { return g_following.load(std::memory_order_relaxed) != 0; }

        void PoseAttached(void* model)
        {
            if (!model || !Following()) return;
            const Pose pose = PoseOf(model);
            if (pose.kind == Pose::OnBone)
                PoseOnBone(model, pose.keyBone, pose.offset);
            else if (pose.kind == Pose::Bones)
                PoseFromParent(model);
        }

        bool WatchingBones() { return g_boneLive.load() != 0; }

        void DispatchBones(void* model)
        {
            wxl_m2_bones bones{};
            bones.struct_size = sizeof bones;
            if (!binding::m2_bones(model, &bones) || !bones.file) return;
            const uint32_t mask = WantsOf(bones);
            for (uint32_t i = 0; i < kBoneSlots; ++i)
            {
                if (!(mask & (1u << i))) continue;
                BoneWatcher& watcher = g_bones[i];
                if (!watcher.live.load()) continue;
                BoneCall call{&watcher, &bones, WXL_TRUE};
                GuardedBones(watcher, &InvokeBoneWatcher, call);
            }
        }

        bool HasModels() { return !g_models.empty(); }

        void* SourceOf(void* model)
        {
            if (!model) return nullptr;
            if (g_models.contains(model)) return model;
            void* source = binding::m2_model_source(model);
            if (source && g_models.contains(source)) return source;
            void* file = binding::m2_model_file(model);
            if (!file) return nullptr;
            for (const auto& [owned, record] : g_models)
                if (record.file == file) return owned;
            return nullptr;
        }

        void ForgetMod(uint32_t mod)
        {
            std::vector<void*> theirs;
            for (const auto& [model, record] : g_models)
                if (record.owner == mod) theirs.push_back(model);
            for (void* model : theirs) Forget(model);
            for (BoneWatcher& watcher : g_bones)
                if (watcher.owner == mod) DropBones(watcher);
        }

        void Clear()
        {
            for (BoneWatcher& watcher : g_bones) DropBones(watcher);
            {
                std::unique_lock lock(g_wantLock);
                g_wants.clear();
            }
            g_models.clear();
            g_followFiles.clear();
            g_following.store(0, std::memory_order_relaxed);
        }
    }
}
