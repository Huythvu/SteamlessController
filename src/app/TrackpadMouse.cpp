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
    for (PadState& ps : m_pad) {
        ApplyDpad(ps, 0);
        ReleaseButtons(ps);
        ps = PadState{};
    }
    m_lastMouseMove.store(0);
    m_lastScrollMove.store(0);
}

// --- Mouse cursor movement -------------------------------------------------
void TrackpadMouse::DoMouse(PadState& ps, const Pad& pad) {
    if (pad.touching && ps.touching) {
        const int rawdx = pad.x - ps.prevX;
        const int rawdy = pad.y - ps.prevY;
        const int adx   = rawdx < 0 ? -rawdx : rawdx;
        const int ady   = rawdy < 0 ? -rawdy : rawdy;
        m_lastMouseMove.store(adx + ady);
        if (adx + ady > m_mouseDeadzone) {
            const float fdx =  rawdx * m_sensitivity + ps.accumX;
            const float fdy = -rawdy * m_sensitivity + ps.accumY;   // up = up
            const int   idx = static_cast<int>(fdx);
            const int   idy = static_cast<int>(fdy);
            ps.accumX = fdx - idx;
            ps.accumY = fdy - idy;
            if (idx != 0 || idy != 0) {
                INPUT input{};
                input.type       = INPUT_MOUSE;
                input.mi.dwFlags = MOUSEEVENTF_MOVE;
                input.mi.dx      = idx;
                input.mi.dy      = idy;
                SendInput(1, &input, sizeof(INPUT));
            }
        }
    }
    if (pad.touching) { ps.prevX = pad.x; ps.prevY = pad.y; }
    else { ps.accumX = ps.accumY = 0.0f; m_lastMouseMove.store(0); }
    ps.touching = pad.touching;
}

// --- Scroll wheel (vertical + horizontal) ----------------------------------
void TrackpadMouse::DoScroll(PadState& ps, const Pad& pad) {
    auto sendWheel = [](DWORD flag, int ticks) {
        INPUT input{};
        input.type         = INPUT_MOUSE;
        input.mi.dwFlags   = flag;
        input.mi.mouseData = static_cast<DWORD>(ticks);
        SendInput(1, &input, sizeof(INPUT));
    };

    if (pad.touching && !ps.sTouching) {
        ps.sStartX = pad.x; ps.sStartY = pad.y;
        ps.sActive = false;
        ps.sBufX = ps.sBufY = 0;
        ps.sAccum = ps.sAccumX = 0.0f;
    }

    if (pad.touching && ps.sTouching) {
        const int rawdy = pad.y - ps.sPrevY;
        const int rawdx = pad.x - ps.sPrevX;
        const int ady   = rawdy < 0 ? -rawdy : rawdy;
        const int adx   = rawdx < 0 ? -rawdx : rawdx;
        m_lastScrollMove.store(ady + adx);

        if (m_smartScroll) {
            const long long ddx = pad.x - ps.sStartX;
            const long long ddy = pad.y - ps.sStartY;
            if (!ps.sActive &&
                ddx * ddx + ddy * ddy >=
                    static_cast<long long>(kScrollActivate) * kScrollActivate)
                ps.sActive = true;

            auto axis = [&](int rawd, int ad, float dirSign, float& accum, DWORD flag) {
                if (ad <= kScrollNoise) { accum *= 0.6f; return; }
                const float v = dirSign * static_cast<float>(rawd) * m_scrollSensitivity;
                if ((v < 0.0f) != (accum < 0.0f) && accum != 0.0f) accum = 0.0f;
                accum += v;
                const int ticks = static_cast<int>(accum);
                accum -= ticks;
                if (ticks != 0) sendWheel(flag, ticks);
            };
            if (ps.sActive) {
                axis(ps.sBufY, ps.sBufY < 0 ? -ps.sBufY : ps.sBufY,
                     m_invertScroll ? -1.0f : 1.0f, ps.sAccum,  MOUSEEVENTF_WHEEL);
                axis(ps.sBufX, ps.sBufX < 0 ? -ps.sBufX : ps.sBufX,
                     1.0f, ps.sAccumX, MOUSEEVENTF_HWHEEL);
            }
            ps.sBufY = rawdy; ps.sBufX = rawdx;
        } else {
            if (ady > m_scrollDeadzone) {
                const float dir    = m_invertScroll ? -1.0f : 1.0f;
                const float fdelta = dir * rawdy * m_scrollSensitivity + ps.sAccum;
                const int   ticks  = static_cast<int>(fdelta);
                ps.sAccum = fdelta - ticks;
                if (ticks != 0) sendWheel(MOUSEEVENTF_WHEEL, ticks);
            }
            if (adx > m_scrollDeadzone) {
                const float fdelta = rawdx * m_scrollSensitivity + ps.sAccumX;
                const int   ticks  = static_cast<int>(fdelta);
                ps.sAccumX = fdelta - ticks;
                if (ticks != 0) sendWheel(MOUSEEVENTF_HWHEEL, ticks);
            }
        }
    }

    if (pad.touching) { ps.sPrevY = pad.y; ps.sPrevX = pad.x; }
    else {
        ps.sAccum = ps.sAccumX = 0.0f;
        ps.sActive = false;
        ps.sBufX = ps.sBufY = 0;
        m_lastScrollMove.store(0);
    }
    ps.sTouching = pad.touching;
}

