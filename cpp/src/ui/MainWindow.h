#pragma once
#include "core/CommandLine.h"
#include "core/Hotkeys.h"
#include "core/PlayerService.h"
#include "core/Playlist.h"
#include "core/ResumeStore.h"
#include "core/Settings.h"
#include "ui/D2DControls.h"
#include "ui/Dialogs.h"
#include "win/MediaControls.h"
#include "win/WinInterop.h"
#include <windows.h>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace reel {

class MainWindow {
public:
    static constexpr UINT WM_APP_VLC_EVENT = WM_APP + 1;
    static constexpr UINT WM_APP_FORWARDED_ARGS = WM_APP + 2;
    static constexpr UINT WM_APP_SMTC_BUTTON = WM_APP + 3;
    static constexpr UINT WM_APP_REBUILD_DONE = WM_APP + 4;
    static constexpr UINT WM_APP_OSD_MSG = WM_APP + 5;

    MainWindow(HINSTANCE hInst, D2DContext& d2d, SettingsService& settings, PlayerService& engine, ResumeStore& resume);
    ~MainWindow();

    bool create();
    HWND hwnd() const { return _hwnd; }
    void showAndHandleCommandLine(const CommandLine& cmd);
    void postForwardedArgs(std::vector<std::wstring> args);

private:
    enum class VlcEventKind {
        Opening, StateChanged, EndReached, Error, Buffering, LengthChanged, TracksChanged, VoutChanged, SnapshotTaken
    };
    struct VlcEventMsg {
        VlcEventKind kind;
        int64_t intArg = 0;
        float floatArg = 0.0f;
        std::wstring strArg;
    };

