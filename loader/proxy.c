#include <windows.h>

#define VERSION_EXPORTS(X) \
    X(0, GetFileVersionInfoA) X(1, GetFileVersionInfoByHandle) X(2, GetFileVersionInfoExA) \
    X(3, GetFileVersionInfoExW) X(4, GetFileVersionInfoSizeA) X(5, GetFileVersionInfoSizeExA) \
    X(6, GetFileVersionInfoSizeExW) X(7, GetFileVersionInfoSizeW) X(8, GetFileVersionInfoW) \
    X(9, VerFindFileA) X(10, VerFindFileW) X(11, VerInstallFileA) X(12, VerInstallFileW) \
    X(13, VerLanguageNameA) X(14, VerLanguageNameW) X(15, VerQueryValueA) X(16, VerQueryValueW)

#define EXPORT_NAME(i, name) #name,
static const char *exportNames[] = { VERSION_EXPORTS(EXPORT_NAME) };
#define EXPORT_COUNT (sizeof exportNames / sizeof *exportNames)

__attribute__((used)) FARPROC realProcs[EXPORT_COUNT];

#define EXPORT_STUB(i, name) __asm__(".globl " #name "\n" #name ": jmp *realProcs+" #i "*8(%rip)\n");
VERSION_EXPORTS(EXPORT_STUB)

int loadRealVersion(void)
{
    wchar_t path[MAX_PATH];
    UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length + 13 >= MAX_PATH)
        return 0;
    wcscat(path, L"\\version.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real)
        return 0;
    for (size_t i = 0; i < EXPORT_COUNT; i++)
        realProcs[i] = GetProcAddress(real, exportNames[i]);
    return realProcs[8] != NULL;
}
