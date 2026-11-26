#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace reel {

class TimeFormat {
public:
    static std::wstring format(int64_t ms);
    static bool tryParse(std::wstring_view text, int64_t& outMs);
};

} // namespace reel