    static LRESULT CALLBACK mainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK videoHostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMainMsg(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleVideoMsg(UINT msg, WPARAM wp, LPARAM lp);

    void buildMenus();
    void populateDynamicSubmenu(HMENU menu);
    void refreshMenuStates();
    void showContextMenu(int screenX, int screenY);
    void layoutChildren();
    void syncOverlays();

    // Playback & actions
    void runAction(const std::string& action);
    void openLocations(const std::vector<std::wstring>& locations, bool play, int64_t startMs = 0);
    void openItems(const std::vector<MediaItemPtr>& items, bool play, int64_t startMs = 0);
    void playItem(const MediaItemPtr& item, int64_t startMs = 0, bool offerResume = true);
    void restartCurrent(const std::wstring& osd = {});
    void applyVideoFilters(const std::wstring& osd = {});
    std::vector<std::string> buildExtraOptions() const;

    void togglePlayPause();
    void stopPlayback();
    void nextItem();
    void previousItem();
    bool tryPlaySibling(int offset);
    void seek(int64_t ms, bool asyncCoalesced = false);
    void seekRelative(int64_t deltaMs);
    void setVolume(int vol);
    void changeVolume(int delta);
    void toggleMute();
    void setRate(float r);
    void cycleAbLoop();
    void clearAbLoop(bool quiet = false);
    void exportAbClip();
    void previousFrame();
    void acceptResume();
    void dismissResume();
    void takeSnapshot();
    void toggleRecord();
    void rotateVideo();
    void setAspect(int idx);
    void setCrop(int idx);
    void setZoom(int idx);
    void setDeinterlace(int idx);
    void cycleAudioTrack();
    void cycleSubtitleTrack();

    // UI state
    void setFullscreen(bool on);
    void setMiniPlayer(bool on);
    void showFullscreenBar();
    void hideFullscreenBar(bool immediate = false);
    void tickFsBarFade();
    void ensureAnimTimer();
    void setMinimal(bool on);
    void applyTheme();
    void updateWindowTitle();
    void pushControlBarState();
    void showOsd(const std::wstring& text);
    void showError(const std::wstring& text);

    // Engine event handlers (UI thread)
    void postVlcEvent(VlcEventMsg ev);
    void drainVlcEvents();
    void onStateChanged();
    void onMediaStarted();
    void onEndReached();
    void onError();
    void refreshTracks();
    void onTick250ms();
    void saveResumePosition();
    void saveState();
    void handleCommandLine(const CommandLine& cmd, bool fromOtherInstance);

    HINSTANCE _hInst;
    D2DContext& _d2d;
    SettingsService& _settings;
    PlayerService& _engine;
    ResumeStore& _resume;

    Playlist _playlist;
    HotkeyMap _hotkeys;
    MediaControls _smtc;
    TaskbarController _taskbar;
    EffectsState _effects;

    HWND _hwnd = nullptr;
    HWND _videoHost = nullptr;
    HMENU _mainMenu = nullptr;
    HMENU _dynTitle = nullptr, _dynChapter = nullptr, _dynSpeed = nullptr;
    HMENU _dynAudioTrack = nullptr, _dynAudioDevice = nullptr, _dynStereo = nullptr;
    HMENU _dynVideoTrack = nullptr, _dynZoom = nullptr, _dynAspect = nullptr, _dynCrop = nullptr, _dynDeint = nullptr;
    HMENU _dynSubTrack = nullptr;

    std::unique_ptr<D2DMenuBar> _menuBar;
    std::unique_ptr<D2DControlBar> _dockedBar;
    std::unique_ptr<D2DControlBar> _fullscreenBar;
    std::unique_ptr<D2DOverlay> _overlay;
    std::unique_ptr<D2DIdlePanel> _idlePanel;

    // Playback state
    bool _darkTheme = true;
    bool _isIdle = true;
    bool _isPlaying = false;
    bool _isPaused = false;
    bool _hasVideo = false;
    bool _isSeekable = false;
    bool _isUserSeeking = false;
    bool _isRecording = false;
    bool _isFullscreen = false;
    bool _isMiniPlayer = false;
    bool _isMinimal = false;
    bool _fsBarVisible = false;
    bool _animTimerActive = false;
    float _fsBarAlpha = 0.0f;
    float _fsBarTargetAlpha = 0.0f;
    bool _cursorHidden = false;
    bool _appliedForCurrentMedia = false;
    bool _rebuilding = false;
    int _playGeneration = 0;
    int _consecutiveErrors = 0;
    int _timelineTick = 0;

    int _volume = 100;
    bool _muted = false;
    float _rate = 1.0f;
    int64_t _posMs = 0;
    int64_t _lenMs = 0;
    std::wstring _title = L"Reel Player";
    std::wstring _statusText = L"Stopped";
    std::wstring _recordPath;

    int _aspectIdx = 0, _cropIdx = 0, _zoomIdx = 0, _deintIdx = 0;
    std::vector<ChapterMark> _chapters;
    std::optional<int64_t> _abStartMs;
    std::optional<int64_t> _abEndMs;

    std::optional<std::pair<MediaItemPtr, int64_t>> _resumeOffer;
    ULONGLONG _resumeOfferTick = 0;
    ULONGLONG _fsLastMouseTick = 0;
    POINT _lastMousePt{ -1, -1 };

    // Saved window state before fullscreen
    DWORD _savedStyle = 0;
    DWORD _savedExStyle = 0;
    WINDOWPLACEMENT _savedPlacement{ sizeof(WINDOWPLACEMENT) };
    bool _savedMaximized = false;

    // Pending filter rebuild state
    struct RestartInfo {
        bool wasPaused = false;
        std::wstring osd;
        int64_t audioDelayUs = 0;
        int64_t spuDelayUs = 0;
    };
    std::optional<RestartInfo> _restart;
    MediaItemPtr _rebuildItem;
    int64_t _rebuildPosMs = 0;
    int _rebuildGen = 0;
    RestartInfo _rebuildRestart;

    // Dynamic menu command lookup
    std::vector<std::string> _dynAudioDeviceIds;

    // Thread-safe queues
    std::mutex _evMtx;
    std::deque<VlcEventMsg> _evQueue;
    std::mutex _fwdMtx;
    std::deque<std::vector<std::wstring>> _fwdQueue;
    UINT _taskbarCreatedMsg = 0;
};

} // namespace reel
