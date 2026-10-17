#include "FH2ML.h"
#include "GameBaseLocator.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

extern "C" __declspec(dllexport) int FH2ML_Initialize(const FH2ML_HostAPI *api);

namespace {

std::atomic<bool> g_workerStarted{false};
std::atomic<bool> g_initialized{false};

void standalone_log(const char *owner, const char *message) {
    if (!message) {
        return;
    }

    char buffer[2048]{};
    std::snprintf(buffer, sizeof(buffer), "[%s] %s\n", owner ? owner : "fh2vr", message);
    OutputDebugStringA(buffer);
}

int standalone_register_mod(const char *, const char *) {
    return 1;
}

int standalone_register_patch(const FH2ML_Patch *patch) {
    return patch && patch->size >= sizeof(FH2ML_Patch) ? 1 : 0;
}

int standalone_query_mod(const char *, FH2ML_ModInfo *) {
    return 0;
}

uintptr_t standalone_get_game_module() {
    return fh2vr::find_fh2_game_base();
}

FH2ML_HostAPI g_standaloneApi{
    sizeof(FH2ML_HostAPI),
    FH2ML_API_VERSION,
    &standalone_log,
    &standalone_register_mod,
    &standalone_register_patch,
    &standalone_query_mod,
    &standalone_get_game_module,
};

DWORD WINAPI standalone_worker(void *) {
    standalone_log("fh2vr", "Standalone runtime loaded; waiting for supported FH2 1.0.0.64 image");

    while (!g_initialized.load(std::memory_order_acquire)) {
        if (!fh2vr::find_fh2_game_base()) {
            Sleep(500);
            continue;
        }

        if (FH2ML_Initialize(&g_standaloneApi)) {
            g_initialized.store(true, std::memory_order_release);
            standalone_log("fh2vr", "Standalone runtime initialized successfully");
            break;
        }

        standalone_log("fh2vr", "Standalone runtime init not ready; retrying");
        Sleep(2000);
    }

    return 0;
}

} // namespace

extern "C" __declspec(dllexport) int FH2VR_StandaloneInitialize() {
    if (g_initialized.load(std::memory_order_acquire)) {
        return 1;
    }

    bool expected = false;
    if (!g_workerStarted.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel)) {
        return 1;
    }

    HANDLE thread = CreateThread(nullptr, 0, standalone_worker, nullptr, 0, nullptr);
    if (!thread) {
        g_workerStarted.store(false, std::memory_order_release);
        standalone_log("fh2vr", "Failed to create standalone initialization worker");
        return 0;
    }

    CloseHandle(thread);
    return 1;
}
