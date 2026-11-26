#include "ui/D2DControls.h"
#include "core/MediaFiles.h"
#include "core/StrUtil.h"
#include "core/TimeFormat.h"
#include "win/WinInterop.h"
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

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
    safeRelease(_brush);
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
        _rt->CreateSolidColorBrush(rgba(0xFFFFFF), &_brush);
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
    ID2D1SolidColorBrush* br = _brush;
    if (!br) { _rt->EndDraw(); return; }

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

    if (_rt->EndDraw() == D2DERR_RECREATE_TARGET) { safeRelease(_brush); safeRelease(_rt); }
}

static thread_local D2DMenuBar* g_activeMenuBar = nullptr;
static thread_local HHOOK g_menuHook = nullptr;

int D2DMenuBar::hitTestMenu(POINT screenPt) const {
    if (!_hwnd) return -1;
    RECT barRc{};
    GetWindowRect(_hwnd, &barRc);
    if (!PtInRect(&barRc, screenPt)) return -1;
    POINT c = screenPt;
    ScreenToClient(_hwnd, &c);
    float scale = dpiScaleFor(_hwnd);
    float x = static_cast<float>(c.x) / scale;
    for (int i = 0; i < static_cast<int>(_rects.size()); ++i) {
        if (x >= _rects[i].left - 1.5f && x <= _rects[i].right + 1.5f) return i;
    }
    return -1;
}

LRESULT CALLBACK D2DMenuBar::menuHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == MSGF_MENU && g_activeMenuBar && lp) {
        auto* m = reinterpret_cast<MSG*>(lp);
        auto* self = g_activeMenuBar;
        if (m->message == WM_MOUSEMOVE) {
            POINT pt{};
            GetCursorPos(&pt);
            if (pt.x != self->_lastHookPt.x || pt.y != self->_lastHookPt.y) {
                self->_lastHookPt = pt;
                RECT barRc{};
                GetWindowRect(self->_hwnd, &barRc);
                if (PtInRect(&barRc, pt)) {
                    int idx = self->hitTestMenu(pt);
                    if (idx >= 0 && idx != self->_openIdx) {
                        self->_nextOpenIdx = idx;
                        EndMenu();
                    } else if (idx < 0 && !self->_rects.empty()) {
                        // Only auto-close if cursor moved well past the last menu label into empty bar space
                        POINT c = pt;
                        ScreenToClient(self->_hwnd, &c);
                        float x = static_cast<float>(c.x) / dpiScaleFor(self->_hwnd);
                        if (x > self->_rects.back().right + 12.0f || x < self->_rects.front().left - 12.0f) {
                            EndMenu();
                        }
                    }
                } else {
                    HWND under = WindowFromPoint(pt);
                    wchar_t cls[32] = {};
                    if (under) GetClassNameW(under, cls, 32);
                    bool onPopup = (_wcsicmp(cls, L"#32768") == 0);
                    RECT bridge = barRc;
                    bridge.bottom += 6;
                    if (!onPopup && !PtInRect(&bridge, pt)) {
                        EndMenu();
                    }
                }
            }
        }
    }
    return CallNextHookEx(g_menuHook, code, wp, lp);
}

