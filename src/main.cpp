#include "app.h"
#include "core/settings.h"
#include "ui/paint.h"
#include "ui/window.h"

#include <objbase.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    auto setDpiAwareness = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")));
    if (setDpiAwareness)
    {
        setDpiAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    else
    {
        SetProcessDPIAware();
    }

    // WIC on this thread and the shell icon worker both need COM.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    loadSettings();

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"QuickPal.Native.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND existing = FindWindowW(kWindowClass, kWindowTitle))
        {
            PostMessageW(existing, kShowPaletteMessage, 0, 0);
        }
        CloseHandle(mutex);
        CoUninitialize();
        return 0;
    }

    if (!g_app.graphics.createFactories())
    {
        MessageBoxW(nullptr, L"QuickPal could not initialize Direct2D or DirectWrite.",
                    L"QuickPal", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    if (!registerWindowClass(instance))
    {
        CoUninitialize();
        return 1;
    }

    HWND hwnd = createMainWindow(instance);
    if (!hwnd)
    {
        CoUninitialize();
        return 1;
    }

    // Draw one frame while still hidden so the first Alt+Q does not pay for device
    // creation, font loading, or glyph geometry.
    paintFrame();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex)
    {
        CloseHandle(mutex);
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
