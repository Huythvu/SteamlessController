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
// hit-testing share the same geometry. Both layouts fill the (square) board.
void KeyboardOverlay::BuildLayout() {
    m_keys.clear();
    struct Cell { std::wstring label; wchar_t ch; WORD vk; float w; int mod; };
    struct Row  { float offset; std::vector<Cell> cells; };  // offset/widths in key units
    std::vector<Row> rows;

    auto C     = [](wchar_t c, float w = 1.0f) -> Cell { return { std::wstring(1, c), c, 0, w, 0 }; };
    auto Sp    = [](const wchar_t* l, WORD vk, float w) -> Cell { return { l, 0, vk, w, 0 }; };
    auto Mod   = [](const wchar_t* l, int mod, float w) -> Cell { return { l, 0, 0, w, mod }; };
    auto chars = [&](const wchar_t* s, std::vector<Cell>& out) {
        for (const wchar_t* p = s; *p; ++p) out.push_back(C(*p));
    };

    if (m_layout == Simple) {
        // Even grid that fills the board completely (the original simple board).
        { Row r{0.0f,{}}; chars(L"1234567890", r.cells); rows.push_back(std::move(r)); }
        { Row r{0.0f,{}}; chars(L"qwertyuiop", r.cells); rows.push_back(std::move(r)); }
        { Row r{0.0f,{}}; chars(L"asdfghjkl", r.cells); r.cells.push_back(Sp(L"<-", VK_BACK, 1.0f)); rows.push_back(std::move(r)); }
        { Row r{0.0f,{}}; chars(L"zxcvbnm,.", r.cells); r.cells.push_back(Sp(L"Enter", VK_RETURN, 1.0f)); rows.push_back(std::move(r)); }
        { Row r{0.0f,{}}; r.cells.push_back(Sp(L"Space", VK_SPACE, 10.0f)); rows.push_back(std::move(r)); }
    } else {
        // ISO-style: staggered, with modifier keys filling the offsets so every
        // row spans the full width (no empty space). 15 units per row.
        { Row r{0.0f,{}}; r.cells.push_back(C(L'`'));
          chars(L"1234567890", r.cells);
          r.cells.push_back(C(L'-')); r.cells.push_back(C(L'='));
          r.cells.push_back(Sp(L"<-", VK_BACK, 2.0f)); rows.push_back(std::move(r)); }
        { Row r{0.0f,{}}; r.cells.push_back(Sp(L"Tab", VK_TAB, 1.5f));
          chars(L"qwertyuiop", r.cells);
          r.cells.push_back(C(L'[')); r.cells.push_back(C(L']'));
          r.cells.push_back(C(L'\\', 1.5f)); rows.push_back(std::move(r)); }
        { Row r{0.0f,{}}; r.cells.push_back(Mod(L"Caps", 2, 1.75f));
          chars(L"asdfghjkl", r.cells);
          r.cells.push_back(C(L';')); r.cells.push_back(C(L'\''));
          r.cells.push_back(Sp(L"Enter", VK_RETURN, 2.25f)); rows.push_back(std::move(r)); }
        { Row r{0.0f,{}}; r.cells.push_back(Mod(L"Shift", 1, 2.0f));
          chars(L"zxcvbnm", r.cells);
          r.cells.push_back(C(L',')); r.cells.push_back(C(L'.')); r.cells.push_back(C(L'/'));
          r.cells.push_back(Mod(L"Shift", 1, 3.0f)); rows.push_back(std::move(r)); }
        { Row r{0.0f,{}}; r.cells.push_back(Sp(L"Space", VK_SPACE, 9.0f));
          r.cells.push_back(Sp(L"Lf", VK_LEFT, 1.5f));
          r.cells.push_back(Sp(L"Up", VK_UP,   1.5f));
          r.cells.push_back(Sp(L"Dn", VK_DOWN, 1.5f));
          r.cells.push_back(Sp(L"Rt", VK_RIGHT,1.5f)); rows.push_back(std::move(r)); }
    }

    // The widest row defines the key unit so every row shares one scale.
    float maxUnits = 0.0f;
    for (const Row& r : rows) {
        float u = r.offset;
        for (const Cell& c : r.cells) u += c.w;
        if (u > maxUnits) maxUnits = u;
    }
    if (maxUnits <= 0.0f) maxUnits = 1.0f;
    const float unit = static_cast<float>(m_w) / maxUnits;

    const int nRows = static_cast<int>(rows.size());
    const int rowH  = m_h / nRows;
    for (int ri = 0; ri < nRows; ++ri) {
        float x = rows[ri].offset * unit;
        const int n = static_cast<int>(rows[ri].cells.size());
        for (int ci = 0; ci < n; ++ci) {
            const Cell& c = rows[ri].cells[ci];
            const float w = c.w * unit;
            Key k;
            k.rc    = { static_cast<int>(x), ri * rowH,
                        (ci == n - 1) ? m_w : static_cast<int>(x + w),
                        (ri == nRows - 1) ? m_h : (ri + 1) * rowH };
            k.label = c.label;
            k.ch    = c.ch;
            k.vk    = c.vk;
            k.mod   = c.mod;
            m_keys.push_back(std::move(k));
            x += w;
        }
    }
}

