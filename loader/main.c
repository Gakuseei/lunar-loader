#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "loader.h"

#define LOADER_VERSION "1.0"

int loadRealVersion(void);

Mod mods[MAX_MODS];
int modCount;
HMODULE gameAssembly;
unsigned char *gameAssemblyStart, *gameAssemblyEnd;

static HMODULE self;
static wchar_t modsDir[MAX_PATH];
static HANDLE loaderLogFile = INVALID_HANDLE_VALUE;
static volatile LONG crashesLogged;
static FARPROC(WINAPI *realGetProcAddress)(HMODULE, LPCSTR);
static int (*realIl2cppInit)(const char *);

static void writeLine(HANDLE file, const char *format, va_list args)
{
    if (file == INVALID_HANDLE_VALUE)
        return;
    char line[1024];
    SYSTEMTIME now;
    GetLocalTime(&now);
    int length = snprintf(line, sizeof line - 2, "%02d:%02d:%02d ", now.wHour, now.wMinute, now.wSecond);
    int text = vsnprintf(line + length, sizeof line - 2 - length, format, args);
    length += text < 0 ? 0 : text >= (int)sizeof line - 2 - length ? (int)sizeof line - 3 - length : text;
    line[length++] = '\r';
    line[length++] = '\n';
    DWORD written;
    WriteFile(file, line, length, &written, NULL);
}

void loaderLog(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    writeLine(loaderLogFile, format, args);
    va_end(args);
}

void modLog(Mod *mod, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    writeLine(mod->log, format, args);
    va_end(args);
}

static HANDLE createLog(const wchar_t *path)
{
    wchar_t previous[MAX_PATH];
    size_t length = wcslen(path);
    if (length > 4 && length + 6 < MAX_PATH) {
        swprintf(previous, MAX_PATH, L"%.*ls-prev.log", (int)(length - 4), path);
        MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING);
    }
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void utf8(const wchar_t *wide, char *buffer, int size)
{
    int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, size, NULL, NULL);
    if (written <= 0)
        buffer[0] = 0;
}

static void wide(const char *text, wchar_t *buffer, int size)
{
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, buffer, size) <= 0)
        buffer[0] = 0;
}

static void copyText(char *target, size_t size, const char *source)
{
    size_t length = 0;
    while (source && source[length] && length + 1 < size) {
        target[length] = source[length];
        length++;
    }
    target[length] = 0;
}

/** Logs where a crash happened; the game's own crash handling continues unchanged */
static LONG CALLBACK logCrash(EXCEPTION_POINTERS *info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&crashesLogged) > 5)
        return EXCEPTION_CONTINUE_SEARCH;
    unsigned char *address = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = NULL;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)address, &module))
        GetModuleFileNameW(module, path, MAX_PATH);
    wchar_t *file = wcsrchr(path, L'\\');
    char name[MAX_PATH];
    utf8(file ? file + 1 : path, name, sizeof name);
    loaderLog("Fehler %08lx in %s+0x%llx", code, name, (unsigned long long)(address - (unsigned char *)module));
    return EXCEPTION_CONTINUE_SEARCH;
}

int isHost(void)
{
    static uint8_t (*joined)(const void *);
    static const MethodInfo *method;
    if (!method) {
        method = findMethod("Assembly-CSharp", "MoonCleaners", "MoonCrew", "get_Joined", 0);
        joined = (uint8_t(*)(const void *))methodCode(method);
    }
    return joined ? !joined(method) : 1;
}

int germanText(void)
{
    static Il2CppString *(*code)(const void *);
    static const MethodInfo *method;
    if (!method) {
        method = findMethod("Assembly-CSharp", "MoonCleaners", "MoonLoc", "get_Code", 0);
        code = (Il2CppString * (*)(const void *)) methodCode(method);
    }
    char language[16];
    if (!code || !stringToUtf8(code(method), language, sizeof language))
        return 0;
    return (language[0] == 'd' || language[0] == 'D') && (language[1] == 'e' || language[1] == 'E');
}

void setModEnabled(Mod *mod, int enabled)
{
    enabled = enabled != 0;
    if (mod->enabled == enabled)
        return;
    mod->enabled = enabled;
    WritePrivateProfileStringW(L"Mod", L"Enabled", enabled ? L"1" : L"0", mod->ini);
    modLog(mod, enabled ? "eingeschaltet" : "ausgeschaltet");
    if (mod->loaded && !mod->incompatible[0] && mod->onEnabled)
        mod->onEnabled(mod, enabled);
}

void saveSetting(Setting *setting)
{
    wchar_t key[48], value[16];
    wide(setting->key, key, 48);
    swprintf(value, 16, L"%d", setting->value);
    WritePrivateProfileStringW(L"Settings", key, value, setting->mod->ini);
}

