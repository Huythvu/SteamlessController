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

    m_hwnd = CreateWindowExW(0, WNDCLASS_NAME, L"SteamlessController",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1350, 840, nullptr, nullptr, hInstance, nullptr);
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

    m_controller = std::make_unique<ControllerManager>(
        [this](bool, bool, bool vigemMissing) {
            m_pendingVigemMissing.store(vigemMissing);
            PostMessageW(m_hwnd, WM_STATE_CHANGED, 0, 0);
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
    return 0;
}

LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) return g_app->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
        }
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
        else if (LOWORD(wp) == IDM_EXIT) { m_controller->DisableGameMode(); m_done = true; PostQuitMessage(0); }
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

    case WM_TIMER:
        if (wp == BATT_TIMER) UpdateTrayIcon();
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
        slider("Mouse deadzone", c.GetMouseDeadzone(), 1, 100,
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
        slider("Scroll sensitivity", c.GetScrollSensitivity(), 1, 100,
               &ControllerManager::SetScrollSensitivity);
        slider("Scroll deadzone", c.GetScrollDeadzone(), 1, 100,
               &ControllerManager::SetScrollDeadzone);
        ImGui::EndDisabled();
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
        toggle("Buzz on mouse / scroll movement", c.IsHapticOnMove(),
               &ControllerManager::SetHapticOnMove);
        ImGui::BeginDisabled(!c.IsHapticOnClick() && !c.IsHapticOnMove());
        slider("Intensity (clicks per movement)", c.GetHapticIntensity(), 1, 100,
               &ControllerManager::SetHapticIntensity);
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::BeginDisabled(!c.IsConnected());
        if (ImGui::Button("Test haptic")) c.TestHaptic();
        ImGui::EndDisabled();
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

        toggle("Enable back grip buttons", c.IsBackButtonsEnabled(),
               &ControllerManager::SetBackButtonsEnabled);

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

// Where each physical button sits on the diagram (canvas-local pixels) and the
// short label drawn inside it. Indices refer to InputMapper::kSources.
namespace {
struct Spot { int idx; float x, y, r; const char* label; };
constexpr float kCanvasW = 600.0f, kCanvasH = 380.0f;
const Spot kLayout[] = {
    { 4,   95,  50, 26, "LB" },   { 5,  505,  50, 26, "RB" },
    { 6,  120, 165, 32, "LS" },   { 7,  400, 270, 32, "RS" },
    { 11, 235, 215, 20, "Up" },   { 12, 235, 305, 20, "Dn" },
    { 13, 190, 260, 20, "Lt" },   { 14, 280, 260, 20, "Rt" },
    { 3,  500, 120, 24, "Y" },    { 0,  500, 210, 24, "A" },
    { 2,  455, 165, 24, "X" },    { 1,  545, 165, 24, "B" },
    { 9,  280, 165, 18, "View" }, { 10, 310, 120, 16, "Steam" }, { 8, 340, 165, 18, "Menu" },
    { 15, 160, 350, 18, "L4" },   { 16, 215, 350, 18, "L5" },
    { 17, 385, 350, 18, "R4" },   { 18, 440, 350, 18, "R5" },
};
}

// Controller tab: a visual gamepad whose buttons light up live and are remapped
// by clicking one and then pressing the key to bind (JoyToKey style).
void TrayApp::DrawControllerTab() {
    uint8_t rep[64];
    size_t n = m_controller->GetLatestReport(rep, sizeof(rep));

    if (ImGui::Button("Reset all mappings")) {
        m_controller->ResetButtonMappings();
        SaveSettings();
        m_recordIndex = -1;
    }
    ImGui::SameLine(0, 16);
    if (m_recordIndex >= 0)
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.20f, 1.0f),
            "Press a key for \"%s\"  (Esc cancel, Del clear)",
            Narrow(InputMapper::kSources[m_recordIndex].name).c_str());
    else
        ImGui::TextDisabled("Click a button, then press a key to bind it. Right-click = reset to default.");
    ImGui::Spacing();

    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(kCanvasW, kCanvasH));        // reserve the canvas
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + kCanvasW, o.y + kCanvasH),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), 12.0f);
    dl->AddRect(o, ImVec2(o.x + kCanvasW, o.y + kCanvasH),
                ImGui::GetColorU32(ImGuiCol_Border), 12.0f);

    ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
    ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    for (const Spot& sp : kLayout) {
        const InputMapper::Source& s = InputMapper::kSources[sp.idx];
        bool live = (n > s.byteIndex) && (rep[s.byteIndex] & s.mask) != 0;
        bool rec  = (m_recordIndex == sp.idx);
        ImVec2 c(o.x + sp.x, o.y + sp.y);

        ImGui::SetCursorScreenPos(ImVec2(c.x - sp.r, c.y - sp.r));
        ImGui::InvisibleButton((std::string("##spot") + std::to_string(sp.idx)).c_str(),
                               ImVec2(sp.r * 2, sp.r * 2),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))  m_recordIndex = sp.idx;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            m_controller->SetButtonAction(sp.idx, s.def);
            SaveSettings();
            if (rec) m_recordIndex = -1;
        }
        if (hovered)
            ImGui::SetTooltip("%s  ->  %s", Narrow(s.name).c_str(),
                              ActionLabel(m_controller->GetButtonAction(sp.idx)).c_str());

        ImU32 fill = rec   ? IM_COL32(240, 200, 50, 255)
                   : live  ? IM_COL32(60, 150, 240, 255)
                   : hovered ? IM_COL32(90, 95, 105, 255)
                             : IM_COL32(60, 63, 70, 255);
        dl->AddCircleFilled(c, sp.r, fill, 32);
        dl->AddCircle(c, sp.r, border, 32);
        ImVec2 ts = ImGui::CalcTextSize(sp.label);
        dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), textCol, sp.label);
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + kCanvasH));

    ImGui::Spacing();
    auto trig = [&](int off) -> float {
        if (n < static_cast<size_t>(off) + 2) return 0.0f;
        int16_t v; std::memcpy(&v, rep + off, 2);
        float f = v / 32767.0f;
        return f < 0 ? 0 : (f > 1 ? 1 : f);
    };
    ImGui::TextDisabled("TRIGGERS");
    ImGui::ProgressBar(trig(6), ImVec2(-1, 0), "");
    ImGui::ProgressBar(trig(8), ImVec2(-1, 0), "");
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
    m_controller->SetBackButtonsEnabled  (rb(L"BackButtons",     false));
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
    m_controller->SetHapticOnMove         (rb(L"HapticOnMove",  false));
    m_controller->SetHapticIntensity      (static_cast<int>(rd(L"HapticDensity",        50)));

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
    wb(L"BackButtons",     m_controller->IsBackButtonsEnabled());
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
