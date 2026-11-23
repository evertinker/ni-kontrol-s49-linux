/* Native VERSION.dll for the Ableton Live process only.
 * Live imports VERSION.dll, and this Wine's AppInit loader is a stub,
 * so DllMain is what gets the NKS socket hook into that process.
 * Every export calls the copy of Wine's own version.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static HMODULE real;

static void note(const char *msg)
{
    FILE *f = fopen("Z:\\tmp\\ni-version-proxy.log", "a");
    if (!f)
        return;
    fputs(msg, f);
    fputc('\n', f);
    fclose(f);
}

static FARPROC need(const char *name)
{
    FARPROC fn = real ? GetProcAddress(real, name) : NULL;
    if (!fn)
        note(name);
    return fn;
}

#define WRAP(ret, name, args, call) \
    ret WINAPI name args { \
        typedef ret (WINAPI *fn_t) args; \
        fn_t fn = (fn_t)need(#name); \
        return fn ? fn call : (ret)0; \
    }

WRAP(DWORD, GetFileVersionInfoSizeA, (LPCSTR a, LPDWORD b), (a, b))
WRAP(DWORD, GetFileVersionInfoSizeW, (LPCWSTR a, LPDWORD b), (a, b))
WRAP(BOOL, GetFileVersionInfoA, (LPCSTR a, DWORD b, DWORD c, LPVOID d), (a, b, c, d))
WRAP(BOOL, GetFileVersionInfoW, (LPCWSTR a, DWORD b, DWORD c, LPVOID d), (a, b, c, d))
WRAP(DWORD, GetFileVersionInfoSizeExA, (DWORD a, LPCSTR b, LPDWORD c), (a, b, c))
WRAP(DWORD, GetFileVersionInfoSizeExW, (DWORD a, LPCWSTR b, LPDWORD c), (a, b, c))
WRAP(BOOL, GetFileVersionInfoExA, (DWORD a, LPCSTR b, DWORD c, DWORD d, LPVOID e), (a, b, c, d, e))
WRAP(BOOL, GetFileVersionInfoExW, (DWORD a, LPCWSTR b, DWORD c, DWORD d, LPVOID e), (a, b, c, d, e))
WRAP(DWORD, VerFindFileA, (DWORD a, LPSTR b, LPSTR c, LPSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h), (a, b, c, d, e, f, g, h))
WRAP(DWORD, VerFindFileW, (DWORD a, LPWSTR b, LPWSTR c, LPWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h), (a, b, c, d, e, f, g, h))
WRAP(DWORD, VerInstallFileA, (DWORD a, LPSTR b, LPSTR c, LPSTR d, LPSTR e, LPSTR f, LPSTR g, PUINT h), (a, b, c, d, e, f, g, h))
WRAP(DWORD, VerInstallFileW, (DWORD a, LPWSTR b, LPWSTR c, LPWSTR d, LPWSTR e, LPWSTR f, LPWSTR g, PUINT h), (a, b, c, d, e, f, g, h))
WRAP(DWORD, VerLanguageNameA, (DWORD a, LPSTR b, DWORD c), (a, b, c))
WRAP(DWORD, VerLanguageNameW, (DWORD a, LPWSTR b, DWORD c), (a, b, c))
WRAP(BOOL, VerQueryValueA, (LPCVOID a, LPCSTR b, LPVOID *c, PUINT d), (a, b, c, d))
WRAP(BOOL, VerQueryValueW, (LPCVOID a, LPCWSTR b, LPVOID *c, PUINT d), (a, b, c, d))

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(hinst);
    if (LoadLibraryW(L"C:\\ni-afunix-hook.dll"))
        note("hook loaded");
    else
        note("hook missing");
    real = LoadLibraryW(L"C:\\ni-version-real.dll");
    note(real ? "real version loaded" : "real version missing");
    return TRUE;
}
