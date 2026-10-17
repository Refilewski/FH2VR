#pragma once

#include <openvr.h>

#include <string>

namespace fh2vr {

// M2 headset preview entry point. The implementation now routes the newest
// retained native D3D12 backbuffer frame to both eyes through OpenVR DirectX12
// with Submit_TextureWithPose. The legacy parameters remain temporarily so the
// tracking/control code does not need to change while AER is retired.
bool submit_headset_preview(vr::IVRCompositor *compositor, bool alternatingEyes, int sourceEye,
                            bool swapEyes, std::string &error);

// Legacy GDI/AER diagnostic sampling is disabled in M2.
bool get_headset_preview_mean_diff(double &meanDiff);

void shutdown_headset_preview();

} // namespace fh2vr
