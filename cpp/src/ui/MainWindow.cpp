#include "ui/MainWindow.h"
#include "core/MediaFiles.h"
#include "core/PlaylistParser.h"
#include "core/StrUtil.h"
#include "core/TimeFormat.h"
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

namespace reel {

namespace {
constexpr UINT_PTR TimerTickId = 1;
constexpr UINT_PTR TimerAnimId = 2;
constexpr UINT_PTR TimerFsHideId = 3;
constexpr UINT_PTR TimerClickPauseId = 4;

// Static menu command IDs
enum CmdId : UINT {
    ID_OPEN_FILE = 1001, ID_OPEN_MULTI, ID_OPEN_FOLDER, ID_OPEN_DISC, ID_OPEN_NETWORK, ID_OPEN_CAPTURE, ID_SAVE_PLAYLIST, ID_QUIT,
    ID_SEEK_FWD10, ID_SEEK_BACK10, ID_JUMP_TIME, ID_RECORD, ID_AB_LOOP, ID_EXPORT_AB, ID_PLAY_PAUSE, ID_STOP, ID_PREV, ID_NEXT, ID_NEXT_FRAME, ID_PREV_FRAME,
    ID_VOL_UP, ID_VOL_DOWN, ID_MUTE,
    ID_FULLSCREEN, ID_MINI_PLAYER, ID_FIT_WINDOW, ID_FILE_BROWSER, ID_ALWAYS_ON_TOP, ID_ROTATE, ID_SNAPSHOT,
    ID_ADD_SUB, ID_SUB_DELAY_DOWN, ID_SUB_DELAY_UP, ID_SUB_SIZE_UP, ID_SUB_SIZE_DOWN, ID_SUB_APPEARANCE,
    ID_EFFECTS, ID_SYNC, ID_MEDIA_INFO, ID_CODEC_INFO, ID_MESSAGES, ID_PREFERENCES,
    ID_TOGGLE_PLAYLIST, ID_MINIMAL, ID_TOGGLE_STATUSBAR,
    ID_SHORTCUTS, ID_USER_GUIDE, ID_ABOUT,
    ID_SPEED_FASTER, ID_SPEED_NORMAL, ID_SPEED_SLOWER,

    // Dynamic command ranges
    ID_DYN_TITLE_BASE = 2000,
    ID_DYN_CHAPTER_BASE = 2200,
    ID_DYN_SPEED_BASE = 2600,
    ID_DYN_AUDIOTRACK_BASE = 2700,
    ID_DYN_AUDIODEV_BASE = 2800,
    ID_DYN_STEREO_BASE = 2900,
    ID_DYN_VIDEOTRACK_BASE = 3000,
    ID_DYN_ZOOM_BASE = 3100,
    ID_DYN_ASPECT_BASE = 3200,
    ID_DYN_CROP_BASE = 3300,
    ID_DYN_DEINT_BASE = 3400,
    ID_DYN_SUBTRACK_BASE = 3500,
};

constexpr float kRates[] = { 0.25f, 0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f };
constexpr const char* kAspects[] = { "", "16:9", "4:3", "1:1", "16:10", "2.21:1", "2.35:1", "2.39:1", "5:4" };
constexpr const char* kCrops[] = { "", "16:10", "16:9", "4:3", "1.85:1", "2.21:1", "2.35:1", "2.39:1", "5:3", "5:4", "1:1" };
struct ZoomDef { const wchar_t* name; float scale; };
constexpr ZoomDef kZooms[] = { {L"Fit to window", 0.0f}, {L"1:4 Quarter", 0.25f}, {L"1:2 Half", 0.5f}, {L"1:1 Original", 1.0f}, {L"2:1 Double", 2.0f} };
struct DeintDef { const wchar_t* name; const char* mode; };
constexpr DeintDef kDeints[] = {
    {L"Off", nullptr}, {L"Blend", "blend"}, {L"Bob", "bob"}, {L"Discard", "discard"}, {L"Linear", "linear"},
    {L"Mean", "mean"}, {L"X", "x"}, {L"Yadif", "yadif"}, {L"Yadif (2x)", "yadif2x"}, {L"Phosphor", "phosphor"}, {L"Film NTSC (IVTC)", "ivtc"}
};
struct StereoDef { const wchar_t* name; int ch; };
constexpr StereoDef kStereos[] = {
    {L"Stereo", libvlc_AudioChannel_Stereo}, {L"Reverse stereo", libvlc_AudioChannel_RStereo},
    {L"Left", libvlc_AudioChannel_Left}, {L"Right", libvlc_AudioChannel_Right}, {L"Dolby Surround", libvlc_AudioChannel_Dolbys}
};
struct TransformDef { const wchar_t* name; const char* type; };
constexpr TransformDef kTransforms[] = {
    {L"None", ""}, {L"Rotate 90°", "90"}, {L"Rotate 180°", "180"}, {L"Rotate 270°", "270"},
    {L"Flip horizontally", "hflip"}, {L"Flip vertically", "vflip"}, {L"Transpose", "transpose"}, {L"Anti-transpose", "antitranspose"}
};
} // namespace

MainWindow::MainWindow(HINSTANCE hInst, D2DContext& d2d, SettingsService& settings, PlayerService& engine, ResumeStore& resume)
    : _hInst(hInst), _d2d(d2d), _settings(settings), _engine(engine), _resume(resume),
      _hotkeys(settings.current().hotkeys) {
    auto& s = _settings.current();
    _playlist.setLoop(s.loop);
    _playlist.setShuffle(s.shuffle);
    _volume = std::clamp(s.volume, 0, 200);
    _muted = s.muted;
}

MainWindow::~MainWindow() {
    _engine.onOpening = nullptr;
    _engine.onStateChanged = nullptr;
    _engine.onEndReached = nullptr;
    _engine.onError = nullptr;
    _engine.onBuffering = nullptr;
    _engine.onLengthChanged = nullptr;
    _engine.onTracksChanged = nullptr;
    _engine.onVoutChanged = nullptr;
    _engine.onSnapshotTaken = nullptr;
    _smtc.dispose();
    if (_mainMenu) { DestroyMenu(_mainMenu); _mainMenu = nullptr; }
    WinInterop::keepAwake(false);
}

bool MainWindow::create() {
    _taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarButtonCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainWindow::mainWndProc;
    wc.hInstance = _hInst;
    wc.hIcon = LoadIconW(_hInst, L"IDI_ICON1");
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"ReelPlayerMainWindow";
    RegisterClassExW(&wc);

    WNDCLASSEXW vwc{};
    vwc.cbSize = sizeof(vwc);
    vwc.style = CS_DBLCLKS;
    vwc.lpfnWndProc = &MainWindow::videoHostProc;
    vwc.hInstance = _hInst;
    vwc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vwc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    vwc.lpszClassName = L"ReelVideoHost";
    RegisterClassExW(&vwc);

    buildMenus();

    const auto& s = _settings.current();
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = std::clamp(static_cast<int>(s.windowWidth), 480, std::max<int>(480, work.right - work.left));
    int h = std::clamp(static_cast<int>(s.windowHeight), 280, std::max<int>(280, work.bottom - work.top));
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (s.windowLeft && s.windowTop) {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        int lx = static_cast<int>(*s.windowLeft), ly = static_cast<int>(*s.windowTop);
        if (lx < vx + vw - 100 && ly < vy + vh - 100 && lx + w > vx + 100 && ly > vy - 20) {
            x = lx; y = ly;
        }
    }

    _hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES, L"ReelPlayerMainWindow", L"Reel Player",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, w, h, nullptr, nullptr, _hInst, this);
    if (!_hwnd) return false;

    // Dark-themed Direct2D menu bar (avoids Windows' unstyled white native menu bar)
    _menuBar = std::make_unique<D2DMenuBar>(_d2d);
    _menuBar->create(_hwnd, _hInst);
    {
        static constexpr const wchar_t* kTopLabels[] = {
            L"Media", L"Playback", L"Audio", L"Video", L"Subtitle", L"Tools", L"View", L"Help"
        };
        std::vector<std::pair<std::wstring, HMENU>> topMenus;
        for (int i = 0; i < 8; ++i) topMenus.emplace_back(kTopLabels[i], GetSubMenu(_mainMenu, i));
        _menuBar->setMenus(std::move(topMenus));
    }

    // Left-hand docked File Browser panel (pure Direct2D directory tree, video-only, resizable)
    _fileBrowser = std::make_unique<FileBrowserPanel>(_d2d);
    _fileBrowser->create(_hwnd, _hInst);
    _fileBrowser->setCallbacks({
        [this](const std::wstring& path) {
            openItems({ std::make_shared<MediaItem>(path) }, true);
        },
        [this](const std::wstring& path) {
            if (auto cur = _playlist.current(); cur && cur->isFile() && _wcsicmp(cur->location.c_str(), path.c_str()) == 0) {
                stopPlayback();
                Sleep(140);
            }
        },
        [this](const std::wstring& path) {
            showOsd(L"Moved to Recycle Bin: " + fs::path(path).filename().wstring());
        },
        [this](const std::wstring& dir) {
            _settings.current().fileBrowserDir = dir;
        },
        [this](int newWidthDip) {
            _settings.current().fileBrowserWidth = static_cast<double>(newWidthDip);
            layoutChildren();
        },
        [this](FileSortMode mode, bool asc) {
            _settings.current().fileBrowserSort = (mode == FileSortMode::Size) ? L"Size"
                : (mode == FileSortMode::DateCreated ? L"Date" : L"Name");
            _settings.current().fileBrowserSortAsc = asc;
            saveState();
        },
        [this]() {
            setFileBrowser(false);
        }
    });
    {
        FileSortMode sm = FileSortMode::Name;
        if (s.fileBrowserSort == L"Size") sm = FileSortMode::Size;
        else if (s.fileBrowserSort == L"Date") sm = FileSortMode::DateCreated;
        _fileBrowser->setSort(sm, s.fileBrowserSortAsc);
    }

    // Video host child HWND
    _videoHost = CreateWindowExW(0, L"ReelVideoHost", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                 0, 0, 640, 400, _hwnd, nullptr, _hInst, this);
    _engine.setHwnd(_videoHost);

    // Direct2D children
    _idlePanel = std::make_unique<D2DIdlePanel>(_d2d);
    _idlePanel->create(_hwnd, _hInst);
    _idlePanel->onOpenFile = [this] { runAction("OpenFile"); };
    _idlePanel->onDoubleClick = [this] { runAction("Fullscreen"); };

    auto makeBarCallbacks = [this]() -> ControlBarCallbacks {
        return {
            [this](const char* act) { runAction(act); },
            [this](int64_t ms, bool dragging) { seek(ms, dragging); },
            [this](bool seeking) { _isUserSeeking = seeking; },
            [this](int vol) { setVolume(vol); },
            [this] {
                _settings.current().showRemainingTime = !_settings.current().showRemainingTime;
                pushControlBarState();
            },
            [this](const char* act) -> std::wstring {
                auto g = _hotkeys.gesturesFor(act);
                return g.empty() ? L"" : toWide(g[0]);
            },
            [this] { ensureAnimTimer(); }
        };
    };

    _dockedBar = std::make_unique<D2DControlBar>(_d2d, false);
    _dockedBar->create(_hwnd, _hInst);
    _dockedBar->setCallbacks(makeBarCallbacks());

    _fullscreenBar = std::make_unique<D2DControlBar>(_d2d, true);
    _fullscreenBar->create(_hwnd, _hInst);
    _fullscreenBar->setCallbacks(makeBarCallbacks());

    _overlay = std::make_unique<D2DOverlay>(_d2d);
    _overlay->create(_hwnd, _hInst);

