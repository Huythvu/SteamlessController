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
