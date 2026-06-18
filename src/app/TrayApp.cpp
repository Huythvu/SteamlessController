#include "TrayApp.h"
#include "ControllerManager.h"
#include "resource.h"
#include <shellapi.h>
#include <commctrl.h>
#include <dbt.h>
#include <winreg.h>

static TrayApp* g_app = nullptr;

static constexpr wchar_t WNDCLASS_NAME[] = L"SteamlessControllerWindow";

// Main-window client area. Controls are laid out within this.
static constexpr int WIN_W = 360;
static constexpr int WIN_H = 650;

TrayApp::TrayApp() {
    g_app = this;
}

TrayApp::~TrayApp() {
    RemoveTrayIcon();
    if (m_font) DeleteObject(m_font);
    g_app = nullptr;
}

bool TrayApp::Init(HINSTANCE hInstance) {
    m_hInstance = hInstance;
    m_iconOff   = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_OFF));
    m_iconOn    = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_ON));
    m_wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    // Enable modern visual styles for the standard controls (checkboxes/buttons).
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = WNDCLASS_NAME;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hIcon         = m_iconOff;
    wc.hIconSm       = m_iconOff;
    if (!RegisterClassExW(&wc)) return false;

    // Fixed-size window: caption + close + minimize, no resize or maximize.
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{ 0, 0, WIN_W, WIN_H };
    AdjustWindowRect(&rc, style, FALSE);

    // Created hidden — shown only when the user opens it from the tray.
    m_hwnd = CreateWindowExW(0, WNDCLASS_NAME, L"SteamlessController",
                             style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, hInstance, nullptr);
    if (!m_hwnd) return false;

    // Register for HID device arrival/removal notifications.
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size       = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    // HID device interface GUID
    filter.dbcc_classguid  = {0x4D1E55B2, 0xF16F, 0x11CF,
                              {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
    RegisterDeviceNotificationW(m_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    m_controller = std::make_unique<ControllerManager>(
        [this](bool connected, bool gameModeActive, bool vigemMissing) {
            UpdateTrayIcon(connected, gameModeActive, vigemMissing);
        });

    LoadSettings();
    AddTrayIcon();
    return true;
}

int TrayApp::Run() {
    MSG msg;
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) return -1;
        // Let standard keyboard navigation (Tab / Space) work in the window.
        if (!IsDialogMessageW(m_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) return g_app->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == m_wmTaskbar) {
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        CreateControls(hwnd);
        return 0;

    case WM_TRAY:
        if (LOWORD(lp) == NIN_BALLOONUSERCLICK)
            ShellExecuteW(nullptr, L"open", L"https://github.com/nefarius/ViGEmBus/releases/latest",
                          nullptr, nullptr, SW_SHOWNORMAL);
        else if (LOWORD(lp) == WM_LBUTTONUP)
            ShowMainWindow();
        else if (LOWORD(lp) == WM_RBUTTONUP)
            ShowContextMenu();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:
            ShowMainWindow();
            break;
        case IDC_TOGGLE:
            if (m_controller->IsGameModeActive())
                m_controller->DisableGameMode();
            else
                m_controller->EnableGameMode();
            RefreshControls();
            break;
        case IDC_TRACKPAD:
            m_controller->SetTrackpadMouseEnabled(IsDlgButtonChecked(hwnd, IDC_TRACKPAD) == BST_CHECKED);
            SaveSettings();
            break;
        case IDC_SCROLL:
            m_controller->SetScrollWheelEnabled(IsDlgButtonChecked(hwnd, IDC_SCROLL) == BST_CHECKED);
            SaveSettings();
            break;
        case IDC_INVERT:
            m_controller->SetInvertScroll(IsDlgButtonChecked(hwnd, IDC_INVERT) == BST_CHECKED);
            SaveSettings();
            break;
        case IDC_BACKBUTTONS:
            m_controller->SetBackButtonsEnabled(IsDlgButtonChecked(hwnd, IDC_BACKBUTTONS) == BST_CHECKED);
            SaveSettings();
            break;
        case IDC_LEFT_TRACKPAD:
            m_controller->SetUseLeftTrackpad(IsDlgButtonChecked(hwnd, IDC_LEFT_TRACKPAD) == BST_CHECKED);
            SaveSettings();
            break;
        case IDC_STARTUP:
            SetStartupEnabled(IsDlgButtonChecked(hwnd, IDC_STARTUP) == BST_CHECKED);
            break;
        case IDM_EXIT:
            m_controller->DisableGameMode();
            PostQuitMessage(0);
            break;
        }
        return 0;

    case WM_HSCROLL: {
        HWND bar = reinterpret_cast<HWND>(lp);
        int  pos = static_cast<int>(SendMessageW(bar, TBM_GETPOS, 0, 0));
        UINT valId = 0;
        if (bar == GetDlgItem(hwnd, IDC_SENS)) {
            m_controller->SetTrackpadSensitivity(pos);   valId = IDC_SENS_VAL;
        } else if (bar == GetDlgItem(hwnd, IDC_SCROLL_SENS)) {
            m_controller->SetScrollSensitivity(pos);     valId = IDC_SCROLL_VAL;
        } else if (bar == GetDlgItem(hwnd, IDC_LDEADZONE)) {
            m_controller->SetLeftDeadzone(pos);          valId = IDC_LDEADZONE_VAL;
        } else if (bar == GetDlgItem(hwnd, IDC_RDEADZONE)) {
            m_controller->SetRightDeadzone(pos);         valId = IDC_RDEADZONE_VAL;
        } else if (bar == GetDlgItem(hwnd, IDC_LSTICK)) {
            m_controller->SetLeftStickSensitivity(pos);  valId = IDC_LSTICK_VAL;
        } else if (bar == GetDlgItem(hwnd, IDC_RSTICK)) {
            m_controller->SetRightStickSensitivity(pos); valId = IDC_RSTICK_VAL;
        } else {
            return 0;
        }
        SetDlgItemInt(hwnd, valId, static_cast<UINT>(pos), FALSE);
        SaveSettings();
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        // Make the status label's background blend with the window.
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_CLOSE:
        // Closing the window just hides it — the app keeps running in the tray.
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)
            m_controller->OnDeviceChange();
        return TRUE;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Main window controls
// ---------------------------------------------------------------------------

void TrayApp::CreateControls(HWND hwnd) {
    // Use the standard UI font instead of the ugly default system font.
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    m_font = CreateFontIndirectW(&ncm.lfMessageFont);

    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                    int x, int y, int w, int h, UINT id) -> HWND {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 x, y, w, h, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                                 m_hInstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        return c;
    };

    const int M  = 20;            // margin
    const int W  = WIN_W - 2*M;   // content width
    const int VW = 36;            // value-label width (right-aligned number)

    // Helper: a labelled slider with a live numeric readout to its right.
    auto slider = [&](const wchar_t* label, int y, UINT id, UINT valId, int mn, int mx) {
        make(L"STATIC", label, SS_LEFT,  M,           y, W - VW, 18, 0);
        make(L"STATIC", L"",   SS_RIGHT, M + W - VW,   y, VW,     18, valId);
        HWND b = make(TRACKBAR_CLASSW, L"", TBS_HORZ | WS_TABSTOP, M, y + 20, W, 28, id);
        SendMessageW(b, TBM_SETRANGE, TRUE, MAKELONG(mn, mx));
        SendMessageW(b, TBM_SETPAGESIZE, 0, 10);
    };

    make(L"STATIC", L"", SS_LEFT,                       M,  15, W, 20, IDC_STATUS);
    make(L"BUTTON", L"Enable Steamless Mode", BS_PUSHBUTTON | WS_TABSTOP,
                                                        M,  45, W, 34, IDC_TOGGLE);

    make(L"STATIC", L"Mouse && Trackpad", SS_LEFT,     M,  86, W, 18, 0);
    make(L"BUTTON", L"Trackpad Mouse",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 M, 108, W, 22, IDC_TRACKPAD);
    make(L"BUTTON", L"Left Trackpad Scroll Wheel",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 M, 132, W, 22, IDC_SCROLL);
    make(L"BUTTON", L"Invert Scroll Direction",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 M, 156, W, 22, IDC_INVERT);
    make(L"BUTTON", L"Back Buttons for Clicking",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 M, 180, W, 22, IDC_BACKBUTTONS);
    make(L"BUTTON", L"Use Left Trackpad Instead",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 M, 204, W, 22, IDC_LEFT_TRACKPAD);

    slider(L"Mouse sensitivity",  236, IDC_SENS,        IDC_SENS_VAL,   1, 100);
    slider(L"Scroll sensitivity", 290, IDC_SCROLL_SENS, IDC_SCROLL_VAL, 1, 100);

    make(L"STATIC", L"Sticks", SS_LEFT,                M, 344, W, 18, 0);
    slider(L"Left stick deadzone (%)",   366, IDC_LDEADZONE, IDC_LDEADZONE_VAL, 0, 90);
    slider(L"Right stick deadzone (%)",  420, IDC_RDEADZONE, IDC_RDEADZONE_VAL, 0, 90);
    slider(L"Left stick sensitivity",    474, IDC_LSTICK,    IDC_LSTICK_VAL,    1, 100);
    slider(L"Right stick sensitivity",   528, IDC_RSTICK,    IDC_RSTICK_VAL,    1, 100);

    make(L"STATIC", L"General", SS_LEFT,               M, 582, W, 18, 0);
    make(L"BUTTON", L"Start with Windows",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 M, 604, W, 22, IDC_STARTUP);
}

void TrayApp::RefreshControls() {
    if (!m_controller) return;

    bool connected = m_controller->IsConnected();
    bool gameModeOn = m_controller->IsGameModeActive();

    SetDlgItemTextW(m_hwnd, IDC_TOGGLE,
                    gameModeOn ? L"Disable Steamless Mode" : L"Enable Steamless Mode");
    EnableWindow(GetDlgItem(m_hwnd, IDC_TOGGLE), connected);

    const wchar_t* status = gameModeOn ? L"Steamless Mode: ON"
                          : connected  ? L"Controller connected (Steamless Mode off)"
                                       : L"No controller found";
    SetDlgItemTextW(m_hwnd, IDC_STATUS, status);

    CheckDlgButton(m_hwnd, IDC_TRACKPAD,
                   m_controller->IsTrackpadMouseEnabled() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_SCROLL,
                   m_controller->IsScrollWheelEnabled() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_INVERT,
                   m_controller->IsInvertScroll() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_BACKBUTTONS,
                   m_controller->IsBackButtonsEnabled() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_LEFT_TRACKPAD,
                   m_controller->IsUseLeftTrackpad() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_STARTUP,
                   IsStartupEnabled() ? BST_CHECKED : BST_UNCHECKED);

    // Sliders + their numeric readouts.
    auto setSlider = [&](UINT id, UINT valId, int pos) {
        SendMessageW(GetDlgItem(m_hwnd, id), TBM_SETPOS, TRUE, pos);
        SetDlgItemInt(m_hwnd, valId, static_cast<UINT>(pos), FALSE);
    };
    setSlider(IDC_SENS,        IDC_SENS_VAL,      m_controller->GetTrackpadSensitivity());
    setSlider(IDC_SCROLL_SENS, IDC_SCROLL_VAL,    m_controller->GetScrollSensitivity());
    setSlider(IDC_LDEADZONE,   IDC_LDEADZONE_VAL, m_controller->GetLeftDeadzone());
    setSlider(IDC_RDEADZONE,   IDC_RDEADZONE_VAL, m_controller->GetRightDeadzone());
    setSlider(IDC_LSTICK,      IDC_LSTICK_VAL,    m_controller->GetLeftStickSensitivity());
    setSlider(IDC_RSTICK,      IDC_RSTICK_VAL,    m_controller->GetRightStickSensitivity());
}

void TrayApp::ShowMainWindow() {
    RefreshControls();
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------

void TrayApp::AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_hwnd;
    nid.uID              = TRAY_UID;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon            = m_iconOff;
    wcscpy_s(nid.szTip, L"Steamless Controller");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void TrayApp::RemoveTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void TrayApp::UpdateTrayIcon(bool connected, bool gameModeActive, bool vigemMissing) {
    if (vigemMissing) { ShowViGEmBalloon(); return; }
    bool gameModeOn = gameModeActive;

    const wchar_t* tip = gameModeOn  ? L"Steamless Controller — Steamless Mode ON"
                       : connected   ? L"Steamless Controller — Connected (Steamless Mode OFF)"
                                     : L"Steamless Controller — No controller found";

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = TRAY_UID;
    nid.uFlags = NIF_TIP | NIF_ICON;
    nid.hIcon  = gameModeOn ? m_iconOn : m_iconOff;
    wcscpy_s(nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);

    // Keep the window in sync if it happens to be open.
    if (IsWindowVisible(m_hwnd))
        RefreshControls();
}

void TrayApp::ShowViGEmBalloon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_hwnd;
    nid.uID              = TRAY_UID;
    nid.uFlags           = NIF_INFO;
    nid.dwInfoFlags      = NIIF_WARNING;
    wcscpy_s(nid.szInfoTitle, L"Driver required");
    wcscpy_s(nid.szInfo,      L"ViGEmBus is not installed. Click here to download it.");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------------------
// Settings / startup (registry)
// ---------------------------------------------------------------------------

static constexpr wchar_t REG_KEY[]     = L"Software\\SteamlessController";
static constexpr wchar_t REG_RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr wchar_t APP_NAME[]    = L"SteamlessController";

bool TrayApp::IsStartupEnabled() const {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    bool exists = RegQueryValueExW(key, APP_NAME, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

void TrayApp::SetStartupEnabled(bool enabled) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_WRITE, &key) != ERROR_SUCCESS)
        return;

    if (enabled) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        RegSetValueExW(key, APP_NAME, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(path),
                       static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, APP_NAME);
    }

    RegCloseKey(key);
}

