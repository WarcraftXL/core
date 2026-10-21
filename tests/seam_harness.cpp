// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The seam harness: the seam bodies, driven by hand against a real mod host and a fake client.

#include "binding/Binding.hpp"
#include "objects/Objects.hpp"
#include "runtime/Attachment.hpp"
#include "runtime/Frame.hpp"
#include "runtime/Stages.hpp"
#include "seams/Table.hpp"
#include "services/DataService.hpp"
#include "services/GxService.hpp"
#include "services/M2FileService.hpp"
#include "services/M2Service.hpp"
#include "services/ScriptService.hpp"
#include "services/StorageService.hpp"

#include <chrono>

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"
#include "wxl/seams/seams.h"
#include "wxl/services/script.h"
#include "wxl/services/storage.h"

#if defined(_M_IX86)
    #include "seams/Detour.hpp"
    #include "seams/RegparmX86.hpp"
#endif

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

// --- What Core.cpp and the attachments supply in the DLL -------------------------------------

namespace
{
    std::unique_ptr<wxl::host::ModHost> g_host;
}

namespace wxl::core
{
    wxl::host::ModHost* Host() { return g_host.get(); }
}

namespace
{
    int g_released = 0;
}

// The attachment is per-architecture and the harness is neither, so it stands in for it. What
// the seam bodies ask of it is counted, which is how the checks below know it was asked.
namespace wxl::core::attachment
{
    void Release(uint32_t) { ++g_released; }
    void ReleaseAll() { ++g_released; }
}

// --- A fake client, handed to the binding as a wxl_core_init ---------------------------------

namespace
{
    /// An object as the fake client keeps it; the binding's accessors read these.
    struct FakeObject
    {
        uint64_t guid   = 0;
        uint32_t typeId = 0;
        uint32_t entry  = 0;
        wxl_vec3 position{};
        uint32_t health = 0;
        int32_t  visibleItems[19]{};
        uint32_t castingSpellId = 0; ///< the cast the client shows on a unit
        uint32_t channelSpellId = 0; ///< the channel it shows
    };

    /// A visible item as CGPlayer_C::UpdateInventoryComponent is handed one.
    struct FakeVisibleItem
    {
        int32_t itemId = 0;
    };

    struct FakeArea
    {
        int32_t tileX = 0;
        int32_t tileY = 0;
    };

    int32_t     g_mapId       = -1;
    uint64_t    g_targetGuid  = 0;
    FakeObject* g_localPlayer = nullptr;

    const FakeObject* Obj(const void* object) { return static_cast<const FakeObject*>(object); }

    int32_t WXL_CALL FakeMapId(void) { return g_mapId; }
    const void* WXL_CALL FakeLocalPlayer(void) { return g_localPlayer; }
    uint64_t WXL_CALL FakeTargetGuid(void) { return g_targetGuid; }

    uint64_t WXL_CALL FakeGuid(const void* object) { return Obj(object)->guid; }
    uint32_t WXL_CALL FakeTypeId(const void* object) { return Obj(object)->typeId; }
    uint32_t WXL_CALL FakeEntry(const void* object) { return Obj(object)->entry; }
    uint32_t WXL_CALL FakeHealth(const void* object) { return Obj(object)->health; }
    uint32_t WXL_CALL FakeCastingSpellId(const void* object) { return Obj(object)->castingSpellId; }
    uint32_t WXL_CALL FakeChannelSpellId(const void* object) { return Obj(object)->channelSpellId; }

