#pragma once
#include "core/MediaItem.h"
#include "core/PlayerService.h"
#include "core/Playlist.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace reel {

/// Shared Direct2D / DirectWrite factories (initialized once, ~5 ms).
struct D2DContext {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dwrite = nullptr;
    IWICImagingFactory* wic = nullptr;
    IDWriteTextFormat* fontUi = nullptr;
    IDWriteTextFormat* fontUiBold = nullptr;
    IDWriteTextFormat* fontSmall = nullptr;
    IDWriteTextFormat* fontTitle = nullptr;
    IDWriteTextFormat* fontOsd = nullptr;
    IDWriteTextFormat* fontIcon = nullptr;
    IDWriteTextFormat* fontIconBig = nullptr;
    IDWriteTextFormat* fontIconSmall = nullptr;

    bool init();
    void shutdown();
};

/// Read-only snapshot of playback UI state rendered by the ControlBar.
struct ControlBarState {
    bool darkTheme = true;
    bool isFullscreenBar = false;
    bool isPlaying = false;
    bool isSeekable = false;
    bool hasVideo = false;
    int64_t positionMs = 0;
    int64_t lengthMs = 0;
    bool showRemaining = false;
    int volume = 100;
    bool isMuted = false;
    float rate = 1.0f;
    LoopMode loop = LoopMode::Off;
    bool isFullscreen = false;
    std::vector<ChapterMark> chapters;
    std::optional<int64_t> abStartMs;
    std::optional<int64_t> abEndMs;
    bool hasResumeOffer = false;
    std::wstring resumeOfferText;
};

struct ControlBarCallbacks {
    std::function<void(const char*)> onAction;
    std::function<void(int64_t)> onSeek;
    std::function<void(bool)> onSeekingChanged;
    std::function<void(int)> onVolumeChanged;
    std::function<void()> onToggleRemaining;
};

/// Sleek Direct2D top menu bar that matches the dark/light theme and opens native dark-mode HMENU popups.
class D2DMenuBar {
public:
    static constexpr int BarHeight = 30;

    explicit D2DMenuBar(D2DContext& ctx);
    ~D2DMenuBar();

    bool create(HWND parent, HINSTANCE hInst);
    HWND hwnd() const { return _hwnd; }
    void setDarkTheme(bool dark);
    void setMenus(std::vector<std::pair<std::wstring, HMENU>> menus);
    void redraw();

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMsg(UINT msg, WPARAM wp, LPARAM lp);
    void ensureTarget();
    void computeLayout();
    void paint();
    void openMenuAt(int index);

    D2DContext& _ctx;
    HWND _hwnd = nullptr;
    HWND _owner = nullptr;
    ID2D1HwndRenderTarget* _rt = nullptr;
    bool _dark = true;
    int _hoverIdx = -1;
    int _openIdx = -1;
    bool _mouseInside = false;
    std::vector<std::pair<std::wstring, HMENU>> _menus;
    std::vector<D2D1_RECT_F> _rects;
};

/// Pure Direct2D ControlBar (used both docked at bottom and floating in fullscreen).
class D2DControlBar {
public:
    static constexpr int BarHeight = 76;

    D2DControlBar(D2DContext& ctx, bool floatingPill);
    ~D2DControlBar();

    bool create(HWND parent, HINSTANCE hInst);
    HWND hwnd() const { return _hwnd; }
    void setState(const ControlBarState& s);
    void setCallbacks(ControlBarCallbacks cb) { _cb = std::move(cb); }
    void setOpacity(BYTE alpha);
    void redraw();

private:
    struct HitRect {
        D2D1_RECT_F rc;
        const char* action;
        std::wstring tooltip;
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMsg(UINT msg, WPARAM wp, LPARAM lp);

    void ensureTarget();
    void releaseTarget();
    void paint();
    void computeLayout(float w, float h);
    int64_t msFromX(float x) const;
    int volFromX(float x) const;
    std::wstring chapterAt(int64_t ms) const;

    D2DContext& _ctx;
    bool _floating = false;
    HWND _hwnd = nullptr;
    ID2D1HwndRenderTarget* _rt = nullptr;
    ControlBarState _state;
    ControlBarCallbacks _cb;

    // Layout rects
    D2D1_RECT_F _seekRect{};
    D2D1_RECT_F _volRect{};
    D2D1_RECT_F _totalTimeRect{};
    std::vector<HitRect> _buttons;

    // Interaction state
    bool _mouseInside = false;
    float _mouseX = -1, _mouseY = -1;
    bool _seekHover = false;
    bool _seekDragging = false;
    bool _volDragging = false;
    int _hoverBtn = -1;
    int _pressedBtn = -1;
    int64_t _lastLiveSeekTick = 0;
    int64_t _lastSeekMs = -1;
};

/// Layered transparent OSD pill + Error toast + Drop overlay rendered over libVLC's HWND.
class D2DOverlay {
public:
    explicit D2DOverlay(D2DContext& ctx);
    ~D2DOverlay();

    bool create(HWND owner, HINSTANCE hInst);
    HWND hwnd() const { return _hwnd; }
    void syncToRect(const RECT& screenRect);

    void showOsd(const std::wstring& text);
    void showError(const std::wstring& text);
    void hideError();
    void setDropOverlay(bool visible, const std::wstring& text = L"Drop media to play");
    void tickAnimation(); // called at ~30-60 Hz while animating

private:
    void render();

    D2DContext& _ctx;
    HWND _hwnd = nullptr;
    RECT _rect{};
    std::wstring _osdText;
    ULONGLONG _osdStartTick = 0;
    float _osdAlpha = 0.0f;

    std::wstring _errorText;
    ULONGLONG _errorStartTick = 0;

    bool _dropVisible = false;
    std::wstring _dropText = L"Drop media to play";
};

/// Direct2D Idle screen (radial vignette + logo + "Open File" pill CTA).
class D2DIdlePanel {
public:
    explicit D2DIdlePanel(D2DContext& ctx);
    ~D2DIdlePanel();

    bool create(HWND parent, HINSTANCE hInst);
    HWND hwnd() const { return _hwnd; }
    std::function<void()> onOpenFile;
    std::function<void()> onDoubleClick;
    void redraw();

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMsg(UINT msg, WPARAM wp, LPARAM lp);
    void ensureTarget();
    void loadLogo();
    void paint();

    D2DContext& _ctx;
    HWND _hwnd = nullptr;
    ID2D1HwndRenderTarget* _rt = nullptr;
    ID2D1Bitmap* _logo = nullptr;
    D2D1_RECT_F _ctaRect{};
    bool _ctaHover = false;
    bool _ctaPressed = false;
};

} // namespace reel
