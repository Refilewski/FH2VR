#include "UiModeDetector.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr std::uintptr_t kAudioEngineRva = 0x0365D3D8;
constexpr std::uintptr_t kRegistryPointerOffset = 0x630;
constexpr std::uintptr_t kLoadingRecordOffset = 0x7A0;
constexpr std::size_t kLoadingRecordBytes = 29;
constexpr std::uintptr_t kPauseRecordOffset = 0xC00;
constexpr std::size_t kPauseRecordBytes = 28;
constexpr std::uintptr_t kMapRecordOffset = 0xE80;
constexpr std::size_t kMapRecordBytes = 32;

constexpr std::uintptr_t kPlayerManagerRva = 0x034ABA60;
constexpr std::uintptr_t kGameManagerPointerOffset = 0x48;
constexpr std::uintptr_t kGameManagerVtableRva = 0x02C5F8C8;
constexpr std::size_t kGameManagerBytes = 0x40;
constexpr std::uintptr_t kSimulationManagerPointerOffset = 0x50;
constexpr std::uintptr_t kSimulationManagerVtableRva = 0x02C5FD70;
constexpr std::size_t kSimulationManagerBytes = 0x28;
constexpr std::size_t kAudioFrontendBytes = 0x48;

constexpr std::uint32_t kFloatOneBits = 0x3F800000u;
constexpr std::uint32_t kMinus80Bits = 0xC2A00000u;

std::atomic<std::uintptr_t> g_gameBase{0};

bool checked_add(std::uintptr_t base, std::uintptr_t offset, std::uintptr_t &out) {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - base) {
        return false;
    }
    out = base + offset;
    return true;
}

bool pointer_value(std::uintptr_t value) {
    constexpr std::uintptr_t kMin = 0x10000;
    constexpr std::uintptr_t kMax = 0x00007FFFFFFFE000ull;
    return value >= kMin && value <= kMax && (value & 7u) == 0;
}

