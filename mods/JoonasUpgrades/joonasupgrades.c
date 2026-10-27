#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "modapi.h"

#define JOONAS_KIND 99
#define BRANCH_SERVICES 6
#define STOCK_BRANCHES 6
#define MAX_LEVEL 3
#define LEVEL_REFRESH_MS 500

#define AT(object, offset, type) (*(type *)((unsigned char *)(object) + (offset)))

enum { UP_DRIVE, UP_SIZE, UP_TANK, UPGRADE_COUNT };

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    float r, g, b, a;
} Color;

static const struct {
    const char *id, *titleDe, *titleEn, *summaryDe, *summaryEn;
    int glyph;
    int prices[MAX_LEVEL];
    float factor[MAX_LEVEL + 1];
} upgrades[UPGRADE_COUNT] = {
    [UP_DRIVE] = { "joonas.drive", "J.O.O.N.A.S. Antrieb", "J.O.O.N.A.S. Drive",
        "Stärkere Motoren: Jeder J.O.O.N.A.S. fährt, dreht und schrubbt 2-, 3- oder 5-mal so schnell.",
        "Stronger motors: every J.O.O.N.A.S. drives, turns and scrubs 2x, 3x or 5x as fast.",
        20, { 2500, 5000, 10000 }, { 1.0f, 2.0f, 3.0f, 5.0f } },
    [UP_SIZE] = { "joonas.size", "J.O.O.N.A.S. Größe", "J.O.O.N.A.S. Size",
        "Jeder J.O.O.N.A.S. wird 1,5-, 2- oder 2,5-mal so groß und putzt eine entsprechend breitere Bahn.",
        "Every J.O.O.N.A.S. grows 1.5x, 2x or 2.5x and cleans a path that much wider.",
        4, { 3000, 6000, 12000 }, { 1.0f, 1.5f, 2.0f, 2.5f } },
    [UP_TANK] = { "joonas.tank", "J.O.O.N.A.S. Tank", "J.O.O.N.A.S. Tank",
        "Tank für 150, 300 oder 600 kg, größere Kapseln, und Fänger nehmen alle 5, 3 oder 1,5 s eine Kapsel statt alle 9 s.",
        "Holds 150, 300 or 600 kg, launches bigger capsules, and catchers take a capsule every 5, 3 or 1.5 s instead of 9 s.",
        21, { 1500, 3000, 6000 }, { 1.0f, 2.5f, 5.0f, 10.0f } },
};

static const float capsuleFactor[MAX_LEVEL + 1] = { 1.0f, 1.5f, 2.0f, 2.5f };
static const float catchSeconds[MAX_LEVEL + 1] = { 9.0f, 5.0f, 3.0f, 1.5f };
static const Color servicesColor = { 1.0f, 0.47f, 0.56f, 1.0f };

/** ReserveCatch: next free slot = max(earliest, booked) + 9 s; the addss reads 9.0 from the constant pool */
static const char catchPattern[] = "F3 0F 5F F7 8B D7 0F 28 D6 F3 0F 58 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 8B E8 01 00 00";
#define CATCH_INSTRUCTION 9
#define CATCH_LENGTH 8

static const ModApi *api;
static Mod *self;

static struct {
    void *(*fieldFromName)(void *, const char *);
    size_t (*fieldOffset)(void *);
    void (*staticGet)(void *, void *);
    void (*staticSet)(void *, void *);
    void *(*objectClass)(void *);
    void *(*elementClass)(void *);
    void *(*arrayNew)(void *, uintptr_t);
    void *(*stringNew)(const char *);
    void *(*objectNew)(void *);
    void (*barrier)(void *, void **, void *);
    void (*classInit)(void *);
    void *(*invoke)(const void *, void *, void **, void **);
    void *(*methodFromName)(void *, const char *, int);
    void *(*handleNew)(void *, uint8_t);
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
    int world, launcher, storageId, capacity, launchKilograms;
    int vehicleStorage, treeLevels, progressStore;
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
    const void *treeLevel, *getTransform, *getScale, *setScale, *locCode;
    void *treeClass;
    void *local, *ids[UPGRADE_COUNT];
} game;

static struct {
    unsigned char *site;
    const float *targets[MAX_LEVEL + 1];
    int shown;
} catcher = { .shown = -1 };

static volatile LONG injecting;
static int injected;
static DWORD mainThread;

static struct {
    wchar_t path[MAX_PATH];
    char world[96];
    int level[UPGRADE_COUNT];
    int reported[UPGRADE_COUNT];
} backup;

