#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

class TrackpadMouse {
public:
    void SetTrackpadEnabled(bool enabled)        { m_trackpadEnabled    = enabled; }
    void SetBackButtonsEnabled(bool enabled)     { m_backButtonsEnabled = enabled; }
    void SetUseLeftTrackpad(bool enabled)        { m_useLeftTrackpad    = enabled; }
    void SetScrollEnabled(bool enabled)          { m_scrollEnabled      = enabled; }
    void SetInvertScroll(bool enabled)           { m_invertScroll       = enabled; }
    void SetSensitivity(float sensitivity)       { m_sensitivity        = sensitivity; }
    void SetScrollSensitivity(float sensitivity) { m_scrollSensitivity  = sensitivity; }

    // Local trackpad haptics. The sink fires a pulse on (side, amplitude).
    void SetHapticSink(std::function<void(uint8_t, uint16_t)> sink) { m_haptic = std::move(sink); }
    void SetHapticOnClick(bool enabled)    { m_hapticOnClick = enabled; }
    void SetHapticOnMove(bool enabled)     { m_hapticOnMove  = enabled; }
    void SetMoveTickDistance(float dist)   { m_moveTickDistance = dist; }   // trackpad units / tick

    void Update(const uint8_t* buf, size_t n);
    void Reset();

private:
    struct Pad { bool touching; bool clicking; int16_t x; int16_t y; };
    static Pad ReadPad(const uint8_t* buf, bool left);

    bool     m_trackpadEnabled    = false;
    bool     m_backButtonsEnabled = false;
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
    int16_t  m_scrollPrevY    = 0;
    float    m_scrollAccum    = 0.0f;

    // Click state
    bool     m_prevClick  = false;
    bool     m_prevR4     = false;
    bool     m_prevR5     = false;

    float    m_sensitivity       = 0.015f;
    float    m_scrollSensitivity = 0.06f;

    // Haptics
    std::function<void(uint8_t, uint16_t)> m_haptic;
    bool     m_hapticOnClick    = false;
    bool     m_hapticOnMove     = false;
    float    m_moveTickDistance = 3000.0f;   // smaller = more ticks per movement
    float    m_moveAccum        = 0.0f;      // distance since last move tick

    // Fixed pulse strengths (amplitude is barely perceptible, so density is
    // the user-facing control; these just need to be "felt").
    static constexpr float HAPTIC_CLICK  = 700.0f;
    static constexpr float HAPTIC_MOVE   = 600.0f;
    static constexpr float HAPTIC_SCROLL = 700.0f;
    static constexpr int   MOVE_JITTER   = 50;   // ignore deltas below this (resting jitter)

    // side 0 = right pad, 1 = left pad
    uint8_t  mousePadSide()  const { return static_cast<uint8_t>(m_useLeftTrackpad ? 1 : 0); }
    uint8_t  scrollPadSide() const { return static_cast<uint8_t>(m_useLeftTrackpad ? 0 : 1); }
    void     fireHaptic(uint8_t side, float amp) {
        if (m_haptic) m_haptic(side, static_cast<uint16_t>(amp));
    }
};
