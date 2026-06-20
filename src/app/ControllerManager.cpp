#include "ControllerManager.h"
#include "VirtualController.h"
#include "steam/SteamController.h"
#include <memory>
#include <cstring>
#include <cmath>

static std::unique_ptr<SteamController> g_ctrl;

// Map a 1..100 slider position to a usable speed. Kept deliberately modest at
// the top end — the previous 0.1 max was far too fast to be usable.
static float MouseSensFromPos(int pos) {
    return 0.002f + (pos - 1) / 99.0f * (0.040f - 0.002f);
}
static float ScrollSensFromPos(int pos) {
    return 0.010f + (pos - 1) / 99.0f * (0.200f - 0.010f);
}

// Haptic "density": 1..100 where 50 = baseline spacing. Higher pos = denser
// (shorter distance between ticks), lower = sparser. baseline * 2^((50-pos)/50).
static float MoveTickFromPos(int pos) {
    return 8000.0f * std::pow(2.0f, (50 - pos) / 50.0f);
}

// Deadzone: a 1..100 slider where 50 = baseline, 100 = 2x, 1 ~= 0.5x.
static int MouseDzFromPos(int pos) {
    return static_cast<int>(120.0f * std::pow(2.0f, (pos - 50) / 50.0f));
}
static int ScrollDzFromPos(int pos) {
    return static_cast<int>(600.0f * std::pow(2.0f, (pos - 50) / 50.0f));
}

// Map a 1..100 stick-sensitivity position to a response-curve exponent:
// 50 = linear, lower = gentler near center, higher = more aggressive.
static float StickExpFromPos(int pos) {
    if (pos <= 50) return 2.0f + (pos - 1)  / 49.0f * (1.0f - 2.0f);  // 1→2.0, 50→1.0
    return                1.0f + (pos - 50) / 50.0f * (0.4f - 1.0f);  // 50→1.0, 100→0.4
}

ControllerManager::ControllerManager(StateChangedFn onStateChanged)
    : m_onStateChanged(std::move(onStateChanged))
{
    // Route trackpad haptic pulses to the physical controller.
    m_trackpad.SetHapticSink([](uint8_t side, uint16_t amp, uint8_t count) {
        if (g_ctrl) g_ctrl->TrackpadHaptic(side, amp, count);
    });
    TryOpen();
}

ControllerManager::~ControllerManager() {
    Close(/*restoreLizard=*/true);
}

void ControllerManager::OnDeviceChange() {
    if (!m_connected.load())
        TryOpen();
    else if (!g_ctrl || !g_ctrl->IsStillConnected())
        Close(/*restoreLizard=*/false);
}

void ControllerManager::EnableGameMode() {
    if (!m_connected.load() || m_gameModeActive.load()) return;
    if (!g_ctrl->DisableLizardMode()) return;

    m_virtual = std::make_unique<VirtualController>();
    if (!m_virtual->IsValid()) {
        bool missing = m_virtual->IsDriverMissing();
        m_virtual.reset();
        g_ctrl->EnableLizardMode();
        if (missing) m_onStateChanged(m_connected.load(), m_gameModeActive.load(), /*vigemMissing=*/true);
        return;
    }
    ApplyStickConfigToVirtual();

    m_gameModeActive = true;
    {
        std::lock_guard<std::mutex> lock(m_inputMutex);
        const bool susp = m_trackpadSuspended.load();
        m_trackpad.Reset();
        m_trackpad.SetTrackpadEnabled(m_trackpadMouseEnabled && !susp);
        m_trackpad.SetBackButtonsEnabled(m_backButtonsEnabled);
        m_trackpad.SetUseLeftTrackpad(m_useLeftTrackpad);
        m_trackpad.SetScrollEnabled(m_scrollWheelEnabled && !susp);
        m_trackpad.SetInvertScroll(m_invertScroll);
        m_trackpad.SetSensitivity(MouseSensFromPos(m_trackpadSensitivity));
        m_trackpad.SetScrollSensitivity(ScrollSensFromPos(m_scrollSensitivity));
        m_trackpad.SetMouseDeadzone(MouseDzFromPos(m_mouseDeadzone));
        m_trackpad.SetScrollDeadzone(ScrollDzFromPos(m_scrollDeadzone));
        m_trackpad.SetHapticOnClick(m_hapticOnClick);
        m_trackpad.SetHapticOnMove(m_hapticOnMove);
        m_trackpad.SetClickHardness(m_clickHardness);
        m_trackpad.SetMoveTickDistance(MoveTickFromPos(m_hapticIntensity));
    }
    StartReadLoop();
    m_onStateChanged(m_connected.load(), m_gameModeActive.load(), false);
}

void ControllerManager::DisableGameMode() {
    if (!m_gameModeActive.load()) return;
    StopReadLoop();
    {
        std::lock_guard<std::mutex> lock(m_inputMutex);
        m_trackpad.Reset();
        m_mapper.ReleaseKeys();
        m_virtual.reset();
    }
    g_ctrl->EnableLizardMode();
    m_gameModeActive = false;
    m_onStateChanged(m_connected.load(), m_gameModeActive.load(), false);
}

