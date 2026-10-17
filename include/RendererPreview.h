#pragma once

#include <openvr.h>
#include <windows.h>

#include <string>

// The active M9 renderer historically used VK_F5 for its manual GUI toggle.
// Public controls are now intentionally limited to F1/F2, so remap that token
// to F2 at compile time. Physical F5 no longer triggers FH2VR.
#ifdef VK_F5
#undef VK_F5
#endif
#define VK_F5 VK_F2

namespace fh2vr {

// Submits the newest retained FH2 D3D12 backbuffer copy to OpenVR. The active
// renderer handles projection/FOV matching, reprojection and the GUI overlay.
bool submit_direct_d3d12_mono_preview(vr::IVRCompositor *compositor,
                                      std::string &error);

} // namespace fh2vr
