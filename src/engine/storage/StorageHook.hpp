// SPDX-License-Identifier: GPL-3.0-or-later
// The detours on the client's file opens and reads that let redirects, providers and transforms answer.

#pragma once

namespace wxl::storage
{
    /// Attaches the open, size, read, seek and close detours; before the batch is enabled. An open no
    /// extension claims reads natively, as stock.
    void Install();

    /// Publishes wxl.storage, the registrations of Providers.hpp for extensions.
    void Publish();
}
