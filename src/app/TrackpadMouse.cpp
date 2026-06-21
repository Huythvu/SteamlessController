#include "TrackpadMouse.h"
#include "steam/SteamController.h"
#include <Windows.h>
#include <cstring>
#include <cmath>

static constexpr uint8_t BTN_TP_RT_CLICK = 0x40;  // buf[4] bit 6 — right pad hard press

// Read one trackpad's touch / click / position from a state report.
TrackpadMouse::Pad TrackpadMouse::ReadPad(const uint8_t* buf, bool left) {
    Pad p{};
    const uint8_t b2 = buf[4];
    const uint8_t b3 = buf[5];
    if (left) {
        p.touching = (b3 & SteamController::BTN_TP_LT)       != 0;
        p.clicking = (b3 & SteamController::BTN_TP_LT_CLICK) != 0;
        std::memcpy(&p.x, buf + 18, 2);
        std::memcpy(&p.y, buf + 20, 2);
    } else {
        p.touching = (b2 & SteamController::BTN_TP_RT) != 0;
        p.clicking = (b2 & BTN_TP_RT_CLICK)            != 0;
        std::memcpy(&p.x, buf + 24, 2);
        std::memcpy(&p.y, buf + 26, 2);
    }
    return p;
}

void TrackpadMouse::Reset() {
    // Mouse-button output (pad clicks / paddles) is now owned by InputMapper;
    // here we only clear local movement + haptic edge state.
    m_touching  = false;
    m_prevClick = false;
    m_prevX     = 0;
    m_prevY     = 0;
    m_accumX    = 0.0f;
    m_accumY    = 0.0f;
    m_scrollTouching = false;
    m_scrollPrevClick = false;
    m_scrollPrevY    = 0;
    m_scrollPrevX    = 0;
    m_scrollAccum    = 0.0f;
    m_scrollAccumX   = 0.0f;
    m_scrollMoveAccum = 0.0f;
    m_moveAccum      = 0.0f;
    m_hpMt = m_hpSt  = false;
    m_lastMouseMove.store(0);
    m_lastScrollMove.store(0);
}

