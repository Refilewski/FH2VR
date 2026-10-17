#include "HeadsetPreview.h"
#include "RendererPreview.h"

namespace fh2vr {

bool submit_headset_preview(vr::IVRCompositor *compositor, bool alternatingEyes, int sourceEye,
                            bool swapEyes, std::string &error) {
    // M2 intentionally disables the old GDI/BitBlt preview path. The newest
    // retained D3D12 backbuffer frame is submitted as the same mono image to
    // both eyes while we validate native D3D12 -> OpenVR and compositor pose
    // reprojection. AER/sourceEye/swapEyes are ignored in this milestone.
    (void)alternatingEyes;
    (void)sourceEye;
    (void)swapEyes;
    return submit_direct_d3d12_mono_preview(compositor, error);
}

bool get_headset_preview_mean_diff(double &meanDiff) {
    // M2 is deliberately mono and no longer maintains CPU/GDI image samples.
    meanDiff = 0.0;
    return false;
}

void shutdown_headset_preview() {
    // Renderer-owned D3D12 resources live for the process lifetime in M2.
}

} // namespace fh2vr
