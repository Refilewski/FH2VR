#pragma once

namespace fh2vr {

// Called by the active renderer preview whenever it has a fresh OpenVR
// projection. The FOV hook uses this target only while preview submission is
// active, so normal monitor rendering is left alone when F9 preview is off.
void update_fov_match_target(float sourceVerticalFovRadians,
                             float sourceHorizontalFovRadians,
                             float sourceAspect,
                             float combinedVerticalFovRadians,
                             float combinedHorizontalFovRadians);

} // namespace fh2vr

// Renderer milestone files keep most implementation helpers in an anonymous
// namespace. Make the public FH2VR FOV-target function available there without
// duplicating the declaration or implementation.
using fh2vr::update_fov_match_target;