    wxl_bool WXL_CALL FakePosition(const void* object, wxl_vec3* out)
    {
        if (Obj(object)->typeId == WXL_OBJECT_ITEM) return WXL_FALSE;
        *out = Obj(object)->position;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeVisibleItemId(const void* object, uint32_t slot, int32_t* out)
    {
        if (slot >= 19) return WXL_FALSE;
        *out = Obj(object)->visibleItems[slot];
        return WXL_TRUE;
    }

    int32_t WXL_CALL FakeVisibleItemIdOf(const void* item)
    {
        return static_cast<const FakeVisibleItem*>(item)->itemId;
    }

    int32_t WXL_CALL FakeTileX(const void* area) { return static_cast<const FakeArea*>(area)->tileX; }
    int32_t WXL_CALL FakeTileY(const void* area) { return static_cast<const FakeArea*>(area)->tileY; }

    // --- A fake script engine: a value stack, globals, an event table, as the script rows see them

    struct FakeValue
    {
        uint32_t    type = WXL_VALUE_NIL;
        double      number = 0.0;
        std::string string;
        bool        boolean = false;
    };

    /// A global the runtime set: the C closure and the slot in its upvalue.
    struct FakeClosure
    {
        wxl_script_native_fn fn   = nullptr;
        uint32_t             slot = 0;
    };

    struct FakeSignal
    {
        uint32_t               index = 0;
        std::vector<FakeValue> args;
    };

    struct FakeVm
    {
        bool                                  live = false;
        std::vector<FakeValue>                stack;
        std::map<std::string, FakeClosure>    closures;      ///< globals the runtime set
        std::set<std::string>                 clientGlobals; ///< globals the client set
        uint32_t                              runningSlot = 0;
        std::vector<std::string>              errors;
        std::vector<std::string>              ran;
        std::vector<std::vector<std::string>> eventTables; ///< every table installed, in order
        std::vector<FakeSignal>               signals;
    };

    FakeVm  g_vm;
    int32_t g_uiInitialized = 0;
    uint8_t g_uiLoggingIn   = 0;

    FakeVm* Vm(void* state) { return static_cast<FakeVm*>(state); }

    /// The stack slot a positive Lua index names, or null.
    const FakeValue* At(void* state, int32_t index)
    {
        FakeVm* vm = Vm(state);
        if (!vm || index <= 0 || static_cast<size_t>(index) > vm->stack.size()) return nullptr;
        return &vm->stack[static_cast<size_t>(index) - 1];
    }

    void* WXL_CALL FakeScriptState(void) { return g_vm.live ? &g_vm : nullptr; }
    int32_t WXL_CALL FakeUiInitialized(void) { return g_uiInitialized; }
    uint8_t WXL_CALL FakeUiLoggingIn(void) { return g_uiLoggingIn; }

    wxl_bool WXL_CALL FakeGlobalIsSet(void* state, const char* name)
    {
        return Vm(state)->closures.count(name) || Vm(state)->clientGlobals.count(name) ? WXL_TRUE
                                                                                      : WXL_FALSE;
    }

    void WXL_CALL FakeSetGlobalClosure(void* state, const char* name, wxl_script_native_fn fn, uint32_t slot)
    {
        Vm(state)->closures[name] = FakeClosure{fn, slot};
    }

    void WXL_CALL FakeClearGlobal(void* state, const char* name) { Vm(state)->closures.erase(name); }
    uint32_t WXL_CALL FakeClosureSlot(void* state) { return Vm(state)->runningSlot; }
    int32_t WXL_CALL FakeTop(void* state) { return static_cast<int32_t>(Vm(state)->stack.size()); }

    uint32_t WXL_CALL FakeValueType(void* state, int32_t index)
    {
        const FakeValue* value = At(state, index);
        return value ? value->type : WXL_VALUE_NIL;
    }

    double WXL_CALL FakeToNumber(void* state, int32_t index)
    {
        const FakeValue* value = At(state, index);
        return value ? value->number : 0.0;
    }

    const char* WXL_CALL FakeToString(void* state, int32_t index, uint32_t* length)
    {
        const FakeValue* value = At(state, index);
        if (!value) return nullptr;
        // A number reads as its text, as lua_tolstring converts one in place.
        if (value->type == WXL_VALUE_NUMBER)
        {
            static std::string rendered;
            char               text[32];
            snprintf(text, sizeof text, "%.14g", value->number);
            rendered = text;
            if (length) *length = static_cast<uint32_t>(rendered.size());
            return rendered.c_str();
        }
        if (length) *length = static_cast<uint32_t>(value->string.size());
        return value->string.c_str();
    }

    wxl_bool WXL_CALL FakeToBool(void* state, int32_t index)
    {
        const FakeValue* value = At(state, index);
        return value && value->boolean ? WXL_TRUE : WXL_FALSE;
    }

    void WXL_CALL FakePushNumber(void* state, double number)
    {
        FakeValue value;
        value.type   = WXL_VALUE_NUMBER;
        value.number = number;
        Vm(state)->stack.push_back(value);
    }

    void WXL_CALL FakePushString(void* state, const char* text, uint32_t length)
    {
        FakeValue value;
        value.type   = WXL_VALUE_STRING;
        value.string = std::string(text, length);
        Vm(state)->stack.push_back(value);
    }

    void WXL_CALL FakePushBool(void* state, wxl_bool flag)
    {
        FakeValue value;
        value.type    = WXL_VALUE_BOOL;
        value.boolean = flag != WXL_FALSE;
        Vm(state)->stack.push_back(value);
    }

    void WXL_CALL FakePushNil(void* state) { Vm(state)->stack.push_back(FakeValue{}); }

    void WXL_CALL FakePop(void* state, int32_t count)
    {
        std::vector<FakeValue>& stack = Vm(state)->stack;
        const size_t            n     = count > 0 ? static_cast<size_t>(count) : 0;
        stack.resize(stack.size() > n ? stack.size() - n : 0);
    }

    /// The fake records the error and returns, where the client's would not; the thunk is
    /// written so that it returns nothing to the caller either way.
    void WXL_CALL FakeError(void* state, const char* message) { Vm(state)->errors.push_back(message); }

    void WXL_CALL FakeRun(const char* source, const char* name)
    {
        g_vm.ran.push_back(std::string(name) + ": " + source);
    }

    void WXL_CALL FakeCreateEvents(const char** names, uint32_t count)
    {
        g_vm.eventTables.push_back(std::vector<std::string>(names, names + count));
    }

    void WXL_CALL FakeSignalEvent(uint32_t index, void* state, int32_t argCount)
    {
        FakeSignal signal;
        signal.index                  = index;
        std::vector<FakeValue>& stack = Vm(state)->stack;
        const size_t            n     = argCount > 0 ? static_cast<size_t>(argCount) : 0;
        signal.args.assign(stack.end() - static_cast<std::ptrdiff_t>(n), stack.end());
        Vm(state)->signals.push_back(signal);
    }

    /// Calls a global the runtime set, as the client's Lua would: the arguments on the stack,
    /// the closure's slot in its upvalue, the results being whatever the function left on top.
    std::vector<FakeValue> CallGlobal(const char* name, std::vector<FakeValue> args, bool* found)
    {
        const auto it = g_vm.closures.find(name);
        *found         = it != g_vm.closures.end();
        if (!*found) return {};
        g_vm.stack       = std::move(args);
        g_vm.runningSlot = it->second.slot;
        const int32_t pushed = it->second.fn(&g_vm);
        const size_t  n      = pushed > 0 ? static_cast<size_t>(pushed) : 0;
        std::vector<FakeValue> results(g_vm.stack.end() - static_cast<std::ptrdiff_t>(n), g_vm.stack.end());
        g_vm.stack.clear();
        return results;
    }

    FakeValue Number(double number)
    {
        FakeValue value;
        value.type   = WXL_VALUE_NUMBER;
        value.number = number;
        return value;
    }

    FakeValue String(const char* text)
    {
        FakeValue value;
        value.type   = WXL_VALUE_STRING;
        value.string = text;
        return value;
    }

    /// The accessors the fake client hands over: what the seams and the checks below read.
    /// A message as the client holds one: a buffer, its end, and where reading starts.
    struct FakeStore
    {
        const uint8_t* data = nullptr;
        uint32_t       size = 0;
        uint32_t       read = 0;
    };

    const void* WXL_CALL FakeMessageData(const void* store)
    {
        return store ? static_cast<const FakeStore*>(store)->data : nullptr;
    }

    uint32_t WXL_CALL FakeMessageSize(const void* store)
    {
        return store ? static_cast<const FakeStore*>(store)->size : 0u;
    }

    uint32_t WXL_CALL FakeMessageRead(const void* store)
    {
        return store ? static_cast<const FakeStore*>(store)->read : 0u;
    }

    /// A pending cast, as the spell rows read one.
    struct FakePendingCast
    {
        uint64_t caster  = 0;
        uint32_t spellId = 0;
        uint8_t  count   = 0;
    };

    uint64_t WXL_CALL FakePendingCaster(const void* cast)
    {
        return static_cast<const FakePendingCast*>(cast)->caster;
    }

    uint32_t WXL_CALL FakePendingSpellId(const void* cast)
    {
        return static_cast<const FakePendingCast*>(cast)->spellId;
    }

    uint8_t WXL_CALL FakePendingCount(const void* cast)
    {
        return static_cast<const FakePendingCast*>(cast)->count;
    }

    /// One file the fake client holds, and the cursor a reader is at.
    struct FakeFile
    {
        std::string name;
        std::string bytes;
        uint64_t    at    = 0;
        bool        open  = false;
    };

    std::vector<FakeFile> g_files;
    int                   g_opens  = 0;
    int                   g_closes = 0;

    FakeFile* FileNamed(const char* name)
    {
        for (FakeFile& file : g_files)
            if (file.name == name) return &file;
        return nullptr;
    }

    wxl_bool WXL_CALL FakeFileExists(const char* path)
    {
        return path && FileNamed(path) ? WXL_TRUE : WXL_FALSE;
    }

    void* WXL_CALL FakeFileOpen(const char* path)
    {
        FakeFile* file = path ? FileNamed(path) : nullptr;
        if (!file) return nullptr;
        file->at   = 0;
        file->open = true;
        ++g_opens;
        return file;
    }

    uint64_t WXL_CALL FakeFileSize(void* file)
    {
        return file ? static_cast<const FakeFile*>(file)->bytes.size() : 0u;
    }

    wxl_bool WXL_CALL FakeFileSeek(void* file, uint64_t offset)
    {
        if (!file) return WXL_FALSE;
        static_cast<FakeFile*>(file)->at = offset;
        return WXL_TRUE;
    }

    uint32_t WXL_CALL FakeFileRead(void* handle, void* buffer, uint32_t count)
    {
        FakeFile* file = static_cast<FakeFile*>(handle);
        if (!file || !buffer || file->at >= file->bytes.size()) return 0;
        const uint64_t left = file->bytes.size() - file->at;
        const uint32_t got  = left < count ? static_cast<uint32_t>(left) : count;
        memcpy(buffer, file->bytes.data() + file->at, got);
        file->at += got;
        return got;
    }

    void WXL_CALL FakeFileClose(void* file)
    {
        if (!file) return;
        static_cast<FakeFile*>(file)->open = false;
        ++g_closes;
    }

    wxl_bool WXL_CALL FakeArchiveMount(const char*, int32_t) { return WXL_TRUE; }

    // The graphics device, as the paint and the device's resets see it.
    bool     g_gxContext = true;
    bool     g_gxVisible = true;
    bool     g_gxDevice  = true;
    uint32_t g_gxWidth   = 1920;
    uint32_t g_gxHeight  = 1080;

    wxl_bool WXL_CALL FakeGxDeviceExists() { return g_gxDevice ? WXL_TRUE : WXL_FALSE; }
    wxl_bool WXL_CALL FakeGxHasContext() { return g_gxDevice && g_gxContext ? WXL_TRUE : WXL_FALSE; }
    wxl_bool WXL_CALL FakeGxWindowVisible() { return g_gxDevice && g_gxVisible ? WXL_TRUE : WXL_FALSE; }

    wxl_bool WXL_CALL FakeGxWindowSize(uint32_t* width, uint32_t* height)
    {
        if (!g_gxDevice || !width || !height) return WXL_FALSE;
        *width  = g_gxWidth;
        *height = g_gxHeight;
        return WXL_TRUE;
    }

    /// The batch the fake model renderer last drew: what gx_model_draw answers, once per draw.
    wxl_gx_model_draw g_gxDraw{};
    bool              g_gxDrew = false;

    wxl_bool WXL_CALL FakeGxModelDraw(const void* render, wxl_gx_model_draw* out)
    {
        if (!render || !out || !g_gxDrew) return WXL_FALSE;
        g_gxDrew = false;
        *out     = g_gxDraw;
        return WXL_TRUE;
    }

    // --- wxl.m2: one fake appearance, and a fake model heap counting references ---------------

    struct FakeComponent
    {
        void*    model;
        uint32_t race;
        uint32_t sex;
        uint32_t display[12];
        bool     npc;
    };

    struct FakeModel
    {
        int         refs;
        void*       parent;
        void*       source;
        uint32_t    geoFirst, geoLast;
        wxl_bool    geoVisible;
        uint32_t    texType;
        std::string texture;
        int         posed;
        uint32_t    attachment = 0;
        wxl_bool    toRoot     = WXL_FALSE;
        void*       file       = nullptr; // a copy's: the model file it shares; a made model is its own
    };

    std::vector<FakeModel*> g_fakeLive; // every fake model alive, for m2_children
    int                     g_fakeCopies = 0;

    FakeComponent g_component{};
    int           g_charModel       = 0; // the appearance's character model; never released by the fake
    int           g_wearerAsked     = 0;
    int           g_fakeModelsFreed = 0;

    FakeModel* AsFake(void* model) { return model == &g_charModel ? nullptr : static_cast<FakeModel*>(model); }

    wxl_bool WXL_CALL FakeComponentWearer(const void* component, uint32_t item_slot, wxl_m2_equipment* out)
    {
        ++g_wearerAsked;
        if (component != &g_component || !out) return WXL_FALSE;
        out->wearer     = const_cast<void*>(component);
        out->model      = g_component.model;
        out->race_id    = g_component.race;
        out->sex_id     = g_component.sex;
        out->item_slot  = item_slot;
        out->display_id = item_slot < 12 ? g_component.display[item_slot] : 0;
        out->npc        = g_component.npc ? WXL_TRUE : WXL_FALSE;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeItemDisplay(uint32_t id, wxl_m2_item_display* out)
    {
        if (id != 416036 || !out) return WXL_FALSE;
        out->id               = id;
        out->model[0]         = "belt.mdx";
        out->model[1]         = "";
        out->model_texture[0] = "belt_skin";
        out->model_texture[1] = "";
        out->icon[1]          = "53::96";
        return WXL_TRUE;
    }

    const char* WXL_CALL FakeRacePrefix(uint32_t race) { return race == 1 ? "Hu" : nullptr; }

    void* WXL_CALL FakeCreateModel(void* scene_model, const char* path)
    {
        if (!scene_model || !path || !*path) return nullptr;
        FakeModel* model = new FakeModel{1, nullptr, nullptr, 0, 0, WXL_FALSE, 0, {}, 0};
        g_fakeLive.push_back(model);
        return model;
    }

    void WXL_CALL FakeReleaseModel(void* model)
    {
        FakeModel* fake = AsFake(model);
        if (!fake || --fake->refs > 0) return;
        std::erase(g_fakeLive, fake);
        delete fake;
        ++g_fakeModelsFreed;
    }

    wxl_bool WXL_CALL FakeAttachModel(void* model, void* parent, uint32_t attachment, wxl_bool to_root)
    {
        FakeModel* fake = AsFake(model);
        if (!fake || !parent || (attachment == 99 && !to_root)) return WXL_FALSE; // 99: a point the parent lacks
        if (!fake->parent) ++fake->refs;                                           // the parent's reference
        fake->parent     = parent;
        fake->attachment = attachment;
        fake->toRoot     = to_root;
        return WXL_TRUE;
    }

    void WXL_CALL FakeDetachModel(void* model)
    {
        FakeModel* fake = AsFake(model);
        if (!fake || !fake->parent) return;
        fake->parent = nullptr;
        FakeReleaseModel(model);
    }

    void* WXL_CALL FakeModelParent(const void* model)
    {
        FakeModel* fake = AsFake(const_cast<void*>(model));
        return fake ? fake->parent : nullptr;
    }

    void* WXL_CALL FakeModelSource(const void* model)
    {
        FakeModel* fake = AsFake(const_cast<void*>(model));
        return fake ? fake->source : nullptr;
    }

    void WXL_CALL FakeSetGeosets(void* model, uint32_t first, uint32_t last, wxl_bool visible)
    {
        if (FakeModel* fake = AsFake(model))
        {
            fake->geoFirst   = first;
            fake->geoLast    = last;
            fake->geoVisible = visible;
        }
    }

    wxl_bool WXL_CALL FakeReplaceTexture(void* model, uint32_t type, const char* path)
    {
        FakeModel* fake = AsFake(model);
        if (!fake || !path || std::strcmp(path, "missing.blp") == 0) return WXL_FALSE;
        fake->texType = type;
        fake->texture = path;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakePoseFromParent(void* model)
    {
        FakeModel* fake = AsFake(model);
        if (!fake) return WXL_FALSE;
        ++fake->posed;
        return WXL_TRUE;
    }

    void* WXL_CALL FakeModelFile(const void* model)
    {
        FakeModel* fake = AsFake(const_cast<void*>(model));
        return fake ? (fake->file ? fake->file : fake) : nullptr;
    }

    wxl_bool WXL_CALL FakeAttachmentPoint(const void* model, uint32_t attachment, wxl_vec3* out)
    {
        if (model != &g_charModel || attachment != 32 || !out) return WXL_FALSE;
        *out = wxl_vec3{0.1f, 0.2f, 1.1f};
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeHasKeyBone(const void* model, uint32_t key_bone) { return model && key_bone != 99 ? WXL_TRUE : WXL_FALSE; }

    uint32_t g_posedOnBone = 0; // the key bone the last on-bone pose asked for
    wxl_vec3 g_posedOffset{};

    wxl_bool WXL_CALL FakePoseOnBone(void* model, uint32_t key_bone, const wxl_vec3* offset)
    {
        FakeModel* fake = AsFake(model);
        if (!fake || !offset) return WXL_FALSE;
        fake->posed += 100;
        g_posedOnBone = key_bone;
        g_posedOffset = *offset;
        return WXL_TRUE;
    }

    // --- wxl.m2file: one fake model file, its bytes and skin, and what was done to it -----------

    struct FakeM2File
    {
        uint8_t  bytes[64];
        uint32_t size;
        uint8_t  skin[48];
        bool     hasSkin;
        int      asked;    // m2_file_info calls
        uint32_t setSize;  // the last size the seam set, 0xFFFFFFFF for none
        int      animated; // m2_file_animate_bones calls
    };

    FakeM2File g_m2File{};

    wxl_bool WXL_CALL FakeM2FileInfo(const void* file, wxl_m2file* out)
    {
        ++g_m2File.asked;
        if (file != &g_m2File || !out) return WXL_FALSE;
        out->name = "creature\\fake\\fake.m2";
        out->data = g_m2File.bytes;
        out->size = g_m2File.size;
        out->skin = g_m2File.hasSkin ? g_m2File.skin : nullptr;
        return WXL_TRUE;
    }

    void WXL_CALL FakeM2FileSetSize(void* file, uint32_t size)
    {
        if (file == &g_m2File) g_m2File.setSize = size;
    }

    void WXL_CALL FakeM2FileAnimateBones(void* file)
    {
        if (file == &g_m2File) ++g_m2File.animated;
    }

    uint32_t WXL_CALL FakeChildren(const void* model, void** out, uint32_t capacity)
    {
        uint32_t count = 0;
        for (FakeModel* fake : g_fakeLive)
        {
            if (fake->parent != model) continue;
            if (out && count < capacity) out[count] = fake;
            ++count;
        }
        return count;
    }

    wxl_bool WXL_CALL FakeAttachInfo(const void* model, uint32_t* attachment, wxl_bool* to_root)
    {
        FakeModel* fake = AsFake(const_cast<void*>(model));
        if (!fake || !fake->parent) return WXL_FALSE;
        *attachment = fake->attachment;
        *to_root    = fake->toRoot;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL FakeAttachCopy(void* source, void* parent, uint32_t attachment, wxl_bool to_root)
    {
        if (!source || !parent) return WXL_FALSE;
        FakeModel* copy = new FakeModel{1, parent, source, 0, 0, WXL_FALSE, 0, {}, 0, attachment, to_root};
        g_fakeLive.push_back(copy);
        ++g_fakeCopies;
        return WXL_TRUE;
    }

    /// Every other row stays null, which is what a client that hands nothing over looks like.
    wxl_core_init FakeInit()
    {
        wxl_core_init init{};
        init.struct_size            = sizeof init;
        init.map_id                 = &FakeMapId;
        init.local_player_object    = &FakeLocalPlayer;
        init.ui_target_guid         = &FakeTargetGuid;
        init.object_guid            = &FakeGuid;
        init.object_type_id         = &FakeTypeId;
        init.object_entry           = &FakeEntry;
        init.object_position        = &FakePosition;
        init.unit_health            = &FakeHealth;
        init.unit_casting_spell_id  = &FakeCastingSpellId;
        init.unit_channel_spell_id  = &FakeChannelSpellId;
        init.player_visible_item_id = &FakeVisibleItemId;
        init.visible_item_id        = &FakeVisibleItemIdOf;
        init.map_area_tile_x        = &FakeTileX;
        init.map_area_tile_y        = &FakeTileY;
        init.ui_initialized         = &FakeUiInitialized;
        init.ui_logging_in          = &FakeUiLoggingIn;
        init.script_state           = &FakeScriptState;
        init.script_global_is_set   = &FakeGlobalIsSet;
        init.script_set_global_closure = &FakeSetGlobalClosure;
        init.script_clear_global    = &FakeClearGlobal;
        init.script_closure_slot    = &FakeClosureSlot;
        init.script_top             = &FakeTop;
        init.script_value_type      = &FakeValueType;
        init.script_to_number       = &FakeToNumber;
        init.script_to_string       = &FakeToString;
        init.script_to_bool         = &FakeToBool;
        init.script_push_number     = &FakePushNumber;
        init.script_push_string     = &FakePushString;
        init.script_push_bool       = &FakePushBool;
        init.script_push_nil        = &FakePushNil;
        init.script_pop             = &FakePop;
        init.script_error           = &FakeError;
        init.script_run             = &FakeRun;
        init.script_create_events   = &FakeCreateEvents;
        init.script_signal_event    = &FakeSignalEvent;
        init.message_data           = &FakeMessageData;
        init.message_size           = &FakeMessageSize;
        init.message_read           = &FakeMessageRead;
        init.file_exists            = &FakeFileExists;
        init.file_open              = &FakeFileOpen;
        init.file_size              = &FakeFileSize;
        init.file_seek              = &FakeFileSeek;
        init.file_read              = &FakeFileRead;
        init.file_close             = &FakeFileClose;
        init.archive_mount          = &FakeArchiveMount;
        init.gx_device_exists       = &FakeGxDeviceExists;
        init.gx_has_context         = &FakeGxHasContext;
        init.gx_window_visible      = &FakeGxWindowVisible;
        init.gx_window_size         = &FakeGxWindowSize;
        init.gx_model_draw          = &FakeGxModelDraw;
        init.component_wearer       = &FakeComponentWearer;
        init.m2_item_display        = &FakeItemDisplay;
        init.m2_race_prefix         = &FakeRacePrefix;
        init.m2_create_model        = &FakeCreateModel;
        init.m2_release_model       = &FakeReleaseModel;
        init.m2_attach_model        = &FakeAttachModel;
        init.m2_detach_model        = &FakeDetachModel;
        init.m2_model_parent        = &FakeModelParent;
        init.m2_model_source        = &FakeModelSource;
        init.m2_set_geosets         = &FakeSetGeosets;
        init.m2_replace_texture     = &FakeReplaceTexture;
        init.m2_pose_from_parent    = &FakePoseFromParent;
        init.m2_children            = &FakeChildren;
        init.m2_attach_info         = &FakeAttachInfo;
        init.m2_attach_copy         = &FakeAttachCopy;
        init.m2_model_file          = &FakeModelFile;
        init.m2_attachment_point    = &FakeAttachmentPoint;
        init.m2_has_key_bone        = &FakeHasKeyBone;
        init.m2_pose_on_bone        = &FakePoseOnBone;
        init.m2_file_info           = &FakeM2FileInfo;
        init.m2_file_set_size       = &FakeM2FileSetSize;
        init.m2_file_animate_bones  = &FakeM2FileAnimateBones;
        init.pending_cast_caster    = &FakePendingCaster;
        init.pending_cast_spell_id  = &FakePendingSpellId;
        init.pending_cast_count     = &FakePendingCount;
        return init;
    }
}

// --- What a mod receives ----------------------------------------------------------------------

namespace
{
    /// One event as a handler saw it: which, and a copy of the bytes it was handed.
    struct Received
    {
        wxl_event         event;
        std::vector<char> args;

        template <class T>
        T As() const
        {
            T typed{};
            if (args.size() >= sizeof typed) memcpy(&typed, args.data(), sizeof typed);
            return typed;
        }
    };

    std::vector<Received> g_received;

    /// The size of the struct each event carries, so the harness can keep a copy of it.
    size_t ArgsSize(wxl_event event)
    {
        switch (event)
        {
#define WXL_EVENT(family, name, id, args) case WXL_EV_##name: return sizeof(args);
#include "wxl/events.def"
#undef WXL_EVENT
        default: return 0;
        }
    }

    /// One tag per event id; a subscription's `user` is the address of its own, which is how a
    /// handler that is only handed `user` and `args` knows which event arrived.
    wxl_event g_tags[WXL_EV_COUNT]{};

    void WXL_CALL OnEvent(void* user, const void* args)
    {
        const wxl_event event = *static_cast<const wxl_event*>(user);
        Received record{ event, {} };
        const size_t size = ArgsSize(event);
        if (args && size)
        {
            record.args.resize(size);
            memcpy(record.args.data(), args, size);
        }
        g_received.push_back(std::move(record));
    }

    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++g_failures;
    }

    /// How many of an event arrived.
    size_t Count(wxl_event event)
    {
        size_t n = 0;
        for (const Received& record : g_received) n += record.event == event;
        return n;
    }

    /// The one event received, or null when none or more than one arrived.
    const Received* Only(wxl_event event)
    {
        const Received* found = nullptr;
        for (const Received& record : g_received)
        {
            if (record.event != event) continue;
            if (found) return nullptr;
            found = &record;
        }
        return found;
    }
}

// --- The checks -------------------------------------------------------------------------------

namespace
{
    /// How far the client has booted: announced once each, in order, gaps left as gaps.
    void CheckStages()
    {
        namespace stages = wxl::core::stages;
        stages::Restart();

        stages::Reach(WXL_STAGE_EARLY);
        Check(stages::Reached() == WXL_STAGE_EARLY, "the first stage is announced");

        stages::Reach(WXL_STAGE_STORAGE);
        Check(stages::Reached() == WXL_STAGE_STORAGE, "and the next one after it");

        stages::Reach(WXL_STAGE_STORAGE);
        stages::Reach(WXL_STAGE_EARLY);
        Check(stages::Reached() == WXL_STAGE_STORAGE,
              "a stage already announced, and one behind it, are both ignored");

        stages::Reach(WXL_STAGE_SCRIPT);
        Check(stages::Reached() == WXL_STAGE_SCRIPT,
              "a stage that was never reached is skipped, not invented: with no device brought "
              "up, script follows storage and graphics is a gap");

        stages::Restart();
        Check(stages::Reached() == WXL_STAGE_EARLY, "a runtime that starts over starts over");

        stages::Reach(WXL_STAGE_EARLY);
        std::thread([] { stages::Reach(WXL_STAGE_STORAGE); }).join();
        Check(stages::Reached() == WXL_STAGE_EARLY,
              "a stage reached on another thread is not announced there");
        stages::Reach(WXL_STAGE_STORAGE);
        Check(stages::Reached() == WXL_STAGE_STORAGE, "the main thread announces it on its next pass");
        stages::Restart();
    }

    /// The rows a client hands over decide whether its accessors are called at all.
    void CheckInitRows()
    {
        namespace binding = wxl::core::binding;
        char why[512] = {};

        wxl_core_init init = FakeInit();
        init.row_count     = WXL_CORE_INIT_ROW_COUNT;
        init.rows          = wxl_core_init_rows;
        Check(binding::Agrees(init, why, sizeof why), "a client built from the same table agrees");

        const wxl_core_init bare = FakeInit();
        Check(!binding::Agrees(bare, why, sizeof why) && strstr(why, "no rows"),
              "one that hands over no rows is refused");

        std::vector<const char*> rows(wxl_core_init_rows, wxl_core_init_rows + WXL_CORE_INIT_ROW_COUNT);
        rows[5]   = "something: else ()";
        init.rows = rows.data();
        Check(!binding::Agrees(init, why, sizeof why) && strstr(why, "row 5 differs") &&
                  strstr(why, wxl_core_init_rows[5]),
              "one row that differs is refused, naming both sides of it");

        init.rows      = wxl_core_init_rows;
        init.row_count = WXL_CORE_INIT_ROW_COUNT - 3;
        Check(binding::Agrees(init, why, sizeof why), "an older client, with fewer rows, agrees");

        init.row_count   = WXL_CORE_INIT_ROW_COUNT;
        init.struct_size = static_cast<uint32_t>(offsetof(wxl_core_init, map_id) + 8 * sizeof(void*));
        Check(!binding::Agrees(init, why, sizeof why) && strstr(why, "struct_size"),
              "rows that its struct_size does not hold are refused");
    }

    /// Every event the seam table says some body raises is one the host will let a mod subscribe
    /// to. A seam that raises an event the attachment never published is a silent dead event.
    void CheckEveryRaisedEventIsSubscribable()
    {
        for (const wxl_event event : wxl::core::seams::RaisedEvents())
        {
            g_tags[event] = event;
            const wxl_subscription subscription =
                g_host->Events().Subscribe(event, &OnEvent, &g_tags[event], 0);
            char what[128];
            snprintf(what, sizeof what, "a mod can subscribe to event %u", unsigned(event));
            Check(subscription != WXL_SUBSCRIPTION_NONE, what);
        }
    }

    /// Every row declares a return type and a convention the generator knows.
    void CheckEveryRowDeclaresItsAbi()
    {
        for (const wxl::core::seams::Seam& seam : wxl::core::seams::Table())
        {
            const bool known = strcmp(seam.convention, "cdecl") == 0 ||
                               strcmp(seam.convention, "thiscall") == 0 ||
                               strcmp(seam.convention, "stdcall") == 0 ||
                               strcmp(seam.convention, "regparm") == 0;
            char what[160];
            snprintf(what, sizeof what, "%s declares %s %s %s", seam.name, seam.returns,
                     seam.convention, seam.params);
            Check(known && seam.returns[0] != 0, what);
        }
    }

    /// Event.DispatchUpdate: Before raises Update carrying the tick's own elapsed_sec.
    void CheckEventDispatchUpdate()
    {
        g_received.clear();
        wxl::seam::Event::DispatchUpdate::Before(nullptr, 1000u, 0.125f);

        const Received* update = Only(WXL_EV_Update);
        Check(update != nullptr, "Event.DispatchUpdate raises Update exactly once");
        if (!update) return;
        Check(update->As<wxl_update_args>().delta == 0.125f,
              "Update carries the seam's elapsed_sec as its delta");
    }

    /// World.Preload: the pair raises WorldEnter on the way in and WorldLeave only once a world
    /// has been entered, and neither when the loading screen is not being shown.
    void CheckWorldPreload()
    {
        const wxl_vec3 where{ 1.0f, 2.0f, 3.0f };
        g_mapId = 571;

        // A preload with no loading screen is a far-sight, cinematic or game-object preload.
        g_received.clear();
        wxl::seam::World::Preload::Before(&where, 0);
        wxl::seam::World::Preload::After(&where, 0);
        Check(g_received.empty(), "World.Preload raises nothing when show_loading is clear");

        // The first real preload enters a world; there is no world to leave yet.
        g_received.clear();
        wxl::seam::World::Preload::Before(&where, 1);
        wxl::seam::World::Preload::After(&where, 1);
        Check(Only(WXL_EV_WorldLeave) == nullptr, "the first world entry raises no WorldLeave");

        const Received* entered = Only(WXL_EV_WorldEnter);
        Check(entered != nullptr, "World.Preload raises WorldEnter once a world is loaded");
        if (entered)
        {
            const wxl_world_args args = entered->As<wxl_world_args>();
            Check(args.map_id == 571u, "WorldEnter carries the map the binding reports");
            Check(args.position.x == 1.0f && args.position.y == 2.0f && args.position.z == 3.0f,
                  "WorldEnter carries the position the seam was called with");
        }

        // The next one leaves the world just entered, naming it, and enters the new one.
        g_mapId = 0;
        g_received.clear();
        wxl::seam::World::Preload::Before(&where, 1);
        const Received* left = Only(WXL_EV_WorldLeave);
        Check(left != nullptr, "the next world entry raises WorldLeave first");
        if (left)
            Check(left->As<wxl_world_args>().map_id == 571u,
                  "WorldLeave names the map being left, not the one arriving");
        wxl::seam::World::Preload::After(&where, 1);
        Check(Only(WXL_EV_WorldEnter) != nullptr, "and WorldEnter after it");
    }

    /// ObjectMgr: InitObject's After tracks and announces; ObjDelete's Before announces and
    /// untracks, after which the handle a mod kept reads as absent; DestroyRemovedObject
    /// announces only what is still tracked.
    void CheckObjectMgr()
    {
        FakeObject boar;
        boar.guid     = 0xF130000000000123ull;
        boar.typeId   = WXL_OBJECT_UNIT;
        boar.entry    = 113;
        boar.position = { 4.0f, 5.0f, 6.0f };
        boar.health   = 90;

        g_received.clear();
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 1000u);

        const Received* created = Only(WXL_EV_ObjectCreate);
        Check(created != nullptr, "ObjectMgr.InitObject raises ObjectCreate exactly once");
        if (!created) return;
        const wxl_object_args args = created->As<wxl_object_args>();
        Check(args.object != WXL_HANDLE_NONE, "ObjectCreate carries a handle");
        Check(args.guid == boar.guid, "ObjectCreate carries the object's guid, read through the binding");
        Check(args.type == WXL_OBJECT_UNIT, "ObjectCreate carries the client's type id");

        const wxl_object_api* api = wxl::core::objects::Api();
        Check(api->valid(args.object) == WXL_TRUE, "the handle resolves through wxl.object");
        Check(api->guid(args.object) == boar.guid, "and reads the guid");
        uint32_t health = 0;
        Check(api->unit_health(args.object, &health) == WXL_OK && health == 90,
              "a unit read goes through the binding to the client's object");
        wxl_vec3 position{};
        Check(api->position(args.object, &position) == WXL_OK && position.y == 5.0f,
              "so does a position");
        Check(api->from_guid(boar.guid) == args.object, "the guid resolves back to the handle");

        // Announcing the same object again tracks it once.
        g_received.clear();
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 1001u);
        Check(Only(WXL_EV_ObjectCreate) != nullptr &&
                  Only(WXL_EV_ObjectCreate)->As<wxl_object_args>().object == args.object,
              "a second InitObject on the same object answers the same handle");

        // ObjDelete: announced while resident, then absent.
        g_received.clear();
        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
        const Received* destroyed = Only(WXL_EV_ObjectDestroy);
        Check(destroyed != nullptr, "ObjectMgr.ObjDelete raises ObjectDestroy exactly once");
        if (destroyed)
        {
            Check(destroyed->As<wxl_object_args>().object == args.object,
                  "ObjectDestroy carries the handle ObjectCreate did");
            Check(destroyed->As<wxl_object_args>().guid == boar.guid, "and its guid");
        }
        Check(api->valid(args.object) == WXL_FALSE, "after ObjDelete the handle no longer resolves");
        Check(api->unit_health(args.object, &health) == WXL_E_NOT_FOUND,
              "a read through the stale handle is absent, not the object's last value");
        Check(api->from_guid(boar.guid) == WXL_HANDLE_NONE, "and the guid resolves to nothing");

        // DestroyRemovedObject on what ObjDelete already let go: nothing.
        g_received.clear();
        wxl::seam::ObjectMgr::DestroyRemovedObject::Before(&boar);
        Check(g_received.empty(), "DestroyRemovedObject after ObjDelete raises nothing");

        // DestroyRemovedObject on a tracked object (the DestroyAllObjects path): once.
        FakeObject chest;
        chest.guid   = 0xF110000000000456ull;
        chest.typeId = WXL_OBJECT_GAMEOBJECT;
        wxl::seam::ObjectMgr::InitObject::After(&chest, nullptr, 1002u);
        g_received.clear();
        wxl::seam::ObjectMgr::DestroyRemovedObject::Before(&chest);
        Check(Only(WXL_EV_ObjectDestroy) != nullptr &&
                  Only(WXL_EV_ObjectDestroy)->As<wxl_object_args>().type == WXL_OBJECT_GAMEOBJECT,
              "DestroyRemovedObject on a tracked object raises ObjectDestroy once");
        Check(api->valid(Only(WXL_EV_ObjectDestroy)->As<wxl_object_args>().object) == WXL_FALSE,
              "and the handle is gone after it");
    }

    /// Unit: OnDeath's After raises UnitDeath for a tracked unit; the aura pair raises
    /// UnitAuraChanged with the spell and the direction; an untracked unit raises nothing.
    void CheckUnit()
    {
        FakeObject wolf;
        wolf.guid   = 0xF130000000000789ull;
        wolf.typeId = WXL_OBJECT_UNIT;
        wxl::seam::ObjectMgr::InitObject::After(&wolf, nullptr, 2000u);
        const wxl_handle handle = wxl::core::objects::HandleOf(&wolf);

        g_received.clear();
        wxl::seam::Unit::OnDeath::After(&wolf);
        Check(Only(WXL_EV_UnitDeath) != nullptr &&
                  Only(WXL_EV_UnitDeath)->As<wxl_unit_args>().unit == handle,
              "Unit.OnDeath raises UnitDeath once with the unit's handle");

        g_received.clear();
        wxl::seam::Unit::OnAuraAdded::After(&wolf, 3u, 0, nullptr, 8921);
        const Received* added = Only(WXL_EV_UnitAuraChanged);
        Check(added != nullptr, "Unit.OnAuraAdded raises UnitAuraChanged once");
        if (added)
        {
            const wxl_unit_aura_args args = added->As<wxl_unit_aura_args>();
            Check(args.unit == handle && args.spell_id == 8921u && args.removed == WXL_FALSE,
                  "with the unit, the spell, and removed clear");
        }

        g_received.clear();
        wxl::seam::Unit::OnAuraRemoved::After(&wolf, 3u, 0, nullptr, 8921);
        const Received* removed = Only(WXL_EV_UnitAuraChanged);
        Check(removed != nullptr && removed->As<wxl_unit_aura_args>().removed == WXL_TRUE,
              "Unit.OnAuraRemoved raises UnitAuraChanged once with removed set");

        FakeObject stranger;
        stranger.guid   = 0xF13000000000ABCDull;
        stranger.typeId = WXL_OBJECT_UNIT;
        g_received.clear();
        wxl::seam::Unit::OnDeath::After(&stranger);
        wxl::seam::Unit::OnAuraAdded::After(&stranger, 0u, 0, nullptr, 1);
        Check(g_received.empty(), "a unit the runtime never tracked raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&wolf);
    }

    /// Player: UpdateInventoryComponent raises EquipmentChanged when the slot's item id moved,
    /// with the new id, and nothing for an enchant-only refresh.
    void CheckPlayer()
    {
        FakeObject me;
        me.guid            = 0x0000000000000042ull;
        me.typeId          = WXL_OBJECT_PLAYER;
        me.visibleItems[15] = 19019;
        wxl::seam::ObjectMgr::InitObject::After(&me, nullptr, 3000u);
        const wxl_handle handle = wxl::core::objects::HandleOf(&me);

        FakeVisibleItem old;
        old.itemId = 0;
        g_received.clear();
        wxl::seam::Player::UpdateInventoryComponent::Before(&me, 15, &old);
        wxl::seam::Player::UpdateInventoryComponent::After(&me, 15, &old);
        const Received* equipped = Only(WXL_EV_EquipmentChanged);
        Check(equipped != nullptr, "Player.UpdateInventoryComponent raises EquipmentChanged once when the slot filled");
        if (equipped)
        {
            const wxl_equipment_args args = equipped->As<wxl_equipment_args>();
            Check(args.unit == handle && args.slot == 15u && args.item_id == 19019u,
                  "with the player's handle, the slot and the new item id");
        }

        old.itemId = 19019;
        g_received.clear();
        wxl::seam::Player::UpdateInventoryComponent::Before(&me, 15, &old);
        wxl::seam::Player::UpdateInventoryComponent::After(&me, 15, &old);
        Check(g_received.empty(), "the same id again (an enchant refresh) raises nothing");

        me.visibleItems[15] = 0;
        g_received.clear();
        wxl::seam::Player::UpdateInventoryComponent::Before(&me, 15, &old);
        wxl::seam::Player::UpdateInventoryComponent::After(&me, 15, &old);
        Check(Only(WXL_EV_EquipmentChanged) != nullptr &&
                  Only(WXL_EV_EquipmentChanged)->As<wxl_equipment_args>().item_id == 0u,
              "clearing the slot raises EquipmentChanged with an item id of 0");

        wxl::seam::ObjectMgr::ObjDelete::Before(&me);
    }

    /// GameUI: the target is announced once per change, with the previous and current handles,
    /// through SendTarget after Target's internal clear, and through a standalone ClearTarget.
    void CheckGameUI()
    {
        FakeObject rat;
        rat.guid   = 0xF130000000000AAAull;
        rat.typeId = WXL_OBJECT_UNIT;
        FakeObject bat;
        bat.guid   = 0xF130000000000BBBull;
        bat.typeId = WXL_OBJECT_UNIT;
        wxl::seam::ObjectMgr::InitObject::After(&rat, nullptr, 4000u);
        wxl::seam::ObjectMgr::InitObject::After(&bat, nullptr, 4001u);
        const wxl_handle ratHandle = wxl::core::objects::HandleOf(&rat);
        const wxl_handle batHandle = wxl::core::objects::HandleOf(&bat);

        // CGGameUI::Target(rat): ClearTarget(old, 0), the static written, SendTarget(rat).
        g_received.clear();
        wxl::seam::GameUI::ClearTarget::After(0, 0);
        g_targetGuid = rat.guid;
        wxl::seam::GameUI::SendTarget::After(rat.guid);
        const Received* first = Only(WXL_EV_TargetChanged);
        Check(first != nullptr, "targeting from nothing raises TargetChanged once");
        if (first)
            Check(first->As<wxl_target_args>().previous == WXL_HANDLE_NONE &&
                      first->As<wxl_target_args>().current == ratHandle,
                  "from the null handle to the target's");

        // CGGameUI::Target(bat): the internal clear is not announced, the change is, once.
        g_received.clear();
        g_targetGuid = 0;
        wxl::seam::GameUI::ClearTarget::After(rat.guid, 0);
        Check(g_received.empty(), "Target's own pre-clear (a2 clear) announces nothing");
        g_targetGuid = bat.guid;
        wxl::seam::GameUI::SendTarget::After(bat.guid);
        const Received* second = Only(WXL_EV_TargetChanged);
        Check(second != nullptr, "retargeting raises TargetChanged once");
        if (second)
            Check(second->As<wxl_target_args>().previous == ratHandle &&
                      second->As<wxl_target_args>().current == batHandle,
                  "from the previous target's handle to the new one's");

        // The summon-friend script: SendTarget with the static unchanged announces nothing.
        g_received.clear();
        wxl::seam::GameUI::SendTarget::After(rat.guid);
        Check(g_received.empty(), "SendTarget without a change of the static announces nothing");

        // A standalone ClearTarget (a2 set): announced, to the null handle.
        g_received.clear();
        g_targetGuid = 0;
        wxl::seam::GameUI::ClearTarget::After(bat.guid, 1);
        const Received* cleared = Only(WXL_EV_TargetChanged);
        Check(cleared != nullptr, "a standalone ClearTarget raises TargetChanged once");
        if (cleared)
            Check(cleared->As<wxl_target_args>().previous == batHandle &&
                      cleared->As<wxl_target_args>().current == WXL_HANDLE_NONE,
                  "from the target's handle to the null handle");

        wxl::seam::ObjectMgr::ObjDelete::Before(&rat);
        wxl::seam::ObjectMgr::ObjDelete::Before(&bat);
    }

    /// MapArea: PrepareChunk's Before raises TerrainChunkBuild with the map, the tile and the chunk.
    void CheckMapArea()
    {
        FakeArea area;
        area.tileX = 32;
        area.tileY = 48;
        g_mapId    = 0;

        g_received.clear();
        wxl::seam::MapArea::PrepareChunk::Before(&area, 3, 9);
        const Received* built = Only(WXL_EV_TerrainChunkBuild);
        Check(built != nullptr, "MapArea.PrepareChunk raises TerrainChunkBuild once");
        if (!built) return;
        const wxl_terrain_chunk_args args = built->As<wxl_terrain_chunk_args>();
        Check(args.map_id == 0u && args.tile_x == 32 && args.tile_y == 48,
              "with the map and the tile index read through the binding");
        Check(args.chunk_x == 3 && args.chunk_y == 9, "and the chunk the call named");
    }

    // --- wxl.script: what a mod registers, called back through the fake engine -----------------

    wxl_str Str(const char* text) { return wxl_str{text, static_cast<uint32_t>(strlen(text))}; }

    const wxl_script_api& Script() { return wxl::core::services::ScriptApi(); }

    int      g_greetCalls      = 0;
    uint32_t g_greetArgTypes[3]{};
    bool     g_greetWrongTypeMissed = false;

    /// Greet(n, name): answers "hi <name>", n * 2, true, nil; fails on anything else.
    void WXL_CALL Greet(wxl_script_call* call, void* user)
    {
        const wxl_script_api& api = Script();
        if (user) *static_cast<int*>(user) += 1;

        double  n = 0.0;
        wxl_str s{nullptr, 0};
        if (api.arg_count(call) != 2 || !api.arg_number(call, 0, &n) || !api.arg_string(call, 1, &s))
        {
            api.fail(call, Str("usage: Greet(number, string)"));
            return;
        }
        for (uint32_t i = 0; i < 3; ++i) g_greetArgTypes[i] = api.arg_type(call, i);
        wxl_bool flag = WXL_FALSE;
        wxl_str  text{nullptr, 0};
        g_greetWrongTypeMissed = api.arg_bool(call, 0, &flag) == WXL_FALSE &&
                                 api.arg_string(call, 0, &text) == WXL_FALSE &&
                                 api.arg_number(call, 1, &n) == WXL_FALSE && n == 21.0;

        char line[64];
        snprintf(line, sizeof line, "hi %.*s", static_cast<int>(s.size), s.data);
        api.push_string(call, Str(line));
        api.push_number(call, n * 2);
        api.push_bool(call, WXL_TRUE);
        api.push_nil(call);
    }

    /// Fails after pushing: the push must not reach the caller.
    void WXL_CALL Failing(wxl_script_call* call, void*)
    {
        Script().push_number(call, 1.0);
        Script().fail(call, Str("nope"));
    }

#if defined(_MSC_VER)
    void WXL_CALL Crashing(wxl_script_call*, void*)
    {
        volatile int* nowhere = nullptr;
        *nowhere              = 1;
    }
#endif

    /// A mod's ScriptVmReady handler: registers its function on every new context and keeps
    /// the answer in `user`.
    void WXL_CALL OnVmReadyRegister(void* user, const void*)
    {
        *static_cast<wxl_result*>(user) = Script().register_function("WxlHarnessGreet", &Greet, &g_greetCalls);
    }

    /// FrameScript: Initialize's After raises ScriptVmReady naming the context CGGameUI's state
    /// implies, Destroy's Before raises ScriptVmClosing naming the same one; the service takes
    /// registrations only between the two, drives calls through the engine, and starts over on
    /// the next context.
    void CheckFrameScript()
    {
        const wxl_script_api& api = Script();
        Check(g_host->Services().Get(WXL_SCRIPT_SERVICE, WXL_SCRIPT_SERVICE_VERSION) == &api,
              "wxl.script is published to mods");
        Check(api.struct_size == sizeof(wxl_script_api), "with the layout script.h declares");

        Check(api.register_function("WxlHarnessEarly", &Greet, nullptr) == WXL_E_FAILED,
              "register_function before any context exists is refused");
        Check(api.register_event("WXL_HARNESS_EARLY") == WXL_E_FAILED &&
                  api.fire_event("WXL_HARNESS_EARLY", nullptr, 0) == WXL_E_FAILED &&
                  api.run(Str("x = 1"), "early") == WXL_E_FAILED,
              "so are register_event, fire_event and run");

        wxl_result registered = WXL_E_FAILED;
        const wxl_subscription onReady =
            g_host->Events().Subscribe(WXL_EV_ScriptVmReady, &OnVmReadyRegister, &registered, 0);

        // The login-side context: CGGameUI is neither logging in nor initialised.
        g_vm.live       = true;
        g_uiInitialized = 0;
        g_uiLoggingIn   = 0;
        g_received.clear();
        wxl::seam::FrameScript::Initialize::After(1, 0);
        const Received* ready = Only(WXL_EV_ScriptVmReady);
        Check(ready != nullptr, "FrameScript.Initialize raises ScriptVmReady once");
        if (ready)
            Check(ready->As<wxl_script_vm_args>().context == WXL_SCRIPT_CONTEXT_GLUE &&
                      ready->As<wxl_script_vm_args>().native_state == &g_vm,
                  "it says glue while CGGameUI is neither logging in nor initialised, and carries the client's state");
        Check(api.context() == WXL_SCRIPT_CONTEXT_GLUE, "context() says the same");
        Check(registered == WXL_OK && g_vm.closures.count("WxlHarnessGreet") == 1,
              "a function registered from the ScriptVmReady handler is a global of that context");
        Check(api.register_function("WxlHarnessGreet", &Greet, nullptr) == WXL_E_INVALID,
              "registering a name again is refused");
        g_vm.clientGlobals.insert("GetTime");
        Check(api.register_function("GetTime", &Greet, nullptr) == WXL_E_INVALID,
              "a global the client set is not shadowed");
        Check(api.register_function(nullptr, &Greet, nullptr) == WXL_E_INVALID &&
                  api.register_function("", &Greet, nullptr) == WXL_E_INVALID &&
                  api.register_function("WxlHarnessNull", nullptr, nullptr) == WXL_E_INVALID,
              "a null name or function is refused");

        bool found = false;
        std::vector<FakeValue> results = CallGlobal("WxlHarnessGreet", {Number(21.0), String("bob")}, &found);
        Check(found && g_greetCalls == 1, "a call from script reaches the mod's function");
        Check(results.size() == 4 && results[0].type == WXL_VALUE_STRING && results[0].string == "hi bob" &&
                  results[1].type == WXL_VALUE_NUMBER && results[1].number == 42.0 &&
                  results[2].type == WXL_VALUE_BOOL && results[2].boolean &&
                  results[3].type == WXL_VALUE_NIL,
              "with its arguments, and brings back what it pushed, in order");
        Check(g_greetArgTypes[0] == WXL_VALUE_NUMBER && g_greetArgTypes[1] == WXL_VALUE_STRING &&
                  g_greetArgTypes[2] == WXL_VALUE_NIL,
              "arg_type names each argument's type and nil past the last");
        Check(g_greetWrongTypeMissed, "an argument of the wrong type is a miss, not a conversion");
        results = CallGlobal("WxlHarnessGreet", {Number(1.0)}, &found);
        Check(results.empty() && !g_vm.errors.empty() && g_vm.errors.back() == "usage: Greet(number, string)",
              "fail raises its message on the script side once the function returns");

        Check(api.register_function("WxlHarnessFail", &Failing, nullptr) == WXL_OK, "a second function registers");
        results = CallGlobal("WxlHarnessFail", {}, &found);
        Check(found && results.empty() && g_vm.errors.back() == "nope",
              "a function that fails after pushing hands the caller the error, not the results");

        Check(api.run(Str("print(1)"), "harness") == WXL_OK && !g_vm.ran.empty() &&
                  g_vm.ran.back() == "harness: print(1)",
              "run hands the chunk and its name to the engine");

        // Events: one declared before the client installs its table, one after.
        Check(api.register_event("WXL_HARNESS_EVENT") == WXL_OK && g_vm.eventTables.size() == 1 &&
                  g_vm.eventTables.back() == std::vector<std::string>{"WXL_HARNESS_EVENT"},
              "an event declared before the client's table is installed on its own");
        Check(api.register_event("WXL_HARNESS_EVENT") == WXL_OK && g_vm.eventTables.size() == 1,
              "declaring it again changes nothing");
        const char* clientNames[] = {"PLAYER_LOGIN", "PLAYER_LOGOUT"};
        wxl::seam::FrameScript::CreateEvents::After(clientNames, 2);
        Check(g_vm.eventTables.size() == 2 &&
                  g_vm.eventTables.back() == std::vector<std::string>{"PLAYER_LOGIN", "PLAYER_LOGOUT", "WXL_HARNESS_EVENT"},
              "once the client installs its table, FrameScript.CreateEvents installs it again with the declared event behind it");
        Check(api.register_event("WXL_HARNESS_LATE") == WXL_OK && g_vm.eventTables.size() == 3 &&
                  g_vm.eventTables.back() == std::vector<std::string>{"PLAYER_LOGIN", "PLAYER_LOGOUT", "WXL_HARNESS_EVENT", "WXL_HARNESS_LATE"},
              "an event declared afterwards is appended, keeping every index");
        const wxl_value payload[2] = {wxl_value{WXL_VALUE_NUMBER, {7.0}}, wxl_value{WXL_VALUE_STRING, {0.0}}};
        wxl_value       text       = payload[1];
        text.as.string             = Str("x");
        const wxl_value args[2]    = {payload[0], text};
        Check(api.fire_event("WXL_HARNESS_LATE", args, 2) == WXL_OK && g_vm.signals.size() == 1 &&
                  g_vm.signals[0].index == 3 && g_vm.signals[0].args.size() == 2 &&
                  g_vm.signals[0].args[0].number == 7.0 && g_vm.signals[0].args[1].string == "x" &&
                  g_vm.stack.empty(),
              "fire_event signals the event at its index with the values pushed, and leaves the stack as it found it");
        Check(api.fire_event("WXL_HARNESS_NOT_DECLARED", nullptr, 0) == WXL_E_NOT_FOUND,
              "firing an event nobody declared is not found");

#if defined(_MSC_VER)
        Check(api.register_function("WxlHarnessCrash", &Crashing, nullptr) == WXL_OK, "a faulting function registers like any other");
        results = CallGlobal("WxlHarnessCrash", {}, &found);
        Check(found && results.empty() && g_vm.errors.back().find("WxlHarnessCrash faulted (access violation") != std::string::npos,
              "a script function that faults answers the caller with an error naming the fault, and the harness is still here");
        Check(g_vm.closures.count("WxlHarnessCrash") == 0, "and the global is taken away");
        Check(g_vm.closures.count("WxlHarnessGreet") == 1, "the others stay");
#endif

        // Closing: announced with the context Ready named, then nothing is accepted.
        g_received.clear();
        wxl::seam::FrameScript::Destroy::Before();
        const Received* closing = Only(WXL_EV_ScriptVmClosing);
        Check(closing != nullptr && closing->As<wxl_script_vm_args>().context == WXL_SCRIPT_CONTEXT_GLUE,
              "FrameScript.Destroy raises ScriptVmClosing once, naming the context Ready did");
        g_vm.live = false;
        Check(api.register_function("WxlHarnessLate", &Greet, nullptr) == WXL_E_FAILED &&
                  api.fire_event("WXL_HARNESS_LATE", nullptr, 0) == WXL_E_FAILED,
              "after ScriptVmClosing nothing is accepted until the next context");
        {
            // A closure the dead context left behind, called anyway: an error, not the mod.
            g_greetCalls      = 0;
            FakeClosure stale = g_vm.closures["WxlHarnessGreet"];
            g_vm.closures.clear();
            g_vm.runningSlot = stale.slot;
            const int32_t pushed = stale.fn(&g_vm);
            Check(pushed == 0 && g_greetCalls == 0 && g_vm.errors.back().find("no longer provides") != std::string::npos,
                  "a closure outliving its registration answers an error instead of calling the mod");
        }
        g_received.clear();
        wxl::seam::FrameScript::Destroy::Before();
        Check(g_received.empty(), "a Destroy with no context announced raises nothing");

        // The game context: InitializeFrameXML sets m_loggingIn before it creates the context.
        g_vm.live     = true;
        g_uiLoggingIn = 1;
        registered    = WXL_E_FAILED;
        g_received.clear();
        wxl::seam::FrameScript::Initialize::After(1, 1);
        ready = Only(WXL_EV_ScriptVmReady);
        Check(ready != nullptr && ready->As<wxl_script_vm_args>().context == WXL_SCRIPT_CONTEXT_GAME,
              "the context created while CGGameUI is logging in is the game's");
        Check(registered == WXL_OK && g_vm.closures.count("WxlHarnessGreet") == 1,
              "the handler registered again on the new context, and the name was free: the old registration died with its context");
        Check(api.fire_event("WXL_HARNESS_LATE", nullptr, 0) == WXL_E_NOT_FOUND,
              "the events declared on the old context are gone too");
        results = CallGlobal("WxlHarnessGreet", {Number(2.0), String("ann")}, &found);
        Check(found && results.size() == 4 && results[0].string == "hi ann", "and the new registration works");

        // ReloadUI: Flush destroys and recreates with m_initialized still set.
        wxl::seam::FrameScript::Destroy::Before();
        g_vm.closures.clear();
        g_uiLoggingIn   = 0;
        g_uiInitialized = 1;
        g_received.clear();
        wxl::seam::FrameScript::Initialize::After(1, 0);
        ready = Only(WXL_EV_ScriptVmReady);
        Check(ready != nullptr && ready->As<wxl_script_vm_args>().context == WXL_SCRIPT_CONTEXT_GAME,
              "a context recreated while the game interface is initialised is the game's");

        // Back to the glue: ShutdownGame cleared both flags before the flush.
        wxl::seam::FrameScript::Destroy::Before();
        g_vm.closures.clear();
        g_uiInitialized = 0;
        g_received.clear();
        wxl::seam::FrameScript::Initialize::After(1, 0);
        ready = Only(WXL_EV_ScriptVmReady);
        Check(ready != nullptr && ready->As<wxl_script_vm_args>().context == WXL_SCRIPT_CONTEXT_GLUE,
              "a context recreated once the game interface is down is the glue's");

        wxl::seam::FrameScript::Destroy::Before();
        g_vm.live = false;
        g_host->Events().Unsubscribe(onReady);
    }

    // --- NetClient: the gated seams, and what a handler's answer does to the call -------------

    /// What the message handler below saw, and whether it is to stop what it is shown.
    struct MessageWatch
    {
        int      calls  = 0;
        uint32_t opcode = 0;
        uint32_t size   = 0;
        uint8_t  first  = 0;
        bool     stop   = false;
    };

    void WXL_CALL OnMessage(void* user, const void* args)
    {
        MessageWatch&           watch   = *static_cast<MessageWatch*>(user);
        const wxl_message_args& message = *static_cast<const wxl_message_args*>(args);
        ++watch.calls;
        watch.opcode = message.opcode;
        watch.size   = message.size;
        watch.first  = message.size ? static_cast<const uint8_t*>(message.data)[0] : uint8_t(0);
        if (watch.stop && message.suppressed) *message.suppressed = WXL_TRUE;
    }

    void CheckNetClient()
    {
        MessageWatch incoming;
        MessageWatch outgoing;
        const wxl_subscription in =
            g_host->Events().Subscribe(WXL_EV_MessageReceived, &OnMessage, &incoming, 0);
        const wxl_subscription out =
            g_host->Events().Subscribe(WXL_EV_MessageSent, &OnMessage, &outgoing, 0);

        // An incoming message: a uint16 opcode, then three bytes of body.
        const uint8_t received[] = { 0x34, 0x12, 0xAA, 0xBB, 0xCC };
        FakeStore     store{ received, sizeof received, 0 };

        void*    self    = reinterpret_cast<void*>(0x1000);
        void*    message = &store;
        uint32_t when    = 7;
        int32_t  unused  = 0;
        int32_t  answer  = -1;

        bool taken = wxl::seam::NetClient::ProcessMessage::Gate(&answer, &self, &when, &message,
                                                                &unused);
        Check(!taken, "a message nobody stops lets the client's own dispatch run");
        Check(incoming.calls == 1 && incoming.opcode == 0x1234u,
              "the opcode is the uint16 the server wrote");
        Check(incoming.size == 3u && incoming.first == 0xAA,
              "and the body is what follows the opcode");
        Check(store.read == 0u, "reading the message left the store's cursor where it was");

        incoming.stop = true;
        taken = wxl::seam::NetClient::ProcessMessage::Gate(&answer, &self, &when, &message,
                                                           &unused);
        Check(taken && answer == 0,
              "a handler that suppresses it takes the call, and the client answers 0");

        // An outgoing message: a uint32 opcode, then two bytes of body.
        const uint8_t sent[] = { 0x78, 0x56, 0x00, 0x00, 0x11, 0x22 };
        FakeStore     outStore{ sent, sizeof sent, 0 };
        message = &outStore;

        Check(!wxl::seam::NetClient::Send::Gate(&self, &message), "a message nobody stops is sent");
        Check(outgoing.calls == 1 && outgoing.opcode == 0x5678u,
              "the opcode is the uint32 the client wrote");
        Check(outgoing.size == 2u && outgoing.first == 0x11, "and the body is what follows it");

        outgoing.stop = true;
        Check(wxl::seam::NetClient::Send::Gate(&self, &message),
              "a handler that suppresses it stops the send");

        // A store that has been reset carries a cursor past its end: there is no message in it.
        outgoing.stop = false;
        FakeStore reset{ sent, 0, 0xFFFFFFFFu };
        message = &reset;
        const int before = outgoing.calls;
        Check(!wxl::seam::NetClient::Send::Gate(&self, &message),
              "a reset store is not a message, and the call goes through untouched");

        FakeStore stub{ sent, 2, 0 };
        message = &stub;
        Check(!wxl::seam::NetClient::Send::Gate(&self, &message),
              "neither is a store too short to name an opcode");
        Check(outgoing.calls == before, "and neither of those raised anything");

        g_host->Events().Unsubscribe(in);
        g_host->Events().Unsubscribe(out);
    }

    // --- Input: a gate that can answer a window message in the client's place -----------------

    bool g_swallow = false;
    int  g_messages = 0;

    void WXL_CALL OnInput(void*, const void* args)
    {
        const wxl_input_args& input = *static_cast<const wxl_input_args*>(args);
        ++g_messages;
        if (g_swallow && input.handled) *input.handled = WXL_TRUE;
    }

    void CheckInput()
    {
        const wxl_subscription sub =
            g_host->Events().Subscribe(WXL_EV_Input, &OnInput, nullptr, 0);

        void*     window  = reinterpret_cast<void*>(0x2000);
        uint32_t  message = 0x0100; // WM_KEYDOWN
        uintptr_t wparam  = 0x41;
        intptr_t  lparam  = 1;
        intptr_t  answer  = -1;

        g_swallow = false;
        Check(!wxl::seam::Input::WindowProc::Gate(&answer, &window, &message, &wparam, &lparam),
              "a message nobody claims reaches the client's own window procedure");
        Check(g_messages == 1, "and the handler saw it once");

        g_swallow = true;
        Check(wxl::seam::Input::WindowProc::Gate(&answer, &window, &message, &wparam, &lparam),
              "a handler that writes handled takes the call away from the client");
        Check(answer == 0, "and the window procedure answers 0, as one that handled a message does");

        g_host->Events().Unsubscribe(sub);
    }

    // --- wxl.data: a client database served from a mod file instead of the client's ----------

    void CheckDataService()
    {
        g_files.clear();
        g_files.push_back(FakeFile{"Mods\\mymod\\Spell.dbc", "WDBC-mine", 0, false});
        wxl::core::services::data::Clear();

        const wxl_data_api& api = wxl::core::services::DataApi();
        const wxl_str mine = Str("Mods\\mymod\\Spell.dbc");

        Check(api.serve_table(Str("Spell.dbc"), Str("Mods\\mymod\\Nope.dbc")) == WXL_E_NOT_FOUND,
              "a table cannot be served from a file the client cannot read");
        Check(api.serve_table(Str("Spell.dbc"), mine) == WXL_OK, "and can be from one it can");
        Check(api.serve_table(Str("DBFilesClient\\Spell.dbc"), mine) == WXL_E_FAILED,
              "the same table under its full name is the same table, and is already taken");

        // What the gate answers for the path the client is about to open, written where the
        // seam keeps it: the service hands over a copy, never a pointer into its own tables.
        char        redirect[512] = {};
        const char* full = "DBFilesClient\\Spell.dbc";
        Check(wxl::core::services::data::ServedPath(full, strlen(full), redirect, sizeof redirect)
                  && strcmp(redirect, "Mods\\mymod\\Spell.dbc") == 0,
              "so the client opens the mod's file instead of its own");

        const char* other = "DBFilesClient\\Item.dbc";
        Check(!wxl::core::services::data::ServedPath(other, strlen(other), redirect,
                                                    sizeof redirect),
              "a database nobody serves is opened as the client asked");
        const char* texture = "World\\Maps\\Azeroth.wdt";
        Check(!wxl::core::services::data::ServedPath(texture, strlen(texture), redirect,
                                                    sizeof redirect),
              "and a file that is not a database is not even looked up");
        const char* upper = "DBFilesClient\\CHARTITLES.DBC";
        Check(!wxl::core::services::data::ServedPath(upper, strlen(upper), redirect,
                                                    sizeof redirect)
                  && api.is_loaded(Str("CharTitles.dbc")) == WXL_TRUE,
              "a database named in capitals is the same database, suffix and all");

        Check(api.is_loaded(Str("Spell.dbc")) == WXL_TRUE
                  && api.is_loaded(Str("Talent.dbc")) == WXL_FALSE,
              "a table the client has read is known to have been read");
        Check(api.serve_table(Str("Item.dbc"), mine) == WXL_OK,
              "serving one the client read already is allowed");
        Check(api.restore_table(Str("Item.dbc")) == WXL_OK
                  && api.served_from(Str("Item.dbc")).size == 0,
              "and giving a table back leaves the client's own file");

        wxl::core::services::data::Clear();
        g_files.clear();
    }

    // --- Sound: a gate that can keep a sound from starting -------------------------------------

    bool g_hushSound = false;
    int  g_sounds    = 0;
    uint32_t g_lastKit = 0;

    void WXL_CALL OnSound(void*, const void* args)
    {
        const wxl_sound_args& sound = *static_cast<const wxl_sound_args*>(args);
        ++g_sounds;
        g_lastKit = sound.sound_id;
        if (g_hushSound && sound.suppressed) *sound.suppressed = WXL_TRUE;
    }

    void CheckSound()
    {
        const wxl_subscription sub =
            g_host->Events().Subscribe(WXL_EV_SoundPlay, &OnSound, nullptr, 0);

        int32_t     kit        = 1234;
        const void* position   = nullptr;
        void*       object     = nullptr;
        void*       properties = nullptr;
        uint32_t    advanced   = 0;
        uint32_t*   soundId    = nullptr;
        int32_t     distance   = 1;
        int32_t     keep       = 0;
        int32_t     answer     = -1;

        g_hushSound = false;
        Check(!wxl::seam::Sound::PlaySoundKit::Gate(&answer, &kit, &position, &object,
                                                     &properties, &advanced, &soundId,
                                                     &distance, &keep),
              "a sound nobody stops is played by the client");
        Check(g_sounds == 1 && g_lastKit == 1234u,
              "and the handler is told which kit, whether it was named by id or by path");

        g_hushSound = true;
        Check(wxl::seam::Sound::PlaySoundKit::Gate(&answer, &kit, &position, &object,
                                                    &properties, &advanced, &soundId,
                                                    &distance, &keep),
              "a handler that suppresses it takes the call");
        Check(answer == 17, "and the client is answered as it answers itself when sound is off");

        g_host->Events().Unsubscribe(sub);
    }

    // --- Storage: a gate that changes what the client is about to open ------------------------

    const char* g_answerWith = nullptr;

    void WXL_CALL OnFileOpen(void*, const void* args)
    {
        const wxl_file_open_args& open = *static_cast<const wxl_file_open_args*>(args);
        if (!g_answerWith || !open.redirect || !open.redirect_capacity) return;
        const size_t length = strlen(g_answerWith);
        if (length + 1 > open.redirect_capacity) return;
        memcpy(open.redirect, g_answerWith, length + 1);
    }

    void CheckStorage()
    {
        const wxl_subscription sub =
            g_host->Events().Subscribe(WXL_EV_FileOpen, &OnFileOpen, nullptr, 0);

        void*       archive = nullptr;
        const char* asked   = "World\\Maps\\Azeroth\\Azeroth.wdt";
        const char* name    = asked;
        uint32_t    flags   = 0;
        void**      file    = nullptr;
        int32_t     answer  = -1;

        g_answerWith = nullptr;
        Check(!wxl::seam::Storage::OpenFile::Gate(&answer, &archive, &name, &flags, &file),
              "a file nobody claims is opened by the client, as asked");
        Check(name == asked, "and the name it was handed is untouched");

        g_answerWith = "Mods\\example\\Azeroth.wdt";
        Check(!wxl::seam::Storage::OpenFile::Gate(&answer, &archive, &name, &flags, &file),
              "a redirected file is still opened by the client: the gate does not take the call");
        Check(name != asked && strcmp(name, g_answerWith) == 0,
              "but what it opens is the path the handler wrote");

        // The redirected path has to outlive the gate: the client opens it afterwards.
        const char* kept = name;
        g_answerWith     = nullptr;
        name             = asked;
        wxl::seam::Storage::OpenFile::Gate(&answer, &archive, &name, &flags, &file);
        Check(kept[0] == 0,
              "a redirect lives in one buffer per thread, so it lasts until that thread opens "
              "the next file and no longer");

        g_host->Events().Unsubscribe(sub);
    }

    /// Client.Destroy: the last thing a mod is told, and the point at which what a mod left
    /// jumping into its own code is taken away before that code is freed.
    // --- Render: the slots of a frame --------------------------------------------------------
    //
    // FrameBegin and FrameEnd come from two seams, and the client presents more often than it paints
    // a frame: its loading screen presents frames of its own, and a paint with nothing to draw into
    // presents anyway. What a mod is promised is pairs, in the client's order, and nothing outside
    // a frame. Each way the client can break that is driven here.

    void CheckRender()
    {
        wxl::core::frame::Reset();
        int top   = 0; // CSimpleTop's `this`; the seam does not read it
        int world = 0; // the world frame the world pass is handed

        auto paint = [] { wxl::seam::Scrn::OnPaint::Before(nullptr, nullptr); };
        auto present = []
        {
            wxl::seam::Gx::ScenePresent::Before();
        };

        g_gxContext = false;
        g_received.clear();
        paint();
        present();
        Check(g_received.empty(), "a paint with no device context opens no frame, and ends none");

        g_gxContext = true;
        g_gxVisible = false;
        g_received.clear();
        paint();
        present();
        Check(g_received.empty(), "nor does one whose window is minimised");
        g_gxVisible = true;

        g_received.clear();
        present();
        Check(g_received.empty(), "a present no paint opened a frame for, as the loading screen's, "
                                  "raises nothing");

        g_received.clear();
        wxl::seam::WorldFrame::RenderWorld::Before(&world);
        wxl::seam::WorldFrame::RenderWorld::After(&world);
        wxl::seam::SimpleTop::OnLayerRender::After(&top);
        Check(g_received.empty(), "the world pass and the interface's end raise nothing outside a frame");

        // A frame in the client's own order: the paint, the interface drawing with the world inside
        // it, the present. A frame announces no stage: by the time the client paints one, the
        // script context already exists, so the graphics stage is the device's to announce.
        wxl::core::stages::Restart();
        wxl::core::stages::Reach(WXL_STAGE_STORAGE);
        g_received.clear();
        paint();
        Check(wxl::core::stages::Reached() == WXL_STAGE_STORAGE, "a frame announces no stage");
        wxl::seam::WorldFrame::RenderWorld::Before(&world);
        wxl::seam::WorldFrame::RenderWorld::After(&world);
        wxl::seam::SimpleTop::OnLayerRender::After(&top);
        present();

        const wxl_event order[] = {WXL_EV_FrameBegin, WXL_EV_WorldRenderBegin, WXL_EV_WorldRenderEnd,
                                   WXL_EV_UiRenderEnd, WXL_EV_FrameEnd};
        bool inOrder = g_received.size() == 5;
        for (size_t i = 0; inOrder && i < 5; ++i) inOrder = g_received[i].event == order[i];
        Check(inOrder, "a frame raises FrameBegin, the world pass, the interface's end and "
                       "FrameEnd, in that order");

        uint64_t first = 0;
        if (inOrder)
        {
            first     = g_received[0].As<wxl_frame_args>().index;
            bool same = first != 0;
            for (const Received& received : g_received)
                same = same && received.As<wxl_frame_args>().index == first;
            Check(same, "and every slot of it carries that frame's index");
        }

        g_received.clear();
        present();
        Check(g_received.empty(), "a frame ends once: a second present after it raises nothing");

        g_received.clear();
        paint();
        paint();
        Check(Count(WXL_EV_FrameBegin) == 1, "a paint reached again with its frame still open "
                                             "opens no second one");
        present();
        Check(Count(WXL_EV_FrameEnd) == 1, "and that frame closes once");
        const Received* next = Only(WXL_EV_FrameBegin);
        Check(next && next->As<wxl_frame_args>().index == first + 1, "the next frame counts up by one");

        // The device going and coming back carries the size it renders at.
        g_gxWidth  = 1280;
        g_gxHeight = 720;
        g_received.clear();
        wxl::seam::Gx::ReleaseD3dResources::Before(&top, 0);
        const Received* lost = Only(WXL_EV_SwapchainLost);
        Check(lost && lost->As<wxl_swapchain_args>().width == 1280
                  && lost->As<wxl_swapchain_args>().height == 720,
              "SwapchainLost carries the size that is about to go away");

        g_gxWidth  = 2560;
        g_gxHeight = 1440;
        g_received.clear();
        wxl::seam::Gx::NotifyOnDeviceRestored::Before(&top);
        const Received* back = Only(WXL_EV_SwapchainRestored);
        Check(back && back->As<wxl_swapchain_args>().width == 2560
                  && back->As<wxl_swapchain_args>().height == 1440,
              "and SwapchainRestored the size it came back at");

        // With no device at all there is no size to report, and none is made up.
        g_gxDevice = false;
        g_received.clear();
        wxl::seam::Gx::ReleaseD3dResources::Before(&top, 0);
        const Received* gone = Only(WXL_EV_SwapchainLost);
        Check(gone && gone->As<wxl_swapchain_args>().width == 0
                  && gone->As<wxl_swapchain_args>().height == 0,
              "a device torn down reports no size rather than an invented one");
        g_gxDevice = true;
        g_gxWidth  = 1920;
        g_gxHeight = 1080;
        wxl::core::frame::Reset();

        // The graphics stage belongs to the device being made, between storage and script, which
        // is the order InitializeGlobal and InitializeClient bring them up in.
        wxl::core::stages::Restart();
        wxl::core::stages::Reach(WXL_STAGE_STORAGE);
        g_gxDevice = false;
        wxl::seam::ConsoleDevice::Initialize::After("World of Warcraft", true, true);
        Check(wxl::core::stages::Reached() == WXL_STAGE_STORAGE,
              "a device bring-up that left no device announces no graphics stage");
        g_gxDevice = true;
        wxl::seam::ConsoleDevice::Initialize::After("World of Warcraft", true, true);
        Check(wxl::core::stages::Reached() == WXL_STAGE_GRAPHICS,
              "one that left a device announces the graphics stage");
        wxl::core::stages::Reach(WXL_STAGE_SCRIPT);
        Check(wxl::core::stages::Reached() == WXL_STAGE_SCRIPT, "and script still follows it");
    }

    void CheckClientDestroy()
    {
        namespace stages = wxl::core::stages;
        stages::Restart();
        stages::Reach(WXL_STAGE_READY);
        g_released = 0;

        wxl::seam::Client::Destroy::Before();
        Check(stages::Reached() == WXL_STAGE_SHUTDOWN,
              "Client.Destroy announces the shutdown stage");
        Check(g_released == 1,
              "and takes back every detour a mod left behind, before its binary is freed");

        stages::Restart();
    }

    // --- NetClient: the connection coming up and going away ------------------------------------

    void CheckNetConnection()
    {
        g_received.clear();
        wxl::seam::NetClient::HandleConnect::After(1, nullptr);
        Check(Only(WXL_EV_Connected) != nullptr,
              "it is raised once the client has moved itself into the connected state");

        g_received.clear();
        wxl::seam::NetClient::HandleDisconnect::Before(nullptr);
        Check(Only(WXL_EV_Disconnected) != nullptr,
              "Disconnected is raised before the client tears the connection down");
    }

    // --- wxl.storage: the client's files, as a mod reads them ---------------------------------

    void CheckStorageService()
    {
        g_files.clear();
        g_files.push_back(FakeFile{"World\\Maps\\Azeroth\\Azeroth.wdt", "MVER....hello", 0, false});
        g_opens  = 0;
        g_closes = 0;

        const wxl_storage_api& api = wxl::core::services::StorageApi();
        const wxl_str          path{g_files[0].name.c_str(),
                                    static_cast<uint32_t>(g_files[0].name.size())};
        const wxl_str          missing = Str("World\\Maps\\Nowhere.wdt");

        Check(api.exists(path) == WXL_TRUE && api.exists(missing) == WXL_FALSE,
              "wxl.storage answers for a name the client has and one it does not");

        uint64_t size = 0;
        Check(api.size(path, &size) == WXL_OK && size == g_files[0].bytes.size(),
              "the length of a file is its length");
        Check(api.size(missing, &size) == WXL_E_NOT_FOUND,
              "and a file that is not there is not found, rather than empty");

        // A buffer too small answers what it would have taken, which is how the facade sizes.
        char     small[4] = {};
        uint64_t needed   = 0;
        Check(api.read(path, small, sizeof small, &needed) == WXL_E_INVALID
                  && needed == g_files[0].bytes.size(),
              "a buffer too small is refused, and says what would have fitted");

        std::vector<char> whole(static_cast<size_t>(size));
        uint64_t          read = 0;
        Check(api.read(path, whole.data(), whole.size(), &read) == WXL_OK && read == size
                  && std::string(whole.begin(), whole.end()) == g_files[0].bytes,
              "and a buffer the right size gets the whole file");

        // Streaming: a handle, a read at an offset, a close.
        const wxl_handle file = api.open(path);
        Check(file != WXL_HANDLE_NONE, "a file opens");

        char     part[5] = {};
        uint64_t got     = 0;
        Check(api.read_at(file, 8, part, 5, &got) == WXL_OK && got == 5
                  && std::string(part, 5) == "hello",
              "and reads where it is asked to, not where it last was");

        api.close(file);
        Check(g_closes == g_opens, "a closed file is closed on the client's side too");
        Check(api.read_at(file, 0, part, 1, &got) == WXL_E_NOT_FOUND,
              "a handle kept past its close answers gone, not whatever took its place");

        // What a mod leaves open is closed for it.
        const wxl_handle leaked = api.open(path);
        Check(leaked != WXL_HANDLE_NONE, "a mod opens a file and forgets it");
        wxl::core::services::storage::CloseEverything();
        Check(g_closes == g_opens, "and the runtime closes it on the way out");
        Check(api.read_at(leaked, 0, part, 1, &got) == WXL_E_NOT_FOUND,
              "its handle stops resolving with it");

        g_files.clear();
    }

    // --- Spell: casts read off the server's messages, and the two requests a mod can refuse ---

    /// A message body, built byte by byte the way the server writes one.
    struct Body
    {
        std::vector<uint8_t> bytes;

        Body& U8(uint8_t value)
        {
            bytes.push_back(value);
            return *this;
        }

        Body& U32(uint32_t value)
        {
            for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
            return *this;
        }

        Body& U64(uint64_t value)
        {
            for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
            return *this;
        }

        /// A guid packed as the server packs it: the mask, then the non-zero bytes.
        Body& Packed(uint64_t guid)
        {
            uint8_t mask = 0;
            for (int i = 0; i < 8; ++i)
                if ((guid >> (i * 8)) & 0xFF) mask |= static_cast<uint8_t>(1 << i);
            bytes.push_back(mask);
            for (int i = 0; i < 8; ++i)
                if (mask & (1 << i)) bytes.push_back(static_cast<uint8_t>(guid >> (i * 8)));
            return *this;
        }

        FakeStore Store() const { return FakeStore{ bytes.data(), static_cast<uint32_t>(bytes.size()), 0 }; }
    };

    constexpr uint32_t kMsgSpellStart   = 0x131;
    constexpr uint32_t kMsgSpellGo      = 0x132;
    constexpr uint32_t kMsgSpellFailure = 0x133;

    /// The hit and miss lists of a SpellCastGo, copied out while the args were valid.
    struct GoWatch
    {
        std::vector<uint8_t> hits;
        std::vector<uint8_t> misses;
        std::vector<uint8_t> reasons;
    };

    void WXL_CALL OnSpellGo(void* user, const void* args)
    {
        GoWatch&                   watch = *static_cast<GoWatch*>(user);
        const wxl_spell_cast_args& cast  = *static_cast<const wxl_spell_cast_args*>(args);
        const uint8_t* hits    = static_cast<const uint8_t*>(cast.hits);
        const uint8_t* misses  = static_cast<const uint8_t*>(cast.misses);
        const uint8_t* reasons = static_cast<const uint8_t*>(cast.miss_reasons);
        watch.hits.assign(hits, hits + (hits ? cast.hits_size : 0));
        watch.misses.assign(misses, misses + (misses ? cast.misses_size : 0));
        watch.reasons.assign(reasons, reasons + (reasons ? cast.miss_count : 0));
    }

    /// Eight bytes as a little-endian guid.
    uint64_t GuidAt(const std::vector<uint8_t>& bytes, size_t at)
    {
        uint64_t guid = 0;
        if (bytes.size() < at + 8) return 0;
        for (int i = 0; i < 8; ++i) guid |= static_cast<uint64_t>(bytes[at + i]) << (i * 8);
        return guid;
    }

    void CheckSpell()
    {
        FakeObject mage;
        mage.guid   = 0x0000000000000ABCull;
        mage.typeId = WXL_OBJECT_PLAYER;
        FakeObject boar;
        boar.guid   = 0xF130000000000123ull;
        boar.typeId = WXL_OBJECT_UNIT;
        wxl::seam::ObjectMgr::InitObject::After(&mage, nullptr, 3000u);
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 3001u);
        const wxl_handle mageHandle = wxl::core::objects::HandleOf(&mage);
        const wxl_handle boarHandle = wxl::core::objects::HandleOf(&boar);
        const uint64_t   stranger   = 0xF13000000000FFFFull;

        // SMSG_SPELL_START: the shared header, the cast time, then a unit target.
        Body start;
        start.Packed(0).Packed(mage.guid).U8(7).U32(133).U32(0x2).U32(1500).U32(0x2).Packed(boar.guid);
        FakeStore store = start.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_Group_30::Before(nullptr, kMsgSpellStart, 0, &store);
        const Received* started = Only(WXL_EV_SpellCastStart);
        Check(started != nullptr && Count(WXL_EV_SpellCastGo) == 0,
              "Spell.Packet_Group_30 raises SpellCastStart once for SMSG_SPELL_START");
        if (started)
        {
            const wxl_spell_cast_args args = started->As<wxl_spell_cast_args>();
            Check(args.caster == mageHandle && args.caster_guid == mage.guid,
                  "with the caster's handle and guid read off the packed guid");
            Check(args.target == boarHandle && args.target_guid == boar.guid && args.target_flags == 0x2,
                  "the object target the target flags name");
            Check(args.spell_id == 133 && args.cast_count == 7 && args.cast_flags == 0x2 &&
                      args.cast_time == 1500 && args.cast_item == 0,
                  "the spell, the count, the flags, the cast time, and no cast item");
            Check(args.hit_count == 0 && args.miss_count == 0 && args.hits == nullptr &&
                      args.misses == nullptr && args.miss_reasons == nullptr,
                  "and no hits or misses on a start");
        }
        Check(store.read == 0u, "reading the message left the store's cursor where it was");

        // SMSG_SPELL_GO: the header, the server's time, one hit, two misses, the first of them
        // reflected, no target.
        Body go;
        go.Packed(0x4000000000000042ull).Packed(stranger).U8(2).U32(5176).U32(0x800).U32(9999)
            .U8(1).U64(boar.guid).U8(2).U64(mage.guid).U8(11).U8(0).U64(stranger).U8(3).U32(0);
        store = go.Store();
        g_received.clear();
        GoWatch                watch;
        const wxl_subscription goSub =
            g_host->Events().Subscribe(WXL_EV_SpellCastGo, &OnSpellGo, &watch, 0);
        wxl::seam::Spell::Packet_Group_30::Before(nullptr, kMsgSpellGo, 0, &store);
        g_host->Events().Unsubscribe(goSub);
        const Received* went = Only(WXL_EV_SpellCastGo);
        Check(went != nullptr && Count(WXL_EV_SpellCastStart) == 0,
              "Spell.Packet_Group_30 raises SpellCastGo once for SMSG_SPELL_GO");
        if (went)
        {
            const wxl_spell_cast_args args = went->As<wxl_spell_cast_args>();
            Check(args.caster == WXL_HANDLE_NONE && args.caster_guid == stranger,
                  "a caster the client does not hold has no handle but keeps its guid");
            Check(args.hit_count == 1 && args.miss_count == 2 && args.target_guid == 0 &&
                      args.target == WXL_HANDLE_NONE && args.cast_item == 0x4000000000000042ull,
                  "the hits and misses are counted past a reflect result, and no target is none");
            Check(args.spell_id == 5176 && args.cast_time == 0, "and a go carries no cast time");
            Check(args.hits_size == 8 && watch.hits.size() == 8 && GuidAt(watch.hits, 0) == boar.guid,
                  "the hits are handed over as the guids the server listed");
            Check(args.misses_size == 16 && watch.misses.size() == 16 &&
                      GuidAt(watch.misses, 0) == mage.guid && GuidAt(watch.misses, 8) == stranger,
                  "the misses as the guids the server listed, the reflect result left out");
            Check(watch.reasons.size() == 2 && watch.reasons[0] == 11 && watch.reasons[1] == 3,
                  "and one reason per miss, in the same order");
        }

        // What raises nothing: another opcode, a message cut short.
        g_received.clear();
        store = start.Store();
        wxl::seam::Spell::Packet_Group_30::Before(nullptr, kMsgSpellFailure, 0, &store);
        FakeStore cut{ start.bytes.data(), 6, 0 };
        wxl::seam::Spell::Packet_Group_30::Before(nullptr, kMsgSpellStart, 0, &cut);
        Check(g_received.empty(), "an opcode the handler does not serve, or a truncated message, raises nothing");

        // SMSG_SPELL_FAILURE: the caster, the count, the spell, the reason.
        Body failure;
        failure.Packed(boar.guid).U8(3).U32(8921).U8(24);
        store = failure.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_SPELL_FAILURE::Before(nullptr, kMsgSpellFailure, 0, &store);
        const Received* interrupted = Only(WXL_EV_SpellCastInterrupted);
        Check(interrupted != nullptr, "Spell.Packet_SMSG_SPELL_FAILURE raises SpellCastInterrupted once");
        if (interrupted)
        {
            const wxl_spell_fail_args args = interrupted->As<wxl_spell_fail_args>();
            Check(args.caster == boarHandle && args.caster_guid == boar.guid && args.spell_id == 8921 &&
                      args.cast_count == 3 && args.reason == 24,
                  "with the caster, the spell, the count and the reason");
            Check(args.custom_error == -1 && args.custom_arg == -1, "and no custom error");
        }

        // Spell_C_SpellFailed: the pending cast, read through the binding.
        FakePendingCast pending;
        pending.caster  = mage.guid;
        pending.spellId = 116;
        pending.count   = 9;
        g_received.clear();
        wxl::seam::Spell::SpellFailed::Before(&pending, nullptr, 67, 30, 1, 1);
        const Received* failed = Only(WXL_EV_SpellCastFailed);
        Check(failed != nullptr, "Spell.SpellFailed raises SpellCastFailed once");
        if (failed)
        {
            const wxl_spell_fail_args args = failed->As<wxl_spell_fail_args>();
            Check(args.caster == mageHandle && args.caster_guid == mage.guid && args.spell_id == 116 &&
                      args.cast_count == 9 && args.reason == 67,
                  "with the cast's caster, spell and count, and the reason");
            Check(args.custom_error == 30 && args.custom_arg == 1, "and the custom error as passed");
        }
        g_received.clear();
        wxl::seam::Spell::SpellFailed::Before(nullptr, nullptr, 67, -1, -1, 0);
        Check(g_received.empty(), "a null cast raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&mage);
        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
    }

    bool g_refuse = false;
    int  g_requests = 0;
    wxl_spell_request_args g_lastSpellRequest{};
    wxl_item_use_args      g_lastItemUse{};

    void WXL_CALL OnSpellRequest(void*, const void* args)
    {
        const wxl_spell_request_args& request = *static_cast<const wxl_spell_request_args*>(args);
        ++g_requests;
        g_lastSpellRequest = request;
        if (g_refuse && request.refused) *request.refused = WXL_TRUE;
    }

    void WXL_CALL OnItemUse(void*, const void* args)
    {
        const wxl_item_use_args& use = *static_cast<const wxl_item_use_args*>(args);
        ++g_requests;
        g_lastItemUse = use;
        if (g_refuse && use.refused) *use.refused = WXL_TRUE;
    }

    void CheckSpellRequest()
    {
        const wxl_subscription sub =
            g_host->Events().Subscribe(WXL_EV_SpellCastRequest, &OnSpellRequest, nullptr, 0);

        FakeObject player;
        player.guid   = 0x0000000000000ABCull;
        player.typeId = WXL_OBJECT_PLAYER;
        FakeObject potion;
        potion.guid   = 0x4000000000000042ull;
        potion.typeId = WXL_OBJECT_ITEM;
        wxl::seam::ObjectMgr::InitObject::After(&player, nullptr, 4000u);
        wxl::seam::ObjectMgr::InitObject::After(&potion, nullptr, 4001u);

        uint8_t     answer    = 1;
        void*       caster    = &player;
        int32_t     spellId   = 133;
        const void* item      = &potion;
        uint64_t    target    = 0xF130000000000123ull;
        void*       cast      = nullptr;
        uint8_t     confirmed = 0;

        g_refuse   = false;
        g_requests = 0;
        Check(!wxl::seam::Spell::CastSpell::Gate(&answer, &caster, &spellId, &item, &target, &cast,
                                                 &confirmed),
              "a cast nobody refuses is left to the client");
        Check(g_requests == 1 && g_lastSpellRequest.caster == wxl::core::objects::HandleOf(&player) &&
                  g_lastSpellRequest.spell_id == 133 &&
                  g_lastSpellRequest.item == wxl::core::objects::HandleOf(&potion) &&
                  g_lastSpellRequest.target_guid == target && g_lastSpellRequest.confirmed == WXL_FALSE,
              "and the handler is told the caster, the spell, the item and the target");

        g_refuse  = true;
        confirmed = 1;
        item      = nullptr;
        Check(wxl::seam::Spell::CastSpell::Gate(&answer, &caster, &spellId, &item, &target, &cast,
                                                &confirmed),
              "a handler that refuses it takes the call");
        Check(answer == 0 && g_lastSpellRequest.item == WXL_HANDLE_NONE &&
                  g_lastSpellRequest.confirmed == WXL_TRUE,
              "the client is answered false; no item is no handle, and the second ask says so");

        g_host->Events().Unsubscribe(sub);

        // CGItem_C::UseItem: the item itself is `this`, the target comes by address.
        const wxl_subscription use =
            g_host->Events().Subscribe(WXL_EV_ItemUseRequest, &OnItemUse, nullptr, 0);
        int32_t         result     = 1;
        void*           self       = &potion;
        const uint64_t* targetPtr  = &target;
        int32_t         confirmed2 = 0;

        g_refuse   = false;
        g_requests = 0;
        Check(!wxl::seam::Item::UseItem::Gate(&result, &self, &targetPtr, &confirmed2),
              "an item use nobody refuses is left to the client");
        Check(g_requests == 1 && g_lastItemUse.item == wxl::core::objects::HandleOf(&potion) &&
                  g_lastItemUse.item_guid == potion.guid && g_lastItemUse.target_guid == target &&
                  g_lastItemUse.confirmed == WXL_FALSE,
              "and the handler is told the item, its guid and the target");

        g_refuse  = true;
        targetPtr = nullptr;
        Check(wxl::seam::Item::UseItem::Gate(&result, &self, &targetPtr, &confirmed2),
              "a handler that refuses it takes the call");
        Check(result == 0 && g_lastItemUse.target_guid == 0,
              "the client is answered 0, and no target address is no target");

        g_host->Events().Unsubscribe(use);
        wxl::seam::ObjectMgr::ObjDelete::Before(&player);
        wxl::seam::ObjectMgr::ObjDelete::Before(&potion);
    }

    // --- Spell, continued: channels, a resumed cast, a delay, cooldowns, a cancel ---------

    constexpr uint32_t kMsgChannelStart   = 0x139;
    constexpr uint32_t kMsgChannelUpdate  = 0x13A;
    constexpr uint32_t kMsgResumeCastBar  = 0x14D;
    constexpr uint32_t kMsgSpellDelayed   = 0x1E2;
    constexpr uint32_t kMsgSpellCooldown  = 0x134;
    constexpr uint32_t kMsgItemCooldown   = 0xB0;
    constexpr uint32_t kMsgCooldownEvent  = 0x135;
    constexpr uint32_t kMsgClearCooldown  = 0x1DE;
    constexpr uint32_t kMsgModifyCooldown = 0x491;
    constexpr uint32_t kMsgCooldownCheat  = 0x1E1;

    void CheckSpellProgress()
    {
        FakeObject priest;
        priest.guid           = 0x0000000000000ABCull;
        priest.typeId         = WXL_OBJECT_PLAYER;
        priest.castingSpellId = 585;
        priest.channelSpellId = 15407;
        FakeObject boar;
        boar.guid   = 0xF130000000000123ull;
        boar.typeId = WXL_OBJECT_UNIT;
        wxl::seam::ObjectMgr::InitObject::After(&priest, nullptr, 5000u);
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 5001u);
        const wxl_handle priestHandle = wxl::core::objects::HandleOf(&priest);
        const wxl_handle boarHandle   = wxl::core::objects::HandleOf(&boar);
        const uint64_t   stranger     = 0xF13000000000FFFFull;

        // MSG_CHANNEL_START: the caster, the spell, the length.
        Body start;
        start.Packed(priest.guid).U32(15407).U32(3000);
        FakeStore store = start.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_MSG_CHANNEL_START::Before(nullptr, kMsgChannelStart, 0, &store);
        const Received* started = Only(WXL_EV_SpellChannelStart);
        Check(started != nullptr, "Spell.Packet_MSG_CHANNEL_START raises SpellChannelStart once");
        if (started)
        {
            const wxl_spell_progress_args args = started->As<wxl_spell_progress_args>();
            Check(args.caster == priestHandle && args.caster_guid == priest.guid && args.spell_id == 15407,
                  "with the caster and the spell");
            Check(args.duration == 3000 && args.remaining == 3000 && args.target_guid == 0 &&
                      args.target == WXL_HANDLE_NONE,
                  "the length as both duration and remaining, and no target");
        }
        Check(store.read == 0u, "reading the message left the store's cursor where it was");
        Body none;
        none.Packed(priest.guid).U32(15407).U32(0);
        store = none.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_MSG_CHANNEL_START::Before(nullptr, kMsgChannelStart, 0, &store);
        Check(g_received.empty(), "a channel of no length raises nothing, as the client shows none");

        // MSG_CHANNEL_UPDATE with time left: the spell is the one the unit holds.
        Body update;
        update.Packed(priest.guid).U32(1200);
        store = update.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_MSG_CHANNEL_UPDATE::Before(nullptr, kMsgChannelUpdate, 0, &store);
        const Received* updated = Only(WXL_EV_SpellChannelUpdate);
        Check(updated != nullptr && Count(WXL_EV_SpellChannelStop) == 0,
              "Spell.Packet_MSG_CHANNEL_UPDATE raises SpellChannelUpdate while time is left");
        if (updated)
        {
            const wxl_spell_progress_args args = updated->As<wxl_spell_progress_args>();
            Check(args.caster == priestHandle && args.spell_id == 15407 && args.remaining == 1200 &&
                      args.duration == 0,
                  "with the channel's spell read off the unit, the time left and no length");
        }
        Body stop;
        stop.Packed(stranger).U32(0);
        store = stop.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_MSG_CHANNEL_UPDATE::Before(nullptr, kMsgChannelUpdate, 0, &store);
        const Received* stopped = Only(WXL_EV_SpellChannelStop);
        Check(stopped != nullptr && Count(WXL_EV_SpellChannelUpdate) == 0,
              "and SpellChannelStop when none is left");
        if (stopped)
        {
            const wxl_spell_progress_args args = stopped->As<wxl_spell_progress_args>();
            Check(args.caster == WXL_HANDLE_NONE && args.caster_guid == stranger && args.spell_id == 0,
                  "a caster not in view keeps its guid and has no spell to read");
        }

        // SMSG_UNIT_SPELLCAST_START: the caster, the target, the spell, the time left, the length.
        Body resume;
        resume.Packed(priest.guid).Packed(boar.guid).U32(585).U32(900).U32(2500);
        store = resume.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_UNIT_SPELLCAST_START::Before(nullptr, kMsgResumeCastBar, 0, &store);
        const Received* resumed = Only(WXL_EV_SpellCastResume);
        Check(resumed != nullptr, "Spell.Packet_SMSG_UNIT_SPELLCAST_START raises SpellCastResume once");
        if (resumed)
        {
            const wxl_spell_progress_args args = resumed->As<wxl_spell_progress_args>();
            Check(args.caster == priestHandle && args.target == boarHandle && args.target_guid == boar.guid &&
                      args.spell_id == 585 && args.remaining == 900 && args.duration == 2500,
                  "with the caster, the target, the spell, the time left and the length");
        }

        // SMSG_SPELL_DELAYED: the caster and the delay; the spell is the unit's cast.
        Body delayed;
        delayed.Packed(priest.guid).U32(500);
        store = delayed.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_SPELL_DELAYED::Before(nullptr, kMsgSpellDelayed, 0, &store);
        const Received* pushed = Only(WXL_EV_SpellCastDelayed);
        Check(pushed != nullptr, "Spell.Packet_SMSG_SPELL_DELAYED raises SpellCastDelayed once");
        if (pushed)
        {
            const wxl_spell_delay_args args = pushed->As<wxl_spell_delay_args>();
            Check(args.caster == priestHandle && args.caster_guid == priest.guid && args.spell_id == 585 &&
                      args.delay == 500,
                  "with the caster, the cast read off the unit and the delay");
        }
        FakeStore cut{ delayed.bytes.data(), 3, 0 };
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_SPELL_DELAYED::Before(nullptr, kMsgSpellDelayed, 0, &cut);
        Check(g_received.empty(), "a message cut short raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&priest);
        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
    }

    void CheckSpellCooldown()
    {
        FakeObject player;
        player.guid   = 0x0000000000000ABCull;
        player.typeId = WXL_OBJECT_PLAYER;
        FakeObject potion;
        potion.guid   = 0x4000000000000042ull;
        potion.typeId = WXL_OBJECT_ITEM;
        wxl::seam::ObjectMgr::InitObject::After(&player, nullptr, 6000u);
        wxl::seam::ObjectMgr::InitObject::After(&potion, nullptr, 6001u);
        const wxl_handle playerHandle = wxl::core::objects::HandleOf(&player);
        const wxl_handle potionHandle = wxl::core::objects::HandleOf(&potion);

        // SMSG_SPELL_COOLDOWN: the unit, the flags, two spells, and three bytes that are no pair.
        Body cooldown;
        cooldown.U64(player.guid).U8(1).U32(133).U32(8000).U32(116).U32(0).U8(7).U8(7).U8(7);
        FakeStore store = cooldown.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_SPELL_COOLDOWN::Before(nullptr, kMsgSpellCooldown, 0, &store);
        Check(Count(WXL_EV_SpellCooldown) == 2 && g_received.size() == 2,
              "Spell.Packet_SMSG_SPELL_COOLDOWN raises SpellCooldown once per whole pair");
        if (g_received.size() == 2)
        {
            const wxl_spell_cooldown_args first  = g_received[0].As<wxl_spell_cooldown_args>();
            const wxl_spell_cooldown_args second = g_received[1].As<wxl_spell_cooldown_args>();
            Check(first.unit == playerHandle && first.unit_guid == player.guid && first.spell_id == 133 &&
                      first.duration == 8000 && first.flags == 1 && first.item == WXL_HANDLE_NONE &&
                      first.item_guid == 0,
                  "with the unit, the spell, the duration, the flags and no item");
            Check(second.spell_id == 116 && second.duration == 0 && second.unit == playerHandle,
                  "the second pair the same unit, with the spell's own recovery time");
        }
        Check(store.read == 0u, "reading the message left the store's cursor where it was");

        // SMSG_ITEM_COOLDOWN: the item and the spell it casts.
        Body item;
        item.U64(potion.guid).U32(2370);
        store = item.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_SMSG_ITEM_COOLDOWN::Before(nullptr, kMsgItemCooldown, 0, &store);
        const Received* onItem = Only(WXL_EV_SpellCooldown);
        Check(onItem != nullptr, "Spell.Packet_SMSG_ITEM_COOLDOWN raises SpellCooldown once");
        if (onItem)
        {
            const wxl_spell_cooldown_args args = onItem->As<wxl_spell_cooldown_args>();
            Check(args.item == potionHandle && args.item_guid == potion.guid && args.spell_id == 2370 &&
                      args.unit == WXL_HANDLE_NONE && args.unit_guid == 0 && args.duration == 0,
                  "with the item, the spell, no unit and no duration");
        }

        // Packet_Group_31: the spell and the unit, then what the opcode says.
        Body change;
        change.U32(133).U64(player.guid);
        const struct
        {
            uint32_t opcode;
            uint32_t change;
        } opcodes[] = { { kMsgCooldownEvent, 0 }, { kMsgClearCooldown, 1 } };
        for (const auto& opcode : opcodes)
        {
            store = change.Store();
            g_received.clear();
            wxl::seam::Spell::Packet_Group_31::Before(nullptr, opcode.opcode, 0, &store);
            const Received* changed = Only(WXL_EV_SpellCooldownChanged);
            char            what[128];
            snprintf(what, sizeof what, "Spell.Packet_Group_31 raises SpellCooldownChanged %u for opcode 0x%X",
                     unsigned(opcode.change), unsigned(opcode.opcode));
            const wxl_spell_cooldown_change_args args =
                changed ? changed->As<wxl_spell_cooldown_change_args>() : wxl_spell_cooldown_change_args{};
            Check(changed && args.change == opcode.change && args.delay == 0 && args.spell_id == 133 &&
                      args.unit == playerHandle && args.unit_guid == player.guid,
                  what);
        }
        Body modify;
        modify.U32(133).U64(player.guid).U32(static_cast<uint32_t>(-1000));
        store = modify.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_Group_31::Before(nullptr, kMsgModifyCooldown, 0, &store);
        const Received* shifted = Only(WXL_EV_SpellCooldownChanged);
        Check(shifted != nullptr && shifted->As<wxl_spell_cooldown_change_args>().change == 2 &&
                  shifted->As<wxl_spell_cooldown_change_args>().delay == -1000,
              "a modify carries change 2 and its shift, signed");
        store = change.Store();
        g_received.clear();
        wxl::seam::Spell::Packet_Group_31::Before(nullptr, kMsgCooldownCheat, 0, &store);
        Check(g_received.empty(), "an opcode the row does not serve raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&player);
        wxl::seam::ObjectMgr::ObjDelete::Before(&potion);
    }

    void CheckSpellCancel()
    {
        FakeObject mage;
        mage.guid   = 0x0000000000000ABCull;
        mage.typeId = WXL_OBJECT_PLAYER;
        wxl::seam::ObjectMgr::InitObject::After(&mage, nullptr, 7000u);
        const wxl_handle mageHandle = wxl::core::objects::HandleOf(&mage);

        FakePendingCast pending;
        pending.caster  = mage.guid;
        pending.spellId = 133;
        pending.count   = 4;
        g_received.clear();
        wxl::seam::Spell::CancelSpell::Before(&pending, 1, 1, 27);
        const Received* cancelled = Only(WXL_EV_SpellCastCancel);
        Check(cancelled != nullptr, "Spell.CancelSpell raises SpellCastCancel once");
        if (cancelled)
        {
            const wxl_spell_cancel_args args = cancelled->As<wxl_spell_cancel_args>();
            Check(args.caster == mageHandle && args.caster_guid == mage.guid && args.spell_id == 133 &&
                      args.cast_count == 4 && args.reason == 27 && args.notify_server == WXL_TRUE,
                  "with the cast's caster, spell and count, the reason, and that the server may be told");
        }
        g_received.clear();
        wxl::seam::Spell::CancelSpell::Before(&pending, 0, 0, 40);
        const Received* quiet = Only(WXL_EV_SpellCastCancel);
        Check(quiet && quiet->As<wxl_spell_cancel_args>().notify_server == WXL_FALSE &&
                  quiet->As<wxl_spell_cancel_args>().reason == 40,
              "a cancel the server is not told of says so");
        g_received.clear();
        wxl::seam::Spell::CancelSpell::Before(nullptr, 1, 1, 27);
        Check(g_received.empty(), "a null cast raises nothing");

        wxl::seam::ObjectMgr::ObjDelete::Before(&mage);
    }

    // --- Combat: attacks, rounds, the environment, and the spell logs ----------------------

    constexpr uint32_t kMsgAttackStart         = 0x143;
    constexpr uint32_t kMsgAttackStop          = 0x144;
    constexpr uint32_t kMsgAttackSwingNotInRange = 0x145;
    constexpr uint32_t kMsgAttackerStateUpdate = 0x14A;
    constexpr uint32_t kMsgEnvironmentalDamage = 0x1FC;

    void CheckCombat()
    {
        FakeObject warrior;
        warrior.guid   = 0x0000000000000ABCull;
        warrior.typeId = WXL_OBJECT_PLAYER;
        FakeObject boar;
        boar.guid   = 0xF130000000000123ull;
        boar.typeId = WXL_OBJECT_UNIT;
        wxl::seam::ObjectMgr::InitObject::After(&warrior, nullptr, 8000u);
        wxl::seam::ObjectMgr::InitObject::After(&boar, nullptr, 8001u);
        const wxl_handle warriorHandle = wxl::core::objects::HandleOf(&warrior);
        const wxl_handle boarHandle    = wxl::core::objects::HandleOf(&boar);
        const uint64_t   stranger      = 0xF13000000000FFFFull;

        // SMSG_ATTACKSTART: two full guids.
        Body attack;
        attack.U64(warrior.guid).U64(boar.guid);
        FakeStore store = attack.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackStart, 0, &store);
        const Received* began = Only(WXL_EV_CombatAttackStart);
        Check(began != nullptr, "Combat.Packet_Group_29 raises CombatAttackStart for SMSG_ATTACKSTART");
        if (began)
        {
            const wxl_combat_attack_args args = began->As<wxl_combat_attack_args>();
            Check(args.attacker == warriorHandle && args.attacker_guid == warrior.guid &&
                      args.victim == boarHandle && args.victim_guid == boar.guid,
                  "with the attacker and the victim");
        }
        Check(store.read == 0u, "reading the message left the store's cursor where it was");

        // SMSG_ATTACKSTOP: packed guids and the facing flag.
        Body stop;
        stop.Packed(boar.guid).Packed(warrior.guid).U32(1);
        store = stop.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackStop, 0, &store);
        const Received* ended = Only(WXL_EV_CombatAttackStop);
        Check(ended && ended->As<wxl_combat_attack_args>().attacker == boarHandle &&
                  ended->As<wxl_combat_attack_args>().victim == warriorHandle,
              "CombatAttackStop for SMSG_ATTACKSTOP, with the packed guids");

        // SMSG_ATTACKERSTATEUPDATE that landed: a crit over two schools, absorbs per school, a block.
        const uint32_t landedFlags = 0x200 | 0x20 | 0x2000;
        Body           round;
        round.U32(landedFlags).Packed(warrior.guid).Packed(boar.guid).U32(150).U32(0).U8(2)
            .U32(1).U32(0).U32(100).U32(4).U32(0).U32(50).U32(10).U32(5)
            .U8(1).U32(0).U32(0).U32(20);
        store = round.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackerStateUpdate, 0, &store);
        const Received* landed = Only(WXL_EV_CombatDamage);
        Check(landed != nullptr && Count(WXL_EV_CombatMiss) == 0,
              "a round that landed raises CombatDamage once and no CombatMiss");
        if (landed)
        {
            const wxl_combat_damage_args args = landed->As<wxl_combat_damage_args>();
            Check(args.source == warriorHandle && args.target == boarHandle && args.kind == 0 &&
                      args.spell_id == 0 && args.amount == 150 && args.overkill == 0,
                  "with the attacker, the victim, a plain swing and the total");
            Check(args.school == 5 && args.absorbed == 15 && args.resisted == 0 && args.blocked == 20 &&
                      args.crit == WXL_TRUE && args.hit_info == landedFlags,
                  "the schools joined, the absorbs summed, the block, the crit and the flags");
        }
        Body dodged;
        dodged.U32(0).Packed(warrior.guid).Packed(boar.guid).U32(0).U32(0).U8(1).U32(1).U32(0).U32(0)
            .U8(2).U32(0).U32(845);
        store = dodged.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackerStateUpdate, 0, &store);
        const Received* escaped = Only(WXL_EV_CombatMiss);
        Check(escaped != nullptr && Count(WXL_EV_CombatDamage) == 0,
              "a round the victim dodged raises CombatMiss and no CombatDamage");
        if (escaped)
        {
            const wxl_combat_miss_args args = escaped->As<wxl_combat_miss_args>();
            Check(args.melee == WXL_TRUE && args.reason == 2 && args.spell_id == 845 && args.hit_info == 0 &&
                      args.source == warriorHandle && args.target == boarHandle,
                  "with the victim's state as the reason and the swing's spell");
        }
        Body missed;
        missed.U32(0x10).Packed(warrior.guid).Packed(boar.guid).U32(0).U32(0).U8(0).U8(0).U32(0).U32(0);
        store = missed.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackerStateUpdate, 0, &store);
        const Received* plain = Only(WXL_EV_CombatMiss);
        Check(plain && plain->As<wxl_combat_miss_args>().reason == 0 &&
                  plain->As<wxl_combat_miss_args>().hit_info == 0x10,
              "a plain miss carries no reason and the miss flag");

