#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "modapi.h"

#define UPGRADE_ID "conveyor.speed"
#define MAX_LEVEL 3
#define GLYPH_EXPRESS 14
#define STOCK_SPEED 1.1f

static const float factors[MAX_LEVEL + 1] = { 1.0f, 1.5f, 2.0f, 3.0f };
static const int prices[MAX_LEVEL] = { 150, 450, 1200 };

/** MoonWorld methods that load the BeltSpeed constant; shortcut: tread animation and belt audio keep the stock pace */
static const struct {
    const char *name;
    int argc, loads;
} users[] = {
    { "StepConveyors", 1, 3 },
    { "BeltSurfaceVelocity", 3, 1 },
    { "ApplyCrewBelts", 2, 1 },
};
#define USER_COUNT (int)(sizeof users / sizeof *users)
#define MAX_SITES 8

static const ModApi *api;
static Mod *self;
static volatile float *speed;
static unsigned char *sites[MAX_SITES];
static int siteCount, level;
static ULONGLONG nextLevelCheck;

static void (*originalUpdate)(void *, const void *);

/** rip-relative operands reach 2 GB, so the speed value has to live near GameAssembly.dll */
static float *nearFloat(unsigned char *around)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    uintptr_t step = info.dwAllocationGranularity, base = (uintptr_t)around & ~(step - 1);
    for (uintptr_t distance = step; distance < 0x40000000; distance += step) {
        uintptr_t candidates[2] = { base + distance, base > distance ? base - distance : 0 };
        for (int i = 0; i < 2; i++) {
            void *memory = candidates[i]
                ? VirtualAlloc((void *)candidates[i], step, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
                : NULL;
            if (memory)
                return memory;
        }
    }
    return NULL;
}

/** Finds movss xmm, [rip+x] loads of 1.1f inside one game function, using its unwind entry for the bounds */
static int findLoads(unsigned char *code, int expected)
{
    DWORD64 imageBase;
    RUNTIME_FUNCTION *entry = RtlLookupFunctionEntry((DWORD64)code, &imageBase, NULL);
    if (!entry)
        return 0;
    unsigned char *image = (unsigned char *)imageBase;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(image + ((IMAGE_DOS_HEADER *)image)->e_lfanew);
    unsigned char *imageEnd = image + nt->OptionalHeader.SizeOfImage;
    unsigned char *begin = image + entry->BeginAddress, *end = image + entry->EndAddress;
    int found = 0;
    for (unsigned char *p = begin; p + 9 <= end && siteCount < MAX_SITES; p++) {
        int rex = p[1] >= 0x40 && p[1] <= 0x4F;
        unsigned char *opcode = p + 1 + rex;
        if (p[0] != 0xF3 || opcode[0] != 0x0F || opcode[1] != 0x10 || (opcode[2] & 0xC7) != 0x05)
            continue;
        unsigned char *next = opcode + 7;
        int32_t displacement;
        memcpy(&displacement, opcode + 3, sizeof displacement);
        unsigned char *target = next + displacement;
        if (target < image || target + sizeof(float) > imageEnd || *(const float *)target != STOCK_SPEED)
            continue;
        sites[siteCount++] = next;
        found++;
    }
    return found == expected;
}

static int redirect(unsigned char *next)
{
    intptr_t distance = (unsigned char *)speed - next;
    int32_t displacement = (int32_t)distance;
    return distance == displacement && api->write(next - 4, &displacement, sizeof displacement);
}

static void updateHook(void *world, const void *method)
{
    ULONGLONG now = GetTickCount64();
    if (now >= nextLevelCheck) {
        nextLevelCheck = now + 500;
        int bought = api->upgradeLevel(world, UPGRADE_ID);
        if (bought != level)
            api->log(self, "Stufe %d, Band %.2f m/s", bought, STOCK_SPEED * factors[bought]);
        level = bought;
        *speed = STOCK_SPEED * (api->enabled(self) ? factors[level] : 1.0f);
    }
    originalUpdate(world, method);
}

MOD_EXPORT int ModInit(const ModApi *modApi, Mod *mod)
{
    api = modApi;
    self = mod;
    api->describe(mod, "Förderband-Upgrades", "Conveyor Upgrades",
        "Bänder, Splitter und Merger laufen schneller. Kaufbar in der Upgrade-Matrix im Tab SERVICES.",
        "Belts, splitters and mergers run faster. Buy it in the upgrade matrix, SERVICES tab.");
    if (api->version < 2) {
        api->incompatible(mod, "Loader zu alt");
        return 0;
    }
    for (int i = 0; i < USER_COUNT; i++) {
        unsigned char *code = api->code(api->method("Assembly-CSharp", "MoonCleaners", "MoonWorld", users[i].name,
            users[i].argc));
        if (!code || !findLoads(code, users[i].loads)) {
            api->incompatible(mod, "Band-Tempo im Spiel-Code nicht gefunden");
            return 0;
        }
    }
    speed = nearFloat(sites[0]);
    if (!speed) {
        api->incompatible(mod, "Kein Speicher in der Nähe des Spiel-Codes");
        return 0;
    }
    *speed = STOCK_SPEED;
    for (int i = 0; i < siteCount; i++) {
        if (!redirect(sites[i])) {
            api->incompatible(mod, "Band-Tempo konnte nicht umgeleitet werden");
            return 0;
        }
    }
    void *update = api->code(api->method("Assembly-CSharp", "MoonCleaners", "MoonWorld", "Update", 0));
    if (!update || !api->hook(update, (void *)updateHook, (void **)&originalUpdate)) {
        api->incompatible(mod, "Welt konnte nicht erweitert werden");
        return 0;
    }
    api->upgrade(mod, UPGRADE_ID, "Förderband-Tempo", "Conveyor Speed",
        "Bänder, Splitter und Merger laufen 1,5-, 2- oder 3-mal so schnell.",
        "Belts, splitters and mergers run 1.5x, 2x or 3x as fast.", GLYPH_EXPRESS, MAX_LEVEL, prices);
    api->log(mod, "bereit, %d Stellen umgeleitet", siteCount);
    return 1;
}
