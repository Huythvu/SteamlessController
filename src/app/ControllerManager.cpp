#include "ControllerManager.h"
#include "VirtualController.h"
#include "steam/SteamController.h"
#include <memory>

static std::unique_ptr<SteamController> g_ctrl;

// Map a 1..100 slider position to a usable speed. Kept deliberately modest at
// the top end — the previous 0.1 max was far too fast to be usable.
static float MouseSensFromPos(int pos) {
    return 0.002f + (pos - 1) / 99.0f * (0.040f - 0.002f);
}
static float ScrollSensFromPos(int pos) {
    return 0.010f + (pos - 1) / 99.0f * (0.200f - 0.010f);
}

ControllerManager::ControllerManager(StateChangedFn onStateChanged)
    : m_onStateChanged(std::move(onStateChanged))
{
    TryOpen();
}

ControllerManager::~ControllerManager() {
    Close(/*restoreLizard=*/true);
}

void ControllerManager::OnDeviceChange() {
    if (!m_connected)
        TryOpen();
    else if (!g_ctrl || !g_ctrl->IsStillConnected())
        Close(/*restoreLizard=*/false);
}

void ControllerManager::EnableGameMode() {
    if (!m_connected || m_gameModeActive) return;
    if (!g_ctrl->DisableLizardMode()) return;

    m_virtual = std::make_unique<VirtualController>();
    if (!m_virtual->IsValid()) {
        bool missing = m_virtual->IsDriverMissing();
        m_virtual.reset();
        g_ctrl->EnableLizardMode();
        if (missing) m_onStateChanged(m_connected, m_gameModeActive, /*vigemMissing=*/true);
        return;
    }

    m_gameModeActive = true;
    m_trackpad.Reset();
    m_trackpad.SetTrackpadEnabled(m_trackpadMouseEnabled);
    m_trackpad.SetBackButtonsEnabled(m_backButtonsEnabled);
    m_trackpad.SetUseLeftTrackpad(m_useLeftTrackpad);
    m_trackpad.SetScrollEnabled(m_scrollWheelEnabled);
    m_trackpad.SetInvertScroll(m_invertScroll);
    m_trackpad.SetSensitivity(MouseSensFromPos(m_trackpadSensitivity));
    m_trackpad.SetScrollSensitivity(ScrollSensFromPos(m_scrollSensitivity));
    StartReadLoop();
    m_onStateChanged(m_connected, m_gameModeActive, false);
}

void ControllerManager::DisableGameMode() {
    if (!m_gameModeActive) return;
    StopReadLoop();
    m_trackpad.Reset();
    m_virtual.reset();
    g_ctrl->EnableLizardMode();
    m_gameModeActive = false;
    m_onStateChanged(m_connected, m_gameModeActive, false);
}

void ControllerManager::SetTrackpadMouseEnabled(bool enabled) {
    m_trackpadMouseEnabled = enabled;
    m_trackpad.SetTrackpadEnabled(enabled);
}

void ControllerManager::SetBackButtonsEnabled(bool enabled) {
    m_backButtonsEnabled = enabled;
    m_trackpad.SetBackButtonsEnabled(enabled);
}

void ControllerManager::SetUseLeftTrackpad(bool enabled) {
    m_useLeftTrackpad = enabled;
    m_trackpad.SetUseLeftTrackpad(enabled);
}

void ControllerManager::SetScrollWheelEnabled(bool enabled) {
    m_scrollWheelEnabled = enabled;
    m_trackpad.SetScrollEnabled(enabled);
}

void ControllerManager::SetInvertScroll(bool enabled) {
    m_invertScroll = enabled;
    m_trackpad.SetInvertScroll(enabled);
}

void ControllerManager::SetTrackpadSensitivity(int pos) {
    if (pos < 1)   pos = 1;
    if (pos > 100) pos = 100;
    m_trackpadSensitivity = pos;
    m_trackpad.SetSensitivity(MouseSensFromPos(pos));
}

void ControllerManager::SetScrollSensitivity(int pos) {
    if (pos < 1)   pos = 1;
    if (pos > 100) pos = 100;
    m_scrollSensitivity = pos;
    m_trackpad.SetScrollSensitivity(ScrollSensFromPos(pos));
}

void ControllerManager::TryOpen() {
    if (!g_ctrl) g_ctrl = std::make_unique<SteamController>();
    if (g_ctrl->Open()) {
        m_connected = true;
        m_onStateChanged(m_connected, m_gameModeActive, false);
    }
}

void ControllerManager::Close(bool restoreLizard) {
    StopReadLoop();
    m_virtual.reset();
    if (g_ctrl) {
        if (restoreLizard && m_gameModeActive)
            g_ctrl->EnableLizardMode();
        g_ctrl->Close();
    }
    m_connected      = false;
    m_gameModeActive = false;
    m_onStateChanged(m_connected, m_gameModeActive, false);
}

void ControllerManager::StartReadLoop() {
    m_readRunning = true;
    m_readThread  = std::thread(&ControllerManager::ReadLoop, this);
}

void ControllerManager::StopReadLoop() {
    m_readRunning = false;
    if (m_readThread.joinable())
        m_readThread.join();
}

void ControllerManager::ReadLoop() {
    uint8_t buf[64];
    while (m_readRunning) {
        bool deviceLost = false;
        size_t n = g_ctrl->ReadReport(buf, sizeof(buf), /*timeoutMs=*/32, &deviceLost);
        // Controller was unplugged: stop reading so we don't spin on a dead
        // handle. The WM_DEVICECHANGE handler tears the connection down.
        if (deviceLost) break;
        if (n == 0) continue;
        if (buf[0] != SteamController::REPORT_STATE) continue;
        if (m_virtual) m_virtual->Update(buf, n);
        m_trackpad.Update(buf, n);
    }
}
