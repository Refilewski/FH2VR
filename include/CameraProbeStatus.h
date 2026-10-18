#pragma once

namespace fh2vr {
// Research-only, read-only inspection of the Driver-camera vtable slot.
//  1 = the M11 camera probe still occupies the slot
//  0 = the slot is readable but points elsewhere (hook replaced/missing)
// -1 = the probe was not installed or the slot is not safely readable
int camera_probe_slot_state();
// M11.7 only: sample cached FH2 view translation without mutating game state.
// Called only for selected game-owned command-list submission markers.
bool camera_probe_read_cached_view(float &tx, float &ty, float &tz);
} // namespace fh2vr