void D2DMenuBar::openMenuAt(int index) {
    if (index < 0 || index >= static_cast<int>(_menus.size())) return;
    if (g_activeMenuBar) return;

    g_activeMenuBar = this;
    g_menuHook = SetWindowsHookExW(WH_MSGFILTER, &D2DMenuBar::menuHookProc, nullptr, GetCurrentThreadId());

    while (index >= 0 && index < static_cast<int>(_menus.size())) {
        _openIdx = index;
        _hoverIdx = index;
        _nextOpenIdx = -1;
        GetCursorPos(&_lastHookPt);
        redraw();

        float scale = dpiScaleFor(_hwnd);
        POINT pt = { static_cast<LONG>(_rects[index].left * scale), static_cast<LONG>(BarHeight * scale) };
        ClientToScreen(_hwnd, &pt);
        TrackPopupMenu(_menus[index].second, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, _owner, nullptr);
        index = _nextOpenIdx;
    }

    if (g_menuHook) { UnhookWindowsHookEx(g_menuHook); g_menuHook = nullptr; }
    g_activeMenuBar = nullptr;
    _openIdx = -1;
    _nextOpenIdx = -1;
    POINT pt{};
    GetCursorPos(&pt);
    _hoverIdx = hitTestMenu(pt);
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

static constexpr UINT WM_APP_THUMB_READY = WM_APP + 40;
struct ThumbResultMsg {
    int64_t gen = 0;
    int64_t timeMs = 0;
    std::vector<uint8_t> imgBytes;
    bool fromOnDemand = false;
    bool ffmpegMissing = false;
};

D2DControlBar::D2DControlBar(D2DContext& ctx, bool floatingPill)
    : _ctx(ctx), _floating(floatingPill) {}

D2DControlBar::~D2DControlBar() {
    stopPrewarm();
    releaseTarget();
    if (_popupHwnd) DestroyWindow(_popupHwnd);
    if (_hwnd) DestroyWindow(_hwnd);
}

bool D2DControlBar::create(HWND parent, HINSTANCE hInst) {
    static bool registered = false;
    const wchar_t* cls = L"ReelD2DControlBar";
    const wchar_t* popCls = L"ReelD2DPopupTip";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &D2DControlBar::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);

        WNDCLASSEXW pwc{};
        pwc.cbSize = sizeof(pwc);
        pwc.lpfnWndProc = DefWindowProcW;
        pwc.hInstance = hInst;
        pwc.lpszClassName = popCls;
        RegisterClassExW(&pwc);
        registered = true;
    }

    DWORD style = _floating ? (WS_POPUP) : (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
    DWORD exStyle = _floating ? (WS_EX_LAYERED | WS_EX_NOACTIVATE) : 0;
    _hwnd = CreateWindowExW(exStyle, cls, L"", style, 0, 0, 800, BarHeight, parent, nullptr, hInst, this);
    if (_hwnd && _floating) {
        SetLayeredWindowAttributes(_hwnd, 0, 240, LWA_ALPHA);
        WinInterop::setWindowRoundedAndBackdrop(_hwnd, true);
    }
    if (_hwnd) {
        _popupHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            popCls, L"", WS_POPUP, 0, 0, 1, 1, parent, nullptr, hInst, nullptr);
    }
    return _hwnd != nullptr;
}

void D2DControlBar::setState(const ControlBarState& s) {
    bool mediaChanged = (s.mediaPath != _thumbMediaPath);
    if (mediaChanged) {
        stopPrewarm();
        _thumbMediaPath = s.mediaPath;
        _thumbMediaLenMs = 0;
        _thumbCache.clear();
        _lastThumbBucket = -1;
        _pendingDemandMs = -1;
        _onDemandBusy = false;
        ++_thumbReqGen;
    }
    _state = s;
    // Start single-pass background keyframe storyboard pre-warming once we know the duration
    // (only on the docked bar so fullscreen/docked don't double-extract; or whichever is active).
    if (!_floating && !_ffmpegMissing && _state.hasVideo && !_thumbMediaPath.empty() &&
        _state.lengthMs > 2000 && _thumbMediaLenMs == 0) {
        _thumbMediaLenMs = _state.lengthMs;
        startPrewarmThumbnails(_thumbMediaPath, _thumbMediaLenMs);
    }
    redraw();
}

void D2DControlBar::setOpacity(BYTE alpha) {
    if (_hwnd && _floating) SetLayeredWindowAttributes(_hwnd, 0, alpha, LWA_ALPHA);
    if (alpha == 0) hidePopup();
}

void D2DControlBar::redraw() {
    if (_hwnd && IsWindowVisible(_hwnd)) InvalidateRect(_hwnd, nullptr, FALSE);
}

void D2DControlBar::hidePopup() {
    if (_popupHwnd && IsWindowVisible(_popupHwnd)) ShowWindow(_popupHwnd, SW_HIDE);
}

bool D2DControlBar::isAnimating() const {
    float target = (_seekHover || _seekDragging) ? 1.0f : 0.0f;
    return std::abs(_seekAnim - target) > 0.01f;
}

