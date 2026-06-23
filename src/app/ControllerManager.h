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
    using KeyboardSetOpenFn = std::function<void(bool open)>;   // fired from the read thread

    explicit ControllerManager(StateChangedFn onStateChanged,
                               KeyboardSetOpenFn onKeyboardSetOpen = nullptr);
    ~ControllerManager();
    ControllerManager(const ControllerManager&) = delete;
    ControllerManager& operator=(const ControllerManager&) = delete;

    // Called when Windows reports a device arrival or removal (WM_DEVICECHANGE).
    void OnDeviceChange();

    // On-screen keyboard mode: suppress normal mouse/gamepad/key output so the
    // trackpad can drive the keyboard overlay instead.
    void SetKeyboardMode(bool on) { m_keyboardMode = on; }
    bool IsKeyboardMode() const   { return m_keyboardMode.load(); }
    void KeyboardHaptic(uint8_t side, uint16_t amp = 2000);   // pulse on a key press (side 0=right,1=left)

    // --- On-screen keyboard configuration (source indices into InputMapper) ---
    void SetKbOpenButton(int i)   { m_kbOpenButton = i; }   int  GetKbOpenButton() const   { return m_kbOpenButton; }
    void SetKbOpenModifier(int i) { m_kbOpenModifier = i; } int  GetKbOpenModifier() const { return m_kbOpenModifier; }
    void SetKbOpenHold(bool b)    { m_kbOpenHold = b; }     bool IsKbOpenHold() const      { return m_kbOpenHold; }
    void SetKbClickLeft(int i)    { m_kbClickL = i; }       int  GetKbClickLeft() const    { return m_kbClickL; }
    void SetKbClickRight(int i)   { m_kbClickR = i; }       int  GetKbClickRight() const   { return m_kbClickR; }
    void SetKbUsePadClick(bool b) { m_kbUsePadClick = b; }  bool IsKbUsePadClick() const   { return m_kbUsePadClick; }
    void SetKbSplit(bool b)       { m_kbSplit = b; }        bool IsKbSplit() const         { return m_kbSplit; }
    void SetKbRelative(bool b)    { m_kbRelative = b; }     bool IsKbRelative() const      { return m_kbRelative; }
    void SetKbRelSens(int i)      { m_kbRelSens = i; }      int  GetKbRelSens() const      { return m_kbRelSens; }   // 1..100 (slide speed)
    void SetKbBall(bool b)        { m_kbBall = b; }         bool IsKbBall() const          { return m_kbBall; }
    void SetKbLayout(int i)       { m_kbLayout = i; }       int  GetKbLayout() const       { return m_kbLayout; }
    void SetKbKeyBackspace(int i) { m_kbKeyBack = i; }      int  GetKbKeyBackspace() const { return m_kbKeyBack; }
    void SetKbKeySpace(int i)     { m_kbKeySpace = i; }     int  GetKbKeySpace() const     { return m_kbKeySpace; }
    void SetKbKeyEnter(int i)     { m_kbKeyEnter = i; }     int  GetKbKeyEnter() const     { return m_kbKeyEnter; }

    // Toggle game mode on/off. No-op if controller is not connected.
    void EnableGameMode();
    void DisableGameMode();

    // Auto-enable Steamless Mode whenever a controller is connected.
    void SetAutoEnable(bool enabled) { m_autoEnable = enabled; }
    bool IsAutoEnable() const        { return m_autoEnable; }
    void ApplyAutoEnable();   // enable now if configured + connected + off

    // Per-pad role. side 0 = right pad, 1 = left pad; role is a PadRole value.
    void SetPadRole(int side, int role);
    int  GetPadRole(int side) const { return static_cast<int>(side == 1 ? m_padRoleLeft : m_padRoleRight); }
    void SetDpadWASD(bool wasd);
    bool IsDpadWASD() const { return m_dpadWASD; }
    void SetDpadSingle(bool b);     bool IsDpadSingle() const     { return m_dpadSingle; }
    void SetDpadOnClick(bool b);    bool IsDpadOnClick() const    { return m_dpadOnClick; }
    void SetButtonsOnClick(bool b); bool IsButtonsOnClick() const { return m_btnOnClick; }
    void SetButtonsSwap(bool b);    bool IsButtonsSwap() const    { return m_btnSwap; }
    void SetPadStickDeadzone(int pos); int GetPadStickDeadzone() const { return m_padStickDz; }  // 0..90
    void SetInvertScroll(bool enabled);
    void SetSmartScroll(bool enabled);
    void SetTrackpadSensitivity(int pos);   // 1..100
    void SetScrollSensitivity(int pos);     // 1..100
    void SetMouseDeadzone(int pos);         // 1..100 (50 = baseline)
    void SetScrollDeadzone(int pos);        // 1..100 (50 = baseline)

    void SetLeftDeadzone(int pos);          // 0..90
    void SetRightDeadzone(int pos);         // 0..90
    void SetLeftStickSensitivity(int pos);  // 1..100
    void SetRightStickSensitivity(int pos); // 1..100

    bool IsConnected()             const { return m_connected.load(); }
    bool IsGameModeActive()        const { return m_gameModeActive.load(); }
    bool IsInvertScroll()          const { return m_invertScroll; }
    bool IsSmartScroll()           const { return m_smartScroll; }
    int  GetTrackpadSensitivity()  const { return m_trackpadSensitivity; }
    int  GetScrollSensitivity()    const { return m_scrollSensitivity; }
    int  GetMouseDeadzone()        const { return m_mouseDeadzone; }
    int  GetScrollDeadzone()       const { return m_scrollDeadzone; }
    int  GetMouseDeadzoneRaw()     const;   // per-frame threshold in pad units
    int  GetScrollDeadzoneRaw()    const;
    int  GetLastMouseMove()        const { return m_trackpad.LastMouseMove(); }
    int  GetLastScrollMove()       const { return m_trackpad.LastScrollMove(); }
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
    void SetHapticIntensity(int pos);    // 1..100 (movement density, both pads)
    void SetHapticClickHardness(int level);  // 1..3 (soft/medium/hard)
    bool IsHapticOnClick()    const { return m_hapticOnClick; }
    bool IsHapticOnMove()     const { return m_hapticOnMove; }
    int  GetHapticIntensity() const { return m_hapticIntensity; }
    int  GetHapticClickHardness() const { return m_clickHardness; }

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

    // True when on USB cable power (wired) rather than the wireless dongle.
    bool IsCharging() const;

