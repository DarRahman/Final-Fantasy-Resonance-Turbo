#include <windows.h>
#include <xinput.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"

using namespace Gdiplus;

typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
typedef BOOL(WINAPI* PFN_QueryPerformanceCounter)(LARGE_INTEGER*);
typedef ULONGLONG(WINAPI* PFN_GetTickCount64)(void);

static HMODULE g_realDwmapi = NULL;
static HINSTANCE g_hModule = NULL;
static PFN_XInputGetState g_pfnXInputGetState = NULL;
static PFN_QueryPerformanceCounter fpOriginalQpc = NULL;
static PFN_GetTickCount64 fpOriginalGetTickCount64 = NULL;

static SRWLOCK g_timingLock = SRWLOCK_INIT;
static double g_speedMultiplier = 2.5;
static double g_activeSpeed = 1.0;
static bool g_turboEnabled = false;

static LARGE_INTEGER g_lastRealQpc = {};
static LARGE_INTEGER g_baseVirtualQpc = {};
static ULONGLONG g_lastRealTick = 0;
static ULONGLONG g_baseVirtualTick = 0;
static bool g_timingInitialized = false;

static int g_gamepadButtonMode = 0;
static int g_keyboardKey = VK_F1;
static DWORD g_debounceMs = 250;
static int g_cycleKeyboardKey = 0x71;
static int g_cycleGamepadMode = -1;
static HANDLE g_hThread = NULL;
static bool g_running = true;

static const double g_speedPresets[] = { 1.5, 2.0, 2.5, 3.0, 3.5, 4.0 };
static const int g_numPresets = 6;
static int g_currentPresetIdx = 2;
static bool g_alwaysShowSpeed = false;
static bool g_enableSpeedCycling = true;
static bool g_isCustomLockedSpeed = false;

static HWND g_hwndGame = NULL;
static HWND g_hwndOSD = NULL;
static HWND g_hwndToast = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static HBITMAP g_hIndicatorBmp = NULL;
static HBITMAP g_hToastBmps[6] = { NULL };
static int g_bmpWidth = 0;
static int g_bmpHeight = 0;
static int g_toastWidth = 108;
static int g_toastHeight = 26;
static DWORD g_toastExpireTick = 0;

