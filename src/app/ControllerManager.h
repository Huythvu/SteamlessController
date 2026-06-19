#pragma once
#include "TrackpadMouse.h"
#include "InputMapper.h"
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <cstdint>

class VirtualController;

// Manages the Steam Controller lifecycle: device discovery, lizard mode
// disable/enable, and the heartbeat that keeps lizard mode off.
// All public methods are safe to call from the UI thread.
class ControllerManager {
public:
    using StateChangedFn = std::function<void(bool connected, bool gameModeActive, bool vigemMissing)>;

    explicit ControllerManager(StateChangedFn onStateChanged);
    ~ControllerManager();
    ControllerManager(const ControllerManager&) = delete;
    ControllerManager& operator=(const ControllerManager&) = delete;

    // Called when Windows reports a device arrival or removal (WM_DEVICECHANGE).
    void OnDeviceChange();

    // Toggle game mode on/off. No-op if controller is not connected.
    void EnableGameMode();
    void DisableGameMode();

    void SetTrackpadMouseEnabled(bool enabled);
    void SetBackButtonsEnabled(bool enabled);
    void SetUseLeftTrackpad(bool enabled);
    void SetScrollWheelEnabled(bool enabled);
    void SetInvertScroll(bool enabled);
    void SetTrackpadSensitivity(int pos);   // 1..100
    void SetScrollSensitivity(int pos);     // 1..100
    void SetMouseDeadzone(int units);       // 0..500
    void SetScrollDeadzone(int units);      // 0..500

    void SetLeftDeadzone(int pos);          // 0..90
    void SetRightDeadzone(int pos);         // 0..90
    void SetLeftStickSensitivity(int pos);  // 1..100
    void SetRightStickSensitivity(int pos); // 1..100

    bool IsConnected()             const { return m_connected.load(); }
    bool IsGameModeActive()        const { return m_gameModeActive.load(); }
    bool IsTrackpadMouseEnabled()  const { return m_trackpadMouseEnabled; }
    bool IsBackButtonsEnabled()    const { return m_backButtonsEnabled; }
    bool IsUseLeftTrackpad()       const { return m_useLeftTrackpad; }
    bool IsScrollWheelEnabled()    const { return m_scrollWheelEnabled; }
    bool IsInvertScroll()          const { return m_invertScroll; }
    int  GetTrackpadSensitivity()  const { return m_trackpadSensitivity; }
    int  GetScrollSensitivity()    const { return m_scrollSensitivity; }
    int  GetMouseDeadzone()        const { return m_mouseDeadzone; }
    int  GetScrollDeadzone()       const { return m_scrollDeadzone; }
    int  GetLeftDeadzone()         const { return m_lDeadzone; }
    int  GetRightDeadzone()        const { return m_rDeadzone; }
    int  GetLeftStickSensitivity() const { return m_lStickSens; }
    int  GetRightStickSensitivity()const { return m_rStickSens; }

    // Temporarily disable trackpad mouse + scroll output (e.g. while the live
    // input monitor is open) without altering the saved settings.
    void SuspendTrackpad(bool suspended);

    // Experimental: fire a test haptic pulse on both actuators (if connected).
    void TestHaptic();

    // Trackpad haptic feedback (on the pad being used).
    void SetHapticOnClick(bool enabled);
    void SetHapticOnMove(bool enabled);
    void SetHapticIntensity(int pos);    // 1..100
    bool IsHapticOnClick()    const { return m_hapticOnClick; }
    bool IsHapticOnMove()     const { return m_hapticOnMove; }
    int  GetHapticIntensity() const { return m_hapticIntensity; }

    // Button remapping. Thread-safe (serialized against the read loop).
    void                SetButtonAction(int sourceIndex, InputMapper::Action a);
    InputMapper::Action GetButtonAction(int sourceIndex) const;
    void                ResetButtonMappings();

    // Copy the most recent input report (for the live monitor). Returns the
    // number of bytes copied, or 0 if none captured yet. Thread-safe.
    size_t GetLatestReport(uint8_t* out, size_t outSize) const;

    // Approximate battery percentage from the latest report, or -1 if unknown
    // (e.g. no report captured yet). Thread-safe.
    int GetBatteryPercent() const;

private:
    void TryOpen();
    void Close(bool restoreLizard);
    void ApplyStickConfigToVirtual();
    void StartReadLoop();
    void StopReadLoop();
    void ReadLoop();

    StateChangedFn                     m_onStateChanged;
    std::atomic<bool>                  m_connected{false};
    std::atomic<bool>                  m_gameModeActive{false};
    bool                               m_trackpadMouseEnabled = false;
    bool                               m_backButtonsEnabled   = false;
    bool                               m_useLeftTrackpad      = false;
    bool                               m_scrollWheelEnabled   = false;
    bool                               m_invertScroll         = false;
    std::atomic<bool>                  m_trackpadSuspended{false};
    bool                               m_hapticOnClick        = false;
    bool                               m_hapticOnMove         = false;
    int                                m_hapticIntensity      = 60;   // 1..100
    int                                m_trackpadSensitivity  = 35;   // 1..100
    int                                m_scrollSensitivity    = 30;   // 1..100
    int                                m_mouseDeadzone        = 20;   // 0..500
    int                                m_scrollDeadzone       = 120;  // 0..500
    int                                m_lDeadzone            = 10;   // 0..90 (%)
    int                                m_rDeadzone            = 10;   // 0..90 (%)
    int                                m_lStickSens           = 50;   // 1..100
    int                                m_rStickSens           = 50;   // 1..100
    std::unique_ptr<VirtualController> m_virtual;
    TrackpadMouse                      m_trackpad;
    InputMapper                        m_mapper;
    mutable std::mutex                 m_inputMutex;
    std::thread                        m_readThread;
    std::atomic<bool>                  m_readRunning{false};

    mutable std::mutex                 m_reportMutex;
    uint8_t                            m_lastReport[64] = {};
    size_t                             m_lastReportLen  = 0;
};