        // SMSG_ENVIRONMENTALDAMAGELOG: a full guid, the cause, the amounts.
        Body lava;
        lava.U64(warrior.guid).U8(3).U32(400).U32(0).U32(0);
        store = lava.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgEnvironmentalDamage, 0, &store);
        const Received* burned = Only(WXL_EV_CombatDamage);
        Check(burned != nullptr, "CombatDamage for SMSG_ENVIRONMENTALDAMAGELOG");
        if (burned)
        {
            const wxl_combat_damage_args args = burned->As<wxl_combat_damage_args>();
            Check(args.kind == 3 && args.environment == 3 && args.amount == 400 && args.school == 4 &&
                      args.source == WXL_HANDLE_NONE && args.source_guid == 0 && args.target == warriorHandle,
                  "with no source, the cause, the amount and the cause's school");
        }
        store = attack.Store();
        g_received.clear();
        wxl::seam::Combat::Packet_Group_29::Before(nullptr, kMsgAttackSwingNotInRange, 0, &store);
        Check(g_received.empty(), "an opcode the row does not report raises nothing");

        // SMSG_SPELLLOGMISS: two targets, one not in view.
        Body missLog;
        missLog.U32(116).U64(warrior.guid).U8(0).U32(2).U64(boar.guid).U8(3).U64(stranger).U8(7);
        store = missLog.Store();
        g_received.clear();
        wxl::seam::Combat::HandleSpellLogMiss::Before(&store, 0);
        Check(Count(WXL_EV_CombatMiss) == 2 && g_received.size() == 2,
              "Combat.HandleSpellLogMiss raises CombatMiss once per target");
        if (g_received.size() == 2)
        {
            const wxl_combat_miss_args first  = g_received[0].As<wxl_combat_miss_args>();
            const wxl_combat_miss_args second = g_received[1].As<wxl_combat_miss_args>();
            Check(first.source == warriorHandle && first.target == boarHandle && first.spell_id == 116 &&
                      first.reason == 3 && first.melee == WXL_FALSE && first.hit_info == 0,
                  "with the caster, the target, the spell and the reason");
            Check(second.target == WXL_HANDLE_NONE && second.target_guid == stranger && second.reason == 7,
                  "a target not in view keeps its guid");
        }
        Body missDebug;
        missDebug.U32(116).U64(warrior.guid).U8(1).U32(1).U64(boar.guid).U8(1).U32(0).U32(0);
        store = missDebug.Store();
        g_received.clear();
        wxl::seam::Combat::HandleSpellLogMiss::Before(&store, 0);
        Check(Only(WXL_EV_CombatMiss) && Only(WXL_EV_CombatMiss)->As<wxl_combat_miss_args>().reason == 1,
              "the debug floats after a target are skipped");
        Body missCut;
        missCut.U32(116).U64(warrior.guid).U8(0).U32(2).U64(boar.guid).U8(3);
        store = missCut.Store();
        g_received.clear();
        wxl::seam::Combat::HandleSpellLogMiss::Before(&store, 0);
        Check(g_received.empty(), "a list cut short raises none of it");

        // SMSG_SPELLNONMELEEDAMAGELOG: the target first, then the caster.
        Body spell;
        spell.Packed(boar.guid).Packed(warrior.guid).U32(116).U32(900).U32(100).U8(16).U32(50).U32(25)
            .U8(0).U8(0).U32(0).U32(0x2).U8(0);
        store = spell.Store();
        g_received.clear();
        wxl::seam::Combat::HandleSpellNonMeleeDamageLog::Before(&store, 0);
        const Received* hit = Only(WXL_EV_CombatDamage);
        Check(hit != nullptr, "Combat.HandleSpellNonMeleeDamageLog raises CombatDamage once");
        if (hit)
        {
            const wxl_combat_damage_args args = hit->As<wxl_combat_damage_args>();
            Check(args.source == warriorHandle && args.target == boarHandle && args.kind == 1 &&
                      args.spell_id == 116 && args.amount == 900 && args.overkill == 100,
                  "with the caster, the target, the spell, the damage and the overkill");
            Check(args.school == 16 && args.absorbed == 50 && args.resisted == 25 && args.blocked == 0 &&
                      args.crit == WXL_TRUE && args.hit_info == 0x2,
                  "the school, the amounts, the crit and the flags");
        }

        // SMSG_SPELLHEALLOG: the target first, then the caster.
        Body heal;
        heal.Packed(warrior.guid).Packed(boar.guid).U32(2061).U32(1200).U32(200).U32(0).U8(1).U8(0);
        store = heal.Store();
        g_received.clear();
        wxl::seam::Combat::HandleSpellHealLog::Before(&store, 0);
        const Received* healed = Only(WXL_EV_CombatHeal);
        Check(healed != nullptr, "Combat.HandleSpellHealLog raises CombatHeal once");
        if (healed)
        {
            const wxl_combat_heal_args args = healed->As<wxl_combat_heal_args>();
            Check(args.source == boarHandle && args.target == warriorHandle && args.spell_id == 2061 &&
                      args.amount == 1200 && args.overheal == 200 && args.absorbed == 0 &&
                      args.crit == WXL_TRUE && args.periodic == WXL_FALSE,
                  "with the caster, the target, the spell, the amounts and the crit");
        }

        // SMSG_PERIODICAURALOG: a damage tick, an energize tick, a heal tick, then a type the
        // client stops at.
        Body ticks;
        ticks.Packed(boar.guid).Packed(warrior.guid).U32(589).U32(4)
            .U32(3).U32(80).U32(0).U32(32).U32(0).U32(10).U8(0)
            .U32(21).U32(0).U32(30)
            .U32(8).U32(60).U32(0).U32(0).U8(1)
            .U32(77).U32(1);
        store = ticks.Store();
        g_received.clear();
        wxl::seam::Combat::HandlePeriodicAuraLog::Before(&store, 0);
        Check(g_received.size() == 2 && g_received[0].event == WXL_EV_CombatDamage &&
                  g_received[1].event == WXL_EV_CombatHeal,
              "Combat.HandlePeriodicAuraLog raises the damage and heal ticks, in order, up to the stop");
        if (g_received.size() == 2)
        {
            const wxl_combat_damage_args tick = g_received[0].As<wxl_combat_damage_args>();
            const wxl_combat_heal_args   hot  = g_received[1].As<wxl_combat_heal_args>();
            Check(tick.kind == 2 && tick.spell_id == 589 && tick.amount == 80 && tick.school == 32 &&
                      tick.resisted == 10 && tick.crit == WXL_FALSE && tick.hit_info == 0 &&
                      tick.source == warriorHandle && tick.target == boarHandle,
                  "the damage tick with its amounts, the school and no flags");
            Check(hot.periodic == WXL_TRUE && hot.amount == 60 && hot.crit == WXL_TRUE &&
                      hot.spell_id == 589,
                  "the heal tick as periodic, with its amount and crit");
        }
        Body tickCut;
        tickCut.Packed(boar.guid).Packed(warrior.guid).U32(589).U32(2)
            .U32(3).U32(80).U32(0).U32(32).U32(0).U32(10).U8(0)
            .U32(3).U32(80);
        store = tickCut.Store();
        g_received.clear();
        wxl::seam::Combat::HandlePeriodicAuraLog::Before(&store, 0);
        Check(g_received.empty(), "a tick cut short raises none of the log");

        wxl::seam::ObjectMgr::ObjDelete::Before(&warrior);
        wxl::seam::ObjectMgr::ObjDelete::Before(&boar);
    }

    // --- FrameScript: the UI events the client fires, and the errors its Lua raises --------

    /// What the UI event handler below saw, copied out while the args were valid.
    struct UiWatch
    {
        int         calls = 0;
        uint32_t    index = 0;
        std::string name;
        std::string format;
        std::string values;
        uint32_t    count = 0;
    };

    void WXL_CALL OnUiEvent(void* user, const void* args)
    {
        UiWatch&                 watch = *static_cast<UiWatch*>(user);
        const wxl_ui_event_args& event = *static_cast<const wxl_ui_event_args*>(args);
        ++watch.calls;
        watch.index  = event.index;
        watch.name   = std::string(event.name.data, event.name.size);
        watch.format = std::string(event.format.data ? event.format.data : "", event.format.size);
        watch.values = std::string(static_cast<const char*>(event.values ? event.values : ""),
                                   event.values_size);
        watch.count  = event.value_count;
    }

    /// Fires an event the way the client's variadic wrapper does: through a va_list.
    void Fire(uint32_t index, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        wxl::seam::FrameScript::vSignalEvent::Before(index, format, args);
        va_end(args);
    }

    void CheckUiEvent()
    {
        // A game context with the client's event table installed.
        g_vm.live       = true;
        g_uiLoggingIn   = 1;
        g_uiInitialized = 0;
        wxl::seam::FrameScript::Initialize::After(1, 0);
        static const char* names[] = { "PLAYER_LOGIN", "CHAT_MSG_SAY", "UNIT_HEALTH" };
        wxl::seam::FrameScript::CreateEvents::After(names, 3);

        UiWatch                watch;
        const wxl_subscription sub =
            g_host->Events().Subscribe(WXL_EV_UiEvent, &OnUiEvent, &watch, 0);

        Fire(1, "%s%d%f%b%u%s", "hello", -5, 2.5, 1, 7u, static_cast<const char*>(nullptr));
        Check(watch.calls == 1 && watch.index == 1 && watch.name == "CHAT_MSG_SAY",
              "FrameScript.vSignalEvent raises UiEvent once, naming the event at the index");
        const std::string expected("shello\0d-5\0f2.5\0b1\0u7\0n\0", 24);
        Check(watch.format == "%s%d%f%b%u%s" && watch.count == 6 && watch.values == expected,
              "with the format and every argument rendered by its kind, a null string as nil");

        Fire(0, nullptr);
        Check(watch.calls == 2 && watch.name == "PLAYER_LOGIN" && watch.count == 0 &&
                  watch.values.empty() && watch.format.empty(),
              "an event without arguments carries none");

        Fire(2, "%x%d", 3);
        Check(watch.calls == 3 && watch.count == 1 && watch.values == std::string("d3\0", 3),
              "a letter the client does not read reads nothing here either");

        Fire(9, "%d", 1);
        Check(watch.calls == 3, "an index outside the installed table raises nothing");

        g_host->Events().Unsubscribe(sub);
        wxl::seam::FrameScript::Destroy::Before();
        g_vm.live = false;
    }

    void CheckScriptError()
    {
        g_vm.live       = true;
        g_uiLoggingIn   = 0;
        g_uiInitialized = 0;
        wxl::seam::FrameScript::Initialize::After(1, 0);

        g_vm.stack.clear();
        g_vm.stack.push_back(String("Interface\\FrameXML\\Boom.lua:12: attempt to call nil"));
        g_received.clear();
        wxl::seam::FrameScript::HandleError::Before(&g_vm);
        const Received* error = Only(WXL_EV_ScriptError);
        Check(error != nullptr, "FrameScript.HandleError raises ScriptError once");
        if (error)
        {
            const wxl_script_error_args args = error->As<wxl_script_error_args>();
            Check(args.context == WXL_SCRIPT_CONTEXT_GLUE, "naming the live context");
            Check(std::string(args.message.data, args.message.size) ==
                      "Interface\\FrameXML\\Boom.lua:12: attempt to call nil",
                  "and carrying the message on top of the stack");
        }

        g_vm.stack.clear();
        g_vm.stack.push_back(Number(42));
        g_received.clear();
        wxl::seam::FrameScript::HandleError::Before(&g_vm);
        Check(Only(WXL_EV_ScriptError) != nullptr &&
                  std::string(Only(WXL_EV_ScriptError)->As<wxl_script_error_args>().message.data) == "42",
              "a number reads as its text");

        g_vm.stack.clear();
        g_vm.stack.push_back(FakeValue{});
        g_received.clear();
        wxl::seam::FrameScript::HandleError::Before(&g_vm);
        Check(Only(WXL_EV_ScriptError) != nullptr &&
                  std::string(Only(WXL_EV_ScriptError)->As<wxl_script_error_args>().message.data) ==
                      "UNKNOWN ERROR",
              "anything else is what the client will report it as");

        g_vm.stack.clear();
        wxl::seam::FrameScript::Destroy::Before();
        g_vm.live = false;
    }

