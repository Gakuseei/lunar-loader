#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "modapi.h"

#define JOONAS_KIND 99
#define MAX_LEVEL 3
#define LEVEL_REFRESH_MS 500

#define AT(object, offset, type) (*(type *)((unsigned char *)(object) + (offset)))

enum { UP_DRIVE, UP_SIZE, UP_TANK, UPGRADE_COUNT };

typedef struct {
    float x, y, z;
} Vec3;

static const struct {
    const char *id, *titleDe, *titleEn, *summaryDe, *summaryEn;
    int glyph;
    int prices[MAX_LEVEL];
    float factor[MAX_LEVEL + 1];
} upgrades[UPGRADE_COUNT] = {
    [UP_DRIVE] = { "joonas.drive", "J.O.O.N.A.S. Antrieb", "J.O.O.N.A.S. Drive",
        "Stärkere Motoren: Jeder J.O.O.N.A.S. fährt, dreht und schrubbt 2-, 3- oder 5-mal so schnell.",
        "Stronger motors: every J.O.O.N.A.S. drives, turns and scrubs 2x, 3x or 5x as fast.",
        20, { 1250, 2500, 5000 }, { 1.0f, 2.0f, 3.0f, 5.0f } },
    [UP_SIZE] = { "joonas.size", "J.O.O.N.A.S. Größe", "J.O.O.N.A.S. Size",
        "Jeder J.O.O.N.A.S. wird 1,5-, 2- oder 2,5-mal so groß und putzt eine entsprechend breitere Bahn.",
        "Every J.O.O.N.A.S. grows 1.5x, 2x or 2.5x and cleans a path that much wider.",
        4, { 1500, 3000, 6000 }, { 1.0f, 1.5f, 2.0f, 2.5f } },
    [UP_TANK] = { "joonas.tank", "J.O.O.N.A.S. Tank", "J.O.O.N.A.S. Tank",
        "Tank für 150, 300 oder 600 kg, größere Kapseln, und Fänger nehmen alle 5, 3 oder 1,5 s eine Kapsel statt alle 9 s.",
        "Holds 150, 300 or 600 kg, launches bigger capsules, and catchers take a capsule every 5, 3 or 1.5 s instead of 9 s.",
        21, { 750, 1500, 3000 }, { 1.0f, 2.5f, 5.0f, 10.0f } },
};

static const float capsuleFactor[MAX_LEVEL + 1] = { 1.0f, 1.5f, 2.0f, 2.5f };
static const float catchSeconds[MAX_LEVEL + 1] = { 9.0f, 5.0f, 3.0f, 1.5f };

/** ReserveCatch: next free slot = max(earliest, booked) + 9 s; the addss reads 9.0 from the constant pool */
static const char catchPattern[] = "F3 0F 5F F7 8B D7 0F 28 D6 F3 0F 58 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 8B E8 01 00 00";
#define CATCH_INSTRUCTION 9
#define CATCH_LENGTH 8

static const ModApi *api;
static Mod *self;

static struct {
    void *(*fieldFromName)(void *, const char *);
    size_t (*fieldOffset)(void *);
    void *(*objectClass)(void *);
    void *(*invoke)(const void *, void *, void **, void **);
    void *(*methodFromName)(void *, const char *, int);
} il;

typedef struct {
    const char *name;
    int group;
    int offset;
    float stock;
} FloatField;

#define FIELD(name, group) { name, group, -1, 0.0f }

enum { TRAVEL_SPEED, HALF_WIDTH = 5 };

static FloatField robotFloats[] = {
    [TRAVEL_SPEED] = FIELD("travelSpeed", UP_DRIVE), FIELD("cleaningSpeed", UP_DRIVE), FIELD("turnRate", UP_DRIVE),
    FIELD("acceleration", UP_DRIVE), FIELD("halfLength", UP_SIZE), [HALF_WIDTH] = FIELD("halfWidth", UP_SIZE),
    FIELD("bodyHeight", UP_SIZE), FIELD("deckHeight", UP_SIZE), FIELD("cleaningWidth", UP_SIZE),
    FIELD("cleaningDepth", UP_SIZE), FIELD("cleaningOffset", UP_SIZE), FIELD("laneOverrun", UP_SIZE),
    FIELD("laneEdgeRun", UP_SIZE), FIELD("wheelRadius", UP_SIZE),
};
static FloatField rigFloats[] = { FIELD("motionRate", UP_DRIVE), FIELD("padSize", UP_SIZE), FIELD("padHeight", UP_SIZE) };
#define ROBOT_FLOATS (int)(sizeof robotFloats / sizeof *robotFloats)
#define RIG_FLOATS (int)(sizeof rigFloats / sizeof *rigFloats)

