#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace reel {

class MediaFiles {
public:
    static const std::vector<std::wstring>& videoExtensions();
    static const std::vector<std::wstring>& audioExtensions();
    static const std::vector<std::wstring>& subtitleExtensions();
    static const std::vector<std::wstring>& playlistExtensions();

    static bool isMedia(std::wstring_view path);
    static bool isSubtitle(std::wstring_view path);
    static bool isPlaylist(std::wstring_view path);

    static std::vector<std::wstring> enumerateFolder(const std::wstring& folder);
    static std::optional<std::wstring> findSiblingMedia(const std::wstring& filePath, int offset);

    // Double-null-terminated Win32 OPENFILENAME filter strings.
    static std::wstring win32MediaFilter();
    static std::wstring win32SubtitleFilter();

    static int naturalCompare(std::wstring_view a, std::wstring_view b);
    static bool naturalLess(std::wstring_view a, std::wstring_view b) { return naturalCompare(a, b) < 0; }
};

} // namespace reel
