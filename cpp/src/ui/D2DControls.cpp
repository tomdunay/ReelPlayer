#include "ui/D2DControls.h"
#include "core/MediaFiles.h"
#include "core/StrUtil.h"
#include "core/TimeFormat.h"
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace reel {

static inline D2D1_COLOR_F rgba(uint32_t hex, float a = 1.0f) {
    return D2D1::ColorF(
        ((hex >> 16) & 0xFF) / 255.0f,
        ((hex >> 8) & 0xFF) / 255.0f,
        (hex & 0xFF) / 255.0f,
        a);
}

static inline bool ptInRect(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

template <typename T>
static inline void safeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static inline float dpiScaleFor(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

// ============================================================================
// D2DContext
// ============================================================================

bool D2DContext::init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite))))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));

    auto makeFont = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out,
                        DWRITE_TEXT_ALIGNMENT halign = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (SUCCEEDED(dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out)) && *out) {
            (*out)->SetTextAlignment(halign);
            (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    };

    makeFont(L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_NORMAL, &fontUi);
    makeFont(L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fontUiBold);
    makeFont(L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, &fontSmall);
    makeFont(L"Segoe UI", 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fontTitle, DWRITE_TEXT_ALIGNMENT_CENTER);
    makeFont(L"Segoe UI", 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fontOsd, DWRITE_TEXT_ALIGNMENT_CENTER);

    // Segoe Fluent Icons (Win11) falls back cleanly to Segoe MDL2 Assets (Win10).
    makeFont(L"Segoe Fluent Icons", 15.0f, DWRITE_FONT_WEIGHT_NORMAL, &fontIcon, DWRITE_TEXT_ALIGNMENT_CENTER);
    makeFont(L"Segoe Fluent Icons", 16.5f, DWRITE_FONT_WEIGHT_NORMAL, &fontIconBig, DWRITE_TEXT_ALIGNMENT_CENTER);
    makeFont(L"Segoe Fluent Icons", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, &fontIconSmall, DWRITE_TEXT_ALIGNMENT_CENTER);
    return true;
}

void D2DContext::shutdown() {
    safeRelease(fontUi);
    safeRelease(fontUiBold);
    safeRelease(fontSmall);
    safeRelease(fontTitle);
    safeRelease(fontOsd);
    safeRelease(fontIcon);
    safeRelease(fontIconBig);
    safeRelease(fontIconSmall);
    safeRelease(wic);
    safeRelease(dwrite);
    safeRelease(d2d);
}

// ============================================================================
// D2DMenuBar
// ============================================================================

D2DMenuBar::D2DMenuBar(D2DContext& ctx) : _ctx(ctx) {}

D2DMenuBar::~D2DMenuBar() {
    safeRelease(_rt);
    if (_hwnd) DestroyWindow(_hwnd);
}

bool D2DMenuBar::create(HWND parent, HINSTANCE hInst) {
    _owner = parent;
    static bool registered = false;
    const wchar_t* cls = L"ReelD2DMenuBar";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &D2DMenuBar::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        registered = true;
    }
    _hwnd = CreateWindowExW(0, cls, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                            0, 0, 800, BarHeight, parent, nullptr, hInst, this);
    return _hwnd != nullptr;
}

void D2DMenuBar::setDarkTheme(bool dark) {
    _dark = dark;
    redraw();
}

void D2DMenuBar::setMenus(std::vector<std::pair<std::wstring, HMENU>> menus) {
    _menus = std::move(menus);
    computeLayout();
    redraw();
}

void D2DMenuBar::redraw() {
    if (_hwnd && IsWindowVisible(_hwnd)) InvalidateRect(_hwnd, nullptr, FALSE);
}

void D2DMenuBar::ensureTarget() {
    if (_rt || !_hwnd || !_ctx.d2d) return;
    RECT rc{};
    GetClientRect(_hwnd, &rc);
    if (SUCCEEDED(_ctx.d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom))),
            &_rt)) && _rt) {
        float dpi = dpiScaleFor(_hwnd) * 96.0f;
        _rt->SetDpi(dpi, dpi);
    }
}

void D2DMenuBar::computeLayout() {
    _rects.clear();
    float x = 8.0f;
    for (const auto& [label, hm] : _menus) {
        float w = static_cast<float>(label.size()) * 7.2f + 18.0f;
        _rects.push_back(D2D1::RectF(x, 3.0f, x + w, static_cast<float>(BarHeight - 3)));
        x += w + 2.0f;
    }
}

void D2DMenuBar::paint() {
    ensureTarget();
    if (!_rt) return;
    float dpi = dpiScaleFor(_hwnd) * 96.0f;
    _rt->SetDpi(dpi, dpi);
    D2D1_SIZE_F sz = _rt->GetSize();
    if (sz.width < 10 || sz.height < 10) return;

    _rt->BeginDraw();
    const D2D1_COLOR_F bg = _dark ? rgba(0x16161B) : rgba(0xF3F4F6);
    const D2D1_COLOR_F fg = _dark ? rgba(0xE4E4E7) : rgba(0x18181B);
    const D2D1_COLOR_F hairline = _dark ? rgba(0xFFFFFF, 0.10f) : rgba(0x000000, 0.10f);
    _rt->Clear(bg);

    ID2D1SolidColorBrush* br = nullptr;
    _rt->CreateSolidColorBrush(fg, &br);

    br->SetColor(hairline);
    _rt->DrawLine(D2D1::Point2F(0, sz.height - 0.5f), D2D1::Point2F(sz.width, sz.height - 0.5f), br, 1.0f);

    _ctx.fontUi->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    for (int i = 0; i < static_cast<int>(_menus.size()) && i < static_cast<int>(_rects.size()); ++i) {
        if (i == _openIdx || i == _hoverIdx) {
            br->SetColor(rgba(0x808080, i == _openIdx ? 0.28f : 0.18f));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(_rects[i], 5.0f, 5.0f), br);
        }
        br->SetColor(fg);
        _rt->DrawText(_menus[i].first.c_str(), static_cast<UINT32>(_menus[i].first.size()), _ctx.fontUi, _rects[i], br);
    }
    _ctx.fontUi->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    safeRelease(br);
    if (_rt->EndDraw() == D2DERR_RECREATE_TARGET) safeRelease(_rt);
}