bool D2DControlBar::tickAnimation() {
    float target = (_seekHover || _seekDragging) ? 1.0f : 0.0f;
    float diff = target - _seekAnim;
    if (std::abs(diff) <= 0.01f) {
        if (_seekAnim != target) { _seekAnim = target; redraw(); }
        return false;
    }
    _seekAnim += diff * 0.35f;
    if (std::abs(target - _seekAnim) <= 0.02f) _seekAnim = target;
    redraw();
    return std::abs(_seekAnim - target) > 0.01f;
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
        _rt->CreateSolidColorBrush(rgba(0xFFFFFF), &_brush);
    }
}

void D2DControlBar::releaseTarget() {
    safeRelease(_brush);
    safeRelease(_rt);
}

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
    _compactLayout = _state.isMiniPlayer || (w < 560.0f);

    if (_compactLayout) {
        const float padX = 10.0f;
        const float timeW = 46.0f;
        _seekRect = D2D1::RectF(padX + timeW + 4.0f, 4.0f, w - padX - timeW - 4.0f, 24.0f);
        _totalTimeRect = D2D1::RectF(w - padX - timeW, 4.0f, w - padX, 24.0f);
        _volRect = D2D1::RectF(-1, -1, -1, -1);

        const float rowCy = std::max(40.0f, h - 21.0f);
        const float btnW = 30.0f;
        const float btnH = 28.0f;
        const float cx = w * 0.5f;

        // Center: Prev | Play/Pause (34x34) | Next
        _buttons.push_back({ D2D1::RectF(cx - 17.0f, rowCy - 17.0f, cx + 17.0f, rowCy + 17.0f), "PlayPause", L"Play / Pause" });
        _buttons.push_back({ D2D1::RectF(cx - 17.0f - 36.0f, rowCy - btnH * 0.5f, cx - 17.0f - 6.0f, rowCy + btnH * 0.5f), "Previous", L"Previous" });
        _buttons.push_back({ D2D1::RectF(cx + 17.0f + 6.0f, rowCy - btnH * 0.5f, cx + 17.0f + 36.0f, rowCy + btnH * 0.5f), "Next", L"Next" });

        // Left: Mute
        _buttons.push_back({ D2D1::RectF(padX, rowCy - btnH * 0.5f, padX + btnW, rowCy + btnH * 0.5f), "Mute", L"Mute" });

        // Right: Fullscreen | Exit PiP
        float rx = w - padX;
        rx -= btnW;
        _buttons.push_back({ D2D1::RectF(rx, rowCy - btnH * 0.5f, rx + btnW, rowCy + btnH * 0.5f), "Fullscreen", L"Fullscreen" });
        rx -= btnW + 4.0f;
        _buttons.push_back({ D2D1::RectF(rx, rowCy - btnH * 0.5f, rx + btnW, rowCy + btnH * 0.5f), "MiniPlayer", L"Exit Picture-in-Picture" });
        return;
    }

    const float padX = 14.0f;
    const float timeW = 54.0f;
    // Row 1: Seek bar (y: 6..28)
    _seekRect = D2D1::RectF(padX + timeW + 6.0f, 6.0f, w - padX - timeW - 6.0f, 28.0f);
    _totalTimeRect = D2D1::RectF(w - padX - timeW, 6.0f, w - padX, 28.0f);

    // Row 2: Controls (y: 32..70)
    const float rowCy = std::max(46.0f, h - 24.0f);
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
        lx += pw + 4.0f;
        if (_state.abEndMs.has_value()) {
            _buttons.push_back({ D2D1::RectF(lx, rowCy - 12.0f, lx + 56.0f, rowCy + 12.0f), "ExportAbClip", L"Export A-B clip" });
            lx += 60.0f;
        }
    }
    if (_state.hasResumeOffer && lx + 148.0f < cx - 64.0f) {
        float pw = 122.0f;
        _buttons.push_back({ D2D1::RectF(lx, rowCy - 13.0f, lx + pw, rowCy + 13.0f), "AcceptResume", L"Continue" });
        _buttons.push_back({ D2D1::RectF(lx + pw + 2.0f, rowCy - 13.0f, lx + pw + 24.0f, rowCy + 13.0f), "DismissResume", L"Dismiss" });
    }

    // Right cluster (right-to-left): Fullscreen | PiP | Effects | Snapshot | | Vol% | VolSlider | Mute
    float rx = w - padX;
    auto pushRight = [&](const char* act, const wchar_t* tip) {
        rx -= btnW;
        _buttons.push_back({ D2D1::RectF(rx, rowCy - btnH * 0.5f, rx + btnW, rowCy + btnH * 0.5f), act, tip });
        rx -= 2.0f;
    };
    pushRight("Fullscreen", L"Fullscreen");
    pushRight("MiniPlayer", L"Picture-in-Picture");
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

    ID2D1SolidColorBrush* br = _brush;
    if (!br) { _rt->EndDraw(); return; }

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
    D2D1_RECT_F elapsedRc = _compactLayout
        ? D2D1::RectF(10.0f, 4.0f, 56.0f, 24.0f)
        : D2D1::RectF(14.0f, 6.0f, 68.0f, 28.0f);
    _rt->DrawText(elapsed.c_str(), static_cast<UINT32>(elapsed.size()), _ctx.fontSmall, elapsedRc, br);
    _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    _rt->DrawText(total.c_str(), static_cast<UINT32>(total.size()), _ctx.fontSmall, _totalTimeRect, br);
    _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    // Seek bar (smoothly expands from 3.5px to 6.5px on hover/drag with soft thumb glow)
    const bool seekHot = _seekHover || _seekDragging;
    const float anim = std::clamp(_seekAnim, 0.0f, 1.0f);
    const float trackH = 3.5f + 3.0f * anim;
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

    // Chapter ticks (subtle vertical ticks along the seek track)
    if (_state.chapters.size() > 1) {
        float tickH = 3.5f + 2.0f * anim;
        br->SetColor(rgba(0xFFD166, 0.85f));
        for (size_t i = 1; i < _state.chapters.size(); ++i) {
            float cxMark = trackL + static_cast<float>(_state.chapters[i].fraction) * usable;
            _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cxMark - 1.0f, trackY - tickH, cxMark + 1.0f, trackY + tickH), 1.0f, 1.0f), br);
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

    // Seek thumb knob with soft glow halo on hover/drag
    if (_state.isSeekable) {
        float r = 5.0f + 2.2f * anim;
        if (anim > 0.05f) {
            br->SetColor(rgba(0x14B8A6, 0.28f * anim));
            _rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fillX, trackY), r + 4.5f * anim, r + 4.5f * anim), br);
        }
        br->SetColor(rgba(0xFFFFFF, 0.92f + 0.08f * anim));
        _rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fillX, trackY), r, r), br);
        br->SetColor(seekHot ? tealHover : teal);
        _rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(fillX, trackY), r, r), br, 2.0f);
    }

    // Volume slider + % text (hidden in compact/PiP layout to avoid overlap)
    if (!_compactLayout && _volRect.right > _volRect.left) {
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

        if (act == "ExportAbClip") {
            br->SetColor(hovered ? rgba(0x14B8A6, 0.34f) : rgba(0x14B8A6, 0.18f));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(b.rc, 10.0f, 10.0f), br);
            br->SetColor(rgba(0x14B8A6, 0.45f));
            _rt->DrawRoundedRectangle(D2D1::RoundedRect(b.rc, 10.0f, 10.0f), br, 1.0f);
            br->SetColor(teal);
            _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            _rt->DrawText(L"Export", 6, _ctx.fontSmall, b.rc, br);
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

        bool active = (act == "CycleLoop" && _state.loop != LoopMode::Off) ||
                      (act == "MiniPlayer" && _state.isMiniPlayer);
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
        else if (act == "MiniPlayer") glyph = L"\uE8A7";
        else if (act == "Fullscreen") glyph = _state.isFullscreen ? L"\uE73F" : L"\uE740";

        D2D1_COLOR_F fg = active ? teal : textColor;
        if (disabled) fg.a = 0.35f;
        br->SetColor(fg);
        _rt->DrawText(glyph, 1, _ctx.fontIcon, b.rc, br);
    }

    if (_rt->EndDraw() == D2DERR_RECREATE_TARGET) releaseTarget();
}

