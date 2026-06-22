#include "TrayApp.h"
#include "ControllerManager.h"
#include "InputMapper.h"
#include "resource.h"
#include <shellapi.h>
#include <dbt.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#pragma warning(push, 0)
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#pragma warning(pop)

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static TrayApp* g_app = nullptr;
static constexpr wchar_t WNDCLASS_NAME[] = L"SteamlessControllerWindow";
static constexpr wchar_t REG_KEY[]       = L"Software\\SteamlessController";
static constexpr wchar_t REG_PROFILES[]  = L"Software\\SteamlessController\\Profiles";
static constexpr wchar_t REG_RUN_KEY[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr wchar_t APP_NAME[]      = L"SteamlessController";

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16 helpers (ImGui is UTF-8; the registry/Win32 is UTF-16)
// ---------------------------------------------------------------------------
static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring Widen(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

static bool ProfileExists(const std::wstring& name) {
    HKEY k;
    std::wstring path = std::wstring(REG_PROFILES) + L"\\" + name;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegCloseKey(k);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------
TrayApp::TrayApp()  { g_app = this; }
TrayApp::~TrayApp() {
    RemoveTrayIcon();
    if (m_devNotify) UnregisterDeviceNotification(m_devNotify);
    if (m_hwnd) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        DestroyWindow(m_hwnd);
    }
    g_app = nullptr;
}

bool TrayApp::Init(HINSTANCE hInstance) {
    m_hInstance = hInstance;
    m_iconOff   = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_OFF));
    m_iconOn    = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_ON));
    m_wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = WNDCLASS_NAME;
    wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hIcon         = m_iconOff;
    wc.hIconSm       = m_iconOff;
    if (!RegisterClassExW(&wc)) return false;

    // Restore the last window position/size (falls back to a sensible default).
    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT, ww = 1350, wh = 840;
    {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            auto rd = [&](const wchar_t* name, int& out) {
                DWORD v = 0, sz = sizeof(v);
                if (RegQueryValueExW(k, name, nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(&v), &sz) == ERROR_SUCCESS)
                    out = static_cast<int>(v);
            };
            rd(L"WinX", wx); rd(L"WinY", wy); rd(L"WinW", ww); rd(L"WinH", wh);
            RegCloseKey(k);
        }
        if (ww < 600) ww = 1350;
        if (wh < 400) wh = 840;
    }

    m_hwnd = CreateWindowExW(0, WNDCLASS_NAME, L"SteamlessController",
                             WS_OVERLAPPEDWINDOW, wx, wy, ww, wh,
                             nullptr, nullptr, hInstance, nullptr);
    if (!m_hwnd) return false;

    // Dark non-client area (title bar / borders) to match the ImGui theme.
    // Attribute 20 is the documented value (Win10 2004+); older builds used
    // 19, so fall back to that if the first call is rejected.
    BOOL dark = TRUE;
    if (DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &dark, sizeof(dark)) != S_OK)
        DwmSetWindowAttribute(m_hwnd, 19, &dark, sizeof(dark));

    if (!CreateDeviceD3D(m_hwnd)) {
        CleanupDeviceD3D();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // don't litter an imgui.ini
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding  = 4.0f;
    style.GrabRounding   = 4.0f;
    style.WindowPadding  = ImVec2(12, 12);
    style.ItemSpacing    = ImVec2(10, 8);
    style.ScaleAllSizes(1.5f);    // ~50% larger UI
    io.FontGlobalScale = 1.5f;
    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_device, m_ctx);

    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size       = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid  = {0x4D1E55B2, 0xF16F, 0x11CF,
                              {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
    m_devNotify = RegisterDeviceNotificationW(m_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    m_keyboard.Init(hInstance);
    m_controller = std::make_unique<ControllerManager>(
        [this](bool, bool, bool vigemMissing) {
            m_pendingVigemMissing.store(vigemMissing);
            PostMessageW(m_hwnd, WM_STATE_CHANGED, 0, 0);
        },
        [this](bool open) {              // keyboard open/close (from the read thread)
            PostMessageW(m_hwnd, WM_KB_SETOPEN, open ? 1 : 0, 0);
        });

    LoadSettings();
    m_controller->ApplyAutoEnable();
    AddTrayIcon();
    SetTimer(m_hwnd, BATT_TIMER, 5000, nullptr);
    return true;   // starts hidden in the tray
}

// ---------------------------------------------------------------------------
// DX11
// ---------------------------------------------------------------------------
bool TrayApp::CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount       = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags             = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = hWnd;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
        &sd, &m_swap, &m_device, &got, &m_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
            &sd, &m_swap, &m_device, &got, &m_ctx);
    if (hr != S_OK) return false;
    CreateRenderTarget();
    return true;
}

void TrayApp::CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    m_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        m_device->CreateRenderTargetView(back, nullptr, &m_rtv);
        back->Release();
    }
}
void TrayApp::CleanupRenderTarget() { if (m_rtv) { m_rtv->Release(); m_rtv = nullptr; } }
void TrayApp::CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (m_swap)   { m_swap->Release();   m_swap = nullptr; }
    if (m_ctx)    { m_ctx->Release();    m_ctx = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }
}

// ---------------------------------------------------------------------------
// Message loop
// ---------------------------------------------------------------------------
int TrayApp::Run() {
    while (!m_done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) m_done = true;
        }
        if (m_done) break;

        if (!m_visible) {          // idle in the tray with no CPU/GPU cost
            WaitMessage();
            continue;
        }
        RenderFrame();
    }
    return 0;
}

void TrayApp::RenderFrame() {
    if (!m_rtv) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();

    const float clear[4] = { 0.09f, 0.09f, 0.10f, 1.0f };
    m_ctx->OMSetRenderTargets(1, &m_rtv, nullptr);
    m_ctx->ClearRenderTargetView(m_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    m_swap->Present(1, 0);     // vsync caps to the monitor refresh
}

void TrayApp::SaveWindowPlacement() {
    if (!m_hwnd) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(m_hwnd, &wp)) return;
    const RECT& r = wp.rcNormalPosition;   // size when not minimized/maximized
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        auto wd = [&](const wchar_t* name, int v) {
            DWORD dw = static_cast<DWORD>(v);
            RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dw), sizeof(dw));
        };
        wd(L"WinX", r.left);
        wd(L"WinY", r.top);
        wd(L"WinW", r.right - r.left);
        wd(L"WinH", r.bottom - r.top);
        RegCloseKey(k);
    }
}

LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) return g_app->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Remap-by-recording: once a button on the diagram is armed, the next mouse
    // button the user presses becomes its mapping. Handled before ImGui so the
    // click binds instead of activating a widget.
    if (m_recordIndex >= 0) {
        uint16_t mb = 0;
        // Left-click only binds over empty space; over a UI item it re-arms /
        // resets as usual (so you can pick a different button while armed).
        if      (msg == WM_LBUTTONDOWN) { if (!ImGui::IsAnyItemHovered()) mb = InputMapper::MBTN_LEFT; }
        else if (msg == WM_RBUTTONDOWN) mb = InputMapper::MBTN_RIGHT;
        else if (msg == WM_MBUTTONDOWN) mb = InputMapper::MBTN_MIDDLE;
        else if (msg == WM_XBUTTONDOWN)
            mb = (HIWORD(wp) == XBUTTON1) ? InputMapper::MBTN_X1 : InputMapper::MBTN_X2;
        if (mb) {
            int idx = m_recordIndex;
            m_recordIndex = -1;
            m_controller->SetButtonAction(idx, { InputMapper::Type::Mouse, mb });
            SaveSettings();
            return 0;
        }
    }

    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;

    // Remap-by-recording: once a button on the diagram is armed, the next key
    // the user presses becomes its mapping (Esc cancels, Del/Backspace clears).
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && m_recordIndex >= 0) {
        int idx = m_recordIndex;
        m_recordIndex = -1;
        if (wp == VK_ESCAPE) {
            // cancel: leave the mapping untouched
        } else if (wp == VK_DELETE || wp == VK_BACK) {
            m_controller->SetButtonAction(idx, { InputMapper::Type::None, 0 });
            SaveSettings();
        } else {
            m_controller->SetButtonAction(
                idx, { InputMapper::Type::Key, static_cast<uint16_t>(wp) });
            SaveSettings();
        }
        return 0;
    }

    if (msg == m_wmTaskbar) { AddTrayIcon(); return 0; }

    switch (msg) {
    case WM_SIZE:
        if (m_device && wp != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            m_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
            if (m_visible) RenderFrame();   // repaint live while dragging the edge
        }
        return 0;

    case WM_EXITSIZEMOVE:
        SaveWindowPlacement();
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
        if (LOWORD(wp) == IDM_OPEN) ShowMainWindow();
        else if (LOWORD(wp) == IDM_EXIT) {
            SaveWindowPlacement();
            m_controller->DisableGameMode(); m_done = true; PostQuitMessage(0);
        }
        return 0;

    case WM_STATE_CHANGED:
        if (m_pendingVigemMissing.exchange(false)) {
            NOTIFYICONDATAW nid{};
            nid.cbSize      = sizeof(nid);
            nid.hWnd        = m_hwnd;
            nid.uID         = TRAY_UID;
            nid.uFlags      = NIF_INFO;
            nid.dwInfoFlags = NIIF_WARNING;
            wcscpy_s(nid.szInfoTitle, L"Driver required");
            wcscpy_s(nid.szInfo, L"ViGEmBus is not installed. Click here to download it.");
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        UpdateTrayIcon();
        return 0;

    case WM_KB_SETOPEN: {
        const bool open = wp != 0;
        if (open && !m_keyboard.IsVisible()) {
            m_keyboard.SetSplit(m_controller->IsKbSplit());
            m_keyboard.Show();
            m_controller->SetKeyboardMode(true);
            m_kbPrevActive[0] = m_kbPrevActive[1] = false;
            m_kbWasTouch[0] = m_kbWasTouch[1] = false;
            SetTimer(m_hwnd, KB_TIMER, 16, nullptr);   // ~60 Hz poll
        } else if (!open && m_keyboard.IsVisible()) {
            m_keyboard.Hide();
            m_controller->SetKeyboardMode(false);
            KillTimer(m_hwnd, KB_TIMER);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == BATT_TIMER) UpdateTrayIcon();
        else if (wp == KB_TIMER) PollKeyboard();
        return 0;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)
            m_controller->OnDeviceChange();
        return TRUE;

    case WM_CLOSE:
        HideToTray();
        return 0;

    case WM_DESTROY:
        m_done = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
void TrayApp::DrawUI() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    DrawTopBar();
    ImGui::Separator();

    ImGui::BeginChild("sidebar", ImVec2(240, 0), ImGuiChildFlags_Border);
    DrawProfilesSidebar();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_Border);
    DrawTabs();
    ImGui::EndChild();

    ImGui::End();
}

void TrayApp::DrawTopBar() {
    bool connected = m_controller->IsConnected();
    bool game      = m_controller->IsGameModeActive();

    ImGui::TextUnformatted(game ? "Steamless Mode: ON"
                         : connected ? "Controller connected"
                                     : "No controller found");
    ImGui::SameLine(0, 24);

    ImGui::BeginDisabled(!connected);
    if (ImGui::Button(game ? "Disable Steamless Mode" : "Enable Steamless Mode")) {
        if (game) m_controller->DisableGameMode();
        else      m_controller->EnableGameMode();
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0, 12);
    if (ImGui::Button("Refresh"))   // re-scan for a controller turned on just now
        m_controller->OnDeviceChange();

    ImGui::SameLine(0, 24);
    int batt = m_controller->GetBatteryPercent();
    if (batt >= 0) {
        bool charging = m_controller->IsCharging();
        ImGui::Text("Battery: %d%%%s", batt,
                    charging ? (batt >= 100 ? " (charged)" : " (charging)") : "");
    } else {
        ImGui::TextUnformatted("Battery: --");
    }
}

void TrayApp::DrawProfilesSidebar() {
    ImGui::TextUnformatted("PROFILES");
    ImGui::Separator();

    // Reserve room at the bottom for the editing controls so the list scrolls
    // instead of pushing the buttons around. With the buttons anchored, you
    // can spam-click Delete without the target sliding out from under you.
    float footer = ImGui::GetFrameHeightWithSpacing() * 3.0f +
                   ImGui::GetStyle().ItemSpacing.y * 2.0f;
    ImGui::BeginChild("plist", ImVec2(0, -footer), false);

    // Click to switch; drag a row up/down to reorder (persisted immediately).
    auto profiles = ListProfiles();
    for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
        const std::wstring& p = profiles[i];
        bool sel = (p == m_activeProfile);
        if (ImGui::Selectable(Narrow(p).c_str(), sel))
            SwitchProfile(p);

        if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
            int j = i + (ImGui::GetMouseDragDelta(0).y < 0.0f ? -1 : 1);
            if (j >= 0 && j < static_cast<int>(profiles.size())) {
                std::swap(profiles[i], profiles[j]);
                SaveProfileOrder(profiles);
                ImGui::ResetMouseDragDelta();
            }
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##pname", "name", m_profileNameBuf, sizeof(m_profileNameBuf));
    if (ImGui::Button("New")) {
        CreateProfile(Widen(m_profileNameBuf));
        m_profileNameBuf[0] = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rename")) {
        RenameProfile(Widen(m_profileNameBuf));
        m_profileNameBuf[0] = 0;
    }
    if (ImGui::Button("Delete"))
        DeleteProfile();
}

