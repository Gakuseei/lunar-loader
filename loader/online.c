#include <stdio.h>
#include <string.h>

#include "loader.h"

#include <bcrypt.h>
#include <urlmon.h>

#ifndef INDEX_URL
#define INDEX_URL L"https://raw.githubusercontent.com/Gakuseei/lunar-loader/main/mods.ini"
#define URL_PREFIX "https://"
#endif

OnlineMod onlineMods[MAX_ONLINE];
int onlineCount;

static wchar_t modsPath[MAX_PATH], indexPath[MAX_PATH], settingsPath[MAX_PATH];
static volatile LONG state = ONLINE_OFF, changed, busy;

static void wide(const char *text, wchar_t *buffer, int size)
{
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, buffer, size) <= 0)
        buffer[0] = 0;
}

static void utf8(const wchar_t *text, char *buffer, int size)
{
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, size, NULL, NULL) <= 0)
        buffer[0] = 0;
}

static void notify(void)
{
    InterlockedExchange(&changed, 1);
}

int onlineChanged(void)
{
    return InterlockedExchange(&changed, 0);
}

OnlineState onlineState(void)
{
    return (OnlineState)state;
}

int onlineEnabled(void)
{
    return GetPrivateProfileIntW(L"Loader", L"Online", 0, settingsPath) != 0;
}

static int sha256File(const wchar_t *path, char hex[65])
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32], buffer[65536];
    DWORD read;
    int ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0))
        && BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0));
    while (ok && ReadFile(file, buffer, sizeof buffer, &read, NULL) && read)
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, buffer, read, 0));
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof digest, 0));
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    for (int i = 0; ok && i < 32; i++)
        sprintf(hex + i * 2, "%02x", digest[i]);
    return ok;
}

static void modFile(const char *folder, const wchar_t *suffix, wchar_t *path)
{
    wchar_t name[64];
    wide(folder, name, 64);
    swprintf(path, MAX_PATH, L"%ls\\%ls\\%ls.dll%ls", modsPath, name, name, suffix);
}

static void markerFile(const char *folder, wchar_t *path)
{
    wchar_t name[64];
    wide(folder, name, 64);
    swprintf(path, MAX_PATH, L"%ls\\%ls\\remove", modsPath, name);
}

static int exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

InstallState installState(const char *folder)
{
    wchar_t path[MAX_PATH];
    markerFile(folder, path);
    if (exists(path))
        return INSTALL_REMOVING;
    modFile(folder, L".new", path);
    if (exists(path))
        return INSTALL_PENDING;
    modFile(folder, L"", path);
    if (!exists(path))
        return INSTALL_NONE;
    OnlineMod *online = onlineMod(folder);
    char hex[65];
    if (online && online->sha256[0] && sha256File(path, hex) && _stricmp(hex, online->sha256))
        return INSTALL_OUTDATED;
    return INSTALL_CURRENT;
}

OnlineMod *onlineMod(const char *folder)
{
    for (int i = 0; state == ONLINE_READY && i < onlineCount; i++)
        if (!_stricmp(onlineMods[i].folder, folder))
            return &onlineMods[i];
    return NULL;
}

/** GetPrivateProfileStringW reads UTF-16 only, the index is UTF-8 on GitHub */
static int toUtf16(const wchar_t *path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    DWORD size = GetFileSize(file, NULL), read = 0;
    char *text = size < (1 << 20) ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
    int ok = text && ReadFile(file, text, size, &read, NULL) && read == size;
    CloseHandle(file);
    int length = ok ? MultiByteToWideChar(CP_UTF8, 0, text, (int)size, NULL, 0) : 0;
    wchar_t *converted = length ? HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(wchar_t)) : NULL;
    if (converted) {
        converted[0] = 0xFEFF;
        MultiByteToWideChar(CP_UTF8, 0, text, (int)size, converted + 1, length);
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD written;
        ok = file != INVALID_HANDLE_VALUE && WriteFile(file, converted, (length + 1) * sizeof(wchar_t), &written, NULL);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
    if (text)
        HeapFree(GetProcessHeap(), 0, text);
    if (converted)
        HeapFree(GetProcessHeap(), 0, converted);
    return ok && converted;
}

static void readText(const wchar_t *section, const wchar_t *key, char *out, int size)
{
    wchar_t value[512];
    GetPrivateProfileStringW(section, key, L"", value, 512, indexPath);
    utf8(value, out, size);
}

static int parseIndex(void)
{
    static wchar_t sections[8192];
    DWORD length = GetPrivateProfileSectionNamesW(sections, 8192, indexPath);
    int count = 0;
    for (wchar_t *section = sections; length && *section && count < MAX_ONLINE; section += wcslen(section) + 1) {
        OnlineMod *mod = &onlineMods[count];
        utf8(section, mod->folder, sizeof mod->folder);
        readText(section, L"name", mod->name, sizeof mod->name);
        readText(section, L"de", mod->de, sizeof mod->de);
        readText(section, L"en", mod->en, sizeof mod->en);
        readText(section, L"version", mod->version, sizeof mod->version);
        readText(section, L"author", mod->author, sizeof mod->author);
        readText(section, L"url", mod->url, sizeof mod->url);
        readText(section, L"sha256", mod->sha256, sizeof mod->sha256);
        mod->community = GetPrivateProfileIntW(section, L"community", 1, indexPath) != 0;
        int safeFolder = mod->folder[0] && !strpbrk(mod->folder, "\\/:.");
        if (safeFolder && mod->name[0] && !strncmp(mod->url, URL_PREFIX, strlen(URL_PREFIX)) && strlen(mod->sha256) == 64)
            count++;
    }
    onlineCount = count;
    return count > 0;
}

