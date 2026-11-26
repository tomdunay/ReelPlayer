#pragma once
#include "core/Playlist.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace reel {

struct AppSettings {
    // Interface
    std::wstring theme = L"Dark"; // Dark | Light | System
    bool oneInstance = true;
    bool enqueueWhenRunning = false;
    bool resumePlayback = true;
    bool showStatusBar = true;
    bool showPlaylist = false;
    bool alwaysOnTop = false;
    bool showRemainingTime = false;
    bool pauseOnMinimize = false;
    bool autoAdvanceFolder = true;
    bool startFullscreen = false;
    bool autoFitWindow = true;
    bool clickToPause = true;

    // Engine (applied at startup)
    bool hardwareDecoding = true;
    std::wstring hwDecodeMode = L"Auto"; // Auto | D3D11 | DXVA2 | Off
    int fileCachingMs = 300;
    int networkCachingMs = 1000;
    bool fastSeek = true;
    std::wstring audioDeviceId;

    // Subtitles (applied at startup)
    std::wstring subtitleLanguage;
    std::wstring subtitleEncoding;
    std::wstring subtitleSize = L"Normal";
    std::wstring subtitleColor = L"#FFFFFF";
    std::wstring subtitleOutline = L"Normal";
    bool autoLoadSubtitles = true;

    // Snapshots
    std::wstring snapshotFolder;
    std::wstring snapshotFormat = L"png";

    // Hotkeys overrides: actionId -> "Gesture1|Gesture2"
    std::map<std::string, std::string> hotkeys;

    // State
    int volume = 100;
    bool muted = false;
    LoopMode loop = LoopMode::Off;
    bool shuffle = false;
    std::optional<double> windowLeft;
    std::optional<double> windowTop;
    double windowWidth = 960;
    double windowHeight = 600;
    bool windowMaximized = false;
    double playlistWidth = 320;
    bool showFileBrowser = false;
    double fileBrowserWidth = 280;
    std::wstring fileBrowserDir;
    std::wstring lastFolder;
    std::vector<std::wstring> lastPlaylist;
    int lastPlaylistIndex = -1;

    AppSettings();
};

class SettingsService {
public:
    explicit SettingsService(const std::wstring& directory = {});

    static std::wstring appDataDirectory();
    static std::wstring defaultPicturesFolder();
    static std::wstring defaultVideosFolder();

    const std::wstring& directory() const { return _dir; }
    const std::wstring& filePath() const { return _filePath; }
    AppSettings& current() { return _current; }
    const AppSettings& current() const { return _current; }

    AppSettings& load();
    bool save() const;

    static int subtitleSizeToVlc(const std::wstring& size);
    static int subtitleOutlineToVlc(const std::wstring& outline);

private:
    std::wstring _dir;
    std::wstring _filePath;
    AppSettings _current;
};

} // namespace reel