void TrayApp::DrawTabs() {
    if (!ImGui::BeginTabBar("tabs")) return;

    // Small helpers: a toggle / slider that pushes the value into the
    // controller and persists settings only when the user actually changes it.
    auto& c = *m_controller;
    auto toggle = [&](const char* label, bool cur, void (ControllerManager::*set)(bool)) {
        bool v = cur;
        if (ImGui::Checkbox(label, &v)) { (c.*set)(v); SaveSettings(); }
    };
    auto slider = [&](const char* label, int cur, int lo, int hi,
                      void (ControllerManager::*set)(int)) {
        int v = cur;
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt(label, &v, lo, hi)) { (c.*set)(v); SaveSettings(); }
    };

    if (ImGui::BeginTabItem("Trackpad")) {
        ImGui::TextDisabled("MOUSE");
        toggle("Trackpad as mouse", c.IsTrackpadMouseEnabled(),
               &ControllerManager::SetTrackpadMouseEnabled);
        toggle("Use the left trackpad for the mouse", c.IsUseLeftTrackpad(),
               &ControllerManager::SetUseLeftTrackpad);
        ImGui::BeginDisabled(!c.IsTrackpadMouseEnabled());
        slider("Mouse sensitivity", c.GetTrackpadSensitivity(), 1, 100,
               &ControllerManager::SetTrackpadSensitivity);
        slider("Mouse deadzone (0 = off)", c.GetMouseDeadzone(), 0, 100,
               &ControllerManager::SetMouseDeadzone);
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("SCROLL");
        toggle("Scroll wheel (other trackpad)", c.IsScrollWheelEnabled(),
               &ControllerManager::SetScrollWheelEnabled);
        ImGui::BeginDisabled(!c.IsScrollWheelEnabled());
        toggle("Invert scroll direction", c.IsInvertScroll(),
               &ControllerManager::SetInvertScroll);
        // Smart scroll: accumulate movement so slow strokes still scroll, with
        // built-in tap rejection + lift-off filtering (its own internal tuning).
        toggle("Smart scroll (slow strokes, tap rejection)", c.IsSmartScroll(),
               &ControllerManager::SetSmartScroll);
        slider("Scroll sensitivity", c.GetScrollSensitivity(), 1, 100,
               &ControllerManager::SetScrollSensitivity);
        ImGui::BeginDisabled(c.IsSmartScroll());   // deadzone unused in smart mode
        slider("Scroll deadzone (0 = off)", c.GetScrollDeadzone(), 0, 100,
               &ControllerManager::SetScrollDeadzone);
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        // --- live view ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("LIVE VIEW (enable Steamless Mode to see input)");
        uint8_t rep[64];
        size_t n = c.GetLatestReport(rep, sizeof(rep));
        bool useLeft = c.IsUseLeftTrackpad();
        auto rd16  = [&](int idx) -> int16_t {
            int16_t v = 0; if (n >= static_cast<size_t>(idx) + 2) std::memcpy(&v, rep + idx, 2);
            return v;
        };
        auto parse = [&](bool left, bool& t, bool& clk, int16_t& x, int16_t& y) {
            if (n < 30) { t = clk = false; x = y = 0; return; }
            if (left) { t = (rep[5] & 0x02) != 0; clk = (rep[5] & 0x04) != 0; x = rd16(18); y = rd16(20); }
            else      { t = (rep[4] & 0x20) != 0; clk = (rep[4] & 0x40) != 0; x = rd16(24); y = rd16(26); }
        };
        bool lt, lc; int16_t lx, ly; parse(true,  lt, lc, lx, ly);   // physical left pad
        bool rt, rc; int16_t rx, ry; parse(false, rt, rc, rx, ry);   // physical right pad

        // Use the exact per-report movement the deadzone check sees, smoothed a
        // little so the bar isn't jittery. Crossing the red line now means the
        // mouse/scroll is actually producing output.
        m_tpMouseVel  = m_tpMouseVel  * 0.6f + static_cast<float>(c.GetLastMouseMove())  * 0.4f;
        m_tpScrollVel = m_tpScrollVel * 0.6f + static_cast<float>(c.GetLastScrollMove()) * 0.4f;
        int mdz = c.GetMouseDeadzoneRaw();
        int sdz = c.GetScrollDeadzoneRaw();
        // velFrac: the deadzone sits at the half-way (red) mark; >=0.5 => output.
        float mFrac = mdz > 0 ? m_tpMouseVel  / (2.0f * mdz) : (m_tpMouseVel  > 0 ? 1.0f : 0.0f);
        float sFrac = sdz > 0 ? m_tpScrollVel / (2.0f * sdz) : (m_tpScrollVel > 0 ? 1.0f : 0.0f);

        // Lay the pads out as they physically sit: left pad on the left, right
        // pad on the right. Labels show each pad's current role.
        bool leftIsMouse = useLeft;
        DrawTrackpadView(leftIsMouse ? "Left pad (mouse)" : "Left pad (scroll)",
                         lt, lc, lx / 32767.0f, ly / 32767.0f, leftIsMouse ? mFrac : sFrac);
        ImGui::SameLine(0, 24);
        DrawTrackpadView(leftIsMouse ? "Right pad (scroll)" : "Right pad (mouse)",
                         rt, rc, rx / 32767.0f, ry / 32767.0f, leftIsMouse ? sFrac : mFrac);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Sticks")) {
        ImGui::TextDisabled("LEFT STICK");
        slider("Deadzone##l", c.GetLeftDeadzone(), 0, 90,
               &ControllerManager::SetLeftDeadzone);
        slider("Sensitivity##l", c.GetLeftStickSensitivity(), 1, 100,
               &ControllerManager::SetLeftStickSensitivity);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("RIGHT STICK");
        slider("Deadzone##r", c.GetRightDeadzone(), 0, 90,
               &ControllerManager::SetRightDeadzone);
        slider("Sensitivity##r", c.GetRightStickSensitivity(), 1, 100,
               &ControllerManager::SetRightStickSensitivity);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("LIVE (dot = position, red ring = deadzone)");
        uint8_t rep[64];
        size_t n = c.GetLatestReport(rep, sizeof(rep));
        auto axis = [&](int off) -> float {
            if (n < static_cast<size_t>(off) + 2) return 0.0f;
            int16_t v; std::memcpy(&v, rep + off, 2);
            return v / 32767.0f;
        };
        DrawStickView(axis(10), axis(12), c.GetLeftDeadzone()  / 100.0f);
        ImGui::SameLine(0, 24);
        DrawStickView(axis(14), axis(16), c.GetRightDeadzone() / 100.0f);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Controller")) {
        DrawControllerTab();
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Haptics")) {
        ImGui::TextDisabled("Local trackpad feedback (the pad you're using).");
        ImGui::Spacing();
        toggle("Buzz on click", c.IsHapticOnClick(),
               &ControllerManager::SetHapticOnClick);
        ImGui::BeginDisabled(!c.IsHapticOnClick());
        ImGui::TextUnformatted("Click hardness");
        int hard = c.GetHapticClickHardness();
        const char* names[3] = { "Soft", "Medium", "Hard" };
        for (int lvl = 1; lvl <= 3; ++lvl) {
            ImGui::SameLine();
            if (ImGui::RadioButton(names[lvl - 1], hard == lvl)) {
                c.SetHapticClickHardness(lvl);
                SaveSettings();
            }
        }
        ImGui::EndDisabled();

        // Movement texture: identical on both pads, one shared intensity.
        toggle("Buzz on movement (both pads)", c.IsHapticOnMove(),
               &ControllerManager::SetHapticOnMove);
        ImGui::BeginDisabled(!c.IsHapticOnMove());
        slider("Movement intensity", c.GetHapticIntensity(), 1, 100,
               &ControllerManager::SetHapticIntensity);
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::BeginDisabled(!c.IsConnected());
        if (ImGui::Button("Test haptic")) c.TestHaptic();
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Keyboard")) {
        // A combo to pick a source button (or None). Returns via the setter.
        auto sourceCombo = [&](const char* label, int cur, void (ControllerManager::*set)(int)) {
            std::string prev = cur < 0 ? "None" : Narrow(InputMapper::kSources[cur].name);
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo(label, prev.c_str())) {
                if (ImGui::Selectable("None", cur < 0)) { (c.*set)(-1); SaveSettings(); }
                for (int i = 0; i < InputMapper::kSourceCount; ++i)
                    if (ImGui::Selectable(Narrow(InputMapper::kSources[i].name).c_str(), cur == i)) {
                        (c.*set)(i); SaveSettings();
                    }
                ImGui::EndCombo();
            }
        };

        ImGui::TextDisabled("OPEN / CLOSE");
        sourceCombo("Modifier (hold)", c.GetKbOpenModifier(), &ControllerManager::SetKbOpenModifier);
        sourceCombo("Open button",     c.GetKbOpenButton(),   &ControllerManager::SetKbOpenButton);
        ImGui::TextDisabled("Leave the modifier as None for a single-button trigger.");
        bool hold = c.IsKbOpenHold();
        if (ImGui::RadioButton("Toggle on/off", !hold)) { c.SetKbOpenHold(false); SaveSettings(); }
        ImGui::SameLine();
        if (ImGui::RadioButton("Hold to keep open", hold)) { c.SetKbOpenHold(true); SaveSettings(); }

        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextDisabled("TYPING / CLICK");
        bool usePad = c.IsKbUsePadClick();
        if (ImGui::Checkbox("Press the trackpad to type the key", &usePad)) {
            c.SetKbUsePadClick(usePad); SaveSettings();
        }
        sourceCombo("Left click button",  c.GetKbClickLeft(),  &ControllerManager::SetKbClickLeft);
        sourceCombo("Right click button", c.GetKbClickRight(), &ControllerManager::SetKbClickRight);
        ImGui::TextDisabled("Each pad's button types the key that pad is pointing at.");

        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextDisabled("LAYOUT");
        bool split = c.IsKbSplit();
        if (ImGui::Checkbox("Split: each pad controls its half", &split)) {
            c.SetKbSplit(split); SaveSettings();
        }
        bool rel = c.IsKbRelative();
        if (ImGui::Checkbox("Slide to move (relative) instead of touch position", &rel)) {
            c.SetKbRelative(rel); SaveSettings();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("General")) {
        bool autoEnable = c.IsAutoEnable();
        if (ImGui::Checkbox("Auto-enable Steamless Mode", &autoEnable)) {
            c.SetAutoEnable(autoEnable);
            SaveSettings();
        }
        bool startup = IsStartupEnabled();
        if (ImGui::Checkbox("Start with Windows", &startup))
            SetStartupEnabled(startup);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("SteamlessController");
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

// Human-readable label for a mapped action (searches the target tables).
static std::string ActionLabel(InputMapper::Action a) {
    if (a.type == InputMapper::Type::None) return "-";
    if (a.type == InputMapper::Type::Mouse) {
        switch (a.value) {
            case InputMapper::MBTN_LEFT:   return "Mouse Left";
            case InputMapper::MBTN_RIGHT:  return "Mouse Right";
            case InputMapper::MBTN_MIDDLE: return "Mouse Middle";
            case InputMapper::MBTN_X1:     return "Mouse X1";
            case InputMapper::MBTN_X2:     return "Mouse X2";
            default:                       return "Mouse?";
        }
    }
    const InputMapper::Target* t = nullptr; int cnt = 0;
    const char* prefix = "";
    if (a.type == InputMapper::Type::Xbox) {
        t = InputMapper::kXboxTargets; cnt = InputMapper::kXboxTargetCount;
    } else {
        t = InputMapper::kKeyTargets;  cnt = InputMapper::kKeyTargetCount;
        prefix = "Key ";
    }
    for (int i = 0; i < cnt; ++i)
        if (t[i].type == a.type && t[i].value == a.value)
            return std::string(prefix) + Narrow(t[i].name);

    // A recorded key that isn't in the preset table: show it anyway.
    if (a.type == InputMapper::Type::Key) {
        if (a.value >= 0x21 && a.value <= 0x7E)
            return std::string("Key ") + static_cast<char>(a.value);
        char b[16]; std::snprintf(b, sizeof(b), "Key %u", a.value);
        return b;
    }
    return "?";
}

// Controller tab: the same gamepad layout as the original input monitor, drawn
// to scale. Real (remappable) buttons are clickable: click one then press a key
// to bind it (JoyToKey style). A legend on the right lists every mapping.
void TrayApp::DrawControllerTab() {
    uint8_t rep[64];
    size_t n = m_controller->GetLatestReport(rep, sizeof(rep));

    // While a button is armed, a physical gamepad press also binds it -- to the
    // pressed button's DEFAULT action (from the factory layout), not its current
    // mapping. That way re-pressing a remapped button still resolves to what it
    // physically is (e.g. pressing A always means "A"), so A->B is reversible.
    if (m_recordIndex >= 0 && n >= 30) {
        for (int j = 0; j < InputMapper::kSourceCount; ++j) {
            const InputMapper::Source& s = InputMapper::kSources[j];
            // Only standard gamepad buttons are recordable this way (skip the
            // paddles and trackpad clicks, which default to None / Mouse).
            if (s.def.type != InputMapper::Type::Xbox) continue;
            if (n > s.byteIndex && (rep[s.byteIndex] & s.mask) != 0) {
                m_controller->SetButtonAction(m_recordIndex, s.def);
                SaveSettings();
                m_recordIndex = -1;
                break;
            }
        }
    }

    if (ImGui::Button("Reset all mappings")) {
        m_controller->ResetButtonMappings();
        SaveSettings();
        m_recordIndex = -1;
    }
    ImGui::SameLine(0, 16);
    if (m_recordIndex >= 0)
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.20f, 1.0f),
            "Press a key, gamepad button, or mouse button for \"%s\"   (Esc cancel, Del clear)",
            Narrow(InputMapper::kSources[m_recordIndex].name).c_str());
    else
        ImGui::TextDisabled("Click a button then press a key / gamepad / mouse button. Right-click = reset to default.");
    ImGui::Spacing();

    const float S = 1.2f;                 // scale the original pixel layout
    const float baseW = 506.0f, baseH = 600.0f;
    const float canvasW = baseW * S, canvasH = baseH * S;

    ImGui::BeginChild("diagram", ImVec2(canvasW + 6, canvasH + 6), false);
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(canvasW, canvasH));
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 border  = IM_COL32(120, 124, 132, 255);
    ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 dotCol  = IM_COL32(80, 180, 255, 255);
    auto PX = [&](float v) { return o.x + v * S; };
    auto PY = [&](float v) { return o.y + v * S; };
    auto bit  = [&](int byteIdx, uint8_t mask) {
        return n > static_cast<size_t>(byteIdx) && (rep[byteIdx] & mask) != 0;
    };
    auto rd16 = [&](int idx) -> int16_t {
        int16_t v = 0; if (n >= static_cast<size_t>(idx) + 2) std::memcpy(&v, rep + idx, 2);
        return v;
    };
    auto srcPressed = [&](int idx) {
        const auto& s = InputMapper::kSources[idx];
        return bit(s.byteIndex, s.mask);
    };

    // Click target for a remappable source over a base-coords rect. Returns hover.
    auto remapHit = [&](int idx, float x, float y, float w, float h) -> bool {
        ImGui::SetCursorScreenPos(ImVec2(PX(x), PY(y)));
        ImGui::InvisibleButton((std::string("##s") + std::to_string(idx)).c_str(),
                               ImVec2(w * S, h * S),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))  m_recordIndex = idx;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            m_controller->SetButtonAction(idx, InputMapper::kSources[idx].def);
            SaveSettings();
            if (m_recordIndex == idx) m_recordIndex = -1;
        }
        if (hov)
            ImGui::SetTooltip("%s  ->  %s", Narrow(InputMapper::kSources[idx].name).c_str(),
                              ActionLabel(m_controller->GetButtonAction(idx)).c_str());
        return hov;
    };
    auto fillCol = [&](int idx, bool pressed, bool hov) -> ImU32 {
        if (idx >= 0 && m_recordIndex == idx) return IM_COL32(240, 200, 50, 255);
        if (pressed) return IM_COL32(50, 180, 80, 255);
        if (hov)     return IM_COL32(95, 100, 110, 255);
        return IM_COL32(70, 74, 82, 255);
    };
    auto centerText = [&](float l, float t, float r, float b, const char* s) {
        ImVec2 ts = ImGui::CalcTextSize(s);
        dl->AddText(ImVec2((l + r) / 2 - ts.x / 2, (t + b) / 2 - ts.y / 2), textCol, s);
    };

    // --- shapes (base coords) ---
    auto rrect = [&](float x, float y, float w, float h, int idx, bool pressed, const char* s) {
        bool hov = idx >= 0 ? remapHit(idx, x, y, w, h) : false;
        dl->AddRectFilled(ImVec2(PX(x), PY(y)), ImVec2(PX(x + w), PY(y + h)),
                          fillCol(idx, pressed, hov), 6.0f * S);
        dl->AddRect(ImVec2(PX(x), PY(y)), ImVec2(PX(x + w), PY(y + h)), border, 6.0f * S);
        centerText(PX(x), PY(y), PX(x + w), PY(y + h), s);
    };
    auto circle = [&](float cx, float cy, float r, int idx, bool pressed, const char* s) {
        bool hov = idx >= 0 ? remapHit(idx, cx - r, cy - r, 2 * r, 2 * r) : false;
        ImVec2 c(PX(cx), PY(cy));
        dl->AddCircleFilled(c, r * S, fillCol(idx, pressed, hov), 32);
        dl->AddCircle(c, r * S, border, 32);
        centerText(PX(cx - r), PY(cy - r), PX(cx + r), PY(cy + r), s);
    };
    auto roundPad = [&](float cx, float cy, float r, int idx, bool clicked,
                        int16_t vx, int16_t vy, const char* s) {
        bool hov = idx >= 0 ? remapHit(idx, cx - r, cy - r, 2 * r, 2 * r) : false;
        ImVec2 c(PX(cx), PY(cy));
        dl->AddCircleFilled(c, r * S, fillCol(idx, clicked, hov), 40);
        dl->AddCircle(c, r * S, border, 40);
        float half = (r - 9) * S;
        dl->AddCircleFilled(ImVec2(c.x + vx / 32767.0f * half, c.y - vy / 32767.0f * half),
                            7.0f, dotCol);
        ImVec2 ts = ImGui::CalcTextSize(s);
        dl->AddText(ImVec2(c.x - ts.x / 2, PY(cy + r) + 3), textCol, s);
    };
    auto squarePad = [&](float x, float y, float size, int idx, bool active,
                         int16_t vx, int16_t vy, const char* s) {
        bool clicked = srcPressed(idx);
        bool hov = remapHit(idx, x, y, size, size);
        ImVec2 a(PX(x), PY(y)), b(PX(x + size), PY(y + size));
        dl->AddRectFilled(a, b, fillCol(idx, clicked, hov), 14.0f * S);
        dl->AddRect(a, b, border, 14.0f * S);
        if (active) {
            float half = (size / 2 - 10) * S;
            ImVec2 c((a.x + b.x) / 2, (a.y + b.y) / 2);
            dl->AddCircleFilled(ImVec2(c.x + vx / 32767.0f * half, c.y - vy / 32767.0f * half),
                                7.0f, dotCol);
        }
        ImVec2 ts = ImGui::CalcTextSize(s);
        dl->AddText(ImVec2((a.x + b.x) / 2 - ts.x / 2, b.y + 3), textCol, s);
    };
    auto bar = [&](float x, float y, float w, float h, float frac, const char* s) {
        ImVec2 a(PX(x), PY(y)), b(PX(x + w), PY(y + h));
        dl->AddRectFilled(a, b, IM_COL32(55, 58, 65, 255), 5.0f * S);
        if (frac > 0.02f)
            dl->AddRectFilled(a, ImVec2(PX(x + w * frac), PY(y + h)),
                              IM_COL32(50, 180, 80, 255), 5.0f * S);
        dl->AddRect(a, b, border, 5.0f * S);
        centerText(a.x, a.y, b.x, b.y, s);
    };
    auto trig = [&](int idx) -> float {
        int16_t v = rd16(idx);
        return v <= 0 ? 0.0f : v / 32767.0f;
    };

    // --- controller body ---
    dl->AddRectFilled(ImVec2(PX(30), PY(80)), ImVec2(PX(470), PY(540)),
                      IM_COL32(48, 50, 56, 255), 60.0f * S);
    dl->AddRect(ImVec2(PX(30), PY(80)), ImVec2(PX(470), PY(540)), border, 60.0f * S);

    // triggers + bumpers
    bar(54, 22, 96, 18, trig(6), "LT");
    bar(350, 22, 96, 18, trig(8), "RT");
    rrect(54, 46, 96, 24, 4, srcPressed(4), "LB");
    rrect(350, 46, 96, 24, 5, srcPressed(5), "RB");
    // d-pad
    rrect(103, 126, 32, 28, 11, srcPressed(11), "Up");
    rrect(103, 182, 32, 28, 12, srcPressed(12), "Dn");
    rrect(71,  154, 32, 28, 13, srcPressed(13), "Lt");
    rrect(135, 154, 32, 28, 14, srcPressed(14), "Rt");
    // face buttons
    circle(382, 132, 18, 3, srcPressed(3), "Y");
    circle(382, 188, 18, 0, srcPressed(0), "A");
    circle(353, 160, 18, 2, srcPressed(2), "X");
    circle(411, 160, 18, 1, srcPressed(1), "B");
    // center buttons
    circle(214, 158, 14, 9,  srcPressed(9),  "V");
    circle(286, 158, 14, 8,  srcPressed(8),  "M");
    circle(250, 160, 18, 10, srcPressed(10), "S");
    // sticks (clickable = stick-click)
    roundPad(125, 280, 46, 6, srcPressed(6), rd16(10), rd16(12), "Left Stick");
    roundPad(375, 280, 46, 7, srcPressed(7), rd16(14), rd16(16), "Right Stick");
    // trackpads (click is remappable: left pad = src 20, right pad = src 19)
    squarePad(70,  360, 110, 20, bit(5, 0x02), rd16(18), rd16(20), "Left Pad");
    squarePad(320, 360, 110, 19, bit(4, 0x20), rd16(24), rd16(26), "Right Pad");
    // paddles + grips
    rrect(16,  560, 74, 26, 15, srcPressed(15), "L4");
    rrect(96,  560, 74, 26, 16, srcPressed(16), "L5");
    rrect(176, 560, 74, 26, -1, bit(5, 0x20), "L Grip");
    rrect(256, 560, 74, 26, -1, bit(5, 0x10), "R Grip");
    rrect(336, 560, 74, 26, 17, srcPressed(17), "R4");
    rrect(416, 560, 74, 26, 18, srcPressed(18), "R5");

    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + canvasH));
    ImGui::EndChild();

    // --- legend: every mapping, clear and clickable ---
    ImGui::SameLine();
    ImGui::BeginChild("legend", ImVec2(0, canvasH + 6), true);
    ImGui::TextDisabled("BUTTON  ->  MAPPING");
    ImGui::Separator();
    for (int i = 0; i < InputMapper::kSourceCount; ++i) {
        bool pressed = srcPressed(i);
        bool rec     = (m_recordIndex == i);
        char row[96];
        std::snprintf(row, sizeof(row), "%-13s  %s##leg%d",
                      Narrow(InputMapper::kSources[i].name).c_str(),
                      ActionLabel(m_controller->GetButtonAction(i)).c_str(), i);
        if (pressed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.85f, 0.45f, 1.0f));
        if (ImGui::Selectable(row, rec)) m_recordIndex = i;
        if (pressed) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            m_controller->SetButtonAction(i, InputMapper::kSources[i].def);
            SaveSettings();
            if (rec) m_recordIndex = -1;
        }
    }
    ImGui::EndChild();
}