    // Wire libVLC callbacks (marshal to UI thread via PostMessage)
    _engine.onInitFailed = [this](std::wstring err) {
        AppLog::write(L"[libVLC init] " + err);
        auto* msg = new std::wstring(L"Engine init failed: " + err);
        if (!PostMessageW(_hwnd, WM_APP_OSD_MSG, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
    };
    _engine.onOpening = [this] { postVlcEvent({ VlcEventKind::Opening }); };
    _engine.onStateChanged = [this] { postVlcEvent({ VlcEventKind::StateChanged }); };
    _engine.onEndReached = [this] { postVlcEvent({ VlcEventKind::EndReached }); };
    _engine.onError = [this] { postVlcEvent({ VlcEventKind::Error }); };
    _engine.onBuffering = [this](float c) { postVlcEvent({ VlcEventKind::Buffering, 0, c }); };
    _engine.onLengthChanged = [this](int64_t len) { postVlcEvent({ VlcEventKind::LengthChanged, len }); };
    _engine.onTracksChanged = [this] { postVlcEvent({ VlcEventKind::TracksChanged }); };
    _engine.onVoutChanged = [this](int cnt) { postVlcEvent({ VlcEventKind::VoutChanged, cnt }); };
    _engine.onSnapshotTaken = [this](std::wstring f) { postVlcEvent({ VlcEventKind::SnapshotTaken, 0, 0, std::move(f) }); };

    // Wire SMTC media keys
    _smtc.onButtonPressed = [this](const char* btn) {
        PostMessageW(_hwnd, WM_APP_SMTC_BUTTON, 0, reinterpret_cast<LPARAM>(btn));
    };

    applyTheme();
    if (s.alwaysOnTop) SetWindowPos(_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (s.showFileBrowser) setFileBrowser(true);

    SetTimer(_hwnd, TimerTickId, 250, nullptr);
    layoutChildren();
    pushControlBarState();
    return true;
}

void MainWindow::showAndHandleCommandLine(const CommandLine& cmd) {
    ShowWindow(_hwnd, _settings.current().windowMaximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(_hwnd);
    layoutChildren();
    syncOverlays();
    handleCommandLine(cmd, false);

    // Defer SMTC + Taskbar COM attachment slightly so first video frame starts with zero delay
    _smtc.attach(_hwnd);
    _taskbar.attach(_hwnd);
}

void MainWindow::postForwardedArgs(std::vector<std::wstring> args) {
    std::lock_guard lk(_fwdMtx);
    _fwdQueue.push_back(std::move(args));
}

void MainWindow::postVlcEvent(VlcEventMsg ev) {
    {
        std::lock_guard lk(_evMtx);
        _evQueue.push_back(std::move(ev));
    }
    if (_hwnd) PostMessageW(_hwnd, WM_APP_VLC_EVENT, 0, 0);
}

// ============================================================================
// Menus
// ============================================================================

void MainWindow::buildMenus() {
    _mainMenu = CreateMenu();
    auto addItem = [](HMENU m, UINT id, const wchar_t* text) { AppendMenuW(m, MF_STRING, id, text); };
    auto addSep = [](HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };
    auto addSub = [](HMENU parent, const wchar_t* text) {
        HMENU sub = CreatePopupMenu();
        AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), text);
        return sub;
    };

    HMENU mMedia = addSub(_mainMenu, L"&Media");
    addItem(mMedia, ID_OPEN_FILE, L"&Open File...\tCtrl+O");
    addItem(mMedia, ID_OPEN_MULTI, L"Open &Multiple Files...\tCtrl+Shift+O");
    addItem(mMedia, ID_OPEN_FOLDER, L"Open &Folder...\tCtrl+F");
    addItem(mMedia, ID_OPEN_DISC, L"Open &Disc...\tCtrl+D");
    addItem(mMedia, ID_OPEN_NETWORK, L"Open &Network Stream...\tCtrl+N");
    addItem(mMedia, ID_OPEN_CAPTURE, L"Open &Capture Device...\tCtrl+C");
    addSep(mMedia);
    addItem(mMedia, ID_QUIT, L"&Quit\tCtrl+Q");

    HMENU mPlay = addSub(_mainMenu, L"P&layback");
    _dynTitle = addSub(mPlay, L"&Title");
    _dynChapter = addSub(mPlay, L"&Chapter");
    addSep(mPlay);
    _dynSpeed = addSub(mPlay, L"&Speed");
    addSep(mPlay);
    addItem(mPlay, ID_SEEK_FWD10, L"&Jump Forward\tRight");
    addItem(mPlay, ID_SEEK_BACK10, L"Jump Bac&kward\tLeft");
    addItem(mPlay, ID_JUMP_TIME, L"Jump to Specific &Time...\tCtrl+T");
    addSep(mPlay);
    addItem(mPlay, ID_RECORD, L"&Record");
    addItem(mPlay, ID_AB_LOOP, L"A-B &Loop\tShift+L");
    addItem(mPlay, ID_EXPORT_AB, L"Export A-B &Clip...\tCtrl+Shift+E");
    addSep(mPlay);
    addItem(mPlay, ID_PLAY_PAUSE, L"&Play / Pause\tSpace");
    addItem(mPlay, ID_STOP, L"&Stop\tS");
    addItem(mPlay, ID_PREV, L"Pre&vious\tP");
    addItem(mPlay, ID_NEXT, L"Ne&xt\tN");
    addItem(mPlay, ID_NEXT_FRAME, L"Next &Frame\tE");
    addItem(mPlay, ID_PREV_FRAME, L"Pre&vious Frame\tShift+E");

    HMENU mAudio = addSub(_mainMenu, L"&Audio");
    _dynAudioTrack = addSub(mAudio, L"Audio &Track");
    _dynAudioDevice = addSub(mAudio, L"Audio &Device");
    _dynStereo = addSub(mAudio, L"&Stereo Mode");
    addSep(mAudio);
    addItem(mAudio, ID_VOL_UP, L"&Increase Volume\tUp");
    addItem(mAudio, ID_VOL_DOWN, L"&Decrease Volume\tDown");
    addItem(mAudio, ID_MUTE, L"&Mute\tM");

    HMENU mVideo = addSub(_mainMenu, L"&Video");
    _dynVideoTrack = addSub(mVideo, L"Video &Track");
    addSep(mVideo);
    addItem(mVideo, ID_FULLSCREEN, L"&Fullscreen\tF");
    addItem(mVideo, ID_ALWAYS_ON_TOP, L"Always &on Top");
    addSep(mVideo);
    _dynZoom = addSub(mVideo, L"&Zoom");
    _dynAspect = addSub(mVideo, L"&Aspect Ratio");
    _dynCrop = addSub(mVideo, L"&Crop");
    addSep(mVideo);
    _dynDeint = addSub(mVideo, L"&Deinterlace");
    addSep(mVideo);
    addItem(mVideo, ID_ROTATE, L"&Rotate 90°\tCtrl+R");
    addItem(mVideo, ID_SNAPSHOT, L"Take &Snapshot\tShift+S");

    HMENU mSub = addSub(_mainMenu, L"Subti&tle");
    addItem(mSub, ID_ADD_SUB, L"&Add Subtitle File...");
    _dynSubTrack = addSub(mSub, L"Sub &Track");
    addSep(mSub);
    addItem(mSub, ID_SUB_DELAY_DOWN, L"&Delay -50 ms\tG");
    addItem(mSub, ID_SUB_DELAY_UP, L"D&elay +50 ms\tH");
    addSep(mSub);
    addItem(mSub, ID_SUB_SIZE_UP, L"&Larger Subtitles\tAlt+Up");
    addItem(mSub, ID_SUB_SIZE_DOWN, L"&Smaller Subtitles\tAlt+Down");
    addItem(mSub, ID_SUB_APPEARANCE, L"Subtitle &Appearance...");

    HMENU mTools = addSub(_mainMenu, L"T&ools");
    addItem(mTools, ID_EFFECTS, L"&Effects and Filters...\tCtrl+E");
    addItem(mTools, ID_SYNC, L"&Track Synchronization...");
    addItem(mTools, ID_MEDIA_INFO, L"Media &Information...\tCtrl+I");
    addItem(mTools, ID_CODEC_INFO, L"&Codec Information...\tCtrl+J");
    addSep(mTools);
    addItem(mTools, ID_MESSAGES, L"&Messages...\tCtrl+M");
    addSep(mTools);
    addItem(mTools, ID_PREFERENCES, L"&Preferences...\tCtrl+P");

    HMENU mView = addSub(_mainMenu, L"V&iew");
    addItem(mView, ID_FILE_BROWSER, L"&File Browser\tCtrl+B");
    addSep(mView);
    addItem(mView, ID_FIT_WINDOW, L"Fit &Window to Video\tCtrl+1");
    addItem(mView, ID_MINI_PLAYER, L"&Picture-in-Picture\tShift+P");
    addItem(mView, ID_MINIMAL, L"&Minimal Interface\tCtrl+H");
    addItem(mView, ID_FULLSCREEN, L"&Fullscreen Interface\tF");

    HMENU mHelp = addSub(_mainMenu, L"&Help");
    addItem(mHelp, ID_USER_GUIDE, L"&How to use this player and all its features");
    addSep(mHelp);
    addItem(mHelp, ID_SHORTCUTS, L"&Keyboard Shortcuts");
    addItem(mHelp, ID_ABOUT, L"&About");
}

void MainWindow::populateDynamicSubmenu(HMENU menu) {
    while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
    bool active = _engine.isActive();

    auto addTracks = [&](const std::vector<TrackOption>& tracks, UINT baseId) {
        for (size_t i = 0; i < tracks.size() && i < 100; ++i) {
            AppendMenuW(menu, MF_STRING | (tracks[i].selected ? MF_CHECKED : 0),
                        baseId + static_cast<UINT>(tracks[i].id + 1), tracks[i].name.c_str());
        }
    };

    if (menu == _dynTitle && active) addTracks(_engine.titles(), ID_DYN_TITLE_BASE);
    else if (menu == _dynChapter && active) {
        auto chs = _engine.chapters();
        for (size_t i = 0; i < chs.size() && i < 300; ++i) {
            std::wstring label = chs[i].name + L"  (" + TimeFormat::format(chs[i].startMs) + L")";
            AppendMenuW(menu, MF_STRING, ID_DYN_CHAPTER_BASE + static_cast<UINT>(i), label.c_str());
        }
    } else if (menu == _dynSpeed) {
        AppendMenuW(menu, MF_STRING, ID_SPEED_FASTER, L"&Faster\t]");
        AppendMenuW(menu, MF_STRING, ID_SPEED_NORMAL, L"N&ormal Speed\t=");
        AppendMenuW(menu, MF_STRING, ID_SPEED_SLOWER, L"&Slower\t[");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < std::size(kRates); ++i) {
            wchar_t buf[16];
            std::swprintf(buf, 16, L"%.2gx", kRates[i]);
            bool chk = std::abs(kRates[i] - _rate) < 0.001f;
            AppendMenuW(menu, MF_STRING | (chk ? MF_CHECKED : 0), ID_DYN_SPEED_BASE + static_cast<UINT>(i), buf);
        }
    } else if (menu == _dynAudioTrack && active) addTracks(_engine.audioTracks(), ID_DYN_AUDIOTRACK_BASE);
    else if (menu == _dynAudioDevice) {
        _dynAudioDeviceIds.clear();
        auto devs = _engine.audioDevices();
        for (size_t i = 0; i < devs.size() && i < 80; ++i) {
            _dynAudioDeviceIds.push_back(devs[i].id);
            AppendMenuW(menu, MF_STRING | (devs[i].selected ? MF_CHECKED : 0),
                        ID_DYN_AUDIODEV_BASE + static_cast<UINT>(i), devs[i].name.c_str());
        }
    } else if (menu == _dynStereo) {
        int cur = active ? _engine.audioChannel() : libvlc_AudioChannel_Stereo;
        for (size_t i = 0; i < std::size(kStereos); ++i) {
            UINT f = MF_STRING | (kStereos[i].ch == cur ? MF_CHECKED : 0) | (active ? 0 : MF_GRAYED);
            AppendMenuW(menu, f, ID_DYN_STEREO_BASE + static_cast<UINT>(i), kStereos[i].name);
        }
    } else if (menu == _dynVideoTrack && active) addTracks(_engine.videoTracks(), ID_DYN_VIDEOTRACK_BASE);
    else if (menu == _dynSubTrack && active) addTracks(_engine.subtitleTracks(), ID_DYN_SUBTRACK_BASE);
    else if (menu == _dynZoom) {
        for (size_t i = 0; i < std::size(kZooms); ++i)
            AppendMenuW(menu, MF_STRING | (static_cast<int>(i) == _zoomIdx ? MF_CHECKED : 0), ID_DYN_ZOOM_BASE + static_cast<UINT>(i), kZooms[i].name);
    } else if (menu == _dynAspect) {
        for (size_t i = 0; i < std::size(kAspects); ++i)
            AppendMenuW(menu, MF_STRING | (static_cast<int>(i) == _aspectIdx ? MF_CHECKED : 0), ID_DYN_ASPECT_BASE + static_cast<UINT>(i),
                        i == 0 ? L"Default" : toWide(kAspects[i]).c_str());
    } else if (menu == _dynCrop) {
        for (size_t i = 0; i < std::size(kCrops); ++i)
            AppendMenuW(menu, MF_STRING | (static_cast<int>(i) == _cropIdx ? MF_CHECKED : 0), ID_DYN_CROP_BASE + static_cast<UINT>(i),
                        i == 0 ? L"Default" : toWide(kCrops[i]).c_str());
    } else if (menu == _dynDeint) {
        for (size_t i = 0; i < std::size(kDeints); ++i)
            AppendMenuW(menu, MF_STRING | (static_cast<int>(i) == _deintIdx ? MF_CHECKED : 0), ID_DYN_DEINT_BASE + static_cast<UINT>(i), kDeints[i].name);
    }

    if (GetMenuItemCount(menu) == 0) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(empty)");
}

void MainWindow::refreshMenuStates() {
    auto chk = [&](UINT id, bool on) { CheckMenuItem(_mainMenu, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    auto en = [&](UINT id, bool on) { EnableMenuItem(_mainMenu, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    bool active = _engine.isActive();

    chk(ID_FULLSCREEN, _isFullscreen);
    chk(ID_MINI_PLAYER, _isMiniPlayer);
    chk(ID_FILE_BROWSER, _showFileBrowser);
    chk(ID_ALWAYS_ON_TOP, _settings.current().alwaysOnTop);
    chk(ID_MINIMAL, _isMinimal);
    chk(ID_RECORD, _isRecording);
    chk(ID_AB_LOOP, _abStartMs.has_value());
    chk(ID_MUTE, _muted);

    en(ID_EXPORT_AB, _abStartMs.has_value() && _abEndMs.has_value());
    for (UINT id : { ID_RECORD, ID_SNAPSHOT, ID_NEXT_FRAME, ID_PREV_FRAME, ID_JUMP_TIME, ID_SEEK_FWD10, ID_SEEK_BACK10, ID_ADD_SUB, ID_SUB_DELAY_DOWN, ID_SUB_DELAY_UP, ID_STOP })
        en(id, active);
}

void MainWindow::showContextMenu(int screenX, int screenY) {
    HMENU cm = CreatePopupMenu();
    AppendMenuW(cm, MF_STRING, ID_PLAY_PAUSE, _isPlaying ? L"Pause\tSpace" : L"Play\tSpace");
    AppendMenuW(cm, MF_STRING | (_engine.isActive() ? 0 : MF_GRAYED), ID_STOP, L"Stop\tS");
    AppendMenuW(cm, MF_STRING, ID_PREV, L"Previous\tP");
    AppendMenuW(cm, MF_STRING, ID_NEXT, L"Next\tN");
    AppendMenuW(cm, MF_SEPARATOR, 0, nullptr);

    auto addTrackSubmenu = [&](const wchar_t* label, const std::vector<TrackOption>& tracks, UINT baseId, bool withAddSub = false) {
        HMENU sub = CreatePopupMenu();
        if (withAddSub) {
            AppendMenuW(sub, MF_STRING | (_engine.isActive() ? 0 : MF_GRAYED), ID_ADD_SUB, L"Add Subtitle File...");
            AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        }
        if (_engine.isActive() && !tracks.empty()) {
            for (size_t i = 0; i < tracks.size() && i < 100; ++i) {
                AppendMenuW(sub, MF_STRING | (tracks[i].selected ? MF_CHECKED : 0),
                            baseId + static_cast<UINT>(tracks[i].id + 1), tracks[i].name.c_str());
            }
        } else {
            AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"(empty)");
        }
        AppendMenuW(cm, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), label);
    };

    addTrackSubmenu(L"Audio Track", _engine.isActive() ? _engine.audioTracks() : std::vector<TrackOption>{}, ID_DYN_AUDIOTRACK_BASE);
    addTrackSubmenu(L"Video Track", _engine.isActive() ? _engine.videoTracks() : std::vector<TrackOption>{}, ID_DYN_VIDEOTRACK_BASE);
    addTrackSubmenu(L"Subtitle Track", _engine.isActive() ? _engine.subtitleTracks() : std::vector<TrackOption>{}, ID_DYN_SUBTRACK_BASE, true);

    AppendMenuW(cm, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(cm, MF_STRING | (_isFullscreen ? MF_CHECKED : 0), ID_FULLSCREEN, L"Fullscreen\tF");
    AppendMenuW(cm, MF_STRING | (_isMiniPlayer ? MF_CHECKED : 0), ID_MINI_PLAYER, L"Picture-in-Picture\tShift+P");
    AppendMenuW(cm, MF_STRING | (_hasVideo ? 0 : MF_GRAYED), ID_SNAPSHOT, L"Take Snapshot\tShift+S");
    AppendMenuW(cm, MF_STRING, ID_ROTATE, L"Rotate 90°\tCtrl+R");
    AppendMenuW(cm, MF_STRING | (_abStartMs.has_value() ? MF_CHECKED : 0), ID_AB_LOOP, L"A-B Loop\tShift+L");
    AppendMenuW(cm, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(cm, MF_STRING, ID_OPEN_FILE, L"Open File...\tCtrl+O");
    AppendMenuW(cm, MF_STRING, ID_EFFECTS, L"Effects and Filters...\tCtrl+E");
    AppendMenuW(cm, MF_STRING, ID_MEDIA_INFO, L"Media Information...\tCtrl+I");
    AppendMenuW(cm, MF_STRING, ID_PREFERENCES, L"Preferences...\tCtrl+P");
    TrackPopupMenu(cm, TPM_RIGHTBUTTON, screenX, screenY, 0, _hwnd, nullptr);
    DestroyMenu(cm);
}

// ============================================================================
// Layout & Overlays
// ============================================================================

void MainWindow::layoutChildren() {
    if (!_hwnd) return;
    RECT rc{};
    GetClientRect(_hwnd, &rc);
    int w = std::max<int>(1, rc.right);
    int h = std::max<int>(1, rc.bottom);
    float scale = GetDpiForWindow(_hwnd) / 96.0f;
    if (scale <= 0.0f) scale = 1.0f;

    int menuH = (_isFullscreen || _isMiniPlayer || _isMinimal) ? 0 : static_cast<int>(std::lround(D2DMenuBar::BarHeight * scale));
    int barH = (_isFullscreen || _isMiniPlayer) ? 0 : static_cast<int>(std::lround(D2DControlBar::BarHeight * scale));
    int browserW = 0;
    if (_fileBrowser && _showFileBrowser && !_isFullscreen && !_isMiniPlayer && !_isMinimal) {
        int desired = static_cast<int>(std::lround(
            std::clamp(_settings.current().fileBrowserWidth,
                       static_cast<double>(FileBrowserPanel::MinWidthDip),
                       static_cast<double>(FileBrowserPanel::MaxWidthDip)) * scale));
        int maxAllowed = std::max(0, w - static_cast<int>(std::lround(240.0f * scale)));
        browserW = std::min(desired, maxAllowed);
    }
    int vidW = std::max<int>(1, w - browserW);
    int vidH = std::max<int>(1, h - menuH - barH);

    if (_menuBar) {
        if (menuH > 0) {
            MoveWindow(_menuBar->hwnd(), 0, 0, w, menuH, TRUE);
            ShowWindow(_menuBar->hwnd(), SW_SHOW);
        } else {
            ShowWindow(_menuBar->hwnd(), SW_HIDE);
        }
    }
    if (_fileBrowser) {
        if (browserW > 0) {
            MoveWindow(_fileBrowser->hwnd(), 0, menuH, browserW, std::max<int>(1, h - menuH), TRUE);
            ShowWindow(_fileBrowser->hwnd(), SW_SHOW);
        } else {
            ShowWindow(_fileBrowser->hwnd(), SW_HIDE);
        }
    }
    if (_dockedBar) {
        if (_isFullscreen || _isMiniPlayer) ShowWindow(_dockedBar->hwnd(), SW_HIDE);
        else {
            MoveWindow(_dockedBar->hwnd(), browserW, menuH + vidH, vidW, barH, TRUE);
            ShowWindow(_dockedBar->hwnd(), SW_SHOW);
        }
    }
    if (_videoHost) MoveWindow(_videoHost, browserW, menuH, vidW, vidH, TRUE);
    if (_idlePanel) {
        MoveWindow(_idlePanel->hwnd(), browserW, menuH, vidW, vidH, TRUE);
        ShowWindow(_idlePanel->hwnd(), _isIdle ? SW_SHOW : SW_HIDE);
    }
    syncOverlays();
}

void MainWindow::syncOverlays() {
    if (!_hwnd || !_videoHost) return;
    RECT vr{};
    GetWindowRect(_videoHost, &vr);
    if (_overlay) _overlay->syncToRect(vr);

    if (_fullscreenBar && (_isFullscreen || _isMiniPlayer) && (_fsBarVisible || _fsBarAlpha > 1.0f)) {
        float scale = GetDpiForWindow(_hwnd) / 96.0f;
        if (scale <= 0.0f) scale = 1.0f;
        int vw = vr.right - vr.left;
        int barH = static_cast<int>(std::lround((_isMiniPlayer ? 64 : D2DControlBar::BarHeight) * scale));
        int bw = _isMiniPlayer
            ? std::max(static_cast<int>(240 * scale), vw - static_cast<int>(16 * scale))
            : std::min(static_cast<int>(960 * scale), std::max(static_cast<int>(460 * scale), vw - static_cast<int>(72 * scale)));
        int bx = vr.left + (vw - bw) / 2;
        int by = vr.bottom - barH - static_cast<int>((_isMiniPlayer ? 8 : 22) * scale);
        SetWindowPos(_fullscreenBar->hwnd(), HWND_TOPMOST, bx, by, bw, barH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// ============================================================================
// Actions & Playback
// ============================================================================

void MainWindow::runAction(const std::string& action) {
    if (action == "OpenFile" || action == "OpenMultiple") {
        auto files = Dialogs::openFiles(_hwnd, _settings.current().lastFolder, action == "OpenMultiple");
        if (!files.empty()) openLocations(files, true);
    } else if (action == "OpenFolder") {
        if (auto dir = Dialogs::openFolder(_hwnd, _settings.current().lastFolder)) openLocations({ *dir }, true);
    } else if (action == "OpenDisc") {
        if (auto item = Dialogs::showOpenDisc(_hwnd)) openItems({ *item }, true);
    } else if (action == "OpenNetwork") {
        if (auto item = Dialogs::showOpenNetwork(_hwnd)) openItems({ *item }, true);
    } else if (action == "OpenCapture") {
        if (auto item = Dialogs::showOpenCapture(_hwnd)) openItems({ *item }, true);
    } else if (action == "Quit") {
        PostMessageW(_hwnd, WM_CLOSE, 0, 0);
    } else if (action == "PlayPause") togglePlayPause();
    else if (action == "Stop") stopPlayback();
    else if (action == "Next") nextItem();
    else if (action == "Previous") previousItem();
    else if (action == "NextFrame") { if (_engine.isActive()) _engine.nextFrame(); }
    else if (action == "PrevFrame") previousFrame();
    else if (action == "SeekBack10") seekRelative(-10000);
    else if (action == "SeekFwd10") seekRelative(10000);
    else if (action == "SeekBack3") seekRelative(-3000);
    else if (action == "SeekFwd3") seekRelative(3000);
    else if (action == "SeekBack60") seekRelative(-60000);
    else if (action == "SeekFwd60") seekRelative(60000);
    else if (action == "SeekBack300") seekRelative(-300000);
    else if (action == "SeekFwd300") seekRelative(300000);
    else if (action == "Faster") {
        float next = kRates[std::size(kRates) - 1];
        for (float r : kRates) if (r > _rate + 0.001f) { next = r; break; }
        setRate(next);
    } else if (action == "Slower") {
        float prev = kRates[0];
        for (float r : kRates) if (r < _rate - 0.001f) prev = r;
        setRate(prev);
    } else if (action == "NormalSpeed") setRate(1.0f);
    else if (action == "JumpToTime") {
        if (!_engine.isActive()) return;
        if (auto t = Dialogs::showJumpToTime(_hwnd, _engine.timeMs(), _engine.lengthMs())) seek(*t);
    } else if (action == "Record") toggleRecord();
    else if (action == "AbLoop") cycleAbLoop();
    else if (action == "ExportAbClip") exportAbClip();
    else if (action == "CycleLoop") {
        auto next = _playlist.loop() == LoopMode::Off ? LoopMode::One : LoopMode::Off;
        _playlist.setLoop(next);
        _settings.current().loop = next;
        showOsd(next == LoopMode::One ? L"Loop: on" : L"Loop: off");
        pushControlBarState();
    } else if (action == "AcceptResume") acceptResume();
    else if (action == "DismissResume") dismissResume();
    else if (action == "VolumeUp") changeVolume(5);
    else if (action == "VolumeDown") changeVolume(-5);
    else if (action == "Mute") toggleMute();
    else if (action == "CycleAudio") cycleAudioTrack();
    else if (action == "AudioDelayDown") { int64_t ms = _engine.audioDelayUs() / 1000 - 50; _engine.setAudioDelayUs(ms * 1000); showOsd(L"Audio delay: " + std::to_wstring(ms) + L" ms"); }
    else if (action == "AudioDelayUp") { int64_t ms = _engine.audioDelayUs() / 1000 + 50; _engine.setAudioDelayUs(ms * 1000); showOsd(L"Audio delay: " + std::to_wstring(ms) + L" ms"); }
    else if (action == "Fullscreen") setFullscreen(!_isFullscreen);
    else if (action == "MiniPlayer") setMiniPlayer(!_isMiniPlayer);
    else if (action == "FitWindow") fitWindowToVideo(true);
    else if (action == "FileBrowser") setFileBrowser(!_showFileBrowser);
    else if (action == "SubSizeUp") stepSubtitleSize(1);
    else if (action == "SubSizeDown") stepSubtitleSize(-1);
    else if (action == "SubAppearance") openSubtitleAppearance();
    else if (action == "ExitFullscreen") {
        if (_isFullscreen) setFullscreen(false);
        else if (_isMiniPlayer) setMiniPlayer(false);
    }
    else if (action == "AlwaysOnTop") {
        bool on = !_settings.current().alwaysOnTop;
        _settings.current().alwaysOnTop = on;
        SetWindowPos(_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        showOsd(on ? L"Always on top: on" : L"Always on top: off");
    } else if (action == "CycleAspect") setAspect(_aspectIdx + 1);
    else if (action == "CycleCrop") setCrop(_cropIdx + 1);
    else if (action == "CycleZoom") setZoom(_zoomIdx + 1);
    else if (action == "Snapshot") takeSnapshot();
    else if (action == "RotateVideo") rotateVideo();
    else if (action == "ShowTime") {
        showOsd(_engine.isActive() ? (TimeFormat::format(_engine.timeMs()) + L" / " + TimeFormat::format(_engine.lengthMs())) : L"Stopped");
    } else if (action == "AddSubtitle") {
        if (!_engine.isActive()) { showOsd(L"Play something first"); return; }
        std::wstring initDir = _settings.current().lastFolder;
        if (auto cur = _playlist.current(); cur && cur->isFile()) initDir = fs::path(cur->location).parent_path().wstring();
        if (auto sub = Dialogs::openSubtitle(_hwnd, initDir)) {
            _engine.addSubtitle(*sub);
            showOsd(L"Subtitle added: " + fs::path(*sub).filename().wstring());
        }
    } else if (action == "CycleSubtitle") cycleSubtitleTrack();
    else if (action == "SubDelayDown") { int64_t ms = _engine.spuDelayUs() / 1000 - 50; _engine.setSpuDelayUs(ms * 1000); showOsd(L"Subtitle delay: " + std::to_wstring(ms) + L" ms"); }
    else if (action == "SubDelayUp") { int64_t ms = _engine.spuDelayUs() / 1000 + 50; _engine.setSpuDelayUs(ms * 1000); showOsd(L"Subtitle delay: " + std::to_wstring(ms) + L" ms"); }
    else if (action == "Effects") {
        Dialogs::showEffects(_hwnd, _effects,
            [this] { _engine.applyEqualizer(_effects.eqEnabled, _effects.eqPreamp, _effects.eqBands); },
            [this] { _engine.setAdjust(_effects.adjustEnabled, _effects.brightness, _effects.contrast, _effects.hue, _effects.saturation, _effects.gamma); },
            [this] { applyVideoFilters(L"Video filters applied"); });
    } else if (action == "Sync") {
        Dialogs::showSync(_hwnd, _engine,
            [this](int64_t ms) { _engine.setAudioDelayUs(ms * 1000); showOsd(L"Audio delay: " + std::to_wstring(ms) + L" ms"); },
            [this](int64_t ms) { _engine.setSpuDelayUs(ms * 1000); showOsd(L"Subtitle delay: " + std::to_wstring(ms) + L" ms"); });
    } else if (action == "MediaInfo" || action == "CodecInfo") Dialogs::showMediaInfo(_hwnd, _engine);
    else if (action == "Messages") Dialogs::showMessages(_hwnd, _engine.log());
    else if (action == "Preferences") {
        bool restart = false;
        if (Dialogs::showPreferences(_hwnd, _settings, restart)) {
            _hotkeys = HotkeyMap(_settings.current().hotkeys);
            applyTheme();
            saveState();
            if (restart) showOsd(L"Some changes apply after restarting Reel Player");
        }
    } else if (action == "MinimalInterface") setMinimal(!_isMinimal);
    else if (action == "UserGuide") Dialogs::showUserGuide(_hwnd);
    else if (action == "Shortcuts") Dialogs::showShortcuts(_hwnd, _hotkeys);
    else if (action == "About") Dialogs::showAbout(_hwnd);
}

void MainWindow::openLocations(const std::vector<std::wstring>& locations, bool play, int64_t startMs) {
    std::vector<std::wstring> rest;
    for (const auto& loc : locations) {
        if (trim(loc).empty()) continue;
        std::error_code ec;
        if (fs::is_regular_file(loc, ec) && MediaFiles::isSubtitle(loc)) {
            if (_engine.isActive()) {
                _engine.addSubtitle(loc);
                showOsd(L"Subtitle added: " + fs::path(loc).filename().wstring());
            }
            continue;
        }
        rest.push_back(loc);
    }

    std::vector<MediaItemPtr> items;
    for (const auto& loc : rest) {
        std::error_code ec;
        if (fs::is_directory(loc, ec)) {
            _settings.current().lastFolder = loc;
            for (const auto& f : MediaFiles::enumerateFolder(loc)) items.push_back(std::make_shared<MediaItem>(f));
        } else if (PlaylistParser::shouldExpand(loc)) {
            _settings.current().lastFolder = fs::path(loc).parent_path().wstring();
            auto loaded = PlaylistParser::load(loc);
            items.insert(items.end(), loaded.begin(), loaded.end());
        } else {
            if (fs::is_regular_file(loc, ec)) _settings.current().lastFolder = fs::path(loc).parent_path().wstring();
            items.push_back(std::make_shared<MediaItem>(loc));
        }
    }
    if (!items.empty()) openItems(items, play, startMs);
}

void MainWindow::openItems(const std::vector<MediaItemPtr>& items, bool play, int64_t startMs) {
    if (items.empty()) return;
    std::vector<MediaItemPtr> resolved;
    std::vector<MediaItemPtr> toAdd;
    for (const auto& item : items) {
        MediaItemPtr found;
        if (item->options.empty() && item->subtitleFile.empty()) {
            for (const auto& ex : _playlist.items()) {
                if (equalsIgnoreCase(ex->location, item->location)) { found = ex; break; }
            }
        }
        auto chosen = found ? found : item;
        resolved.push_back(chosen);
        if (!found && std::find(toAdd.begin(), toAdd.end(), chosen) == toAdd.end()) toAdd.push_back(chosen);
    }
    if (!toAdd.empty()) _playlist.addRange(toAdd);
    if (play) playItem(resolved[0], startMs);
    else showOsd(resolved.size() == 1 ? (L"Added: " + resolved[0]->title) : (L"Added " + std::to_wstring(resolved.size()) + L" items"));
}

void MainWindow::playItem(const MediaItemPtr& item, int64_t startMs, bool offerResume) {
    saveResumePosition();
    _playGeneration++;
    clearAbLoop(true);
    dismissResume();
    _playlist.setCurrent(item);
    _appliedForCurrentMedia = false;
    _fittedForCurrentMedia = false;
    _restart.reset();
    if (_fileBrowser) _fileBrowser->setActiveFile(item->isFile() ? item->location : L"");
    if (item->isFile()) {
        fs::path p(item->location);
        if (p.has_parent_path()) {
            std::wstring dir = p.parent_path().wstring();
            if (_wcsicmp(_settings.current().fileBrowserDir.c_str(), dir.c_str()) != 0) {
                _settings.current().fileBrowserDir = dir;
                if (_showFileBrowser && _fileBrowser) _fileBrowser->openDirectory(dir);
            }
        }
    }
    if (_isRecording) { _isRecording = false; _recordPath.clear(); }

    _title = item->title;
    _isIdle = false;
    _posMs = startMs;
    _lenMs = item->durationMs;
    _chapters.clear();
    if (_idlePanel) ShowWindow(_idlePanel->hwnd(), SW_HIDE);

    _engine.open(item, startMs, buildExtraOptions());

    if (offerResume && startMs == 0 && _settings.current().resumePlayback && item->isFile()) {
        if (auto pos = _resume.get(item->location)) {
            _resumeOffer = { item, *pos };
            _resumeOfferTick = GetTickCount64();
        }
    }
    updateWindowTitle();
    pushControlBarState();
}

void MainWindow::restartCurrent(const std::wstring& osd) {
    auto item = _playlist.current();
    if (!item || !_engine.isActive()) return;
    int64_t pos = std::max<int64_t>(0, _engine.timeMs());
    _appliedForCurrentMedia = false;
    _restart = RestartInfo{ _engine.state() == libvlc_Paused, osd, _engine.audioDelayUs(), _engine.spuDelayUs() };
    _engine.open(item, pos, buildExtraOptions());
}

void MainWindow::applyVideoFilters(const std::wstring& osd) {
    if (_rebuilding) return;
    _rebuilding = true;
    _rebuildGen = _playGeneration;
    _rebuildItem = _engine.isActive() ? _playlist.current() : nullptr;
    _rebuildPosMs = _rebuildItem ? std::max<int64_t>(0, _engine.timeMs()) : 0;
    _rebuildRestart = RestartInfo{ _engine.state() == libvlc_Paused, osd, _engine.audioDelayUs(), _engine.spuDelayUs() };

    auto opts = PlayerService::videoFilterOptions(kTransforms[_effects.transformIndex].type, _effects.sharpen, _effects.sharpenSigma);
    HWND hwnd = _hwnd;
    std::thread([this, opts = std::move(opts), hwnd] {
        auto fut = _engine.rebuildAsync(opts);
        bool ok = fut.get();
        PostMessageW(hwnd, WM_APP_REBUILD_DONE, ok ? 1 : 0, 0);
    }).detach();
}

std::vector<std::string> MainWindow::buildExtraOptions() const {
    std::vector<std::string> opts;
    if (_isRecording && !_recordPath.empty()) {
        std::string dst = toUtf8(_recordPath);
        opts.push_back(":sout=#duplicate{dst=display,dst=std{access=file,mux=ts,dst='" + dst + "'}}");
        opts.push_back(":sout-keep");
    }
    return opts;
}

void MainWindow::togglePlayPause() {
    auto st = _engine.state();
    if (st == libvlc_Playing) _engine.pause();
    else if (st == libvlc_Paused) _engine.resume();
    else if (auto cur = _playlist.current()) playItem(cur);
    else if (auto first = _playlist.peekNext(true)) playItem(first);
    else runAction("OpenFile");
}

void MainWindow::stopPlayback() {
    saveResumePosition();
    _playGeneration++;
    dismissResume();
    clearAbLoop(true);
    _engine.stopKeepItem();
    _isRecording = false;
    _posMs = 0;
    _isIdle = true;
    _isPlaying = false;
    _isPaused = false;
    _hasVideo = false;
    if (_idlePanel) ShowWindow(_idlePanel->hwnd(), SW_SHOW);
    WinInterop::keepAwake(false);
    updateWindowTitle();
    pushControlBarState();
}

void MainWindow::nextItem() {
    if (auto next = _playlist.peekNext(true)) { playItem(next); return; }
    if (tryPlaySibling(+1)) return;
    showOsd(L"End of playlist");
}

void MainWindow::previousItem() {
    if (auto prev = _playlist.peekPrevious()) { playItem(prev); return; }
    if (tryPlaySibling(-1)) return;
    if (auto cur = _playlist.current()) {
        if (_engine.isActive()) seek(0);
        else playItem(cur);
    }
}

bool MainWindow::tryPlaySibling(int offset) {
    if (!_settings.current().autoAdvanceFolder) return false;
    auto cur = _playlist.current();
    if (!cur || !cur->isFile()) return false;
    auto sib = MediaFiles::findSiblingMedia(cur->location, offset);
    if (!sib) return false;
    openItems({ std::make_shared<MediaItem>(*sib) }, true);
    return true;
}

void MainWindow::seek(int64_t ms, bool asyncCoalesced) {
    if (!_engine.isActive() || !_engine.isSeekable()) return;
    int64_t len = _engine.lengthMs();
    ms = std::max<int64_t>(0, len > 0 ? std::min<int64_t>(ms, len - 500) : ms);
    _engine.setTimeMs(ms, asyncCoalesced);
    _posMs = ms;
    _lastSeekTick = GetTickCount64();
    if (!asyncCoalesced) _smtc.updateTimeline(ms, len);
    pushControlBarState();
}

void MainWindow::seekRelative(int64_t deltaMs) {
    if (!_engine.isActive()) return;
    if (!_engine.isSeekable()) { showOsd(L"This media is not seekable"); return; }
    // If user taps arrow keys rapidly before VLC finishes the previous seek, accumulate from _posMs
    int64_t base = (GetTickCount64() - _lastSeekTick < 500) ? _posMs : _engine.timeMs();
    int64_t target = std::max<int64_t>(0, base + deltaMs);
    int64_t len = _engine.lengthMs();
    if (len > 0) target = std::min<int64_t>(target, std::max<int64_t>(0, len - 500));
    seek(target);
    showOsd(TimeFormat::format(target) + L" / " + TimeFormat::format(len));
}

void MainWindow::setVolume(int vol) {
    _volume = std::clamp(vol, 0, 200);
    _settings.current().volume = _volume;
    if (_engine.isActive()) _engine.setVolume(_volume);
    pushControlBarState();
}

void MainWindow::changeVolume(int delta) {
    setVolume(_volume + delta);
    if (_muted && delta > 0) { _muted = false; _settings.current().muted = false; if (_engine.isActive()) _engine.setMute(false); }
    showOsd(L"Volume " + std::to_wstring(_volume) + L"%");
}

void MainWindow::toggleMute() {
    _muted = !_muted;
    _settings.current().muted = _muted;
    if (_engine.isActive()) _engine.setMute(_muted);
    showOsd(_muted ? L"Muted" : (L"Volume " + std::to_wstring(_volume) + L"%"));
    pushControlBarState();
}

void MainWindow::setRate(float r) {
    _rate = std::clamp(r, 0.25f, 4.0f);
    if (_engine.isActive()) _engine.setRate(_rate);
    wchar_t buf[32];
    std::swprintf(buf, 32, L"Speed: %.2fx", _rate);
    showOsd(buf);
    pushControlBarState();
}

void MainWindow::cycleAbLoop() {
    if (!_engine.isActive() || !_engine.isSeekable()) { showOsd(L"Play seekable media for A-B loop"); return; }
    int64_t now = std::max<int64_t>(0, _engine.timeMs());
    if (!_abStartMs) {
        _abStartMs = now;
        _abEndMs.reset();
        showOsd(L"A-B loop: point A set (" + TimeFormat::format(now) + L")");
    } else if (!_abEndMs) {
        if (now <= *_abStartMs + 250) { showOsd(L"Point B must be after point A"); return; }
        _abEndMs = now;
        seek(*_abStartMs);
        showOsd(L"A-B loop: " + TimeFormat::format(*_abStartMs) + L" - " + TimeFormat::format(now));
    } else {
        clearAbLoop();
    }
    pushControlBarState();
}

void MainWindow::clearAbLoop(bool quiet) {
    if (!_abStartMs && !_abEndMs) return;
    _abStartMs.reset();
    _abEndMs.reset();
    if (!quiet) showOsd(L"A-B loop: off");
    pushControlBarState();
}

void MainWindow::previousFrame() {
    if (!_engine.isActive() || !_engine.isSeekable()) return;
    if (_isPlaying) _engine.pause();
    double fps = _engine.videoFps();
    int64_t stepMs = fps > 0.0 ? std::max<int64_t>(16, static_cast<int64_t>(std::lround(1000.0 / fps))) : 40;
    int64_t target = std::max<int64_t>(0, _engine.timeMs() - stepMs);
    seek(target);
}

void MainWindow::exportAbClip() {
    if (!_abStartMs || !_abEndMs) { showOsd(L"Set both A and B points first (Shift+L)"); return; }
    auto item = _engine.currentItem();
    if (!item || !item->isFile()) { showOsd(L"Export A-B clip requires a local file"); return; }

    int64_t startMs = *_abStartMs;
    int64_t endMs = *_abEndMs;
    fs::path src(item->location);
    std::wstring aStr = TimeFormat::format(startMs); std::replace(aStr.begin(), aStr.end(), L':', L'.');
    std::wstring bStr = TimeFormat::format(endMs);   std::replace(bStr.begin(), bStr.end(), L':', L'.');
    fs::path dst = src.parent_path() / (src.stem().wstring() + L"-clip-" + aStr + L"-" + bStr + src.extension().wstring());

    showOsd(L"Exporting clip...");
    HWND hwnd = _hwnd;
    std::wstring srcPath = src.wstring();
    std::wstring dstPath = dst.wstring();
    std::thread([hwnd, srcPath, dstPath, startMs, endMs] {
        wchar_t ssBuf[32], toBuf[32];
        std::swprintf(ssBuf, 32, L"%.3f", startMs / 1000.0);
        std::swprintf(toBuf, 32, L"%.3f", endMs / 1000.0);
        std::wstring cmd = L"ffmpeg -y -ss " + std::wstring(ssBuf) + L" -to " + std::wstring(toBuf) +
                           L" -i \"" + srcPath + L"\" -c copy \"" + dstPath + L"\"";
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        std::wstring* msg = nullptr;
        if (ok) {
            WaitForSingleObject(pi.hProcess, 60000);
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            msg = new std::wstring(code == 0
                ? (L"Clip saved: " + fs::path(dstPath).filename().wstring())
                : L"Clip export failed (ffmpeg error)");
        } else {
            msg = new std::wstring(L"Install ffmpeg on PATH to export lossless A-B clips");
        }
        PostMessageW(hwnd, WM_APP_OSD_MSG, 0, reinterpret_cast<LPARAM>(msg));
    }).detach();
}

void MainWindow::acceptResume() {
    if (_resumeOffer && _resumeOffer->first == _playlist.current()) seek(_resumeOffer->second);
    dismissResume();
}

void MainWindow::dismissResume() {
    if (!_resumeOffer) return;
    _resumeOffer.reset();
    pushControlBarState();
}

void MainWindow::takeSnapshot() {
    if (!_engine.isActive() || !_hasVideo) { showOsd(L"No video to snapshot"); return; }
    auto item = _engine.currentItem();
    std::wstring folder = _settings.current().snapshotFolder;
    std::wstring rawStem = _title;
    if (item && item->isFile()) {
        std::error_code ec;
        auto parent = fs::path(item->location).parent_path();
        if (!parent.empty() && fs::exists(parent, ec)) folder = parent.wstring();
        rawStem = fs::path(item->location).stem().wstring();
    }
    std::error_code ec;
    fs::create_directories(folder, ec);

    std::wstring stem;
    for (wchar_t c : rawStem) {
        if (wcschr(L"<>:\"/\\|?*", c) == nullptr) stem.push_back(c);
    }
    if (stem.size() > 60) stem.resize(60);

    std::wstring ts = TimeFormat::format(_engine.timeMs());
    std::replace(ts.begin(), ts.end(), L':', L'.');
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t hms[16];
    std::swprintf(hms, 16, L"%02u%02u%02u", st.wHour, st.wMinute, st.wSecond);
    std::wstring ext = _settings.current().snapshotFormat == L"jpg" ? L"jpg" : L"png";
    std::wstring file = (fs::path(folder) / (stem + L"-" + ts + L"-" + hms + L"." + ext)).wstring();
    if (!_engine.takeSnapshot(file)) showOsd(L"Snapshot failed");
}

void MainWindow::toggleRecord() {
    if (!_engine.isActive()) { showOsd(L"Nothing to record"); return; }
    if (!_isRecording) {
        auto dir = SettingsService::defaultVideosFolder();
        std::error_code ec;
        fs::create_directories(dir, ec);
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t fname[64];
        std::swprintf(fname, 64, L"Recording-%04u%02u%02u-%02u%02u%02u.ts", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        _recordPath = (fs::path(dir) / fname).wstring();
        _isRecording = true;
        restartCurrent(L"Recording to " + std::wstring(fname));
    } else {
        _isRecording = false;
        std::wstring name = fs::path(_recordPath).filename().wstring();
        _recordPath.clear();
        restartCurrent(L"Recording stopped: " + name);
    }
}

void MainWindow::rotateVideo() {
    int next = _effects.transformIndex == 1 ? 2 : (_effects.transformIndex == 2 ? 3 : (_effects.transformIndex == 3 ? 0 : 1));
    _effects.transformIndex = next;
    std::wstring label = next == 0 ? L"Rotation: none" : (L"Rotation: " + std::wstring(kTransforms[next].name));
    applyVideoFilters(label);
    if (!_engine.isActive()) showOsd(label);
}

void MainWindow::setAspect(int idx) {
    int n = static_cast<int>(std::size(kAspects));
    _aspectIdx = (idx % n + n) % n;
    _engine.setAspectRatio(kAspects[_aspectIdx][0] ? kAspects[_aspectIdx] : nullptr);
    showOsd(L"Aspect ratio: " + (_aspectIdx == 0 ? std::wstring(L"Default") : toWide(kAspects[_aspectIdx])));
}

void MainWindow::setCrop(int idx) {
    int n = static_cast<int>(std::size(kCrops));
    _cropIdx = (idx % n + n) % n;
    _engine.setCropGeometry(kCrops[_cropIdx][0] ? kCrops[_cropIdx] : nullptr);
    showOsd(L"Crop: " + (_cropIdx == 0 ? std::wstring(L"Default") : toWide(kCrops[_cropIdx])));
}

void MainWindow::setZoom(int idx) {
    int n = static_cast<int>(std::size(kZooms));
    _zoomIdx = (idx % n + n) % n;
    _engine.setScale(kZooms[_zoomIdx].scale);
    showOsd(L"Zoom: " + std::wstring(kZooms[_zoomIdx].name));
}

void MainWindow::setDeinterlace(int idx) {
    if (idx < 0 || idx >= static_cast<int>(std::size(kDeints))) return;
    _deintIdx = idx;
    _engine.setDeinterlace(kDeints[idx].mode);
    showOsd(L"Deinterlace: " + std::wstring(kDeints[idx].name));
}

void MainWindow::cycleAudioTrack() {
    auto tracks = _engine.audioTracks();
    std::vector<TrackOption> valid;
    for (const auto& t : tracks) if (t.id >= 0) valid.push_back(t);
    if (valid.empty()) { showOsd(L"No audio tracks"); return; }
    int idx = -1;
    for (int i = 0; i < static_cast<int>(valid.size()); ++i) if (valid[i].selected) { idx = i; break; }
    const auto& next = valid[(idx + 1) % valid.size()];
    _engine.setAudioTrack(next.id);
    showOsd(L"Audio track: " + next.name);
}

void MainWindow::cycleSubtitleTrack() {
    auto tracks = _engine.subtitleTracks();
    if (tracks.empty()) { showOsd(L"No subtitle tracks"); return; }
    int idx = -1;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) if (tracks[i].selected) { idx = i; break; }
    const auto& next = tracks[(idx + 1) % tracks.size()];
    _engine.setSubtitleTrack(next.id);
    showOsd(L"Subtitle track: " + next.name);
}

// ============================================================================
// Fullscreen, Minimal, Playlist, Theme
// ============================================================================

void MainWindow::setFullscreen(bool on) {
    if (on == _isFullscreen) return;
    if (on && _isMiniPlayer) setMiniPlayer(false);
    _isFullscreen = on;
    if (on) {
        _savedStyle = static_cast<DWORD>(GetWindowLongPtrW(_hwnd, GWL_STYLE));
        _savedExStyle = static_cast<DWORD>(GetWindowLongPtrW(_hwnd, GWL_EXSTYLE));
        _savedPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(_hwnd, &_savedPlacement);
        _savedMaximized = IsZoomed(_hwnd) != 0;

        SetWindowLongPtrW(_hwnd, GWL_STYLE, (_savedStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_POPUP);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        // HWND_TOP + SWP_FRAMECHANGED hides the taskbar immediately and shows only the video edge-to-edge.
        SetWindowPos(_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        layoutChildren();
        showFullscreenBar();
    } else {
        hideFullscreenBar(true);
        if (_cursorHidden) { ShowCursor(TRUE); _cursorHidden = false; }
        SetWindowLongPtrW(_hwnd, GWL_STYLE, _savedStyle);
        SetWindowLongPtrW(_hwnd, GWL_EXSTYLE, _savedExStyle);
        if (_savedMaximized) {
            // Restore directly to maximized work area without bouncing through Normal!
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            UINT dpi = GetDpiForWindow(_hwnd);
            int border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            SetWindowPos(_hwnd, nullptr,
                         mi.rcWork.left - border, mi.rcWork.top - border,
                         (mi.rcWork.right - mi.rcWork.left) + border * 2,
                         (mi.rcWork.bottom - mi.rcWork.top) + border * 2,
                         SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPlacement(_hwnd, &_savedPlacement);
            SetWindowPos(_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        layoutChildren();
    }
    pushControlBarState();
}

void MainWindow::setMiniPlayer(bool on) {
    if (on == _isMiniPlayer) return;
    if (on && _isFullscreen) setFullscreen(false);
    _isMiniPlayer = on;
    if (on) {
        _savedStyle = static_cast<DWORD>(GetWindowLongPtrW(_hwnd, GWL_STYLE));
        _savedExStyle = static_cast<DWORD>(GetWindowLongPtrW(_hwnd, GWL_EXSTYLE));
        _savedPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(_hwnd, &_savedPlacement);
        _savedMaximized = IsZoomed(_hwnd) != 0;

        SetWindowLongPtrW(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_THICKFRAME | WS_CLIPCHILDREN);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        float scale = GetDpiForWindow(_hwnd) / 96.0f;
        if (scale <= 0.0f) scale = 1.0f;
        float pipW = 440.0f, pipH = 248.0f;
        unsigned vw = 0, vh = 0;
        if (_engine.videoSize(vw, vh) && vw > 0 && vh > 0) {
            float fit = std::min(440.0f / static_cast<float>(vw), 340.0f / static_cast<float>(vh));
            pipW = std::clamp(static_cast<float>(vw) * fit, 260.0f, 440.0f);
            pipH = std::clamp(static_cast<float>(vh) * fit, 180.0f, 340.0f);
        }
        int mw = static_cast<int>(std::lround(pipW * scale));
        int mh = static_cast<int>(std::lround(pipH * scale));
        int margin = static_cast<int>(std::lround(20 * scale));
        SetWindowPos(_hwnd, HWND_TOPMOST,
                     mi.rcWork.right - mw - margin, mi.rcWork.bottom - mh - margin,
                     mw, mh, SWP_FRAMECHANGED);
        SetFocus(_hwnd);
        layoutChildren();
        showOsd(L"Picture-in-Picture (drag to move, Esc to exit)");
    } else {
        hideFullscreenBar(true);
        SetWindowLongPtrW(_hwnd, GWL_STYLE, _savedStyle);
        SetWindowLongPtrW(_hwnd, GWL_EXSTYLE, _savedExStyle);
        HWND zOrder = _settings.current().alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
        if (_savedMaximized) {
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            UINT dpi = GetDpiForWindow(_hwnd);
            int border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            SetWindowPos(_hwnd, zOrder,
                         mi.rcWork.left - border, mi.rcWork.top - border,
                         (mi.rcWork.right - mi.rcWork.left) + border * 2,
                         (mi.rcWork.bottom - mi.rcWork.top) + border * 2,
                         SWP_FRAMECHANGED | SWP_NOACTIVATE);
        } else {
            SetWindowPlacement(_hwnd, &_savedPlacement);
            SetWindowPos(_hwnd, zOrder, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
        }
        layoutChildren();
    }
    pushControlBarState();
}

void MainWindow::ensureAnimTimer() {
    if (!_animTimerActive && _hwnd) {
        SetTimer(_hwnd, TimerAnimId, 33, nullptr);
        _animTimerActive = true;
    }
}

void MainWindow::showFullscreenBar() {
    if ((!_isFullscreen && !_isMiniPlayer) || !_fullscreenBar) return;
    _fsBarVisible = true;
    _fsBarTargetAlpha = 242.0f;
    _fsLastMouseTick = GetTickCount64();
    if (_cursorHidden) { ShowCursor(TRUE); _cursorHidden = false; }
    _fullscreenBar->setOpacity(static_cast<BYTE>(std::clamp(_fsBarAlpha, 1.0f, 242.0f)));
    syncOverlays();
    ensureAnimTimer();
    SetTimer(_hwnd, TimerFsHideId, 3000, nullptr);
}

void MainWindow::hideFullscreenBar(bool immediate) {
    KillTimer(_hwnd, TimerFsHideId);
    _fsBarVisible = false;
    _fsBarTargetAlpha = 0.0f;
    if (immediate) {
        _fsBarAlpha = 0.0f;
        if (_fullscreenBar) {
            _fullscreenBar->hidePopup();
            ShowWindow(_fullscreenBar->hwnd(), SW_HIDE);
        }
    } else {
        ensureAnimTimer();
    }
    if (_isFullscreen && !_cursorHidden) {
        ShowCursor(FALSE);
        _cursorHidden = true;
    }
}

void MainWindow::tickFsBarFade() {
    if (!_fullscreenBar) return;
    if (std::abs(_fsBarAlpha - _fsBarTargetAlpha) < 0.5f) return;
    // ~140 ms fade-in, ~180 ms fade-out at 33 ms tick
    float step = (_fsBarTargetAlpha > _fsBarAlpha) ? 56.0f : 44.0f;
    if (_fsBarTargetAlpha > _fsBarAlpha)
        _fsBarAlpha = std::min(_fsBarTargetAlpha, _fsBarAlpha + step);
    else
        _fsBarAlpha = std::max(_fsBarTargetAlpha, _fsBarAlpha - step);

    if (_fsBarAlpha <= 1.0f && _fsBarTargetAlpha <= 0.0f) {
        _fsBarAlpha = 0.0f;
        ShowWindow(_fullscreenBar->hwnd(), SW_HIDE);
    } else {
        _fullscreenBar->setOpacity(static_cast<BYTE>(_fsBarAlpha));
    }
}

void MainWindow::fitWindowToVideo(bool showFeedback) {
    if (!_hwnd || _isFullscreen || _isMiniPlayer) return;
    if (IsZoomed(_hwnd)) {
        if (!showFeedback) return;
        ShowWindow(_hwnd, SW_RESTORE);
    }
    unsigned vw = 0, vh = 0;
    if (!_engine.videoSize(vw, vh) || vw == 0 || vh == 0) {
        if (showFeedback) showOsd(L"No active video to fit");
        return;
    }

    UINT dpi = GetDpiForWindow(_hwnd);
    float scale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    int menuH = _isMinimal ? 0 : static_cast<int>(std::lround(D2DMenuBar::BarHeight * scale));
    int barH = static_cast<int>(std::lround(D2DControlBar::BarHeight * scale));

    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int workW = mi.rcWork.right - mi.rcWork.left;
    int workH = mi.rcWork.bottom - mi.rcWork.top;

    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(_hwnd, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(_hwnd, GWL_EXSTYLE));
    RECT borderRc{ 0, 0, 0, 0 };
    AdjustWindowRectExForDpi(&borderRc, style, FALSE, exStyle, dpi);
    int frameW = borderRc.right - borderRc.left;
    int frameH = borderRc.bottom - borderRc.top;

    int maxVidW = std::max(320, static_cast<int>(workW * 0.85) - frameW);
    int maxVidH = std::max(240, static_cast<int>(workH * 0.85) - frameH - menuH - barH);
    double fit = std::min({ 1.0, static_cast<double>(maxVidW) / vw, static_cast<double>(maxVidH) / vh });
    // Ensure a comfortable minimum size so the control bar remains uncrowded
    double minW = 560.0 * scale;
    double minH = 280.0 * scale;
    double targetVidW = vw * fit;
    double targetVidH = vh * fit;
    if (targetVidW < minW && targetVidH < minH) {
        double up = std::min(static_cast<double>(maxVidW) / vw, static_cast<double>(maxVidH) / vh);
        double need = std::min(up, std::max(minW / vw, minH / vh));
        targetVidW = vw * need;
        targetVidH = vh * need;
    }

    int winW = std::clamp(static_cast<int>(std::lround(targetVidW)) + frameW, static_cast<int>(std::lround(minW)) + frameW, workW);
    int winH = std::clamp(static_cast<int>(std::lround(targetVidH)) + menuH + barH + frameH, static_cast<int>(std::lround(minH)) + menuH + barH + frameH, workH);

    RECT cur{};
    GetWindowRect(_hwnd, &cur);
    int cx = (cur.left + cur.right) / 2;
    int cy = (cur.top + cur.bottom) / 2;
    int nx = std::clamp<int>(cx - winW / 2, mi.rcWork.left, mi.rcWork.right - winW);
    int ny = std::clamp<int>(cy - winH / 2, mi.rcWork.top, mi.rcWork.bottom - winH);
    SetWindowPos(_hwnd, nullptr, nx, ny, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
    if (showFeedback) {
        showOsd(L"Window fitted to " + std::to_wstring(vw) + L"×" + std::to_wstring(vh));
    }
}

void MainWindow::stepSubtitleSize(int delta) {
    static constexpr const wchar_t* kSizes[] = { L"Smaller", L"Small", L"Normal", L"Large", L"Larger" };
    auto& s = _settings.current();
    int idx = 2;
    for (int i = 0; i < 5; ++i) if (s.subtitleSize == kSizes[i]) { idx = i; break; }
    int next = std::clamp(idx + delta, 0, 4);
    if (next == idx) { showOsd(L"Subtitle size: " + s.subtitleSize); return; }
    s.subtitleSize = kSizes[next];
    _engine.setBaseOptions(PlayerService::buildOptions(s));
    saveState();
    if (_engine.isActive()) applyVideoFilters(L"Subtitle size: " + s.subtitleSize);
    else showOsd(L"Subtitle size: " + s.subtitleSize);
}

void MainWindow::openSubtitleAppearance() {
    auto& s = _settings.current();
    if (!Dialogs::showSubtitleAppearance(_hwnd, s)) return;
    _engine.setBaseOptions(PlayerService::buildOptions(s));
    saveState();
    if (_engine.isActive()) applyVideoFilters(L"Subtitle appearance updated");
    else showOsd(L"Subtitle appearance saved");
}

void MainWindow::setFileBrowser(bool on) {
    if (on && _isFullscreen) setFullscreen(false);
    if (on && _isMiniPlayer) setMiniPlayer(false);
    if (on && _isMinimal) _isMinimal = false;
    _showFileBrowser = on;
    _settings.current().showFileBrowser = on;
    if (on && _fileBrowser) {
        std::wstring dir = _settings.current().fileBrowserDir;
        std::error_code ec;
        if (dir.empty() || !fs::exists(fs::path(dir), ec)) {
            if (auto cur = _playlist.current(); cur && cur->isFile()) {
                fs::path p(cur->location);
                if (p.has_parent_path() && fs::exists(p.parent_path(), ec)) dir = p.parent_path().wstring();
            }
        }
        if (dir.empty() || !fs::exists(fs::path(dir), ec)) dir = _settings.current().lastFolder;
        if (dir.empty() || !fs::exists(fs::path(dir), ec)) dir = SettingsService::defaultVideosFolder();
        if (auto cur = _playlist.current(); cur && cur->isFile()) _fileBrowser->setActiveFile(cur->location);
        _fileBrowser->openDirectory(dir);
    }
    layoutChildren();
    saveState();
}

void MainWindow::setMinimal(bool on) {
    _isMinimal = on;
    if (!_isFullscreen) layoutChildren();
    showOsd(on ? L"Minimal interface (Ctrl+H to restore)" : L"Normal interface");
}

void MainWindow::applyTheme() {
    const auto& t = _settings.current().theme;
    _darkTheme = (t == L"Light") ? false : (t == L"System" ? !WinInterop::systemUsesLightTheme() : true);
    WinInterop::setAppDarkMode(_darkTheme);
    WinInterop::setDarkTitleBar(_hwnd, _darkTheme);
    if (_menuBar) _menuBar->setDarkTheme(_darkTheme);
    if (_fileBrowser) _fileBrowser->setDarkTheme(_darkTheme);
    pushControlBarState();
}

void MainWindow::updateWindowTitle() {
    std::wstring wt = _isIdle ? L"Reel Player" : (_title + L" - Reel Player");
    SetWindowTextW(_hwnd, wt.c_str());
}

void MainWindow::pushControlBarState() {
    ControlBarState st;
    st.darkTheme = _darkTheme;
    st.isPlaying = _isPlaying;
    st.isSeekable = _isSeekable;
    st.hasVideo = _hasVideo;
    st.positionMs = _posMs;
    st.lengthMs = _lenMs;
    st.showRemaining = _settings.current().showRemainingTime;
    st.volume = _volume;
    st.isMuted = _muted;
    st.rate = _rate;
    st.loop = _playlist.loop();
    st.isFullscreen = _isFullscreen;
    st.isMiniPlayer = _isMiniPlayer;
    if (auto cur = _playlist.current(); cur && cur->isFile()) st.mediaPath = cur->location;
    st.chapters = _chapters;
    st.abStartMs = _abStartMs;
    st.abEndMs = _abEndMs;
    st.hasResumeOffer = _resumeOffer.has_value();
    if (_resumeOffer) st.resumeOfferText = L"Continue " + TimeFormat::format(_resumeOffer->second);

    if (_dockedBar) _dockedBar->setState(st);
    if (_fullscreenBar) {
        st.isFullscreenBar = true;
        _fullscreenBar->setState(st);
    }
}

void MainWindow::showOsd(const std::wstring& text) { if (_overlay) { _overlay->showOsd(text); ensureAnimTimer(); } }
void MainWindow::showError(const std::wstring& text) { if (_overlay) { _overlay->showError(text); ensureAnimTimer(); } }

// ============================================================================
// Engine callbacks (UI thread)
// ============================================================================

void MainWindow::drainVlcEvents() {
    std::deque<VlcEventMsg> batch;
    {
        std::lock_guard lk(_evMtx);
        batch.swap(_evQueue);
    }
    for (const auto& ev : batch) {
        switch (ev.kind) {
            case VlcEventKind::Opening:
                _isIdle = false;
                _statusText = L"Opening...";
                break;
            case VlcEventKind::StateChanged:
                onStateChanged();
                break;
            case VlcEventKind::EndReached:
                onEndReached();
                break;
            case VlcEventKind::Error:
                onError();
                break;
            case VlcEventKind::Buffering:
                if (ev.floatArg < 99.5f) _statusText = L"Buffering...";
                break;
            case VlcEventKind::LengthChanged:
                _lenMs = ev.intArg;
                if (auto cur = _playlist.current(); cur && _lenMs > 0) cur->durationMs = _lenMs;
                pushControlBarState();
                break;
            case VlcEventKind::TracksChanged:
                refreshTracks();
                break;
            case VlcEventKind::VoutChanged:
                _hasVideo = ev.intArg > 0;
                WinInterop::keepAwake(_isPlaying && _hasVideo);
                if (ev.intArg > 0) {
                    WinInterop::blackenVlcWindows(_videoHost);
                    if (!_fittedForCurrentMedia && _settings.current().autoFitWindow &&
                        !_isFullscreen && !_isMiniPlayer && !IsZoomed(_hwnd)) {
                        _fittedForCurrentMedia = true;
                        fitWindowToVideo(false);
                    }
                }
                pushControlBarState();
                break;
            case VlcEventKind::SnapshotTaken:
                showOsd(L"Snapshot saved: " + fs::path(ev.strArg).filename().wstring());
                break;
        }
    }
}

void MainWindow::onStateChanged() {
    auto st = _engine.state();
    _isPlaying = (st == libvlc_Playing);
    _isPaused = (st == libvlc_Paused);
    _isSeekable = _engine.isActive() && _engine.isSeekable();
    if (st == libvlc_Stopped || st == libvlc_Ended || st == libvlc_NothingSpecial) {
        _isIdle = true;
        _hasVideo = false;
        if (_idlePanel) ShowWindow(_idlePanel->hwnd(), SW_SHOW);
    }
    if (st == libvlc_Playing && !_appliedForCurrentMedia) {
        _appliedForCurrentMedia = true;
        _consecutiveErrors = 0;
        onMediaStarted();
    }
    WinInterop::keepAwake(_isPlaying && _hasVideo);
    _smtc.updateStatus(_isPlaying, _isPaused);
    _taskbar.updatePlayState(_isPlaying);
    _taskbar.updateProgress(_engine.isActive(), _isPaused, _posMs, _lenMs);
    updateWindowTitle();
    pushControlBarState();
}

void MainWindow::onMediaStarted() {
    _engine.setVolume(_volume);
    _engine.setMute(_muted);
    if (std::abs(_rate - 1.0f) > 0.001f) _engine.setRate(_rate);
    if (!_settings.current().audioDeviceId.empty()) _engine.setAudioDevice(toUtf8(_settings.current().audioDeviceId));
    if (_effects.adjustEnabled)
        _engine.setAdjust(true, _effects.brightness, _effects.contrast, _effects.hue, _effects.saturation, _effects.gamma);
    if (_effects.eqEnabled)
        _engine.applyEqualizer(true, _effects.eqPreamp, _effects.eqBands);
    if (_deintIdx > 0) _engine.setDeinterlace(kDeints[_deintIdx].mode);
    if (_aspectIdx > 0) _engine.setAspectRatio(kAspects[_aspectIdx]);
    if (_cropIdx > 0) _engine.setCropGeometry(kCrops[_cropIdx]);
    if (_zoomIdx > 0) _engine.setScale(kZooms[_zoomIdx].scale);

    auto item = _playlist.current();
    auto metaTitle = _engine.meta(libvlc_meta_NowPlaying);
    if (metaTitle.empty()) metaTitle = _engine.meta(libvlc_meta_Title);
    auto artist = _engine.meta(libvlc_meta_Artist);
    if (item && !trim(metaTitle).empty()) {
        std::wstring stem = fs::path(item->location).stem().wstring();
        std::wstring fname = fs::path(item->location).filename().wstring();
        if ((!item->isFile() || !equalsIgnoreCase(stem, metaTitle)) && !equalsIgnoreCase(fname, metaTitle))
            item->title = metaTitle;
    }
    _title = item ? item->title : _title;
    _smtc.updateMetadata(_title, artist, _engine.videoTrackCount() > 0 || _hasVideo, {});

    if (_restart) {
        auto r = *_restart;
        _restart.reset();
        if (r.audioDelayUs != 0) _engine.setAudioDelayUs(r.audioDelayUs);
        if (r.spuDelayUs != 0) _engine.setSpuDelayUs(r.spuDelayUs);
        if (r.wasPaused) _engine.pause();
        if (!r.osd.empty()) showOsd(r.osd);
    } else {
        showOsd(_title);
    }
    refreshTracks();
    updateWindowTitle();
}

void MainWindow::onEndReached() {
    if (auto cur = _playlist.current(); cur && cur->isFile()) _resume.forget(cur->location);
    if (auto next = _playlist.peekNext(false)) {
        playItem(next);
    } else if (_playlist.items().size() == 1 && _playlist.loop() == LoopMode::Off && tryPlaySibling(+1)) {
        return;
    } else {
        _engine.stopKeepItem();
        _isIdle = true;
        _posMs = 0;
        if (_idlePanel) ShowWindow(_idlePanel->hwnd(), SW_SHOW);
        _smtc.updateStatus(false, false);
        pushControlBarState();
    }
}

void MainWindow::onError() {
    auto item = _playlist.current();
    std::wstring msg = L"Can't play \"" + (item ? item->title : std::wstring(L"media")) + L"\".";
    AppLog::write(msg);
    showError(msg);
    _consecutiveErrors++;
    _isIdle = true;
    if (_consecutiveErrors < static_cast<int>(_playlist.items().size())) {
        if (auto next = _playlist.peekNext(true); next && next != item) { playItem(next); return; }
    }
    _consecutiveErrors = 0;
}

void MainWindow::refreshTracks() {
    if (!_engine.isActive()) return;
    _hasVideo = _hasVideo || _engine.videoTrackCount() > 0;
    _chapters = _engine.chapters();
    pushControlBarState();
}

void MainWindow::onTick250ms() {
    if (_resumeOffer && GetTickCount64() - _resumeOfferTick >= 12000) dismissResume();
    if (_rebuilding || !_engine.isActive()) return;
    int64_t len = _engine.lengthMs();
    int64_t time = _engine.timeMs();
    if (len > 0 && len != _lenMs) _lenMs = len;
    _isSeekable = _engine.isSeekable();
    if (!_isUserSeeking && time >= 0 && (GetTickCount64() - _lastSeekTick) >= 350) {
        _posMs = time;
        if (_abStartMs && _abEndMs && time >= *_abEndMs) {
            seek(*_abStartMs);
            return;
        }
    }
    pushControlBarState();
    _taskbar.updateProgress(true, _isPaused, _posMs, _lenMs);
    if (_isPlaying && ++_timelineTick % 4 == 0) _smtc.updateTimeline(time, len);
}

void MainWindow::saveResumePosition() {
    if (!_settings.current().resumePlayback) return;
    if (auto item = _engine.currentItem(); item && item->isFile() && _engine.isActive())
        _resume.update(item->location, _engine.timeMs(), _engine.lengthMs());
}

void MainWindow::saveState() {
    saveResumePosition();
    _resume.save();
    auto& s = _settings.current();
    s.lastPlaylist.clear();
    for (const auto& i : _playlist.items()) {
        if (!startsWithIgnoreCase(i->location, L"dshow:")) s.lastPlaylist.push_back(i->location);
    }
    s.lastPlaylistIndex = _playlist.currentIndex();
    _settings.save();
}

void MainWindow::handleCommandLine(const CommandLine& cmd, bool fromOtherInstance) {
    if (!cmd.locations.empty()) {
        bool play = !fromOtherInstance || !_settings.current().enqueueWhenRunning;
        openLocations(cmd.locations, play, cmd.startTimeMs);
    }
    if ((cmd.fullscreen || (!fromOtherInstance && _settings.current().startFullscreen)) && !_isFullscreen)
        setFullscreen(true);
    if (fromOtherInstance) {
        if (IsIconic(_hwnd)) ShowWindow(_hwnd, SW_RESTORE);
        SetForegroundWindow(_hwnd);
    }
}

// ============================================================================
// Window Procedures
// ============================================================================

LRESULT CALLBACK MainWindow::mainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->_hwnd = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handleMainMsg(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::videoHostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->_videoHost = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handleVideoMsg(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::handleVideoMsg(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            SetFocus(_hwnd);
            if (_isMiniPlayer) {
                ReleaseCapture();
                SendMessageW(_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            if (_isFullscreen && _fsBarVisible) {
                GetCursorPos(&_lastMousePt);
                hideFullscreenBar();
            }
            if (_settings.current().clickToPause && !_isIdle && _engine.isActive()) {
                UINT delay = std::min<UINT>(220, std::max<UINT>(120, GetDoubleClickTime() / 2));
                SetTimer(_hwnd, TimerClickPauseId, delay, nullptr);
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            KillTimer(_hwnd, TimerClickPauseId);
            if (_isMiniPlayer) setMiniPlayer(false);
            else runAction("Fullscreen");
            return 0;
        case WM_MBUTTONDOWN:
            togglePlayPause();
            return 0;
        case WM_RBUTTONUP: {
            POINT pt{};
            GetCursorPos(&pt);
            showContextMenu(pt.x, pt.y);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (_isFullscreen || _isMiniPlayer) {
                POINT pt{};
                GetCursorPos(&pt);
                if (std::abs(pt.x - _lastMousePt.x) >= 2 || std::abs(pt.y - _lastMousePt.y) >= 2) {
                    _lastMousePt = pt;
                    showFullscreenBar();
                }
            }
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            bool up = delta > 0;
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl) seekRelative(up ? 5000 : -5000);
            else if (shift) runAction(up ? "Faster" : "Slower");
            else changeVolume(up ? 5 : -5);
            return 0;
        }
    }
    return DefWindowProcW(_videoHost, msg, wp, lp);
}

LRESULT MainWindow::handleMainMsg(UINT msg, WPARAM wp, LPARAM lp) {
    if (_taskbarCreatedMsg && msg == _taskbarCreatedMsg) {
        _taskbar.attach(_hwnd);
        return 0;
    }
    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED && _settings.current().pauseOnMinimize) _engine.pause();
            layoutChildren();
            return 0;
        case WM_MOVE:
            syncOverlays();
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lp);
            if (suggested && !_isFullscreen) {
                SetWindowPos(_hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            layoutChildren();
            return 0;
        }
        case WM_TIMER:
            if (wp == TimerTickId) onTick250ms();
            else if (wp == TimerAnimId) {
                if (_overlay) _overlay->tickAnimation();
                tickFsBarFade();
                bool barAnim = false;
                if (_dockedBar) barAnim |= _dockedBar->tickAnimation();
                if (_fullscreenBar) barAnim |= _fullscreenBar->tickAnimation();
                bool overlayAnim = _overlay && _overlay->isAnimating();
                bool fsFadeAnim = std::abs(_fsBarAlpha - _fsBarTargetAlpha) >= 0.5f;
                if (!barAnim && !overlayAnim && !fsFadeAnim) {
                    KillTimer(_hwnd, TimerAnimId);
                    _animTimerActive = false;
                }
            } else if (wp == TimerClickPauseId) {
                KillTimer(_hwnd, TimerClickPauseId);
                if (!_isIdle && _engine.isActive()) togglePlayPause();
            } else if (wp == TimerFsHideId) {
                if ((_isFullscreen || _isMiniPlayer) && _fullscreenBar) {
                    if (_isUserSeeking) { SetTimer(_hwnd, TimerFsHideId, 1000, nullptr); return 0; }
                    POINT pt{}; GetCursorPos(&pt);
                    RECT br{}; GetWindowRect(_fullscreenBar->hwnd(), &br);
                    if (!PtInRect(&br, pt)) hideFullscreenBar();
                    else SetTimer(_hwnd, TimerFsHideId, 1000, nullptr);
                }
            }
            return 0;
        case WM_APP_OSD_MSG: {
            auto* msg = reinterpret_cast<std::wstring*>(lp);
            if (msg) { showOsd(*msg); delete msg; }
            return 0;
        }
        case WM_APP_VLC_EVENT:
            drainVlcEvents();
            return 0;
        case WM_APP_FORWARDED_ARGS: {
            std::deque<std::vector<std::wstring>> q;
            { std::lock_guard lk(_fwdMtx); q.swap(_fwdQueue); }
            for (const auto& args : q) handleCommandLine(CommandLine::parse(args), true);
            return 0;
        }
        case WM_APP_SMTC_BUTTON: {
            const char* btn = reinterpret_cast<const char*>(lp);
            if (btn) {
                if (strcmp(btn, "Play") == 0) { if (_engine.state() == libvlc_Paused) _engine.resume(); else if (!_isPlaying) togglePlayPause(); }
                else if (strcmp(btn, "Pause") == 0) { if (_isPlaying) _engine.pause(); }
                else if (strcmp(btn, "Stop") == 0) stopPlayback();
                else if (strcmp(btn, "Next") == 0) nextItem();
                else if (strcmp(btn, "Previous") == 0) previousItem();
            }
            return 0;
        }
        case WM_APP_REBUILD_DONE: {
            _rebuilding = false;
            _engine.setHwnd(_videoHost);
            _engine.releaseRetired();
            if (wp && _rebuildItem && _rebuildGen == _playGeneration) {
                _appliedForCurrentMedia = false;
                _restart = _rebuildRestart;
                _engine.open(_rebuildItem, _rebuildPosMs, buildExtraOptions());
            } else if (!wp) {
                showError(L"Could not apply video filter.");
            }
            _rebuildItem.reset();
            return 0;
        }
        case WM_INITMENUPOPUP: {
            HMENU hm = reinterpret_cast<HMENU>(wp);
            if (hm == _dynTitle || hm == _dynChapter || hm == _dynSpeed ||
                hm == _dynAudioTrack || hm == _dynAudioDevice || hm == _dynStereo ||
                hm == _dynVideoTrack || hm == _dynZoom || hm == _dynAspect ||
                hm == _dynCrop || hm == _dynDeint || hm == _dynSubTrack) {
                populateDynamicSubmenu(hm);
            } else {
                refreshMenuStates();
            }
            return 0;
        }
        case WM_COMMAND: {
            UINT id = LOWORD(wp);
            // Taskbar thumb buttons
            if (HIWORD(wp) == THBN_CLICKED) {
                if (id == TaskbarController::ThumbPrevId) previousItem();
                else if (id == TaskbarController::ThumbPlayId) togglePlayPause();
                else if (id == TaskbarController::ThumbNextId) nextItem();
                return 0;
            }
            // Dynamic menu ranges
            if (id >= ID_DYN_TITLE_BASE && id < ID_DYN_CHAPTER_BASE) { _engine.setTitle(static_cast<int>(id - ID_DYN_TITLE_BASE) - 1); return 0; }
            if (id >= ID_DYN_CHAPTER_BASE && id < ID_DYN_SPEED_BASE) { _engine.setChapter(static_cast<int>(id - ID_DYN_CHAPTER_BASE)); return 0; }
            if (id >= ID_DYN_SPEED_BASE && id < ID_DYN_AUDIOTRACK_BASE) {
                size_t idx = id - ID_DYN_SPEED_BASE;
                if (idx < std::size(kRates)) setRate(kRates[idx]);
                return 0;
            }
            if (id >= ID_DYN_AUDIOTRACK_BASE && id < ID_DYN_AUDIODEV_BASE) { _engine.setAudioTrack(static_cast<int>(id - ID_DYN_AUDIOTRACK_BASE) - 1); return 0; }
            if (id >= ID_DYN_AUDIODEV_BASE && id < ID_DYN_STEREO_BASE) {
                size_t idx = id - ID_DYN_AUDIODEV_BASE;
                if (idx < _dynAudioDeviceIds.size()) {
                    _settings.current().audioDeviceId = toWide(_dynAudioDeviceIds[idx]);
                    _engine.setAudioDevice(_dynAudioDeviceIds[idx]);
                }
                return 0;
            }
            if (id >= ID_DYN_STEREO_BASE && id < ID_DYN_VIDEOTRACK_BASE) {
                size_t idx = id - ID_DYN_STEREO_BASE;
                if (idx < std::size(kStereos)) { _engine.setAudioChannel(kStereos[idx].ch); showOsd(L"Stereo mode: " + std::wstring(kStereos[idx].name)); }
                return 0;
            }
            if (id >= ID_DYN_VIDEOTRACK_BASE && id < ID_DYN_ZOOM_BASE) { _engine.setVideoTrack(static_cast<int>(id - ID_DYN_VIDEOTRACK_BASE) - 1); return 0; }
            if (id >= ID_DYN_ZOOM_BASE && id < ID_DYN_ASPECT_BASE) { setZoom(static_cast<int>(id - ID_DYN_ZOOM_BASE)); return 0; }
            if (id >= ID_DYN_ASPECT_BASE && id < ID_DYN_CROP_BASE) { setAspect(static_cast<int>(id - ID_DYN_ASPECT_BASE)); return 0; }
            if (id >= ID_DYN_CROP_BASE && id < ID_DYN_DEINT_BASE) { setCrop(static_cast<int>(id - ID_DYN_CROP_BASE)); return 0; }
            if (id >= ID_DYN_DEINT_BASE && id < ID_DYN_SUBTRACK_BASE) { setDeinterlace(static_cast<int>(id - ID_DYN_DEINT_BASE)); return 0; }
            if (id >= ID_DYN_SUBTRACK_BASE && id < ID_DYN_SUBTRACK_BASE + 200) { _engine.setSubtitleTrack(static_cast<int>(id - ID_DYN_SUBTRACK_BASE) - 1); return 0; }

            switch (id) {
                case ID_OPEN_FILE: runAction("OpenFile"); break;
                case ID_OPEN_MULTI: runAction("OpenMultiple"); break;
                case ID_OPEN_FOLDER: runAction("OpenFolder"); break;
                case ID_OPEN_DISC: runAction("OpenDisc"); break;
                case ID_OPEN_NETWORK: runAction("OpenNetwork"); break;
                case ID_OPEN_CAPTURE: runAction("OpenCapture"); break;
                case ID_SAVE_PLAYLIST: runAction("SavePlaylist"); break;
                case ID_QUIT: runAction("Quit"); break;
                case ID_SEEK_FWD10: runAction("SeekFwd10"); break;
                case ID_SEEK_BACK10: runAction("SeekBack10"); break;
                case ID_JUMP_TIME: runAction("JumpToTime"); break;
                case ID_RECORD: runAction("Record"); break;
                case ID_AB_LOOP: runAction("AbLoop"); break;
                case ID_EXPORT_AB: runAction("ExportAbClip"); break;
                case ID_PLAY_PAUSE: runAction("PlayPause"); break;
                case ID_STOP: runAction("Stop"); break;
                case ID_PREV: runAction("Previous"); break;
                case ID_NEXT: runAction("Next"); break;
                case ID_NEXT_FRAME: runAction("NextFrame"); break;
                case ID_PREV_FRAME: runAction("PrevFrame"); break;
                case ID_SPEED_FASTER: runAction("Faster"); break;
                case ID_SPEED_NORMAL: runAction("NormalSpeed"); break;
                case ID_SPEED_SLOWER: runAction("Slower"); break;
                case ID_VOL_UP: runAction("VolumeUp"); break;
                case ID_VOL_DOWN: runAction("VolumeDown"); break;
                case ID_MUTE: runAction("Mute"); break;
                case ID_FULLSCREEN: runAction("Fullscreen"); break;
                case ID_MINI_PLAYER: runAction("MiniPlayer"); break;
                case ID_FIT_WINDOW: runAction("FitWindow"); break;
                case ID_FILE_BROWSER: runAction("FileBrowser"); break;
                case ID_ALWAYS_ON_TOP: runAction("AlwaysOnTop"); break;
                case ID_ROTATE: runAction("RotateVideo"); break;
                case ID_SNAPSHOT: runAction("Snapshot"); break;
                case ID_ADD_SUB: runAction("AddSubtitle"); break;
                case ID_SUB_DELAY_DOWN: runAction("SubDelayDown"); break;
                case ID_SUB_DELAY_UP: runAction("SubDelayUp"); break;
                case ID_SUB_SIZE_UP: runAction("SubSizeUp"); break;
                case ID_SUB_SIZE_DOWN: runAction("SubSizeDown"); break;
                case ID_SUB_APPEARANCE: runAction("SubAppearance"); break;
                case ID_EFFECTS: runAction("Effects"); break;
                case ID_SYNC: runAction("Sync"); break;
                case ID_MEDIA_INFO: runAction("MediaInfo"); break;
                case ID_CODEC_INFO: runAction("CodecInfo"); break;
                case ID_MESSAGES: runAction("Messages"); break;
                case ID_PREFERENCES: runAction("Preferences"); break;
                case ID_TOGGLE_PLAYLIST: runAction("TogglePlaylist"); break;
                case ID_MINIMAL: runAction("MinimalInterface"); break;
                case ID_USER_GUIDE: runAction("UserGuide"); break;
                case ID_SHORTCUTS: runAction("Shortcuts"); break;
                case ID_ABOUT: runAction("About"); break;
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            bool ctrl = ((GetKeyState(VK_CONTROL) | GetKeyState(VK_LCONTROL) | GetKeyState(VK_RCONTROL)) & 0x8000) != 0;
            bool alt = ((GetKeyState(VK_MENU) | GetKeyState(VK_LMENU) | GetKeyState(VK_RMENU)) & 0x8000) != 0;
            bool shift = ((GetKeyState(VK_SHIFT) | GetKeyState(VK_LSHIFT) | GetKeyState(VK_RSHIFT)) & 0x8000) != 0;
            bool win = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
            if (auto g = HotkeyMap::gestureFromVk(static_cast<UINT>(wp), ctrl, alt, shift, win)) {
                if (auto act = _hotkeys.resolve(*g)) {
                    runAction(*act);
                    return 0;
                }
            }
            break;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> files;
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring p(len, L'\0');
                DragQueryFileW(drop, i, p.data(), len + 1);
                files.push_back(std::move(p));
            }
            DragFinish(drop);
            if (!files.empty()) openLocations(files, true);
            SetForegroundWindow(_hwnd);
            return 0;
        }
        case WM_CLOSE: {
            auto& s = _settings.current();
            WINDOWPLACEMENT wpSave{ sizeof(wpSave) };
            if (GetWindowPlacement(_hwnd, &wpSave)) {
                bool transient = _isFullscreen || _isMiniPlayer;
                const RECT& r = transient ? _savedPlacement.rcNormalPosition : wpSave.rcNormalPosition;
                s.windowLeft = r.left;
                s.windowTop = r.top;
                s.windowWidth = r.right - r.left;
                s.windowHeight = r.bottom - r.top;
                s.windowMaximized = transient ? _savedMaximized : (IsZoomed(_hwnd) != 0);
            }
            saveState();
            KillTimer(_hwnd, TimerTickId);
            KillTimer(_hwnd, TimerAnimId);
            KillTimer(_hwnd, TimerFsHideId);
            DestroyWindow(_hwnd);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(_hwnd, msg, wp, lp);
}

} // namespace reel