void D2DMenuBar::openMenuAt(int index) {
    if (index < 0 || index >= static_cast<int>(_menus.size())) return;
    _openIdx = index;
    redraw();
    float scale = dpiScaleFor(_hwnd);
    POINT pt = { static_cast<LONG>(_rects[index].left * scale), static_cast<LONG>(BarHeight * scale) };
    ClientToScreen(_hwnd, &pt);
    TrackPopupMenu(_menus[index].second, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, _owner, nullptr);
    _openIdx = -1;
    redraw();
}

LRESULT CALLBACK D2DMenuBar::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    D2DMenuBar* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<D2DMenuBar*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->_hwnd = hwnd;
    } else {
        self = reinterpret_cast<D2DMenuBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handleMsg(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT D2DMenuBar::handleMsg(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(_hwnd, &ps);
            paint();
            EndPaint(_hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (_rt) _rt->Resize(D2D1::SizeU(std::max<UINT>(1, LOWORD(lp)), std::max<UINT>(1, HIWORD(lp))));
            redraw();
            return 0;
        case WM_MOUSEMOVE: {
            if (!_mouseInside) {
                _mouseInside = true;
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, _hwnd, 0 };
                TrackMouseEvent(&tme);
            }
            float scale = dpiScaleFor(_hwnd);
            float x = static_cast<float>(GET_X_LPARAM(lp)) / scale;
            float y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            int idx = -1;
            for (int i = 0; i < static_cast<int>(_rects.size()); ++i)
                if (ptInRect(_rects[i], x, y)) { idx = i; break; }
            SetCursor(LoadCursorW(nullptr, idx >= 0 ? IDC_HAND : IDC_ARROW));
            if (idx != _hoverIdx) { _hoverIdx = idx; redraw(); }
            return 0;
        }
        case WM_MOUSELEAVE:
            _mouseInside = false;
            _hoverIdx = -1;
            redraw();
            return 0;
        case WM_LBUTTONDOWN: {
            float scale = dpiScaleFor(_hwnd);
            float x = static_cast<float>(GET_X_LPARAM(lp)) / scale;
            float y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            for (int i = 0; i < static_cast<int>(_rects.size()); ++i) {
                if (ptInRect(_rects[i], x, y)) { openMenuAt(i); return 0; }
            }
            return 0;
        }
    }
    return DefWindowProcW(_hwnd, msg, wp, lp);
}

// ============================================================================
// D2DControlBar
// ============================================================================

D2DControlBar::D2DControlBar(D2DContext& ctx, bool floatingPill)
    : _ctx(ctx), _floating(floatingPill) {}

D2DControlBar::~D2DControlBar() {
    releaseTarget();
    if (_hwnd) DestroyWindow(_hwnd);
}

bool D2DControlBar::create(HWND parent, HINSTANCE hInst) {
    static bool registered = false;
    const wchar_t* cls = L"ReelD2DControlBar";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &D2DControlBar::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        registered = true;
    }

    DWORD style = _floating ? (WS_POPUP) : (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
    DWORD exStyle = _floating ? (WS_EX_LAYERED | WS_EX_NOACTIVATE) : 0;
    _hwnd = CreateWindowExW(exStyle, cls, L"", style, 0, 0, 800, BarHeight, parent, nullptr, hInst, this);
    if (_hwnd && _floating) {
        SetLayeredWindowAttributes(_hwnd, 0, 240, LWA_ALPHA);
    }
    return _hwnd != nullptr;
}

void D2DControlBar::setState(const ControlBarState& s) {
    _state = s;
    redraw();
}

void D2DControlBar::setOpacity(BYTE alpha) {
    if (_hwnd && _floating) SetLayeredWindowAttributes(_hwnd, 0, alpha, LWA_ALPHA);
}

void D2DControlBar::redraw() {
    if (_hwnd && IsWindowVisible(_hwnd)) InvalidateRect(_hwnd, nullptr, FALSE);
}

void D2DControlBar::ensureTarget() {
    if (_rt || !_hwnd || !_ctx.d2d) return;
    RECT rc{};
    GetClientRect(_hwnd, &rc);
    D2D1_SIZE_U sz = D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
    if (SUCCEEDED(_ctx.d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(_hwnd, sz),
            &_rt)) && _rt) {
        float dpi = dpiScaleFor(_hwnd) * 96.0f;
        _rt->SetDpi(dpi, dpi);
    }
}

void D2DControlBar::releaseTarget() { safeRelease(_rt); }

int64_t D2DControlBar::msFromX(float x) const {
    float usable = _seekRect.right - _seekRect.left - 12.0f;
    if (usable <= 1.0f || _state.lengthMs <= 0) return 0;
    float frac = std::clamp((x - (_seekRect.left + 6.0f)) / usable, 0.0f, 1.0f);
    return static_cast<int64_t>(frac * static_cast<double>(_state.lengthMs));
}

int D2DControlBar::volFromX(float x) const {
    float usable = _volRect.right - _volRect.left - 10.0f;
    if (usable <= 1.0f) return _state.volume;
    float frac = std::clamp((x - (_volRect.left + 5.0f)) / usable, 0.0f, 1.0f);
    return std::clamp(static_cast<int>(std::lround(frac * 200.0f)), 0, 200);
}

std::wstring D2DControlBar::chapterAt(int64_t ms) const {
    std::wstring name;
    for (const auto& c : _state.chapters) {
        if (c.startMs <= ms) name = c.name;
        else break;
    }
    return name;
}