static struct {
    int world, launcher, storageId, capacity, launchKilograms, vehicleStorage;
} off;

/** Values of the game's own J.O.O.N.A.S., read from the first one before the mod changes any */
static struct {
    int captured;
    double capacity, launchKilograms;
    Vec3 scale;
    int farCaptured, rateOffset, binOffset;
    float rate;
    double bin;
} stock;

static struct {
    void *world;
    ULONGLONG next;
    int level[UPGRADE_COUNT];
    float factor[UPGRADE_COUNT];
    int tankLevel;
} now = { .factor = { 1.0f, 1.0f, 1.0f } };

static struct {
    const void *getTransform, *getScale, *setScale;
} game;

static struct {
    unsigned char *site;
    const float *targets[MAX_LEVEL + 1];
    int shown;
} catcher = { .shown = -1 };

static void (*originalUpdate)(void *, const void *);
static float (*originalCovered)(void *, const void *);
static void *(*originalFarSpec)(void *, int, const void *);

static int offsetOf(void *klass, const char *name)
{
    void *field = klass ? il.fieldFromName(klass, name) : NULL;
    return field ? (int)il.fieldOffset(field) : -1;
}

static int call(const void *method, void *object, void **params)
{
    void *exception = NULL;
    if (!method)
        return 0;
    il.invoke(method, object, params, &exception);
    return exception == NULL;
}

static const float *poolConstant(float value)
{
    unsigned char *base = (unsigned char *)GetModuleHandleW(L"GameAssembly.dll");
    if (!base)
        return NULL;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++, section++) {
        if (memcmp(section->Name, ".rdata", 6))
            continue;
        const float *start = (const float *)(base + section->VirtualAddress);
        for (DWORD i = 0; i < section->Misc.VirtualSize / sizeof(float); i++)
            if (start[i] == value)
                return start + i;
    }
    return NULL;
}

static void prepareCatcher(void)
{
    int matches;
    unsigned char *site = api->find(catchPattern, &matches);
    if (!site) {
        api->log(self, "Fänger-Stelle %d mal statt einmal gefunden, Fänger bleiben bei 9 s", matches);
        return;
    }
    site += CATCH_INSTRUCTION;
    int32_t relative;
    memcpy(&relative, site + 4, sizeof relative);
    const float *current = (const float *)(site + CATCH_LENGTH + relative);
    if (*current != catchSeconds[0]) {
        api->log(self, "Fänger warten nicht 9 s wie erwartet, Fänger bleiben unverändert");
        return;
    }
    catcher.targets[0] = current;
    for (int level = 1; level <= MAX_LEVEL; level++) {
        catcher.targets[level] = poolConstant(catchSeconds[level]);
        if (!catcher.targets[level]) {
            api->log(self, "Konstante %.1f fehlt, Fänger bleiben bei 9 s", catchSeconds[level]);
            return;
        }
    }
    catcher.site = site;
}

static void applyCatcher(int level)
{
    if (!catcher.site || level == catcher.shown)
        return;
    intptr_t distance = (const unsigned char *)catcher.targets[level] - (catcher.site + CATCH_LENGTH);
    if (distance != (int32_t)distance)
        return;
    int32_t relative = (int32_t)distance;
    if (api->write(catcher.site + 4, &relative, sizeof relative)) {
        catcher.shown = level;
        api->log(self, "Fänger nehmen alle %.1f s eine Kapsel", catchSeconds[level]);
    }
}

static void refresh(void *world)
{
    ULONGLONG tick = GetTickCount64();
    if (world == now.world && tick < now.next)
        return;
    now.world = world;
    now.next = tick + LEVEL_REFRESH_MS;
    int on = api->enabled(self);
    for (int i = 0; i < UPGRADE_COUNT; i++) {
        int level = api->upgradeLevel(world, upgrades[i].id);
        if (level != now.level[i])
            api->log(self, "%s jetzt Stufe %d", upgrades[i].id, level);
        now.level[i] = level;
        now.factor[i] = on ? upgrades[i].factor[level] : 1.0f;
    }
    now.tankLevel = on ? now.level[UP_TANK] : 0;
    applyCatcher(now.tankLevel);
}