static void (*originalUpdate)(void *, const void *);
static float (*originalCovered)(void *, const void *);
static void *(*originalFarSpec)(void *, int, const void *);
static void *(*originalFind)(void *, const void *);
static void (*originalTreeScreen)(void *, const void *);
static uint8_t (*originalPurchase)(void *, void *, void *, int32_t, void **, const void *);

static int offsetOf(void *klass, const char *name)
{
    void *field = klass ? il.fieldFromName(klass, name) : NULL;
    return field ? (int)il.fieldOffset(field) : -1;
}

static uintptr_t arrayLength(void *array) { return AT(array, 0x18, uintptr_t); }
static unsigned char *arrayItems(void *array) { return (unsigned char *)array + 0x20; }

static void setReference(void *object, int offset, void *value)
{
    il.barrier(object, (void **)((unsigned char *)object + offset), value);
}

static void setItem(void *array, uintptr_t index, void *value)
{
    il.barrier(array, (void **)arrayItems(array) + index, value);
}

static int stringIs(void *string, const char *text)
{
    if (!string)
        return 0;
    int32_t length = AT(string, 0x10, int32_t);
    const uint16_t *chars = (const uint16_t *)((unsigned char *)string + 0x14);
    for (int32_t i = 0; i < length; i++)
        if (!text[i] || chars[i] != (unsigned char)text[i])
            return 0;
    return text[length] == 0;
}

static int asciiOf(void *string, char *buffer, int size)
{
    buffer[0] = 0;
    if (!string)
        return 0;
    int32_t length = AT(string, 0x10, int32_t);
    const uint16_t *chars = (const uint16_t *)((unsigned char *)string + 0x14);
    int written = 0;
    for (int32_t i = 0; i < length && written + 1 < size; i++)
        if (chars[i] > 32 && chars[i] < 127)
            buffer[written++] = (char)chars[i];
    buffer[written] = 0;
    return written;
}

static void *keptString(const char *text)
{
    void *string = il.stringNew(text);
    il.handleNew(string, 0);
    return string;
}

static int call(const void *method, void *object, void **params)
{
    void *exception = NULL;
    if (!method)
        return 0;
    il.invoke(method, object, params, &exception);
    return exception == NULL;
}

static int german(void)
{
    void *(*code)(const void *) = api->code(game.locCode);
    char language[16];
    return code && GetCurrentThreadId() == mainThread && asciiOf(code(game.locCode), language, sizeof language) >= 2
        && (language[0] | 32) == 'd' && (language[1] | 32) == 'e';
}

static void *staticValue(const char *name)
{
    void *field = il.fieldFromName(game.treeClass, name), *value = NULL;
    if (field)
        il.staticGet(field, &value);
    return value;
}

/** Unlike the getter, il2cpp's static setter takes a reference-type value itself, not a pointer to it */
static int setStatic(const char *name, void *value)
{
    void *field = il.fieldFromName(game.treeClass, name);
    if (field)
        il.staticSet(field, value);
    return field != NULL;
}

static void *sameKindArray(void *array, uintptr_t length)
{
    return il.arrayNew(il.elementClass(il.objectClass(array)), length);
}

static void *longerReferences(void *array, void *extra)
{
    uintptr_t length = arrayLength(array);
    void *longer = sameKindArray(array, length + 1);
    for (uintptr_t i = 0; i < length; i++)
        setItem(longer, i, ((void **)arrayItems(array))[i]);
    setItem(longer, length, extra);
    return longer;
}

static void *findNode(void *nodes, int idOffset, const char *id)
{
    for (uintptr_t i = 0; i < arrayLength(nodes); i++) {
        void *node = ((void **)arrayItems(nodes))[i];
        if (node && stringIs(AT(node, idOffset, void *), id))
            return node;
    }
    return NULL;
}

