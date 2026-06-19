#include "TrayApp.h"
#include "ControllerManager.h"
#include "InputMapper.h"
#include "resource.h"
#include <shellapi.h>
#include <commctrl.h>
#include <dbt.h>
#include <winreg.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

static TrayApp* g_app = nullptr;

static constexpr wchar_t WNDCLASS_NAME[] = L"SteamlessControllerWindow";
static constexpr wchar_t MON_CLASS_NAME[] = L"SteamlessControllerMonitor";
static constexpr wchar_t MAP_CLASS_NAME[] = L"SteamlessControllerMapping";

// Main-window client area. Controls are laid out within this.
static constexpr int WIN_W = 384;
static constexpr int WIN_H = 470;

// Input-monitor window client area.
static constexpr int MON_W = 506;
static constexpr int MON_H = 610;

// Button-mapping window client area.
static constexpr int MAP_W = 566;
static constexpr int MAP_H = 366;

TrayApp::TrayApp() {
    g_app = this;
}

TrayApp::~TrayApp() {
    RemoveTrayIcon();
    if (m_devNotify) UnregisterDeviceNotification(m_devNotify);
    if (m_font) DeleteObject(m_font);
    g_app = nullptr;
}

bool TrayApp::Init(HINSTANCE hInstance) {
    m_hInstance = hInstance;
    m_iconOff   = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_OFF));
    m_iconOn    = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_ON));
    m_wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    // Enable modern visual styles for the standard controls (checkboxes/buttons).
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES };
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

    WNDCLASSEXW mwc{};
    mwc.cbSize        = sizeof(mwc);
    mwc.lpfnWndProc   = MonitorWndProc;
    mwc.hInstance     = hInstance;
    mwc.lpszClassName = MON_CLASS_NAME;
    mwc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    mwc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    mwc.hIcon         = m_iconOff;
    mwc.hIconSm       = m_iconOff;
    if (!RegisterClassExW(&mwc)) return false;

    WNDCLASSEXW pwc{};
    pwc.cbSize        = sizeof(pwc);
    pwc.lpfnWndProc   = MappingWndProc;
    pwc.hInstance     = hInstance;
    pwc.lpszClassName = MAP_CLASS_NAME;
    pwc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    pwc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    pwc.hIcon         = m_iconOff;
    pwc.hIconSm       = m_iconOff;
    if (!RegisterClassExW(&pwc)) return false;

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
    m_devNotify = RegisterDeviceNotificationW(m_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    m_controller = std::make_unique<ControllerManager>(
        [this](bool connected, bool gameModeActive, bool vigemMissing) {
            m_pendingConnected.store(connected);
            m_pendingGameModeActive.store(gameModeActive);
            m_pendingVigemMissing.store(vigemMissing);
            PostMessageW(m_hwnd, WM_STATE_CHANGED, 0, 0);
        });

    LoadSettings();
    AddTrayIcon();
    SetTimer(m_hwnd, BATT_TIMER, 5000, nullptr);   // periodic battery / tooltip refresh
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

    case WM_STATE_CHANGED:
        UpdateTrayIcon(m_pendingConnected.load(),
                       m_pendingGameModeActive.load(),
                       m_pendingVigemMissing.load());
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:
            ShowMainWindow();
            break;
        case IDC_MONITOR:
            ShowMonitor();
            break;
        case IDC_MAPPING:
            ShowMapping();
            break;
        case IDC_HAPTIC_TEST:
            m_controller->TestHaptic();
            break;
        case IDC_HAPTIC_CLICK:
            m_controller->SetHapticOnClick(IsDlgButtonChecked(hwnd, IDC_HAPTIC_CLICK) == BST_CHECKED);
            SaveSettings();
            break;
        case IDC_HAPTIC_MOVE:
            m_controller->SetHapticOnMove(IsDlgButtonChecked(hwnd, IDC_HAPTIC_MOVE) == BST_CHECKED);
            SaveSettings();
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
        } else if (bar == GetDlgItem(hwnd, IDC_MOUSE_DZ)) {
            m_controller->SetMouseDeadzone(pos);         valId = IDC_MOUSE_DZ_VAL;
        } else if (bar == GetDlgItem(hwnd, IDC_SCROLL_DZ)) {
            m_controller->SetScrollDeadzone(pos);        valId = IDC_SCROLL_DZ_VAL;
        } else if (bar == GetDlgItem(hwnd, IDC_LDEADZONE)) {
            m_controller->SetLeftDeadzone(pos);          valId = IDC_LDEADZONE_VAL;
        } else if (bar == GetDlgItem(hwnd, IDC_RDEADZONE)) {
            m_controller->SetRightDeadzone(pos);         valId = IDC_RDEADZONE_VAL;
        } else if (bar == GetDlgItem(hwnd, IDC_LSTICK)) {
            m_controller->SetLeftStickSensitivity(pos);  valId = IDC_LSTICK_VAL;
        } else if (bar == GetDlgItem(hwnd, IDC_RSTICK)) {
            m_controller->SetRightStickSensitivity(pos); valId = IDC_RSTICK_VAL;
        } else if (bar == GetDlgItem(hwnd, IDC_HAPTIC_INT)) {
            m_controller->SetHapticIntensity(pos);       valId = IDC_HAPTIC_VAL;
        } else {
            return 0;
        }
        SetDlgItemInt(hwnd, valId, static_cast<UINT>(pos), FALSE);
        SaveSettings();
        return 0;
    }

    case WM_NOTIFY: {
        LPNMHDR nm = reinterpret_cast<LPNMHDR>(lp);
        if (nm->hwndFrom == m_tab && nm->code == TCN_SELCHANGE)
            ShowTab(static_cast<int>(SendMessageW(m_tab, TCM_GETCURSEL, 0, 0)));
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

    case WM_TIMER:
        if (wp == BATT_TIMER) {
            UpdateBatteryDisplay();
            UpdateTrayIcon(m_controller->IsConnected(), m_controller->IsGameModeActive(), false);
        }
        return 0;

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

    // Controls created while `cur` points at a page are registered to that tab
    // and start hidden; ShowTab() toggles visibility. Controls created with
    // cur==nullptr (status, the toggle button, the tab control) are always shown.
    std::vector<HWND>* cur = nullptr;
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                    int x, int y, int w, int h, UINT id) -> HWND {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style,
                                 x, y, w, h, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                                 m_hInstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        if (cur) cur->push_back(c);
        return c;
    };

    const int PX = 22, PW = WIN_W - 2*PX, VW = 36;   // page content geometry
    auto slider = [&](const wchar_t* label, int y, UINT id, UINT valId, int mn, int mx) {
        make(L"STATIC", label, SS_LEFT,  PX,            y, PW - VW, 18, 0);
        make(L"STATIC", L"",   SS_RIGHT, PX + PW - VW,  y, VW,      18, valId);
        HWND b = make(TRACKBAR_CLASSW, L"", TBS_HORZ | WS_TABSTOP, PX, y + 20, PW, 26, id);
        SendMessageW(b, TBM_SETRANGE, TRUE, MAKELONG(mn, mx));
        SendMessageW(b, TBM_SETPAGESIZE, 0, 10);
    };

    // Always-visible header: status line + the main toggle.
    make(L"STATIC", L"", SS_LEFT | WS_VISIBLE,         14,  8, WIN_W - 28, 18, IDC_STATUS);
    make(L"BUTTON", L"Enable Steamless Mode", BS_PUSHBUTTON | WS_TABSTOP | WS_VISIBLE,
                                                       14, 30, WIN_W - 28, 32, IDC_TOGGLE);

    // Tab control fills the rest of the window.
    m_tab = make(WC_TABCONTROLW, L"", WS_TABSTOP | WS_VISIBLE,
                 10, 70, WIN_W - 20, WIN_H - 80, IDC_TAB);
    const wchar_t* tabs[4] = { L"General", L"Trackpad", L"Sticks", L"Haptics" };
    for (int i = 0; i < 4; ++i) {
        TCITEMW ti{};
        ti.mask    = TCIF_TEXT;
        ti.pszText = const_cast<LPWSTR>(tabs[i]);
        SendMessageW(m_tab, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&ti));
    }

    // --- General ---
    cur = &m_tabPages[0];
    make(L"STATIC", L"Battery: --", SS_LEFT,           PX, 100, PW, 18, IDC_BATTERY);
    make(L"BUTTON", L"Start with Windows",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 PX, 128, PW, 22, IDC_STARTUP);
    make(L"BUTTON", L"Input Monitor", BS_PUSHBUTTON | WS_TABSTOP,
                                                       PX, 160, PW, 30, IDC_MONITOR);
    make(L"BUTTON", L"Button Mapping", BS_PUSHBUTTON | WS_TABSTOP,
                                                       PX, 196, PW, 30, IDC_MAPPING);

    // --- Trackpad ---
    cur = &m_tabPages[1];
    make(L"BUTTON", L"Trackpad Mouse",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 PX, 100, PW, 22, IDC_TRACKPAD);
    make(L"BUTTON", L"Left Trackpad Scroll Wheel",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 PX, 124, PW, 22, IDC_SCROLL);
    make(L"BUTTON", L"Invert Scroll Direction",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 PX, 148, PW, 22, IDC_INVERT);
    make(L"BUTTON", L"Back Buttons for Clicking",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 PX, 172, PW, 22, IDC_BACKBUTTONS);
    make(L"BUTTON", L"Use Left Trackpad Instead",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 PX, 196, PW, 22, IDC_LEFT_TRACKPAD);
    slider(L"Mouse sensitivity",  224, IDC_SENS,        IDC_SENS_VAL,    1, 100);
    slider(L"Mouse deadzone",     274, IDC_MOUSE_DZ,    IDC_MOUSE_DZ_VAL,  0, 200);
    slider(L"Scroll sensitivity", 324, IDC_SCROLL_SENS, IDC_SCROLL_VAL,  1, 100);
    slider(L"Scroll deadzone",    374, IDC_SCROLL_DZ,   IDC_SCROLL_DZ_VAL, 0, 1000);

    // --- Sticks ---
    cur = &m_tabPages[2];
    slider(L"Left stick deadzone (%)",  104, IDC_LDEADZONE, IDC_LDEADZONE_VAL, 0, 90);
    slider(L"Right stick deadzone (%)", 156, IDC_RDEADZONE, IDC_RDEADZONE_VAL, 0, 90);
    slider(L"Left stick sensitivity",   208, IDC_LSTICK,    IDC_LSTICK_VAL,    1, 100);
    slider(L"Right stick sensitivity",  260, IDC_RSTICK,    IDC_RSTICK_VAL,    1, 100);

    // --- Haptics ---
    cur = &m_tabPages[3];
    make(L"BUTTON", L"Haptic feedback on click",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 PX, 100, PW, 22, IDC_HAPTIC_CLICK);
    make(L"BUTTON", L"Haptic feedback on movement / scroll",
         BS_AUTOCHECKBOX | WS_TABSTOP,                 PX, 124, PW, 22, IDC_HAPTIC_MOVE);
    slider(L"Movement tick density", 154, IDC_HAPTIC_INT, IDC_HAPTIC_VAL, 1, 100);
    make(L"BUTTON", L"Test Haptic", BS_PUSHBUTTON | WS_TABSTOP,
                                                       PX, 210, PW, 30, IDC_HAPTIC_TEST);

    cur = nullptr;
    ShowTab(0);
}

