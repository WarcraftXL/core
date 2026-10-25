// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.storage's hash_file and hash_file_async: the MD5 of a file as the client would read it,
// from the layer that holds it, kept by name and layer.

#pragma once

#include "wxl/services/storage.h"

#include <cstdint>

namespace wxl::core::services::hashes
{
    /**
     * @brief Hashes a file, any thread: what wxl_storage_api::hash_file answers.
     *
     * The name is resolved as the client's open resolves it (a served database, an override, a
     * redirect, a scope) and read in the asset pipeline's Inner mode: the layers answer, the
     * transforms and the events do not. @p out's source is valid until the thread's next call.
     */
    wxl_result Hash(const char* name, uint32_t flags, wxl_file_hash* out);

    /// Queues a hash for the runtime's thread; its answer goes to @p fn at a later Pump.
    wxl_result HashAsync(const char* name, uint32_t flags, wxl_file_hash_fn fn, void* user, uint64_t* out,
                         uint32_t mod);

    /// Hands the answers that are ready to their mods. The main thread, once a tick.
    void Pump();

    /// Drops what a mod asked for: it is disabled or unloaded.
    void ForgetMod(uint32_t mod);

    /// Forgets every hash kept: the harness, between checks.
    void ForgetCache();

    /// Stops the thread, waiting for the hash it is on, and drops every request. The way out.
    void Stop();
}
