#pragma once
#include "hid/HidDevice.h"
#include <atomic>
#include <cstdint>
#include <thread>

class SteamController {
public:
    static constexpr uint16_t VALVE_VID        = 0x28DE;
    static constexpr uint16_t SC2026_PID       = 0x1302;  // wired USB
    static constexpr uint16_t SC2026_DONGLE_PID = 0x1304; // wireless dongle ("Steam Controller Puck")

    // HID Usage Page for the vendor collection that carries all game input.
    static constexpr uint16_t VENDOR_USAGE_PAGE = 0xFF00;

    // Input report IDs (device → host)
    // The main state report has shipped under two IDs across firmware revisions:
    // 0x45 (a 2025 firmware) and 0x42 (the original, and again after a 2026
    // update). The byte layout is identical; only the leading report ID differs,
    // so we accept either. Use IsStateReport() rather than comparing to one value.
    static constexpr uint8_t REPORT_STATE         = 0x45;  // main controller state
    static constexpr uint8_t REPORT_STATE_ALT      = 0x42;  // same layout, alternate firmware ID
    static constexpr uint8_t REPORT_SECONDARY      = 0x43;  // 14 bytes: gyro / secondary state
    static constexpr uint8_t REPORT_STATUS         = 0x44;  //  5 bytes: battery / connection
    static constexpr uint8_t REPORT_UNKNOWN_7B     = 0x7B;  // 12 bytes: TBD
    static constexpr uint8_t REPORT_UNKNOWN_79     = 0x79;  //  1 byte:  TBD

    // Smallest byte count we'll treat as a "big" (state-sized) report. The real
    // state report is ~53-54 bytes; every other known report is <= 14 bytes, so
    // this cleanly separates them with margin on both sides.
    static constexpr size_t STATE_REPORT_MIN_LEN = 40;

    // True for a report ID we already know carries the main controller state.
    static constexpr bool IsStateReport(uint8_t id) {
        return id == REPORT_STATE || id == REPORT_STATE_ALT;
    }

    // Same, but future-proofed against a firmware update that only renumbers the
    // report (as happened when it moved 0x42 <-> 0x45). If the ID is unknown but
    // the report is state-sized AND isn't one of the known small reports, treat
    // it as state too, so a pure renumber doesn't break detection again.
    static constexpr bool IsStateReport(uint8_t id, size_t len) {
        if (IsStateReport(id)) return true;
        if (len < STATE_REPORT_MIN_LEN) return false;
        return id != REPORT_SECONDARY && id != REPORT_STATUS
            && id != REPORT_UNKNOWN_7B && id != REPORT_UNKNOWN_79;
    }

    // Feature report IDs — the command channel to the firmware.
    // Commands are wrapped inside Feature Report 0x01 (or 0x02 as fallback).
    // Buffer layout: [feature_report_id | cmd_byte | payload_size | payload...]
    static constexpr uint8_t FEATURE_REPORT_CMD  = 0x01;
    static constexpr uint8_t FEATURE_REPORT_CMD2 = 0x02;  // fallback if 0x01 fails

    // Command bytes (go in buffer[1] inside the feature report)
    static constexpr uint8_t CMD_SET_DIGITAL_MAPPINGS   = 0x80;
    static constexpr uint8_t CMD_CLEAR_DIGITAL_MAPPINGS = 0x81;  // ← lizard off
    static constexpr uint8_t CMD_GET_DIGITAL_MAPPINGS   = 0x82;
    static constexpr uint8_t CMD_SET_DEFAULT_MAPPINGS   = 0x85;  // ← lizard on
    static constexpr uint8_t CMD_SET_SETTINGS           = 0x87;
    static constexpr uint8_t CMD_GET_SETTINGS           = 0x89;
    static constexpr uint8_t CMD_TRIGGER_HAPTIC         = 0x8F;  // legacy SC haptic pulse

    // Setting key IDs (go in the payload of CMD_SET_SETTINGS)
    static constexpr uint8_t SETTING_RIGHT_TRACKPAD_MODE = 0x07;
    static constexpr uint8_t SETTING_LEFT_TRACKPAD_MODE  = 0x08;
    static constexpr uint8_t TRACKPAD_NONE               = 0x00;

    // ---------------------------------------------------------------------------
    // Input report layout — STATE report (buf[0] = 0x45 or 0x42; same layout)
    // ---------------------------------------------------------------------------

    // buf[01]       — 8-bit sequence counter (wraps 0xFF → 0x00)

    // buf[02]       — button bitmask byte 0
    static constexpr uint8_t BTN_A        = 0x01;  // bit 0
    static constexpr uint8_t BTN_B        = 0x02;  // bit 1
    static constexpr uint8_t BTN_X        = 0x04;  // bit 2
    static constexpr uint8_t BTN_Y        = 0x08;  // bit 3
    // bit 4 (0x10): TBD
    static constexpr uint8_t BTN_RS       = 0x20;  // bit 5 — right stick click
    static constexpr uint8_t BTN_MENU     = 0x40;  // bit 6 — ≡ Menu / Start
    static constexpr uint8_t BTN_R4       = 0x80;  // bit 7 — back paddle R4

    // buf[03]       — button bitmask byte 1
    static constexpr uint8_t BTN_R5       = 0x01;  // bit 0 — back paddle R5
    static constexpr uint8_t BTN_RB       = 0x02;  // bit 1
    static constexpr uint8_t BTN_DPAD_DN  = 0x04;  // bit 2
    static constexpr uint8_t BTN_DPAD_RT  = 0x08;  // bit 3
    static constexpr uint8_t BTN_DPAD_LT  = 0x10;  // bit 4
    static constexpr uint8_t BTN_DPAD_UP  = 0x20;  // bit 5
    static constexpr uint8_t BTN_VIEW     = 0x40;  // bit 6 — ⧉ View / Back
    static constexpr uint8_t BTN_LS       = 0x80;  // bit 7 — left stick click

