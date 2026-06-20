#pragma once
#include <cstdint>
#include <cstddef>

// Maps the controller's digital buttons to outputs: an Xbox 360 button, a
// keyboard key, or nothing. Xbox-mapped sources are OR'd into a button bitmask
// (consumed by the virtual gamepad); key-mapped sources inject key presses with
// edge detection so a held button produces one key-down / key-up pair.
class InputMapper {
public:
    enum class Type : uint8_t { None, Xbox, Key, Mouse };
    struct Action { Type type = Type::None; uint16_t value = 0; };

    // A physical button: where to read it and its factory-default action.
    struct Source { const wchar_t* name; uint8_t byteIndex; uint8_t mask; Action def; };

    // A selectable target for the remap UI.
    struct Target { const wchar_t* name; Type type; uint16_t value; };

    // Mouse-button values used by Type::Mouse actions. (Note: avoid the MB_
    // prefix -- MB_RIGHT etc. are Windows MessageBox macros.)
    enum : uint16_t { MBTN_LEFT = 1, MBTN_RIGHT = 2, MBTN_MIDDLE = 3, MBTN_X1 = 4, MBTN_X2 = 5 };

    static constexpr int kSourceCount = 21;
    static const Source  kSources[kSourceCount];

    static const Target  kXboxTargets[];
    static const int     kXboxTargetCount;
    static const Target  kKeyTargets[];
    static const int     kKeyTargetCount;

    InputMapper();

    void   SetAction(int i, Action a);
    Action GetAction(int i) const;
    void   ResetToDefaults();

    // Build Xbox button bits for this frame and inject any key events.
    uint16_t Process(const uint8_t* buf, size_t n);
    // Release any keys currently held (call on teardown / mode change).
    void ReleaseKeys();

private:
    Action m_actions[kSourceCount];
    bool   m_keyDown[kSourceCount] = {};
};
