#include "KeyboardOverlay.h"

static constexpr wchar_t KB_CLASS[] = L"SteamlessControllerKeyboard";

bool KeyboardOverlay::Init(HINSTANCE hInstance) {
    m_hinst = hInstance;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = KB_CLASS;
    wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = nullptr;   // we paint everything ourselves
    if (!RegisterClassExW(&wc)) return false;
    BuildLayout();
    return true;
}

// Build the key grid and pre-compute each key's pixel rectangle so painting and
// hit-testing share the same geometry.
void KeyboardOverlay::BuildLayout() {
    m_keys.clear();
    struct Cell { std::wstring label; wchar_t ch; WORD vk; };
    std::vector<std::vector<Cell>> rows;

    auto rowFromChars = [](const wchar_t* s) {
        std::vector<Cell> r;
        for (const wchar_t* p = s; *p; ++p) r.push_back({ std::wstring(1, *p), *p, 0 });
        return r;
    };

    rows.push_back(rowFromChars(L"1234567890"));
    rows.push_back(rowFromChars(L"qwertyuiop"));
    {
        auto r = rowFromChars(L"asdfghjkl");
        r.push_back({ L"<-", 0, VK_BACK });
        rows.push_back(r);
    }
    {
        auto r = rowFromChars(L"zxcvbnm,.");
        r.push_back({ L"Enter", 0, VK_RETURN });
        rows.push_back(r);
    }
    rows.push_back({ { L"Space", L' ', 0 } });

    const int nRows = static_cast<int>(rows.size());
    const int rowH  = m_h / nRows;
    for (int ri = 0; ri < nRows; ++ri) {
        const int n    = static_cast<int>(rows[ri].size());
        const int keyW = m_w / n;
        for (int ci = 0; ci < n; ++ci) {
            Key k;
            k.rc    = { ci * keyW, ri * rowH,
                        (ci == n - 1) ? m_w : (ci + 1) * keyW,
                        (ri == nRows - 1) ? m_h : (ri + 1) * rowH };
            k.label = rows[ri][ci].label;
            k.ch    = rows[ri][ci].ch;
            k.vk    = rows[ri][ci].vk;
            m_keys.push_back(std::move(k));
        }
    }
}

void KeyboardOverlay::Show() {
    if (!m_hwnd) {
        const int sw = GetSystemMetrics(SM_CXSCREEN);
        const int sh = GetSystemMetrics(SM_CYSCREEN);
        const int x  = (sw - m_w) / 2;
        const int y  = sh - m_h - 80;     // near the bottom
        m_hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            KB_CLASS, L"Keyboard", WS_POPUP,
            x, y, m_w, m_h, nullptr, nullptr, m_hinst, nullptr);
        if (!m_hwnd) return;
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    // Start each pointer centred in its range.
    for (int s = 0; s < 2; ++s) {
        float lo, hi; RangeFor(s, lo, hi);
        m_cx[s] = (lo + hi) * 0.5f;
        m_cy[s] = 0.5f;
        UpdateSel(s);
    }
    // SW_SHOWNOACTIVATE keeps focus on the app being typed into.
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    m_visible = true;
}

void KeyboardOverlay::Hide() {
    if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
    m_visible = false;
}

void KeyboardOverlay::RangeFor(int side, float& lo, float& hi) const {
    if (m_split) {
        // Split exactly at the midpoint. Nudge the left side's upper bound just
        // under 0.5 so it lands in the last left-half key, not the first
        // right-half one (which sits exactly on the midpoint column).
        const float eps = 1.0f / static_cast<float>(m_w);
        lo = (side == 0) ? 0.0f : 0.5f;
        hi = (side == 0) ? 0.5f - eps : 1.0f;
    } else {
        lo = 0.0f; hi = 1.0f;
    }
}