#if defined(_M_IX86)
    // --- The generated detour, on the architecture it is generated for --------------------------

    /// A client-shaped class: a non-static member, `this` in ECX, two stack arguments, a result.
    struct Thing
    {
        int32_t base = 100;
        int32_t __thiscall Add(int32_t a, int32_t b) { return base + a + b; }
    };

    int32_t g_shimBefore = 0;
    int32_t g_shimAfter  = 0;
    void*   g_shimSelf   = nullptr;

    void WXL_CALL ShimBefore(void* self, int32_t a, int32_t b)
    {
        g_shimSelf   = self;
        g_shimBefore = a * 10 + b;
    }

    void WXL_CALL ShimAfter(int32_t result, void* self, int32_t a, int32_t b)
    {
        g_shimAfter = self == g_shimSelf ? result + a + b : -1;
    }

    using ShimSeam = wxl::core::seams::DetourOf<wxl::seam::detail::conv_thiscall,
                                                int32_t(void*, int32_t, int32_t)>;
    using ShimLink = ShimSeam::Link<&ShimBefore, &ShimAfter>;

    /// Calls the generated __fastcall detour the way the hook engine's caller would: as the
    /// __thiscall it stands in for, so a wrong shim corrupts the stack here rather than in a
    /// running client.
    void CheckThiscallShim()
    {
        Thing thing;
        auto  member        = &Thing::Add;
        ShimLink::original  = reinterpret_cast<ShimSeam::Fn>(*reinterpret_cast<void**>(&member));

        using AsThiscall = int32_t(__thiscall*)(Thing*, int32_t, int32_t);
        const AsThiscall detour = reinterpret_cast<AsThiscall>(&ShimLink::Detour);
        const int32_t result = detour(&thing, 5, 7);

        Check(result == 112, "the thiscall shim returns the original's result (100 + 5 + 7)");
        Check(g_shimSelf == &thing && g_shimBefore == 57, "Before received `this` and the arguments");
        Check(g_shimAfter == 112 + 5 + 7, "After received the result first, then `this` and the arguments");
    }

    using ShimBeforeLink = ShimSeam::Link<&ShimBefore, nullptr>;
    using ShimAfterLink  = ShimSeam::Link<nullptr, &ShimAfter>;

    /// The detours of the one-sided shapes: the original runs, and only the declared side does.
    void CheckOneSidedShim()
    {
        Thing thing;
        auto  member = &Thing::Add;
        using AsThiscall = int32_t(__thiscall*)(Thing*, int32_t, int32_t);

        ShimBeforeLink::original = reinterpret_cast<ShimSeam::Fn>(*reinterpret_cast<void**>(&member));
        g_shimSelf = nullptr, g_shimBefore = 0, g_shimAfter = 0;
        int32_t result = reinterpret_cast<AsThiscall>(&ShimBeforeLink::Detour)(&thing, 5, 7);
        Check(result == 112 && g_shimBefore == 57 && g_shimAfter == 0,
              "a Before-only detour runs Before and the original, and no After");

        ShimAfterLink::original = reinterpret_cast<ShimSeam::Fn>(*reinterpret_cast<void**>(&member));
        g_shimSelf = &thing, g_shimBefore = 0, g_shimAfter = 0;
        result = reinterpret_cast<AsThiscall>(&ShimAfterLink::Detour)(&thing, 5, 7);
        Check(result == 112 && g_shimBefore == 0 && g_shimAfter == 112 + 5 + 7,
              "an After-only detour runs the original and After, and no Before");
    }

    int32_t g_gateCalls = 0;
    void*   g_gateSelf  = nullptr;
    bool    g_gateTakes = false;

    bool WXL_CALL ShimGate(int32_t* result, void** self, int32_t* a, int32_t* b)
    {
        ++g_gateCalls;
        g_gateSelf = *self;
        *a         = *a + 1; // what the original is called with, if it is called at all
        if (!g_gateTakes) return false;
        *result = *a * 1000 + *b;
        return true;
    }

    using ShimGateSeam = wxl::core::seams::GateOf<wxl::seam::detail::conv_thiscall,
                                                  int32_t(void*, int32_t, int32_t)>;
    using ShimGateLink = ShimGateSeam::Link<&ShimGate>;

    /// Drives the generated gate as the hook engine's caller would, both ways: once letting the
    /// original run with a rewritten argument, once taking the call away from it.
    void CheckThiscallGate()
    {
        Thing thing;
        auto  member = &Thing::Add;
        ShimGateLink::original =
            reinterpret_cast<ShimGateSeam::Fn>(*reinterpret_cast<void**>(&member));

        using AsThiscall = int32_t(__thiscall*)(Thing*, int32_t, int32_t);
        const AsThiscall detour = reinterpret_cast<AsThiscall>(&ShimGateLink::Detour);

        g_gateTakes    = false;
        int32_t result = detour(&thing, 5, 7);
        Check(result == 100 + 6 + 7,
              "a gate that lets the call through rewrites what the original is given");
        Check(g_gateSelf == &thing && g_gateCalls == 1, "and it was handed the object once");

        g_gateTakes = true;
        result      = detour(&thing, 5, 7);
        Check(result == 6 * 1000 + 7, "a gate that takes the call answers in the original's place");
        Check(g_gateCalls == 2, "and the original never ran");
    }

    // --- The register thunks, against stand-ins with the client's conventions -------------------

    // What a stand-in original found in its registers and on its stack.
    uint32_t g_seenEdi = 0, g_seenEsi = 0, g_seenEax = 0, g_seenEcx = 0;
    uint32_t g_seenArg0 = 0, g_seenArg1 = 0;

    // What the caller's callee-saved registers held once the thunk returned.
    uint32_t g_keptEbx = 0, g_keptEsi = 0, g_keptEdi = 0;

    __declspec(naked) void FakeDispatchUpdate()
    {
        __asm {
            mov  dword ptr [g_seenEdi], edi
            mov  eax, dword ptr [esp+4]
            mov  dword ptr [g_seenArg0], eax
            mov  eax, dword ptr [esp+8]
            mov  dword ptr [g_seenArg1], eax
            ret
        }
    }

    __declspec(naked) void FakeInitObject()
    {
        __asm {
            mov  dword ptr [g_seenEsi], esi
            mov  dword ptr [g_seenEax], eax
            mov  dword ptr [g_seenEcx], ecx
            ret
        }
    }

    __declspec(naked) void FakeObjDelete()
    {
        __asm {
            mov  dword ptr [g_seenEax], eax
            ret
        }
    }

    __declspec(naked) void FakeDestroyRemovedObject()
    {
        __asm {
            mov  dword ptr [g_seenEsi], esi
            ret
        }
    }

    /// Calls a thunk the way IEvtDispatchUpdate's caller does: context in EDI, two stack slots
    /// the caller cleans.
    void CallWithEdi(void* thunk, void* context, uint32_t arg0, uint32_t arg1)
    {
        __asm {
            mov  ebx, 0B0B0B0Bh
            mov  esi, 05050505h
            mov  edi, context
            push arg1
            push arg0
            call thunk
            add  esp, 8
            mov  dword ptr [g_keptEbx], ebx
            mov  dword ptr [g_keptEsi], esi
            mov  dword ptr [g_keptEdi], edi
        }
    }

    /// Calls a thunk the way InitObject's caller does: ESI, EAX and ECX, nothing pushed.
    void CallWithEsiEaxEcx(void* thunk, void* esiValue, uint32_t eaxValue, uint32_t ecxValue)
    {
        __asm {
            mov  ebx, 0B0B0B0Bh
            mov  edi, 0D0D0D0Dh
            mov  esi, esiValue
            mov  eax, eaxValue
            mov  ecx, ecxValue
            call thunk
            mov  dword ptr [g_keptEbx], ebx
            mov  dword ptr [g_keptEsi], esi
            mov  dword ptr [g_keptEdi], edi
        }
    }

    /// Calls a thunk the way ObjDelete's callers do: the object in EAX, nothing pushed.
    void CallWithEax(void* thunk, void* eaxValue)
    {
        __asm {
            mov  ebx, 0B0B0B0Bh
            mov  esi, 05050505h
            mov  edi, 0D0D0D0Dh
            mov  eax, eaxValue
            call thunk
            mov  dword ptr [g_keptEbx], ebx
            mov  dword ptr [g_keptEsi], esi
            mov  dword ptr [g_keptEdi], edi
        }
    }

    /// Every regparm row has a thunk, and each thunk hands the original its registers back.
    void CheckRegparmThunks()
    {
        namespace regparm = wxl::core::seams::regparm;

        for (const wxl::core::seams::Seam& seam : wxl::core::seams::Table())
        {
            if (strcmp(seam.convention, "regparm") != 0) continue;
            char what[160];
            snprintf(what, sizeof what, "%s takes its arguments in registers and has a thunk", seam.name);
            Check(regparm::Find(seam.name) != nullptr, what);
        }
        Check(regparm::Find("World.Preload") == nullptr, "a seam with a stack convention has none");

        const regparm::Row* update  = regparm::Find("Event.DispatchUpdate");
        const regparm::Row* init    = regparm::Find("ObjectMgr.InitObject");
        const regparm::Row* remove  = regparm::Find("ObjectMgr.ObjDelete");
        const regparm::Row* destroy = regparm::Find("ObjectMgr.DestroyRemovedObject");
        if (!update || !init || !remove || !destroy) return;

        *update->original  = reinterpret_cast<void*>(&FakeDispatchUpdate);
        *init->original    = reinterpret_cast<void*>(&FakeInitObject);
        *remove->original  = reinterpret_cast<void*>(&FakeObjDelete);
        *destroy->original = reinterpret_cast<void*>(&FakeDestroyRemovedObject);

        int         context = 0;
        const float elapsed = 0.25f;
        uint32_t    elapsedBits;
        memcpy(&elapsedBits, &elapsed, sizeof elapsedBits);

        g_received.clear();
        CallWithEdi(update->detour, &context, 4242u, elapsedBits);
        Check(g_seenEdi == reinterpret_cast<uint32_t>(&context),
              "Event.DispatchUpdate's original finds the context in EDI");
        Check(g_seenArg0 == 4242u && g_seenArg1 == elapsedBits, "and its two stack arguments");
        const Received* tick = Only(WXL_EV_Update);
        Check(tick && tick->As<wxl_update_args>().delta == elapsed,
              "Before read the arguments the thunk was handed");
        Check(g_keptEbx == 0x0B0B0B0Bu && g_keptEsi == 0x05050505u &&
                  g_keptEdi == reinterpret_cast<uint32_t>(&context),
              "and the caller gets EBX, ESI and EDI back");

        FakeObject spider;
        spider.guid   = 0xF130000000000777ull;
        spider.typeId = WXL_OBJECT_UNIT;

        g_received.clear();
        CallWithEsiEaxEcx(init->detour, &spider, 0x1234u, 77u);
        Check(g_seenEsi == reinterpret_cast<uint32_t>(&spider) && g_seenEax == 0x1234u &&
                  g_seenEcx == 77u,
              "ObjectMgr.InitObject's original finds the object in ESI, the block in EAX, the time in ECX");
        const Received* created = Only(WXL_EV_ObjectCreate);
        Check(created && created->As<wxl_object_args>().guid == spider.guid,
              "After was handed the object from ESI");
        Check(g_keptEbx == 0x0B0B0B0Bu && g_keptEdi == 0x0D0D0D0Du &&
                  g_keptEsi == reinterpret_cast<uint32_t>(&spider),
              "and the caller gets EBX, ESI and EDI back");

        g_received.clear();
        CallWithEax(remove->detour, &spider);
        Check(g_seenEax == reinterpret_cast<uint32_t>(&spider),
              "ObjectMgr.ObjDelete's original finds the object in EAX");
        Check(Only(WXL_EV_ObjectDestroy) != nullptr, "Before was handed the object from EAX");
        Check(g_keptEbx == 0x0B0B0B0Bu && g_keptEsi == 0x05050505u && g_keptEdi == 0x0D0D0D0Du,
              "and the caller gets EBX, ESI and EDI back");

        FakeObject moth;
        CallWithEsiEaxEcx(destroy->detour, &moth, 0u, 0u);
        Check(g_seenEsi == reinterpret_cast<uint32_t>(&moth),
              "ObjectMgr.DestroyRemovedObject's original finds the object in ESI");

        *update->original  = nullptr;
        *init->original    = nullptr;
        *remove->original  = nullptr;
        *destroy->original = nullptr;
        wxl::core::stages::Restart();
    }
