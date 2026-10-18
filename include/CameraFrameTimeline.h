#pragma once

namespace fh2vr {
// M11.3 only: bounded, read-only, no-file-IO event markers.
// Call exclusively from the already installed hooks.
void timeline_note_probe_installed(bool installed);
void timeline_note_camera(bool matrixAvailable, float viewTx, float viewTy, float viewTz);
void timeline_note_execute(unsigned commandListCount);
void timeline_note_present(unsigned syncInterval, unsigned flags);
} // namespace fh2vr