void KeyboardOverlay::UpdateSel(int side) {
    const int hit = HitAt(m_cx[side], m_cy[side]);
    int& sel = (side == 0) ? m_selL : m_selR;
    if (hit != sel) {
        sel = hit;
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void KeyboardOverlay::SetPointerAbs(int side, float nx, float ny) {
    nx = nx < 0 ? 0 : (nx > 1 ? 1 : nx);
    ny = ny < 0 ? 0 : (ny > 1 ? 1 : ny);
    float lo, hi; RangeFor(side, lo, hi);
    m_cx[side] = lo + nx * (hi - lo);   // map pad position into the side's range
    m_cy[side] = ny;
    UpdateSel(side);
}

void KeyboardOverlay::MovePointer(int side, float dnx, float dny) {
    float lo, hi; RangeFor(side, lo, hi);
    float cx = m_cx[side] + dnx;
    float cy = m_cy[side] + dny;
    m_cx[side] = cx < lo ? lo : (cx > hi ? hi : cx);
    m_cy[side] = cy < 0 ? 0 : (cy > 1 ? 1 : cy);
    UpdateSel(side);
}

int KeyboardOverlay::HitAt(float gridX, float ny) const {
    const int px = static_cast<int>(gridX * m_w);
    const int py = static_cast<int>(ny * m_h);
    for (int i = 0; i < static_cast<int>(m_keys.size()); ++i) {
        const RECT& r = m_keys[i].rc;
        if (px >= r.left && px < r.right && py >= r.top && py < r.bottom) return i;
    }
    return -1;
}

void KeyboardOverlay::Commit(int side) {
    const int sel = (side == 0) ? m_selL : m_selR;
    if (sel >= 0 && sel < static_cast<int>(m_keys.size()))
        TypeKey(m_keys[sel]);
}

void KeyboardOverlay::TypeKey(const Key& k) {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[1].type = INPUT_KEYBOARD;
    if (k.vk) {
        in[0].ki.wVk = k.vk;
        in[1].ki.wVk = k.vk;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    } else {
        in[0].ki.wScan   = k.ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1].ki.wScan   = k.ch;
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    }
    SendInput(2, in, sizeof(INPUT));
}

void KeyboardOverlay::Paint(HDC hdc) {
    // Double-buffer to avoid flicker on the frequent repaints.
    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, m_w, m_h);
    HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp));

    RECT full{ 0, 0, m_w, m_h };
    HBRUSH bg = CreateSolidBrush(RGB(24, 25, 28));
    FillRect(mem, &full, bg);
    DeleteObject(bg);

    HBRUSH keyBrush  = CreateSolidBrush(RGB(48, 50, 56));
    HBRUSH leftBrush = CreateSolidBrush(RGB(60, 170, 90));    // left pad pointer
    HBRUSH rightBrush= CreateSolidBrush(RGB(60, 150, 240));   // right pad pointer
    HBRUSH bothBrush = CreateSolidBrush(RGB(150, 110, 230));  // both on same key
    HPEN   pen      = CreatePen(PS_SOLID, 1, RGB(80, 84, 92));
    HPEN   oldPen   = static_cast<HPEN>(SelectObject(mem, pen));
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(235, 237, 240));

    for (int i = 0; i < static_cast<int>(m_keys.size()); ++i) {
        RECT r = m_keys[i].rc;
        InflateRect(&r, -3, -3);
        HBRUSH b = keyBrush;
        if (i == m_selL && i == m_selR) b = bothBrush;
        else if (i == m_selL)           b = leftBrush;
        else if (i == m_selR)           b = rightBrush;
        HBRUSH ob = static_cast<HBRUSH>(SelectObject(mem, b));
        RoundRect(mem, r.left, r.top, r.right, r.bottom, 10, 10);
        SelectObject(mem, ob);
        DrawTextW(mem, m_keys[i].label.c_str(), -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    BitBlt(hdc, 0, 0, m_w, m_h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldPen);
    SelectObject(mem, old);
    DeleteObject(pen);
    DeleteObject(keyBrush);
    DeleteObject(leftBrush);
    DeleteObject(rightBrush);
    DeleteObject(bothBrush);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK KeyboardOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<KeyboardOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (self) self->Paint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // handled in Paint (no flicker)
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;   // never steal focus on click
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
