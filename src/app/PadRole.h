#pragma once

// What a single trackpad does. Each pad (right and left) is assigned one role.
enum class PadRole : int {
    Off    = 0,   // nothing (its click is still remappable via InputMapper)
    Mouse  = 1,   // move the mouse cursor
    Scroll = 2,   // scroll wheel (vertical + horizontal)
    Dpad   = 3,   // directional keys (arrows or WASD) while a direction is held
    Stick  = 4,   // drive the virtual gamepad's right stick (aim)
    Buttons = 5,  // touch zones act as left / middle / right mouse buttons
};
inline constexpr int kPadRoleCount = 6;

// Compute the cardinal D-pad direction bitmask (1=up 2=down 4=left 8=right)
// from a pad position (x,y; +y = up). dz is the center deadzone in pad units.
//   single : only the dominant axis fires (4-way, no diagonals).
//   else   : a cross (each axis independent; corners can light two).
inline int DpadBits(int x, int y, bool single, int dz = 8000) {
    const int ax = x < 0 ? -x : x;
    const int ay = y < 0 ? -y : y;
    if (ax < dz && ay < dz) return 0;              // inside the center deadzone
    int bits = 0;
    if (single) {
        if (ax >= ay) bits = (x < 0) ? 4 : 8;      // left / right
        else          bits = (y > 0) ? 1 : 2;      // up / down
    } else {
        if (y >  dz) bits |= 1;
        if (y < -dz) bits |= 2;
        if (x < -dz) bits |= 4;
        if (x >  dz) bits |= 8;
    }
    return bits;
}

// Which corner quadrant a pad position is in (0=TR 1=TL 2=BR 3=BL), or -1 when
// inside the center deadzone. Used by the D-pad "diagonal" (corner-keys) mode.
inline int DpadQuadrant(int x, int y, int dz = 8000) {
    const int ax = x < 0 ? -x : x;
    const int ay = y < 0 ? -y : y;
    if (ax < dz && ay < dz) return -1;
    const bool right = x >= 0, up = y >= 0;
    return up ? (right ? 0 : 1) : (right ? 2 : 3);
}

inline const char* PadRoleName(PadRole r) {
    switch (r) {
        case PadRole::Off:     return "Off";
        case PadRole::Mouse:   return "Mouse";
        case PadRole::Scroll:  return "Scroll";
        case PadRole::Dpad:    return "Directional keys";
        case PadRole::Stick:   return "Gamepad stick";
        case PadRole::Buttons: return "Mouse buttons";
    }
    return "Off";
}