static Setting *addSetting(Mod *mod, SettingKind kind, const char *key, const char *labelDe, const char *labelEn, int flags,
    void (*changed)(Mod *mod))
{
    if (mod->settingCount >= MAX_SETTINGS) {
        modLog(mod, "zu viele Einstellungen, %s fehlt im Menue", key ? key : labelDe);
        return NULL;
    }
    Setting *setting = &mod->settings[mod->settingCount++];
    setting->mod = mod;
    setting->kind = kind;
    copyText(setting->key, sizeof setting->key, key);
    copyText(setting->labelDe, sizeof setting->labelDe, labelDe);
    copyText(setting->labelEn, sizeof setting->labelEn, labelEn);
    setting->flags = flags;
    setting->changed = changed;
    return setting;
}

static int storedValue(Mod *mod, const char *key, int fallback)
{
    wchar_t name[48];
    wide(key, name, 48);
    return (int)GetPrivateProfileIntW(L"Settings", name, fallback, mod->ini);
}

static void apiDescribe(Mod *mod, const char *nameDe, const char *nameEn, const char *infoDe, const char *infoEn)
{
    copyText(mod->nameDe, sizeof mod->nameDe, nameDe);
    copyText(mod->nameEn, sizeof mod->nameEn, nameEn);
    copyText(mod->infoDe, sizeof mod->infoDe, infoDe);
    copyText(mod->infoEn, sizeof mod->infoEn, infoEn);
}

static void apiIncompatible(Mod *mod, const char *reason)
{
    copyText(mod->incompatible, sizeof mod->incompatible, reason);
    modLog(mod, "passt nicht zu dieser Spielversion: %s", reason);
}

static void apiOnEnabled(Mod *mod, void (*callback)(Mod *mod, int enabled))
{
    mod->onEnabled = callback;
}

static int apiEnabled(Mod *mod)
{
    return mod->enabled && !mod->incompatible[0];
}

static const int *apiToggle(Mod *mod, const char *key, const char *labelDe, const char *labelEn, int fallback, int flags,
    void (*changed)(Mod *mod))
{
    static const int off = 0;
    Setting *setting = addSetting(mod, SETTING_TOGGLE, key, labelDe, labelEn, flags, changed);
    if (!setting)
        return &off;
    setting->value = storedValue(mod, key, fallback) != 0;
    return &setting->value;
}

static const int *apiChoice(Mod *mod, const char *key, const char *labelDe, const char *labelEn, const char *const *options,
    int count, int fallback, int flags, void (*changed)(Mod *mod))
{
    static const int first = 0;
    Setting *setting = addSetting(mod, SETTING_CHOICE, key, labelDe, labelEn, flags, changed);
    if (!setting)
        return &first;
    setting->optionCount = count < MAX_OPTIONS ? count : MAX_OPTIONS;
    for (int i = 0; i < setting->optionCount; i++)
        copyText(setting->options[i], sizeof setting->options[i], options[i]);
    int value = storedValue(mod, key, fallback);
    setting->value = value < 0 || value >= setting->optionCount ? fallback : value;
    return &setting->value;
}

static void apiNote(Mod *mod, const char *textDe, const char *textEn)
{
    addSetting(mod, SETTING_NOTE, "", textDe, textEn, 0, NULL);
}

static void *apiClass(const char *assembly, const char *namespaze, const char *name)
{
    return findClass(assembly, namespaze, name);
}

static void *apiMethod(const char *assembly, const char *namespaze, const char *klass, const char *name, int argc)
{
    return findMethod(assembly, namespaze, klass, name, argc);
}

static void *apiCode(const void *method)
{
    return methodCode(method);
}

static const ModApi modApi = {
    .version = MODAPI_VERSION,
    .log = modLog,
    .describe = apiDescribe,
    .incompatible = apiIncompatible,
    .onEnabled = apiOnEnabled,
    .enabled = apiEnabled,
    .toggle = apiToggle,
    .choice = apiChoice,
    .note = apiNote,
    .isHost = isHost,
    .klass = apiClass,
    .method = apiMethod,
    .code = apiCode,
    .export = il2cppExport,
    .find = findPattern,
    .write = writeCode,
    .hook = hookFunction,
    .hookEnabled = setHookEnabled,
};

