#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "modapi.h"

#define MIN_SUN_ELEVATION_DEGREES 35.0f
#define SHADOW_STEP_DEGREES 0.1f
#define RADIANS 0.017453292f

/** MoonWorld.SolvePower: sunFactor *= daylight; store sunFactor; store daylight > threshold; power = sunFactor * peakKw */
static const char solarPattern[] =
    "F3 41 0F 59 D5 F3 41 0F 11 57 60 44 0F 2F 2D ?? ?? ?? ?? 0F 97 C0 41 88 47 64 F3 41 0F 59 56 60";

/** sunFactor = 1.0f; store sunFactor; daylight = true; pad to the 26 replaced bytes */
static const unsigned char fullSun[26] = {
    0xB8, 0x00, 0x00, 0x80, 0x3F,
    0x66, 0x0F, 0x6E, 0xD0,
    0xF3, 0x41, 0x0F, 0x11, 0x57, 0x60,
    0x41, 0xC6, 0x47, 0x64, 0x01,
    0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00,
};

typedef struct {
    float x, y, z;
} Vec3;

static const ModApi *api;
static Mod *self;
static const int *power, *light;

static unsigned char *solarSite;
static unsigned char solarOriginal[sizeof fullSun];

static Vec3 (*originalSun)(float lunarTime, const void *method);
static const unsigned char *lunarCycle;

static struct {
    const void *getMain, *getTransform, *getPosition, *findAny;
    void *worldType;
    void *worldHandle;
    ULONGLONG nextWorldSearch;
    Vec3 stableUp;
    int hasStableUp;
    void *(*gchandleNew)(void *, uint8_t);
    void *(*gchandleGetTarget)(void *);
    void (*gchandleFree)(void *);
} unity;

static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 add(Vec3 a, Vec3 b) { return (Vec3){ a.x + b.x, a.y + b.y, a.z + b.z }; }
static Vec3 scale(Vec3 v, float s) { return (Vec3){ v.x * s, v.y * s, v.z * s }; }

static int normalize(Vec3 *v)
{
    float length = sqrtf(dot(*v, *v));
    if (length < 1e-6f)
        return 0;
    *v = scale(*v, 1.0f / length);
    return 1;
}

/** Mirror a sun below the horizon above it, then raise it to at least the minimum elevation */
static Vec3 daylightSun(Vec3 sun, Vec3 up)
{
    if (!normalize(&sun))
        return up;
    float height = dot(sun, up);
    if (height < 0) {
        sun = add(sun, scale(up, -2.0f * height));
        height = -height;
    }
    float minHeight = sinf(MIN_SUN_ELEVATION_DEGREES * RADIANS);
    if (height >= minHeight)
        return sun;
    Vec3 horizon = add(sun, scale(up, -height));
    if (!normalize(&horizon))
        return up;
    return add(scale(horizon, cosf(MIN_SUN_ELEVATION_DEGREES * RADIANS)), scale(up, minHeight));
}

static void applyPower(void)
{
    if (!solarSite)
        return;
    int wanted = api->enabled(self) && *power;
    api->write(solarSite, wanted ? fullSun : solarOriginal, sizeof fullSun);
    api->log(self, wanted ? "Strom: volle Sonne fuer alle Panels" : "Strom: wie im Spiel");
}

static int alive(void *object)
{
    return object && *(void **)((unsigned char *)object + 0x10);
}

static Vec3 positionOf(void *component)
{
    void *(*getTransform)(void *, const void *) = api->code(unity.getTransform);
    Vec3 (*getPosition)(void *, const void *) = api->code(unity.getPosition);
    void *transform = getTransform(component, unity.getTransform);
    if (!alive(transform))
        return (Vec3){ NAN, NAN, NAN };
    return getPosition(transform, unity.getPosition);
}

static void *currentWorld(void)
{
    void *world = unity.worldHandle ? unity.gchandleGetTarget(unity.worldHandle) : NULL;
    if (alive(world))
        return world;
    if (unity.worldHandle) {
        unity.gchandleFree(unity.worldHandle);
        unity.worldHandle = NULL;
    }
    ULONGLONG now = GetTickCount64();
    if (now < unity.nextWorldSearch)
        return NULL;
    unity.nextWorldSearch = now + 1000;
    void *(*findAny)(void *, int, const void *) = api->code(unity.findAny);
    world = findAny(unity.worldType, 0, unity.findAny);
    if (!alive(world))
        return NULL;
    unity.worldHandle = unity.gchandleNew(world, 0);
    return world;
}

/** Straight up from the moon's centre to the camera, only updated in small steps so shadows don't shimmer */
static int cameraUp(Vec3 *up)
{
    void *(*getMain)(const void *) = api->code(unity.getMain);
    void *camera = getMain(unity.getMain);
    void *world = currentWorld();
    if (!alive(camera) || !world)
        return 0;
    Vec3 fromCenter = add(positionOf(camera), scale(positionOf(world), -1.0f));
    if (isnan(fromCenter.x) || !normalize(&fromCenter))
        return 0;
    if (!unity.hasStableUp || dot(fromCenter, unity.stableUp) < cosf(SHADOW_STEP_DEGREES * RADIANS)) {
        unity.stableUp = fromCenter;
        unity.hasStableUp = 1;
    }
    *up = unity.stableUp;
    return 1;
}

