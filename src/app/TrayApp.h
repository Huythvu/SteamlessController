#pragma once
#include <Windows.h>
#include <memory>

class ControllerManager;

class TrayApp {
public:
    TrayApp();
    ~TrayApp();

    bool Init(HINSTANCE hInstance);
    int  Run();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void CreateControls(HWND hwnd);
    void RefreshControls();
    void ShowMainWindow();

    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayIcon(bool connected, bool gameModeActive, bool vigemMissing = false);
    void ShowViGEmBalloon();
    void ShowContextMenu();
    void LoadSettings();
    void SaveSettings();
    bool IsStartupEnabled() const;
    void SetStartupEnabled(bool enabled);

    HWND                               m_hwnd      = nullptr;
    HINSTANCE                          m_hInstance = nullptr;
    UINT                               m_wmTaskbar = 0;
    HICON                              m_iconOff   = nullptr;
    HICON                              m_iconOn    = nullptr;
    HFONT                              m_font      = nullptr;
    std::unique_ptr<ControllerManager> m_controller;

    // Tray menu command IDs
    static constexpr UINT IDM_OPEN          = 1001;
    static constexpr UINT IDM_EXIT          = 1002;

    // Main-window control IDs
    static constexpr UINT IDC_STATUS        = 2000;
    static constexpr UINT IDC_TOGGLE        = 2001;
    static constexpr UINT IDC_TRACKPAD      = 2002;
    static constexpr UINT IDC_BACKBUTTONS   = 2003;
    static constexpr UINT IDC_LEFT_TRACKPAD = 2004;
    static constexpr UINT IDC_STARTUP       = 2005;
    static constexpr UINT IDC_SCROLL        = 2006;
    static constexpr UINT IDC_SENS          = 2007;
    static constexpr UINT IDC_SENS_VAL      = 2008;
    static constexpr UINT IDC_INVERT        = 2009;
    static constexpr UINT IDC_SCROLL_SENS   = 2010;
    static constexpr UINT IDC_SCROLL_VAL    = 2011;
    static constexpr UINT IDC_LDEADZONE     = 2012;
    static constexpr UINT IDC_LDEADZONE_VAL = 2013;
    static constexpr UINT IDC_RDEADZONE     = 2014;
    static constexpr UINT IDC_RDEADZONE_VAL = 2015;
    static constexpr UINT IDC_LSTICK        = 2016;
    static constexpr UINT IDC_LSTICK_VAL    = 2017;
    static constexpr UINT IDC_RSTICK        = 2018;
    static constexpr UINT IDC_RSTICK_VAL    = 2019;

    static constexpr UINT WM_TRAY  = WM_APP + 1;
    static constexpr UINT TRAY_UID = 1;
};