static void *newNode(void *template, int upgrade, int de)
{
    void *nodeClass = il.objectClass(template);
    void *node = il.objectNew(nodeClass);
    void *prices = sameKindArray(AT(template, offsetOf(nodeClass, "prices"), void *), MAX_LEVEL);
    memcpy(arrayItems(prices), upgrades[upgrade].prices, sizeof upgrades[upgrade].prices);
    setReference(node, offsetOf(nodeClass, "id"), il.stringNew(upgrades[upgrade].id));
    setReference(node, offsetOf(nodeClass, "title"), il.stringNew(de ? upgrades[upgrade].titleDe : upgrades[upgrade].titleEn));
    setReference(node, offsetOf(nodeClass, "summary"), il.stringNew(de ? upgrades[upgrade].summaryDe : upgrades[upgrade].summaryEn));
    setReference(node, offsetOf(nodeClass, "prices"), prices);
    setReference(node, offsetOf(nodeClass, "requires"), AT(template, offsetOf(nodeClass, "requires"), void *));
    AT(node, offsetOf(nodeClass, "branch"), int32_t) = BRANCH_SERVICES;
    AT(node, offsetOf(nodeClass, "glyph"), int32_t) = upgrades[upgrade].glyph;
    AT(node, offsetOf(nodeClass, "maxLevel"), int32_t) = MAX_LEVEL;
    return node;
}

/** Adds the SERVICES tab and its nodes before the game first looks anything up in the upgrade tree */
static void inject(void)
{
    if (injected || InterlockedCompareExchange(&injecting, 1, 0))
        return;
    injected = -1;
    il.classInit(game.treeClass);
    void *names = staticValue("BranchNames"), *colors = staticValue("BranchColors");
    void *taglines = staticValue("BranchTaglines"), *nodes = staticValue("Nodes"), *byId = staticValue("byId");
    if (!names || !colors || !taglines || !nodes || !byId || !arrayLength(nodes)) {
        injected = 0;
        InterlockedExchange(&injecting, 0);
        return;
    }
    if (arrayLength(names) != STOCK_BRANCHES || arrayLength(colors) != STOCK_BRANCHES
        || arrayLength(taglines) != STOCK_BRANCHES) {
        api->log(self, "Upgrade-Matrix sieht anders aus als erwartet, Tab SERVICES fehlt");
        return;
    }
    int idOffset = offsetOf(il.objectClass(((void **)arrayItems(nodes))[0]), "id");
    void *template = findNode(nodes, idOffset, "clean.width");
    if (!template || findNode(nodes, idOffset, upgrades[UP_DRIVE].id)) {
        api->log(self, "Vorlage clean.width fehlt in der Upgrade-Matrix, Tab SERVICES fehlt");
        return;
    }
    const void *add = il.methodFromName(il.objectClass(byId), "set_Item", 2);
    int de = german();

    void *created[UPGRADE_COUNT];
    for (int i = 0; i < UPGRADE_COUNT; i++)
        created[i] = newNode(template, i, de);
    void *allNodes = nodes;
    for (int i = 0; i < UPGRADE_COUNT; i++)
        allNodes = longerReferences(allNodes, created[i]);
    void *allNames = longerReferences(names, il.stringNew("SERVICES"));
    void *allTaglines = longerReferences(taglines,
        il.stringNew(de ? "Upgrades für eure J.O.O.N.A.S.-Flotte." : "Upgrades for your J.O.O.N.A.S. fleet."));
    void *allColors = sameKindArray(colors, STOCK_BRANCHES + 1);
    memcpy(arrayItems(allColors), arrayItems(colors), STOCK_BRANCHES * sizeof(Color));
    memcpy(arrayItems(allColors) + STOCK_BRANCHES * sizeof(Color), &servicesColor, sizeof servicesColor);

    for (int i = 0; i < UPGRADE_COUNT; i++) {
        void *params[2] = { AT(created[i], idOffset, void *), created[i] };
        if (!call(add, byId, params)) {
            api->log(self, "Eintrag %s konnte nicht angelegt werden", upgrades[i].id);
            return;
        }
    }
    setStatic("BranchColors", allColors);
    setStatic("BranchTaglines", allTaglines);
    setStatic("Nodes", allNodes);
    setStatic("BranchNames", allNames);
    injected = 1;
    api->log(self, "Tab SERVICES mit %d Upgrades in der Upgrade-Matrix", UPGRADE_COUNT);
}

static void *findHook(void *id, const void *method)
{
    inject();
    return originalFind(id, method);
}