// A square stick view: outer bounds, the circular deadzone ring, and a dot
// at the current normalized position (nx,ny in -1..1, +y = up).
void TrayApp::DrawStickView(float nx, float ny, float dz) {
    const float sz = 160.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImVec2(p.x + sz / 2, p.y + sz / 2);
    float r = sz / 2 - 2;

    ImU32 box    = ImGui::GetColorU32(ImGuiCol_FrameBg);
    ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
    dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), box, 6.0f);
    dl->AddRect(p, ImVec2(p.x + sz, p.y + sz), border, 6.0f);
    dl->AddCircle(c, r, border, 48);
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), border);
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), border);
    if (dz > 0.0f)
        dl->AddCircle(c, r * dz, IM_COL32(210, 80, 80, 200), 48);

    float px = nx < -1 ? -1 : (nx > 1 ? 1 : nx);
    float py = ny < -1 ? -1 : (ny > 1 ? 1 : ny);
    ImVec2 dot = ImVec2(c.x + px * r, c.y - py * r);
    dl->AddCircleFilled(dot, 6.0f, IM_COL32(80, 180, 255, 255));

    ImGui::Dummy(ImVec2(sz, sz));
}

// A live trackpad view: square pad with the touch dot, touch/click state, and
// a movement bar with the deadzone marked at the half-way line. When you move
// fast enough to cross the red marker, the bar goes green = output is sent.
void TrayApp::DrawTrackpadView(const char* label, bool touch, bool click,
                               float nx, float ny, float velFrac) {
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label);

    const float sz = 150.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = p, b = ImVec2(p.x + sz, p.y + sz);
    ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);

    dl->AddRectFilled(a, b, click ? IM_COL32(45, 110, 60, 255)
                                   : ImGui::GetColorU32(ImGuiCol_FrameBg), 12.0f);
    dl->AddRect(a, b, touch ? IM_COL32(80, 180, 255, 255) : border, 12.0f);
    if (touch) {
        float px = nx < -1 ? -1 : (nx > 1 ? 1 : nx);
        float py = ny < -1 ? -1 : (ny > 1 ? 1 : ny);
        ImVec2 ctr((a.x + b.x) / 2, (a.y + b.y) / 2);
        float half = sz / 2 - 6;
        dl->AddCircleFilled(ImVec2(ctr.x + px * half, ctr.y - py * half), 7.0f,
                            IM_COL32(80, 180, 255, 255));
    }
    ImGui::Dummy(ImVec2(sz, sz));

    ImGui::TextDisabled("%s   %s", touch ? "TOUCH" : "touch", click ? "CLICK" : "click");

    // movement bar with the deadzone at the half-way mark
    ImVec2 bp = ImGui::GetCursorScreenPos();
    const float bw = sz, bh = 14.0f;
    ImVec2 ba = bp, bb = ImVec2(bp.x + bw, bp.y + bh);
    float f = velFrac < 0 ? 0 : (velFrac > 1 ? 1 : velFrac);
    bool active = velFrac >= 0.5f;
    dl->AddRectFilled(ba, bb, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
    dl->AddRectFilled(ba, ImVec2(bp.x + bw * f, bp.y + bh),
                      active ? IM_COL32(50, 180, 80, 255) : IM_COL32(110, 115, 125, 255), 4.0f);
    float mid = bp.x + bw * 0.5f;
    dl->AddLine(ImVec2(mid, bp.y - 1), ImVec2(mid, bp.y + bh + 1), IM_COL32(230, 80, 80, 255), 2.0f);
    dl->AddRect(ba, bb, border, 4.0f);
    ImGui::Dummy(ImVec2(bw, bh));
    ImGui::TextDisabled("movement | deadzone");
    ImGui::EndGroup();
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
    UpdateTrayIcon();
}

