#include "InputMapper.h"
#include <Windows.h>

// XUSB_BUTTON bit values (stable ABI from ViGEm) — duplicated here so this
// translation unit doesn't need the ViGEm headers.
enum : uint16_t {
    XB_DPAD_UP    = 0x0001, XB_DPAD_DOWN  = 0x0002,
    XB_DPAD_LEFT  = 0x0004, XB_DPAD_RIGHT = 0x0008,
    XB_START      = 0x0010, XB_BACK       = 0x0020,
    XB_LTHUMB     = 0x0040, XB_RTHUMB     = 0x0080,
    XB_LSHOULDER  = 0x0100, XB_RSHOULDER  = 0x0200,
    XB_GUIDE      = 0x0400,
    XB_A = 0x1000, XB_B = 0x2000, XB_X = 0x4000, XB_Y = 0x8000,
};

#define XB(v) InputMapper::Action{ InputMapper::Type::Xbox, (v) }
#define MO(v) InputMapper::Action{ InputMapper::Type::Mouse, (v) }
#define NONE  InputMapper::Action{ InputMapper::Type::None, 0 }

// buf indices: [2]=b0, [3]=b1, [4]=b2, [5]=b3 (see SteamController.h).
const InputMapper::Source InputMapper::kSources[InputMapper::kSourceCount] = {
    { L"A",            2, 0x01, XB(XB_A) },
    { L"B",            2, 0x02, XB(XB_B) },
    { L"X",            2, 0x04, XB(XB_X) },
    { L"Y",            2, 0x08, XB(XB_Y) },
    { L"Left Bumper",  4, 0x08, XB(XB_LSHOULDER) },
    { L"Right Bumper", 3, 0x02, XB(XB_RSHOULDER) },
    { L"Left Stick",   3, 0x80, XB(XB_LTHUMB) },
    { L"Right Stick",  2, 0x20, XB(XB_RTHUMB) },
    { L"Menu",         2, 0x40, XB(XB_START) },
    { L"View",         3, 0x40, XB(XB_BACK) },
    { L"Steam",        4, 0x01, XB(XB_GUIDE) },
    { L"D-Pad Up",     3, 0x20, XB(XB_DPAD_UP) },
    { L"D-Pad Down",   3, 0x04, XB(XB_DPAD_DOWN) },
    { L"D-Pad Left",   3, 0x10, XB(XB_DPAD_LEFT) },
    { L"D-Pad Right",  3, 0x08, XB(XB_DPAD_RIGHT) },
    { L"L4 Paddle",    4, 0x02, NONE },
    { L"L5 Paddle",    4, 0x04, NONE },
    { L"R4 Paddle",    2, 0x80, NONE },
    { L"R5 Paddle",    3, 0x01, NONE },
    // Trackpad hard-presses. Defaults match the old behaviour: the mouse pad
    // (right by default) clicks left, the scroll pad (left) clicks middle.
    { L"Right Pad Click", 4, 0x40, MO(InputMapper::MB_LEFT) },
    { L"Left Pad Click",  5, 0x04, MO(InputMapper::MB_MIDDLE) },
};

const InputMapper::Target InputMapper::kXboxTargets[] = {
    { L"A",            Type::Xbox, XB_A },
    { L"B",            Type::Xbox, XB_B },
    { L"X",            Type::Xbox, XB_X },
    { L"Y",            Type::Xbox, XB_Y },
    { L"Left Bumper",  Type::Xbox, XB_LSHOULDER },
    { L"Right Bumper", Type::Xbox, XB_RSHOULDER },
    { L"Left Stick",   Type::Xbox, XB_LTHUMB },
    { L"Right Stick",  Type::Xbox, XB_RTHUMB },
    { L"Start (Menu)", Type::Xbox, XB_START },
    { L"Back (View)",  Type::Xbox, XB_BACK },
    { L"Guide",        Type::Xbox, XB_GUIDE },
    { L"D-Pad Up",     Type::Xbox, XB_DPAD_UP },
    { L"D-Pad Down",   Type::Xbox, XB_DPAD_DOWN },
    { L"D-Pad Left",   Type::Xbox, XB_DPAD_LEFT },
    { L"D-Pad Right",  Type::Xbox, XB_DPAD_RIGHT },
};
const int InputMapper::kXboxTargetCount =
    sizeof(InputMapper::kXboxTargets) / sizeof(InputMapper::kXboxTargets[0]);

