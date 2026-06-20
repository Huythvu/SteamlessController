#include "TrackpadMouse.h"
#include "steam/SteamController.h"
#include <Windows.h>
#include <cstring>

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
    m_scrollAccum    = 0.0f;
    m_scrollMoveAccum = 0.0f;
    m_moveAccum      = 0.0f;
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
            // Deadzone: ignore movement below the threshold (resting jitter).
            if (adx + ady > m_mouseDeadzone) {
                // Accumulate fractional movement so slow motion isn't lost to
                // truncation (a single frame's delta * sensitivity can be < 1px).
                const float fdx =  rawdx * m_sensitivity + m_accumX;
                const float fdy = -rawdy * m_sensitivity + m_accumY;  // up = up
                const int   idx = static_cast<int>(fdx);
                const int   idy = static_cast<int>(fdy);
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

                // Textured "tick" feedback as the cursor moves across the pad.
                if (m_hapticOnMove) {
                    m_moveAccum += static_cast<float>(adx + ady);
                    if (m_moveAccum >= m_moveTickDistance) {
                        m_moveAccum = 0.0f;
                        fireHaptic(mousePadSide(), HAPTIC_MOVE);
                    }
                }
            }
        }

        if (pad.touching) { m_prevX = pad.x; m_prevY = pad.y; }
        else { m_accumX = m_accumY = 0.0f; m_moveAccum = 0.0f; m_lastMouseMove.store(0); }
        m_touching = pad.touching;

        if (pad.clicking != m_prevClick) {
            // The click output itself is produced by InputMapper (remappable);
            // here we only add the local two-way click haptic.
            if (m_hapticOnClick)
                fireClick(mousePadSide());
            m_prevClick = pad.clicking;
        }
    }

    // --- Trackpad scroll wheel (vertical) ---
    if (m_scrollEnabled) {
        const Pad pad = ReadPad(buf, scrollLeft);

        if (pad.touching && m_scrollTouching) {
            const int rawdy = pad.y - m_scrollPrevY;
            const int ady   = rawdy < 0 ? -rawdy : rawdy;
            m_lastScrollMove.store(ady);   // publish for the live view
            // Deadzone: ignore tiny movement so a resting thumb doesn't scroll.
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

                // Same distance-based tick feedback as the mouse pad, so scroll
                // and movement haptics feel identical (and share the density).
                if (m_hapticOnMove) {
                    m_scrollMoveAccum += static_cast<float>(ady);
                    if (m_scrollMoveAccum >= m_moveTickDistance) {
                        m_scrollMoveAccum = 0.0f;
                        fireHaptic(scrollPadSide(), HAPTIC_MOVE);
                    }
                }
            }
        }

        if (pad.touching) m_scrollPrevY = pad.y;
        else { m_scrollAccum = 0.0f; m_lastScrollMove.store(0); }
        m_scrollTouching = pad.touching;

        // Scroll-pad click (middle-click by default) is produced by InputMapper;
        // here we only add the local click haptic.
        if (pad.clicking != m_scrollPrevClick) {
            if (m_hapticOnClick)
                fireClick(scrollPadSide());
            m_scrollPrevClick = pad.clicking;
        }
    }
}