static void treeScreenHook(void *screen, const void *method)
{
    inject();
    originalTreeScreen(screen, method);
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

static int worldId(void *world, char *buffer, int size)
{
    void *store = off.progressStore >= 0 ? AT(world, off.progressStore, void *) : NULL;
    if (!store)
        return 0;
    int identity = offsetOf(il.objectClass(store), "identity");
    return identity >= 0 && asciiOf(AT(store, identity, void *), buffer, size) >= 8;
}

static int treeLevel(void *world, int upgrade)
{
    int (*level)(void *, void *, void *, const void *) = api->code(game.treeLevel);
    int found = injected == 1 ? level(world, game.local, game.ids[upgrade], game.treeLevel) : 0;
    return found < 0 ? 0 : found > MAX_LEVEL ? MAX_LEVEL : found;
}

static void backupKey(int upgrade, wchar_t *key, int size)
{
    MultiByteToWideChar(CP_UTF8, 0, upgrades[upgrade].id, -1, key, size);
}

/** Bought levels also go to Mods/JoonasUpgrades/worlds.ini, so a save made without the mod loses nothing */
static int openBackup(void *world, wchar_t *section, int size)
{
    char id[sizeof backup.world];
    if (!backup.path[0] || injected != 1 || !api->isHost() || !worldId(world, id, sizeof id))
        return 0;
    MultiByteToWideChar(CP_UTF8, 0, id, -1, section, size);
    if (strcmp(id, backup.world)) {
        strcpy(backup.world, id);
        for (int i = 0; i < UPGRADE_COUNT; i++) {
            wchar_t key[32];
            backupKey(i, key, 32);
            int level = (int)GetPrivateProfileIntW(section, key, 0, backup.path);
            backup.level[i] = level < 0 ? 0 : level > MAX_LEVEL ? MAX_LEVEL : level;
            backup.reported[i] = 0;
        }
    }
    return 1;
}

static void saveBackup(void *world)
{
    wchar_t section[sizeof backup.world];
    if (!openBackup(world, section, sizeof backup.world))
        return;
    for (int i = 0; i < UPGRADE_COUNT; i++) {
        int level = treeLevel(world, i);
        if (level <= backup.level[i])
            continue;
        wchar_t key[32], value[8];
        backupKey(i, key, 32);
        wsprintfW(value, L"%d", level);
        if (WritePrivateProfileStringW(section, key, value, backup.path))
            backup.level[i] = level;
    }
}

static int restoreLevel(void *world, int upgrade, int level)
{
    void *all = off.treeLevels >= 0 ? AT(world, off.treeLevels, void *) : NULL;
    void *team = NULL;
    void *lookup[2] = { game.local, &team };
    if (!all || !call(il.methodFromName(il.objectClass(all), "TryGetValue", 2), all, lookup) || !team)
        return 0;
    int32_t value = level;
    void *store[2] = { game.ids[upgrade], &value };
    return call(il.methodFromName(il.objectClass(team), "set_Item", 2), team, store);
}

static void restoreBackup(void *world)
{
    wchar_t section[sizeof backup.world];
    if (!openBackup(world, section, sizeof backup.world))
        return;
    for (int i = 0; i < UPGRADE_COUNT; i++) {
        if (now.level[i] >= backup.level[i])
            continue;
        int restored = restoreLevel(world, i, backup.level[i]);
        if (restored || !backup.reported[i])
            api->log(self, restored ? "%s: Stufe %d aus der Sicherung wiederhergestellt"
                                    : "%s: Stufe %d in der Sicherung, aber die Welt hat noch keine Upgrade-Liste",
                upgrades[i].id, backup.level[i]);
        backup.reported[i] = 1;
    }
}

static uint8_t purchaseHook(void *world, void *player, void *id, int32_t expected, void **message, const void *method)
{
    uint8_t bought = originalPurchase(world, player, id, expected, message, method);
    for (int i = 0; bought && i < UPGRADE_COUNT; i++) {
        if (stringIs(id, upgrades[i].id)) {
            saveBackup(world);
            now.next = 0;
            break;
        }
    }
    return bought;
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
        int level = treeLevel(world, i);
        if (level != now.level[i])
            api->log(self, "%s jetzt Stufe %d", upgrades[i].id, level);
        now.level[i] = level;
        now.factor[i] = on ? upgrades[i].factor[level] : 1.0f;
    }
    now.tankLevel = on ? now.level[UP_TANK] : 0;
    applyCatcher(now.tankLevel);
    restoreBackup(world);
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
    il.staticGet = api->export("il2cpp_field_static_get_value");
    il.staticSet = api->export("il2cpp_field_static_set_value");
    il.objectClass = api->export("il2cpp_object_get_class");
    il.elementClass = api->export("il2cpp_class_get_element_class");
    il.arrayNew = api->export("il2cpp_array_new");
    il.stringNew = api->export("il2cpp_string_new");
    il.objectNew = api->export("il2cpp_object_new");
    il.barrier = api->export("il2cpp_gc_wbarrier_set_field");
    il.classInit = api->export("il2cpp_runtime_class_init");
    il.invoke = api->export("il2cpp_runtime_invoke");
    il.methodFromName = api->export("il2cpp_class_get_method_from_name");
    il.handleNew = api->export("il2cpp_gchandle_new");
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
    off.treeLevels = offsetOf(world, "treeLevels");
    off.progressStore = offsetOf(world, "progressStore");
    return off.world >= 0 && off.launcher >= 0 && off.storageId >= 0 && off.capacity >= 0 && off.launchKilograms >= 0
        && robotFloats[TRAVEL_SPEED].offset >= 0 && robotFloats[HALF_WIDTH].offset >= 0;
}