void KeyboardOverlay::SetLayout(int layout) {
    if (layout != Simple && layout != Iso) layout = Iso;
    if (layout == m_layout && !m_keys.empty()) return;
    m_layout = layout;
    BuildLayout();
    for (int s = 0; s < 2; ++s) {
        float lo, hi; RangeFor(s, lo, hi);
        m_cx[s] = (lo + hi) * 0.5f;
        m_cy[s] = 0.5f;
        UpdateSel(s);
    }
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void KeyboardOverlay::SendKey(WORD vk) {
    Key k{}; k.vk = vk;
    TypeKey(k);
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
    if (m_ball && m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);  // ball moved
}

void KeyboardOverlay::MovePointer(int side, float dnx, float dny) {
    float lo, hi; RangeFor(side, lo, hi);
    float cx = m_cx[side] + dnx;
    float cy = m_cy[side] + dny;
    m_cx[side] = cx < lo ? lo : (cx > hi ? hi : cx);
    m_cy[side] = cy < 0 ? 0 : (cy > 1 ? 1 : cy);
    UpdateSel(side);
    if (m_ball && m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);  // ball moved
}

int KeyboardOverlay::HitAt(float gridX, float ny) const {
    const int px = static_cast<int>(gridX * m_w);
    const int py = static_cast<int>(ny * m_h);
    int  best = -1;
    long bestD2 = 0;
    for (int i = 0; i < static_cast<int>(m_keys.size()); ++i) {
        const RECT& r = m_keys[i].rc;
        if (px >= r.left && px < r.right && py >= r.top && py < r.bottom) return i;
        // Staggered rows leave gaps; snap to the nearest key so there are no
        // dead zones. Distance from the point to the key rectangle.
        const long dx = px < r.left ? r.left - px : (px >= r.right  ? px - r.right  + 1 : 0);
        const long dy = py < r.top  ? r.top  - py : (py >= r.bottom ? py - r.bottom + 1 : 0);
        const long d2 = dx * dx + dy * dy;
        if (best < 0 || d2 < bestD2) { best = i; bestD2 = d2; }
    }
    return best;
}

void KeyboardOverlay::Commit(int side) {
    const int sel = (side == 0) ? m_selL : m_selR;
    if (sel < 0 || sel >= static_cast<int>(m_keys.size())) return;
    const Key& k = m_keys[sel];
    if (k.mod == 1) { m_shift = !m_shift; if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE); return; }
    if (k.mod == 2) { m_caps  = !m_caps;  if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE); return; }
    TypeKey(k);
}

wchar_t KeyboardOverlay::ShiftChar(wchar_t c) {
    if (c >= L'a' && c <= L'z') return static_cast<wchar_t>(c - L'a' + L'A');
    switch (c) {
        case L'1': return L'!'; case L'2': return L'@'; case L'3': return L'#';
        case L'4': return L'$'; case L'5': return L'%'; case L'6': return L'^';
        case L'7': return L'&'; case L'8': return L'*'; case L'9': return L'(';
        case L'0': return L')'; case L'-': return L'_'; case L'=': return L'+';
        case L'[': return L'{'; case L']': return L'}'; case L'\\': return L'|';
        case L';': return L':'; case L'\'': return L'"'; case L',': return L'<';
        case L'.': return L'>'; case L'/': return L'?'; case L'`': return L'~';
    }
    return c;
}

