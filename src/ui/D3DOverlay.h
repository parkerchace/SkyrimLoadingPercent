#pragma once

namespace D3DOverlay {
    // Call from kDataLoaded — creates a dummy D3D11 device to get the Present
    // vtable slot, then hooks it.  ImGui is initialised on the first Present call
    // using the real swap chain's device, so no timing issues.
    bool Install();
}
