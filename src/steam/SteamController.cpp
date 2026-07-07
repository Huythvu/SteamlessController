#include "SteamController.h"
#include <chrono>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Internal helper: build a 64-byte feature report command buffer.
//
// The 2026 Steam Controller routes firmware commands through Feature Report
// 0x01 (same channel the original SC used, now with an explicit report ID).
//
// Buffer layout:
//   [0] FEATURE_REPORT_CMD (0x01)  — HID feature report ID
//   [1] cmd                         — command byte (0x81, 0x87, etc.)
//   [2] payloadSize                 — number of payload bytes that follow
//   [3..3+payloadSize-1] payload    — command arguments
//   [rest] zeros
// ---------------------------------------------------------------------------

static void BuildCmd(uint8_t (&buf)[64], uint8_t cmd,
                     const uint8_t* payload = nullptr, uint8_t payloadSize = 0) {
    std::memset(buf, 0, 64);
    buf[0] = SteamController::FEATURE_REPORT_CMD;
    buf[1] = cmd;
    buf[2] = payloadSize;
    if (payload && payloadSize)
        std::memcpy(buf + 3, payload, payloadSize);
}

// ---------------------------------------------------------------------------
// Open / Close
// ---------------------------------------------------------------------------

bool SteamController::Open() {
    for (uint16_t pid : { SC2026_PID, SC2026_DONGLE_PID }) {
        auto paths = HidDevice::Enumerate(VALVE_VID, pid, VENDOR_USAGE_PAGE);
        if (paths.empty()) continue;

        // For the wired controller there is only one interface; for the dongle
        // there are up to four slots (one per paired controller). Try each in
        // order and use the first that produces a live input report.
        for (auto const& path : paths) {
            if (!m_device.Open(path)) continue;

            uint8_t buf[64];
            size_t n = m_device.ReadInputReport(buf, sizeof(buf), /*timeoutMs=*/500);
            if (n > 0 && buf[0] == REPORT_STATE) {
                printf("Active interface found for PID=%04X.\n", pid);
                m_pid = pid;
                return true;
            }

            if (n > 0)
                printf("Unexpected report ID 0x%02X on PID=%04X (expected 0x%02X) — possible firmware mismatch.\n",
                       buf[0], pid, REPORT_STATE);
            else
                printf("Read timeout on PID=%04X vendor interface — no active controller on this slot.\n", pid);

            m_device.Close();
        }
    }

    // Fallback: a Steam/firmware update may have changed the PID. Try any other
    // Valve vendor-usage interface that still emits the expected state report,
    // so a PID bump alone doesn't break detection.
    for (const auto& d : HidDevice::EnumerateInfo(VALVE_VID)) {
        if (d.usagePage != VENDOR_USAGE_PAGE) continue;
        if (d.pid == SC2026_PID || d.pid == SC2026_DONGLE_PID) continue;  // already tried
        if (!m_device.Open(d.path)) continue;
        uint8_t buf[64];
        size_t n = m_device.ReadInputReport(buf, sizeof(buf), /*timeoutMs=*/500);
        if (n > 0 && buf[0] == REPORT_STATE) {
            printf("Active interface found for unlisted PID=%04X (firmware update?).\n", d.pid);
            m_pid = d.pid;
            return true;
        }
        m_device.Close();
    }

    printf("No Steam Controller found (wired PID=%04X or dongle PID=%04X).\n",
           SC2026_PID, SC2026_DONGLE_PID);
    return false;
}

static std::string NarrowAscii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c > 0 && c < 128) ? static_cast<char>(c) : '?';
    return s;
}
static bool IContains(const std::wstring& hay, const wchar_t* needle) {
    std::wstring h = hay;
    for (wchar_t& c : h) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
    return h.find(needle) != std::wstring::npos;
}