std::wstring KeyboardOverlay::DisplayLabel(int i) const {
    const Key& k = m_keys[i];
    if (k.vk || k.mod) return k.label;
    wchar_t c = k.ch;
    const bool letter = (c >= L'a' && c <= L'z');
    const bool up = letter ? (m_caps != m_shift) : m_shift;
    if (up) c = ShiftChar(c);
    return std::wstring(1, c);
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
        wchar_t c = k.ch;
        const bool letter = (c >= L'a' && c <= L'z');
        const bool up = letter ? (m_caps != m_shift) : m_shift;
        if (up) c = ShiftChar(c);
        in[0].ki.wScan   = c;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1].ki.wScan   = c;
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    }
    SendInput(2, in, sizeof(INPUT));
    if (m_shift) {                       // one-shot shift is consumed by a key
        m_shift = false;
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

bool KeyboardOverlay::IsModKey(int i) const {
    return i >= 0 && i < static_cast<int>(m_keys.size()) && m_keys[i].mod != 0;
}

bool KeyboardOverlay::IsModActive(int i) const {
    if (i < 0 || i >= static_cast<int>(m_keys.size())) return false;
    const int m = m_keys[i].mod;
    return (m == 1 && m_shift) || (m == 2 && m_caps);
}

void KeyboardOverlay::KeyRect(int i, float& l, float& t, float& r, float& b) const {
    const RECT& rc = m_keys[i].rc;
    l = rc.left   / static_cast<float>(m_w);
    t = rc.top    / static_cast<float>(m_h);
    r = rc.right  / static_cast<float>(m_w);
    b = rc.bottom / static_cast<float>(m_h);
}

std::string KeyboardOverlay::KeyLabel(int i) const {
    const std::wstring w = DisplayLabel(i);
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
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
    HBRUSH modBrush  = CreateSolidBrush(RGB(200, 160, 40));   // active shift/caps
    HPEN   pen      = CreatePen(PS_SOLID, 1, RGB(80, 84, 92));
    HPEN   oldPen   = static_cast<HPEN>(SelectObject(mem, pen));
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(235, 237, 240));

    // In ball mode the keys stay neutral and a floating cursor marks the spot;
    // the hovered key only gets a coloured outline. Otherwise we fill the whole
    // hovered key as before.
    for (int i = 0; i < static_cast<int>(m_keys.size()); ++i) {
        RECT r = m_keys[i].rc;
        InflateRect(&r, -3, -3);
        const bool selL = (i == m_selL), selR = (i == m_selR);
        const bool sel  = selL || selR;
        HBRUSH b = keyBrush;
        if (sel && !m_ball) {
            if (selL && selR) b = bothBrush;
            else if (selL)    b = leftBrush;
            else              b = rightBrush;
        } else if (IsModActive(i)) {
            b = modBrush;
        }
        HBRUSH ob = static_cast<HBRUSH>(SelectObject(mem, b));
        RoundRect(mem, r.left, r.top, r.right, r.bottom, 10, 10);
        SelectObject(mem, ob);
        if (m_ball && sel) {
            COLORREF c = (selL && selR) ? RGB(150, 110, 230)
                       : selL           ? RGB(60, 170, 90)
                                        : RGB(60, 150, 240);
            HPEN hp  = CreatePen(PS_SOLID, 3, c);
            HPEN op  = static_cast<HPEN>(SelectObject(mem, hp));
            HBRUSH hb = static_cast<HBRUSH>(SelectObject(mem, GetStockObject(HOLLOW_BRUSH)));
            RoundRect(mem, r.left, r.top, r.right, r.bottom, 10, 10);
            SelectObject(mem, hb);
            SelectObject(mem, op);
            DeleteObject(hp);
        }
        const std::wstring lbl = DisplayLabel(i);
        DrawTextW(mem, lbl.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Floating cursors ("balls"), one per pad, at the continuous pointer spot.
    if (m_ball) {
        const int rad = 14;
        HBRUSH ballBrush[2] = { leftBrush, rightBrush };
        HPEN   ringPen = CreatePen(PS_SOLID, 2, RGB(235, 237, 240));
        HPEN   op = static_cast<HPEN>(SelectObject(mem, ringPen));
        for (int s = 0; s < 2; ++s) {
            const int cx = static_cast<int>(m_cx[s] * m_w);
            const int cy = static_cast<int>(m_cy[s] * m_h);
            HBRUSH ob = static_cast<HBRUSH>(SelectObject(mem, ballBrush[s]));
            Ellipse(mem, cx - rad, cy - rad, cx + rad, cy + rad);
            SelectObject(mem, ob);
        }
        SelectObject(mem, op);
        DeleteObject(ringPen);
    }

    BitBlt(hdc, 0, 0, m_w, m_h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldPen);
    SelectObject(mem, old);
    DeleteObject(pen);
    DeleteObject(keyBrush);
    DeleteObject(leftBrush);
    DeleteObject(rightBrush);
    DeleteObject(bothBrush);
    DeleteObject(modBrush);
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
