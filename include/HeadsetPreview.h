#pragma once

#include <openvr.h>

#include <string>

namespace fh2vr {

bool submit_mono_headset_preview(vr::IVRCompositor *compositor, std::string &error);
void shutdown_mono_headset_preview();

} // namespace fh2vr