void TrayApp::RemoveTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void TrayApp::UpdateTrayIcon() {
    bool connected = m_controller && m_controller->IsConnected();
    bool game      = m_controller && m_controller->IsGameModeActive();

    const wchar_t* base = game ? L"Steamless Controller - Steamless Mode ON"
                        : connected ? L"Steamless Controller - Connected"
                                    : L"Steamless Controller - No controller found";
    wchar_t tip[128];
    int batt = m_controller ? m_controller->GetBatteryPercent() : -1;
    if (batt >= 0) {
        bool charging = m_controller->IsCharging();
        swprintf_s(tip, L"%s  -  Battery %d%%%s", base, batt,
                   charging ? (batt >= 100 ? L" (charged)" : L" (charging)") : L"");
    } else {
        wcscpy_s(tip, base);
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = TRAY_UID;
    nid.uFlags = NIF_TIP | NIF_ICON;
    nid.hIcon  = game ? m_iconOn : m_iconOff;
    wcscpy_s(nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayApp::ShowMainWindow() {
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    m_visible = true;
}

void TrayApp::HideToTray() {
    SaveWindowPlacement();
    ShowWindow(m_hwnd, SW_HIDE);
    m_visible = false;
}

void TrayApp::ShowContextMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Open");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(m_hwnd);
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
}

// Drive the on-screen keyboard from BOTH trackpads (each aims at its half), with
// an optional extra "type" button. Runs on the UI thread (KB_TIMER) so all the
// GDI/SendInput work happens off the read thread. side: 0 = right, 1 = left
// (matches the haptic actuator numbering).
void TrayApp::PollKeyboard() {
    if (!m_keyboard.IsVisible()) return;
    uint8_t rep[64];
    size_t n = m_controller->GetLatestReport(rep, sizeof(rep));
    if (n < 30) return;

    m_keyboard.SetSplit(m_controller->IsKbSplit());
    const bool relative = m_controller->IsKbRelative();

    auto pad = [&](int xi, int yi) {
        int16_t x, y;
        std::memcpy(&x, rep + xi, 2);
        std::memcpy(&y, rep + yi, 2);
        float nx = (static_cast<float>(x) + 32767.0f) / 65534.0f;   // left->0, right->1
        float ny = (32767.0f - static_cast<float>(y)) / 65534.0f;   // top->0, bottom->1
        return std::pair<float, float>(nx, ny);
    };

    const bool touch[2] = { (rep[5] & 0x02) != 0, (rep[4] & 0x20) != 0 };  // left, right
    const bool click[2] = { (rep[5] & 0x04) != 0, (rep[4] & 0x40) != 0 };
    const int  xi[2] = { 18, 24 }, yi[2] = { 20, 26 };

    for (int s = 0; s < 2; ++s) {
        if (touch[s]) {
            auto p = pad(xi[s], yi[s]);
            if (relative) {
                if (m_kbWasTouch[s])
                    m_keyboard.MovePointer(s, (p.first - m_kbPrevNx[s]) * 1.4f,
                                              (p.second - m_kbPrevNy[s]) * 1.4f);
                m_kbPrevNx[s] = p.first; m_kbPrevNy[s] = p.second;
            } else {
                m_keyboard.SetPointerAbs(s, p.first, p.second);
            }
        }
        m_kbWasTouch[s] = touch[s];
    }

    // Per-side commit: the pad hard-press (if enabled) and/or a remapped button.
    const bool usePad = m_controller->IsKbUsePadClick();
    const int  clickBtn[2] = { m_controller->GetKbClickLeft(),
                               m_controller->GetKbClickRight() };
    const uint8_t actuator[2] = { 1, 0 };   // left pad -> side 1, right pad -> side 0

    auto held = [&](int idx) -> bool {
        if (idx < 0 || idx >= InputMapper::kSourceCount) return false;
        const InputMapper::Source& s = InputMapper::kSources[idx];
        return n > s.byteIndex && (rep[s.byteIndex] & s.mask) != 0;
    };

    for (int s = 0; s < 2; ++s) {
        const bool active = (usePad && click[s]) || held(clickBtn[s]);
        const bool press   = active && !m_kbPrevActive[s];
        const bool release = !active && m_kbPrevActive[s];
        if (press) {
            m_keyboard.Commit(s);
            m_controller->KeyboardHaptic(actuator[s]);   // press feedback
        } else if (release) {
            m_controller->KeyboardHaptic(actuator[s]);   // release feedback
        }
        m_kbPrevActive[s] = active;
    }
}

// ---------------------------------------------------------------------------
// Settings / startup (registry)
// ---------------------------------------------------------------------------
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
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(quoted),
                       static_cast<DWORD>((wcslen(quoted) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, APP_NAME);
    }
    RegCloseKey(key);
}

std::wstring TrayApp::ProfilePath() const {
    return std::wstring(REG_PROFILES) + L"\\" + m_activeProfile;
}

void TrayApp::LoadProfileSettings(HKEY key) {
    auto rb = [&](const wchar_t* name, bool def) -> bool {
        DWORD v = 0, sz = sizeof(v);
        if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(&v), &sz) == ERROR_SUCCESS)
            return v != 0;
        return def;
    };
    auto rd = [&](const wchar_t* name, DWORD def) -> DWORD {
        DWORD v = 0, sz = sizeof(v);
        if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(&v), &sz) == ERROR_SUCCESS)
            return v;
        return def;
    };

    m_controller->SetTrackpadMouseEnabled(rb(L"TrackpadMouse",   false));
    m_controller->SetScrollWheelEnabled  (rb(L"ScrollWheel",     false));
    m_controller->SetInvertScroll        (rb(L"InvertScroll",    false));
    m_controller->SetSmartScroll         (rb(L"SmartScroll",     false));
    m_controller->SetUseLeftTrackpad     (rb(L"UseLeftTrackpad", false));
    m_controller->SetTrackpadSensitivity (static_cast<int>(rd(L"TrackpadSensitivity", 35)));
    m_controller->SetScrollSensitivity   (static_cast<int>(rd(L"ScrollSensitivity",   30)));
    m_controller->SetMouseDeadzone       (static_cast<int>(rd(L"MouseDeadzonePos",     50)));
    m_controller->SetScrollDeadzone      (static_cast<int>(rd(L"ScrollDeadzonePos",    50)));
    m_controller->SetLeftDeadzone        (static_cast<int>(rd(L"LeftDeadzone",         10)));
    m_controller->SetRightDeadzone       (static_cast<int>(rd(L"RightDeadzone",        10)));
    m_controller->SetLeftStickSensitivity (static_cast<int>(rd(L"LeftStickSens",       50)));
    m_controller->SetRightStickSensitivity(static_cast<int>(rd(L"RightStickSens",      50)));
    m_controller->SetHapticOnClick        (rb(L"HapticOnClick", false));
    m_controller->SetHapticOnMove         (rb(L"HapticOnMove", false));
    m_controller->SetHapticIntensity      (static_cast<int>(rd(L"HapticDensity",        50)));
    m_controller->SetHapticClickHardness  (static_cast<int>(rd(L"HapticClickHardness",    2)));
    m_controller->SetKbOpenButton         (static_cast<int>(rd(L"KbOpenButton",    2)));
    m_controller->SetKbOpenModifier       (static_cast<int>(rd(L"KbOpenModifier", 10)));
    m_controller->SetKbOpenHold           (rb(L"KbOpenHold", false));
    m_controller->SetKbClickLeft          (static_cast<int>(rd(L"KbClickL", 0xFFFFFFFF)));
    m_controller->SetKbClickRight         (static_cast<int>(rd(L"KbClickR", 0xFFFFFFFF)));
    m_controller->SetKbUsePadClick        (rb(L"KbUsePadClick", true));
    m_controller->SetKbSplit              (rb(L"KbSplit",    true));
    m_controller->SetKbRelative           (rb(L"KbRelative", false));

    m_controller->ResetButtonMappings();
    for (int i = 0; i < InputMapper::kSourceCount; ++i) {
        wchar_t name[16];
        swprintf_s(name, L"MapBtn%d", i);
        DWORD v = rd(name, 0xFFFFFFFF);
        if (v != 0xFFFFFFFF) {
            InputMapper::Action a;
            a.type  = static_cast<InputMapper::Type>((v >> 16) & 0xFF);
            a.value = static_cast<uint16_t>(v & 0xFFFF);
            m_controller->SetButtonAction(i, a);
        }
    }
}