#endif
}


// --- A measurement, not a check ---------------------------------------------------------------
// Two paths carry work at a rate nothing else here does: the file-open gate, which the client
// reaches once per file it opens, and the event bus, which every seam goes through. These are
// not frame times -- there is no client and no archive behind them -- but they are the real code
// on the real path, so a change to either shows up here before anyone notices it in game.

void WXL_CALL BenchHandler(void*, const void*) {}

void Bench()
{
    using Clock = std::chrono::steady_clock;

    const char* texture = "World\\Expansion01\\Doodads\\Generic\\BloodElf\\Banner01.blp";
    const char* table   = "DBFilesClient\\Spell.dbc";
    char        out[512];

    auto time = [](const char* what, int runs, auto&& body) {
        body();                                 // once, so nothing measured is a cold page
        const Clock::time_point start = Clock::now();
        for (int i = 0; i < runs; ++i) body();
        const double ns =
            std::chrono::duration<double, std::nano>(Clock::now() - start).count() / runs;
        printf("  %-46s %8.1f ns/call\n", what, ns);
    };

    printf("\nbench (%s):\n", sizeof(void*) == 8 ? "x64" : "x86");

    wxl::core::services::data::Clear();
    time("file-open gate, a path that is not a database", 200000, [&] {
        wxl::core::services::data::ServedPath(texture, strlen(texture), out, sizeof out);
    });
    time("file-open gate, a database nobody serves", 200000, [&] {
        wxl::core::services::data::ServedPath(table, strlen(table), out, sizeof out);
    });

    // A bus of its own: the harness's bus carries the recording handlers of the checks.
    wxl::host::EventBus bus({WXL_EV_Update}, nullptr);
    wxl_update_args     args{};
    args.delta = 0.016f;
    time("event bus, no subscriber", 200000, [&] { bus.Emit(WXL_EV_Update, &args); });

    bus.Subscribe(WXL_EV_Update, &BenchHandler, nullptr, 0);
    time("event bus, one subscriber", 200000, [&] { bus.Emit(WXL_EV_Update, &args); });
    bus.Subscribe(WXL_EV_Update, &BenchHandler, nullptr, 0);
    time("event bus, two subscribers", 200000, [&] { bus.Emit(WXL_EV_Update, &args); });

    wxl::core::services::data::Clear();
}

