#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <atomic>

class TrackpadMouse {
public:
    void SetTrackpadEnabled(bool enabled)        { m_trackpadEnabled    = enabled; }
    void SetUseLeftTrackpad(bool enabled)        { m_useLeftTrackpad    = enabled; }
    void SetScrollEnabled(bool enabled)          { m_scrollEnabled      = enabled; }
    void SetInvertScroll(bool enabled)           { m_invertScroll       = enabled; }
    void SetSensitivity(float sensitivity)       { m_sensitivity        = sensitivity; }
    void SetScrollSensitivity(float sensitivity) { m_scrollSensitivity  = sensitivity; }

    // Local trackpad haptics. The sink fires (side, amplitude, pulse count).
    void SetHapticSink(std::function<void(uint8_t, uint16_t, uint8_t)> sink) { m_haptic = std::move(sink); }
    void SetHapticOnClick(bool enabled)    { m_hapticOnClick = enabled; }
    void SetHapticOnMove(bool enabled)     { m_hapticOnMove  = enabled; }   // mouse pad
    void SetScrollHapticMode(int mode)     { m_scrollHapticMode = mode; }   // 0 off, 1 per step, 2 per movement
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

    bool     m_trackpadEnabled    = false;
    bool     m_useLeftTrackpad    = false;
    bool     m_scrollEnabled      = false;
    bool     m_invertScroll       = false;

    // Mouse-movement state
    bool     m_touching   = false;
    int16_t  m_prevX      = 0;
    int16_t  m_prevY      = 0;
    float    m_accumX     = 0.0f;   // carry sub-pixel movement between frames
    float    m_accumY     = 0.0f;

    // Scroll-wheel state
    bool     m_scrollTouching = false;
    bool     m_scrollPrevClick = false;
    int16_t  m_scrollPrevY    = 0;
    float    m_scrollAccum    = 0.0f;
    float    m_scrollMoveAccum = 0.0f;   // distance since last scroll haptic tick

    // Click haptic edge state
    bool     m_prevClick  = false;

    float    m_sensitivity       = 0.015f;
    float    m_scrollSensitivity = 0.06f;

    // Haptics
    std::function<void(uint8_t, uint16_t, uint8_t)> m_haptic;
    bool     m_hapticOnClick    = false;
    bool     m_hapticOnMove     = false;     // mouse-pad movement texture
    int      m_scrollHapticMode = 0;         // 0 off, 1 per scroll step, 2 per movement
    int      m_clickHardness    = 2;         // 1=soft, 2=medium, 3=hard
    float    m_moveTickDistance = 3000.0f;   // smaller = more ticks per movement
    float    m_moveAccum        = 0.0f;      // distance since last move tick
    int      m_mouseDeadzone    = 20;        // per-frame deadzone (units)
    int      m_scrollDeadzone   = 120;
    std::atomic<int> m_lastMouseMove{0};     // live view: latest report's movement
    std::atomic<int> m_lastScrollMove{0};

    // Move-tick pulse strength (density is the user-facing control for moves).
    static constexpr float HAPTIC_MOVE  = 600.0f;

    // side 0 = right pad, 1 = left pad
    uint8_t  mousePadSide()  const { return static_cast<uint8_t>(m_useLeftTrackpad ? 1 : 0); }
    uint8_t  scrollPadSide() const { return static_cast<uint8_t>(m_useLeftTrackpad ? 0 : 1); }
    void     fireHaptic(uint8_t side, float amp, uint8_t count = 1) {
        if (m_haptic) m_haptic(side, static_cast<uint16_t>(amp), count);
    }
    // Click feedback scaled by the 3-step hardness selector.
    void     fireClick(uint8_t side) {
        const float    amp   = m_clickHardness == 1 ? 700.0f
                             : m_clickHardness == 2 ? 1600.0f : 3200.0f;
        const uint8_t  count = static_cast<uint8_t>(m_clickHardness);  // 1..3 pulses
        fireHaptic(side, amp, count);
    }
};
