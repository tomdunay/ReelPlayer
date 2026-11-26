#include "core/MediaFiles.h"
#include "core/StrUtil.h"
#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace reel {

const std::vector<std::wstring>& MediaFiles::videoExtensions() {
    static const std::vector<std::wstring> exts = {
        L".3g2", L".3gp", L".3gp2", L".amv", L".asf", L".avi", L".bik", L".divx", L".drc", L".dv", L".f4v", L".flv", L".gvi",
        L".gxf", L".m1v", L".m2t", L".m2ts", L".m2v", L".m4v", L".mkv", L".mov", L".mp2v", L".mp4", L".mp4v", L".mpe", L".mpeg",
        L".mpeg1", L".mpeg2", L".mpeg4", L".mpg", L".mpv2", L".mts", L".mtv", L".mxf", L".nsv", L".nut", L".nuv", L".ogm",
        L".ogv", L".ogx", L".ps", L".rec", L".rm", L".rmvb", L".rpl", L".thp", L".tod", L".ts", L".tts", L".vob", L".vro",
        L".webm", L".wm", L".wmv", L".wtv", L".xesc", L".y4m", L".hevc", L".h264", L".h265", L".264", L".265", L".ivf", L".iso"
    };
    return exts;
}

const std::vector<std::wstring>& MediaFiles::audioExtensions() {
    static const std::vector<std::wstring> exts = {
        L".3ga", L".a52", L".aac", L".ac3", L".adt", L".adts", L".aif", L".aifc", L".aiff", L".alac", L".amr", L".ape", L".au",
        L".awb", L".caf", L".dts", L".eac3", L".flac", L".it", L".m4a", L".m4b", L".m4p", L".mid", L".mka", L".mlp", L".mod",
        L".mp1", L".mp2", L".mp3", L".mpa", L".mpc", L".oga", L".ogg", L".oma", L".opus", L".qcp", L".ra", L".rmi", L".s3m",
        L".spx", L".thd", L".tta", L".voc", L".vqf", L".w64", L".wav", L".wma", L".wv", L".xa", L".xm"
    };
    return exts;
}

const std::vector<std::wstring>& MediaFiles::subtitleExtensions() {
    static const std::vector<std::wstring> exts = {
        L".srt", L".ass", L".ssa", L".vtt", L".sub", L".idx", L".smi", L".sami", L".txt", L".usf", L".jss", L".psb", L".rt", L".mpl2", L".dks"
    };
    return exts;
}

const std::vector<std::wstring>& MediaFiles::playlistExtensions() {
    static const std::vector<std::wstring> exts = { L".m3u", L".m3u8", L".pls", L".xspf", L".cue" };
    return exts;
}

bool MediaFiles::isMedia(std::wstring_view path) {
    static const std::unordered_set<std::wstring> set = [] {
        std::unordered_set<std::wstring> s;
        for (const auto& e : videoExtensions()) s.insert(e);
        for (const auto& e : audioExtensions()) s.insert(e);
        for (const auto& e : playlistExtensions()) s.insert(e);
        return s;
    }();
    return set.contains(getExtensionLower(path));
}

bool MediaFiles::isSubtitle(std::wstring_view path) {
    static const std::unordered_set<std::wstring> set(subtitleExtensions().begin(), subtitleExtensions().end());
    return set.contains(getExtensionLower(path));
}

bool MediaFiles::isPlaylist(std::wstring_view path) {
    static const std::unordered_set<std::wstring> set(playlistExtensions().begin(), playlistExtensions().end());
    return set.contains(getExtensionLower(path));
}

std::vector<std::wstring> MediaFiles::enumerateFolder(const std::wstring& folder) {
    std::vector<std::wstring> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::wstring p = it->path().wstring();
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        if (isMedia(p) && !endsWithIgnoreCase(p, L".cue")) files.push_back(std::move(p));
    }
    std::sort(files.begin(), files.end(), naturalLess);
    return files;
}

std::optional<std::wstring> MediaFiles::findSiblingMedia(const std::wstring& filePath, int offset) {
    if (offset == 0) return std::nullopt;
    std::error_code ec;
    fs::path fp(filePath);
    fs::path dir = fp.parent_path();
    if (dir.empty() || !fs::exists(dir, ec)) return std::nullopt;

    std::vector<std::wstring> siblings;
    for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::wstring p = it->path().wstring();
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        if (isMedia(p) && !isPlaylist(p)) siblings.push_back(std::move(p));
    }
    std::sort(siblings.begin(), siblings.end(), naturalLess);

    std::wstring target = getFullPath(filePath);
    int idx = -1;
    for (int i = 0; i < static_cast<int>(siblings.size()); ++i) {
        if (equalsIgnoreCase(getFullPath(siblings[i]), target)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return std::nullopt;
    int next = idx + offset;
    if (next < 0 || next >= static_cast<int>(siblings.size())) return std::nullopt;
    return siblings[next];
}

static std::wstring joinPatterns(const std::vector<std::wstring>& exts) {
    std::wstring out;
    for (size_t i = 0; i < exts.size(); ++i) {
        if (i > 0) out.push_back(L';');
        out.push_back(L'*');
        out += exts[i];
    }
    return out;
}

std::wstring MediaFiles::win32MediaFilter() {
    std::vector<std::wstring> all = videoExtensions();
    all.insert(all.end(), audioExtensions().begin(), audioExtensions().end());
    all.insert(all.end(), playlistExtensions().begin(), playlistExtensions().end());

    std::wstring f;
    auto add = [&](std::wstring_view label, const std::wstring& pat) {
        f += label; f.push_back(L'\0');
        f += pat; f.push_back(L'\0');
    };
    add(L"Media files", joinPatterns(all));
    add(L"Video files", joinPatterns(videoExtensions()));
    add(L"Audio files", joinPatterns(audioExtensions()));
    add(L"Playlists", joinPatterns(playlistExtensions()));
    add(L"All files (*.*)", L"*.*");
    f.push_back(L'\0');
    return f;
}

std::wstring MediaFiles::win32SubtitleFilter() {
    std::wstring f;
    f += L"Subtitle files"; f.push_back(L'\0');
    f += joinPatterns(subtitleExtensions()); f.push_back(L'\0');
    f += L"All files (*.*)"; f.push_back(L'\0');
    f += L"*.*"; f.push_back(L'\0');
    f.push_back(L'\0');
    return f;
}

int MediaFiles::naturalCompare(std::wstring_view x, std::wstring_view y) {
    size_t i = 0, j = 0;
    while (i < x.size() && j < y.size()) {
        if (std::iswdigit(x[i]) && std::iswdigit(y[j])) {
            size_t si = i; while (i < x.size() && std::iswdigit(x[i])) ++i;
            size_t sj = j; while (j < y.size() && std::iswdigit(y[j])) ++j;
            auto a = x.substr(si, i - si);
            auto b = y.substr(sj, j - sj);
            while (!a.empty() && a.front() == L'0') a.remove_prefix(1);
            while (!b.empty() && b.front() == L'0') b.remove_prefix(1);
            if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
            int c = a.compare(b);
            if (c != 0) return c < 0 ? -1 : 1;
        } else {
            wchar_t cx = static_cast<wchar_t>(std::towupper(x[i]));
            wchar_t cy = static_cast<wchar_t>(std::towupper(y[j]));
            if (cx != cy) return cx < cy ? -1 : 1;
            ++i; ++j;
        }
    }
    size_t rx = x.size() - i, ry = y.size() - j;
    return rx < ry ? -1 : (rx > ry ? 1 : 0);
}

} // namespace reel
