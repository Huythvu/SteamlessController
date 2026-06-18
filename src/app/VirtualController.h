#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>

class VirtualController {
public:
    VirtualController();
    ~VirtualController();
    VirtualController(const VirtualController&) = delete;
    VirtualController& operator=(const VirtualController&) = delete;

    bool IsValid()          const { return m_valid; }
    bool IsDriverMissing()  const { return m_driverMissing; }

    void Update(const uint8_t* buf, size_t n, uint16_t buttonBits);

    // Per-stick radial deadzone (0..1 fraction) and response-curve exponent
    // (1 = linear, >1 = gentler near center, <1 = more aggressive).
    void SetStickConfig(float dzL, float expL, float dzR, float expR);

    // Latest rumble the game requested (0..255), set from the ViGEm callback.
    void    SetRumble(uint8_t large, uint8_t small) { m_rumbleLarge = large; m_rumbleSmall = small; }
    uint8_t RumbleLevel() const {
        uint8_t a = m_rumbleLarge.load(), b = m_rumbleSmall.load();
        return a > b ? a : b;
    }

private:
    void* m_client       = nullptr;
    void* m_target       = nullptr;
    bool  m_valid        = false;
    bool  m_driverMissing = false;

    float m_dzL  = 0.10f;
    float m_expL = 1.0f;
    float m_dzR  = 0.10f;
    float m_expR = 1.0f;

    std::atomic<uint8_t> m_rumbleLarge{0};
    std::atomic<uint8_t> m_rumbleSmall{0};
};
