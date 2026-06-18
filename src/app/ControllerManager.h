#pragma once
#include "TrackpadMouse.h"
#include <functional>
#include <thread>
#include <atomic>
#include <memory>

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
    void SetTrackpadSensitivity(int pos);   // 1..100

    bool IsConnected()             const { return m_connected; }
    bool IsGameModeActive()        const { return m_gameModeActive; }
    bool IsTrackpadMouseEnabled()  const { return m_trackpadMouseEnabled; }
    bool IsBackButtonsEnabled()    const { return m_backButtonsEnabled; }
    bool IsUseLeftTrackpad()       const { return m_useLeftTrackpad; }
    bool IsScrollWheelEnabled()    const { return m_scrollWheelEnabled; }
    int  GetTrackpadSensitivity()  const { return m_trackpadSensitivity; }

private:
    void TryOpen();
    void Close(bool restoreLizard);
    void StartReadLoop();
    void StopReadLoop();
    void ReadLoop();

    StateChangedFn                     m_onStateChanged;
    bool                               m_connected            = false;
    bool                               m_gameModeActive       = false;
    bool                               m_trackpadMouseEnabled = false;
    bool                               m_backButtonsEnabled   = false;
    bool                               m_useLeftTrackpad      = false;
    bool                               m_scrollWheelEnabled   = false;
    int                                m_trackpadSensitivity  = 15;   // 1..100
    std::unique_ptr<VirtualController> m_virtual;
    TrackpadMouse                      m_trackpad;
    std::thread                        m_readThread;
    std::atomic<bool>                  m_readRunning{false};
};
