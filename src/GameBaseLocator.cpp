#include "GameBaseLocator.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr std::uintptr_t kPreferredBase = 0x140000000ull;
constexpr std::uint16_t kMachineAmd64 = 0x8664;
constexpr std::uint16_t kPe32PlusMagic = 0x020B;
constexpr std::uint32_t kTimestamp = 0x565501ED;
constexpr std::uint32_t kSizeOfImage = 0x03A18000;
constexpr std::uint32_t kEntryPoint = 0x00F18058;
constexpr std::uint16_t kImageFileDll = 0x2000;

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

template <typename T>
T load(const unsigned char *bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

bool matches_supported_fh2(std::uintptr_t base) {
    std::array<unsigned char, 4096> header{};
    if (!memory_readable(reinterpret_cast<const void *>(base), header.size())) {
        return false;
    }
    std::memcpy(header.data(), reinterpret_cast<const void *>(base), header.size());

    if (header[0] != 'M' || header[1] != 'Z') {
        return false;
    }

    const std::uint32_t peOffset = load<std::uint32_t>(header.data(), 0x3C);
    constexpr std::size_t kMinPeBytes = 4 + 20 + 0x70;
    if (peOffset > header.size() ||
        static_cast<std::size_t>(peOffset) + kMinPeBytes > header.size()) {
        return false;
    }

    const std::size_t pe = static_cast<std::size_t>(peOffset);
    if (load<std::uint32_t>(header.data(), pe) != 0x00004550u) {
        return false;
    }

    const std::size_t file = pe + 4;
    const std::size_t optional = file + 20;
    const std::uint16_t machine = load<std::uint16_t>(header.data(), file);
    const std::uint32_t timestamp = load<std::uint32_t>(header.data(), file + 4);
    const std::uint16_t characteristics = load<std::uint16_t>(header.data(), file + 18);
    const std::uint16_t magic = load<std::uint16_t>(header.data(), optional);
    const std::uint32_t entry = load<std::uint32_t>(header.data(), optional + 16);
    const std::uint64_t preferredBase = load<std::uint64_t>(header.data(), optional + 24);
    const std::uint32_t sizeOfImage = load<std::uint32_t>(header.data(), optional + 56);

    return machine == kMachineAmd64 &&
           timestamp == kTimestamp &&
           magic == kPe32PlusMagic &&
           entry == kEntryPoint &&
           preferredBase == kPreferredBase &&
           sizeOfImage == kSizeOfImage &&
           (characteristics & kImageFileDll) == 0;
}

} // namespace

namespace fh2vr {

std::uintptr_t find_fh2_game_base() {
    if (matches_supported_fh2(kPreferredBase)) {
        return kPreferredBase;
    }

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);

    while (cursor < maximum) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void *>(cursor), &info, sizeof(info))) {
            break;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto allocationBase = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
        if (base == allocationBase &&
            info.State == MEM_COMMIT &&
            (info.Type == MEM_IMAGE || info.Type == MEM_PRIVATE) &&
            info.RegionSize >= 4096 &&
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            matches_supported_fh2(base)) {
            return base;
        }

        if (info.RegionSize == 0 ||
            info.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base) {
            break;
        }
        const std::uintptr_t next = base + info.RegionSize;
        if (next <= cursor) {
            break;
        }
        cursor = next;
    }

    return 0;
}

} // namespace fh2vr