    // buf[04]       — button bitmask byte 2
    static constexpr uint8_t BTN_STEAM    = 0x01;  // bit 0 — Steam / Guide
    static constexpr uint8_t BTN_L4       = 0x02;  // bit 1 — back paddle L4
    static constexpr uint8_t BTN_L5       = 0x04;  // bit 2 — back paddle L5
    static constexpr uint8_t BTN_LB       = 0x08;  // bit 3
    static constexpr uint8_t BTN_RS_TOUCH  = 0x10;  // bit 4 — right stick capacitive touch
    static constexpr uint8_t BTN_TP_RT    = 0x20;  // bit 5 — right trackpad active (touch or click)
    // bit 6 (0x40): TBD — possibly right trackpad physical click (hard press only)
    static constexpr uint8_t BTN_RT_FULL  = 0x80;  // bit 7 — right trigger fully pressed (digital threshold)

    // buf[05]       — flags byte
    static constexpr uint8_t BTN_LS_TOUCH    = 0x01;  // bit 0 — left stick capacitive touch
    static constexpr uint8_t BTN_TP_LT      = 0x02;  // bit 1 — left trackpad active (touch or click)
    static constexpr uint8_t BTN_TP_LT_CLICK = 0x04; // bit 2 — left trackpad hard press
    // bit 3 (0x08): TBD
    static constexpr uint8_t FLAG_GRIP_RT = 0x10;  // bit 4 — right grip sensor active
    static constexpr uint8_t FLAG_GRIP_LT = 0x20;  // bit 5 — left grip sensor active
    // other bits TBD

    // buf[06..07]   — left trigger,  16-bit LE signed, 0x0000 (released) – 0x7FFF (full)
    // buf[08..09]   — right trigger, 16-bit LE signed, 0x0000 (released) – 0x7FFF (full)

    // buf[10..11]   — left joystick X,  16-bit LE signed; center ≈ 0x0000, +0x7FFF = right, −0x8000 = left
    // buf[12..13]   — left joystick Y,  16-bit LE signed; center ≈ 0x0000, +0x7FFF = up, −0x8000 = down
    // buf[14..15]   — right joystick X, 16-bit LE signed; center ≈ 0x0000, +0x7FFF = right, −0x8000 = left
    // buf[16..17]   — right joystick Y, 16-bit LE signed; center ≈ 0x0000, +0x7FFF = up, −0x8000 = down

    // buf[18..19]   — left trackpad X, 16-bit LE signed (0x0000 when not touching)
    // buf[20..21]   — left trackpad Y, 16-bit LE signed (0x0000 when not touching)
    // buf[22..23]   — left trackpad contact area, 16-bit LE (0 = no contact; higher = more area/pressure)

    // buf[24..25]   — right trackpad X, 16-bit LE signed (0x0000 when not touching)
    // buf[26..27]   — right trackpad Y, 16-bit LE signed (0x0000 when not touching)
    // buf[28..29]   — right trackpad contact area, 16-bit LE (0 = no contact; higher = more area/pressure)

    // buf[30..31]   — 0x03 0x46 constant (firmware info?)
    // buf[32..39]   — IMU quaternion (4× 16-bit LE, little-endian)
    // buf[40..43]   — unknown / always zero
    // buf[44..45]   — 0xFF 0xFF at full charge (battery level)
    // buf[46..53]   — gyro at rest (constant when stationary)

    // ---------------------------------------------------------------------------

    SteamController() = default;
    ~SteamController() { Close(); }
    SteamController(const SteamController&) = delete;
    SteamController& operator=(const SteamController&) = delete;

    // Finds and opens the vendor HID interface. Returns false if not found.
    bool Open();
    void Close();
    bool IsOpen() const { return m_device.IsOpen(); }

    // Human-readable dump of the Valve HID interfaces present and the report
    // they emit, for troubleshooting a controller that won't connect (e.g.
    // after a Steam/firmware update changed the PID or report id).
    static std::string Diagnostics();

    // True when connected over the wired USB cable (i.e. drawing/charging on
    // USB power), false for the wireless dongle (running on battery).
    bool IsWired() const { return m_pid == SC2026_PID; }

    // Two-step sequence: clears digital mappings + sets trackpads to NONE.
    // Starts the background heartbeat thread on first call.
    bool DisableLizardMode();

    // Restores default mappings. Should be called before process exit.
    bool EnableLizardMode();

    // Haptics (output reports captured from Steam's own traffic).
    //   side: 0 / 1 selects the actuator (right / left).
    // Trackpad actuator pulse (report 0x81); rumble motor (report 0x82).
    void TrackpadHaptic(uint8_t side, uint16_t amplitude, uint8_t count = 1);
    void RumbleHaptic(uint8_t side, uint8_t intensity);

    // Read the next raw input report. buffer[0] = report ID on return.
    // Returns 0 on timeout. If the read fails because the controller was
    // disconnected, *deviceLost is set to true (when provided).
    size_t ReadReport(uint8_t* buffer, size_t size, uint32_t timeoutMs = 16,
                      bool* deviceLost = nullptr);

    // True if the controller (wired or dongle) is still physically present.
    bool IsStillConnected() const;

private:
    void HeartbeatLoop();

    HidDevice       m_device;
    std::thread     m_heartbeat;
    std::atomic<bool> m_running{false};
    uint16_t        m_pid = 0;     // PID of the connected device (0 = none)
};
