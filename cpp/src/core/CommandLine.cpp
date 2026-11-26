#include "core/CommandLine.h"
#include "core/StrUtil.h"
#include "core/TimeFormat.h"
#include <cmath>
#include <cstdlib>

namespace reel {

static std::wstring makeAbsolute(const std::wstring& loc) {
    if (loc.find(L"://") != std::wstring::npos) return loc;
    return getFullPath(loc);
}

CommandLine CommandLine::parse(const std::vector<std::wstring>& args) {
    CommandLine cmd;
    for (const auto& raw : args) {
        std::wstring arg = trim(raw);
        if (arg.empty()) continue;

        if (arg == L"--fullscreen" || arg == L"-f" || arg == L"/fullscreen") {
            cmd.fullscreen = true;
        } else if (startsWithIgnoreCase(arg, L"--start-time=")) {
            std::wstring val = arg.substr(13);
            wchar_t* end = nullptr;
            double secs = std::wcstod(val.c_str(), &end);
            if (!val.empty() && end == val.c_str() + val.size() && std::isfinite(secs) && secs > 0) {
                cmd.startTimeMs = static_cast<int64_t>(std::llround(secs * 1000.0));
            } else {
                int64_t ms = 0;
                if (TimeFormat::tryParse(val, ms)) cmd.startTimeMs = ms;
            }
        } else if (arg.starts_with(L"--")) {
            // Ignore unknown switches.
        } else {
            cmd.locations.push_back(makeAbsolute(arg));
        }
    }
    return cmd;
}

} // namespace reel
