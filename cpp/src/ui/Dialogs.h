#pragma once
#include "core/Hotkeys.h"
#include "core/MediaItem.h"
#include "core/PlayerService.h"
#include "core/Settings.h"
#include <windows.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace reel {

struct EffectsState {
    bool eqEnabled = false;
    int eqPreset = -1;
    float eqPreamp = 12.0f;
    float eqBands[10] = {};

    bool adjustEnabled = false;
    float brightness = 1.0f;
    float contrast = 1.0f;
    float hue = 0.0f;
    float saturation = 1.0f;
    float gamma = 1.0f;

    int transformIndex = 0;
    bool sharpen = false;
    double sharpenSigma = 0.05;
};

class Dialogs {
public:
    static std::vector<std::wstring> openFiles(HWND owner, const std::wstring& initDir, bool multiple);
    static std::optional<std::wstring> openFolder(HWND owner, const std::wstring& initDir);
    static std::optional<std::wstring> openSubtitle(HWND owner, const std::wstring& initDir);
    static std::optional<std::wstring> savePlaylist(HWND owner, const std::wstring& initDir);

    static std::optional<MediaItemPtr> showOpenNetwork(HWND owner);
    static std::optional<MediaItemPtr> showOpenDisc(HWND owner);
    static std::optional<MediaItemPtr> showOpenCapture(HWND owner);
    static std::optional<int64_t> showJumpToTime(HWND owner, int64_t curMs, int64_t lenMs);

    static void showEffects(HWND owner, EffectsState& state,
                            std::function<void()> onEqChanged,
                            std::function<void()> onAdjustChanged,
                            std::function<void()> onFiltersChanged);
    static void showSync(HWND owner, PlayerService& engine,
                         std::function<void(int64_t)> onAudioDelayMs,
                         std::function<void(int64_t)> onSubDelayMs);
    static void showMediaInfo(HWND owner, PlayerService& engine);
    static void showMessages(HWND owner, LogBuffer& log);
    static bool showPreferences(HWND owner, SettingsService& settings, bool& outRestartRequired);
    static void showShortcuts(HWND owner, const HotkeyMap& hotkeys);
    static void showAbout(HWND owner);
};

} // namespace reel
