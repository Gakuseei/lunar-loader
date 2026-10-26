#include <stdint.h>
#include <string.h>

#include "modapi.h"

/** MoonWalker.LateUpdate: if vehicle speed > 1.5 and the camera was left alone for 1.2 s, turn it back behind the vehicle */
static const char returnPattern[] =
    "48 8B 8F 68 01 00 00 48 85 C9 0F 84 ?? ?? ?? ?? 33 D2 E8 ?? ?? ?? ?? 41 0F 2F C6 "
    "0F 86 ?? ?? ?? ?? 33 C9 E8 ?? ?? ?? ?? F3 0F 5C 87 74 01 00 00 0F 2F 05 ?? ?? ?? ?? 0F 86";
#define BRANCH_OFFSET 27

static const ModApi *api;
static Mod *self;
static unsigned char *branch;
static unsigned char original[6], steady[6];

static void apply(Mod *mod, int enabled)
{
    (void)mod;
    api->write(branch, enabled ? steady : original, sizeof original);
    api->log(self, enabled ? "Kamera bleibt stehen" : "Kamera wie im Spiel");
}

MOD_EXPORT int ModInit(const ModApi *modApi, Mod *mod)
{
    api = modApi;
    self = mod;
    api->describe(mod, "Fahrzeug-Kamera", "Steady Vehicle Camera",
        "Die Kamera bleibt beim Fahren so stehen, wie du sie einstellst.",
        "While driving, the camera stays where you put it.");

    int matches;
    unsigned char *site = api->find(returnPattern, &matches);
    if (!site) {
        api->log(mod, "Stelle %d mal statt einmal gefunden", matches);
        api->incompatible(mod, "Kamera-Code nicht gefunden");
        return 0;
    }
    branch = site + BRANCH_OFFSET;
    memcpy(original, branch, sizeof original);

    /** jbe rel32 becomes jmp to the same target: rel32 + 1 because jmp is one byte shorter, then a nop */
    int32_t relative;
    memcpy(&relative, branch + 2, sizeof relative);
    relative += 1;
    steady[0] = 0xE9;
    memcpy(steady + 1, &relative, sizeof relative);
    steady[5] = 0x90;

    api->onEnabled(mod, apply);
    api->log(mod, "bereit");
    return 1;
}
