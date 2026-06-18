#pragma once
#include <cstdint>
#include <cstddef>

class VirtualController {
public:
    VirtualController();
    ~VirtualController();
    VirtualController(const VirtualController&) = delete;
    VirtualController& operator=(const VirtualController&) = delete;

    bool IsValid()          const { return m_valid; }
    bool IsDriverMissing()  const { return m_driverMissing; }

    void Update(const uint8_t* buf, size_t n);

    // Per-stick radial deadzone (0..1 fraction) and response-curve exponent
    // (1 = linear, >1 = gentler near center, <1 = more aggressive).
    void SetStickConfig(float dzL, float expL, float dzR, float expR);

private:
    void* m_client       = nullptr;
    void* m_target       = nullptr;
    bool  m_valid        = false;
    bool  m_driverMissing = false;

    float m_dzL  = 0.10f;
    float m_expL = 1.0f;
    float m_dzR  = 0.10f;
    float m_expR = 1.0f;
};