static DWORD WINAPI fetchIndex(void *unused)
{
    (void)unused;
    wchar_t url[300];
    swprintf(url, 300, L"%ls?t=%llu", INDEX_URL, GetTickCount64());
    int ok = SUCCEEDED(URLDownloadToFileW(NULL, url, indexPath, 0, NULL)) && toUtf16(indexPath) && parseIndex();
    loaderLog(ok ? "Online-Liste geladen: %d Mods" : "Online-Liste nicht erreichbar", onlineCount);
    InterlockedExchange(&state, ok ? ONLINE_READY : ONLINE_FAILED);
    InterlockedExchange(&busy, 0);
    notify();
    return 0;
}

void onlineRefresh(void)
{
    if (!onlineEnabled() || InterlockedCompareExchange(&busy, 1, 0))
        return;
    InterlockedExchange(&state, ONLINE_LOADING);
    HANDLE thread = CreateThread(NULL, 0, fetchIndex, NULL, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else
        InterlockedExchange(&busy, 0);
}

void setOnlineEnabled(int enabled)
{
    WritePrivateProfileStringW(L"Loader", L"Online", enabled ? L"1" : L"0", settingsPath);
    if (!enabled)
        InterlockedExchange(&state, ONLINE_OFF);
    onlineRefresh();
    notify();
}

static DWORD WINAPI download(void *context)
{
    OnlineMod *mod = context;
    wchar_t folder[MAX_PATH], url[300], temporary[MAX_PATH], target[MAX_PATH], name[64];
    char hex[65];
    wide(mod->folder, name, 64);
    swprintf(folder, MAX_PATH, L"%ls\\%ls", modsPath, name);
    CreateDirectoryW(folder, NULL);
    wide(mod->url, url, 300);
    modFile(mod->folder, L".download", temporary);
    modFile(mod->folder, L".new", target);
    int ok = SUCCEEDED(URLDownloadToFileW(NULL, url, temporary, 0, NULL)) && sha256File(temporary, hex)
        && !_stricmp(hex, mod->sha256) && MoveFileExW(temporary, target, MOVEFILE_REPLACE_EXISTING);
    if (!ok)
        DeleteFileW(temporary);
    loaderLog(ok ? "%s geladen, aktiv nach Neustart" : "%s: Download fehlgeschlagen oder Pruefsumme falsch", mod->folder);
    mod->status = ok ? DOWNLOAD_DONE : DOWNLOAD_FAILED;
    InterlockedExchange(&busy, 0);
    notify();
    return 0;
}

void installMod(OnlineMod *mod)
{
    if (InterlockedCompareExchange(&busy, 1, 0))
        return;
    mod->status = DOWNLOAD_RUNNING;
    HANDLE thread = CreateThread(NULL, 0, download, mod, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        mod->status = DOWNLOAD_FAILED;
        InterlockedExchange(&busy, 0);
    }
    notify();
}

void removeMod(const char *folder)
{
    wchar_t path[MAX_PATH];
    markerFile(folder, path);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    modFile(folder, L".new", path);
    DeleteFileW(path);
    loaderLog("%s wird beim naechsten Start entfernt", folder);
    notify();
}

/** Runs before any mod loads: a downloaded .dll.new replaces the old file, a folder marked for removal goes to Mods\.removed */
void applyPending(const wchar_t *dir)
{
    swprintf(modsPath, MAX_PATH, L"%ls", dir);
    swprintf(indexPath, MAX_PATH, L"%ls\\online.ini", dir);
    swprintf(settingsPath, MAX_PATH, L"%ls\\loader.ini", dir);
    wchar_t pattern[MAX_PATH], removed[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    swprintf(removed, MAX_PATH, L"%ls\\.removed", dir);
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || entry.cFileName[0] == L'.')
            continue;
        char folder[64];
        wchar_t path[MAX_PATH], target[MAX_PATH];
        utf8(entry.cFileName, folder, sizeof folder);
        markerFile(folder, path);
        if (exists(path)) {
            DeleteFileW(path);
            CreateDirectoryW(removed, NULL);
            swprintf(path, MAX_PATH, L"%ls\\%ls", dir, entry.cFileName);
            swprintf(target, MAX_PATH, L"%ls\\%ls-%llu", removed, entry.cFileName, GetTickCount64());
            loaderLog(MoveFileW(path, target) ? "%s entfernt" : "%s konnte nicht entfernt werden", folder);
            continue;
        }
        modFile(folder, L".new", path);
        modFile(folder, L"", target);
        if (exists(path))
            loaderLog(MoveFileExW(path, target, MOVEFILE_REPLACE_EXISTING) ? "%s aktualisiert" : "%s: Update fehlgeschlagen", folder);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}
