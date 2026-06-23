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
    enum Layout { Simple = 0, Iso = 1 };

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
    // Pick the key layout (rebuilds the grid).
    void SetLayout(int layout);
    int  GetLayout() const { return m_layout; }
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
    // Accent labels for the controller button mapped to Backspace / Space /
    // Enter, drawn in the corner of those keys (empty = none).
    void SetShortcutLabels(const std::wstring& back, const std::wstring& space, const std::wstring& enter) {
        m_lblBack = back; m_lblSpace = space; m_lblEnter = enter;
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    HWND Hwnd() const { return m_hwnd; }

    // --- geometry/state for the live preview drawn in the settings tab ---
    int  KeyCount() const { return static_cast<int>(m_keys.size()); }
    void KeyRect(int i, float& l, float& t, float& r, float& b) const;  // normalized 0..1
    // For the L-shaped ISO Enter: normalized stem rect (the lower part). Returns
    // false for ordinary keys.
    bool KeyStem(int i, float& l, float& t, float& r, float& b) const;
    std::string KeyLabel(int i) const;          // display label (honours shift/caps), UTF-8
    bool IsModKey(int i) const;                 // shift/caps key
    bool IsModActive(int i) const;              // that modifier is currently on
    WORD KeyVk(int i) const { return (i >= 0 && i < static_cast<int>(m_keys.size())) ? m_keys[i].vk : 0; }
    float CursorX(int side) const { return m_cx[side]; }
    float CursorY(int side) const { return m_cy[side]; }
    float AspectRatio() const { return static_cast<float>(m_w) / static_cast<float>(m_h); }

private:
    // mod: 0 = normal, 1 = sticky Shift (one-shot), 2 = Caps toggle.
    // lshape: an ISO Enter; rc is the top bar and stem is the lower-right part.
    struct Key { RECT rc; RECT stem; std::wstring label; wchar_t ch; WORD vk; int mod; bool lshape; };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void BuildLayout();
    void Paint(HDC hdc);
    int  HitAt(float gridX, float ny) const;
    void RangeFor(int side, float& lo, float& hi) const;
    void UpdateSel(int side);
    void TypeKey(const Key& k);              // applies shift/caps, then clears shift
    std::wstring DisplayLabel(int i) const;  // wide version of KeyLabel
    static wchar_t ShiftChar(wchar_t c);     // shifted form of a character

    HINSTANCE         m_hinst   = nullptr;
    HWND              m_hwnd    = nullptr;
    bool              m_visible = false;
    bool              m_split   = true;
    bool              m_ball    = false;  // draw floating cursors instead of key fills
    int               m_layout  = Iso;
    bool              m_shift   = false;  // one-shot shift pending
    bool              m_caps    = false;  // caps lock
    int               m_selL    = -1;   // left pad's highlighted key
    int               m_selR    = -1;   // right pad's highlighted key
    float             m_cx[2]   = { 0.25f, 0.75f };  // pointer position per side
    float             m_cy[2]   = { 0.5f,  0.5f  };
    int               m_w       = 900;   // keyboard-shaped (wider than tall)
    int               m_h       = 330;
    std::vector<Key>  m_keys;
    std::wstring      m_lblBack, m_lblSpace, m_lblEnter;   // mapped-button accents
};