namespace
{
    // --- M2: the model batches wxl.gx hands its watchers ---------------------------------------

    struct SeenDraws
    {
        int               count = 0;
        wxl_gx_model_draw last{};
    };

    void WXL_CALL SeeDraw(const wxl_gx_model_draw* draw, void* user)
    {
        SeenDraws* seen = static_cast<SeenDraws*>(user);
        ++seen->count;
        seen->last = *draw;
    }

    void CheckM2()
    {
        int render = 0; // CM2SceneRender's `this`; the fake binding only tests it for null
        int model  = 0;
        int root   = 0;

        const wxl_gx_api& gx = wxl::core::services::GxApi();
        Check(!wxl::core::services::gx::Watching(), "nothing watches model batches at the start");

        g_gxDraw                 = wxl_gx_model_draw{};
        g_gxDraw.struct_size     = sizeof g_gxDraw;
        g_gxDraw.model           = &model;
        g_gxDraw.root_model      = &root;
        g_gxDraw.primitive_type  = 4;
        g_gxDraw.base_vertex     = 12;
        g_gxDraw.min_index       = 3;
        g_gxDraw.vertex_count    = 90;
        g_gxDraw.start_index     = 300;
        g_gxDraw.primitive_count = 40;

        g_gxDrew = true;
        wxl::seam::M2::DrawBatch::After(&render);
        Check(g_gxDrew, "with no watcher the seam does not even ask the client for the batch");

        SeenDraws seen;
        uint32_t  token = 0;
        Check(gx.watch_model_draws(&SeeDraw, &seen, &token) == WXL_OK && token != 0,
              "a watcher is accepted and given a token");
        Check(gx.watch_model_draws(nullptr, nullptr, nullptr) == WXL_E_INVALID,
              "a watcher without a function is refused");

        wxl::seam::M2::DrawBatch::After(&render);
        Check(seen.count == 1 && seen.last.model == &model && seen.last.root_model == &root &&
                  seen.last.base_vertex == 12 && seen.last.start_index == 300 &&
                  seen.last.primitive_count == 40,
              "a drawn batch reaches the watcher with the numbers of the device's draw");

        wxl::seam::M2::DrawBatch::After(&render);
        Check(seen.count == 1, "a batch the device did not draw is not handed on");

        wxl::seam::M2::DrawBatch::After(nullptr);
        Check(seen.count == 1, "nor is one from no renderer");

        gx.unwatch_model_draws(token);
        Check(!wxl::core::services::gx::Watching(), "an unwatched watcher is gone");
        g_gxDrew = true;
        wxl::seam::M2::DrawBatch::After(&render);
        Check(seen.count == 1, "and hears nothing more");
        g_gxDrew = false;
    }

