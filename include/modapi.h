#pragma once

#include <stddef.h>

#define MODAPI_VERSION 2

typedef struct Mod Mod;

/** Setting that affects the whole world: only the host's value counts, crewmates see it greyed out */
#define MOD_WORLD 1

typedef struct ModApi {
    int version;

    void (*log)(Mod *mod, const char *format, ...);
    void (*describe)(Mod *mod, const char *nameDe, const char *nameEn, const char *infoDe, const char *infoEn);
    void (*incompatible)(Mod *mod, const char *reason);
    void (*onEnabled)(Mod *mod, void (*callback)(Mod *mod, int enabled));
    int (*enabled)(Mod *mod);

    const int *(*toggle)(Mod *mod, const char *key, const char *labelDe, const char *labelEn,
        int fallback, int flags, void (*changed)(Mod *mod));
    const int *(*choice)(Mod *mod, const char *key, const char *labelDe, const char *labelEn,
        const char *const *options, int count, int fallback, int flags, void (*changed)(Mod *mod));
    void (*note)(Mod *mod, const char *textDe, const char *textEn);

    int (*isHost)(void);

    void *(*klass)(const char *assembly, const char *namespaze, const char *name);
    void *(*method)(const char *assembly, const char *namespaze, const char *klass, const char *name, int argc);
    void *(*code)(const void *method);
    void *(*export)(const char *name);

    unsigned char *(*find)(const char *pattern, int *matches);
    int (*write)(void *target, const void *bytes, size_t length);
    int (*hook)(void *target, void *detour, void **original);
    int (*hookEnabled)(void *target, int enabled);

    /** Buyable upgrade in the shared SERVICES tab of the upgrade matrix; prices holds maxLevel entries, call from ModInit */
    int (*upgrade)(Mod *mod, const char *id, const char *titleDe, const char *titleEn, const char *summaryDe,
        const char *summaryEn, int glyph, int maxLevel, const int *prices);
    int (*upgradeLevel)(void *world, const char *id);
} ModApi;

#define MOD_EXPORT __declspec(dllexport)

/** Every mod exports this; it runs on the game's main thread right after the scripting runtime started */
typedef int (*ModInitFunction)(const ModApi *api, Mod *mod);
