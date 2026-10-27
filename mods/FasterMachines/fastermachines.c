#include <windows.h>
#include <stdint.h>

#include "modapi.h"

#define UPGRADE_ID "machines.speed"
#define MAX_LEVEL 3
#define GLYPH_TURBO 24

static const float factors[MAX_LEVEL + 1] = { 1.0f, 1.5f, 2.0f, 3.0f };
static const int prices[MAX_LEVEL] = { 200, 600, 1500 };

static const char *const machines[] = { "ChamberSpec", "PressSpec" };
#define MACHINE_COUNT (int)(sizeof machines / sizeof *machines)

static const ModApi *api;
static Mod *self;
static int level;
static ULONGLONG nextLevelCheck;

static void (*originalStep)(void *world, float dt, const void *method);

static struct {
    void *field;
    void *spec;
    int cycleOffset;
    float baseSeconds;
} machine[MACHINE_COUNT];

static void *(*classGetFieldFromName)(void *, const char *);
static void (*fieldStaticGetValue)(void *, void *);
static size_t (*fieldGetOffset)(void *);
static void *(*objectGetClass)(void *);

/** The machine specs are shared objects created by the game; the first value we see is the game's own */
static void applySpeed(void)
{
    float factor = api->enabled(self) && api->isHost() ? factors[level] : 1.0f;
    for (int i = 0; i < MACHINE_COUNT; i++) {
        void *spec = NULL;
        fieldStaticGetValue(machine[i].field, &spec);
        if (!spec)
            continue;
        if (spec != machine[i].spec) {
            void *cycle = classGetFieldFromName(objectGetClass(spec), "cycleSeconds");
            if (!cycle)
                continue;
            machine[i].spec = spec;
            machine[i].cycleOffset = (int)fieldGetOffset(cycle);
            machine[i].baseSeconds = *(float *)((unsigned char *)spec + machine[i].cycleOffset);
            api->log(self, "%s: %.1f s pro Ladung im Spiel", machines[i], machine[i].baseSeconds);
        }
        float *seconds = (float *)((unsigned char *)spec + machine[i].cycleOffset);
        float wanted = machine[i].baseSeconds / factor;
        if (*seconds != wanted)
            *seconds = wanted;
    }
}

static void stepHook(void *world, float dt, const void *method)
{
    ULONGLONG now = GetTickCount64();
    if (now >= nextLevelCheck) {
        nextLevelCheck = now + 500;
        int bought = api->upgradeLevel(world, UPGRADE_ID);
        if (bought != level)
            api->log(self, "Stufe %d, Tempo %.1fx", bought, factors[bought]);
        level = bought;
    }
    applySpeed();
    originalStep(world, dt, method);
}

MOD_EXPORT int ModInit(const ModApi *modApi, Mod *mod)
{
    api = modApi;
    self = mod;
    api->describe(mod, "Schnellere Maschinen", "Faster Machines",
        "Pressure chamber und Stone press arbeiten schneller. Kaufbar in der Upgrade-Matrix im Tab SERVICES.",
        "Pressure chamber and stone press work faster. Buy it in the upgrade matrix, SERVICES tab.");
    if (api->version < 2) {
        api->incompatible(mod, "Loader zu alt");
        return 0;
    }

    classGetFieldFromName = api->export("il2cpp_class_get_field_from_name");
    fieldStaticGetValue = api->export("il2cpp_field_static_get_value");
    fieldGetOffset = api->export("il2cpp_field_get_offset");
    objectGetClass = api->export("il2cpp_object_get_class");
    void *world = api->klass("Assembly-CSharp", "MoonCleaners", "MoonWorld");
    void *step = api->code(api->method("Assembly-CSharp", "MoonCleaners", "MoonWorld", "StepProduction", 1));
    if (!classGetFieldFromName || !fieldStaticGetValue || !fieldGetOffset || !objectGetClass || !world || !step) {
        api->incompatible(mod, "Produktions-Code nicht gefunden");
        return 0;
    }
    for (int i = 0; i < MACHINE_COUNT; i++) {
        machine[i].field = classGetFieldFromName(world, machines[i]);
        if (!machine[i].field) {
            api->incompatible(mod, "Maschinen-Werte nicht gefunden");
            return 0;
        }
    }
    if (!api->hook(step, (void *)stepHook, (void **)&originalStep)) {
        api->incompatible(mod, "Produktion konnte nicht erweitert werden");
        return 0;
    }
    api->upgrade(mod, UPGRADE_ID, "Maschinen-Tempo", "Machine Speed",
        "Pressure chamber und Stone press arbeiten 1,5-, 2- oder 3-mal so schnell. Der Stromverbrauch bleibt gleich.",
        "Pressure chamber and stone press work 1.5x, 2x or 3x as fast at the same power use.",
        GLYPH_TURBO, MAX_LEVEL, prices);
    api->log(mod, "bereit");
    return 1;
}
