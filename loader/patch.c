#include <stdlib.h>
#include <string.h>

#include "MinHook.h"
#include "loader.h"

typedef struct {
    unsigned char bytes[256];
    unsigned char known[256];
    size_t length;
} Pattern;

static void parsePattern(const char *text, Pattern *pattern)
{
    pattern->length = 0;
    while (*text && pattern->length < sizeof pattern->bytes) {
        if (*text == ' ') {
            text++;
            continue;
        }
        pattern->known[pattern->length] = *text != '?';
        pattern->bytes[pattern->length] = *text == '?' ? 0 : (unsigned char)strtoul(text, NULL, 16);
        pattern->length++;
        text += 2;
    }
}

static int matchesAt(const unsigned char *code, const Pattern *pattern)
{
    for (size_t i = 0; i < pattern->length; i++)
        if (pattern->known[i] && code[i] != pattern->bytes[i])
            return 0;
    return 1;
}

unsigned char *findPattern(const char *text, int *matches)
{
    Pattern pattern;
    parsePattern(text, &pattern);
    unsigned char *base = (unsigned char *)gameAssembly, *match = NULL;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    int found = 0;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++, section++) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        unsigned char *start = base + section->VirtualAddress;
        for (DWORD i = 0; i + pattern.length <= section->Misc.VirtualSize; i++) {
            if (start[i] == pattern.bytes[0] && matchesAt(start + i, &pattern)) {
                match = start + i;
                found++;
            }
        }
    }
    if (matches)
        *matches = found;
    return found == 1 ? match : NULL;
}

int writeCode(void *target, const void *bytes, size_t length)
{
    DWORD oldProtection;
    if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &oldProtection))
        return 0;
    memcpy(target, bytes, length);
    VirtualProtect(target, length, oldProtection, &oldProtection);
    FlushInstructionCache(GetCurrentProcess(), target, length);
    return 1;
}

int hooksStart(void)
{
    MH_STATUS status = MH_Initialize();
    return status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
}

int hookFunction(void *target, void *detour, void **original)
{
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        loaderLog("Einhaengen bei %p fehlgeschlagen: %s", target, MH_StatusToString(status));
        return 0;
    }
    status = MH_EnableHook(target);
    if (status != MH_OK) {
        loaderLog("Aktivieren bei %p fehlgeschlagen: %s", target, MH_StatusToString(status));
        return 0;
    }
    return 1;
}

int setHookEnabled(void *target, int enabled)
{
    MH_STATUS status = enabled ? MH_EnableHook(target) : MH_DisableHook(target);
    return status == MH_OK || status == MH_ERROR_ENABLED || status == MH_ERROR_DISABLED;
}