static void loadMod(const wchar_t *folder)
{
    if (modCount >= MAX_MODS) {
        loaderLog("zu viele Mods, %ls wird nicht geladen", folder);
        return;
    }
    wchar_t dir[MAX_PATH], dll[MAX_PATH], logPath[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\%ls", modsDir, folder);
    swprintf(dll, MAX_PATH, L"%ls\\%ls.dll", dir, folder);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        loaderLog("%ls: keine %ls.dll im Ordner, uebersprungen", folder, folder);
        return;
    }

    Mod *mod = &mods[modCount++];
    utf8(folder, mod->name, sizeof mod->name);
    copyText(mod->nameDe, sizeof mod->nameDe, mod->name);
    copyText(mod->nameEn, sizeof mod->nameEn, mod->name);
    swprintf(mod->ini, MAX_PATH, L"%ls\\settings.ini", dir);
    swprintf(logPath, MAX_PATH, L"%ls\\%ls.log", dir, folder);
    mod->log = createLog(logPath);
    mod->enabled = GetPrivateProfileIntW(L"Mod", L"Enabled", 1, mod->ini) != 0;

    HMODULE library = LoadLibraryW(dll);
    ModInitFunction init = library ? (ModInitFunction)(void *)GetProcAddress(library, "ModInit") : NULL;
    if (!init) {
        apiIncompatible(mod, library ? "ModInit fehlt" : "DLL konnte nicht geladen werden");
        loaderLog("%s: nicht geladen", mod->name);
        return;
    }
    modLog(mod, "startet");
    int ok = init(&modApi, mod);
    if (!ok && !mod->incompatible[0])
        apiIncompatible(mod, "Start fehlgeschlagen");
    mod->loaded = ok;
    loaderLog("%s: %s", mod->name, ok && !mod->incompatible[0] ? "geladen" : "passt nicht");
    if (mod->loaded && !mod->incompatible[0] && mod->enabled && mod->onEnabled)
        mod->onEnabled(mod, 1);
}

static void loadMods(void)
{
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", modsDir);
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) {
        loaderLog("Ordner Mods ist leer");
        return;
    }
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && entry.cFileName[0] != L'.')
            loadMod(entry.cFileName);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    loaderLog("%d Mods gefunden", modCount);
}

static void startFramework(void)
{
    unsigned char *base = (unsigned char *)gameAssembly;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    gameAssemblyStart = base;
    gameAssemblyEnd = base + nt->OptionalHeader.SizeOfImage;

    if (!il2cppLoad(gameAssembly)) {
        loaderLog("Spiel-Laufzeit passt nicht, Mods bleiben aus");
        return;
    }
    if (!hooksStart()) {
        loaderLog("MinHook startet nicht, Mods bleiben aus");
        return;
    }
    loaderLog(menuInstall() ? "Menue MODS eingerichtet" : "Menue MODS nicht moeglich, Mods laufen trotzdem");
    loadMods();
}

static int il2cppInitHook(const char *domainName)
{
    int result = realIl2cppInit(domainName);
    loaderLog("Spiel-Laufzeit gestartet");
    startFramework();
    return result;
}

static FARPROC WINAPI getProcAddressHook(HMODULE module, LPCSTR name)
{
    FARPROC result = realGetProcAddress(module, name);
    if (result && !realIl2cppInit && ((ULONG_PTR)name >> 16) && strcmp(name, "il2cpp_init") == 0) {
        gameAssembly = module;
        realIl2cppInit = (int (*)(const char *))(void *)result;
        return (FARPROC)(void *)il2cppInitHook;
    }
    return result;
}

static int patchImport(HMODULE module, const char *library, const char *function, void *replacement, void **original)
{
    unsigned char *base = (unsigned char *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress)
        return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *entry = (IMAGE_IMPORT_DESCRIPTOR *)(base + imports.VirtualAddress); entry->Name; entry++) {
        if (_stricmp((const char *)base + entry->Name, library))
            continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (entry->OriginalFirstThunk ? entry->OriginalFirstThunk : entry->FirstThunk));
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + entry->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            IMAGE_IMPORT_BY_NAME *byName = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)byName->Name, function))
                continue;
            DWORD protection;
            if (!VirtualProtect(&slots->u1.Function, sizeof slots->u1.Function, PAGE_READWRITE, &protection))
                return 0;
            *original = (void *)slots->u1.Function;
            slots->u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&slots->u1.Function, sizeof slots->u1.Function, protection, &protection);
            return 1;
        }
    }
    return 0;
}

static void preparePaths(void)
{
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!length || !slash)
        return;
    *slash = 0;
    swprintf(modsDir, MAX_PATH, L"%ls\\Mods", path);
    CreateDirectoryW(modsDir, NULL);
    wchar_t logPath[MAX_PATH];
    swprintf(logPath, MAX_PATH, L"%ls\\ModLoader.log", modsDir);
    loaderLogFile = createLog(logPath);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    self = instance;
    DisableThreadLibraryCalls(instance);
    loadRealVersion();
    HMODULE unity = GetModuleHandleW(L"UnityPlayer.dll");
    if (!unity)
        return TRUE;
    preparePaths();
    loaderLog("Mod-Lader %s", LOADER_VERSION);
    AddVectoredExceptionHandler(1, logCrash);
    if (!patchImport(unity, "KERNEL32.dll", "GetProcAddress", (void *)getProcAddressHook, (void **)&realGetProcAddress))
        loaderLog("Konnte mich nicht in UnityPlayer.dll einklinken, Mods bleiben aus");
    return TRUE;
}
