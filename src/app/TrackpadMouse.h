#pragma once
#include "PadRole.h"
#include <cstdint>
#include <cstddef>
#include <functional>
#include <atomic>

class TrackpadMouse {
public:
    // Per-pad role. side 0 = right pad, 1 = left pad.
    void SetRole(int side, PadRole role)         { if (side >= 0 && side < 2) m_role[side] = role; }
    PadRole GetRole(int side) const              { return (side >= 0 && side < 2) ? m_role[side] : PadRole::Off; }
    void SetDpadWASD(bool wasd)                  { m_dpadWASD = wasd; }
    void SetDpadSingle(bool single)              { m_dpadSingle = single; }   // one direction at a time
    void SetDpadOnClick(bool onClick)            { m_dpadOnClick = onClick; } // require a hard press
    void SetButtonsOnClick(bool onClick)         { m_btnOnClick = onClick; }
    void SetButtonsSwap(bool swap)               { m_btnSwap = swap; }         // swap L/R click zones
    void SetSuspended(bool s)                    { m_suspended = s; }

    void SetInvertScroll(bool enabled)           { m_invertScroll       = enabled; }
    void SetSensitivity(float sensitivity)       { m_sensitivity        = sensitivity; }
    void SetScrollSensitivity(float sensitivity) { m_scrollSensitivity  = sensitivity; }
    void SetSmartScroll(bool enabled)            { m_smartScroll        = enabled; }

    // Local trackpad haptics. The sink fires (side, amplitude, pulse count).
    void SetHapticSink(std::function<void(uint8_t, uint16_t, uint8_t)> sink) { m_haptic = std::move(sink); }
    void SetHapticOnClick(bool enabled)    { m_hapticOnClick = enabled; }
    void SetHapticOnMove(bool enabled)     { m_hapticOnMove  = enabled; }   // both pads (all axes)
    void SetClickHardness(int level)       { m_clickHardness = level < 1 ? 1 : (level > 3 ? 3 : level); }
    void SetMoveTickDistance(float dist)   { m_moveTickDistance = dist; }   // trackpad units / tick
    void SetMouseDeadzone(int dz)          { m_mouseDeadzone  = dz; }       // per-frame units
    void SetScrollDeadzone(int dz)         { m_scrollDeadzone = dz; }

    void Update(const uint8_t* buf, size_t n);
    void Reset();

    // The most recent per-report movement the deadzone check actually sees
    // (mouse = |dx|+|dy|, scroll = |dy|), for the live view. 0 when not touching.
    int  LastMouseMove()  const { return m_lastMouseMove.load(); }
    int  LastScrollMove() const { return m_lastScrollMove.load(); }

private:
    struct Pad { bool touching; bool clicking; int16_t x; int16_t y; };
    static Pad ReadPad(const uint8_t* buf, bool left);

    // Per-pad working state (index 0 = right pad, 1 = left pad).
    struct PadState {
        // mouse
        bool    touching = false;
        int16_t prevX = 0, prevY = 0;
        float   accumX = 0.0f, accumY = 0.0f;
        // scroll
        bool    sTouching = false;
        int16_t sPrevX = 0, sPrevY = 0;
        float   sAccum = 0.0f, sAccumX = 0.0f;
        int16_t sStartX = 0, sStartY = 0;
        bool    sActive = false;
        int     sBufX = 0, sBufY = 0;
        float   sMoveAccum = 0.0f;
        // dpad: bitmask of currently-held directions (1=up 2=down 4=left 8=right)
        int     dpadHeld = 0;
        // buttons role: which mouse button is held (0=none 1=L 2=R 3=M)
        int     btnHeld = 0;
        // click haptic + movement-texture haptic
        bool    prevClick = false;
        int16_t hpX = 0, hpY = 0; bool hpT = false; float moveAccum = 0.0f;
    };
    PadState m_pad[2];

    void DoMouse  (PadState& ps, const Pad& pad);
    void DoScroll (PadState& ps, const Pad& pad);
    void DoDpad   (PadState& ps, const Pad& pad);
    void DoButtons(PadState& ps, const Pad& pad);
    void ReleaseDpad(PadState& ps)    { ApplyDpad(ps, 0); }
    void ReleaseButtons(PadState& ps);
    void ApplyDpad(PadState& ps, int want);

    PadRole  m_role[2]            = { PadRole::Mouse, PadRole::Scroll };  // right, left
    bool     m_dpadWASD           = false;
    bool     m_dpadSingle         = false;
    bool     m_dpadOnClick        = false;
    bool     m_btnOnClick         = false;
    bool     m_btnSwap            = false;
    bool     m_suspended          = false;
    bool     m_invertScroll       = false;
    bool     m_smartScroll        = false;

    static constexpr int kScrollActivate = 200;  // travel from touch-down before scrolling
    static constexpr int kScrollNoise    = 40;   // per-frame noise floor (reject jitter)

    float    m_sensitivity       = 0.015f;
    float    m_scrollSensitivity = 0.06f;

    // Haptics
    std::function<void(uint8_t, uint16_t, uint8_t)> m_haptic;
    bool     m_hapticOnClick    = false;
    bool     m_hapticOnMove     = false;     // movement texture, both pads (all axes)
    int      m_clickHardness    = 2;         // 1=soft, 2=medium, 3=hard
    float    m_moveTickDistance = 3000.0f;   // smaller = more ticks per movement
    int      m_mouseDeadzone    = 20;        // per-frame deadzone (units)
    int      m_scrollDeadzone   = 120;
    std::atomic<int> m_lastMouseMove{0};     // live view: latest report's movement
    std::atomic<int> m_lastScrollMove{0};

    static constexpr float HAPTIC_MOVE = 600.0f;

    void fireHaptic(uint8_t side, float amp, uint8_t count = 1) {
        if (m_haptic) m_haptic(side, static_cast<uint16_t>(amp), count);
    }
    void fireClick(uint8_t side) {
        const float    amp   = m_clickHardness == 1 ? 700.0f
                             : m_clickHardness == 2 ? 1600.0f : 3200.0f;
        const uint8_t  count = static_cast<uint8_t>(m_clickHardness);  // 1..3 pulses
        fireHaptic(side, amp, count);
    }
};
