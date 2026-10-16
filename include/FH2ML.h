// SPDX-License-Identifier: MIT
// FH2ML SDK header; see FH2ML include/LICENSE in the upstream project.
#ifndef FH2ML_API_H
#define FH2ML_API_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FH2ML_API_VERSION 1u
typedef struct FH2ML_Patch {
    uint32_t size;
    const char *owner;
    const char *type;
    uintptr_t address;
    uint32_t length;
    const uint8_t *original;
    const uint8_t *replacement;
} FH2ML_Patch;
typedef struct FH2ML_ModInfo {
    uint32_t size;
    uint32_t loaded;
    uintptr_t module;
    char version[64];
} FH2ML_ModInfo;
typedef struct FH2ML_HostAPI {
    uint32_t size;
    uint32_t version;
    void (*Log)(const char *owner, const char *message);
    int (*RegisterMod)(const char *id, const char *version);
    int (*RegisterPatch)(const FH2ML_Patch *patch);
    int (*QueryMod)(const char *id, FH2ML_ModInfo *info);
    uintptr_t (*GetGameModule)(void);
} FH2ML_HostAPI;
typedef int (*FH2ML_InitializeFn)(const FH2ML_HostAPI *api);
#ifdef __cplusplus
}
#endif
#endif
