#pragma once

#include <cstdint>

namespace fh2vr {

// M11 research-only instrumentation. The camera hook reports the live Driver
// camera pointer here; a separate worker snapshots it so no file I/O happens
// on the game's camera-update thread.
void camera_research_observe(void *camera);

// Starts a short, read-only camera/matrix capture. No VR runtime is required.
bool start_camera_research(std::uintptr_t gameBase);
void stop_camera_research();

} // namespace fh2vr