void D2DControlBar::stopPrewarm() {
    if (_prewarmCancel) {
        _prewarmCancel->store(true);
        _prewarmCancel.reset();
    }
}

const std::vector<uint8_t>* D2DControlBar::findCachedThumb(int64_t ms, int64_t maxDeltaMs) const {
    const std::vector<uint8_t>* best = nullptr;
    int64_t bestDist = maxDeltaMs + 1;
    for (const auto& kv : _thumbCache) {
        int64_t d = std::abs(kv.first - ms);
        if (d < bestDist) {
            bestDist = d;
            best = &kv.second;
        }
    }
    return best;
}

void D2DControlBar::startPrewarmThumbnails(const std::wstring& path, int64_t lengthMs) {
    stopPrewarm();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _prewarmCancel = cancel;
    int64_t gen = _thumbReqGen;
    HWND target = _hwnd;

    // Extract up to ~48 evenly spaced keyframes in a single ultra-fast ffmpeg pass (-skip_frame nokey).
    double durSec = static_cast<double>(lengthMs) / 1000.0;
    double stepSec = std::max(2.0, durSec / 48.0);
    int64_t stepMs = static_cast<int64_t>(std::lround(stepSec * 1000.0));

    std::thread([target, path, gen, stepSec, stepMs, cancel]() {
        wchar_t fpsBuf[48];
        std::swprintf(fpsBuf, 48, L"1/%.3f", stepSec);
        std::wstring cmd =
            L"ffmpeg -v error -an -sn -dn -skip_frame nokey -i \"" + path +
            L"\" -vf \"fps=" + std::wstring(fpsBuf) +
            L",scale=iw*sar:ih,scale='min(192,iw)':'min(144,ih)':force_original_aspect_ratio=decrease\""
            L" -f image2pipe -vcodec mjpeg -q:v 6 -";

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = wr;
        si.hStdError = wr;
        PROCESS_INFORMATION pi{};
        std::wstring cmdCopy = cmd;
        if (!CreateProcessW(nullptr, cmdCopy.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
            CloseHandle(wr);
            CloseHandle(rd);
            auto* msg = new ThumbResultMsg{ gen, 0, {}, false, true };
            if (!PostMessageW(target, WM_APP_THUMB_READY, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
            return;
        }
        CloseHandle(wr);

        std::vector<uint8_t> stream;
        stream.reserve(64 * 1024);
        uint8_t buf[8192];
        DWORD got = 0;
        int frameIdx = 0;
        while (!cancel->load() && ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
            stream.insert(stream.end(), buf, buf + got);
            // Parse complete JPEG frames (SOI 0xFF 0xD8 .. EOI 0xFF 0xD9)
            size_t pos = 0;
            while (pos + 3 < stream.size()) {
                size_t soi = std::string::npos;
                for (size_t i = pos; i + 1 < stream.size(); ++i) {
                    if (stream[i] == 0xFF && stream[i + 1] == 0xD8) { soi = i; break; }
                }
                if (soi == std::string::npos) { stream.clear(); break; }
                size_t eoi = std::string::npos;
                for (size_t i = soi + 2; i + 1 < stream.size(); ++i) {
                    if (stream[i] == 0xFF && stream[i + 1] == 0xD9) { eoi = i + 2; break; }
                }
                if (eoi == std::string::npos) {
                    if (soi > 0) stream.erase(stream.begin(), stream.begin() + static_cast<ptrdiff_t>(soi));
                    break;
                }
                if (cancel->load()) break;
                auto* msg = new ThumbResultMsg{ gen, static_cast<int64_t>(frameIdx) * stepMs,
                                                std::vector<uint8_t>(stream.begin() + static_cast<ptrdiff_t>(soi),
                                                                     stream.begin() + static_cast<ptrdiff_t>(eoi)),
                                                false, false };
                ++frameIdx;
                if (!PostMessageW(target, WM_APP_THUMB_READY, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
                pos = eoi;
            }
            if (pos > 0 && pos <= stream.size()) {
                stream.erase(stream.begin(), stream.begin() + static_cast<ptrdiff_t>(pos));
            }
            if (frameIdx >= 64) break;
        }
        if (cancel->load() || frameIdx >= 64) TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(rd);
    }).detach();
}

void D2DControlBar::requestThumbnail(const std::wstring& path, int64_t ms) {
    if (_ffmpegMissing || path.empty() || !_state.hasVideo) return;
    int64_t bucket = (ms / 2000) * 2000;
    if (findCachedThumb(bucket, 1000)) return;
    if (_onDemandBusy) {
        _pendingDemandMs = bucket;
        return;
    }
    if (bucket == _lastThumbBucket) return;
    _lastThumbBucket = bucket;
    _onDemandBusy = true;
    int64_t gen = _thumbReqGen;
    HWND target = _hwnd;

    std::thread([target, path, bucket, gen]() {
        wchar_t secBuf[32];
        std::swprintf(secBuf, 32, L"%.2f", bucket / 1000.0);
        // -noaccurate_seek + -skip_frame nokey + -an -sn -dn decodes only 1 keyframe (~5-10x faster!)
        std::wstring cmd = L"ffmpeg -v error -an -sn -dn -noaccurate_seek -skip_frame nokey -ss " + std::wstring(secBuf) +
                           L" -i \"" + path + L"\" -frames:v 1 -vf \"scale=iw*sar:ih,scale='min(192,iw)':'min(144,ih)':force_original_aspect_ratio=decrease\" -f image2pipe -vcodec mjpeg -q:v 6 -";

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = wr;
        si.hStdError = wr;
        PROCESS_INFORMATION pi{};
        std::wstring cmdCopy = cmd;
        auto* msg = new ThumbResultMsg{ gen, bucket, {}, true, false };
        if (!CreateProcessW(nullptr, cmdCopy.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(wr);
            CloseHandle(rd);
            msg->ffmpegMissing = true;
            if (!PostMessageW(target, WM_APP_THUMB_READY, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
            return;
        }
        CloseHandle(wr);
        uint8_t buf[8192];
        DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
            if (msg->imgBytes.size() + got > 256 * 1024) break;
            msg->imgBytes.insert(msg->imgBytes.end(), buf, buf + got);
        }
        WaitForSingleObject(pi.hProcess, 1500);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(rd);
        if (!PostMessageW(target, WM_APP_THUMB_READY, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
    }).detach();
}

void D2DControlBar::renderPopup(int anchorScreenX, int anchorScreenY, const std::wstring& line1,
                                const std::wstring& line2, const std::vector<uint8_t>* imgBytes) {
    if (!_popupHwnd || !_ctx.d2d || line1.empty()) { hidePopup(); return; }
    float scale = dpiScaleFor(_hwnd);

    // Decode thumbnail via WIC first so we know its exact pixel dimensions (works for JPEG & BMP)
    IWICFormatConverter* conv = nullptr;
    float thumbW = 0.0f;
    float thumbH = 0.0f;
    if (imgBytes && imgBytes->size() > 16 && _ctx.wic) {
        IWICStream* st = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* fr = nullptr;
        UINT bw = 0, bh = 0;
        if (SUCCEEDED(_ctx.wic->CreateStream(&st)) &&
            SUCCEEDED(st->InitializeFromMemory(const_cast<BYTE*>(imgBytes->data()), static_cast<DWORD>(imgBytes->size()))) &&
            SUCCEEDED(_ctx.wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &fr)) &&
            SUCCEEDED(fr->GetSize(&bw, &bh)) && bw > 0 && bh > 0 &&
            SUCCEEDED(_ctx.wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            constexpr float maxW = 192.0f;
            constexpr float maxH = 135.0f;
            float fit = std::min(maxW / static_cast<float>(bw), maxH / static_cast<float>(bh));
            thumbW = std::max(36.0f, std::round(static_cast<float>(bw) * fit));
            thumbH = std::max(36.0f, std::round(static_cast<float>(bh) * fit));
        } else {
            safeRelease(conv);
        }
        safeRelease(fr);
        safeRelease(dec);
        safeRelease(st);
    }
    bool hasThumb = (conv != nullptr && thumbW > 0.0f && thumbH > 0.0f);
    float textW = std::max(static_cast<float>(line1.size()) * 6.8f, static_cast<float>(line2.size()) * 6.8f) + 20.0f;
    float dw = std::clamp(std::max(thumbW + 12.0f, textW), 64.0f, 320.0f);
    float textH = line2.empty() ? 24.0f : 40.0f;
    float dh = (hasThumb ? (thumbH + 10.0f) : 0.0f) + textH + 4.0f;

    int pw = std::max(1, static_cast<int>(std::lround(dw * scale)));
    int ph = std::max(1, static_cast<int>(std::lround(dh * scale)));

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = pw;
    bi.bmiHeader.biHeight = -ph;
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
        RECT sub = { 0, 0, pw, ph };
        rt->BindDC(memDc, &sub);
        rt->SetDpi(scale * 96.0f, scale * 96.0f);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));

        ID2D1SolidColorBrush* br = nullptr;
        rt->CreateSolidColorBrush(rgba(0x14141A, 0.96f), &br);
        D2D1_RECT_F card = D2D1::RectF(0.5f, 0.5f, dw - 0.5f, dh - 0.5f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(card, 7.0f, 7.0f), br);
        br->SetColor(rgba(0xFFFFFF, 0.20f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(card, 7.0f, 7.0f), br, 1.0f);

        float textTop = 2.0f;
        if (hasThumb) {
            ID2D1Bitmap* d2dBmp = nullptr;
            if (SUCCEEDED(rt->CreateBitmapFromWicBitmap(conv, nullptr, &d2dBmp)) && d2dBmp) {
                float tx = (dw - thumbW) * 0.5f;
                rt->DrawBitmap(d2dBmp, D2D1::RectF(tx, 6.0f, tx + thumbW, 6.0f + thumbH));
                d2dBmp->Release();
            }
            textTop = thumbH + 9.0f;
        }

        _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        br->SetColor(rgba(0xF4F4F6));
        if (line2.empty()) {
            rt->DrawText(line1.c_str(), static_cast<UINT32>(line1.size()), _ctx.fontSmall,
                         D2D1::RectF(6.0f, textTop, dw - 6.0f, dh - 2.0f), br);
        } else {
            rt->DrawText(line1.c_str(), static_cast<UINT32>(line1.size()), _ctx.fontSmall,
                         D2D1::RectF(6.0f, textTop, dw - 6.0f, textTop + 18.0f), br);
            br->SetColor(rgba(0xA1A1AA));
            rt->DrawText(line2.c_str(), static_cast<UINT32>(line2.size()), _ctx.fontSmall,
                         D2D1::RectF(6.0f, textTop + 17.0f, dw - 6.0f, dh - 2.0f), br);
        }
        _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        safeRelease(br);
        rt->EndDraw();
        rt->Release();
    }
    safeRelease(conv);

    int px = anchorScreenX - pw / 2;
    int py = anchorScreenY - ph - static_cast<int>(std::lround(8.0f * scale));
    HMONITOR mon = MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
        px = std::clamp<int>(px, mi.rcWork.left + 4, mi.rcWork.right - pw - 4);
        py = std::max<int>(py, mi.rcWork.top + 4);
    }
    POINT ptDst = { px, py };
    SIZE szWnd = { pw, ph };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    ShowWindow(_popupHwnd, SW_SHOWNOACTIVATE);
    UpdateLayeredWindow(_popupHwnd, screenDc, &ptDst, &szWnd, memDc, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(memDc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

void D2DControlBar::updateHoverPopup() {
    if (!_hwnd || !IsWindowVisible(_hwnd) || !_mouseInside) { hidePopup(); return; }
    float scale = dpiScaleFor(_hwnd);
    if ((_seekHover || _seekDragging) && _state.lengthMs > 0) {
        int64_t hoverMs = msFromX(_mouseX);
        std::wstring ts = TimeFormat::format(hoverMs);
        std::wstring ch = chapterAt(hoverMs);
        const std::vector<uint8_t>* thumb = nullptr;
        if (_state.hasVideo && !_thumbMediaPath.empty()) {
            // Instant hit from pre-warmed storyboard or nearest cached keyframe (within 15s)
            int64_t prewarmStep = std::max<int64_t>(2000, _state.lengthMs / 48);
            thumb = findCachedThumb(hoverMs, std::max<int64_t>(prewarmStep, 15000));
            if (!findCachedThumb(hoverMs, std::max<int64_t>(2500, prewarmStep))) {
                requestThumbnail(_thumbMediaPath, hoverMs);
            }
        }
        POINT pt = { static_cast<LONG>(std::lround(_mouseX * scale)),
                     static_cast<LONG>(std::lround(_seekRect.top * scale)) };
        ClientToScreen(_hwnd, &pt);
        renderPopup(pt.x, pt.y, ts, ch, thumb);
        return;
    }
    if (_hoverBtn >= 0 && _hoverBtn < static_cast<int>(_buttons.size())) {
        const auto& b = _buttons[_hoverBtn];
        std::wstring tip = b.tooltip;
        if (_cb.getShortcutHint) {
            std::wstring key = _cb.getShortcutHint(b.action);
            if (!key.empty()) tip += L"  (" + key + L")";
        }
        POINT pt = { static_cast<LONG>(std::lround((b.rc.left + b.rc.right) * 0.5f * scale)),
                     static_cast<LONG>(std::lround(b.rc.top * scale)) };
        ClientToScreen(_hwnd, &pt);
        renderPopup(pt.x, pt.y, tip, L"", nullptr);
        return;
    }
    hidePopup();
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
                if (now - _lastLiveSeekTick > 75) {
                    _lastLiveSeekTick = now;
                    _lastSeekMs = ms;
                    if (_cb.onSeek) _cb.onSeek(ms, true);
                }
                updateHoverPopup();
                redraw();
                return 0;
            }
            if (_volDragging) {
                int v = volFromX(_mouseX);
                if (v != _state.volume && _cb.onVolumeChanged) _cb.onVolumeChanged(v);
                return 0;
            }

            bool prevSeekHover = _seekHover;
            _seekHover = _state.isSeekable && ptInRect(_seekRect, _mouseX, _mouseY);
            _hoverBtn = -1;
            for (int i = 0; i < static_cast<int>(_buttons.size()); ++i) {
                if (ptInRect(_buttons[i].rc, _mouseX, _mouseY)) { _hoverBtn = i; break; }
            }
            bool hand = _seekHover || _hoverBtn >= 0 || ptInRect(_volRect, _mouseX, _mouseY) || ptInRect(_totalTimeRect, _mouseX, _mouseY);
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            if (_seekHover != prevSeekHover && _cb.onNeedAnimTick) _cb.onNeedAnimTick();
            updateHoverPopup();
            redraw();
            return 0;
        }
        case WM_MOUSELEAVE:
            _mouseInside = false;
            _seekHover = false;
            _hoverBtn = -1;
            hidePopup();
            if (_cb.onNeedAnimTick) _cb.onNeedAnimTick();
            redraw();
            return 0;
        case WM_APP_THUMB_READY: {
            auto* res = reinterpret_cast<ThumbResultMsg*>(lp);
            if (res) {
                if (res->fromOnDemand) _onDemandBusy = false;
                if (res->ffmpegMissing) _ffmpegMissing = true;
                else if (res->gen == _thumbReqGen && !res->imgBytes.empty()) {
                    if (_thumbCache.size() >= 96) _thumbCache.erase(_thumbCache.begin());
                    _thumbCache.emplace_back(res->timeMs, std::move(res->imgBytes));
                    if (_seekHover || _seekDragging) updateHoverPopup();
                }
                if (res->fromOnDemand && _pendingDemandMs >= 0 && !_ffmpegMissing && !_thumbMediaPath.empty()) {
                    int64_t nextMs = _pendingDemandMs;
                    _pendingDemandMs = -1;
                    requestThumbnail(_thumbMediaPath, nextMs);
                }
                delete res;
            }
            return 0;
        }
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
                if (_cb.onSeek) _cb.onSeek(ms, true);
                if (_cb.onNeedAnimTick) _cb.onNeedAnimTick();
                updateHoverPopup();
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
                if ((_lastSeekMs < 0 || std::abs(ms - _lastSeekMs) > 50) && _cb.onSeek) _cb.onSeek(ms, false);
                _lastSeekMs = -1;
                if (_cb.onSeekingChanged) _cb.onSeekingChanged(false);
                if (_cb.onNeedAnimTick) _cb.onNeedAnimTick();
                updateHoverPopup();
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