static const unsigned char *functionStart(const void *address)
{
    DWORD64 imageBase;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry((DWORD64)address, &imageBase, NULL);
    for (int depth = 0; function && depth < 8; depth++) {
        if (function->UnwindData & 1) {
            function = (PRUNTIME_FUNCTION)(imageBase + (function->UnwindData & ~1u));
            continue;
        }
        const unsigned char *unwind = (const unsigned char *)imageBase + function->UnwindData;
        if (!((unwind[0] >> 3) & UNW_FLAG_CHAININFO))
            return (const unsigned char *)imageBase + function->BeginAddress;
        function = (PRUNTIME_FUNCTION)(unwind + 4 + ((unwind[2] + 1) & ~1) * 2);
    }
    return NULL;
}

/** Only the sky and lighting get the raised sun; power, trackers and the ending keep the real one */
static Vec3 sunHook(float lunarTime, const void *method)
{
    Vec3 sun = originalSun(lunarTime, method);
    if (!api->enabled(self) || !*light || functionStart(__builtin_return_address(0)) != lunarCycle)
        return sun;
    Vec3 up;
    return cameraUp(&up) ? daylightSun(sun, up) : sun;
}

static int prepareLight(void)
{
    const char *game = "Assembly-CSharp", *core = "UnityEngine.CoreModule";
    void *sunDirection = api->code(api->method(game, "MoonCleaners", "MoonSolarSystem", "SunDirectionAt", 1));
    lunarCycle = api->code(api->method(game, "MoonCleaners", "MoonSolarSystem", "LunarCycle", 0));
    unity.getMain = api->method(core, "UnityEngine", "Camera", "get_main", 0);
    unity.getTransform = api->method(core, "UnityEngine", "Component", "get_transform", 0);
    unity.getPosition = api->method(core, "UnityEngine", "Transform", "get_position", 0);
    unity.findAny = api->method(core, "UnityEngine", "Object", "FindAnyObjectByType", 2);
    void *(*classGetType)(void *) = api->export("il2cpp_class_get_type");
    void *(*typeGetObject)(void *) = api->export("il2cpp_type_get_object");
    unity.gchandleNew = api->export("il2cpp_gchandle_new");
    unity.gchandleGetTarget = api->export("il2cpp_gchandle_get_target");
    unity.gchandleFree = api->export("il2cpp_gchandle_free");
    void *worldClass = api->klass(game, "MoonCleaners", "MoonWorld");
    if (!sunDirection || !lunarCycle || !api->code(unity.getMain) || !api->code(unity.getTransform)
        || !api->code(unity.getPosition) || !api->code(unity.findAny) || !classGetType || !typeGetObject
        || !unity.gchandleNew || !unity.gchandleGetTarget || !unity.gchandleFree || !worldClass)
        return 0;
    unity.worldType = typeGetObject(classGetType(worldClass));
    return unity.worldType && api->hook(sunDirection, (void *)sunHook, (void **)&originalSun);
}

static void onEnabled(Mod *mod, int enabled)
{
    (void)mod;
    (void)enabled;
    applyPower();
}

static void powerChanged(Mod *mod)
{
    (void)mod;
    applyPower();
}

MOD_EXPORT int ModInit(const ModApi *modApi, Mod *mod)
{
    api = modApi;
    self = mod;
    api->describe(mod, "Immer Sonne", "Always Sun",
        "Solarpanels liefern immer vollen Strom, und es ist überall hell.",
        "Solar panels always run at full power and it is bright everywhere.");
    power = api->toggle(mod, "Power", "Strom", "Power", 1, MOD_WORLD, powerChanged);
    light = api->toggle(mod, "Light", "Licht", "Light", 1, 0, NULL);

    int matches;
    solarSite = api->find(solarPattern, &matches);
    if (solarSite)
        memcpy(solarOriginal, solarSite, sizeof solarOriginal);
    else
        api->log(mod, "Strom: Stelle %d mal statt einmal gefunden, Strom-Teil aus", matches);

    int lightReady = prepareLight();
    if (!lightReady)
        api->log(mod, "Licht: Spiel-Funktionen nicht gefunden, Licht-Teil aus");
    if (!solarSite && !lightReady) {
        api->incompatible(mod, "weder Strom noch Licht gefunden");
        return 0;
    }
    if (!solarSite)
        api->note(mod, "Strom passt nicht zu dieser Spielversion.", "Power is not compatible with this game version.");
    if (!lightReady)
        api->note(mod, "Licht passt nicht zu dieser Spielversion.", "Light is not compatible with this game version.");
    api->onEnabled(mod, onEnabled);
    api->log(mod, "bereit");
    return 1;
}