void TrayApp::SaveProfileSettings(HKEY key) {
    auto wb = [&](const wchar_t* name, bool val) {
        DWORD dw = val ? 1 : 0;
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dw), sizeof(dw));
    };
    auto wd = [&](const wchar_t* name, DWORD val) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
    };

    wb(L"TrackpadMouse",   m_controller->IsTrackpadMouseEnabled());
    wb(L"ScrollWheel",     m_controller->IsScrollWheelEnabled());
    wb(L"InvertScroll",    m_controller->IsInvertScroll());
    wb(L"SmartScroll",     m_controller->IsSmartScroll());
    wb(L"UseLeftTrackpad", m_controller->IsUseLeftTrackpad());
    wb(L"HapticOnClick",   m_controller->IsHapticOnClick());
    wb(L"HapticOnMove",    m_controller->IsHapticOnMove());
    wd(L"TrackpadSensitivity", static_cast<DWORD>(m_controller->GetTrackpadSensitivity()));
    wd(L"ScrollSensitivity",   static_cast<DWORD>(m_controller->GetScrollSensitivity()));
    wd(L"MouseDeadzonePos",    static_cast<DWORD>(m_controller->GetMouseDeadzone()));
    wd(L"ScrollDeadzonePos",   static_cast<DWORD>(m_controller->GetScrollDeadzone()));
    wd(L"LeftDeadzone",        static_cast<DWORD>(m_controller->GetLeftDeadzone()));
    wd(L"RightDeadzone",       static_cast<DWORD>(m_controller->GetRightDeadzone()));
    wd(L"LeftStickSens",       static_cast<DWORD>(m_controller->GetLeftStickSensitivity()));
    wd(L"RightStickSens",      static_cast<DWORD>(m_controller->GetRightStickSensitivity()));
    wd(L"HapticDensity",       static_cast<DWORD>(m_controller->GetHapticIntensity()));
    wd(L"HapticClickHardness", static_cast<DWORD>(m_controller->GetHapticClickHardness()));
    wd(L"KbOpenButton",        static_cast<DWORD>(m_controller->GetKbOpenButton()));
    wd(L"KbOpenModifier",      static_cast<DWORD>(m_controller->GetKbOpenModifier()));
    wb(L"KbOpenHold",          m_controller->IsKbOpenHold());
    wd(L"KbClickL",            static_cast<DWORD>(m_controller->GetKbClickLeft()));
    wd(L"KbClickR",            static_cast<DWORD>(m_controller->GetKbClickRight()));
    wb(L"KbUsePadClick",       m_controller->IsKbUsePadClick());
    wb(L"KbSplit",             m_controller->IsKbSplit());
    wb(L"KbRelative",          m_controller->IsKbRelative());

    for (int i = 0; i < InputMapper::kSourceCount; ++i) {
        InputMapper::Action a = m_controller->GetButtonAction(i);
        DWORD v = (static_cast<DWORD>(static_cast<uint8_t>(a.type)) << 16) |
                  static_cast<DWORD>(a.value);
        wchar_t name[16];
        swprintf_s(name, L"MapBtn%d", i);
        wd(name, v);
    }
}

