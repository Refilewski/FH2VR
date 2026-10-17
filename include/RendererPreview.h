#pragma once

#include <openvr.h>

#include <string>

namespace fh2vr {

// M9 compatibility entrypoint retained inside RendererM10.cpp.
bool submit_direct_d3d12_mono_preview(vr::IVRCompositor *compositor,
                                      std::string &error);

// Active M10 path: M9 capture/UI plus positional HMD parallax.
bool submit_direct_d3d12_mono_preview_m10(vr::IVRCompositor *compositor,
                                          std::string &error);

} // namespace fh2vr
