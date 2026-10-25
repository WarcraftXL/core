// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The wxl.m2 service: the models objects draw with, and the models mods make and hang on them.

#pragma once

#include <cstdint>

#include "wxl/services/m2.h"

namespace wxl::core::services
{
    /// The table, WXL_M2_SERVICE version WXL_M2_SERVICE_VERSION.
    const wxl_m2_api& M2Api();

    namespace m2
    {
        /// Whether any model follows its parent's bones: the animate seams' whole cost when none.
        bool Following();

        /// Poses `model` after the client posed it, when it is a model a mod hung to follow its
        /// parent, or an interface copy of one. Called on the animate threads, while nothing
        /// changes the set of models: the mod calls that change it run on the main thread, which
        /// waits for the animate threads inside CM2Scene::Animate.
        void PoseAttached(void* model);

        /// Whether any mod watches bones: the AnimateMT seam's whole cost for them when none does.
        bool WatchingBones();

        /// Hands `model`'s bones, just posed, to every bone watcher that wants its file. Animate
        /// threads.
        void DispatchBones(void* model);

        /// Whether any mod holds a model it made: the RemoveVisuals seam's whole cost when none.
        bool HasModels();

        /// The model this service made that `model` is, or is an interface copy of; null for none.
        void* SourceOf(void* model);

        /// Detaches and gives back every model a mod made, when it is disabled.
        void ForgetMod(uint32_t mod);

        /// Forgets everything, on the way out: the client's models go with it, untouched.
        void Clear();
    }
}
