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

// Compute the D-pad direction bitmask (1=up 2=down 4=left 8=right) from a pad
// position (x,y; +y = up). dz is the center deadzone in pad units.
//   single   : only the dominant axis fires (4-way, no diagonals).
//   diagonal : dedicated 8-way zones (a corner lights both adjacent keys).
//   otherwise: a simple cross (each axis independent; corners can overlap).
inline int DpadBits(int x, int y, bool single, bool diagonal, int dz = 8000) {
    const int ax = x < 0 ? -x : x;
    const int ay = y < 0 ? -y : y;
    if (ax < dz && ay < dz) return 0;              // inside the center deadzone
    int bits = 0;
    if (single) {
        if (ax >= ay) bits = (x < 0) ? 4 : 8;      // left / right
        else          bits = (y > 0) ? 1 : 2;      // up / down
    } else if (diagonal) {
        if (ay * 2 >= ax) bits |= (y > 0) ? 1 : 2; // axis counts within ~63deg
        if (ax * 2 >= ay) bits |= (x < 0) ? 4 : 8;
    } else {
        if (y >  dz) bits |= 1;
        if (y < -dz) bits |= 2;
        if (x < -dz) bits |= 4;
        if (x >  dz) bits |= 8;
    }
    return bits;
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