static void captureStock(void *robot, void *rig)
{
    for (int i = 0; i < ROBOT_FLOATS; i++)
        robotFloats[i].stock = robotFloats[i].offset >= 0 ? AT(robot, robotFloats[i].offset, float) : 0;
    for (int i = 0; i < RIG_FLOATS; i++)
        rigFloats[i].stock = rigFloats[i].offset >= 0 ? AT(rig, rigFloats[i].offset, float) : 0;
    stock.capacity = AT(robot, off.capacity, double);
    stock.launchKilograms = AT(rig, off.launchKilograms, double);
    void *(*getTransform)(void *, const void *) = api->code(game.getTransform);
    Vec3 (*getScale)(void *, const void *) = api->code(game.getScale);
    stock.scale = getScale(getTransform(robot, game.getTransform), game.getScale);
    stock.captured = 1;
    api->log(self, "J.O.O.N.A.S. im Spiel: %.0f kg Tank, %.0f kg Kapsel, Tempo %.1f m/s",
        stock.capacity, stock.launchKilograms, robotFloats[TRAVEL_SPEED].stock);
}

static void writeFloats(void *object, FloatField *fields, int count)
{
    for (int i = 0; i < count; i++)
        if (fields[i].offset >= 0)
            AT(object, fields[i].offset, float) = fields[i].stock * now.factor[fields[i].group];
}

static void growStorage(void *robot, void *world, double capacity)
{
    void *storages = off.vehicleStorage >= 0 ? AT(world, off.vehicleStorage, void *) : NULL;
    void *id = AT(robot, off.storageId, void *);
    if (!storages || !id)
        return;
    void *storage = NULL;
    void *lookup[2] = { id, &storage };
    if (!call(il.methodFromName(il.objectClass(storages), "TryGetValue", 2), storages, lookup) || !storage)
        return;
    int limit = offsetOf(il.objectClass(storage), "capacity");
    if (limit >= 0)
        AT(storage, limit, double) = capacity;
}

/** Writes stock value times the bought factor, so it is the same whether the robot is fresh or already upgraded */
static void upgradeRobot(void *robot)
{
    void *rig = AT(robot, off.launcher, void *);
    void *world = AT(robot, off.world, void *);
    if (!rig || !world)
        return;
    refresh(world);
    if (!stock.captured)
        captureStock(robot, rig);

    double capacity = stock.capacity * now.factor[UP_TANK];
    float speed = robotFloats[TRAVEL_SPEED].stock * now.factor[UP_DRIVE];
    float width = robotFloats[HALF_WIDTH].stock * now.factor[UP_SIZE];
    int sizeChanged = AT(robot, robotFloats[HALF_WIDTH].offset, float) != width;
    if (AT(robot, robotFloats[TRAVEL_SPEED].offset, float) == speed && !sizeChanged
        && AT(robot, off.capacity, double) == capacity)
        return;

    writeFloats(robot, robotFloats, ROBOT_FLOATS);
    writeFloats(rig, rigFloats, RIG_FLOATS);
    if (AT(robot, off.capacity, double) != capacity) {
        AT(robot, off.capacity, double) = capacity;
        growStorage(robot, world, capacity);
    }
    AT(rig, off.launchKilograms, double) = stock.launchKilograms * capsuleFactor[now.tankLevel];
    if (sizeChanged) {
        void *(*getTransform)(void *, const void *) = api->code(game.getTransform);
        void (*setScale)(void *, Vec3, const void *) = api->code(game.setScale);
        float grow = now.factor[UP_SIZE];
        Vec3 scale = { stock.scale.x * grow, stock.scale.y * grow, stock.scale.z * grow };
        setScale(getTransform(robot, game.getTransform), scale, game.setScale);
    }
}

static void updateHook(void *robot, const void *method)
{
    upgradeRobot(robot);
    originalUpdate(robot, method);
}

static float coveredHook(void *rig, const void *method)
{
    return originalCovered(rig, method) * now.factor[UP_SIZE];
}

/** Far away from players the game only estimates cleaning from this spec; it is cached per robot kind */
static void *farSpecHook(void *world, int kind, const void *method)
{
    void *spec = originalFarSpec(world, kind, method);
    if (!spec || kind != JOONAS_KIND)
        return spec;
    refresh(world);
    if (!stock.farCaptured) {
        void *specClass = il.objectClass(spec);
        stock.rateOffset = offsetOf(specClass, "rate");
        stock.binOffset = offsetOf(specClass, "bin");
        if (stock.rateOffset < 0 || stock.binOffset < 0)
            return spec;
        stock.rate = AT(spec, stock.rateOffset, float);
        stock.bin = AT(spec, stock.binOffset, double);
        stock.farCaptured = 1;
        api->log(self, "J.O.O.N.A.S. aus der Ferne: %.2f m²/s, %.0f kg", stock.rate, stock.bin);
    }
    AT(spec, stock.rateOffset, float) = stock.rate * now.factor[UP_DRIVE] * now.factor[UP_SIZE];
    AT(spec, stock.binOffset, double) = stock.bin * now.factor[UP_TANK];
    return spec;
}

static void onEnabled(Mod *mod, int enabled)
{
    (void)mod;
    (void)enabled;
    now.next = 0;
    if (!enabled)
        applyCatcher(0);
}

