// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.storage service: the client's files, over the binding, the same on both builds.

#pragma once

#include "wxl/services/storage.h"

#include <cstddef>
#include <cstdint>

namespace wxl::core::services
{
    /**
     * @brief The table, WXL_STORAGE_SERVICE version WXL_STORAGE_SERVICE_VERSION.
     *
     * Its struct_size stops short of the rows this runtime cannot answer: a mod reading it
     * through the SDK's facade is told they are unsupported rather than calling into nothing.
     */
    const wxl_storage_api& StorageApi();

    namespace storage
    {
        /// Closes every file a mod left open and forgets its handles. Called on the way out.
        void CloseEverything();

        /// The same for one mod, when it is disabled or unloaded.
        void CloseFilesOf(uint32_t mod);

        /**
         * @brief The folder of the search path that holds @p name and wins it among folders: the
         *        highest priority one the client can open from.
         *
         * True with @p out set to the name the client opens that file by -- its path relative
         * to the client's folder behind two backslashes, which the client reads from the disk at
         * its base path without looking among its loose files (absolute when it has no base
         * path) -- and @p priority to the folder's. Whether the folder wins over the client's own
         * data and the providers is the FindFile gate's to say. False, nothing written, otherwise.
         */
        bool BestFolder(const char* name, char* out, size_t capacity, int32_t* priority);

        /// Whether any folder is in the search path: one load.
        bool AnyFolder();

        /**
         * @brief The name a mod's redirect opens in place of @p name, from any thread.
         *
         * True, with the latest redirect's name in @p out, when a mod redirected @p name; false
         * and nothing written otherwise. One load when no name is redirected.
         */
        bool Redirected(const char* name, char* out, size_t capacity);

        /// Takes a mod's redirects back: it is disabled or unloaded.
        void RemoveRedirectsOf(uint32_t mod);

        /**
         * @brief The name a mod's override puts in place of @p name, from any thread.
         *
         * True, with the latest override's name in @p out, when a mod overrides @p name; false and
         * nothing written otherwise. One load when no name is overridden.
         */
        bool Overridden(const char* name, char* out, size_t capacity);

        /// Takes a mod's overrides back: it is disabled or unloaded.
        void RemoveOverridesOf(uint32_t mod);

        /// Whether a name resolves, in the client's archives, a folder of the search path or a source.
        bool Exists(const char* name);

        /// Whether this thread is listing the archives for find: the opens of their listfiles
        /// are the runtime's own, not the client's.
        bool Listing();

        /// Takes the folders a mod added out of the search: it is disabled or unloaded.
        void RemoveSearchPathsOf(uint32_t mod);

        /// Forgets the search path. Called on the way out, and by the harness.
        void ForgetSearchPaths();
    }
}