void TrayApp::LoadSettings() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    auto readBool = [&](const wchar_t* name, bool def) -> bool {
        DWORD val = 0, size = sizeof(val);
        if (RegQueryValueExW(key, name, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS)
            return val != 0;
        return def;
    };

    auto readDword = [&](const wchar_t* name, DWORD def) -> DWORD {
        DWORD val = 0, size = sizeof(val);
        if (RegQueryValueExW(key, name, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS)
            return val;
        return def;
    };

    m_controller->SetTrackpadMouseEnabled(readBool(L"TrackpadMouse",   false));
    m_controller->SetScrollWheelEnabled  (readBool(L"ScrollWheel",     false));
    m_controller->SetInvertScroll        (readBool(L"InvertScroll",    false));
    m_controller->SetBackButtonsEnabled  (readBool(L"BackButtons",     false));
    m_controller->SetUseLeftTrackpad     (readBool(L"UseLeftTrackpad", false));
    m_controller->SetTrackpadSensitivity (static_cast<int>(readDword(L"TrackpadSensitivity", 35)));
    m_controller->SetScrollSensitivity   (static_cast<int>(readDword(L"ScrollSensitivity",   30)));
    m_controller->SetLeftDeadzone        (static_cast<int>(readDword(L"LeftDeadzone",         10)));
    m_controller->SetRightDeadzone       (static_cast<int>(readDword(L"RightDeadzone",        10)));
    m_controller->SetLeftStickSensitivity (static_cast<int>(readDword(L"LeftStickSens",       50)));
    m_controller->SetRightStickSensitivity(static_cast<int>(readDword(L"RightStickSens",      50)));

    RegCloseKey(key);
}

void TrayApp::SaveSettings() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return;

    auto writeBool = [&](const wchar_t* name, bool val) {
        DWORD dw = val ? 1 : 0;
        RegSetValueExW(key, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&dw), sizeof(dw));
    };

    writeBool(L"TrackpadMouse",   m_controller->IsTrackpadMouseEnabled());
    writeBool(L"ScrollWheel",     m_controller->IsScrollWheelEnabled());
    writeBool(L"InvertScroll",    m_controller->IsInvertScroll());
    writeBool(L"BackButtons",     m_controller->IsBackButtonsEnabled());
    writeBool(L"UseLeftTrackpad", m_controller->IsUseLeftTrackpad());

    auto writeDword = [&](const wchar_t* name, DWORD val) {
        RegSetValueExW(key, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&val), sizeof(val));
    };
    writeDword(L"TrackpadSensitivity", static_cast<DWORD>(m_controller->GetTrackpadSensitivity()));
    writeDword(L"ScrollSensitivity",   static_cast<DWORD>(m_controller->GetScrollSensitivity()));
    writeDword(L"LeftDeadzone",        static_cast<DWORD>(m_controller->GetLeftDeadzone()));
    writeDword(L"RightDeadzone",       static_cast<DWORD>(m_controller->GetRightDeadzone()));
    writeDword(L"LeftStickSens",       static_cast<DWORD>(m_controller->GetLeftStickSensitivity()));
    writeDword(L"RightStickSens",      static_cast<DWORD>(m_controller->GetRightStickSensitivity()));

    RegCloseKey(key);
}

void TrayApp::ShowContextMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Open");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    // SetForegroundWindow is required for the menu to dismiss on click-away.
    SetForegroundWindow(m_hwnd);

    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
}
