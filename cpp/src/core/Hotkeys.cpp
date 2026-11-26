#include "core/Hotkeys.h"
#include "core/StrUtil.h"
#include <algorithm>
#include <cctype>

namespace reel {

const std::vector<HotkeyAction>& HotkeyActions::all() {
    static const std::vector<HotkeyAction> actions = {
        { "PlayPause", "Play / Pause", { "Space", "MediaPlayPause" } },
        { "Stop", "Stop", { "S", "MediaStop" } },
        { "Next", "Next item", { "N", "MediaNextTrack" } },
        { "Previous", "Previous item", { "P", "MediaPreviousTrack" } },
        { "Fullscreen", "Toggle fullscreen", { "F", "F11" } },
        { "ExitFullscreen", "Exit fullscreen", { "Escape" } },
        { "SeekBack10", "Jump back 10 s", { "Left" } },
        { "SeekFwd10", "Jump forward 10 s", { "Right" } },
        { "SeekBack3", "Jump back 3 s", { "Shift+Left" } },
        { "SeekFwd3", "Jump forward 3 s", { "Shift+Right" } },
        { "SeekBack60", "Jump back 1 min", { "Alt+Left" } },
        { "SeekFwd60", "Jump forward 1 min", { "Alt+Right" } },
        { "SeekBack300", "Jump back 5 min", { "Ctrl+Left" } },
        { "SeekFwd300", "Jump forward 5 min", { "Ctrl+Right" } },
        { "VolumeUp", "Volume up", { "Up", "Ctrl+Up" } },
        { "VolumeDown", "Volume down", { "Down", "Ctrl+Down" } },
        { "Mute", "Mute", { "M" } },
        { "Slower", "Slower", { "[" } },
        { "Faster", "Faster", { "]" } },
        { "NormalSpeed", "Normal speed", { "=" } },
        { "NextFrame", "Next frame", { "E" } },
        { "PrevFrame", "Previous frame", { "Shift+E" } },
        { "CycleAspect", "Cycle aspect ratio", { "A" } },
        { "CycleCrop", "Cycle crop", { "C" } },
        { "CycleZoom", "Cycle zoom", { "Z" } },
        { "CycleAudio", "Cycle audio track", { "B" } },
        { "CycleSubtitle", "Cycle subtitle track", { "V" } },
        { "SubDelayDown", "Subtitle delay -50 ms", { "G" } },
        { "SubDelayUp", "Subtitle delay +50 ms", { "H" } },
        { "SubSizeUp", "Larger subtitles", { "Alt+Up" } },
        { "SubSizeDown", "Smaller subtitles", { "Alt+Down" } },
        { "FitWindow", "Fit window to video", { "Ctrl+1" } },
        { "FileBrowser", "Toggle file browser", { "Ctrl+B" } },
        { "AudioDelayDown", "Audio delay -50 ms", { "J" } },
        { "AudioDelayUp", "Audio delay +50 ms", { "K" } },
        { "Snapshot", "Take snapshot", { "Shift+S" } },
        { "RotateVideo", "Rotate video 90° clockwise", { "Ctrl+R" } },
        { "ShowTime", "Show time", { "T" } },
        { "CycleLoop", "Cycle loop mode", { "L" } },
        { "AbLoop", "A-B loop", { "Shift+L" } },
        { "ExportAbClip", "Export A-B clip", { "Ctrl+Shift+E" } },
        { "MiniPlayer", "Picture-in-Picture (Mini player)", { "Shift+P", "Ctrl+Shift+P" } },
        { "OpenFile", "Open file", { "Ctrl+O" } },
        { "OpenMultiple", "Open multiple files", { "Ctrl+Shift+O" } },
        { "OpenFolder", "Open folder", { "Ctrl+F" } },
        { "OpenDisc", "Open disc", { "Ctrl+D" } },
        { "OpenNetwork", "Open network stream", { "Ctrl+N" } },
        { "OpenCapture", "Open capture device", { "Ctrl+C" } },
        { "JumpToTime", "Jump to time", { "Ctrl+T" } },
        { "Effects", "Effects and filters", { "Ctrl+E" } },
        { "MediaInfo", "Media information", { "Ctrl+I" } },
        { "CodecInfo", "Codec information", { "Ctrl+J" } },
        { "Messages", "Messages", { "Ctrl+M" } },
        { "Preferences", "Preferences", { "Ctrl+P" } },
        { "MinimalInterface", "Minimal interface", { "Ctrl+H" } },
        { "Quit", "Quit", { "Ctrl+Q" } },
    };
    return actions;
}

const HotkeyAction* HotkeyActions::find(std::string_view id) {
    for (const auto& a : all()) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

static std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

static std::string canonicalMod(std::string_view m) {
    auto l = toLowerAscii(m);
    if (l == "ctrl" || l == "control") return "Ctrl";
    if (l == "alt") return "Alt";
    if (l == "shift") return "Shift";
    if (l == "win" || l == "windows") return "Win";
    return {};
}

HotkeyMap::HotkeyMap(const std::map<std::string, std::string>& overrides) {
    for (const auto& action : HotkeyActions::all()) {
        std::vector<std::string> gestures;
        if (auto it = overrides.find(action.id); it != overrides.end()) {
            gestures = split(it->second);
        } else {
            for (const auto& g : action.defaultGestures) gestures.push_back(normalize(g));
        }
        _actionToGestures[action.id] = gestures;
        for (const auto& g : gestures) {
            auto key = toLowerAscii(g);
            if (!_gestureToAction.contains(key)) _gestureToAction[key] = action.id;
        }
    }
}

std::optional<std::string> HotkeyMap::resolve(std::string_view gesture) const {
    auto key = toLowerAscii(normalize(gesture));
    if (auto it = _gestureToAction.find(key); it != _gestureToAction.end()) return it->second;
    return std::nullopt;
}

std::vector<std::string> HotkeyMap::gesturesFor(std::string_view actionId) const {
    if (auto it = _actionToGestures.find(std::string(actionId)); it != _actionToGestures.end()) return it->second;
    return {};
}

std::string HotkeyMap::gestureText(std::string_view actionId) const {
    auto g = gesturesFor(actionId);
    return g.empty() ? std::string{} : g.front();
}

std::vector<std::string> HotkeyMap::split(std::string_view joined) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= joined.size()) {
        size_t bar = joined.find('|', pos);
        auto part = bar == std::string_view::npos ? joined.substr(pos) : joined.substr(pos, bar - pos);
        auto norm = normalize(part);
        if (!norm.empty()) out.push_back(std::move(norm));
        if (bar == std::string_view::npos) break;
        pos = bar + 1;
    }
    return out;
}

