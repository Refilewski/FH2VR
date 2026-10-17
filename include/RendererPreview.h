#pragma once

#include <openvr.h>

#include <string>

namespace fh2vr {

// Submits the newest retained FH2 D3D12 backbuffer copy to OpenVR. The active
// renderer handles projection/FOV matching, reprojection and the GUI overlay.
bool submit_direct_d3d12_mono_preview(vr::IVRCompositor *compositor,
                                      std::string &error);

} // namespace fh2vr
