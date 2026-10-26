#include <stdint.h>

#include "modapi.h"

static const char *const speeds[] = { "1x", "1.5x", "2x", "2.5x", "3x", "3.5x", "4x" };
#define SPEED_COUNT (int)(sizeof speeds / sizeof *speeds)

static const char *const machines[] = { "ChamberSpec", "PressSpec" };
#define MACHINE_COUNT (int)(sizeof machines / sizeof *machines)

static const ModApi *api;
static Mod *self;
static const int *speed;

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
    float factor = api->enabled(self) && api->isHost() ? 1.0f + 0.5f * (float)*speed : 1.0f;
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
    applySpeed();
    originalStep(world, dt, method);
}

static void speedChanged(Mod *mod)
{
    (void)mod;
    api->log(self, "Tempo %s", speeds[*speed]);
}

MOD_EXPORT int ModInit(const ModApi *modApi, Mod *mod)
{
    api = modApi;
    self = mod;
    api->describe(mod, "Schnellere Maschinen", "Faster Machines",
        "Pressure chamber und Stone press arbeiten schneller. Der Stromverbrauch bleibt gleich.",
        "Pressure chamber and stone press work faster at the same power use.");
    speed = api->choice(mod, "Speed", "Tempo", "Speed", speeds, SPEED_COUNT, 0, MOD_WORLD, speedChanged);

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
    api->log(mod, "bereit, Tempo %s", speeds[*speed]);
    return 1;
}