void D2DControlBar::computeLayout(float w, float h) {
    _buttons.clear();
    const float padX = 14.0f;
    const float timeW = 54.0f;
    // Row 1: Seek bar (y: 8..28)
    _seekRect = D2D1::RectF(padX + timeW + 6.0f, 6.0f, w - padX - timeW - 6.0f, 28.0f);
    _totalTimeRect = D2D1::RectF(w - padX - timeW, 6.0f, w - padX, 28.0f);

    // Row 2: Controls (y: 32..70)
    const float rowCy = 52.0f;
    const float btnH = 32.0f;
    const float btnW = 34.0f;

    // Center transport cluster: Prev | Play(38x38) | Stop | Next
    const float cx = w * 0.5f;
    _buttons.push_back({ D2D1::RectF(cx - 19.0f, rowCy - 19.0f, cx + 19.0f, rowCy + 19.0f), "PlayPause", L"Play / Pause" });
    _buttons.push_back({ D2D1::RectF(cx - 19.0f - 40.0f, rowCy - btnH * 0.5f, cx - 19.0f - 6.0f, rowCy + btnH * 0.5f), "Previous", L"Previous" });
    _buttons.push_back({ D2D1::RectF(cx + 19.0f + 6.0f, rowCy - btnH * 0.5f, cx + 19.0f + 40.0f, rowCy + btnH * 0.5f), "Stop", L"Stop" });
    _buttons.push_back({ D2D1::RectF(cx + 19.0f + 42.0f, rowCy - btnH * 0.5f, cx + 19.0f + 76.0f, rowCy + btnH * 0.5f), "Next", L"Next" });

    // Left cluster: Loop | [Speed pill] | [A-B pill] | [Resume pill + X]
    float lx = padX;
    _buttons.push_back({ D2D1::RectF(lx, rowCy - btnH * 0.5f, lx + btnW, rowCy + btnH * 0.5f), "CycleLoop", L"Loop" });
    lx += btnW + 6.0f;

    if (std::abs(_state.rate - 1.0f) > 0.001f) {
        float pw = 48.0f;
        _buttons.push_back({ D2D1::RectF(lx, rowCy - 12.0f, lx + pw, rowCy + 12.0f), "NormalSpeed", L"Reset speed" });
        lx += pw + 6.0f;
    }
    if (_state.abStartMs.has_value()) {
        float pw = _state.abEndMs.has_value() ? 112.0f : 68.0f;
        _buttons.push_back({ D2D1::RectF(lx, rowCy - 12.0f, lx + pw, rowCy + 12.0f), "AbLoop", L"A-B Loop" });
        lx += pw + 6.0f;
    }
    if (_state.hasResumeOffer && lx + 148.0f < cx - 64.0f) {
        float pw = 122.0f;
        _buttons.push_back({ D2D1::RectF(lx, rowCy - 13.0f, lx + pw, rowCy + 13.0f), "AcceptResume", L"Continue" });
        _buttons.push_back({ D2D1::RectF(lx + pw + 2.0f, rowCy - 13.0f, lx + pw + 24.0f, rowCy + 13.0f), "DismissResume", L"Dismiss" });
    }

    // Right cluster (right-to-left): Fullscreen | Effects | Snapshot | | Vol% | VolSlider | Mute
    float rx = w - padX;
    auto pushRight = [&](const char* act, const wchar_t* tip) {
        rx -= btnW;
        _buttons.push_back({ D2D1::RectF(rx, rowCy - btnH * 0.5f, rx + btnW, rowCy + btnH * 0.5f), act, tip });
        rx -= 2.0f;
    };
    pushRight("Fullscreen", L"Fullscreen");
    pushRight("Effects", L"Effects");
    pushRight("Snapshot", L"Snapshot");
    rx -= 10.0f; // divider
    rx -= 36.0f; // volume % text
    const float volW = 86.0f;
    _volRect = D2D1::RectF(rx - volW, rowCy - 10.0f, rx, rowCy + 10.0f);
    rx -= volW + 4.0f;
    pushRight("Mute", L"Mute");
}