static void LoadRealDwmapi(void)
{
    if (g_realDwmapi) return;

    wchar_t dirPath[MAX_PATH];
    if (GetModuleFileNameW(g_hModule, dirPath, MAX_PATH) || GetModuleFileNameW(NULL, dirPath, MAX_PATH))
    {
        wchar_t* lastSlash = wcsrchr(dirPath, L'\\');
        if (lastSlash) *(lastSlash + 1) = L'\0';

        wchar_t chainUe4ss[MAX_PATH];
        wcscpy_s(chainUe4ss, MAX_PATH, dirPath);
        wcscat_s(chainUe4ss, MAX_PATH, L"dwmapi_ue4ss.dll");
        if (GetFileAttributesW(chainUe4ss) != INVALID_FILE_ATTRIBUTES)
        {
            g_realDwmapi = LoadLibraryW(chainUe4ss);
            if (g_realDwmapi) return;
        }

        wchar_t chainGeneric[MAX_PATH];
        wcscpy_s(chainGeneric, MAX_PATH, dirPath);
        wcscat_s(chainGeneric, MAX_PATH, L"dwmapi_chain.dll");
        if (GetFileAttributesW(chainGeneric) != INVALID_FILE_ATTRIBUTES)
        {
            g_realDwmapi = LoadLibraryW(chainGeneric);
            if (g_realDwmapi) return;
        }
    }

    wchar_t sysPath[MAX_PATH];
    if (GetSystemDirectoryW(sysPath, MAX_PATH))
    {
        wcscat_s(sysPath, MAX_PATH, L"\\dwmapi.dll");
        g_realDwmapi = LoadLibraryW(sysPath);
    }
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmIsCompositionEnabled(BOOL* pfEnabled)
{
    LoadRealDwmapi();
    typedef HRESULT (WINAPI* PFN)(BOOL*);
    PFN fn = (PFN)GetProcAddress(g_realDwmapi, "DwmIsCompositionEnabled");
    return fn ? fn(pfEnabled) : E_FAIL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmFlush(void)
{
    LoadRealDwmapi();
    typedef HRESULT (WINAPI* PFN)(void);
    PFN fn = (PFN)GetProcAddress(g_realDwmapi, "DwmFlush");
    return fn ? fn() : E_FAIL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmSetWindowAttribute(HWND hwnd, DWORD dwAttribute, LPCVOID pvAttribute, DWORD cbAttribute)
{
    LoadRealDwmapi();
    typedef HRESULT (WINAPI* PFN)(HWND, DWORD, LPCVOID, DWORD);
    PFN fn = (PFN)GetProcAddress(g_realDwmapi, "DwmSetWindowAttribute");
    return fn ? fn(hwnd, dwAttribute, pvAttribute, cbAttribute) : E_FAIL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmGetCompositionTimingInfo(HWND hwnd, void* pTimingInfo)
{
    LoadRealDwmapi();
    typedef HRESULT (WINAPI* PFN)(HWND, void*);
    PFN fn = (PFN)GetProcAddress(g_realDwmapi, "DwmGetCompositionTimingInfo");
    return fn ? fn(hwnd, pTimingInfo) : E_FAIL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmGetColorizationColor(DWORD* pcrColorization, BOOL* pfOpaqueBlend)
{
    LoadRealDwmapi();
    typedef HRESULT (WINAPI* PFN)(DWORD*, BOOL*);
    PFN fn = (PFN)GetProcAddress(g_realDwmapi, "DwmGetColorizationColor");
    return fn ? fn(pcrColorization, pfOpaqueBlend) : E_FAIL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmExtendFrameIntoClientArea(HWND hWnd, const void* pMarInset)
{
    LoadRealDwmapi();
    typedef HRESULT (WINAPI* PFN)(HWND, const void*);
    PFN fn = (PFN)GetProcAddress(g_realDwmapi, "DwmExtendFrameIntoClientArea");
    return fn ? fn(hWnd, pMarInset) : E_FAIL;
}

static void InitializeTiming(LARGE_INTEGER realQpc, ULONGLONG realTick)
{
    g_lastRealQpc = realQpc;
    g_baseVirtualQpc = realQpc;
    g_lastRealTick = realTick;
    g_baseVirtualTick = realTick;
    g_timingInitialized = true;
}

static void SetSimulationSpeed(double newSpeed)
{
    AcquireSRWLockExclusive(&g_timingLock);
    LARGE_INTEGER currentRealQpc;
    fpOriginalQpc(&currentRealQpc);
    ULONGLONG currentRealTick = fpOriginalGetTickCount64 ? fpOriginalGetTickCount64() : GetTickCount64();

    if (!g_timingInitialized)
    {
        InitializeTiming(currentRealQpc, currentRealTick);
    }
    else
    {
        LONGLONG qpcDelta = currentRealQpc.QuadPart - g_lastRealQpc.QuadPart;
        if (qpcDelta < 0) qpcDelta = 0;
        g_baseVirtualQpc.QuadPart += (LONGLONG)(qpcDelta * g_activeSpeed);
        g_lastRealQpc = currentRealQpc;

        LONGLONG tickDelta = (LONGLONG)currentRealTick - (LONGLONG)g_lastRealTick;
        if (tickDelta < 0) tickDelta = 0;
        g_baseVirtualTick += (ULONGLONG)(tickDelta * g_activeSpeed);
        g_lastRealTick = currentRealTick;
    }
    g_activeSpeed = newSpeed;
    ReleaseSRWLockExclusive(&g_timingLock);
}

static BOOL WINAPI Hooked_QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount)
{
    if (!lpPerformanceCount) return FALSE;

    LARGE_INTEGER real;
    if (!fpOriginalQpc(&real)) return FALSE;

    AcquireSRWLockShared(&g_timingLock);
    if (!g_timingInitialized)
    {
        ReleaseSRWLockShared(&g_timingLock);
        AcquireSRWLockExclusive(&g_timingLock);
        if (!g_timingInitialized)
        {
            InitializeTiming(real, fpOriginalGetTickCount64 ? fpOriginalGetTickCount64() : GetTickCount64());
        }
        ReleaseSRWLockExclusive(&g_timingLock);
        AcquireSRWLockShared(&g_timingLock);
    }

    LONGLONG delta = real.QuadPart - g_lastRealQpc.QuadPart;
    if (delta < 0) delta = 0;
    lpPerformanceCount->QuadPart = g_baseVirtualQpc.QuadPart + (LONGLONG)(delta * g_activeSpeed);
    ReleaseSRWLockShared(&g_timingLock);
    return TRUE;
}

static ULONGLONG WINAPI Hooked_GetTickCount64(void)
{
    ULONGLONG real = fpOriginalGetTickCount64 ? fpOriginalGetTickCount64() : GetTickCount64();

    AcquireSRWLockShared(&g_timingLock);
    if (!g_timingInitialized)
    {
        ReleaseSRWLockShared(&g_timingLock);
        AcquireSRWLockExclusive(&g_timingLock);
        if (!g_timingInitialized)
        {
            LARGE_INTEGER realQpc;
            fpOriginalQpc(&realQpc);
            InitializeTiming(realQpc, real);
        }
        ReleaseSRWLockExclusive(&g_timingLock);
        AcquireSRWLockShared(&g_timingLock);
    }

    LONGLONG delta = (LONGLONG)real - (LONGLONG)g_lastRealTick;
    if (delta < 0) delta = 0;
    ULONGLONG scaled = g_baseVirtualTick + (ULONGLONG)(delta * g_activeSpeed);
    ReleaseSRWLockShared(&g_timingLock);
    return scaled;
}

static int ParseGamepadButton(const wchar_t* str)
{
    if (!str || !*str) return -1;
    if (_wcsicmp(str, L"NONE") == 0 || _wcsicmp(str, L"OFF") == 0) return -1;
    if (_wcsicmp(str, L"RIGHT_TRIGGER") == 0 || _wcsicmp(str, L"RT") == 0) return 0;
    if (_wcsicmp(str, L"LEFT_TRIGGER") == 0 || _wcsicmp(str, L"LT") == 0) return 1;
    if (_wcsicmp(str, L"LEFT_THUMB") == 0 || _wcsicmp(str, L"L3") == 0) return 2;
    if (_wcsicmp(str, L"RIGHT_THUMB") == 0 || _wcsicmp(str, L"R3") == 0) return 3;
    if (_wcsicmp(str, L"COMBO_LT_RT") == 0) return 4;
    if (_wcsicmp(str, L"RIGHT_SHOULDER") == 0 || _wcsicmp(str, L"RB") == 0) return 5;
    if (_wcsicmp(str, L"LEFT_SHOULDER") == 0 || _wcsicmp(str, L"LB") == 0) return 6;
    if (_wcsicmp(str, L"DPAD_UP") == 0) return 7;
    if (_wcsicmp(str, L"DPAD_DOWN") == 0) return 8;
    if (_wcsicmp(str, L"DPAD_LEFT") == 0) return 9;
    if (_wcsicmp(str, L"DPAD_RIGHT") == 0) return 10;
    if (_wcsicmp(str, L"BACK") == 0 || _wcsicmp(str, L"VIEW") == 0) return 11;
    if (_wcsicmp(str, L"START") == 0 || _wcsicmp(str, L"MENU") == 0) return 12;
    if (_wcsicmp(str, L"A") == 0) return 13;
    if (_wcsicmp(str, L"B") == 0) return 14;
    if (_wcsicmp(str, L"X") == 0) return 15;
    if (_wcsicmp(str, L"Y") == 0) return 16;
    return -1;
}

static void LoadConfig(void)
{
    wchar_t iniPath[MAX_PATH];
    HMODULE hMod = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)LoadConfig, &hMod);
    GetModuleFileNameW(hMod, iniPath, MAX_PATH);

    wchar_t* lastSlash = wcsrchr(iniPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcscat_s(iniPath, MAX_PATH, L"turbo_config.ini");

    if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES)
    {
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileNameW(NULL, exePath, MAX_PATH))
        {
            wchar_t* exeSlash = wcsrchr(exePath, L'\\');
            if (exeSlash) *(exeSlash + 1) = L'\0';
            wcscat_s(exePath, MAX_PATH, L"turbo_config.ini");
            if (GetFileAttributesW(exePath) != INVALID_FILE_ATTRIBUTES)
            {
                wcscpy_s(iniPath, MAX_PATH, exePath);
            }
        }
    }

    wchar_t buf[128];
    GetPrivateProfileStringW(L"TurboMode", L"SpeedMultiplier", L"2.5", buf, 128, iniPath);
    double val = _wtof(buf);
    if (val >= 1.0 && val <= 10.0) g_speedMultiplier = val;

    GetPrivateProfileStringW(L"TurboMode", L"GamepadButton", L"RIGHT_TRIGGER", buf, 128, iniPath);
    g_gamepadButtonMode = ParseGamepadButton(buf);

    GetPrivateProfileStringW(L"TurboMode", L"KeyboardKey", L"0x70", buf, 128, iniPath);
    g_keyboardKey = (int)wcstol(buf, NULL, 0);
    if (g_keyboardKey <= 0) g_keyboardKey = VK_F1;

    GetPrivateProfileStringW(L"TurboMode", L"CycleSpeedKey", L"0x71", buf, 128, iniPath);
    g_cycleKeyboardKey = (int)wcstol(buf, NULL, 0);
    if (g_cycleKeyboardKey <= 0) g_cycleKeyboardKey = 0x71;

    GetPrivateProfileStringW(L"TurboMode", L"CycleSpeedGamepad", L"NONE", buf, 128, iniPath);
    g_cycleGamepadMode = ParseGamepadButton(buf);

    g_debounceMs = GetPrivateProfileIntW(L"TurboMode", L"DebounceMs", 250, iniPath);
    g_alwaysShowSpeed = (GetPrivateProfileIntW(L"TurboMode", L"AlwaysShowSpeed", 0, iniPath) != 0);
    g_enableSpeedCycling = (GetPrivateProfileIntW(L"TurboMode", L"EnableSpeedCycling", 1, iniPath) != 0);

    g_isCustomLockedSpeed = true;
    for (int i = 0; i < g_numPresets; ++i)
    {
        if (fabs(g_speedMultiplier - g_speedPresets[i]) < 0.05)
        {
            g_currentPresetIdx = i;
            g_isCustomLockedSpeed = false;
            break;
        }
    }
    if (g_isCustomLockedSpeed)
    {
        g_enableSpeedCycling = false;
    }
}

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    if (hwnd == g_hwndOSD || hwnd == g_hwndToast) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd))
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if ((rc.right - rc.left) > 400 && (rc.bottom - rc.top) > 300)
        {
            *(HWND*)lParam = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static HWND GetGameWindow(void)
{
    HWND hwndFound = NULL;
    EnumWindows(EnumWindowsCallback, (LPARAM)&hwndFound);
    return hwndFound;
}

static HBITMAP LoadAssetBitmap(const wchar_t* relName, int* outW, int* outH)
{
    wchar_t assetPath[MAX_PATH];
    GetModuleFileNameW(g_hModule, assetPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(assetPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcscat_s(assetPath, MAX_PATH, L"assets\\");
    wcscat_s(assetPath, MAX_PATH, relName);

    Bitmap* bmp = Bitmap::FromFile(assetPath);
    if (!bmp || bmp->GetLastStatus() != Ok)
    {
        if (bmp) delete bmp;
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileNameW(NULL, exePath, MAX_PATH))
        {
            wchar_t* exeSlash = wcsrchr(exePath, L'\\');
            if (exeSlash) *(exeSlash + 1) = L'\0';
            wcscat_s(exePath, MAX_PATH, L"assets\\");
            wcscat_s(exePath, MAX_PATH, relName);
            bmp = Bitmap::FromFile(exePath);
        }
    }

    HBITMAP hBmp = NULL;
    if (bmp && bmp->GetLastStatus() == Ok)
    {
        if (outW) *outW = bmp->GetWidth();
        if (outH) *outH = bmp->GetHeight();
        bmp->GetHBITMAP(Color(0, 0, 0, 0), &hBmp);
        delete bmp;
    }
    else if (bmp)
    {
        delete bmp;
    }
    return hBmp;
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void InitOSD(void)
{
    GdiplusStartupInput input;
    GdiplusStartup(&g_gdiplusToken, &input, NULL);

    g_hIndicatorBmp = LoadAssetBitmap(L"indicator.png", &g_bmpWidth, &g_bmpHeight);

    const wchar_t* toastNames[6] = {
        L"toast_speed_1_5x.png",
        L"toast_speed_2_0x.png",
        L"toast_speed_2_5x.png",
        L"toast_speed_3_0x.png",
        L"toast_speed_3_5x.png",
        L"toast_speed_4_0x.png"
    };

    for (int i = 0; i < 6; ++i)
    {
        int tw = 0, th = 0;
        g_hToastBmps[i] = LoadAssetBitmap(toastNames[i], &tw, &th);
        if (tw > 0) g_toastWidth = tw;
        if (th > 0) g_toastHeight = th;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = g_hModule;
    wc.lpszClassName = L"FFRSTurboOSD";
    RegisterClassExW(&wc);

    g_hwndOSD = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"FFRSTurboOSD", L"", WS_POPUP,
        0, 0, g_bmpWidth > 0 ? g_bmpWidth : 260, g_bmpHeight > 0 ? g_bmpHeight : 40,
        NULL, NULL, g_hModule, NULL
    );

    WNDCLASSEXW wcToast = {};
    wcToast.cbSize = sizeof(WNDCLASSEXW);
    wcToast.lpfnWndProc = OverlayWndProc;
    wcToast.hInstance = g_hModule;
    wcToast.lpszClassName = L"FFRSTurboToast";
    RegisterClassExW(&wcToast);

    g_hwndToast = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"FFRSTurboToast", L"", WS_POPUP,
        0, 0, g_toastWidth > 0 ? g_toastWidth : 108, g_toastHeight > 0 ? g_toastHeight : 26,
        NULL, NULL, g_hModule, NULL
    );
}

static void GetGameScreenCoords(POINT* ptClient, int* clientW, int* clientH)
{
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid == GetCurrentProcessId() && fg != g_hwndOSD && fg != g_hwndToast)
    {
        RECT rc;
        GetClientRect(fg, &rc);
        if ((rc.right - rc.left) > 400)
        {
            g_hwndGame = fg;
        }
    }

    if (!g_hwndGame || !IsWindow(g_hwndGame))
    {
        g_hwndGame = GetGameWindow();
    }

    *clientW = 1920;
    *clientH = 1080;
    ptClient->x = 0;
    ptClient->y = 0;

    if (g_hwndGame && IsWindow(g_hwndGame))
    {
        RECT rcClient;
        GetClientRect(g_hwndGame, &rcClient);
        ptClient->x = rcClient.left;
        ptClient->y = rcClient.top;
        ClientToScreen(g_hwndGame, ptClient);

        int w = rcClient.right - rcClient.left;
        int h = rcClient.bottom - rcClient.top;
        if (w > 400 && h > 300)
        {
            *clientW = w;
            *clientH = h;
        }
    }
    else
    {
        *clientW = GetSystemMetrics(SM_CXSCREEN);
        *clientH = GetSystemMetrics(SM_CYSCREEN);
    }
}

static void UpdateOSDPosition(void)
{
    if (!g_hwndOSD || !g_hIndicatorBmp) return;

    POINT ptClient;
    int clientW, clientH;
    GetGameScreenCoords(&ptClient, &clientW, &clientH);

    int posX = ptClient.x + (int)(clientW * 0.708333);
    int posY = ptClient.y + (int)(clientH * 0.035185);

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HGDIOBJ hOldBmp = SelectObject(hdcMem, g_hIndicatorBmp);

    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT ptSrc = { 0, 0 };
    SIZE sizeWnd = { (LONG)g_bmpWidth, (LONG)g_bmpHeight };
    POINT ptDst = { posX, posY };

    UpdateLayeredWindow(g_hwndOSD, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    SelectObject(hdcMem, hOldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    SetWindowPos(g_hwndOSD, HWND_TOPMOST, posX, posY, g_bmpWidth, g_bmpHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void HideOverlayWindow(HWND hwnd)
{
    if (hwnd && IsWindow(hwnd))
    {
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
    }
}

static void HideToast(void)
{
    HideOverlayWindow(g_hwndToast);
    g_toastExpireTick = 0;
}

static void ShowToast(int presetIdx, DWORD currentTick)
{
    if (!g_hwndToast || presetIdx < 0 || presetIdx >= 6 || !g_hToastBmps[presetIdx]) return;

    POINT ptClient;
    int clientW, clientH;
    GetGameScreenCoords(&ptClient, &clientW, &clientH);

    int posX = ptClient.x + (int)(clientW * 0.708333);
    int posY = ptClient.y + (int)(clientH * 0.035185);

    int toastX = posX + (g_bmpWidth - g_toastWidth) / 2;
    int toastY = posY + g_bmpHeight + 4;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HGDIOBJ hOldBmp = SelectObject(hdcMem, g_hToastBmps[presetIdx]);

    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT ptSrc = { 0, 0 };
    SIZE sizeWnd = { (LONG)g_toastWidth, (LONG)g_toastHeight };
    POINT ptDst = { toastX, toastY };

    UpdateLayeredWindow(g_hwndToast, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    SelectObject(hdcMem, hOldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    SetWindowPos(g_hwndToast, HWND_TOPMOST, toastX, toastY, g_toastWidth, g_toastHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    if (g_alwaysShowSpeed)
    {
        g_toastExpireTick = 0;
    }
    else
    {
        g_toastExpireTick = currentTick + 1500;
    }
}

static void LoadXInput(void)
{
    HMODULE hXInput = LoadLibraryW(L"xinput1_4.dll");
    if (!hXInput) hXInput = LoadLibraryW(L"xinput1_3.dll");
    if (hXInput)
    {
        g_pfnXInputGetState = (PFN_XInputGetState)GetProcAddress(hXInput, "XInputGetState");
    }
}

static bool CheckGamepadModeActive(int mode)
{
    if (mode < 0 || !g_pfnXInputGetState) return false;
    XINPUT_STATE state;
    for (DWORD i = 0; i < 4; ++i)
    {
        if (g_pfnXInputGetState(i, &state) == ERROR_SUCCESS)
        {
            switch (mode)
            {
                case 0: if (state.Gamepad.bRightTrigger > 30) return true; break;
                case 1: if (state.Gamepad.bLeftTrigger > 30) return true; break;
                case 2: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) return true; break;
                case 3: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) return true; break;
                case 4: if (state.Gamepad.bLeftTrigger > 30 && state.Gamepad.bRightTrigger > 30) return true; break;
                case 5: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) return true; break;
                case 6: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) return true; break;
                case 7: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) return true; break;
                case 8: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) return true; break;
                case 9: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) return true; break;
                case 10: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) return true; break;
                case 11: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) return true; break;
                case 12: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) return true; break;
                case 13: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) return true; break;
                case 14: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_B) return true; break;
                case 15: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_X) return true; break;
                case 16: if (state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) return true; break;
            }
        }
    }
    return false;
}