void TrayApp::LoadSettings() {
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &root) == ERROR_SUCCESS) {
        wchar_t name[128]; DWORD sz = sizeof(name), type = 0;
        if (RegQueryValueExW(root, L"ActiveProfile", nullptr, &type,
                             reinterpret_cast<LPBYTE>(name), &sz) == ERROR_SUCCESS && type == REG_SZ) {
            name[127] = 0;
            if (name[0]) m_activeProfile = name;
        }
        DWORD ae = 0, aesz = sizeof(ae);
        if (RegQueryValueExW(root, L"AutoEnable", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&ae), &aesz) == ERROR_SUCCESS)
            m_controller->SetAutoEnable(ae != 0);
        RegCloseKey(root);
    }

    HKEY pk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ProfilePath().c_str(), 0, KEY_READ, &pk) == ERROR_SUCCESS) {
        LoadProfileSettings(pk);
        RegCloseKey(pk);
    } else {
        HKEY legacy;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &legacy) == ERROR_SUCCESS) {
            LoadProfileSettings(legacy);
            RegCloseKey(legacy);
        }
        SaveSettings();
    }
}

void TrayApp::SaveSettings() {
    HKEY root;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, nullptr, &root, nullptr) == ERROR_SUCCESS) {
        DWORD ae = m_controller->IsAutoEnable() ? 1 : 0;
        RegSetValueExW(root, L"AutoEnable", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&ae), sizeof(ae));
        RegSetValueExW(root, L"ActiveProfile", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(m_activeProfile.c_str()),
                       static_cast<DWORD>((m_activeProfile.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(root);
    }
    HKEY pk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ProfilePath().c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &pk, nullptr) == ERROR_SUCCESS) {
        SaveProfileSettings(pk);
        RegCloseKey(pk);
    }
}