const InputMapper::Target InputMapper::kKeyTargets[] = {
    { L"Space",  Type::Key, VK_SPACE },
    { L"Enter",  Type::Key, VK_RETURN },
    { L"Esc",    Type::Key, VK_ESCAPE },
    { L"Tab",    Type::Key, VK_TAB },
    { L"Shift",  Type::Key, VK_SHIFT },
    { L"Ctrl",   Type::Key, VK_CONTROL },
    { L"Alt",    Type::Key, VK_MENU },
    { L"Up",     Type::Key, VK_UP },
    { L"Down",   Type::Key, VK_DOWN },
    { L"Left",   Type::Key, VK_LEFT },
    { L"Right",  Type::Key, VK_RIGHT },
    { L"A", Type::Key, 'A' }, { L"B", Type::Key, 'B' }, { L"C", Type::Key, 'C' },
    { L"D", Type::Key, 'D' }, { L"E", Type::Key, 'E' }, { L"F", Type::Key, 'F' },
    { L"G", Type::Key, 'G' }, { L"H", Type::Key, 'H' }, { L"I", Type::Key, 'I' },
    { L"J", Type::Key, 'J' }, { L"K", Type::Key, 'K' }, { L"L", Type::Key, 'L' },
    { L"M", Type::Key, 'M' }, { L"N", Type::Key, 'N' }, { L"O", Type::Key, 'O' },
    { L"P", Type::Key, 'P' }, { L"Q", Type::Key, 'Q' }, { L"R", Type::Key, 'R' },
    { L"S", Type::Key, 'S' }, { L"T", Type::Key, 'T' }, { L"U", Type::Key, 'U' },
    { L"V", Type::Key, 'V' }, { L"W", Type::Key, 'W' }, { L"X", Type::Key, 'X' },
    { L"Y", Type::Key, 'Y' }, { L"Z", Type::Key, 'Z' },
    { L"0", Type::Key, '0' }, { L"1", Type::Key, '1' }, { L"2", Type::Key, '2' },
    { L"3", Type::Key, '3' }, { L"4", Type::Key, '4' }, { L"5", Type::Key, '5' },
    { L"6", Type::Key, '6' }, { L"7", Type::Key, '7' }, { L"8", Type::Key, '8' },
    { L"9", Type::Key, '9' },
    { L"F1", Type::Key, VK_F1 }, { L"F2", Type::Key, VK_F2 }, { L"F3", Type::Key, VK_F3 },
    { L"F4", Type::Key, VK_F4 }, { L"F5", Type::Key, VK_F5 }, { L"F6", Type::Key, VK_F6 },
};
const int InputMapper::kKeyTargetCount =
    sizeof(InputMapper::kKeyTargets) / sizeof(InputMapper::kKeyTargets[0]);

#undef XB
#undef MO
#undef NONE

static void SendKey(WORD vk, bool down) {
    INPUT in{};
    in.type     = INPUT_KEYBOARD;
    in.ki.wVk   = vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

static void SendMouse(uint16_t btn, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    switch (btn) {
        case InputMapper::MB_LEFT:   in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;   break;
        case InputMapper::MB_RIGHT:  in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;  break;
        case InputMapper::MB_MIDDLE: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case InputMapper::MB_X1:     in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON1; break;
        case InputMapper::MB_X2:     in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON2; break;
        default: return;
    }
    SendInput(1, &in, sizeof(INPUT));
}

// Release whatever edge-triggered output (key or mouse button) a source holds.
static void ReleaseHeld(const InputMapper::Action& a) {
    if (a.type == InputMapper::Type::Key)   SendKey(static_cast<WORD>(a.value), false);
    else if (a.type == InputMapper::Type::Mouse) SendMouse(a.value, false);
}

InputMapper::InputMapper() {
    ResetToDefaults();
}

void InputMapper::ResetToDefaults() {
    for (int i = 0; i < kSourceCount; ++i)
        m_actions[i] = kSources[i].def;
}

void InputMapper::SetAction(int i, Action a) {
    if (i < 0 || i >= kSourceCount) return;
    // If the source had a key/mouse button held, release it before switching.
    if (m_keyDown[i]) {
        ReleaseHeld(m_actions[i]);
        m_keyDown[i] = false;
    }
    m_actions[i] = a;
}

InputMapper::Action InputMapper::GetAction(int i) const {
    if (i < 0 || i >= kSourceCount) return {};
    return m_actions[i];
}

uint16_t InputMapper::Process(const uint8_t* buf, size_t n) {
    uint16_t bits = 0;
    if (n < 6) return bits;

    for (int i = 0; i < kSourceCount; ++i) {
        const bool pressed = (buf[kSources[i].byteIndex] & kSources[i].mask) != 0;
        const Action& a = m_actions[i];
        if (a.type == Type::Xbox) {
            if (pressed) bits = static_cast<uint16_t>(bits | a.value);
        } else if (a.type == Type::Key) {
            if (pressed != m_keyDown[i]) {
                SendKey(static_cast<WORD>(a.value), pressed);
                m_keyDown[i] = pressed;
            }
        } else if (a.type == Type::Mouse) {
            if (pressed != m_keyDown[i]) {
                SendMouse(a.value, pressed);
                m_keyDown[i] = pressed;
            }
        }
    }
    return bits;
}

void InputMapper::ReleaseKeys() {
    for (int i = 0; i < kSourceCount; ++i) {
        if (m_keyDown[i]) {
            ReleaseHeld(m_actions[i]);
            m_keyDown[i] = false;
        }
    }
}