void TrayApp::ShowTab(int index) {
    for (int i = 0; i < 4; ++i) {
        int how = (i == index) ? SW_SHOW : SW_HIDE;
        for (HWND c : m_tabPages[i]) ShowWindow(c, how);
    }
}

void TrayApp::UpdateBatteryDisplay() {
    int  batt     = m_controller ? m_controller->GetBatteryPercent() : -1;
    bool charging = m_controller && m_controller->IsCharging();
    wchar_t text[48];
    if (batt < 0)        wcscpy_s(text, L"Battery: --");
    else if (charging)   swprintf_s(text, L"Battery: %d%% (%s)", batt, batt >= 100 ? L"charged" : L"charging");
    else                 swprintf_s(text, L"Battery: %d%%", batt);
    SetDlgItemTextW(m_hwnd, IDC_BATTERY, text);
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
    CheckDlgButton(m_hwnd, IDC_HAPTIC_CLICK,
                   m_controller->IsHapticOnClick() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_HAPTIC_MOVE,
                   m_controller->IsHapticOnMove() ? BST_CHECKED : BST_UNCHECKED);

    // Sliders + their numeric readouts.
    auto setSlider = [&](UINT id, UINT valId, int pos) {
        SendMessageW(GetDlgItem(m_hwnd, id), TBM_SETPOS, TRUE, pos);
        SetDlgItemInt(m_hwnd, valId, static_cast<UINT>(pos), FALSE);
    };
    setSlider(IDC_SENS,        IDC_SENS_VAL,      m_controller->GetTrackpadSensitivity());
    setSlider(IDC_MOUSE_DZ,    IDC_MOUSE_DZ_VAL,  m_controller->GetMouseDeadzone());
    setSlider(IDC_SCROLL_SENS, IDC_SCROLL_VAL,    m_controller->GetScrollSensitivity());
    setSlider(IDC_SCROLL_DZ,   IDC_SCROLL_DZ_VAL, m_controller->GetScrollDeadzone());
    setSlider(IDC_LDEADZONE,   IDC_LDEADZONE_VAL, m_controller->GetLeftDeadzone());
    setSlider(IDC_RDEADZONE,   IDC_RDEADZONE_VAL, m_controller->GetRightDeadzone());
    setSlider(IDC_LSTICK,      IDC_LSTICK_VAL,    m_controller->GetLeftStickSensitivity());
    setSlider(IDC_RSTICK,      IDC_RSTICK_VAL,    m_controller->GetRightStickSensitivity());
    setSlider(IDC_HAPTIC_INT,  IDC_HAPTIC_VAL,    m_controller->GetHapticIntensity());

    UpdateBatteryDisplay();
}

