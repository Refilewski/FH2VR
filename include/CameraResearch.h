#pragma once

#include <cstdint>
#include <cstring>

namespace fh2vr {

// M11 research-only instrumentation. A caller may report a live camera pointer
// here; the logger itself also resolves the camera through the known FH2 chain.
void camera_research_observe(void *camera);

// Starts a short, read-only camera/matrix capture. No VR runtime is required.
bool start_camera_research(std::uintptr_t gameBase);
void stop_camera_research();

} // namespace fh2vr