static void prepareBackupPath(void)
{
    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(void *)prepareBackupPath, &module))
        return;
    DWORD length = GetModuleFileNameW(module, backup.path, MAX_PATH);
    wchar_t *slash = wcsrchr(backup.path, L'\\');
    if (!length || !slash || slash - backup.path + 12 >= MAX_PATH) {
        backup.path[0] = 0;
        return;
    }
    wcscpy(slash + 1, L"worlds.ini");
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
    mainThread = GetCurrentThreadId();
    api->describe(mod, "J.O.O.N.A.S. Upgrades", "J.O.O.N.A.S. Upgrades",
        "Neuer Tab SERVICES in der Upgrade-Matrix: Antrieb, Größe und Tank für alle J.O.O.N.A.S.",
        "New SERVICES tab in the upgrade matrix: drive, size and tank for every J.O.O.N.A.S.");

    const char *assembly = "Assembly-CSharp", *space = "MoonCleaners";
    game.treeClass = api->klass(assembly, space, "MoonUpgradeTree");
    game.treeLevel = api->method(assembly, space, "MoonWorld", "TreeLevel", 2);
    game.locCode = api->method(assembly, space, "MoonLoc", "get_Code", 0);
    game.getTransform = api->method("UnityEngine.CoreModule", "UnityEngine", "Component", "get_transform", 0);
    game.getScale = api->method("UnityEngine.CoreModule", "UnityEngine", "Transform", "get_localScale", 0);
    game.setScale = api->method("UnityEngine.CoreModule", "UnityEngine", "Transform", "set_localScale", 1);
    if (!resolveIl2cpp() || !resolveOffsets() || !game.treeClass || !api->code(game.treeLevel)
        || !api->code(game.getTransform) || !api->code(game.getScale) || !api->code(game.setScale)) {
        api->incompatible(mod, "J.O.O.N.A.S. oder Upgrade-Matrix nicht gefunden");
        return 0;
    }
    game.local = keptString("local");
    for (int i = 0; i < UPGRADE_COUNT; i++)
        game.ids[i] = keptString(upgrades[i].id);

    if (!hookMethod("MoonUpgradeTree", "Find", 1, (void *)findHook, (void **)&originalFind)
        || !hookMethod("MoonUpgradeTreeUI", ".ctor", 0, (void *)treeScreenHook, (void **)&originalTreeScreen)) {
        api->incompatible(mod, "Upgrade-Matrix konnte nicht erweitert werden");
        return 0;
    }
    if (!hookMethod("MoonRobotVacuum", "Update", 0, (void *)updateHook, (void **)&originalUpdate)
        || !hookMethod("MoonJoonasRig", "get_CoveredHalfWidth", 0, (void *)coveredHook, (void **)&originalCovered)
        || !hookMethod("MoonWorld", "FarSpec", 1, (void *)farSpecHook, (void **)&originalFarSpec)) {
        api->incompatible(mod, "J.O.O.N.A.S. konnte nicht erweitert werden");
        return 0;
    }
    if (!hookMethod("MoonWorld", "TryPurchaseTreeNode", 4, (void *)purchaseHook, (void **)&originalPurchase))
        api->log(mod, "Kauf nicht erreichbar, keine Sicherungsdatei");
    prepareCatcher();
    if (!catcher.site)
        api->note(mod, "Die Wartezeit am Fänger passt nicht zu dieser Spielversion.",
            "Catcher wait time is not compatible with this game version.");
    prepareBackupPath();
    api->note(mod, "Kaufen: Basis > Upgrade-Matrix > Tab SERVICES. Der Host braucht die Mod.",
        "Buy them at the base: upgrade matrix > SERVICES tab. The host needs the mod.");
    api->onEnabled(mod, onEnabled);
    api->log(mod, "bereit");
    return 1;
}
