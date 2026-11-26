#pragma once
#include "core/MediaItem.h"
#include <string>
#include <string_view>
#include <vector>

namespace reel {

class PlaylistParser {
public:
    static bool isPlaylistFile(std::wstring_view path);
    static bool shouldExpand(const std::wstring& path);

    static std::vector<MediaItemPtr> load(const std::wstring& path);
    static std::vector<MediaItemPtr> parseM3U(const std::string& text, const std::wstring& baseDir);
    static std::vector<MediaItemPtr> parsePls(const std::string& text, const std::wstring& baseDir);
    static std::vector<MediaItemPtr> parseXspf(const std::string& text, const std::wstring& baseDir);

    static bool save(const std::wstring& path, const std::vector<MediaItemPtr>& items);
    static std::string toM3U(const std::vector<MediaItemPtr>& items);
    static std::string toXspf(const std::vector<MediaItemPtr>& items);
};

} // namespace reel
