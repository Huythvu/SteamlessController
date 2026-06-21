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

    // Each trackpad drives its own half: side 0 = left pad -> left half of the
    // keyboard, side 1 = right pad -> right half. Each has its own pointer.
    void SetPointer(int side, float nx, float ny);
    // Type the highlighted key for one side into the focused application.
    void Commit(int side);
    HWND Hwnd() const { return m_hwnd; }

private:
    struct Key { RECT rc; std::wstring label; wchar_t ch; WORD vk; };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void BuildLayout();
    void Paint(HDC hdc);
    int  HitAt(float gridX, float ny) const;
    static void TypeKey(const Key& k);

    HINSTANCE         m_hinst   = nullptr;
    HWND              m_hwnd    = nullptr;
    bool              m_visible = false;
    int               m_selL    = -1;   // left pad's highlighted key
    int               m_selR    = -1;   // right pad's highlighted key
    int               m_w       = 820;
    int               m_h       = 320;
    std::vector<Key>  m_keys;
};
