#pragma once
#include <Windows.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <atomic>

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

    static LRESULT CALLBACK MonitorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMonitorMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void ShowMonitor();
    void PaintMonitor(HWND hwnd);

    static LRESULT CALLBACK MappingWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMappingMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void ShowMapping();
    void CreateMappingControls(HWND hwnd);
    void RefreshMappingControls();

    void CreateControls(HWND hwnd);
    void RefreshControls();
    void ShowMainWindow();
    void ShowTab(int index);

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
    HWND                               m_monitorHwnd = nullptr;
    HWND                               m_mappingHwnd = nullptr;
    HWND                               m_tab         = nullptr;
    std::vector<HWND>                  m_tabPages[4];
    HDEVNOTIFY                         m_devNotify = nullptr;
    std::unique_ptr<ControllerManager> m_controller;
    std::atomic_bool                   m_pendingConnected{false};
    std::atomic_bool                   m_pendingGameModeActive{false};
    std::atomic_bool                   m_pendingVigemMissing{false};

    // Tray menu command IDs
    static constexpr UINT IDM_OPEN          = 1001;
    static constexpr UINT IDM_EXIT          = 1002;
    static constexpr UINT IDC_MONITOR       = 1003;
    static constexpr UINT IDC_MAPPING       = 1004;
    static constexpr UINT IDC_HAPTIC_TEST   = 1005;
    static constexpr UINT IDC_TAB           = 1006;
    static constexpr UINT MON_TIMER         = 1;

    // Mapping window: one combo per source button, plus a reset button.
    static constexpr UINT IDC_MAP_BASE      = 3000;   // .. 3000 + kSourceCount-1
    static constexpr UINT IDC_MAP_RESET     = 3100;

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
    static constexpr UINT IDC_HAPTIC_CLICK  = 2020;
    static constexpr UINT IDC_HAPTIC_MOVE   = 2021;
    static constexpr UINT IDC_HAPTIC_INT    = 2022;
    static constexpr UINT IDC_HAPTIC_VAL    = 2023;
    static constexpr UINT IDC_MOUSE_DZ      = 2024;
    static constexpr UINT IDC_MOUSE_DZ_VAL  = 2025;
    static constexpr UINT IDC_SCROLL_DZ     = 2026;
    static constexpr UINT IDC_SCROLL_DZ_VAL = 2027;

    static constexpr UINT WM_TRAY          = WM_APP + 1;
    static constexpr UINT WM_STATE_CHANGED = WM_APP + 2;
    static constexpr UINT TRAY_UID = 1;
};