void TrackpadMouse::Update(const uint8_t* buf, size_t n) {
    if (n < 30) return;

    // The mouse uses one trackpad; the scroll wheel uses the other one.
    const bool mouseLeft  = m_useLeftTrackpad;
    const bool scrollLeft = !m_useLeftTrackpad;

    // --- Trackpad mouse movement and click ---
    if (m_trackpadEnabled) {
        const Pad pad = ReadPad(buf, mouseLeft);

        if (pad.touching && m_touching) {
            const int rawdx = pad.x - m_prevX;
            const int rawdy = pad.y - m_prevY;
            const int adx   = rawdx < 0 ? -rawdx : rawdx;
            const int ady   = rawdy < 0 ? -rawdy : rawdy;
            m_lastMouseMove.store(adx + ady);   // publish for the live view

            float fdx = 0.0f, fdy = 0.0f;       // pixels to move this frame
            if (m_smartDeadzone) {
                // Anti-jitter radial deadzone -> soft curve -> accel -> smoothing.
                const float mag = std::sqrt(static_cast<float>(rawdx) * rawdx +
                                            static_cast<float>(rawdy) * rawdy);
                const float dz  = static_cast<float>(m_mouseDeadzone);
                float targetX = 0.0f, targetY = 0.0f;
                if (mag > dz) {
                    float t = (mag - dz) / (kSmartMaxInput - dz);   // remap from 0 (no jump)
                    if (t > 1.0f) t = 1.0f;
                    const float shaped = std::pow(t, m_smartCurve); // gentle small moves
                    float speed = shaped * kSmartMaxInput * m_sensitivity;
                    if (m_smartAccel > 0.0f) speed *= 1.0f + m_smartAccel * t;
                    const float inv = 1.0f / mag;
                    targetX =  rawdx * inv * speed;
                    targetY = -rawdy * inv * speed;             // up = up
                }
                // Glide toward the target instead of snapping (smoothing).
                const float follow = 1.0f - m_smartSmoothing;
                m_smoothX += (targetX - m_smoothX) * follow;
                m_smoothY += (targetY - m_smoothY) * follow;
                fdx = m_smoothX + m_accumX;
                fdy = m_smoothY + m_accumY;
            } else if (adx + ady > m_mouseDeadzone) {
                // Hard deadzone + linear response (original behaviour).
                fdx =  rawdx * m_sensitivity + m_accumX;
                fdy = -rawdy * m_sensitivity + m_accumY;        // up = up
            } else {
                fdx = m_accumX;
                fdy = m_accumY;
            }

            // Accumulate fractional movement so slow motion isn't lost to
            // truncation (a single frame's delta can round to < 1px).
            const int idx = static_cast<int>(fdx);
            const int idy = static_cast<int>(fdy);
            m_accumX = fdx - idx;
            m_accumY = fdy - idy;
            if (idx != 0 || idy != 0) {
                INPUT input{};
                input.type       = INPUT_MOUSE;
                input.mi.dwFlags = MOUSEEVENTF_MOVE;
                input.mi.dx      = idx;
                input.mi.dy      = idy;
                SendInput(1, &input, sizeof(INPUT));
            }
        }

        if (pad.touching) { m_prevX = pad.x; m_prevY = pad.y; }
        else { m_accumX = m_accumY = 0.0f; m_smoothX = m_smoothY = 0.0f; m_lastMouseMove.store(0); }
        m_touching = pad.touching;
    }

    // --- Trackpad scroll wheel (vertical + horizontal) ---
    if (m_scrollEnabled) {
        const Pad pad = ReadPad(buf, scrollLeft);

        if (pad.touching && m_scrollTouching) {
            const int rawdy = pad.y - m_scrollPrevY;
            const int rawdx = pad.x - m_scrollPrevX;
            const int ady   = rawdy < 0 ? -rawdy : rawdy;
            const int adx   = rawdx < 0 ? -rawdx : rawdx;
            m_lastScrollMove.store(ady + adx);   // publish for the live view

            // Vertical wheel. Deadzone ignores tiny movement (resting thumb).
            if (ady > m_scrollDeadzone) {
                // Natural direction: finger up scrolls up. Invert flips it.
                const float dir    = m_invertScroll ? -1.0f : 1.0f;
                const float fdelta = dir * rawdy * m_scrollSensitivity + m_scrollAccum;
                const int   ticks  = static_cast<int>(fdelta);
                m_scrollAccum = fdelta - ticks;
                if (ticks != 0) {
                    INPUT input{};
                    input.type         = INPUT_MOUSE;
                    input.mi.dwFlags   = MOUSEEVENTF_WHEEL;
                    input.mi.mouseData = static_cast<DWORD>(ticks);
                    SendInput(1, &input, sizeof(INPUT));
                }
            }

            // Horizontal wheel. Finger right scrolls right (natural swipe).
            if (adx > m_scrollDeadzone) {
                const float fdelta = rawdx * m_scrollSensitivity + m_scrollAccumX;
                const int   ticks  = static_cast<int>(fdelta);
                m_scrollAccumX = fdelta - ticks;
                if (ticks != 0) {
                    INPUT input{};
                    input.type         = INPUT_MOUSE;
                    input.mi.dwFlags   = MOUSEEVENTF_HWHEEL;
                    input.mi.mouseData = static_cast<DWORD>(ticks);
                    SendInput(1, &input, sizeof(INPUT));
                }
            }
        }

        if (pad.touching) { m_scrollPrevY = pad.y; m_scrollPrevX = pad.x; }
        else { m_scrollAccum = 0.0f; m_scrollAccumX = 0.0f; m_lastScrollMove.store(0); }
        m_scrollTouching = pad.touching;
    }

    const Pad mp = ReadPad(buf, mouseLeft);
    const Pad sp = ReadPad(buf, scrollLeft);

    // --- Click haptics ---
    // Fire whenever a pad is hard-pressed, independent of whether the mouse or
    // scroll features are enabled (the click itself is a remappable button now,
    // so it can have a function regardless). Two-way: on press and release.
    if (mp.clicking != m_prevClick) {
        if (m_hapticOnClick) fireClick(mousePadSide());
        m_prevClick = mp.clicking;
    }
    if (sp.clicking != m_scrollPrevClick) {
        if (m_hapticOnClick) fireClick(scrollPadSide());
        m_scrollPrevClick = sp.clicking;
    }

    // --- Movement-texture haptics ---
    // Both pads use the SAME metric (total 2D finger travel) and the same
    // density, so left and right feel identical. Independent of the mouse /
    // scroll output toggles, so e.g. the scroll pad still buzzes per movement
    // even with the scroll wheel turned off.
    auto moveTexture = [&](const Pad& pad, bool& prevTouch,
                           int16_t& px, int16_t& py, float& accum, uint8_t side) {
        if (m_hapticOnMove && pad.touching && prevTouch) {
            const int dx = pad.x - px < 0 ? px - pad.x : pad.x - px;
            const int dy = pad.y - py < 0 ? py - pad.y : pad.y - py;
            accum += static_cast<float>(dx + dy);   // total 2D travel, any direction
            if (accum >= m_moveTickDistance) {
                accum = 0.0f;
                fireHaptic(side, HAPTIC_MOVE);
            }
        }
        if (pad.touching) { px = pad.x; py = pad.y; }
        else                accum = 0.0f;
        prevTouch = pad.touching;
    };
    // Both pads use the same metric (total 2D travel) and density, so the left
    // and right feel identical, independent of the mouse/scroll output toggles.
    moveTexture(mp, m_hpMt, m_hpMx, m_hpMy, m_moveAccum,       mousePadSide());
    moveTexture(sp, m_hpSt, m_hpSx, m_hpSy, m_scrollMoveAccum, scrollPadSide());
}