void TrayApp::ShowMainWindow() {
    RefreshControls();
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
}

// ---------------------------------------------------------------------------
// Live input monitor
// ---------------------------------------------------------------------------

LRESULT CALLBACK TrayApp::MonitorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) return g_app->HandleMonitorMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::HandleMonitorMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == MON_TIMER) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        PaintMonitor(hwnd);
        return 0;
    case WM_CLOSE:
        KillTimer(hwnd, MON_TIMER);
        ShowWindow(hwnd, SW_HIDE);
        if (m_controller) m_controller->SuspendTrackpad(false);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TrayApp::ShowMonitor() {
    if (!m_monitorHwnd) {
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        RECT rc{ 0, 0, MON_W, MON_H };
        AdjustWindowRect(&rc, style, FALSE);
        m_monitorHwnd = CreateWindowExW(0, MON_CLASS_NAME, L"Input Monitor", style,
                                        CW_USEDEFAULT, CW_USEDEFAULT,
                                        rc.right - rc.left, rc.bottom - rc.top,
                                        m_hwnd, nullptr, m_hInstance, nullptr);
        if (!m_monitorHwnd) return;
    }
    ShowWindow(m_monitorHwnd, SW_SHOW);
    SetForegroundWindow(m_monitorHwnd);
    SetTimer(m_monitorHwnd, MON_TIMER, 33, nullptr);   // ~30 Hz refresh
    // Suspend trackpad mouse/scroll so the cursor doesn't fly around while
    // you exercise the controller in front of the monitor.
    if (m_controller) m_controller->SuspendTrackpad(true);
}