    // --- Component and M2.AnimateMT: what wxl.m2 hands its watchers, and the models it drives -----

    struct SeenEquipment
    {
        int              count = 0;
        wxl_m2_equipment last{};
    };

    void WXL_CALL SeeEquipment(const wxl_m2_equipment* change, void* user)
    {
        SeenEquipment* seen = static_cast<SeenEquipment*>(user);
        ++seen->count;
        seen->last = *change;
    }

    wxl_str M2Str(const char* text) { return wxl_str{text, static_cast<uint32_t>(std::strlen(text))}; }

    void CheckM2Service()
    {
        const wxl_m2_api& m2   = wxl::core::services::M2Api();
        g_component            = FakeComponent{};
        g_component.model      = &g_charModel;
        g_component.race       = 1;
        g_component.sex        = 1;
        g_component.display[4] = 416036;
        g_wearerAsked          = 0;

        Check(!wxl::core::services::m2::Watching(), "nothing watches equipment at the start");
        wxl::seam::Component::AddItem::After(&g_component, 4, nullptr, 0);
        Check(g_wearerAsked == 0, "with no watcher the Component seams do not even ask the client");

        SeenEquipment seen;
        uint32_t      token = 0;
        Check(m2.watch_equipment(&SeeEquipment, &seen, &token) == WXL_OK && token != 0,
              "an equipment watcher is accepted and given a token");
        Check(m2.watch_equipment(nullptr, nullptr, nullptr) == WXL_E_INVALID,
              "an equipment watcher without a function is refused");

        wxl::seam::Component::AddItem::After(&g_component, 4, nullptr, 0);
        Check(seen.count == 1 && seen.last.change == WXL_M2_EQUIPPED && seen.last.wearer == &g_component &&
                  seen.last.model == &g_charModel && seen.last.item_slot == 4 && seen.last.display_id == 416036 &&
                  seen.last.race_id == 1 && seen.last.sex_id == 1 &&
                  seen.last.struct_size == sizeof(wxl_m2_equipment),
              "an item put on reaches the watcher with its slot, its display and the wearer");

        wxl::seam::Component::AddItem::After(&g_component, 5, nullptr, 0);
        Check(seen.count == 1, "an empty slot hands nothing on");
        wxl::seam::Component::AddItem::After(&g_component, -1, nullptr, 0);
        Check(seen.count == 1, "nor does a slot out of range");

        wxl::seam::Component::RemoveItem::Before(&g_component, 4);
        Check(seen.count == 2 && seen.last.change == WXL_M2_UNEQUIPPED && seen.last.display_id == 416036,
              "an item about to come off is handed on with the display it was");

        wxl::seam::Component::FreeComponent::Before(&g_component);
        Check(seen.count == 3 && seen.last.change == WXL_M2_FREED && seen.last.wearer == &g_component &&
                  seen.last.item_slot == 0 && seen.last.display_id == 0,
              "a wearer about to be freed is handed on, with no slot");

        wxl::seam::Component::FreeComponent::Before(nullptr);
        Check(seen.count == 3, "no component hands nothing on");

        wxl_m2_item_display row{};
        Check(m2.item_display(416036, &row) == WXL_OK && row.id == 416036 &&
                  std::strcmp(row.model[0], "belt.mdx") == 0 && std::strcmp(row.icon[1], "53::96") == 0,
              "an ItemDisplayInfo row is read through");
        Check(m2.item_display(1, &row) == WXL_E_NOT_FOUND, "a row the client lacks is not found");
        wxl_str prefix{};
        Check(m2.race_prefix(1, &prefix) == WXL_OK && std::string(prefix.data, prefix.size) == "Hu",
              "a race's client prefix is read through");
        Check(m2.race_prefix(99, &prefix) == WXL_E_NOT_FOUND, "a race the client lacks is not found");

        Check(m2.create_model(nullptr, M2Str("a.mdx")) == nullptr, "a model needs a scene to be made in");
        void* piece = m2.create_model(&g_charModel, M2Str("Item\\ObjectComponents\\Collections\\a_HuF.mdx"));
        Check(piece != nullptr, "a model is made in the scene of the one given");
        int stranger = 0;
        Check(m2.attach_model(&stranger, &g_charModel, 19, WXL_M2_ATTACH_ROOT) == WXL_E_INVALID,
              "a model the service did not make is refused");
        Check(m2.attach_model(piece, &g_charModel, 99, 0) == WXL_E_NOT_FOUND,
              "a point the parent lacks is refused without ATTACH_ROOT");
        Check(!wxl::core::services::m2::Following(), "and nothing follows yet");

        Check(m2.attach_model(piece, &g_charModel, 99, WXL_M2_ATTACH_ROOT | WXL_M2_ATTACH_FOLLOW_BONES) == WXL_OK &&
                  m2.model_parent(piece) == &g_charModel,
              "with ATTACH_ROOT it hangs on the parent anyway");
        Check(wxl::core::services::m2::Following() && wxl::core::services::m2::Follows(piece),
              "ATTACH_FOLLOW_BONES makes it follow its parent's bones");

        wxl::seam::M2::AnimateMT::After(piece, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(AsFake(piece)->posed == 1, "a following model is posed from its parent once animated");

        FakeModel copy{1, &g_charModel, piece, 0, 0, WXL_FALSE, 0, {}, 0};
        wxl::seam::M2::AnimateMT::After(&copy, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(copy.posed == 1, "an interface copy of a following model follows too");

        FakeModel loaded{1, &g_charModel, nullptr, 0, 0, WXL_FALSE, 0, {}, 0, 0, WXL_FALSE, piece};
        wxl::seam::M2::AnimateMT::After(&loaded, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(loaded.posed == 1, "a loaded copy, which no longer names its source, follows by the file it shares");

        FakeModel other{1, &g_charModel, nullptr, 0, 0, WXL_FALSE, 0, {}, 0};
        wxl::seam::M2::AnimateMT::After(&other, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(other.posed == 0, "any other model is left as the client posed it");

        m2.set_geosets(piece, 1, 0xFFFF, WXL_FALSE);
        Check(AsFake(piece)->geoFirst == 1 && AsFake(piece)->geoLast == 0xFFFF && !AsFake(piece)->geoVisible,
              "geosets are shown and hidden through");
        Check(m2.replace_texture(piece, 2, M2Str("Item\\ObjectComponents\\Waist\\belt.blp")) == WXL_OK &&
                  AsFake(piece)->texType == 2,
              "a texture is replaced through");
        Check(m2.replace_texture(piece, 2, M2Str("missing.blp")) == WXL_E_NOT_FOUND,
              "a texture with no file is not found");
        Check(m2.replace_texture(&stranger, 2, M2Str("a.blp")) == WXL_E_INVALID,
              "nor on a model the service did not make");

        m2.detach_model(piece);
        Check(m2.model_parent(piece) == nullptr && !wxl::core::services::m2::Following(),
              "a detached model hangs on nothing and follows nothing");
        wxl::seam::M2::AnimateMT::After(piece, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(AsFake(piece)->posed == 1, "and is posed no more");

        const int freed = g_fakeModelsFreed;
        m2.release_model(piece);
        Check(g_fakeModelsFreed == freed + 1, "a released model with no parent is freed");
        m2.release_model(piece);
        Check(g_fakeModelsFreed == freed + 1, "releasing it again does nothing");

        // RemoveVisuals strips what hangs on its points: the service's own models are hung back,
        // and copies of them, which the strip freed, are copied again.
        void* robe = m2.create_model(&g_charModel, M2Str("robe.mdx"));
        void* belt = m2.create_model(&g_charModel, M2Str("belt.mdx"));
        m2.attach_model(robe, &g_charModel, 19, WXL_M2_ATTACH_ROOT | WXL_M2_ATTACH_FOLLOW_BONES);
        m2.attach_model(belt, &g_charModel, 53, 0);
        wxl::seam::Component::RemoveVisuals::Before(&g_charModel);
        FakeDetachModel(robe); // what DetachAllChildrenById(19) does
        wxl::seam::Component::RemoveVisuals::After(&g_charModel);
        Check(m2.model_parent(robe) == &g_charModel && AsFake(robe)->attachment == 19 && AsFake(robe)->toRoot &&
                  wxl::core::services::m2::Follows(robe),
              "a stripped model of the service is hung back where it was, still following");
        Check(m2.model_parent(belt) == &g_charModel && g_fakeCopies == 0, "one on a point not stripped is left alone");

        int        panel = 0;
        FakeModel* shown = new FakeModel{1, &panel, nullptr, 0, 0, WXL_FALSE, 0, {}, 0, 19, WXL_TRUE, robe};
        FakeModel* beltCopy = new FakeModel{1, &panel, belt, 0, 0, WXL_FALSE, 0, {}, 0, 53, WXL_FALSE};
        g_fakeLive.push_back(shown);
        g_fakeLive.push_back(beltCopy);
        wxl::seam::Component::RemoveVisuals::Before(&panel);
        std::erase(g_fakeLive, shown); // the strip frees the copy: the panel held its only reference
        delete shown;
        wxl::seam::Component::RemoveVisuals::After(&panel);
        Check(g_fakeCopies == 1 && FakeChildren(&panel, nullptr, 0) == 2,
              "a stripped interface copy is copied again from the model it was made from");
        std::erase(g_fakeLive, beltCopy);
        delete beltCopy;
        for (FakeModel* fake : std::vector<FakeModel*>(g_fakeLive))
            if (fake->parent == &panel)
            {
                std::erase(g_fakeLive, fake);
                delete fake;
            }
        m2.release_model(robe);
        m2.release_model(belt);
        g_fakeModelsFreed -= 2;

        // A point the body lacks, made by hand: posed on one of its key bones, moved by an offset.
        wxl_vec3 hip{};
        Check(m2.attachment_point(&g_charModel, 32, &hip) == WXL_OK && hip.z == 1.1f, "an attachment point is read through");
        Check(m2.attachment_point(&g_charModel, 53, &hip) == WXL_E_NOT_FOUND, "a point the body lacks is not found");
        void*          buckle = m2.create_model(&g_charModel, M2Str("buckle.mdx"));
        const wxl_vec3 at{0.2f, 0.0f, 1.1f};
        Check(m2.attach_to_bone(buckle, &g_charModel, 53, 99, &at) == WXL_E_NOT_FOUND, "a key bone the body lacks is refused");
        Check(m2.attach_to_bone(buckle, &g_charModel, 53, 5, &at) == WXL_OK && m2.model_parent(buckle) == &g_charModel &&
                  AsFake(buckle)->attachment == 53 && AsFake(buckle)->toRoot,
              "a model is hung on a key bone, as the point it stands for");
        wxl::seam::M2::AnimateMT::After(buckle, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(AsFake(buckle)->posed == 100 && g_posedOnBone == 5 && g_posedOffset.x == 0.2f,
              "and is posed on that bone at the offset every frame");
        wxl::seam::M2::AnimateMTSimple::After(buckle, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(AsFake(buckle)->posed == 200, "and so is one the client animates the simple way (one bone, one sequence)");
        AsFake(buckle)->posed = 100;
        FakeModel buckleCopy{1, &g_charModel, nullptr, 0, 0, WXL_FALSE, 0, {}, 0, 53, WXL_TRUE, buckle};
        wxl::seam::M2::AnimateMT::After(&buckleCopy, nullptr, nullptr, nullptr, 1.0f, 1.0f);
        Check(buckleCopy.posed == 100, "an interface copy of it is posed the same way");
        m2.release_model(buckle);
        g_fakeModelsFreed -= 1;
        Check(!wxl::core::services::m2::Following(), "and nothing is posed once it is released");

        void* kept = m2.create_model(&g_charModel, M2Str("b.mdx"));
        m2.attach_model(kept, &g_charModel, 53, WXL_M2_ATTACH_FOLLOW_BONES);
        Check(AsFake(kept)->refs == 2, "an attached model is held by the mod and the parent");
        wxl::core::services::m2::ForgetMod(wxl::core::Host() ? wxl::core::Host()->CurrentMod() : 0);
        Check(g_fakeModelsFreed == freed + 2 && !wxl::core::services::m2::Following(),
              "a disabled mod's models are detached and freed");

        m2.unwatch_equipment(token);
        Check(!wxl::core::services::m2::Watching(), "an unwatched equipment watcher is gone");
        wxl::seam::Component::AddItem::After(&g_component, 4, nullptr, 0);
        Check(seen.count == 3, "and hears nothing more");
    }

}

namespace
{
    // --- M2 loader seams: the model files wxl.m2file hands its watchers --------------------------

    struct SeenFiles
    {
        int        count = 0;
        wxl_m2file last{};
        bool       refuse = false;
        uint32_t   shrinkTo = 0;
    };

    wxl_bool WXL_CALL SeeFile(wxl_m2file* file, void* user)
    {
        SeenFiles* seen = static_cast<SeenFiles*>(user);
        ++seen->count;
        seen->last = *file;
        if (file->stage == WXL_M2FILE_READ)
        {
            static_cast<uint8_t*>(file->data)[0] = 0x4D; // a rewrite in place
            if (seen->shrinkTo) file->size = seen->shrinkTo;
        }
        return seen->refuse ? WXL_FALSE : WXL_TRUE;
    }

    void CheckM2File()
    {
        const wxl_m2file_api& api = wxl::core::services::M2FileApi();
        g_m2File          = FakeM2File{};
        g_m2File.size     = 64;
        g_m2File.setSize  = 0xFFFFFFFFu;

        Check(!wxl::core::services::m2file::Watching(), "nothing watches model files at the start");
        wxl::seam::M2::LoadSucceededCallback::Before(&g_m2File);
        Check(g_m2File.asked == 0, "with no watcher the loader seams do not even ask the client for the file");

        SeenFiles seen;
        uint32_t  token = 0;
        Check(api.watch_files(&SeeFile, &seen, &token) == WXL_OK && token != 0, "a file watcher is accepted and given a token");
        Check(api.watch_files(nullptr, nullptr, nullptr) == WXL_E_INVALID, "a file watcher without a function is refused");

        wxl::seam::M2::LoadSucceededCallback::Before(&g_m2File);
        Check(seen.count == 1 && seen.last.stage == WXL_M2FILE_READ && seen.last.file == &g_m2File &&
                  seen.last.data == g_m2File.bytes && seen.last.size == 64 && seen.last.skin == nullptr &&
                  seen.last.name && std::strcmp(seen.last.name, "creature\\fake\\fake.m2") == 0,
              "a read file reaches the watcher with its name, bytes and size");
        Check(g_m2File.bytes[0] == 0x4D && g_m2File.setSize == 0xFFFFFFFFu,
              "a watcher rewrites the bytes in place, and an unchanged size is left alone");

        seen.shrinkTo = 40;
        wxl::seam::M2::LoadSucceededCallback::Before(&g_m2File);
        Check(g_m2File.setSize == 40, "a size the watcher lowered is handed back to the client");
        seen.shrinkTo = 0;

        seen.refuse = true;
        wxl::seam::M2::LoadSucceededCallback::Before(&g_m2File);
        Check(g_m2File.setSize == 0, "a refused file is left nothing to parse");
        seen.refuse = false;

        wxl::seam::M2::LoadSucceededCallback::Before(nullptr);
        Check(seen.count == 3, "no file, nothing handed on");

        wxl::seam::M2::Initialize::Before(&g_m2File);
        Check(seen.count == 4 && seen.last.stage == WXL_M2FILE_PARSED && seen.last.data == g_m2File.bytes,
              "a parsed file reaches the watcher before it is initialized");

        wxl::seam::M2::InitializeSkinProfile::Before(&g_m2File);
        Check(seen.count == 4, "no skin profile, no skin stage");
        g_m2File.hasSkin = true;
        wxl::seam::M2::InitializeSkinProfile::Before(&g_m2File);
        Check(seen.count == 5 && seen.last.stage == WXL_M2FILE_SKIN && seen.last.skin == g_m2File.skin,
              "a parsed skin profile reaches the watcher with its header");

        uint8_t anim[32]{};
        wxl::seam::M2::FinishLoadingLowPrioritySequence::Before(&g_m2File, 7, anim, sizeof anim);
        Check(seen.count == 6 && seen.last.stage == WXL_M2FILE_SEQUENCE && seen.last.sequence == anim &&
                  seen.last.sequence_size == sizeof anim && seen.last.sequence_index == 7,
              "an external sequence reaches the watcher with its bytes and index");

        api.animate_bones(&g_m2File);
        Check(g_m2File.animated == 1, "animate_bones reaches the client's file");

        uint8_t* first  = static_cast<uint8_t*>(api.alloc_near(&g_m2File, 100));
        uint8_t* second = static_cast<uint8_t*>(api.alloc_near(&g_m2File, 20));
        bool     zero   = first != nullptr;
        for (int i = 0; first && i < 100; ++i) zero = zero && first[i] == 0;
        const auto reach = [](const void* a, const void* b) {
            const int64_t d = static_cast<int64_t>(reinterpret_cast<uintptr_t>(a)) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(b));
            return d > -0x40000000LL && d < 0x40000000LL;
        };
        Check(first && second && zero && (reinterpret_cast<uintptr_t>(first) & 15) == 0 &&
                  (reinterpret_cast<uintptr_t>(second) & 15) == 0 && second >= first + 100 &&
                  reach(first, &g_m2File) && reach(second, &g_m2File),
              "alloc_near hands out zeroed, aligned, distinct memory within reach of the anchor");
        Check(api.alloc_near(nullptr, 8) == nullptr && api.alloc_near(&g_m2File, 0) == nullptr,
              "alloc_near refuses no anchor and no size");

        api.unwatch_files(token);
        Check(!wxl::core::services::m2file::Watching(), "an unwatched file watcher is gone");
        wxl::seam::M2::Initialize::Before(&g_m2File);
        Check(seen.count == 6, "and hears nothing more");
        wxl::core::services::m2file::Clear();
    }
}

int main(int argc, char** argv)
{
    wxl::core::binding::Bind(FakeInit());

    wxl::host::HostConfig config;
    config.runtime_id   = "harness";
    config.runtime_name = "seam harness";
    config.events       = wxl::core::seams::RaisedEvents();
    config.services.push_back(wxl::host::PublishedService{
        WXL_SCRIPT_SERVICE, WXL_SCRIPT_SERVICE_VERSION, &wxl::core::services::ScriptApi()});
    g_host              = std::make_unique<wxl::host::ModHost>(std::move(config));

    printf("seam harness: %zu seam(s), %zu raisable event(s), %zu binding row(s) left unbound by the fake\n",
           wxl::core::seams::Table().size(), wxl::core::seams::RaisedEvents().size(),
           wxl::core::binding::Unbound().size());

    CheckStages();
    CheckInitRows();
    CheckEveryRaisedEventIsSubscribable();
    CheckEveryRowDeclaresItsAbi();
    CheckEventDispatchUpdate();
    CheckWorldPreload();
    CheckObjectMgr();
    CheckUnit();
    CheckPlayer();
    CheckGameUI();
    CheckMapArea();
    CheckFrameScript();
    CheckNetClient();
    CheckNetConnection();
    CheckClientDestroy();
    CheckInput();
    CheckStorage();
    CheckSound();
    CheckStorageService();
    CheckDataService();
    CheckRender();
    CheckSpell();
    CheckSpellRequest();
    CheckSpellProgress();
    CheckSpellCooldown();
    CheckSpellCancel();
    CheckCombat();
    CheckUiEvent();
    CheckScriptError();
    CheckM2();
    CheckM2Service();
    CheckM2File();
#if defined(_M_IX86)
    CheckThiscallShim();
    CheckOneSidedShim();
    CheckThiscallGate();
    CheckRegparmThunks();
#endif

    printf("\n%d failure(s)\n", g_failures);
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--bench") == 0) Bench();

    g_host.reset();
    return g_failures ? 1 : 0;
}
