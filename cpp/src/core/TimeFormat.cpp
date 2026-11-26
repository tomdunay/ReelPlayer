#include "core/TimeFormat.h"
#include "core/StrUtil.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace reel {

std::wstring TimeFormat::format(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t totalSec = ms / 1000;
    const int64_t hours = totalSec / 3600;
    const int64_t mins = (totalSec % 3600) / 60;
    const int64_t secs = totalSec % 60;
    wchar_t buf[32];
    if (hours > 0) {
        std::swprintf(buf, 32, L"%lld:%02lld:%02lld", hours, mins, secs);
    } else {
        std::swprintf(buf, 32, L"%02lld:%02lld", mins, secs);
    }
    return buf;
}

bool TimeFormat::tryParse(std::wstring_view text, int64_t& outMs) {
    outMs = 0;
    std::wstring s = trim(text);
    if (s.empty()) return false;

    std::vector<std::wstring> parts;
    size_t pos = 0;
    while (true) {
        size_t colon = s.find(L':', pos);
        if (colon == std::wstring::npos) {
            parts.push_back(s.substr(pos));
            break;
        }
        parts.push_back(s.substr(pos, colon - pos));
        pos = colon + 1;
    }
    if (parts.empty() || parts.size() > 3) return false;

    auto parseDouble = [](const std::wstring& str, double& val) -> bool {
        if (str.empty()) return false;
        wchar_t* end = nullptr;
        val = std::wcstod(str.c_str(), &end);
        return end == str.c_str() + str.size() && std::isfinite(val);
    };
    auto parseInt64 = [](const std::wstring& str, int64_t& val) -> bool {
        if (str.empty()) return false;
        wchar_t* end = nullptr;
        val = std::wcstoll(str.c_str(), &end, 10);
        return end == str.c_str() + str.size();
    };

    double secs = 0;
    if (!parseDouble(parts.back(), secs) || secs < 0.0) return false;
    if (parts.size() > 1 && secs >= 60.0) return false;

    int64_t mins = 0, hours = 0;
    if (parts.size() >= 2) {
        if (!parseInt64(parts[parts.size() - 2], mins) || mins < 0 || (parts.size() == 3 && mins >= 60)) return false;
    }
    if (parts.size() == 3) {
        if (!parseInt64(parts[0], hours) || hours < 0) return false;
    }

    outMs = hours * 3600000LL + mins * 60000LL + static_cast<int64_t>(std::llround(secs * 1000.0));
    return true;
}

} // namespace reel
