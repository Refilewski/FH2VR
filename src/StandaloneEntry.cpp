#include "FH2ML.h"
#include "GameBaseLocator.h"

#include <windows.h>

#include <cstdio>
#include <string>

extern "C" __declspec(dllexport) int FH2ML_Initialize(const FH2ML_HostAPI *api);

namespace {

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

} // namespace

extern "C" __declspec(dllexport) int FH2VR_StandaloneInitialize() {
    return FH2ML_Initialize(&g_standaloneApi);
}