void ControllerManager::SetTrackpadMouseEnabled(bool enabled) {
    m_trackpadMouseEnabled = enabled;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    if (!enabled) m_trackpad.Reset();
    m_trackpad.SetTrackpadEnabled(enabled && !m_trackpadSuspended.load());
}

void ControllerManager::SetBackButtonsEnabled(bool enabled) {
    m_backButtonsEnabled = enabled;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    if (!enabled) m_trackpad.Reset();
    m_trackpad.SetBackButtonsEnabled(enabled);
}

void ControllerManager::SetUseLeftTrackpad(bool enabled) {
    m_useLeftTrackpad = enabled;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_trackpad.Reset();
    m_trackpad.SetUseLeftTrackpad(enabled);
}

void ControllerManager::SetScrollWheelEnabled(bool enabled) {
    m_scrollWheelEnabled = enabled;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    if (!enabled) m_trackpad.Reset();
    m_trackpad.SetScrollEnabled(enabled && !m_trackpadSuspended.load());
}

void ControllerManager::SetHapticOnClick(bool enabled) {
    m_hapticOnClick = enabled;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_trackpad.SetHapticOnClick(enabled);
}

void ControllerManager::SetHapticOnMove(bool enabled) {
    m_hapticOnMove = enabled;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_trackpad.SetHapticOnMove(enabled);
}

void ControllerManager::SetHapticIntensity(int pos) {
    if (pos < 1)   pos = 1;
    if (pos > 100) pos = 100;
    m_hapticIntensity = pos;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_trackpad.SetMoveTickDistance(MoveTickFromPos(pos));
}

void ControllerManager::SetHapticClickHardness(int level) {
    if (level < 1) level = 1;
    if (level > 3) level = 3;
    m_clickHardness = level;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_trackpad.SetClickHardness(level);
}

void ControllerManager::TestHaptic() {
    if (!g_ctrl || !m_connected.load()) return;
    // Replay the patterns captured from Steam: trackpad actuators (0x81) and
    // rumble motors (0x82), both sides, so we can tell which fires.
    g_ctrl->TrackpadHaptic(0, 0x0190);
    g_ctrl->TrackpadHaptic(1, 0x0190);
    g_ctrl->RumbleHaptic(0, 0xFD);
    g_ctrl->RumbleHaptic(1, 0xFD);
}

void ControllerManager::SuspendTrackpad(bool suspended) {
    m_trackpadSuspended.store(suspended);
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_trackpad.Reset();
    m_trackpad.SetTrackpadEnabled(m_trackpadMouseEnabled && !suspended);
    m_trackpad.SetScrollEnabled(m_scrollWheelEnabled && !suspended);
}

void ControllerManager::SetInvertScroll(bool enabled) {
    m_invertScroll = enabled;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_trackpad.SetInvertScroll(enabled);
}

void ControllerManager::SetTrackpadSensitivity(int pos) {
    if (pos < 1)   pos = 1;
    if (pos > 100) pos = 100;
    m_trackpadSensitivity = pos;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_trackpad.SetSensitivity(MouseSensFromPos(pos));
}

void ControllerManager::SetScrollSensitivity(int pos) {
    if (pos < 1)   pos = 1;
    if (pos > 100) pos = 100;
    m_scrollSensitivity = pos;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_trackpad.SetScrollSensitivity(ScrollSensFromPos(pos));
}

void ControllerManager::SetMouseDeadzone(int pos) {
    if (pos < 1)   pos = 1;
    if (pos > 100) pos = 100;
    m_mouseDeadzone = pos;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_trackpad.SetMouseDeadzone(MouseDzFromPos(pos));
}

void ControllerManager::SetScrollDeadzone(int pos) {
    if (pos < 1)   pos = 1;
    if (pos > 100) pos = 100;
    m_scrollDeadzone = pos;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_trackpad.SetScrollDeadzone(ScrollDzFromPos(pos));
}

void ControllerManager::SetLeftDeadzone(int pos) {
    if (pos < 0)  pos = 0;
    if (pos > 90) pos = 90;
    m_lDeadzone = pos;
    ApplyStickConfigToVirtual();
}

void ControllerManager::SetRightDeadzone(int pos) {
    if (pos < 0)  pos = 0;
    if (pos > 90) pos = 90;
    m_rDeadzone = pos;
    ApplyStickConfigToVirtual();
}

void ControllerManager::SetLeftStickSensitivity(int pos) {
    if (pos < 1)   pos = 1;
    if (pos > 100) pos = 100;
    m_lStickSens = pos;
    ApplyStickConfigToVirtual();
}