void D2DControlBar::paint() {
    ensureTarget();
    if (!_rt) return;

    float dpi = dpiScaleFor(_hwnd) * 96.0f;
    _rt->SetDpi(dpi, dpi);
    D2D1_SIZE_F sz = _rt->GetSize();
    float w = sz.width;
    float h = sz.height;
    if (w < 10 || h < 10) return;

    computeLayout(w, h);

    _rt->BeginDraw();
    const bool dark = _state.darkTheme || _floating;
    const D2D1_COLOR_F bgColor = _floating ? rgba(0x14141A, 0.96f) : (dark ? rgba(0x16161B) : rgba(0xF3F4F6));
    const D2D1_COLOR_F textColor = dark ? rgba(0xF4F4F6) : rgba(0x18181B);
    const D2D1_COLOR_F mutedText = dark ? rgba(0xA1A1AA) : rgba(0x52525B);
    const D2D1_COLOR_F hairline = dark ? rgba(0xFFFFFF, 0.12f) : rgba(0x000000, 0.12f);
    const D2D1_COLOR_F teal = rgba(0x14B8A6);
    const D2D1_COLOR_F tealHover = rgba(0x2DD4BF);
    const D2D1_COLOR_F amber = rgba(0xFFD166);

    _rt->Clear(bgColor);

    ID2D1SolidColorBrush* br = nullptr;
    _rt->CreateSolidColorBrush(textColor, &br);

    // Top hairline (or rounded border in floating pill)
    if (_floating) {
        br->SetColor(rgba(0xFFFFFF, 0.18f));
        _rt->DrawRectangle(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), br, 1.0f);
    } else {
        br->SetColor(hairline);
        _rt->DrawLine(D2D1::Point2F(0, 0.5f), D2D1::Point2F(w, 0.5f), br, 1.0f);
    }

    // Elapsed & Total time
    std::wstring elapsed = _state.lengthMs > 0 || _state.positionMs > 0 ? TimeFormat::format(_state.positionMs) : L"--:--";
    std::wstring total = _state.lengthMs <= 0 ? L"--:--"
        : (_state.showRemaining ? (L"-" + TimeFormat::format(std::max<int64_t>(0, _state.lengthMs - _state.positionMs)))
                                : TimeFormat::format(_state.lengthMs));

    br->SetColor(mutedText);
    _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    _rt->DrawText(elapsed.c_str(), static_cast<UINT32>(elapsed.size()), _ctx.fontSmall,
                  D2D1::RectF(14.0f, 6.0f, 68.0f, 28.0f), br);
    _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    _rt->DrawText(total.c_str(), static_cast<UINT32>(total.size()), _ctx.fontSmall, _totalTimeRect, br);
    _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    // Seek bar (slim 3px at rest, thickens to 6px on hover/drag)
    const bool seekHot = _seekHover || _seekDragging;
    const float trackH = seekHot ? 6.0f : 3.0f;
    const float trackY = (_seekRect.top + _seekRect.bottom) * 0.5f;
    const float trackL = _seekRect.left + 6.0f;
    const float trackR = _seekRect.right - 6.0f;
    const float usable = std::max(1.0f, trackR - trackL);
    const float frac = (_state.lengthMs > 0) ? std::clamp(static_cast<float>(_state.positionMs) / static_cast<float>(_state.lengthMs), 0.0f, 1.0f) : 0.0f;
    const float fillX = trackL + frac * usable;

    br->SetColor(rgba(0x90909A, 0.35f));
    _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(trackL, trackY - trackH * 0.5f, trackR, trackY + trackH * 0.5f), trackH * 0.5f, trackH * 0.5f), br);

    if (frac > 0.0f) {
        br->SetColor(seekHot ? tealHover : teal);
        _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(trackL, trackY - trackH * 0.5f, fillX, trackY + trackH * 0.5f), trackH * 0.5f, trackH * 0.5f), br);
    }

    // Chapter ticks
    if (_state.chapters.size() > 1) {
        br->SetColor(amber);
        for (size_t i = 1; i < _state.chapters.size(); ++i) {
            float cxMark = trackL + static_cast<float>(_state.chapters[i].fraction) * usable;
            _rt->FillRectangle(D2D1::RectF(cxMark - 1.0f, trackY - 4.0f, cxMark + 1.0f, trackY + 4.0f), br);
        }
    }

    // A-B Loop span & ticks
    if (_state.lengthMs > 0 && _state.abStartMs.has_value()) {
        float ax = trackL + std::clamp(static_cast<float>(*_state.abStartMs) / _state.lengthMs, 0.0f, 1.0f) * usable;
        if (_state.abEndMs.has_value()) {
            float bx = trackL + std::clamp(static_cast<float>(*_state.abEndMs) / _state.lengthMs, 0.0f, 1.0f) * usable;
            br->SetColor(rgba(0xFFD166, 0.55f));
            _rt->FillRectangle(D2D1::RectF(ax, trackY - 2.0f, bx, trackY + 2.0f), br);
            br->SetColor(amber);
            _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx - 1.5f, trackY - 6.0f, bx + 1.5f, trackY + 6.0f), 1.0f, 1.0f), br);
        }
        br->SetColor(amber);
        _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ax - 1.5f, trackY - 6.0f, ax + 1.5f, trackY + 6.0f), 1.0f, 1.0f), br);
    }

    // Seek thumb knob
    if (_state.isSeekable) {
        float r = seekHot ? 7.0f : 5.0f;
        br->SetColor(rgba(0xFFFFFF, seekHot ? 1.0f : 0.92f));
        _rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fillX, trackY), r, r), br);
        br->SetColor(teal);
        _rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(fillX, trackY), r, r), br, 2.0f);
    }

    // Hover timestamp + chapter pill on seek bar
    if (_seekHover && _state.lengthMs > 0) {
        int64_t hoverMs = msFromX(_mouseX);
        std::wstring tip = TimeFormat::format(hoverMs);
        auto ch = chapterAt(hoverMs);
        if (!ch.empty()) tip = ch + L"  Â·  " + tip;
        float tipW = std::min(260.0f, std::max(54.0f, static_cast<float>(tip.size()) * 6.8f + 16.0f));
        float tx = std::clamp(_mouseX - tipW * 0.5f, 8.0f, w - tipW - 8.0f);
        D2D1_RECT_F tipRc = D2D1::RectF(tx, 1.0f, tx + tipW, 21.0f);
        br->SetColor(rgba(0x14141A, 0.94f));
        _rt->FillRoundedRectangle(D2D1::RoundedRect(tipRc, 5.0f, 5.0f), br);
        br->SetColor(rgba(0xFFFFFF, 0.22f));
        _rt->DrawRoundedRectangle(D2D1::RoundedRect(tipRc, 5.0f, 5.0f), br, 1.0f);
        br->SetColor(rgba(0xFFFFFF));
        _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        _rt->DrawText(tip.c_str(), static_cast<UINT32>(tip.size()), _ctx.fontSmall, tipRc, br);
        _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Volume slider + % text
    {
        float vy = (_volRect.top + _volRect.bottom) * 0.5f;
        float vl = _volRect.left + 5.0f, vr = _volRect.right - 5.0f;
        float vf = std::clamp(_state.volume / 200.0f, 0.0f, 1.0f);
        float vx = vl + vf * (vr - vl);
        br->SetColor(rgba(0x90909A, 0.35f));
        _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(vl, vy - 1.5f, vr, vy + 1.5f), 1.5f, 1.5f), br);
        br->SetColor(_state.isMuted ? mutedText : teal);
        _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(vl, vy - 1.5f, vx, vy + 1.5f), 1.5f, 1.5f), br);
        br->SetColor(rgba(0xFFFFFF));
        _rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(vx, vy), 5.0f, 5.0f), br);
        br->SetColor(teal);
        _rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(vx, vy), 5.0f, 5.0f), br, 1.8f);

        std::wstring vtxt = std::to_wstring(_state.volume) + L"%";
        br->SetColor(mutedText);
        _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        _rt->DrawText(vtxt.c_str(), static_cast<UINT32>(vtxt.size()), _ctx.fontSmall,
                      D2D1::RectF(_volRect.right + 2.0f, vy - 10.0f, _volRect.right + 36.0f, vy + 10.0f), br);
        _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        // Subtle divider between volume and right tools
        br->SetColor(hairline);
        _rt->DrawLine(D2D1::Point2F(_volRect.right + 42.0f, vy - 9.0f), D2D1::Point2F(_volRect.right + 42.0f, vy + 9.0f), br, 1.0f);
    }

    // Buttons
    for (int i = 0; i < static_cast<int>(_buttons.size()); ++i) {
        const auto& b = _buttons[i];
        bool hovered = (i == _hoverBtn);
        bool pressed = (i == _pressedBtn);
        std::string act(b.action);

        if (act == "PlayPause") {
            br->SetColor(pressed ? rgba(0x0D9488) : (hovered ? tealHover : teal));
            float rx = (b.rc.right - b.rc.left) * 0.5f;
            _rt->FillRoundedRectangle(D2D1::RoundedRect(b.rc, rx, rx), br);
            br->SetColor(rgba(0x0B0E11));
            const wchar_t* glyph = _state.isPlaying ? L"\uE769" : L"\uE768";
            _rt->DrawText(glyph, 1, _ctx.fontIconBig, b.rc, br);
            continue;
        }

        if (act == "NormalSpeed") {
            br->SetColor(hovered ? rgba(0x14B8A6, 0.30f) : rgba(0x14B8A6, 0.16f));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(b.rc, 10.0f, 10.0f), br);
            wchar_t rbuf[16];
            std::swprintf(rbuf, 16, L"%.2gx", _state.rate);
            br->SetColor(teal);
            _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            _rt->DrawText(rbuf, static_cast<UINT32>(wcslen(rbuf)), _ctx.fontSmall, b.rc, br);
            _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            continue;
        }

        if (act == "AbLoop") {
            br->SetColor(hovered ? rgba(0xFFD166, 0.28f) : rgba(0xFFD166, 0.16f));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(b.rc, 10.0f, 10.0f), br);
            br->SetColor(rgba(0xFFD166, 0.35f));
            _rt->DrawRoundedRectangle(D2D1::RoundedRect(b.rc, 10.0f, 10.0f), br, 1.0f);
            std::wstring txt = _state.abEndMs.has_value()
                ? (L"A-B " + TimeFormat::format(*_state.abStartMs) + L"-" + TimeFormat::format(*_state.abEndMs))
                : (L"A " + TimeFormat::format(*_state.abStartMs) + L"...");
            br->SetColor(amber);
            _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            _rt->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), _ctx.fontSmall, b.rc, br);
            _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            continue;
        }

        if (act == "AcceptResume" || act == "DismissResume") {
            br->SetColor(hovered ? rgba(0x14B8A6, 0.32f) : rgba(0x14B8A6, 0.16f));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(b.rc, 10.0f, 10.0f), br);
            br->SetColor(rgba(0x14B8A6, 0.35f));
            _rt->DrawRoundedRectangle(D2D1::RoundedRect(b.rc, 10.0f, 10.0f), br, 1.0f);
            br->SetColor(teal);
            if (act == "AcceptResume") {
                _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                _rt->DrawText(_state.resumeOfferText.c_str(), static_cast<UINT32>(_state.resumeOfferText.size()), _ctx.fontSmall, b.rc, br);
                _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            } else {
                _rt->DrawText(L"\uE711", 1, _ctx.fontIconSmall, b.rc, br);
            }
            continue;
        }

        bool active = (act == "CycleLoop" && _state.loop != LoopMode::Off);
        bool disabled = (act == "Snapshot" && !_state.hasVideo);

        if (active) {
            br->SetColor(rgba(0x14B8A6, hovered ? 0.28f : 0.16f));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(b.rc, 6.0f, 6.0f), br);
        } else if (pressed) {
            br->SetColor(rgba(0x808080, 0.28f));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(b.rc, 6.0f, 6.0f), br);
        } else if (hovered && !disabled) {
            br->SetColor(rgba(0x808080, 0.16f));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(b.rc, 6.0f, 6.0f), br);
        }

        const wchar_t* glyph = L"";
        if (act == "Previous") glyph = L"\uE892";
        else if (act == "Stop") glyph = L"\uE71A";
        else if (act == "Next") glyph = L"\uE893";
        else if (act == "CycleLoop") glyph = (_state.loop == LoopMode::One) ? L"\uE8ED" : L"\uE8EE";
        else if (act == "Mute") glyph = (_state.isMuted || _state.volume == 0) ? L"\uE74F" : (_state.volume < 34 ? L"\uE993" : (_state.volume < 67 ? L"\uE994" : L"\uE995"));
        else if (act == "Snapshot") glyph = L"\uE722";
        else if (act == "Effects") glyph = L"\uE9E9";
        else if (act == "Fullscreen") glyph = _state.isFullscreen ? L"\uE73F" : L"\uE740";

        D2D1_COLOR_F fg = active ? teal : textColor;
        if (disabled) fg.a = 0.35f;
        br->SetColor(fg);
        _rt->DrawText(glyph, 1, _ctx.fontIcon, b.rc, br);
    }

    safeRelease(br);
    if (_rt->EndDraw() == D2DERR_RECREATE_TARGET) releaseTarget();
}

