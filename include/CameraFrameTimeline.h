#pragma once

namespace fh2vr {
// M11.x research only: bounded, read-only CPU markers from existing hooks.
// These callbacks never submit a second view or alter the camera.
void timeline_note_probe_installed(bool installed);
void timeline_note_camera_before(bool matrixAvailable, float viewTx, float viewTy, float viewTz);
void timeline_note_camera(bool matrixAvailable, float viewTx, float viewTy, float viewTz);
void timeline_note_execute(unsigned commandListCount);
void timeline_note_present(unsigned syncInterval, unsigned flags);
} // namespace fh2vr