void ControllerManager::SetRightStickSensitivity(int pos) {
    if (pos < 1)   pos = 1;
    if (pos > 100) pos = 100;
    m_rStickSens = pos;
    ApplyStickConfigToVirtual();
}

void ControllerManager::ApplyStickConfigToVirtual() {
    std::lock_guard<std::mutex> lock(m_inputMutex);
    if (!m_virtual) return;
    m_virtual->SetStickConfig(m_lDeadzone / 100.0f, StickExpFromPos(m_lStickSens),
                              m_rDeadzone / 100.0f, StickExpFromPos(m_rStickSens));
}

void ControllerManager::TryOpen() {
    if (!g_ctrl) g_ctrl = std::make_unique<SteamController>();
    if (g_ctrl->Open()) {
        m_connected = true;
        m_onStateChanged(m_connected.load(), m_gameModeActive.load(), false);
        ApplyAutoEnable();
    }
}

void ControllerManager::ApplyAutoEnable() {
    if (m_autoEnable && m_connected.load() && !m_gameModeActive.load())
        EnableGameMode();
}

void ControllerManager::Close(bool restoreLizard) {
    StopReadLoop();
    {
        std::lock_guard<std::mutex> lock(m_inputMutex);
        m_trackpad.Reset();
        m_mapper.ReleaseKeys();
        m_virtual.reset();
    }
    if (g_ctrl) {
        if (restoreLizard && m_gameModeActive.load())
            g_ctrl->EnableLizardMode();
        g_ctrl->Close();
    }
    m_connected      = false;
    m_gameModeActive = false;
    m_onStateChanged(m_connected.load(), m_gameModeActive.load(), false);
}

void ControllerManager::StartReadLoop() {
    if (m_readThread.joinable())
        m_readThread.join();
    m_readRunning = true;
    m_readThread  = std::thread(&ControllerManager::ReadLoop, this);
}

void ControllerManager::StopReadLoop() {
    m_readRunning = false;
    if (m_readThread.joinable())
        m_readThread.join();
    // Drop the snapshot so the live monitor stops showing a frozen frame.
    std::lock_guard<std::mutex> lock(m_reportMutex);
    m_lastReportLen = 0;
}

void ControllerManager::ReadLoop() {
    uint8_t buf[64];
    bool lost = false;
    while (m_readRunning) {
        bool deviceLost = false;
        size_t n = g_ctrl->ReadReport(buf, sizeof(buf), /*timeoutMs=*/32, &deviceLost);
        // Controller was unplugged: stop reading so we don't spin on a dead
        // handle. Tear down the virtual device here as a fallback for wireless
        // dongles that remain present after the controller slot disappears.
        if (deviceLost) { lost = true; break; }
        if (n == 0) continue;
        // The secondary (0x43) report carries battery percent in byte 2.
        if (buf[0] == SteamController::REPORT_SECONDARY && n >= 3) {
            m_batteryPercent.store(buf[2] > 100 ? 100 : buf[2]);
            continue;
        }
        if (buf[0] != SteamController::REPORT_STATE) continue;
        {
            std::lock_guard<std::mutex> lock(m_inputMutex);
            uint16_t buttonBits = m_mapper.Process(buf, n);
            if (m_virtual) m_virtual->Update(buf, n, buttonBits);
            m_trackpad.Update(buf, n);
        }

        // Publish a snapshot for the live input monitor.
        {
            std::lock_guard<std::mutex> lock(m_reportMutex);
            std::memcpy(m_lastReport, buf, n);
            m_lastReportLen = n;
        }
    }

    if (lost) {
        m_readRunning = false;
        {
            std::lock_guard<std::mutex> lock(m_inputMutex);
            m_trackpad.Reset();
            m_mapper.ReleaseKeys();
            m_virtual.reset();
        }
        if (g_ctrl) g_ctrl->Close();
        m_connected = false;
        m_gameModeActive = false;
        m_onStateChanged(false, false, false);
    }
}

size_t ControllerManager::GetLatestReport(uint8_t* out, size_t outSize) const {
    std::lock_guard<std::mutex> lock(m_reportMutex);
    size_t len = m_lastReportLen < outSize ? m_lastReportLen : outSize;
    std::memcpy(out, m_lastReport, len);
    return len;
}

int ControllerManager::GetBatteryPercent() const {
    return m_batteryPercent.load();   // -1 until a 0x43 report arrives
}

bool ControllerManager::IsCharging() const {
    return m_connected.load() && g_ctrl && g_ctrl->IsWired();
}

void ControllerManager::SetButtonAction(int sourceIndex, InputMapper::Action a) {
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_mapper.SetAction(sourceIndex, a);
}

InputMapper::Action ControllerManager::GetButtonAction(int sourceIndex) const {
    std::lock_guard<std::mutex> lock(m_inputMutex);
    return m_mapper.GetAction(sourceIndex);
}

void ControllerManager::ResetButtonMappings() {
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_mapper.ReleaseKeys();
    m_mapper.ResetToDefaults();
}
