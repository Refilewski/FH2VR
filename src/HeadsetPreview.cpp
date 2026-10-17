#include "HeadsetPreview.h"
#include "RendererPreview.h"

namespace fh2vr {

bool submit_headset_preview(vr::IVRCompositor *compositor, bool alternatingEyes, int sourceEye,
                            bool swapEyes, std::string &error) {
    // The current path is mono scene rendering with M10 projection/FOV,
    // roll-neutral pose, positional HMD parallax and the layered GUI path.
    // Legacy AER/sourceEye/swapEyes inputs are intentionally ignored.
    (void)alternatingEyes;
    (void)sourceEye;
    (void)swapEyes;
    return submit_direct_d3d12_mono_preview_m10(compositor, error);
}

bool get_headset_preview_mean_diff(double &meanDiff) {
    meanDiff = 0.0;
    return false;
}

void shutdown_headset_preview() {
    // Renderer-owned D3D12 resources live for the process lifetime.
}

} // namespace fh2vr