LRESULT CALLBACK D2DControlBar::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    D2DControlBar* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<D2DControlBar*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->_hwnd = hwnd;
    } else {
        self = reinterpret_cast<D2DControlBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handleMsg(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT D2DControlBar::handleMsg(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(_hwnd, &ps);
            paint();
            EndPaint(_hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (_rt) {
                UINT w = std::max<UINT>(1, LOWORD(lp));
                UINT h = std::max<UINT>(1, HIWORD(lp));
                _rt->Resize(D2D1::SizeU(w, h));
            }
            redraw();
            return 0;
        case WM_MOUSEMOVE: {
            if (!_mouseInside) {
                _mouseInside = true;
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, _hwnd, 0 };
                TrackMouseEvent(&tme);
            }
            float scale = dpiScaleFor(_hwnd);
            _mouseX = static_cast<float>(GET_X_LPARAM(lp)) / scale;
            _mouseY = static_cast<float>(GET_Y_LPARAM(lp)) / scale;

            if (_seekDragging && _state.isSeekable) {
                int64_t ms = msFromX(_mouseX);
                _state.positionMs = ms;
                ULONGLONG now = GetTickCount64();
                if (now - _lastLiveSeekTick > 100) {
                    _lastLiveSeekTick = now;
                    _lastSeekMs = ms;
                    if (_cb.onSeek) _cb.onSeek(ms);
                }
                redraw();
                return 0;
            }
            if (_volDragging) {
                int v = volFromX(_mouseX);
                if (v != _state.volume && _cb.onVolumeChanged) _cb.onVolumeChanged(v);
                return 0;
            }

            _seekHover = _state.isSeekable && ptInRect(_seekRect, _mouseX, _mouseY);
            _hoverBtn = -1;
            for (int i = 0; i < static_cast<int>(_buttons.size()); ++i) {
                if (ptInRect(_buttons[i].rc, _mouseX, _mouseY)) { _hoverBtn = i; break; }
            }
            bool hand = _seekHover || _hoverBtn >= 0 || ptInRect(_volRect, _mouseX, _mouseY) || ptInRect(_totalTimeRect, _mouseX, _mouseY);
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            redraw();
            return 0;
        }
        case WM_MOUSELEAVE:
            _mouseInside = false;
            _seekHover = false;
            _hoverBtn = -1;
            redraw();
            return 0;
        case WM_LBUTTONDOWN: {
            float scale = dpiScaleFor(_hwnd);
            float x = static_cast<float>(GET_X_LPARAM(lp)) / scale;
            float y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            if (_state.isSeekable && ptInRect(_seekRect, x, y)) {
                _seekDragging = true;
                SetCapture(_hwnd);
                if (_cb.onSeekingChanged) _cb.onSeekingChanged(true);
                int64_t ms = msFromX(x);
                _state.positionMs = ms;
                _lastSeekMs = ms;
                _lastLiveSeekTick = GetTickCount64();
                if (_cb.onSeek) _cb.onSeek(ms);
                redraw();
                return 0;
            }
            if (ptInRect(_volRect, x, y)) {
                _volDragging = true;
                SetCapture(_hwnd);
                if (_cb.onVolumeChanged) _cb.onVolumeChanged(volFromX(x));
                return 0;
            }
            if (ptInRect(_totalTimeRect, x, y)) {
                if (_cb.onToggleRemaining) _cb.onToggleRemaining();
                return 0;
            }
            for (int i = 0; i < static_cast<int>(_buttons.size()); ++i) {
                if (ptInRect(_buttons[i].rc, x, y)) {
                    _pressedBtn = i;
                    SetCapture(_hwnd);
                    redraw();
                    return 0;
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            float scale = dpiScaleFor(_hwnd);
            float x = static_cast<float>(GET_X_LPARAM(lp)) / scale;
            float y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            if (_seekDragging) {
                _seekDragging = false;
                ReleaseCapture();
                int64_t ms = msFromX(x);
                if (ms != _lastSeekMs && _cb.onSeek) _cb.onSeek(ms);
                _lastSeekMs = -1;
                if (_cb.onSeekingChanged) _cb.onSeekingChanged(false);
                redraw();
                return 0;
            }
            if (_volDragging) {
                _volDragging = false;
                ReleaseCapture();
                return 0;
            }
            if (_pressedBtn >= 0) {
                int idx = _pressedBtn;
                _pressedBtn = -1;
                ReleaseCapture();
                if (idx < static_cast<int>(_buttons.size()) && ptInRect(_buttons[idx].rc, x, y)) {
                    const char* act = _buttons[idx].action;
                    if (_cb.onAction) _cb.onAction(act);
                }
                redraw();
                return 0;
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (_seekDragging) {
                _seekDragging = false;
                if (_cb.onSeekingChanged) _cb.onSeekingChanged(false);
            }
            _volDragging = false;
            _pressedBtn = -1;
            redraw();
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (_cb.onVolumeChanged) _cb.onVolumeChanged(std::clamp(_state.volume + (delta > 0 ? 5 : -5), 0, 200));
            return 0;
        }
    }
    return DefWindowProcW(_hwnd, msg, wp, lp);
}

// ============================================================================
// D2DOverlay (WS_EX_LAYERED premultiplied alpha over libVLC HWND)
// ============================================================================

D2DOverlay::D2DOverlay(D2DContext& ctx) : _ctx(ctx) {}
D2DOverlay::~D2DOverlay() { if (_hwnd) DestroyWindow(_hwnd); }

bool D2DOverlay::create(HWND owner, HINSTANCE hInst) {
    static bool registered = false;
    const wchar_t* cls = L"ReelD2DOverlay";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInst;
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        registered = true;
    }
    _hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        cls, L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr, hInst, nullptr);
    return _hwnd != nullptr;
}

void D2DOverlay::syncToRect(const RECT& screenRect) {
    _rect = screenRect;
    if (_hwnd && IsWindowVisible(_hwnd)) render();
}

void D2DOverlay::showOsd(const std::wstring& text) {
    _osdText = text;
    _osdStartTick = GetTickCount64();
    _osdAlpha = 1.0f;
    render();
}

void D2DOverlay::showError(const std::wstring& text) {
    _errorText = text;
    _errorStartTick = GetTickCount64();
    render();
}

void D2DOverlay::hideError() {
    if (_errorText.empty()) return;
    _errorText.clear();
    render();
}

void D2DOverlay::setDropOverlay(bool visible, const std::wstring& text) {
    if (_dropVisible == visible && _dropText == text) return;
    _dropVisible = visible;
    _dropText = text;
    render();
}

void D2DOverlay::tickAnimation() {
    bool dirty = false;
    ULONGLONG now = GetTickCount64();
    if (!_osdText.empty()) {
        ULONGLONG dt = now - _osdStartTick;
        if (dt >= 1600) {
            _osdText.clear();
            _osdAlpha = 0.0f;
            dirty = true;
        } else if (dt > 1200) {
            _osdAlpha = 1.0f - static_cast<float>(dt - 1200) / 400.0f;
            dirty = true;
        }
    }
    if (!_errorText.empty() && now - _errorStartTick >= 6000) {
        _errorText.clear();
        dirty = true;
    }
    if (dirty) render();
}

void D2DOverlay::render() {
    if (!_hwnd || !_ctx.d2d) return;
    bool anything = (!_osdText.empty() && _osdAlpha > 0.01f) || !_errorText.empty() || _dropVisible;
    if (!anything) {
        if (IsWindowVisible(_hwnd)) ShowWindow(_hwnd, SW_HIDE);
        return;
    }

    int w = std::max<int>(1, _rect.right - _rect.left);
    int h = std::max<int>(1, _rect.bottom - _rect.top);

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(memDc, bmp);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1DCRenderTarget* rt = nullptr;
    if (SUCCEEDED(_ctx.d2d->CreateDCRenderTarget(&props, &rt)) && rt) {
        RECT sub = { 0, 0, w, h };
        rt->BindDC(memDc, &sub);
        float scale = dpiScaleFor(_hwnd);
        rt->SetDpi(scale * 96.0f, scale * 96.0f);
        float dw = static_cast<float>(w) / scale;
        float dh = static_cast<float>(h) / scale;
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));

        ID2D1SolidColorBrush* br = nullptr;
        rt->CreateSolidColorBrush(rgba(0xFFFFFF), &br);

        // Drop overlay
        if (_dropVisible) {
            br->SetColor(rgba(0x0B0E14, 0.70f));
            rt->FillRectangle(D2D1::RectF(0, 0, dw, dh), br);
            D2D1_RECT_F box = D2D1::RectF(24, 24, dw - 24, dh - 24);
            br->SetColor(rgba(0x14B8A6, 0.12f));
            rt->FillRoundedRectangle(D2D1::RoundedRect(box, 14, 14), br);
            br->SetColor(rgba(0x14B8A6, 0.95f));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 14, 14), br, 2.0f);
            br->SetColor(rgba(0xFFFFFF, 0.98f));
            rt->DrawText(_dropText.c_str(), static_cast<UINT32>(_dropText.size()), _ctx.fontTitle, box, br);
        }

        // OSD frosted pill at top-right
        if (!_osdText.empty() && _osdAlpha > 0.01f) {
            float pillW = std::min(560.0f, std::max(90.0f, static_cast<float>(_osdText.size()) * 9.2f + 36.0f));
            D2D1_RECT_F pill = D2D1::RectF(dw - pillW - 20.0f, 18.0f, dw - 20.0f, 54.0f);
            br->SetColor(rgba(0x14141A, 0.88f * _osdAlpha));
            rt->FillRoundedRectangle(D2D1::RoundedRect(pill, 18.0f, 18.0f), br);
            br->SetColor(rgba(0xFFFFFF, 0.18f * _osdAlpha));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(pill, 18.0f, 18.0f), br, 1.0f);
            br->SetColor(rgba(0xF4F4F6, _osdAlpha));
            rt->DrawText(_osdText.c_str(), static_cast<UINT32>(_osdText.size()), _ctx.fontOsd, pill, br);
        }

        // Error toast at top-left
        if (!_errorText.empty()) {
            float errW = std::min(dw - 40.0f, std::max(240.0f, static_cast<float>(_errorText.size()) * 7.5f + 40.0f));
            D2D1_RECT_F pill = D2D1::RectF(16.0f, 16.0f, 16.0f + errW, 54.0f);
            br->SetColor(rgba(0x7F1D1D, 0.92f));
            rt->FillRoundedRectangle(D2D1::RoundedRect(pill, 10.0f, 10.0f), br);
            br->SetColor(rgba(0xFCA5A5, 0.35f));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(pill, 10.0f, 10.0f), br, 1.0f);
            br->SetColor(rgba(0xFFFFFF));
            rt->DrawText(_errorText.c_str(), static_cast<UINT32>(_errorText.size()), _ctx.fontUi,
                         D2D1::RectF(pill.left + 14.0f, pill.top, pill.right - 14.0f, pill.bottom), br);
        }

        safeRelease(br);
        rt->EndDraw();
        rt->Release();
    }

    POINT ptDst = { _rect.left, _rect.top };
    SIZE szWnd = { w, h };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    ShowWindow(_hwnd, SW_SHOWNOACTIVATE);
    UpdateLayeredWindow(_hwnd, screenDc, &ptDst, &szWnd, memDc, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(memDc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

// ============================================================================
// D2DIdlePanel
// ============================================================================

D2DIdlePanel::D2DIdlePanel(D2DContext& ctx) : _ctx(ctx) {}
D2DIdlePanel::~D2DIdlePanel() {
    safeRelease(_logo);
    safeRelease(_rt);
    if (_hwnd) DestroyWindow(_hwnd);
}

bool D2DIdlePanel::create(HWND parent, HINSTANCE hInst) {
    static bool registered = false;
    const wchar_t* cls = L"ReelD2DIdlePanel";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &D2DIdlePanel::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        registered = true;
    }
    _hwnd = CreateWindowExW(0, cls, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 640, 400, parent, nullptr, hInst, this);
    return _hwnd != nullptr;
}

void D2DIdlePanel::redraw() { if (_hwnd && IsWindowVisible(_hwnd)) InvalidateRect(_hwnd, nullptr, FALSE); }

void D2DIdlePanel::ensureTarget() {
    if (_rt || !_hwnd || !_ctx.d2d) return;
    RECT rc{};
    GetClientRect(_hwnd, &rc);
    _ctx.d2d->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom))),
        &_rt);
    loadLogo();
}

