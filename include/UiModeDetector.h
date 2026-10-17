#pragma once

#include <cstdint>

namespace fh2vr {

enum class UiMode {
    Unknown,
    PauseMenu,
    PhotoMode,
    WorldMap,
    HorizonHub,
    Garage,
    Loading,
};

void set_ui_game_base(std::uintptr_t gameBase);
UiMode read_ui_mode();
const char *ui_mode_name(UiMode mode);
bool ui_mode_wants_flat_overlay(UiMode mode);

} // namespace fh2vr
