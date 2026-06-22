#pragma once

namespace ScaleformManager {
    void RegisterMenuSink();
    void ReleaseLoadingMenuHold();  // called by DrawOverlay when linger/hold conditions are met
}