static int resolveIl2cpp(void)
{
    il.fieldFromName = api->export("il2cpp_class_get_field_from_name");
    il.fieldOffset = api->export("il2cpp_field_get_offset");
    il.objectClass = api->export("il2cpp_object_get_class");
    il.invoke = api->export("il2cpp_runtime_invoke");
    il.methodFromName = api->export("il2cpp_class_get_method_from_name");
    void **all = (void **)&il;
    for (size_t i = 0; i < sizeof il / sizeof(void *); i++)
        if (!all[i])
            return 0;
    return 1;
}

static int resolveOffsets(void)
{
    const char *assembly = "Assembly-CSharp", *space = "MoonCleaners";
    void *robot = api->klass(assembly, space, "MoonRobotVacuum");
    void *rig = api->klass(assembly, space, "MoonJoonasRig");
    void *world = api->klass(assembly, space, "MoonWorld");
    for (int i = 0; i < ROBOT_FLOATS; i++)
        robotFloats[i].offset = offsetOf(robot, robotFloats[i].name);
    for (int i = 0; i < RIG_FLOATS; i++)
        rigFloats[i].offset = offsetOf(rig, rigFloats[i].name);
    off.world = offsetOf(robot, "world");
    off.launcher = offsetOf(robot, "launcher");
    off.storageId = offsetOf(robot, "storageId");
    off.capacity = offsetOf(robot, "capacityKilograms");
    off.launchKilograms = offsetOf(rig, "launchKilograms");
    off.vehicleStorage = offsetOf(world, "vehicleStorage");
    return off.world >= 0 && off.launcher >= 0 && off.storageId >= 0 && off.capacity >= 0 && off.launchKilograms >= 0
        && robotFloats[TRAVEL_SPEED].offset >= 0 && robotFloats[HALF_WIDTH].offset >= 0;
}

static int hookMethod(const char *klass, const char *name, int argc, void *detour, void **original)
{
    void *code = api->code(api->method("Assembly-CSharp", "MoonCleaners", klass, name, argc));
    return code && api->hook(code, detour, original);
}

MOD_EXPORT int ModInit(const ModApi *modApi, Mod *mod)
{
    api = modApi;
    self = mod;
    api->describe(mod, "J.O.O.N.A.S. Upgrades", "J.O.O.N.A.S. Upgrades",
        "Neuer Tab SERVICES in der Upgrade-Matrix: Antrieb, Größe und Tank für alle J.O.O.N.A.S.",
        "New SERVICES tab in the upgrade matrix: drive, size and tank for every J.O.O.N.A.S.");

    game.getTransform = api->method("UnityEngine.CoreModule", "UnityEngine", "Component", "get_transform", 0);
    game.getScale = api->method("UnityEngine.CoreModule", "UnityEngine", "Transform", "get_localScale", 0);
    game.setScale = api->method("UnityEngine.CoreModule", "UnityEngine", "Transform", "set_localScale", 1);
    if (api->version < 2 || !resolveIl2cpp() || !resolveOffsets() || !api->code(game.getTransform)
        || !api->code(game.getScale) || !api->code(game.setScale)) {
        api->incompatible(mod, "J.O.O.N.A.S. nicht gefunden oder Loader zu alt");
        return 0;
    }
    for (int i = 0; i < UPGRADE_COUNT; i++)
        api->upgrade(mod, upgrades[i].id, upgrades[i].titleDe, upgrades[i].titleEn, upgrades[i].summaryDe,
            upgrades[i].summaryEn, upgrades[i].glyph, MAX_LEVEL, upgrades[i].prices);
    if (!hookMethod("MoonRobotVacuum", "Update", 0, (void *)updateHook, (void **)&originalUpdate)
        || !hookMethod("MoonJoonasRig", "get_CoveredHalfWidth", 0, (void *)coveredHook, (void **)&originalCovered)
        || !hookMethod("MoonWorld", "FarSpec", 1, (void *)farSpecHook, (void **)&originalFarSpec)) {
        api->incompatible(mod, "J.O.O.N.A.S. konnte nicht erweitert werden");
        return 0;
    }
    prepareCatcher();
    if (!catcher.site)
        api->note(mod, "Die Wartezeit am Fänger passt nicht zu dieser Spielversion.",
            "Catcher wait time is not compatible with this game version.");
    api->note(mod, "Kaufen: Basis > Upgrade-Matrix > Tab SERVICES. Der Host braucht die Mod.",
        "Buy them at the base: upgrade matrix > SERVICES tab. The host needs the mod.");
    api->onEnabled(mod, onEnabled);
    api->log(mod, "bereit");
    return 1;
}
