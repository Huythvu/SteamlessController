#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include "KeyboardOverlay.h"

class ControllerManager;

// Dear ImGui / DirectX 11 application: a single window that hides to the tray.
class TrayApp {
public:
    TrayApp();
    ~TrayApp();

    bool Init(HINSTANCE hInstance);
    int  Run();

private:
    // --- DX11 plumbing ---
    bool CreateDeviceD3D(HWND hWnd);
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void RenderFrame();
    void SaveWindowPlacement();

    // --- UI (one frame) ---
    void DrawUI();
    void DrawTopBar();
    void DrawProfilesSidebar();
    void DrawTabs();
    void DrawControllerTab();
    void DrawStickView(float nx, float ny, float dz);
    void DrawTrackpadView(const char* label, bool touch, bool click,
                          float nx, float ny, float velFrac);
    void DrawKeyboardPreview(float size);   // live view of the on-screen keyboard

    // --- Tray ---
    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayIcon();
    void ShowMainWindow();
    void HideToTray();
    void ShowContextMenu();
    void PollKeyboard();   // drive the on-screen keyboard from the trackpad

    // --- Settings / profiles (registry) ---
    std::wstring ProfilePath() const;
    void LoadProfileSettings(HKEY key);
    void SaveProfileSettings(HKEY key);
    void LoadSettings();
    void SaveSettings();
    std::vector<std::wstring> ListProfiles() const;
    std::vector<std::wstring> ReadProfileOrder() const;
    void SaveProfileOrder(const std::vector<std::wstring>& order) const;
    void SwitchProfile(const std::wstring& name);
    void CreateProfile(const std::wstring& name);
    void RenameProfile(const std::wstring& newName);
    void DeleteProfile();
    bool IsStartupEnabled() const;
    void SetStartupEnabled(bool enabled);

    HINSTANCE                          m_hInstance = nullptr;
    HWND                               m_hwnd      = nullptr;
    ID3D11Device*                      m_device    = nullptr;
    ID3D11DeviceContext*               m_ctx       = nullptr;
    IDXGISwapChain*                    m_swap      = nullptr;
    ID3D11RenderTargetView*            m_rtv       = nullptr;

    bool                               m_visible   = false;
    bool                               m_done      = false;
    UINT                               m_wmTaskbar = 0;
    HICON                              m_iconOff   = nullptr;
    HICON                              m_iconOn    = nullptr;
    HDEVNOTIFY                         m_devNotify = nullptr;

    std::unique_ptr<ControllerManager> m_controller;
    std::wstring                       m_activeProfile = L"Default";

    // On-screen keyboard overlay.
    KeyboardOverlay                    m_keyboard;
    bool                               m_kbPrevActive[2] = { false, false };  // commit edge per side
    float                              m_kbPrevNx[2]  = { 0.0f, 0.0f };        // relative mode
    float                              m_kbPrevNy[2]  = { 0.0f, 0.0f };
    bool                               m_kbWasTouch[2]= { false, false };
    int                                m_kbPrevSel[2] = { -1, -1 };            // hover haptic edge
    bool                               m_kbPrevShortcut[3] = { false, false, false };  // back/space/enter edges
    int                                m_kbRecordTarget = -1;                  // 0=open 1=mod 2=clickL 3=clickR 4=back 5=space 6=enter

    // UI scratch state
    int                                m_currentTab = 0;
    int                                m_recordIndex = -1;  // source listening for a key
    char                               m_profileNameBuf[64] = {};

    // Trackpad live-view: smoothed movement readout for the deadzone bar.
    float                              m_tpMouseVel = 0.0f, m_tpScrollVel = 0.0f;

    // Tray state marshaled from the controller callback (any thread).
    std::atomic_bool                   m_pendingConnected{false};
    std::atomic_bool                   m_pendingGameMode{false};
    std::atomic_bool                   m_pendingVigemMissing{false};

    static constexpr UINT WM_TRAY          = WM_APP + 1;
    static constexpr UINT WM_STATE_CHANGED = WM_APP + 2;
    static constexpr UINT WM_KB_SETOPEN    = WM_APP + 3;
    static constexpr UINT TRAY_UID         = 1;
    static constexpr UINT BATT_TIMER       = 2;
    static constexpr UINT KB_TIMER         = 3;
    static constexpr UINT IDM_OPEN         = 1001;
    static constexpr UINT IDM_EXIT         = 1002;
};
