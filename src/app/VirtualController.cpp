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

static XUSB_REPORT Translate(const uint8_t* buf, size_t n, uint16_t buttonBits,
                             float dzL, float expL, float dzR, float expR) {
    XUSB_REPORT r{};
    if (n < 18) return r;

    // Button bits are supplied by the InputMapper (configurable remapping).
    r.wButtons = buttonBits;

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

void VirtualController::Update(const uint8_t* buf, size_t n, uint16_t buttonBits) {
    if (!m_valid) return;
    XUSB_REPORT report = Translate(buf, n, buttonBits, m_dzL, m_expL, m_dzR, m_expR);

    // A trackpad in "Gamepad stick" mode overrides the right stick with its
    // absolute touch position (centered when not touching).
    if (m_padStick >= 0 && n >= 30) {
        const bool left = (m_padStick == 1);
        const bool touch = left ? (buf[5] & SteamController::BTN_TP_LT) != 0
                                : (buf[4] & SteamController::BTN_TP_RT) != 0;
        if (touch) {
            int16_t px, py;
            memcpy(&px, buf + (left ? 18 : 24), 2);
            memcpy(&py, buf + (left ? 20 : 26), 2);
            ApplyStick(px, py, m_dzR, m_expR, report.sThumbRX, report.sThumbRY);
        } else {
            report.sThumbRX = 0;
            report.sThumbRY = 0;
        }
    }

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
