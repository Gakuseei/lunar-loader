#include <stdio.h>
#include <string.h>

#include "loader.h"

#define MAX_UPGRADES 32
#define MAX_LEVEL 5
#define STOCK_BRANCHES 6
#define SERVICES_BRANCH 6

typedef struct {
    Mod *mod;
    char id[48], titleDe[96], titleEn[96], summaryDe[256], summaryEn[256];
    int glyph, maxLevel, prices[MAX_LEVEL];
    GcHandle key;
} Upgrade;

static Upgrade upgrades[MAX_UPGRADES];
static int upgradeCount;

static struct {
    void (*staticGet)(void *, void *);
    void (*staticSet)(void *, void *);
    void *(*fieldFromName)(void *, const char *);
    void (*classInit)(void *);
    void *(*objectNew)(void *);
    void (*barrier)(void *, void **, void *);
} il;

static Il2CppClass *treeClass;
static const MethodInfo *treeLevelMethod;
static GcHandle local;
static DWORD mainThread;
static volatile LONG injecting;
static int injected;

static void *(*originalFind)(void *, const void *);
static void (*originalTreeScreen)(void *, const void *);
static uint8_t (*originalPurchase)(void *, void *, void *, int32_t, void **, const void *);

static struct {
    wchar_t path[MAX_PATH];
    void *world;
    char identity[96];
    int level[MAX_UPGRADES];
    int pending;
} backup;

static void copyText(char *target, size_t size, const char *source)
{
    size_t length = 0;
    while (source && source[length] && length + 1 < size) {
        target[length] = source[length];
        length++;
    }
    target[length] = 0;
}

static uintptr_t arrayLength(void *array) { return *(uintptr_t *)((unsigned char *)array + 0x18); }
static void **arrayItems(void *array) { return (void **)((unsigned char *)array + 0x20); }

static void *staticValue(const char *name)
{
    void *field = il.fieldFromName(treeClass, name), *value = NULL;
    if (field)
        il.staticGet(field, &value);
    return value;
}

/** Unlike the getter, il2cpp's static setter takes a reference-type value itself, not a pointer to it */
static void setStatic(const char *name, void *value)
{
    void *field = il.fieldFromName(treeClass, name);
    if (field)
        il.staticSet(field, value);
}

static void setReference(void *object, const char *name, void *value)
{
    int offset = fieldOffset(classOf(object), name);
    if (offset >= 0)
        il.barrier(object, (void **)((unsigned char *)object + offset), value);
}

static void setInt(void *object, const char *name, int value)
{
    int offset = fieldOffset(classOf(object), name);
    if (offset >= 0)
        *(int32_t *)((unsigned char *)object + offset) = value;
}

static void *sameKindArray(void *array, int length)
{
    return newArray(elementClass(classOf(array)), length);
}

static void *longerArray(void *array, void *extra)
{
    int length = (int)arrayLength(array);
    void *longer = sameKindArray(array, length + 1);
    for (int i = 0; i < length; i++)
        setArrayItem(longer, i, arrayItems(array)[i]);
    setArrayItem(longer, length, extra);
    return longer;
}

static int stringIs(void *string, const char *text)
{
    char buffer[64];
    return string && stringToUtf8(string, buffer, sizeof buffer) && !strcmp(buffer, text);
}

static void *findNode(void *nodes, const char *id)
{
    for (uintptr_t i = 0; i < arrayLength(nodes); i++) {
        void *node = arrayItems(nodes)[i];
        if (node && stringIs(*(void **)((unsigned char *)node + fieldOffset(classOf(node), "id")), id))
            return node;
    }
    return NULL;
}

static void *newNode(void *template, Upgrade *upgrade, int de)
{
    void *node = il.objectNew(classOf(template));
    int pricesOffset = fieldOffset(classOf(template), "prices");
    int requiresOffset = fieldOffset(classOf(template), "requires");
    void *prices = sameKindArray(*(void **)((unsigned char *)template + pricesOffset), upgrade->maxLevel);
    memcpy(arrayItems(prices), upgrade->prices, upgrade->maxLevel * sizeof(int32_t));
    setReference(node, "id", aliveTarget(upgrade->key));
    setReference(node, "title", newString(de ? upgrade->titleDe : upgrade->titleEn));
    setReference(node, "summary", newString(de ? upgrade->summaryDe : upgrade->summaryEn));
    setReference(node, "prices", prices);
    setReference(node, "requires", sameKindArray(*(void **)((unsigned char *)template + requiresOffset), 0));
    setInt(node, "branch", SERVICES_BRANCH);
    setInt(node, "glyph", upgrade->glyph);
    setInt(node, "maxLevel", upgrade->maxLevel);
    return node;
}

