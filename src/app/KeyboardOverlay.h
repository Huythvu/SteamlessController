#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include <vector>

// A lightweight on-screen keyboard overlay (GDI). It is topmost and never takes
// focus (WS_EX_NOACTIVATE), so the keystrokes it injects land in whatever app
// the user was typing into. The controller drives it: the trackpad points at a
// key (SetPointer) and a click commits it (Commit).
class KeyboardOverlay {
public:
    bool Init(HINSTANCE hInstance);
    void Show();
    void Hide();
    bool IsVisible() const { return m_visible; }

    // Each trackpad drives its own pointer. Split mode confines a pad to its
    // half of the board; otherwise each pad can reach the whole board.
    void SetSplit(bool split) { m_split = split; }
    // Ball mode: draw a floating cursor per pad instead of filling the whole
    // hovered key (Steam-style). Selection/typing is unchanged.
    void SetBallMode(bool ball) { m_ball = ball; }
    // The key index each side is currently pointing at (-1 = none).
    int  Selected(int side) const { return side == 0 ? m_selL : m_selR; }
    // Absolute: the pad position maps straight to a board position.
    void SetPointerAbs(int side, float nx, float ny);
    // Relative: slide from anywhere; the pointer moves by the finger delta.
    void MovePointer(int side, float dnx, float dny);
    // Type the highlighted key for one side into the focused application.
    void Commit(int side);
    // Inject a single virtual-key press (used by the remappable Space /
    // Backspace / Enter shortcut buttons).
    void SendKey(WORD vk);
    HWND Hwnd() const { return m_hwnd; }

private:
    struct Key { RECT rc; std::wstring label; wchar_t ch; WORD vk; };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void BuildLayout();
    void Paint(HDC hdc);
    int  HitAt(float gridX, float ny) const;
    void RangeFor(int side, float& lo, float& hi) const;
    void UpdateSel(int side);
    static void TypeKey(const Key& k);

    HINSTANCE         m_hinst   = nullptr;
    HWND              m_hwnd    = nullptr;
    bool              m_visible = false;
    bool              m_split   = true;
    bool              m_ball    = false;  // draw floating cursors instead of key fills
    int               m_selL    = -1;   // left pad's highlighted key
    int               m_selR    = -1;   // right pad's highlighted key
    float             m_cx[2]   = { 0.25f, 0.75f };  // pointer position per side
    float             m_cy[2]   = { 0.5f,  0.5f  };
    int               m_w       = 920;
    int               m_h       = 340;
    std::vector<Key>  m_keys;
};
