#pragma once

#include <openvr.h>

#include <string>

namespace fh2vr {

// Captures the latest visible FH2 frame. In mono mode the same texture is sent
// to both eyes. In alternating-eye mode the frame is stored for sourceEye and
// the latest retained left/right textures are submitted together. swapEyes
// swaps only the final compositor assignment, making F11 a real L/R diagnostic.
bool submit_headset_preview(vr::IVRCompositor *compositor, bool alternatingEyes, int sourceEye,
                            bool swapEyes, std::string &error);
void shutdown_headset_preview();

} // namespace fh2vr