/** Adds the SERVICES tab and its nodes before the game first looks anything up in the upgrade tree */
static void inject(void)
{
    if (injected || InterlockedCompareExchange(&injecting, 1, 0))
        return;
    injected = -1;
    il.classInit(treeClass);
    void *names = staticValue("BranchNames"), *colors = staticValue("BranchColors");
    void *taglines = staticValue("BranchTaglines"), *nodes = staticValue("Nodes"), *byId = staticValue("byId");
    if (!names || !colors || !taglines || !nodes || !byId || !arrayLength(nodes)) {
        injected = 0;
        InterlockedExchange(&injecting, 0);
        return;
    }
    void *template = findNode(nodes, "clean.width");
    if (arrayLength(names) != STOCK_BRANCHES || arrayLength(colors) != STOCK_BRANCHES
        || arrayLength(taglines) != STOCK_BRANCHES || !template) {
        loaderLog("Upgrade-Matrix sieht anders aus als erwartet, Tab SERVICES fehlt");
        return;
    }
    const MethodInfo *add = classMethod(classOf(byId), "set_Item", 2);
    int de = GetCurrentThreadId() == mainThread && germanText();
    void *allNodes = nodes;
    for (int i = 0; i < upgradeCount; i++) {
        void *node = newNode(template, &upgrades[i], de);
        void *params[2] = { aliveTarget(upgrades[i].key), node };
        invoke(add, byId, params);
        if (invokeError)
            return;
        allNodes = longerArray(allNodes, node);
    }
    static const float servicesColor[4] = { 1.0f, 0.47f, 0.56f, 1.0f };
    int colorSize = arrayElementSize(classOf(colors));
    void *allColors = sameKindArray(colors, STOCK_BRANCHES + 1);
    memcpy(arrayItems(allColors), arrayItems(colors), STOCK_BRANCHES * colorSize);
    memcpy((unsigned char *)arrayItems(allColors) + STOCK_BRANCHES * colorSize, servicesColor, sizeof servicesColor);
    void *allTaglines = longerArray(taglines,
        newString(de ? "Upgrades aus euren Mods." : "Upgrades from your mods."));
    setStatic("BranchColors", allColors);
    setStatic("BranchTaglines", allTaglines);
    setStatic("Nodes", allNodes);
    setStatic("BranchNames", longerArray(names, newString("SERVICES")));
    injected = 1;
    loaderLog("Tab SERVICES mit %d Upgrades in der Upgrade-Matrix", upgradeCount);
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

static Upgrade *upgradeById(const char *id)
{
    for (int i = 0; i < upgradeCount; i++)
        if (!strcmp(upgrades[i].id, id))
            return &upgrades[i];
    return NULL;
}

static int treeLevel(void *world, Upgrade *upgrade)
{
    int (*level)(void *, void *, void *, const void *) = methodCode(treeLevelMethod);
    int found = level && injected == 1 ? level(world, aliveTarget(local), aliveTarget(upgrade->key), treeLevelMethod) : 0;
    return found < 0 ? 0 : found > upgrade->maxLevel ? upgrade->maxLevel : found;
}

static int worldIdentity(void *world, char *buffer, int size)
{
    int storeOffset = fieldOffset(classOf(world), "progressStore");
    void *store = storeOffset >= 0 ? *(void **)((unsigned char *)world + storeOffset) : NULL;
    int identityOffset = store ? fieldOffset(classOf(store), "identity") : -1;
    return identityOffset >= 0 && stringToUtf8(*(void **)((unsigned char *)store + identityOffset), buffer, size) >= 8;
}

static void backupKey(Upgrade *upgrade, wchar_t *key, int size)
{
    MultiByteToWideChar(CP_UTF8, 0, upgrade->id, -1, key, size);
}

/** Bought levels also go to Mods/upgrades.ini, so a save made without the mods loses nothing */
static int openBackup(void *world, wchar_t *section, int size)
{
    char identity[sizeof backup.identity];
    if (!backup.path[0] || injected != 1 || !isHost() || !worldIdentity(world, identity, sizeof identity))
        return 0;
    MultiByteToWideChar(CP_UTF8, 0, identity, -1, section, size);
    if (strcmp(identity, backup.identity)) {
        copyText(backup.identity, sizeof backup.identity, identity);
        backup.pending = 0;
        for (int i = 0; i < upgradeCount; i++) {
            wchar_t key[48];
            backupKey(&upgrades[i], key, 48);
            int level = (int)GetPrivateProfileIntW(section, key, 0, backup.path);
            backup.level[i] = level < 0 ? 0 : level > upgrades[i].maxLevel ? upgrades[i].maxLevel : level;
            backup.pending |= backup.level[i] > 0;
        }
    }
    return 1;
}

static int restoreLevel(void *world, Upgrade *upgrade, int level)
{
    int levelsOffset = fieldOffset(classOf(world), "treeLevels");
    void *all = levelsOffset >= 0 ? *(void **)((unsigned char *)world + levelsOffset) : NULL;
    void *team = NULL;
    void *lookup[2] = { aliveTarget(local), &team };
    if (!all || !invoke(classMethod(classOf(all), "TryGetValue", 2), all, lookup) || !team)
        return 0;
    int32_t value = level;
    void *store[2] = { aliveTarget(upgrade->key), &value };
    invoke(classMethod(classOf(team), "set_Item", 2), team, store);
    return !invokeError;
}

static void restoreBackup(void *world)
{
    wchar_t section[sizeof backup.identity];
    if (!openBackup(world, section, sizeof backup.identity))
        return;
    backup.world = world;
    if (!backup.pending)
        return;
    backup.pending = 0;
    for (int i = 0; i < upgradeCount; i++) {
        if (treeLevel(world, &upgrades[i]) >= backup.level[i])
            continue;
        if (restoreLevel(world, &upgrades[i], backup.level[i]))
            loaderLog("%s: Stufe %d aus der Sicherung wiederhergestellt", upgrades[i].id, backup.level[i]);
        else
            backup.pending = 1;
    }
}

static void saveBackup(void *world, Upgrade *upgrade)
{
    wchar_t section[sizeof backup.identity];
    if (!openBackup(world, section, sizeof backup.identity))
        return;
    int index = (int)(upgrade - upgrades), level = treeLevel(world, upgrade);
    if (level <= backup.level[index])
        return;
    wchar_t key[48], value[8];
    backupKey(upgrade, key, 48);
    wsprintfW(value, L"%d", level);
    if (WritePrivateProfileStringW(section, key, value, backup.path))
        backup.level[index] = level;
}

static uint8_t purchaseHook(void *world, void *player, void *id, int32_t expected, void **message, const void *method)
{
    uint8_t bought = originalPurchase(world, player, id, expected, message, method);
    char text[48];
    Upgrade *upgrade = bought && stringToUtf8(id, text, sizeof text) ? upgradeById(text) : NULL;
    if (upgrade)
        saveBackup(world, upgrade);
    return bought;
}

int registerUpgrade(Mod *mod, const char *id, const char *titleDe, const char *titleEn, const char *summaryDe,
    const char *summaryEn, int glyph, int maxLevel, const int *prices)
{
    if (upgradeCount >= MAX_UPGRADES || maxLevel < 1 || maxLevel > MAX_LEVEL || upgradeById(id) || injected) {
        modLog(mod, "Upgrade %s nicht angelegt", id);
        return 0;
    }
    Upgrade *upgrade = &upgrades[upgradeCount++];
    upgrade->mod = mod;
    copyText(upgrade->id, sizeof upgrade->id, id);
    copyText(upgrade->titleDe, sizeof upgrade->titleDe, titleDe);
    copyText(upgrade->titleEn, sizeof upgrade->titleEn, titleEn);
    copyText(upgrade->summaryDe, sizeof upgrade->summaryDe, summaryDe);
    copyText(upgrade->summaryEn, sizeof upgrade->summaryEn, summaryEn);
    upgrade->glyph = glyph;
    upgrade->maxLevel = maxLevel;
    memcpy(upgrade->prices, prices, maxLevel * sizeof *prices);
    upgrade->key = keepAlive(newString(id));
    return 1;
}

int upgradeLevel(void *world, const char *id)
{
    Upgrade *upgrade = upgradeById(id);
    if (!upgrade || !world)
        return 0;
    if (world != backup.world || backup.pending)
        restoreBackup(world);
    return treeLevel(world, upgrade);
}

static int hookGame(const char *klass, const char *name, int argc, void *detour, void **original)
{
    void *code = methodCode(findMethod("Assembly-CSharp", "MoonCleaners", klass, name, argc));
    return code && hookFunction(code, detour, original);
}

void upgradesInstall(const wchar_t *modsDir)
{
    if (!upgradeCount)
        return;
    mainThread = GetCurrentThreadId();
    il.staticGet = il2cppExport("il2cpp_field_static_get_value");
    il.staticSet = il2cppExport("il2cpp_field_static_set_value");
    il.fieldFromName = il2cppExport("il2cpp_class_get_field_from_name");
    il.classInit = il2cppExport("il2cpp_runtime_class_init");
    il.objectNew = il2cppExport("il2cpp_object_new");
    il.barrier = il2cppExport("il2cpp_gc_wbarrier_set_field");
    treeClass = findClass("Assembly-CSharp", "MoonCleaners", "MoonUpgradeTree");
    treeLevelMethod = findMethod("Assembly-CSharp", "MoonCleaners", "MoonWorld", "TreeLevel", 2);
    local = keepAlive(newString("local"));
    swprintf(backup.path, MAX_PATH, L"%ls\\upgrades.ini", modsDir);
    if (!il.staticGet || !il.staticSet || !il.fieldFromName || !il.classInit || !il.objectNew || !il.barrier
        || !treeClass || !methodCode(treeLevelMethod)
        || !hookGame("MoonUpgradeTree", "Find", 1, (void *)findHook, (void **)&originalFind)
        || !hookGame("MoonUpgradeTreeUI", ".ctor", 0, (void *)treeScreenHook, (void **)&originalTreeScreen)) {
        loaderLog("Upgrade-Matrix nicht erweiterbar, %d Upgrades fehlen", upgradeCount);
        upgradeCount = 0;
        return;
    }
    if (!hookGame("MoonWorld", "TryPurchaseTreeNode", 4, (void *)purchaseHook, (void **)&originalPurchase))
        loaderLog("Kauf nicht erreichbar, keine Sicherung der Upgrade-Stufen");
}
