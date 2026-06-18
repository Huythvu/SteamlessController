#include "VirtualController.h"
#include "steam/SteamController.h"
#include <ViGEmClient.h>
#include <cstdio>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Radial deadzone + response curve for one analog stick.
// ---------------------------------------------------------------------------

static void ApplyStick(int16_t rx, int16_t ry, float dz, float expo,
                       SHORT& ox, SHORT& oy) {
    const float nx  = rx / 32767.0f;
    const float ny  = ry / 32767.0f;
    const float mag = std::sqrt(nx*nx + ny*ny);
    if (mag <= dz || mag <= 0.0f) { ox = 0; oy = 0; return; }

    const float clamped = mag > 1.0f ? 1.0f : mag;
    const float scaled  = (clamped - dz) / (1.0f - dz);   // re-map deadzone..1 → 0..1
    const float curved  = std::pow(scaled, expo);
    const float factor  = curved / mag;                   // preserve direction
    const float fx = std::clamp(nx * factor, -1.0f, 1.0f);
    const float fy = std::clamp(ny * factor, -1.0f, 1.0f);
    ox = static_cast<SHORT>(fx * 32767.0f);
    oy = static_cast<SHORT>(fy * 32767.0f);
}

// ---------------------------------------------------------------------------
// Report translation — 0x45 → XUSB_REPORT
// ---------------------------------------------------------------------------

static XUSB_REPORT Translate(const uint8_t* buf, size_t n,
                             float dzL, float expL, float dzR, float expR) {
    XUSB_REPORT r{};
    if (n < 18) return r;

    const uint8_t b0 = buf[2];
    const uint8_t b1 = buf[3];
    const uint8_t b2 = buf[4];

    // Face buttons
    if (b0 & SteamController::BTN_A) r.wButtons |= XUSB_GAMEPAD_A;
    if (b0 & SteamController::BTN_B) r.wButtons |= XUSB_GAMEPAD_B;
    if (b0 & SteamController::BTN_X) r.wButtons |= XUSB_GAMEPAD_X;
    if (b0 & SteamController::BTN_Y) r.wButtons |= XUSB_GAMEPAD_Y;

    // Bumpers
    if (b2 & SteamController::BTN_LB) r.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (b1 & SteamController::BTN_RB) r.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;

    // Menu / View (Start / Back)
    if (b0 & SteamController::BTN_MENU) r.wButtons |= XUSB_GAMEPAD_START;
    if (b1 & SteamController::BTN_VIEW) r.wButtons |= XUSB_GAMEPAD_BACK;

    // Stick clicks
    if (b1 & SteamController::BTN_LS) r.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
    if (b0 & SteamController::BTN_RS) r.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;

    // Steam / Guide button
    if (b2 & SteamController::BTN_STEAM) r.wButtons |= XUSB_GAMEPAD_GUIDE;

    // D-pad
    if (b1 & SteamController::BTN_DPAD_UP)  r.wButtons |= XUSB_GAMEPAD_DPAD_UP;
    if (b1 & SteamController::BTN_DPAD_DN)  r.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
    if (b1 & SteamController::BTN_DPAD_LT)  r.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (b1 & SteamController::BTN_DPAD_RT)  r.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;

    // Triggers: 16-bit signed (0x0000–0x7FFF) → 8-bit (0–255)
    int16_t ltRaw, rtRaw;
    memcpy(&ltRaw, buf + 6, 2);
    memcpy(&rtRaw, buf + 8, 2);
    r.bLeftTrigger  = static_cast<uint8_t>(std::clamp<int>(ltRaw >> 7, 0, 255));
    r.bRightTrigger = static_cast<uint8_t>(std::clamp<int>(rtRaw >> 7, 0, 255));

    // Sticks: 16-bit signed, same range as XInput. Apply deadzone + curve.
    int16_t lx, ly, rx, ry;
    memcpy(&lx, buf + 10, 2);
    memcpy(&ly, buf + 12, 2);
    memcpy(&rx, buf + 14, 2);
    memcpy(&ry, buf + 16, 2);
    ApplyStick(lx, ly, dzL, expL, r.sThumbLX, r.sThumbLY);
    ApplyStick(rx, ry, dzR, expR, r.sThumbRX, r.sThumbRY);

    return r;
}

// ---------------------------------------------------------------------------
// VirtualController
// ---------------------------------------------------------------------------

VirtualController::VirtualController() {
    m_client = vigem_alloc();
    if (!m_client) { printf("[ViGEm] alloc failed\n"); return; }

    VIGEM_ERROR err = vigem_connect(static_cast<PVIGEM_CLIENT>(m_client));
    if (!VIGEM_SUCCESS(err)) {
        if (err == VIGEM_ERROR_BUS_NOT_FOUND || err == VIGEM_ERROR_BUS_ACCESS_FAILED)
            m_driverMissing = true;
        vigem_free(static_cast<PVIGEM_CLIENT>(m_client));
        m_client = nullptr;
        return;
    }

    m_target = vigem_target_x360_alloc();
    if (!m_target) { printf("[ViGEm] target alloc failed\n"); return; }

    err = vigem_target_add(static_cast<PVIGEM_CLIENT>(m_client),
                           static_cast<PVIGEM_TARGET>(m_target));
    if (!VIGEM_SUCCESS(err)) {
        printf("[ViGEm] target_add failed: 0x%08X\n", err);
        vigem_target_free(static_cast<PVIGEM_TARGET>(m_target));
        m_target = nullptr;
        return;
    }

    printf("[ViGEm] Virtual Xbox 360 controller connected\n");
    m_valid = true;
}

VirtualController::~VirtualController() {
    if (m_client && m_target) {
        vigem_target_remove(static_cast<PVIGEM_CLIENT>(m_client),
                            static_cast<PVIGEM_TARGET>(m_target));
    }
    if (m_target) vigem_target_free(static_cast<PVIGEM_TARGET>(m_target));
    if (m_client) {
        vigem_disconnect(static_cast<PVIGEM_CLIENT>(m_client));
        vigem_free(static_cast<PVIGEM_CLIENT>(m_client));
    }
}

void VirtualController::Update(const uint8_t* buf, size_t n) {
    if (!m_valid) return;
    XUSB_REPORT report = Translate(buf, n, m_dzL, m_expL, m_dzR, m_expR);
    vigem_target_x360_update(static_cast<PVIGEM_CLIENT>(m_client),
                             static_cast<PVIGEM_TARGET>(m_target),
                             report);
}

void VirtualController::SetStickConfig(float dzL, float expL, float dzR, float expR) {
    m_dzL  = dzL;
    m_expL = expL;
    m_dzR  = dzR;
    m_expR = expR;
}
