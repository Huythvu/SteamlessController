#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <memory>
#include <vector>
#include <string>
#include <atomic>

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

    // --- Tray ---
    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayIcon();
    void ShowMainWindow();
    void HideToTray();
    void ShowContextMenu();

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

    // UI scratch state
    int                                m_currentTab = 0;
    int                                m_recordIndex = -1;  // source listening for a key
    char                               m_profileNameBuf[64] = {};

    // Trackpad live-view state: previous touch sample + smoothed velocity.
    int                                m_tpMx = 0, m_tpMy = 0; bool m_tpMt = false;
    int                                m_tpSx = 0, m_tpSy = 0; bool m_tpSt = false;
    float                              m_tpMouseVel = 0.0f, m_tpScrollVel = 0.0f;

    // Tray state marshaled from the controller callback (any thread).
    std::atomic_bool                   m_pendingConnected{false};
    std::atomic_bool                   m_pendingGameMode{false};
    std::atomic_bool                   m_pendingVigemMissing{false};

    static constexpr UINT WM_TRAY          = WM_APP + 1;
    static constexpr UINT WM_STATE_CHANGED = WM_APP + 2;
    static constexpr UINT TRAY_UID         = 1;
    static constexpr UINT BATT_TIMER       = 2;
    static constexpr UINT IDM_OPEN         = 1001;
    static constexpr UINT IDM_EXIT         = 1002;
};
