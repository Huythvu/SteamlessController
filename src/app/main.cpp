#include "TrayApp.h"
#include <Windows.h>
#include <cwchar>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int) {
    const bool startHidden = pCmdLine && wcsstr(pCmdLine, L"--tray") != nullptr;

    // Single instance: if one is already running, ask it to open its window
    // (so launching the exe again brings the app up) and exit.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"SteamlessController_SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        UINT showMsg = RegisterWindowMessageW(L"SteamlessControllerShowApp");
        PostMessageW(HWND_BROADCAST, showMsg, 0, 0);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    TrayApp app;
    int result = 0;
    if (app.Init(hInstance, startHidden))
        result = app.Run();

    CloseHandle(mutex);
    return result;
}