std::string HotkeyMap::normalize(std::string_view gesture) {
    std::string text = trimUtf8(gesture);
    if (text.empty()) return {};
    std::string key;
    if (text == "+" || text.ends_with("++")) {
        key = "+";
        text = text.size() > 1 ? text.substr(0, text.size() - 2) : "";
    }
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t plus = text.find('+', pos);
        auto p = trimUtf8(plus == std::string::npos ? text.substr(pos) : text.substr(pos, plus - pos));
        if (!p.empty()) parts.push_back(p);
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    if (key.empty() && !parts.empty()) {
        key = parts.back();
        parts.pop_back();
    }
    static constexpr const char* ModOrder[] = { "Ctrl", "Alt", "Shift", "Win" };
    std::vector<std::string> mods;
    for (const auto& p : parts) {
        auto m = canonicalMod(p);
        if (!m.empty() && std::find(mods.begin(), mods.end(), m) == mods.end()) mods.push_back(m);
    }
    std::sort(mods.begin(), mods.end(), [](const std::string& a, const std::string& b) {
        auto idx = [](const std::string& x) {
            for (int i = 0; i < 4; ++i) if (x == ModOrder[i]) return i;
            return 4;
        };
        return idx(a) < idx(b);
    });
    if (key.size() == 1) key[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    else if (key.size() > 1) key[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    if (toLowerAscii(key) == "esc") key = "Escape";

    if (mods.empty()) return key;
    std::string out;
    for (const auto& m : mods) out += m + "+";
    return out + key;
}

std::optional<std::string> HotkeyMap::gestureFromVk(UINT vk, bool ctrl, bool alt, bool shift, bool win) {
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
        vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_LWIN || vk == VK_RWIN || vk == 0)
        return std::nullopt;

    std::string key;
    if (vk >= 'A' && vk <= 'Z') key = std::string(1, static_cast<char>(vk));
    else if (vk >= '0' && vk <= '9') key = std::string(1, static_cast<char>(vk));
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) key = "Num" + std::to_string(vk - VK_NUMPAD0);
    else if (vk >= VK_F1 && vk <= VK_F24) key = "F" + std::to_string(vk - VK_F1 + 1);
    else {
        switch (vk) {
            case VK_SPACE: key = "Space"; break;
            case VK_ESCAPE: key = "Escape"; break;
            case VK_RETURN: key = "Enter"; break;
            case VK_TAB: key = "Tab"; break;
            case VK_BACK: key = "Back"; break;
            case VK_DELETE: key = "Delete"; break;
            case VK_LEFT: key = "Left"; break;
            case VK_RIGHT: key = "Right"; break;
            case VK_UP: key = "Up"; break;
            case VK_DOWN: key = "Down"; break;
            case VK_HOME: key = "Home"; break;
            case VK_END: key = "End"; break;
            case VK_PRIOR: key = "PageUp"; break;
            case VK_NEXT: key = "PageDown"; break;
            case VK_OEM_4: key = "["; break;
            case VK_OEM_6: key = "]"; break;
            case VK_OEM_PLUS: key = "="; break;
            case VK_OEM_MINUS: key = "-"; break;
            case VK_ADD: key = "+"; break;
            case VK_SUBTRACT: key = "-"; break;
            case VK_OEM_COMMA: key = ","; break;
            case VK_OEM_PERIOD: key = "."; break;
            case VK_OEM_2: key = "/"; break;
            case VK_OEM_1: key = ";"; break;
            case VK_OEM_7: key = "'"; break;
            case VK_OEM_3: key = "`"; break;
            case VK_OEM_5: key = "\\"; break;
            case VK_MEDIA_PLAY_PAUSE: key = "MediaPlayPause"; break;
            case VK_MEDIA_STOP: key = "MediaStop"; break;
            case VK_MEDIA_NEXT_TRACK: key = "MediaNextTrack"; break;
            case VK_MEDIA_PREV_TRACK: key = "MediaPreviousTrack"; break;
            default: return std::nullopt;
        }
    }

    std::string prefix;
    if (ctrl) prefix += "Ctrl+";
    if (alt) prefix += "Alt+";
    if (shift) prefix += "Shift+";
    if (win) prefix += "Win+";
    return prefix + key;
}

} // namespace reel
