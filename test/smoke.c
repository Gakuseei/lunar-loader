#include <windows.h>
#include <stdio.h>

int main(void)
{
    HMODULE proxy = LoadLibraryW(L"version.dll");
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(proxy, path, MAX_PATH);
    DWORD (WINAPI *size)(LPCWSTR, LPDWORD) = (void *)GetProcAddress(proxy, "GetFileVersionInfoSizeW");
    wchar_t kernel32[MAX_PATH];
    GetSystemDirectoryW(kernel32, MAX_PATH);
    wcscat(kernel32, L"\\kernel32.dll");
    DWORD handle = 0, result = size ? size(kernel32, &handle) : 0;
    printf("proxy %ls, forwarded size %lu\n", path, result);
    return proxy && result ? 0 : 1;
}