void D2DIdlePanel::loadLogo() {
    if (_logo || !_rt || !_ctx.wic) return;
    HRSRC res = FindResourceW(nullptr, L"IDR_LOGO_PNG", RT_RCDATA);
    if (!res) return;
    HGLOBAL hg = LoadResource(nullptr, res);
    if (!hg) return;
    void* ptr = LockResource(hg);
    DWORD sz = SizeofResource(nullptr, res);
    if (!ptr || sz == 0) return;

    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(_ctx.wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(ptr), sz)) &&
        SUCCEEDED(_ctx.wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) &&
        SUCCEEDED(_ctx.wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        _rt->CreateBitmapFromWicBitmap(conv, nullptr, &_logo);
    }
    safeRelease(conv);
    safeRelease(frame);
    safeRelease(dec);
    safeRelease(stream);
}

void D2DIdlePanel::paint() {
    ensureTarget();
    if (!_rt) return;
    float dpi = dpiScaleFor(_hwnd) * 96.0f;
    _rt->SetDpi(dpi, dpi);
    D2D1_SIZE_F sz = _rt->GetSize();
    float w = sz.width, h = sz.height;
    if (w < 10 || h < 10) return;

    _rt->BeginDraw();

    // Radial vignette background
    D2D1_GRADIENT_STOP stops[2] = {
        { 0.0f, rgba(0x161821) },
        { 1.0f, rgba(0x0A0B0F) }
    };
    ID2D1GradientStopCollection* stopCol = nullptr;
    ID2D1RadialGradientBrush* rad = nullptr;
    if (SUCCEEDED(_rt->CreateGradientStopCollection(stops, 2, &stopCol)) &&
        SUCCEEDED(_rt->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(w * 0.5f, h * 0.45f), D2D1::Point2F(0, 0), w * 0.65f, h * 0.65f),
            stopCol, &rad))) {
        _rt->FillRectangle(D2D1::RectF(0, 0, w, h), rad);
    } else {
        _rt->Clear(rgba(0x0A0B0F));
    }
    safeRelease(rad);
    safeRelease(stopCol);

    float cx = w * 0.5f, cy = h * 0.5f;
    if (_logo) {
        _rt->DrawBitmap(_logo, D2D1::RectF(cx - 46.0f, cy - 108.0f, cx + 46.0f, cy - 16.0f), 0.92f);
    }

    ID2D1SolidColorBrush* br = nullptr;
    _rt->CreateSolidColorBrush(rgba(0xFFFFFF, 0.95f), &br);
    const wchar_t* title = L"Reel Player";
    _rt->DrawText(title, static_cast<UINT32>(wcslen(title)), _ctx.fontTitle, D2D1::RectF(cx - 200, cy - 6, cx + 200, cy + 24), br);

    br->SetColor(rgba(0xFFFFFF, 0.55f));
    const wchar_t* sub = L"Drop a video or audio file here";
    _ctx.fontUi->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    _rt->DrawText(sub, static_cast<UINT32>(wcslen(sub)), _ctx.fontUi, D2D1::RectF(cx - 220, cy + 26, cx + 220, cy + 48), br);

    // Open File pill CTA
    _ctaRect = D2D1::RectF(cx - 56.0f, cy + 62.0f, cx + 56.0f, cy + 94.0f);
    br->SetColor(_ctaPressed ? rgba(0x0D9488) : (_ctaHover ? rgba(0x2DD4BF) : rgba(0x14B8A6)));
    _rt->FillRoundedRectangle(D2D1::RoundedRect(_ctaRect, 16.0f, 16.0f), br);
    br->SetColor(rgba(0x0B0E11));
    _ctx.fontUiBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    const wchar_t* cta = L"Open File";
    _rt->DrawText(cta, static_cast<UINT32>(wcslen(cta)), _ctx.fontUiBold, _ctaRect, br);
    _ctx.fontUi->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    _ctx.fontUiBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    safeRelease(br);
    if (_rt->EndDraw() == D2DERR_RECREATE_TARGET) { safeRelease(_logo); safeRelease(_rt); }
}

