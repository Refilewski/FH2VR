#include "CameraResearch.h"
#include "GameBaseLocator.h"

#include <windows.h>

namespace {

DWORD WINAPI camera_research_bootstrap(void *) {
    for (;;) {
        const std::uintptr_t gameBase = fh2vr::find_fh2_game_base();
        if (gameBase) {
            fh2vr::start_camera_research(gameBase);
            return 0;
        }
        Sleep(500);
    }
}

struct CameraResearchBootstrap {
    CameraResearchBootstrap() {
        HANDLE thread = CreateThread(nullptr, 0, camera_research_bootstrap,
                                     nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
};

CameraResearchBootstrap g_cameraResearchBootstrap;

} // namespace
