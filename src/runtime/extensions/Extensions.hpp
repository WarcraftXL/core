// SPDX-License-Identifier: GPL-3.0-or-later
// Loading the extensions of Extensions\, in two stages of the client's startup.

#pragma once

namespace wxl::runtime::extensions
{
    /// Publishes the core's services and attaches the two load seams (OpenArchives for "before-archives",
    /// engine init for the rest), from DllMain; extensions load in folder-name order, a failure stops none.
    bool InstallLoader();

    /// Loads the engine-stage extensions on the first call. The device-create seam calls it too: the
    /// client builds its graphics device before it reaches engine initialisation, and a device
    /// extension must be there first.
    void EnsureLoaded();
}
