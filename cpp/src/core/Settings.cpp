#include "core/Settings.h"
#include "core/StrUtil.h"
#include <shlobj.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace reel {

static std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}

std::wstring SettingsService::appDataDirectory() {
    // Portable mode: if portable.txt exists next to ReelPlayer.exe, store config/resume/logs beside the exe.
    wchar_t exeBuf[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) > 0) {
        fs::path exeDir = fs::path(exeBuf).parent_path();
        std::error_code ec;
        if (fs::exists(exeDir / L"portable.txt", ec)) {
            return (exeDir / L"data").wstring();
        }
    }
    auto appData = knownFolder(FOLDERID_RoamingAppData);
    return (fs::path(appData) / L"ReelPlayer").wstring();
}

std::wstring SettingsService::defaultPicturesFolder() {
    return (fs::path(knownFolder(FOLDERID_Pictures)) / L"ReelPlayer").wstring();
}

std::wstring SettingsService::defaultVideosFolder() {
    return (fs::path(knownFolder(FOLDERID_Videos)) / L"ReelPlayer").wstring();
}

AppSettings::AppSettings() : snapshotFolder(SettingsService::defaultPicturesFolder()) {}

SettingsService::SettingsService(const std::wstring& directory)
    : _dir(directory.empty() ? appDataDirectory() : directory),
      _filePath((fs::path(_dir) / L"settings.json").wstring()) {}

static LoopMode parseLoopMode(const json& j) {
    if (j.is_string()) {
        auto s = j.get<std::string>();
        if (s == "All") return LoopMode::All;
        if (s == "One") return LoopMode::One;
    } else if (j.is_number_integer()) {
        int v = j.get<int>();
        if (v == 1) return LoopMode::All;
        if (v == 2) return LoopMode::One;
    }
    return LoopMode::Off;
}

static const char* loopModeToString(LoopMode m) {
    switch (m) {
        case LoopMode::All: return "All";
        case LoopMode::One: return "One";
        default: return "Off";
    }
}

AppSettings& SettingsService::load() {
    std::error_code ec;
    if (!fs::exists(_filePath, ec)) return _current;
    try {
        std::ifstream in(fs::path(_filePath), std::ios::binary);
        json j = json::parse(in);
        AppSettings s;
        auto getBool = [&](const char* k, bool& dst) { if (j.contains(k) && j[k].is_boolean()) dst = j[k].get<bool>(); };
        auto getInt = [&](const char* k, int& dst) { if (j.contains(k) && j[k].is_number_integer()) dst = j[k].get<int>(); };
        auto getDbl = [&](const char* k, double& dst) { if (j.contains(k) && j[k].is_number()) dst = j[k].get<double>(); };
        auto getWstr = [&](const char* k, std::wstring& dst) { if (j.contains(k) && j[k].is_string()) dst = toWide(j[k].get<std::string>()); };

        getWstr("Theme", s.theme);
        getBool("OneInstance", s.oneInstance);
        getBool("EnqueueWhenRunning", s.enqueueWhenRunning);
        getBool("ResumePlayback", s.resumePlayback);
        getBool("ShowStatusBar", s.showStatusBar);
        getBool("ShowPlaylist", s.showPlaylist);
        getBool("AlwaysOnTop", s.alwaysOnTop);
        getBool("ShowRemainingTime", s.showRemainingTime);
        getBool("PauseOnMinimize", s.pauseOnMinimize);
        getBool("AutoAdvanceFolder", s.autoAdvanceFolder);
        getBool("StartFullscreen", s.startFullscreen);

        getBool("HardwareDecoding", s.hardwareDecoding);
        getWstr("HwDecodeMode", s.hwDecodeMode);
        if (!j.contains("HwDecodeMode") && !s.hardwareDecoding) s.hwDecodeMode = L"Off";
        s.hardwareDecoding = (s.hwDecodeMode != L"Off");
        getInt("FileCachingMs", s.fileCachingMs);
        getInt("NetworkCachingMs", s.networkCachingMs);
        getBool("FastSeek", s.fastSeek);
        getWstr("AudioDeviceId", s.audioDeviceId);

        getWstr("SubtitleLanguage", s.subtitleLanguage);
        getWstr("SubtitleEncoding", s.subtitleEncoding);
        getWstr("SubtitleSize", s.subtitleSize);
        getWstr("SubtitleColor", s.subtitleColor);
        getWstr("SubtitleOutline", s.subtitleOutline);
        getBool("AutoLoadSubtitles", s.autoLoadSubtitles);

        getWstr("SnapshotFolder", s.snapshotFolder);
        getWstr("SnapshotFormat", s.snapshotFormat);

        if (j.contains("Hotkeys") && j["Hotkeys"].is_object()) {
            for (auto& [k, v] : j["Hotkeys"].items()) {
                if (v.is_string()) s.hotkeys[k] = v.get<std::string>();
            }
        }

        getInt("Volume", s.volume);
        getBool("Muted", s.muted);
        if (j.contains("Loop")) s.loop = parseLoopMode(j["Loop"]);
        getBool("Shuffle", s.shuffle);
        if (j.contains("WindowLeft") && j["WindowLeft"].is_number()) s.windowLeft = j["WindowLeft"].get<double>();
        if (j.contains("WindowTop") && j["WindowTop"].is_number()) s.windowTop = j["WindowTop"].get<double>();
        getDbl("WindowWidth", s.windowWidth);
        getDbl("WindowHeight", s.windowHeight);
        getBool("WindowMaximized", s.windowMaximized);
        getDbl("PlaylistWidth", s.playlistWidth);
        getWstr("LastFolder", s.lastFolder);
        if (j.contains("LastPlaylist") && j["LastPlaylist"].is_array()) {
            for (const auto& el : j["LastPlaylist"]) {
                if (el.is_string()) s.lastPlaylist.push_back(toWide(el.get<std::string>()));
            }
        }
        getInt("LastPlaylistIndex", s.lastPlaylistIndex);
        _current = std::move(s);
    } catch (...) {
        std::error_code copyEc;
        fs::copy_file(_filePath, _filePath + L".bad", fs::copy_options::overwrite_existing, copyEc);
        _current = AppSettings();
    }
    return _current;
}

