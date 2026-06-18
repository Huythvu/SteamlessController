#pragma once
#include <cstdint>
#include <cstddef>

class TrackpadMouse {
public:
    void SetTrackpadEnabled(bool enabled)     { m_trackpadEnabled    = enabled; }
    void SetBackButtonsEnabled(bool enabled)  { m_backButtonsEnabled = enabled; }
    void SetUseLeftTrackpad(bool enabled)     { m_useLeftTrackpad    = enabled; }
    void SetScrollEnabled(bool enabled)       { m_scrollEnabled      = enabled; }
    void SetSensitivity(float sensitivity)    { m_sensitivity        = sensitivity; }

    void Update(const uint8_t* buf, size_t n);
    void Reset();

private:
    struct Pad { bool touching; bool clicking; int16_t x; int16_t y; };
    static Pad ReadPad(const uint8_t* buf, bool left);

    bool     m_trackpadEnabled    = false;
    bool     m_backButtonsEnabled = false;
    bool     m_useLeftTrackpad    = false;
    bool     m_scrollEnabled      = false;

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

    float    m_sensitivity = 0.015f;
    static constexpr float SCROLL_SENSITIVITY = 0.06f;
};
