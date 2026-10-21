// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The wxl.m2 service: character appearances and the models mods hang on them, for the x64 build.

#pragma once

#include <cstdint>

#include "wxl/services/m2.h"

namespace wxl::core::services
{
    /// The table, WXL_M2_SERVICE version WXL_M2_SERVICE_VERSION.
    const wxl_m2_api& M2Api();

    namespace m2
    {
        /// How a model wxl.m2 attached is posed each frame, after the client posed it itself.
        struct Pose
        {
            enum Kind : uint32_t
            {
                None,   ///< as the client poses it
                Bones,  ///< each bone as the parent's bone it matches (ATTACH_FOLLOW_BONES)
                OnBone, ///< every bone as one key bone of the parent, moved by an offset (attach_to_bone)
            };
            Kind     kind = None;
            uint32_t keyBone = 0;
            wxl_vec3 offset{};
        };

        /// How `model`, or the model it is an interface copy of, is posed; false for not at all.
        /// Read on the animate threads, while nothing changes the set.
        bool PoseOf(void* model, Pose& out);

        /// Whether any mod watches equipment: the Component seams' whole cost when none does.
        bool Watching();

        /// Hands a change to every equipment watcher, each under the guard.
        void DispatchEquipment(const wxl_m2_equipment& change);

        /// Whether any model follows its parent's bones: the AnimateMT seam's whole cost when none.
        bool Following();

        /// Whether `model`, or the model it was duplicated from, follows its parent's bones. Read
        /// on the animate threads, while nothing changes the set.
        bool Follows(void* model);

        /// Whether any mod holds a model it made: the RemoveVisuals seam's whole cost when none does.
        bool HasModels();

        /// Whether `model` is one this service made and a mod still holds.
        bool Owns(void* model);

        /// The model this service made that `model` is, or is an interface copy of; null for none.
        void* SourceOf(void* model);

        /// Drops every watcher a mod registered and every model it made, when it is disabled.
        void ForgetMod(uint32_t mod);

        /// Forgets everything, on the way out.
        void Clear();
    }
}
