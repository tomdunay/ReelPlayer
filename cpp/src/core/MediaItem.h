#pragma once
#include "core/StrUtil.h"
#include "core/TimeFormat.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace reel {

struct MediaItem {
    std::wstring location;
    std::wstring title;
    int64_t durationMs = 0;
    std::vector<std::wstring> options;
    std::wstring subtitleFile;
    bool isCurrent = false;

    explicit MediaItem(std::wstring_view rawLocation, std::wstring_view customTitle = {}, int64_t durMs = 0)
        : location(normalizeLocation(rawLocation)),
          title(trim(customTitle).empty() ? defaultTitle(location) : trim(customTitle)),
          durationMs(durMs) {}

    bool isFile() const { return isLocalPath(location); }

    std::string mrlUtf8() const {
        return isFile() ? toUtf8(localPathToFileUri(location)) : toUtf8(location);
    }

    std::wstring mrl() const {
        return isFile() ? localPathToFileUri(location) : location;
    }

    std::wstring durationText() const {
        return durationMs > 0 ? TimeFormat::format(durationMs) : L"--:--";
    }

    static bool isLocalPath(std::wstring_view loc) {
        if (loc.find(L"://") != std::wstring_view::npos) return false;
        if (loc.size() >= 2 && loc[0] == L'\\' && loc[1] == L'\\') return true;
        if (loc.size() >= 3 && std::iswalpha(loc[0]) && loc[1] == L':' && (loc[2] == L'\\' || loc[2] == L'/')) return true;
        return false;
    }

    static std::wstring normalizeLocation(std::wstring_view raw) {
        std::wstring loc = trim(raw);
        if (loc.size() >= 2 && loc.front() == L'"' && loc.back() == L'"') {
            loc = loc.substr(1, loc.size() - 2);
        }
        if (startsWithIgnoreCase(loc, L"file:")) {
            return fileUriToLocalPath(loc);
        }
        return loc;
    }

    static std::wstring defaultTitle(std::wstring_view loc) {
        if (isLocalPath(loc)) {
            std::wstring s(loc);
            while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
            size_t sep = s.find_last_of(L"\\/");
            return sep == std::wstring::npos ? s : s.substr(sep + 1);
        }
        // URL: last path segment before '?'
        size_t q = loc.find(L'?');
        std::wstring_view noQuery = q == std::wstring_view::npos ? loc : loc.substr(0, q);
        while (!noQuery.empty() && noQuery.back() == L'/') noQuery.remove_suffix(1);
        size_t slash = noQuery.find_last_of(L'/');
        if (slash != std::wstring_view::npos && slash + 1 < noQuery.size()) {
            std::wstring seg = toWide(percentDecode(toUtf8(noQuery.substr(slash + 1))));
            if (!seg.empty()) return seg;
        }
        return std::wstring(loc);
    }
};

using MediaItemPtr = std::shared_ptr<MediaItem>;

} // namespace reel