static bool CheckGamepadTriggered(void)
{
    return CheckGamepadModeActive(g_gamepadButtonMode);
}

static bool CheckCycleGamepadTriggered(void)
{
    return CheckGamepadModeActive(g_cycleGamepadMode);
}

static DWORD WINAPI InputListenerThread(LPVOID lpParam)
{
    LoadXInput();
    InitOSD();

    bool prevGamepad = false;
    bool prevKeyboard = false;
    bool prevCycleGamepad = false;
    bool prevCycleKeyboard = false;
    DWORD lastToggleTick = 0;
    DWORD lastCycleTick = 0;

    while (g_running)
    {
        bool currGamepad = CheckGamepadTriggered();
        bool currKeyboard = (GetAsyncKeyState(g_keyboardKey) & 0x8000) != 0;
        bool currCycleGamepad = CheckCycleGamepadTriggered();
        bool currCycleKeyboard = (GetAsyncKeyState(g_cycleKeyboardKey) & 0x8000) != 0;
        DWORD currentTick = GetTickCount();

        bool pressed = (currGamepad && !prevGamepad) || (currKeyboard && !prevKeyboard);
        if (pressed && (currentTick - lastToggleTick >= g_debounceMs))
        {
            // Toggle Turbo Mode state on button press
            g_turboEnabled = !g_turboEnabled;
            SetSimulationSpeed(g_turboEnabled ? g_speedMultiplier : 1.0);
            lastToggleTick = currentTick;

            if (g_turboEnabled)
            {
                UpdateOSDPosition();
                if (!g_isCustomLockedSpeed)
                {
                    ShowToast(g_currentPresetIdx, currentTick);
                }
            }
            else
            {
                HideOverlayWindow(g_hwndOSD);
                HideToast();
            }
        }

        bool cyclePressed = (currCycleGamepad && !prevCycleGamepad) || (currCycleKeyboard && !prevCycleKeyboard);
        if (g_enableSpeedCycling && !g_isCustomLockedSpeed && cyclePressed && (currentTick - lastCycleTick >= g_debounceMs))
        {
            g_currentPresetIdx = (g_currentPresetIdx + 1) % g_numPresets;
            g_speedMultiplier = g_speedPresets[g_currentPresetIdx];
            if (g_turboEnabled)
            {
                SetSimulationSpeed(g_speedMultiplier);
                ShowToast(g_currentPresetIdx, currentTick);
            }
            lastCycleTick = currentTick;
        }

        if (!g_alwaysShowSpeed && g_toastExpireTick > 0 && currentTick >= g_toastExpireTick)
        {
            HideToast();
        }

        prevGamepad = currGamepad;
        prevKeyboard = currKeyboard;
        prevCycleGamepad = currCycleGamepad;
        prevCycleKeyboard = currCycleKeyboard;
        Sleep(16);
    }
    return 0;
}