std::string SteamController::Diagnostics() {
    std::string s;
    char line[512];
    auto all = HidDevice::EnumerateInfo(0);   // every HID device on the system

    // Anything that looks like a Steam/Valve device, by VID or by name -- this
    // also catches a VID change from a firmware/Steam update.
    std::vector<const HidDevice::Info*> valve;
    for (const auto& d : all) {
        const bool named = IContains(d.product, L"steam") || IContains(d.product, L"valve")
                        || IContains(d.manufacturer, L"valve") || IContains(d.manufacturer, L"steam");
        if (d.vid == VALVE_VID || named) valve.push_back(&d);
    }

    if (valve.empty()) {
        std::snprintf(line, sizeof(line),
            "No Steam/Valve HID device found (scanned %d HID devices).\n",
            static_cast<int>(all.size()));
        s += line;
        s += "- Windows isn't seeing the controller as an HID device.\n";
        s += "- Check Device Manager (Human Interface Devices) with it plugged in.\n";
        s += "- Try another cable/port; a driver or Steam update may have changed it.\n";
        return s;
    }

    std::snprintf(line, sizeof(line), "Steam/Valve HID interfaces (%d):\n",
                  static_cast<int>(valve.size()));
    s += line;
    for (const auto* d : valve) {
        std::snprintf(line, sizeof(line),
            "  VID=%04X PID=%04X UsagePage=%04X Usage=%02X  \"%s\"%s\n",
            d->vid, d->pid, d->usagePage, d->usage, NarrowAscii(d->product).c_str(),
            d->usagePage == VENDOR_USAGE_PAGE ? "  (vendor input)" : "");
        s += line;
    }
    for (const auto* d : valve) {
        if (d->usagePage != VENDOR_USAGE_PAGE) continue;
        HidDevice dev;
        if (!dev.Open(d->path)) {
            std::snprintf(line, sizeof(line), "  PID=%04X vendor: could not open (in use?)\n", d->pid);
            s += line; continue;
        }
        uint8_t buf[64];
        size_t n = dev.ReadInputReport(buf, sizeof(buf), /*timeoutMs=*/350);
        if (n > 0)
            std::snprintf(line, sizeof(line),
                          "  PID=%04X vendor: report id=0x%02X, %d bytes (app wants 0x%02X)\n",
                          d->pid, buf[0], static_cast<int>(n), REPORT_STATE);
        else
            std::snprintf(line, sizeof(line),
                          "  PID=%04X vendor: no report in 350ms (Steam holding it?)\n", d->pid);
        s += line;
        dev.Close();
    }
    std::snprintf(line, sizeof(line),
                  "\nApp expects: VID 28DE, PID 1302/1304, UsagePage FF00, report 0x%02X.\n",
                  REPORT_STATE);
    s += line;
    s += "If the VID / PID / UsagePage / report id above differ, that's the change.\n";
    return s;
}

void SteamController::Close() {
    if (m_running.exchange(false) && m_heartbeat.joinable())
        m_heartbeat.join();
    m_device.Close();
    m_pid = 0;
}

// ---------------------------------------------------------------------------
// Lizard mode
// ---------------------------------------------------------------------------

bool SteamController::DisableLizardMode() {
    uint8_t buf[64];

    // Step 1: CLEAR_DIGITAL_MAPPINGS — kills keyboard/mouse button emulation.
    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);
    if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
        printf("Failed to send CLEAR_DIGITAL_MAPPINGS.\n");
        return false;
    }

    // Step 2: SET_SETTINGS — set both trackpads to TRACKPAD_NONE.
    // Payload: pairs of [setting_id, val_lo, val_hi].
    const uint8_t settingsPayload[] = {
        SETTING_LEFT_TRACKPAD_MODE,  0x00, 0x00,
        SETTING_RIGHT_TRACKPAD_MODE, 0x00, 0x00,
    };
    BuildCmd(buf, CMD_SET_SETTINGS, settingsPayload, sizeof(settingsPayload));
    if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
        printf("Failed to send SET_SETTINGS_VALUES.\n");
        return false;
    }

    if (!m_running.exchange(true))
        m_heartbeat = std::thread(&SteamController::HeartbeatLoop, this);

    return true;
}

bool SteamController::EnableLizardMode() {
    if (m_running.exchange(false) && m_heartbeat.joinable())
        m_heartbeat.join();

    uint8_t buf[64];
    BuildCmd(buf, CMD_SET_DEFAULT_MAPPINGS);
    return m_device.SendFeatureReport(buf, sizeof(buf));
}

// ---------------------------------------------------------------------------
// Haptics (experimental)
// ---------------------------------------------------------------------------

void SteamController::TrackpadHaptic(uint8_t side, uint16_t amplitude, uint8_t count) {
    // Output report 0x81: [id, side, amp_lo, amp_hi, 0, 0, count, 0]. The count
    // byte repeats the pulse -- more pulses make a firmer-feeling click.
    if (count < 1) count = 1;
    const uint8_t report[8] = {
        0x81, side,
        static_cast<uint8_t>(amplitude & 0xFF), static_cast<uint8_t>(amplitude >> 8),
        0x00, 0x00, count, 0x00,
    };
    m_device.WriteOutputReport(report, sizeof(report));
}

void SteamController::RumbleHaptic(uint8_t side, uint8_t intensity) {
    // Output report 0x82: [id, side, 0x01, intensity]
    const uint8_t report[4] = { 0x82, side, 0x01, intensity };
    m_device.WriteOutputReport(report, sizeof(report));
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

size_t SteamController::ReadReport(uint8_t* buffer, size_t size, uint32_t timeoutMs,
                                  bool* deviceLost) {
    return m_device.ReadInputReport(buffer, size, timeoutMs, deviceLost);
}

bool SteamController::IsStillConnected() const {
    // Re-enumerate: if neither the wired controller nor the dongle is present,
    // the device is gone. A live Windows file handle stays "valid" after an
    // unplug, so we can't rely on IsOpen() to detect removal.
    for (uint16_t pid : { SC2026_PID, SC2026_DONGLE_PID }) {
        if (!HidDevice::Enumerate(VALVE_VID, pid, VENDOR_USAGE_PAGE).empty())
            return true;
    }
    return false;
}


// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

void SteamController::HeartbeatLoop() {
    uint8_t buf[64];
    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);

    while (m_running.load()) {
        m_device.SendFeatureReport(buf, sizeof(buf));
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
}