LRESULT CALLBACK D2DIdlePanel::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    D2DIdlePanel* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<D2DIdlePanel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->_hwnd = hwnd;
    } else {
        self = reinterpret_cast<D2DIdlePanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handleMsg(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT D2DIdlePanel::handleMsg(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(_hwnd, &ps);
            paint();
            EndPaint(_hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (_rt) _rt->Resize(D2D1::SizeU(std::max<UINT>(1, LOWORD(lp)), std::max<UINT>(1, HIWORD(lp))));
            redraw();
            return 0;
        case WM_MOUSEMOVE: {
            float scale = dpiScaleFor(_hwnd);
            float x = static_cast<float>(GET_X_LPARAM(lp)) / scale, y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            bool h = ptInRect(_ctaRect, x, y);
            SetCursor(LoadCursorW(nullptr, h ? IDC_HAND : IDC_ARROW));
            if (h != _ctaHover) { _ctaHover = h; redraw(); }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            float scale = dpiScaleFor(_hwnd);
            float x = static_cast<float>(GET_X_LPARAM(lp)) / scale, y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            if (ptInRect(_ctaRect, x, y)) { _ctaPressed = true; SetCapture(_hwnd); redraw(); }
            return 0;
        }
        case WM_LBUTTONUP: {
            float scale = dpiScaleFor(_hwnd);
            float x = static_cast<float>(GET_X_LPARAM(lp)) / scale, y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            if (_ctaPressed) {
                _ctaPressed = false;
                ReleaseCapture();
                redraw();
                if (ptInRect(_ctaRect, x, y) && onOpenFile) onOpenFile();
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            if (onDoubleClick) onDoubleClick();
            return 0;
    }
    return DefWindowProcW(_hwnd, msg, wp, lp);
}


} // namespace reel