void TrayApp::PaintMonitor(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);

    // Double-buffer to avoid flicker on the 30 Hz repaint.
    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(mem, bmp));

    FillRect(mem, &rc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
    SelectObject(mem, m_font);
    SetBkMode(mem, TRANSPARENT);

    uint8_t buf[64];
    size_t  n = m_controller ? m_controller->GetLatestReport(buf, sizeof(buf)) : 0;

    if (n < 30) {
        RECT t = rc;
        DrawTextW(mem, L"Enable Steamless Mode to see live input.", -1, &t,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        HBRUSH onBrush   = CreateSolidBrush(RGB(40, 180, 70));
        HBRUSH offBrush  = CreateSolidBrush(RGB(210, 212, 216));
        HBRUSH dotBrush  = CreateSolidBrush(RGB(40, 120, 220));
        HBRUSH bodyBrush = CreateSolidBrush(RGB(228, 230, 234));
        HPEN   outline   = CreatePen(PS_SOLID, 1, RGB(110, 110, 110));
        HPEN   oldPen    = static_cast<HPEN>(SelectObject(mem, outline));

        auto rd16 = [&](int idx) -> int16_t {
            int16_t v; std::memcpy(&v, buf + idx, 2); return v;
        };
        auto bit = [&](int byteIdx, uint8_t mask) -> bool {
            return (buf[byteIdx] & mask) != 0;
        };
        auto label = [&](int x, int y, int w, const wchar_t* s) {
            RECT lr{ x, y, x + w, y + 16 };
            DrawTextW(mem, s, -1, &lr, DT_CENTER | DT_SINGLELINE);
        };

        // Filled rounded-rect button with a centred caption.
        auto rrect = [&](int x, int y, int w, int h, bool on, const wchar_t* s) {
            HBRUSH ob = static_cast<HBRUSH>(SelectObject(mem, on ? onBrush : offBrush));
            RoundRect(mem, x, y, x + w, y + h, 8, 8);
            SelectObject(mem, ob);
            RECT tr{ x, y, x + w, y + h };
            DrawTextW(mem, s, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };
        // Filled circle button with a centred caption.
        auto circle = [&](int cx, int cy, int r, bool on, const wchar_t* s) {
            HBRUSH ob = static_cast<HBRUSH>(SelectObject(mem, on ? onBrush : offBrush));
            Ellipse(mem, cx - r, cy - r, cx + r, cy + r);
            SelectObject(mem, ob);
            RECT tr{ cx - r, cy - r, cx + r, cy + r };
            DrawTextW(mem, s, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };
        // Round pad (stick or trackpad): ring highlights on click, dot tracks position.
        auto roundPad = [&](int cx, int cy, int r, bool clicked, bool active,
                            int16_t vx, int16_t vy, const wchar_t* s) {
            HBRUSH ob = static_cast<HBRUSH>(SelectObject(mem, clicked ? onBrush : bodyBrush));
            Ellipse(mem, cx - r, cy - r, cx + r, cy + r);
            SelectObject(mem, ob);
            if (active) {
                int half = r - 9;
                int dx = cx + static_cast<int>(vx / 32767.0f * half);
                int dy = cy - static_cast<int>(vy / 32767.0f * half);
                HBRUSH o2 = static_cast<HBRUSH>(SelectObject(mem, dotBrush));
                Ellipse(mem, dx - 9, dy - 9, dx + 9, dy + 9);
                SelectObject(mem, o2);
            }
            label(cx - r, cy + r + 3, 2 * r, s);
        };
        // Rounded-square pad (trackpad): highlights on click, dot tracks touch.
        auto squarePad = [&](int x, int y, int size, bool clicked, bool active,
                             int16_t vx, int16_t vy, const wchar_t* s) {
            HBRUSH ob = static_cast<HBRUSH>(SelectObject(mem, clicked ? onBrush : bodyBrush));
            RoundRect(mem, x, y, x + size, y + size, 18, 18);
            SelectObject(mem, ob);
            if (active) {
                int half = size / 2 - 10;
                int dx = x + size / 2 + static_cast<int>(vx / 32767.0f * half);
                int dy = y + size / 2 - static_cast<int>(vy / 32767.0f * half);
                HBRUSH o2 = static_cast<HBRUSH>(SelectObject(mem, dotBrush));
                Ellipse(mem, dx - 9, dy - 9, dx + 9, dy + 9);
                SelectObject(mem, o2);
            }
            label(x, y + size + 4, size, s);
        };
        // Horizontal trigger bar that fills with travel.
        auto bar = [&](int x, int y, int w, int h, float frac, const wchar_t* s) {
            HBRUSH ob = static_cast<HBRUSH>(SelectObject(mem, offBrush));
            RoundRect(mem, x, y, x + w, y + h, 6, 6);
            int fw = static_cast<int>(frac * w);
            if (fw > 4) {
                SelectObject(mem, onBrush);
                RoundRect(mem, x, y, x + fw, y + h, 6, 6);
            }
            SelectObject(mem, ob);
            RECT tr{ x, y, x + w, y + h };
            DrawTextW(mem, s, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };
        auto trig = [&](int idx) -> float {
            int16_t v = rd16(idx);
            return v <= 0 ? 0.0f : v / 32767.0f;
        };

        // --- Controller body ---
        HBRUSH bodyOld = static_cast<HBRUSH>(SelectObject(mem, bodyBrush));
        RoundRect(mem, 30, 80, 470, 540, 70, 70);
        SelectObject(mem, bodyOld);

        // --- Triggers + bumpers across the top ---
        bar(54, 22, 96, 18, trig(6), L"LT");
        bar(350, 22, 96, 18, trig(8), L"RT");
        rrect(54, 46, 96, 24, bit(4, 0x08), L"LB");
        rrect(350, 46, 96, 24, bit(3, 0x02), L"RB");

        // --- D-pad (left) ---
        rrect(103, 126, 32, 28, bit(3, 0x20), L"Up");
        rrect(103, 182, 32, 28, bit(3, 0x04), L"Dn");
        rrect(71,  154, 32, 28, bit(3, 0x10), L"Lt");
        rrect(135, 154, 32, 28, bit(3, 0x08), L"Rt");

        // --- Face buttons A/B/X/Y diamond (right) ---
        circle(382, 132, 18, bit(2, 0x08), L"Y");
        circle(382, 188, 18, bit(2, 0x01), L"A");
        circle(353, 160, 18, bit(2, 0x04), L"X");
        circle(411, 160, 18, bit(2, 0x02), L"B");

        // --- Center buttons (View / Steam / Menu) ---
        circle(214, 158, 14, bit(3, 0x40), L"V");      // View
        circle(286, 158, 14, bit(2, 0x40), L"M");      // Menu
        circle(250, 160, 18, bit(4, 0x01), L"S");      // Steam
        label(140, 186, 220, L"View      Steam      Menu");

        // --- Thumbsticks (above the trackpads) ---
        roundPad(125, 280, 46, bit(3, 0x80), true, rd16(10), rd16(12), L"Left Stick");
        roundPad(375, 280, 46, bit(2, 0x20), true, rd16(14), rd16(16), L"Right Stick");

        // --- Trackpads (rounded squares, below the sticks) ---
        squarePad(70,  360, 110, bit(5, 0x04), bit(5, 0x02), rd16(18), rd16(20), L"Left Trackpad");
        squarePad(320, 360, 110, bit(4, 0x40), bit(4, 0x20), rd16(24), rd16(26), L"Right Trackpad");

        // --- Back paddles + grips (physically behind the controller) ---
        rrect(16,  560, 74, 26, bit(4, 0x02), L"L4");
        rrect(96,  560, 74, 26, bit(4, 0x04), L"L5");
        rrect(176, 560, 74, 26, bit(5, 0x20), L"L Grip");
        rrect(256, 560, 74, 26, bit(5, 0x10), L"R Grip");
        rrect(336, 560, 74, 26, bit(2, 0x80), L"R4");
        rrect(416, 560, 74, 26, bit(3, 0x01), L"R5");

        SelectObject(mem, oldPen);
        DeleteObject(outline);
        DeleteObject(onBrush);
        DeleteObject(offBrush);
        DeleteObject(dotBrush);
        DeleteObject(bodyBrush);
    }

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Button mapping window
// ---------------------------------------------------------------------------

// Combo option index <-> action. Option 0 = None; 1..NX = Xbox targets;
// NX+1.. = key targets.
static InputMapper::Action MapIndexToAction(int idx) {
    const int nx = InputMapper::kXboxTargetCount;
    if (idx <= 0) return { InputMapper::Type::None, 0 };
    if (idx <= nx) {
        const auto& t = InputMapper::kXboxTargets[idx - 1];
        return { t.type, t.value };
    }
    int k = idx - 1 - nx;
    if (k >= 0 && k < InputMapper::kKeyTargetCount) {
        const auto& t = InputMapper::kKeyTargets[k];
        return { t.type, t.value };
    }
    return { InputMapper::Type::None, 0 };
}

static int MapActionToIndex(InputMapper::Action a) {
    const int nx = InputMapper::kXboxTargetCount;
    if (a.type == InputMapper::Type::Xbox) {
        for (int i = 0; i < nx; ++i)
            if (InputMapper::kXboxTargets[i].value == a.value) return 1 + i;
    } else if (a.type == InputMapper::Type::Key) {
        for (int i = 0; i < InputMapper::kKeyTargetCount; ++i)
            if (InputMapper::kKeyTargets[i].value == a.value) return 1 + nx + i;
    }
    return 0;
}

static void PopulateMapCombo(HWND combo) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(None)"));
    wchar_t buf[48];
    for (int i = 0; i < InputMapper::kXboxTargetCount; ++i) {
        swprintf_s(buf, L"Xbox: %s", InputMapper::kXboxTargets[i].name);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    for (int i = 0; i < InputMapper::kKeyTargetCount; ++i) {
        swprintf_s(buf, L"Key: %s", InputMapper::kKeyTargets[i].name);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
}

LRESULT CALLBACK TrayApp::MappingWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) return g_app->HandleMappingMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::HandleMappingMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateMappingControls(hwnd);
        return 0;

    case WM_COMMAND: {
        UINT id   = LOWORD(wp);
        UINT code = HIWORD(wp);
        if (id == IDC_MAP_RESET && code == BN_CLICKED) {
            m_controller->ResetButtonMappings();
            RefreshMappingControls();
            SaveSettings();
        } else if (code == CBN_SELCHANGE && id >= IDC_MAP_BASE &&
                   id < IDC_MAP_BASE + static_cast<UINT>(InputMapper::kSourceCount)) {
            int i   = static_cast<int>(id - IDC_MAP_BASE);
            int sel = static_cast<int>(SendMessageW(reinterpret_cast<HWND>(lp), CB_GETCURSEL, 0, 0));
            m_controller->SetButtonAction(i, MapIndexToAction(sel));
            SaveSettings();
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TrayApp::ShowMapping() {
    if (!m_mappingHwnd) {
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        RECT rc{ 0, 0, MAP_W, MAP_H };
        AdjustWindowRect(&rc, style, FALSE);
        m_mappingHwnd = CreateWindowExW(0, MAP_CLASS_NAME, L"Button Mapping", style,
                                        CW_USEDEFAULT, CW_USEDEFAULT,
                                        rc.right - rc.left, rc.bottom - rc.top,
                                        m_hwnd, nullptr, m_hInstance, nullptr);
        if (!m_mappingHwnd) return;
    }
    RefreshMappingControls();
    ShowWindow(m_mappingHwnd, SW_SHOW);
    SetForegroundWindow(m_mappingHwnd);
}

void TrayApp::CreateMappingControls(HWND hwnd) {
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                  int x, int y, int w, int h, UINT id) -> HWND {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 x, y, w, h, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                                 m_hInstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        return c;
    };

    const int rows = (InputMapper::kSourceCount + 1) / 2;  // 10 per column
    for (int i = 0; i < InputMapper::kSourceCount; ++i) {
        const bool leftCol = i < rows;
        const int  row = leftCol ? i : i - rows;
        const int  x   = leftCol ? 16 : 296;
        const int  y   = 14 + row * 30;
        mk(L"STATIC", InputMapper::kSources[i].name, SS_LEFT, x, y + 3, 96, 18, 0);
        HWND combo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                        x + 100, y, 150, 240, IDC_MAP_BASE + i);
        PopulateMapCombo(combo);
    }

    mk(L"BUTTON", L"Reset to Defaults", BS_PUSHBUTTON | WS_TABSTOP,
       16, 14 + rows * 30 + 6, 160, 28, IDC_MAP_RESET);
}

void TrayApp::RefreshMappingControls() {
    if (!m_mappingHwnd || !m_controller) return;
    for (int i = 0; i < InputMapper::kSourceCount; ++i) {
        int idx = MapActionToIndex(m_controller->GetButtonAction(i));
        SendMessageW(GetDlgItem(m_mappingHwnd, IDC_MAP_BASE + i), CB_SETCURSEL, idx, 0);
    }
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

    const wchar_t* base = gameModeOn ? L"Steamless Controller - Steamless Mode ON"
                        : connected  ? L"Steamless Controller - Connected (Steamless Mode OFF)"
                                     : L"Steamless Controller - No controller found";

    wchar_t tip[128];
    int  batt     = m_controller ? m_controller->GetBatteryPercent() : -1;
    bool charging = m_controller && m_controller->IsCharging();
    if (batt < 0)      wcscpy_s(tip, base);
    else               swprintf_s(tip, L"%s  -  Battery %d%%%s", base, batt,
                                   charging ? (batt >= 100 ? L" (charged)" : L" (charging)") : L"");

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
        wchar_t quoted[MAX_PATH + 2];
        swprintf_s(quoted, L"\"%s\"", path);
        RegSetValueExW(key, APP_NAME, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted),
                       static_cast<DWORD>((wcslen(quoted) + 1) * sizeof(wchar_t)));
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
    m_controller->SetMouseDeadzone       (static_cast<int>(readDword(L"MouseDeadzone",       120)));
    m_controller->SetScrollDeadzone      (static_cast<int>(readDword(L"ScrollDeadzone",      600)));
    m_controller->SetLeftDeadzone        (static_cast<int>(readDword(L"LeftDeadzone",         10)));
    m_controller->SetRightDeadzone       (static_cast<int>(readDword(L"RightDeadzone",        10)));
    m_controller->SetLeftStickSensitivity (static_cast<int>(readDword(L"LeftStickSens",       50)));
    m_controller->SetRightStickSensitivity(static_cast<int>(readDword(L"RightStickSens",      50)));
    m_controller->SetHapticOnClick        (readBool(L"HapticOnClick", false));
    m_controller->SetHapticOnMove         (readBool(L"HapticOnMove",  false));
    m_controller->SetHapticIntensity      (static_cast<int>(readDword(L"HapticIntensity",     60)));

    // Button mappings: 0xFFFFFFFF sentinel means "not set" -> keep default.
    for (int i = 0; i < InputMapper::kSourceCount; ++i) {
        wchar_t name[16];
        swprintf_s(name, L"MapBtn%d", i);
        DWORD v = readDword(name, 0xFFFFFFFF);
        if (v != 0xFFFFFFFF) {
            InputMapper::Action a;
            a.type  = static_cast<InputMapper::Type>((v >> 16) & 0xFF);
            a.value = static_cast<uint16_t>(v & 0xFFFF);
            m_controller->SetButtonAction(i, a);
        }
    }

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
    writeBool(L"HapticOnClick",   m_controller->IsHapticOnClick());
    writeBool(L"HapticOnMove",    m_controller->IsHapticOnMove());

    auto writeDword = [&](const wchar_t* name, DWORD val) {
        RegSetValueExW(key, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&val), sizeof(val));
    };
    writeDword(L"TrackpadSensitivity", static_cast<DWORD>(m_controller->GetTrackpadSensitivity()));
    writeDword(L"ScrollSensitivity",   static_cast<DWORD>(m_controller->GetScrollSensitivity()));
    writeDword(L"MouseDeadzone",       static_cast<DWORD>(m_controller->GetMouseDeadzone()));
    writeDword(L"ScrollDeadzone",      static_cast<DWORD>(m_controller->GetScrollDeadzone()));
    writeDword(L"LeftDeadzone",        static_cast<DWORD>(m_controller->GetLeftDeadzone()));
    writeDword(L"RightDeadzone",       static_cast<DWORD>(m_controller->GetRightDeadzone()));
    writeDword(L"LeftStickSens",       static_cast<DWORD>(m_controller->GetLeftStickSensitivity()));
    writeDword(L"RightStickSens",      static_cast<DWORD>(m_controller->GetRightStickSensitivity()));
    writeDword(L"HapticIntensity",     static_cast<DWORD>(m_controller->GetHapticIntensity()));

    for (int i = 0; i < InputMapper::kSourceCount; ++i) {
        InputMapper::Action a = m_controller->GetButtonAction(i);
        DWORD v = (static_cast<DWORD>(static_cast<uint8_t>(a.type)) << 16) |
                  static_cast<DWORD>(a.value);
        wchar_t name[16];
        swprintf_s(name, L"MapBtn%d", i);
        writeDword(name, v);
    }

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