// --- Directional keys (arrows / WASD) --------------------------------------
void TrackpadMouse::ApplyDpad(PadState& ps, int want) {
    const int  dirs[4] = { 1, 2, 4, 8 };                       // up, down, left, right
    const WORD vkArrow[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    const WORD vkWasd [4] = { 'W',   'S',     'A',     'D'     };
    for (int i = 0; i < 4; ++i) {
        const bool now = (want & dirs[i]) != 0;
        const bool was = (ps.dpadHeld & dirs[i]) != 0;
        if (now == was) continue;
        INPUT in{};
        in.type     = INPUT_KEYBOARD;
        in.ki.wVk   = m_dpadWASD ? vkWasd[i] : vkArrow[i];
        in.ki.dwFlags = now ? 0 : KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT));
    }
    ps.dpadHeld = want;
}

void TrackpadMouse::DoDpad(PadState& ps, const Pad& pad) {
    int want = 0;
    const bool active = pad.touching && (!m_dpadOnClick || pad.clicking);
    if (active) {
        const int dz  = 8000;                       // center deadzone (~25%)
        const int ax  = pad.x, ay = pad.y;
        const int aax = ax < 0 ? -ax : ax;
        const int aay = ay < 0 ? -ay : ay;
        if (m_dpadSingle) {
            // Only the dominant axis fires (no diagonals).
            if (aax >= aay) { if (ax < -dz) want |= 4; else if (ax > dz) want |= 8; }
            else            { if (ay >  dz) want |= 1; else if (ay < -dz) want |= 2; }
        } else {
            if (ay >  dz) want |= 1;                 // up
            if (ay < -dz) want |= 2;                 // down
            if (ax < -dz) want |= 4;                 // left
            if (ax >  dz) want |= 8;                 // right
        }
    }
    ApplyDpad(ps, want);
}

// --- Mouse-button touch zones ----------------------------------------------
void TrackpadMouse::ReleaseButtons(PadState& ps) {
    if (ps.btnHeld == 0) return;
    const DWORD up[4] = { 0, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP };
    INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = up[ps.btnHeld];
    SendInput(1, &in, sizeof(INPUT));
    ps.btnHeld = 0;
}

void TrackpadMouse::DoButtons(PadState& ps, const Pad& pad) {
    const bool active = m_btnOnClick ? pad.clicking : pad.touching;
    if (active && ps.btnHeld == 0) {
        // Three vertical zones; the outer two can be swapped (1=L 2=R 3=M).
        const int lb = m_btnSwap ? 2 : 1;
        const int rb = m_btnSwap ? 1 : 2;
        const int btn = pad.x < -10922 ? lb : (pad.x > 10922 ? rb : 3);
        const DWORD down[4] = { 0, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN };
        INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = down[btn];
        SendInput(1, &in, sizeof(INPUT));
        ps.btnHeld = btn;
    } else if (!active && ps.btnHeld != 0) {
        ReleaseButtons(ps);
    }
}

void TrackpadMouse::Update(const uint8_t* buf, size_t n) {
    if (n < 30) return;

    for (int side = 0; side < 2; ++side) {
        PadState&  ps   = m_pad[side];
        const Pad  pad  = ReadPad(buf, side == 1);   // side 1 = left pad
        const PadRole role = m_suspended ? PadRole::Off : m_role[side];

        // Release any held output when a pad isn't (or stops) producing it.
        if (role != PadRole::Dpad)    ReleaseDpad(ps);
        if (role != PadRole::Buttons) ReleaseButtons(ps);

        switch (role) {
            case PadRole::Mouse:   DoMouse(ps, pad);   break;
            case PadRole::Scroll:  DoScroll(ps, pad);  break;
            case PadRole::Dpad:    DoDpad(ps, pad);    break;
            case PadRole::Buttons: DoButtons(ps, pad); break;
            case PadRole::Stick:   // handled by the virtual controller
            case PadRole::Off:
            default: break;
        }

        // Click haptic (two-way), independent of role.
        if (pad.clicking != ps.prevClick) {
            if (m_hapticOnClick) fireClick(static_cast<uint8_t>(side));
            ps.prevClick = pad.clicking;
        }

        // Movement-texture haptic, independent of role. Both pads use the same
        // metric (total 2D travel) and density so they feel identical.
        if (m_hapticOnMove && pad.touching && ps.hpT) {
            const int dx = pad.x - ps.hpX < 0 ? ps.hpX - pad.x : pad.x - ps.hpX;
            const int dy = pad.y - ps.hpY < 0 ? ps.hpY - pad.y : pad.y - ps.hpY;
            ps.moveAccum += static_cast<float>(dx + dy);
            if (ps.moveAccum >= m_moveTickDistance) {
                ps.moveAccum = 0.0f;
                fireHaptic(static_cast<uint8_t>(side), HAPTIC_MOVE);
            }
        }
        if (pad.touching) { ps.hpX = pad.x; ps.hpY = pad.y; }
        else                ps.moveAccum = 0.0f;
        ps.hpT = pad.touching;
    }
}