private:
    void TryOpen();
    void Close(bool restoreLizard);
    void ApplyStickConfigToVirtual();
    void ApplyPadRoles();   // push the pad roles to the trackpad + virtual controller
    void StartReadLoop();
    void StopReadLoop();
    void ReadLoop();

    StateChangedFn                     m_onStateChanged;
    KeyboardSetOpenFn                  m_onKeyboardSetOpen;
    std::atomic<bool>                  m_keyboardMode{false};
    bool                               m_kbWantOpen   = false;
    bool                               m_prevKbCombo  = false;
    int                                m_kbOpenButton   = 2;    // X
    int                                m_kbOpenModifier = 10;   // Steam (-1 = none)
    bool                               m_kbOpenHold     = false;
    int                                m_kbClickL       = -1;   // source index, -1 = none
    int                                m_kbClickR       = -1;
    bool                               m_kbUsePadClick  = true;
    bool                               m_kbSplit        = true;
    bool                               m_kbRelative     = false;
    int                                m_kbRelSens      = 50;   // 1..100 (relative slide speed)
    bool                               m_kbBall         = false;
    int                                m_kbLayout       = 1;    // 0 = Simple, 1 = ISO
    int                                m_kbKeyBack      = -1;   // shortcut: Backspace
    int                                m_kbKeySpace     = -1;   // shortcut: Space
    int                                m_kbKeyEnter     = -1;   // shortcut: Enter
    std::atomic<bool>                  m_connected{false};
    std::atomic<bool>                  m_gameModeActive{false};
    PadRole                            m_padRoleRight         = PadRole::Mouse;
    PadRole                            m_padRoleLeft          = PadRole::Scroll;
    bool                               m_dpadWASD             = false;
    bool                               m_dpadSingle           = false;
    bool                               m_dpadOnClick          = false;
    bool                               m_btnOnClick           = false;
    bool                               m_btnSwap              = false;
    int                                m_padStickDz           = 10;    // 0..90 (%)
    bool                               m_invertScroll         = false;
    bool                               m_smartScroll          = false;
    std::atomic<bool>                  m_trackpadSuspended{false};
    bool                               m_autoEnable           = false;
    bool                               m_hapticOnClick        = false;
    bool                               m_hapticOnMove         = false;
    int                                m_hapticIntensity      = 50;   // 1..100 (50 = baseline)
    int                                m_clickHardness        = 2;    // 1..3 (soft/medium/hard)
    int                                m_trackpadSensitivity  = 35;   // 1..100
    int                                m_scrollSensitivity    = 30;   // 1..100
    int                                m_mouseDeadzone        = 50;   // 1..100 (slider pos)
    int                                m_scrollDeadzone       = 50;   // 1..100 (slider pos)
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
    std::atomic<int>                   m_batteryPercent{-1};   // from 0x43 reports
};
