#pragma once

#include <openvr.h>

#include <string>

namespace fh2vr {

// Submits the newest retained FH2 D3D12 backbuffer copy as the same mono frame
// to both OpenVR eyes. The texture is submitted with the compositor render pose
// so SteamVR can apply rotational reprojection while we validate the native
// renderer path before adding depth reprojection/stereo synthesis.
bool submit_direct_d3d12_mono_preview(vr::IVRCompositor *compositor, std::string &error);

} // namespace fh2vr