// The display order is a separate, user-controllable list (most-recently
// added first, draggable). It's stored as a newline-delimited string so we
// don't depend on RegEnumKey's alphabetical ordering.
std::vector<std::wstring> TrayApp::ReadProfileOrder() const {
    std::vector<std::wstring> order;
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &root) == ERROR_SUCCESS) {
        wchar_t buf[2048]; DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(root, L"ProfileOrder", nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS && type == REG_SZ) {
            buf[sz / sizeof(wchar_t)] = 0;   // guarantee termination
            std::wstring s(buf), cur;
            for (wchar_t c : s) {
                if (c == L'\n') { if (!cur.empty()) order.push_back(cur); cur.clear(); }
                else            cur.push_back(c);
            }
            if (!cur.empty()) order.push_back(cur);
        }
        RegCloseKey(root);
    }
    return order;
}

void TrayApp::SaveProfileOrder(const std::vector<std::wstring>& order) const {
    std::wstring s;
    for (auto const& n : order) { s += n; s += L'\n'; }
    HKEY root;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, nullptr, &root, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(root, L"ProfileOrder", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(s.c_str()),
                       static_cast<DWORD>((s.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(root);
    }
}

std::vector<std::wstring> TrayApp::ListProfiles() const {
    // Everything that actually exists as a profile subkey.
    std::vector<std::wstring> actual;
    HKEY base;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PROFILES, 0, KEY_READ, &base) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            wchar_t nm[128]; DWORD sz = 128;
            if (RegEnumKeyExW(base, i, nm, &sz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            actual.push_back(nm);
        }
        RegCloseKey(base);
    }

    // Apply the saved order, dropping stale entries; append any profiles that
    // exist but aren't listed yet (e.g. created before ordering existed).
    auto contains = [](const std::vector<std::wstring>& v, const std::wstring& x) {
        for (auto const& e : v) if (e == x) return true;
        return false;
    };
    std::vector<std::wstring> out;
    for (auto const& n : ReadProfileOrder())
        if (contains(actual, n) && !contains(out, n)) out.push_back(n);
    for (auto const& n : actual)
        if (!contains(out, n)) out.push_back(n);

    if (out.empty()) out.push_back(L"Default");
    return out;
}

void TrayApp::SwitchProfile(const std::wstring& name) {
    if (name.empty() || name == m_activeProfile || !ProfileExists(name)) return;
    m_activeProfile = name;
    HKEY pk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ProfilePath().c_str(), 0, KEY_READ, &pk) == ERROR_SUCCESS) {
        LoadProfileSettings(pk);
        RegCloseKey(pk);
    }
    SaveSettings();
}

void TrayApp::CreateProfile(const std::wstring& name) {
    if (name.empty() || ProfileExists(name)) return;
    m_activeProfile = name;
    SaveSettings();
    // Newest profile goes to the bottom of the list.
    auto order = ReadProfileOrder();
    order.push_back(name);
    SaveProfileOrder(order);
}

void TrayApp::RenameProfile(const std::wstring& newName) {
    if (newName.empty() || newName == m_activeProfile || ProfileExists(newName)) return;
    std::wstring oldName = m_activeProfile;
    m_activeProfile = newName;
    SaveSettings();
    std::wstring oldPath = std::wstring(REG_PROFILES) + L"\\" + oldName;
    RegDeleteKeyW(HKEY_CURRENT_USER, oldPath.c_str());
    // Keep the renamed profile in its existing slot.
    auto order = ReadProfileOrder();
    bool replaced = false;
    for (auto& n : order) if (n == oldName) { n = newName; replaced = true; break; }
    if (!replaced) order.push_back(newName);
    SaveProfileOrder(order);
}

void TrayApp::DeleteProfile() {
    auto profiles = ListProfiles();
    if (profiles.size() <= 1) return;
    std::wstring victim = m_activeProfile;
    RegDeleteKeyW(HKEY_CURRENT_USER, ProfilePath().c_str());
    auto order = ReadProfileOrder();
    order.erase(std::remove(order.begin(), order.end(), victim), order.end());
    SaveProfileOrder(order);
    m_activeProfile = (profiles[0] != victim) ? profiles[0] : profiles[1];
    HKEY pk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ProfilePath().c_str(), 0, KEY_READ, &pk) == ERROR_SUCCESS) {
        LoadProfileSettings(pk);
        RegCloseKey(pk);
    }
    SaveSettings();
}