bool SettingsService::save() const {
    try {
        std::error_code ec;
        fs::create_directories(_dir, ec);
        const auto& s = _current;
        json j;
        j["Theme"] = toUtf8(s.theme);
        j["OneInstance"] = s.oneInstance;
        j["EnqueueWhenRunning"] = s.enqueueWhenRunning;
        j["ResumePlayback"] = s.resumePlayback;
        j["ShowStatusBar"] = s.showStatusBar;
        j["ShowPlaylist"] = s.showPlaylist;
        j["AlwaysOnTop"] = s.alwaysOnTop;
        j["ShowRemainingTime"] = s.showRemainingTime;
        j["PauseOnMinimize"] = s.pauseOnMinimize;
        j["AutoAdvanceFolder"] = s.autoAdvanceFolder;
        j["StartFullscreen"] = s.startFullscreen;
        j["HardwareDecoding"] = s.hardwareDecoding;
        j["HwDecodeMode"] = toUtf8(s.hwDecodeMode);
        j["FileCachingMs"] = s.fileCachingMs;
        j["NetworkCachingMs"] = s.networkCachingMs;
        j["FastSeek"] = s.fastSeek;
        j["AudioDeviceId"] = s.audioDeviceId.empty() ? json(nullptr) : json(toUtf8(s.audioDeviceId));
        j["SubtitleLanguage"] = toUtf8(s.subtitleLanguage);
        j["SubtitleEncoding"] = toUtf8(s.subtitleEncoding);
        j["SubtitleSize"] = toUtf8(s.subtitleSize);
        j["SubtitleColor"] = toUtf8(s.subtitleColor);
        j["SubtitleOutline"] = toUtf8(s.subtitleOutline);
        j["AutoLoadSubtitles"] = s.autoLoadSubtitles;
        j["SnapshotFolder"] = toUtf8(s.snapshotFolder);
        j["SnapshotFormat"] = toUtf8(s.snapshotFormat);
        j["Hotkeys"] = s.hotkeys;
        j["Volume"] = s.volume;
        j["Muted"] = s.muted;
        j["Loop"] = loopModeToString(s.loop);
        j["Shuffle"] = s.shuffle;
        j["WindowLeft"] = s.windowLeft ? json(*s.windowLeft) : json(nullptr);
        j["WindowTop"] = s.windowTop ? json(*s.windowTop) : json(nullptr);
        j["WindowWidth"] = s.windowWidth;
        j["WindowHeight"] = s.windowHeight;
        j["WindowMaximized"] = s.windowMaximized;
        j["PlaylistWidth"] = s.playlistWidth;
        j["LastFolder"] = s.lastFolder.empty() ? json(nullptr) : json(toUtf8(s.lastFolder));
        json lp = json::array();
        for (const auto& item : s.lastPlaylist) lp.push_back(toUtf8(item));
        j["LastPlaylist"] = lp;
        j["LastPlaylistIndex"] = s.lastPlaylistIndex;

        std::wstring tmp = _filePath + L".tmp";
        {
            std::ofstream out(fs::path(tmp), std::ios::binary | std::ios::trunc);
            if (!out) return false;
            std::string dump = j.dump(2);
            out.write(dump.data(), static_cast<std::streamsize>(dump.size()));
        }
        return MoveFileExW(tmp.c_str(), _filePath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    } catch (...) {
        return false;
    }
}

int SettingsService::subtitleSizeToVlc(const std::wstring& size) {
    if (size == L"Smaller") return 20;
    if (size == L"Small") return 18;
    if (size == L"Large") return 12;
    if (size == L"Larger") return 6;
    return 16;
}

int SettingsService::subtitleOutlineToVlc(const std::wstring& outline) {
    if (outline == L"None") return 0;
    if (outline == L"Thin") return 2;
    if (outline == L"Thick") return 6;
    return 4;
}

} // namespace reel
