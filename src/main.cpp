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
static HANDLE g_hThread = NULL;
static bool g_running = true;

static HWND g_hwndGame = NULL;
static HWND g_hwndOSD = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static HBITMAP g_hIndicatorBmp = NULL;
static int g_bmpWidth = 0;
static int g_bmpHeight = 0;

static void LoadRealDwmapi(void)
{
    if (g_realDwmapi) return;
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
    LARGE_INTEGER currentRealQpc;
    fpOriginalQpc(&currentRealQpc);
    ULONGLONG currentRealTick = fpOriginalGetTickCount64 ? fpOriginalGetTickCount64() : GetTickCount64();

    AcquireSRWLockExclusive(&g_timingLock);
    if (!g_timingInitialized)
    {
        InitializeTiming(currentRealQpc, currentRealTick);
    }
    else
    {
        LONGLONG qpcDelta = currentRealQpc.QuadPart - g_lastRealQpc.QuadPart;
        g_baseVirtualQpc.QuadPart += (LONGLONG)(qpcDelta * g_activeSpeed);
        g_lastRealQpc = currentRealQpc;

        ULONGLONG tickDelta = currentRealTick - g_lastRealTick;
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

    ULONGLONG delta = real - g_lastRealTick;
    ULONGLONG scaled = g_baseVirtualTick + (ULONGLONG)(delta * g_activeSpeed);
    ReleaseSRWLockShared(&g_timingLock);
    return scaled;
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

    wchar_t buf[128];
    GetPrivateProfileStringW(L"TurboMode", L"SpeedMultiplier", L"2.5", buf, 128, iniPath);
    double val = _wtof(buf);
    if (val >= 1.0 && val <= 10.0) g_speedMultiplier = val;

    GetPrivateProfileStringW(L"TurboMode", L"GamepadButton", L"RIGHT_TRIGGER", buf, 128, iniPath);
    if (_wcsicmp(buf, L"RIGHT_TRIGGER") == 0) g_gamepadButtonMode = 0;
    else if (_wcsicmp(buf, L"LEFT_TRIGGER") == 0) g_gamepadButtonMode = 1;
    else if (_wcsicmp(buf, L"LEFT_THUMB") == 0) g_gamepadButtonMode = 2;
    else if (_wcsicmp(buf, L"RIGHT_THUMB") == 0) g_gamepadButtonMode = 3;
    else if (_wcsicmp(buf, L"COMBO_LT_RT") == 0) g_gamepadButtonMode = 4;

    GetPrivateProfileStringW(L"TurboMode", L"KeyboardKey", L"0x70", buf, 128, iniPath);
    g_keyboardKey = (int)wcstol(buf, NULL, 0);
    if (g_keyboardKey <= 0) g_keyboardKey = VK_F1;

    g_debounceMs = GetPrivateProfileIntW(L"TurboMode", L"DebounceMs", 250, iniPath);
}

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    if (hwnd == g_hwndOSD) return TRUE;

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

static void InitOSD(void)
{
    GdiplusStartupInput input;
    GdiplusStartup(&g_gdiplusToken, &input, NULL);

    wchar_t assetPath[MAX_PATH];
    GetModuleFileNameW(g_hModule, assetPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(assetPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcscat_s(assetPath, MAX_PATH, L"assets\\indicator.png");

    Bitmap* bmp = Bitmap::FromFile(assetPath);
    if (bmp && bmp->GetLastStatus() == Ok)
    {
        g_bmpWidth = bmp->GetWidth();
        g_bmpHeight = bmp->GetHeight();
        bmp->GetHBITMAP(Color(0, 0, 0, 0), &g_hIndicatorBmp);
        delete bmp;
    }
    else
    {
        if (bmp) delete bmp;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g_hModule;
    wc.lpszClassName = L"FFRSTurboOSD";
    RegisterClassExW(&wc);

    g_hwndOSD = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"FFRSTurboOSD", L"", WS_POPUP,
        0, 0, g_bmpWidth > 0 ? g_bmpWidth : 260, g_bmpHeight > 0 ? g_bmpHeight : 40,
        NULL, NULL, g_hModule, NULL
    );
}

static void UpdateOSDPosition(void)
{
    if (!g_hwndOSD || !g_hIndicatorBmp) return;

    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid == GetCurrentProcessId() && fg != g_hwndOSD)
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
        if (!g_hwndGame) return;
    }

    RECT rcClient;
    GetClientRect(g_hwndGame, &rcClient);
    POINT ptClient = { rcClient.left, rcClient.top };
    ClientToScreen(g_hwndGame, &ptClient);

    int clientW = rcClient.right - rcClient.left;
    int clientH = rcClient.bottom - rcClient.top;
    if (clientW <= 0 || clientH <= 0) return;

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

static void LoadXInput(void)
{
    HMODULE hXInput = LoadLibraryW(L"xinput1_4.dll");
    if (!hXInput) hXInput = LoadLibraryW(L"xinput1_3.dll");
    if (hXInput)
    {
        g_pfnXInputGetState = (PFN_XInputGetState)GetProcAddress(hXInput, "XInputGetState");
    }
}

static bool CheckGamepadTriggered(void)
{
    if (!g_pfnXInputGetState) return false;
    XINPUT_STATE state;
    for (DWORD i = 0; i < 4; ++i)
    {
        if (g_pfnXInputGetState(i, &state) == ERROR_SUCCESS)
        {
            switch (g_gamepadButtonMode)
            {
                case 0:
                    if (state.Gamepad.bRightTrigger > 30) return true;
                    break;
                case 1:
                    if (state.Gamepad.bLeftTrigger > 30) return true;
                    break;
                case 2:
                    if (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) return true;
                    break;
                case 3:
                    if (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) return true;
                    break;
                case 4:
                    if (state.Gamepad.bLeftTrigger > 30 && state.Gamepad.bRightTrigger > 30) return true;
                    break;
                default:
                    if (state.Gamepad.bRightTrigger > 30) return true;
                    break;
            }
        }
    }
    return false;
}

static DWORD WINAPI InputListenerThread(LPVOID lpParam)
{
    LoadXInput();
    InitOSD();

    bool prevGamepad = false;
    bool prevKeyboard = false;
    DWORD lastToggleTick = 0;

    while (g_running)
    {
        bool currGamepad = CheckGamepadTriggered();
        bool currKeyboard = (GetAsyncKeyState(g_keyboardKey) & 0x8000) != 0;
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
                ShowWindow(g_hwndOSD, SW_SHOWNOACTIVATE);
            }
            else
            {
                ShowWindow(g_hwndOSD, SW_HIDE);
            }
        }

        prevGamepad = currGamepad;
        prevKeyboard = currKeyboard;
        Sleep(16);
    }
    return 0;
}

static void InitializeHooks(void)
{
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
    if (g_hIndicatorBmp)
    {
        DeleteObject(g_hIndicatorBmp);
        g_hIndicatorBmp = NULL;
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