bool memory_readable(const void *address, std::size_t size) {
    if (!address || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto regionStart = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto regionEnd = regionStart + info.RegionSize;
    return start >= regionStart && start + size >= start && start + size <= regionEnd;
}

bool read_exact(std::uintptr_t address, void *output, std::size_t size) {
    if (!output || !memory_readable(reinterpret_cast<const void *>(address), size)) {
        return false;
    }
    std::memcpy(output, reinterpret_cast<const void *>(address), size);
    return true;
}

bool read_pointer(std::uintptr_t address, std::uintptr_t &value) {
    value = 0;
    return read_exact(address, &value, sizeof(value)) && pointer_value(value);
}

std::uint32_t u32(const unsigned char *bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

std::int32_t i32(const unsigned char *bytes, std::size_t offset) {
    std::int32_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

std::uintptr_t uptr(const unsigned char *bytes, std::size_t offset) {
    std::uintptr_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

template <std::size_t N>
bool stable_read(std::uintptr_t address, std::array<unsigned char, N> &out) {
    std::array<unsigned char, N> second{};
    if (!read_exact(address, out.data(), out.size()) ||
        !read_exact(address, second.data(), second.size())) {
        return false;
    }
    return out == second;
}

struct FrontendState {
    bool valid = false;
    bool hubFamily = false;
};

struct HorizonFlowState {
    bool valid = false;
    std::int32_t primary = 0;
    std::int32_t secondary = 0;
};

struct GameManagerState {
    bool valid = false;
    bool loading = false;
};

FrontendState read_frontend(std::uintptr_t gameBase) {
    std::uintptr_t audioSlot = 0;
    if (!checked_add(gameBase, kAudioEngineRva, audioSlot)) {
        return {};
    }

    std::uintptr_t audio = 0;
    if (!read_pointer(audioSlot, audio)) {
        return {};
    }

    std::array<unsigned char, kAudioFrontendBytes> bytes{};
    if (!stable_read(audio, bytes)) {
        return {};
    }

    std::uintptr_t verifyAudio = 0;
    if (!read_pointer(audioSlot, verifyAudio) || verifyAudio != audio) {
        return {};
    }

    const bool guards =
        i32(bytes.data(), 0x30) == 0 &&
        i32(bytes.data(), 0x34) == 0 &&
        u32(bytes.data(), 0x3C) == kMinus80Bits &&
        u32(bytes.data(), 0x40) == kMinus80Bits &&
        u32(bytes.data(), 0x44) == kMinus80Bits;
    if (!guards) {
        return {};
    }

    const std::uint32_t marker = u32(bytes.data(), 0x38);
    if (marker != 0u && marker != kMinus80Bits) {
        return {};
    }

    return {true, marker == 0u};
}

HorizonFlowState read_flow(std::uintptr_t gameBase) {
    std::uintptr_t playerSlot = 0;
    if (!checked_add(gameBase, kPlayerManagerRva, playerSlot)) {
        return {};
    }

    std::uintptr_t player = 0;
    if (!read_pointer(playerSlot, player)) {
        return {};
    }

    std::uintptr_t managerSlot = 0;
    if (!checked_add(player, kSimulationManagerPointerOffset, managerSlot)) {
        return {};
    }

    std::uintptr_t manager = 0;
    if (!read_pointer(managerSlot, manager)) {
        return {};
    }

    std::array<unsigned char, kSimulationManagerBytes> bytes{};
    if (!stable_read(manager, bytes)) {
        return {};
    }

    std::uintptr_t verifyManager = 0;
    std::uintptr_t verifyPlayer = 0;
    if (!read_pointer(managerSlot, verifyManager) || verifyManager != manager ||
        !read_pointer(playerSlot, verifyPlayer) || verifyPlayer != player) {
        return {};
    }

    std::uintptr_t expectedVtable = 0;
    if (!checked_add(gameBase, kSimulationManagerVtableRva, expectedVtable)) {
        return {};
    }

    const bool valid =
        uptr(bytes.data(), 0) == expectedVtable &&
        i32(bytes.data(), 0x18) == 1 &&
        i32(bytes.data(), 0x24) == 5;
    if (!valid) {
        return {};
    }

    return {true, i32(bytes.data(), 0x10), i32(bytes.data(), 0x20)};
}

GameManagerState read_game_manager(std::uintptr_t gameBase) {
    std::uintptr_t playerSlot = 0;
    if (!checked_add(gameBase, kPlayerManagerRva, playerSlot)) {
        return {};
    }

    std::uintptr_t player = 0;
    if (!read_pointer(playerSlot, player)) {
        return {};
    }

    std::uintptr_t managerSlot = 0;
    if (!checked_add(player, kGameManagerPointerOffset, managerSlot)) {
        return {};
    }

    std::uintptr_t manager = 0;
    if (!read_pointer(managerSlot, manager)) {
        return {};
    }

    std::array<unsigned char, kGameManagerBytes> bytes{};
    if (!stable_read(manager, bytes)) {
        return {};
    }

    std::uintptr_t verifyManager = 0;
    std::uintptr_t verifyPlayer = 0;
    if (!read_pointer(managerSlot, verifyManager) || verifyManager != manager ||
        !read_pointer(playerSlot, verifyPlayer) || verifyPlayer != player) {
        return {};
    }

    std::uintptr_t expectedVtable = 0;
    if (!checked_add(gameBase, kGameManagerVtableRva, expectedVtable) ||
        uptr(bytes.data(), 0) != expectedVtable) {
        return {};
    }

    const bool guards =
        bytes[0x28] == 0 &&
        bytes[0x29] == 1 &&
        bytes[0x2A] == 1 &&
        bytes[0x2B] == 1 &&
        bytes[0x2C] == 1 &&
        bytes[0x2F] == 0;
    if (!guards) {
        return {};
    }

    const unsigned char a = bytes[0x2D];
    const unsigned char b = bytes[0x2E];
    if (!((a == 0 && b == 0) || (a == 1 && b == 1))) {
        return {};
    }

    return {true, a == 0 && b == 0};
}

template <std::size_t N, std::size_t M>
bool record_name_is(const std::array<unsigned char, N> &record,
                    const char (&name)[M]) {
    static_assert(N >= 16 + M);
    return std::memcmp(record.data() + 16, name, M) == 0;
}

fh2vr::UiMode decode_loading(const std::array<unsigned char, kLoadingRecordBytes> &record) {
    constexpr char kName[] = "BASE_Loading";
    if (u32(record.data(), 0) != kFloatOneBits ||
        i32(record.data(), 12) != 1 ||
        !record_name_is(record, kName) ||
        u32(record.data(), 4) != kFloatOneBits) {
        return fh2vr::UiMode::Unknown;
    }
    return fh2vr::UiMode::Loading;
}

fh2vr::UiMode decode_pause(const std::array<unsigned char, kPauseRecordBytes> &record) {
    constexpr char kName[] = "BASE_Paused";
    if (u32(record.data(), 0) != kFloatOneBits ||
        i32(record.data(), 12) != 2 ||
        !record_name_is(record, kName) ||
        u32(record.data(), 4) != kFloatOneBits) {
        return fh2vr::UiMode::Unknown;
    }

    switch (i32(record.data(), 8)) {
    case 1:
        return fh2vr::UiMode::PauseMenu;
    case 2:
        return fh2vr::UiMode::PhotoMode;
    default:
        return fh2vr::UiMode::Unknown;
    }
}

fh2vr::UiMode read_mixer_mode(std::uintptr_t gameBase) {
    std::uintptr_t audioSlot = 0;
    if (!checked_add(gameBase, kAudioEngineRva, audioSlot)) {
        return fh2vr::UiMode::Unknown;
    }

    std::uintptr_t audio = 0;
    if (!read_pointer(audioSlot, audio)) {
        return fh2vr::UiMode::Unknown;
    }

    std::uintptr_t registrySlot = 0;
    if (!checked_add(audio, kRegistryPointerOffset, registrySlot)) {
        return fh2vr::UiMode::Unknown;
    }

    std::uintptr_t registry = 0;
    if (!read_pointer(registrySlot, registry)) {
        return fh2vr::UiMode::Unknown;
    }

    std::uintptr_t loadingAddress = 0;
    std::uintptr_t pauseAddress = 0;
    std::uintptr_t mapAddress = 0;
    if (!checked_add(registry, kLoadingRecordOffset, loadingAddress) ||
        !checked_add(registry, kPauseRecordOffset, pauseAddress) ||
        !checked_add(registry, kMapRecordOffset, mapAddress)) {
        return fh2vr::UiMode::Unknown;
    }

    std::array<unsigned char, kLoadingRecordBytes> loading{};
    std::array<unsigned char, kPauseRecordBytes> pause{};
    std::array<unsigned char, kMapRecordBytes> map{};
    if (!stable_read(loadingAddress, loading) ||
        !stable_read(pauseAddress, pause) ||
        !stable_read(mapAddress, map)) {
        return fh2vr::UiMode::Unknown;
    }

    std::uintptr_t verifyRegistry = 0;
    std::uintptr_t verifyAudio = 0;
    if (!read_pointer(registrySlot, verifyRegistry) || verifyRegistry != registry ||
        !read_pointer(audioSlot, verifyAudio) || verifyAudio != audio) {
        return fh2vr::UiMode::Unknown;
    }

    if (decode_loading(loading) == fh2vr::UiMode::Loading) {
        return fh2vr::UiMode::Loading;
    }

    constexpr char kPauseName[] = "BASE_Paused";
    constexpr char kMapName[] = "BASE_Map_Paused";
    if (u32(pause.data(), 0) != kFloatOneBits ||
        i32(pause.data(), 12) != 2 ||
        !record_name_is(pause, kPauseName) ||
        u32(map.data(), 0) != kFloatOneBits ||
        i32(map.data(), 12) != 2 ||
        !record_name_is(map, kMapName)) {
        return fh2vr::UiMode::Unknown;
    }

    const std::uint32_t pauseActive = u32(pause.data(), 4);
    const std::int32_t pauseSubmode = i32(pause.data(), 8);
    const std::uint32_t mapActive = u32(map.data(), 4);
    const std::int32_t mapSubmode = i32(map.data(), 8);

    const bool pauseInactive = pauseActive == 0u && pauseSubmode == 0;
    const bool mapInactive = mapActive == 0u && mapSubmode == 0;
    const bool mapPositive = mapActive == kFloatOneBits && mapSubmode == 1;

    if (pauseInactive && mapPositive) {
        return fh2vr::UiMode::WorldMap;
    }
    if (mapInactive) {
        return decode_pause(pause);
    }
    return fh2vr::UiMode::Unknown;
}

} // namespace

namespace fh2vr {

void set_ui_game_base(std::uintptr_t gameBase) {
    g_gameBase.store(gameBase, std::memory_order_release);
}

UiMode read_ui_mode() {
    const std::uintptr_t gameBase = g_gameBase.load(std::memory_order_acquire);
    if (!pointer_value(gameBase)) {
        return UiMode::Unknown;
    }

    const FrontendState frontend = read_frontend(gameBase);
    const HorizonFlowState flow = read_flow(gameBase);
    const GameManagerState manager = read_game_manager(gameBase);

    if (frontend.valid && frontend.hubFamily && flow.valid) {
        if (flow.primary == 3 && flow.secondary >= 0 && flow.secondary <= 0x40) {
            return UiMode::Garage;
        }
        if (flow.primary == 2 && flow.secondary == 0) {
            return UiMode::HorizonHub;
        }
    }

    if (manager.valid && manager.loading) {
        return UiMode::Loading;
    }

    return read_mixer_mode(gameBase);
}

const char *ui_mode_name(UiMode mode) {
    switch (mode) {
    case UiMode::PauseMenu:
        return "PauseMenu";
    case UiMode::PhotoMode:
        return "PhotoMode";
    case UiMode::WorldMap:
        return "WorldMap";
    case UiMode::HorizonHub:
        return "HorizonHub";
    case UiMode::Garage:
        return "Garage";
    case UiMode::Loading:
        return "Loading";
    case UiMode::Unknown:
    default:
        return "Unknown";
    }
}

bool ui_mode_wants_flat_overlay(UiMode mode) {
    return mode != UiMode::Unknown;
}

} // namespace fh2vr
