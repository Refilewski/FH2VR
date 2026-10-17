#pragma once

#include <openvr.h>
#include <windows.h>

#include <string>

// M9 still uses the historical VK_F5 token internally. Remap that token at
// compile time so the only public/manual GUI key is F2. F5 no longer triggers
// FH2VR. This can disappear once M9 is folded into the next renderer cleanup.
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