static void InitializeHooks(void)
{
    LoadRealDwmapi();
    LoadConfig();
    if (MH_Initialize() == MH_OK)
    {
        MH_CreateHookApi(L"kernel32.dll", "QueryPerformanceCounter", (LPVOID)&Hooked_QueryPerformanceCounter, (LPVOID*)&fpOriginalQpc);
        MH_CreateHookApi(L"kernel32.dll", "GetTickCount64", (LPVOID)&Hooked_GetTickCount64, (LPVOID*)&fpOriginalGetTickCount64);
        MH_EnableHook(MH_ALL_HOOKS);
    }
    g_hThread = CreateThread(NULL, 0, InputListenerThread, NULL, 0, NULL);
}

static void UninitializeHooks(void)
{
    g_running = false;
    if (g_hThread)
    {
        WaitForSingleObject(g_hThread, 500);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    if (g_hwndOSD)
    {
        DestroyWindow(g_hwndOSD);
        g_hwndOSD = NULL;
    }
    if (g_hwndToast)
    {
        DestroyWindow(g_hwndToast);
        g_hwndToast = NULL;
    }
    if (g_hIndicatorBmp)
    {
        DeleteObject(g_hIndicatorBmp);
        g_hIndicatorBmp = NULL;
    }
    for (int i = 0; i < 6; ++i)
    {
        if (g_hToastBmps[i])
        {
            DeleteObject(g_hToastBmps[i]);
            g_hToastBmps[i] = NULL;
        }
    }
    if (g_gdiplusToken)
    {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    if (g_realDwmapi)
    {
        FreeLibrary(g_realDwmapi);
        g_realDwmapi = NULL;
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved)
{
    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            g_hModule = hinstDLL;
            DisableThreadLibraryCalls(hinstDLL);
            InitializeHooks();
            break;
        case DLL_PROCESS_DETACH:
            if (!lpReserved)
            {
                UninitializeHooks();
            }
            break;
    }
    return TRUE;
}
