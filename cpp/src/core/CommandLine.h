#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace reel {

struct CommandLine {
    std::vector<std::wstring> locations;
    bool fullscreen = false;
    int64_t startTimeMs = 0;

    static CommandLine parse(const std::vector<std::wstring>& args);
};

} // namespace reel
