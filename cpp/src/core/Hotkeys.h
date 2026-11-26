#pragma once
#include <windows.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace reel {

struct HotkeyAction {
    std::string id;
    std::string description;
    std::vector<std::string> defaultGestures;
};

class HotkeyActions {
public:
    static const std::vector<HotkeyAction>& all();
    static const HotkeyAction* find(std::string_view id);
};

class HotkeyMap {
public:
    explicit HotkeyMap(const std::map<std::string, std::string>& overrides = {});

    std::optional<std::string> resolve(std::string_view gesture) const;
    std::vector<std::string> gesturesFor(std::string_view actionId) const;
    std::string gestureText(std::string_view actionId) const;

    static std::vector<std::string> split(std::string_view joined);
    static std::string normalize(std::string_view gesture);
    static std::optional<std::string> gestureFromVk(UINT vk, bool ctrl, bool alt, bool shift, bool win);

private:
    std::map<std::string, std::string> _gestureToAction; // lowercased canonical gesture -> actionId
    std::map<std::string, std::vector<std::string>> _actionToGestures;
};

} // namespace reel
