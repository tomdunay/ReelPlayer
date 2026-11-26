#include "core/PlaylistParser.h"
#include "core/StrUtil.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace reel {

static std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find_first_of("\r\n", pos);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, nl - pos));
        if (text[nl] == '\r' && nl + 1 < text.size() && text[nl + 1] == '\n') pos = nl + 2;
        else pos = nl + 1;
    }
    return lines;
}

static std::wstring resolveLocation(const std::string& raw, const std::wstring& baseDir) {
    std::wstring loc = toWide(raw);
    if (loc.find(L"://") != std::wstring::npos) {
        if (startsWithIgnoreCase(loc, L"file:")) return fileUriToLocalPath(loc);
        return loc;
    }
    if (MediaItem::isLocalPath(loc)) return loc;
    std::replace(loc.begin(), loc.end(), L'/', L'\\');
    std::error_code ec;
    fs::path combined = fs::path(baseDir) / fs::path(loc);
    return combined.lexically_normal().wstring();
}

static std::string xmlUnescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            auto rest = s.substr(i);
            if (rest.starts_with("&amp;")) { out += '&'; i += 4; continue; }
            if (rest.starts_with("&lt;")) { out += '<'; i += 3; continue; }
            if (rest.starts_with("&gt;")) { out += '>'; i += 3; continue; }
            if (rest.starts_with("&quot;")) { out += '"'; i += 5; continue; }
            if (rest.starts_with("&apos;")) { out += '\''; i += 5; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}

static std::string xmlEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

bool PlaylistParser::isPlaylistFile(std::wstring_view path) {
    auto ext = getExtensionLower(path);
    return ext == L".m3u" || ext == L".m3u8" || ext == L".pls" || ext == L".xspf";
}

bool PlaylistParser::shouldExpand(const std::wstring& path) {
    std::error_code ec;
    if (!isPlaylistFile(path) || !fs::is_regular_file(path, ec)) return false;
    auto ext = getExtensionLower(path);
    if (!ext.starts_with(L".m3u")) return true;
    std::ifstream in(fs::path(path), std::ios::binary);
    std::string line;
    for (int i = 0; i < 50 && std::getline(in, line); ++i) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        line = trimUtf8(line);
        if (line.size() >= 7 && _strnicmp(line.c_str(), "#EXT-X-", 7) == 0) return false;
    }
    return true;
}

std::vector<MediaItemPtr> PlaylistParser::load(const std::wstring& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    std::wstring baseDir = fs::path(getFullPath(path)).parent_path().wstring();
    auto ext = getExtensionLower(path);
    if (ext == L".pls") return parsePls(text, baseDir);
    if (ext == L".xspf") return parseXspf(text, baseDir);
    return parseM3U(text, baseDir);
}

std::vector<MediaItemPtr> PlaylistParser::parseM3U(const std::string& text, const std::wstring& baseDir) {
    std::vector<MediaItemPtr> items;
    std::wstring pendingTitle;
    int64_t pendingDur = 0;

    for (auto line : splitLines(text)) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        line = trimUtf8(line);
        if (line.empty()) continue;
        if (line.size() >= 8 && _strnicmp(line.c_str(), "#EXTINF:", 8) == 0) {
            std::string body = line.substr(8);
            size_t comma = body.find(',');
            std::string durPart = comma == std::string::npos ? body : body.substr(0, comma);
            size_t sp = durPart.find(' ');
            if (sp != std::string::npos) durPart = durPart.substr(0, sp);
            char* end = nullptr;
            double secs = std::strtod(durPart.c_str(), &end);
            pendingDur = (end && end != durPart.c_str() && std::isfinite(secs) && secs > 0)
                ? static_cast<int64_t>(secs * 1000.0) : 0;
            pendingTitle = comma == std::string::npos ? L"" : toWide(trimUtf8(body.substr(comma + 1)));
            continue;
        }
        if (line[0] == '#') continue;

        items.push_back(std::make_shared<MediaItem>(resolveLocation(line, baseDir), pendingTitle, pendingDur));
        pendingTitle.clear();
        pendingDur = 0;
    }
    return items;
}

static bool tryIndexed(const std::string& key, const char* prefix, int& outIndex) {
    size_t plen = std::strlen(prefix);
    if (key.size() <= plen || _strnicmp(key.c_str(), prefix, plen) != 0) return false;
    char* end = nullptr;
    long v = std::strtol(key.c_str() + plen, &end, 10);
    if (!end || *end != '\0') return false;
    outIndex = static_cast<int>(v);
    return true;
}

std::vector<MediaItemPtr> PlaylistParser::parsePls(const std::string& text, const std::wstring& baseDir) {
    std::map<int, std::string> files;
    std::map<int, std::wstring> titles;
    std::map<int, int64_t> lengths;

    for (auto line : splitLines(text)) {
        line = trimUtf8(line);
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = trimUtf8(line.substr(0, eq));
        std::string val = trimUtf8(line.substr(eq + 1));
        int n = 0;
        if (tryIndexed(key, "File", n)) files[n] = val;
        else if (tryIndexed(key, "Title", n)) titles[n] = toWide(val);
        else if (tryIndexed(key, "Length", n)) {
            char* end = nullptr;
            long long secs = std::strtoll(val.c_str(), &end, 10);
            if (end && *end == '\0' && secs > 0) lengths[n] = secs * 1000LL;
        }
    }

    std::vector<MediaItemPtr> items;
    for (const auto& [idx, f] : files) {
        std::wstring t = titles.count(idx) ? titles[idx] : L"";
        int64_t d = lengths.count(idx) ? lengths[idx] : 0;
        items.push_back(std::make_shared<MediaItem>(resolveLocation(f, baseDir), t, d));
    }
    return items;
}

static std::string extractTagText(std::string_view block, std::string_view tag) {
    std::string open = "<" + std::string(tag);
    std::string close = "</" + std::string(tag) + ">";
    size_t pos = block.find(open);
    while (pos != std::string_view::npos) {
        size_t after = pos + open.size();
        if (after < block.size() && (block[after] == '>' || std::isspace(static_cast<unsigned char>(block[after])))) {
            size_t gt = block.find('>', after);
            if (gt == std::string_view::npos) return {};
            size_t end = block.find(close, gt + 1);
            if (end == std::string_view::npos) return {};
            return trimUtf8(xmlUnescape(block.substr(gt + 1, end - (gt + 1))));
        }
        pos = block.find(open, after);
    }
    return {};
}

std::vector<MediaItemPtr> PlaylistParser::parseXspf(const std::string& text, const std::wstring& baseDir) {
    std::vector<MediaItemPtr> items;
    std::string_view sv(text);
    size_t pos = 0;
    while (true) {
        size_t start = sv.find("<track", pos);
        if (start == std::string_view::npos) break;
        size_t after = start + 6;
        if (after >= sv.size() || (sv[after] != '>' && !std::isspace(static_cast<unsigned char>(sv[after])))) {
            pos = after;
            continue;
        }
        size_t gt = sv.find('>', after);
        if (gt == std::string_view::npos) break;
        size_t end = sv.find("</track>", gt + 1);
        if (end == std::string_view::npos) break;
        auto block = sv.substr(gt + 1, end - (gt + 1));
        pos = end + 8;

        std::string loc = extractTagText(block, "location");
        if (loc.empty()) continue;
        std::string title = extractTagText(block, "title");
        std::string durStr = extractTagText(block, "duration");
        int64_t dur = 0;
        if (!durStr.empty()) {
            char* e = nullptr;
            long long v = std::strtoll(durStr.c_str(), &e, 10);
            if (e && *e == '\0' && v > 0) dur = v;
        }
        items.push_back(std::make_shared<MediaItem>(resolveLocation(loc, baseDir), toWide(title), dur));
    }
    return items;
}

bool PlaylistParser::save(const std::wstring& path, const std::vector<MediaItemPtr>& items) {
    std::string content = getExtensionLower(path) == L".xspf" ? toXspf(items) : toM3U(items);
    std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

std::string PlaylistParser::toM3U(const std::vector<MediaItemPtr>& items) {
    std::ostringstream sb;
    sb << "#EXTM3U\n";
    for (const auto& item : items) {
        int64_t secs = item->durationMs > 0 ? item->durationMs / 1000 : -1;
        sb << "#EXTINF:" << secs << "," << toUtf8(item->title) << "\n";
        sb << toUtf8(item->location) << "\n";
    }
    return sb.str();
}

std::string PlaylistParser::toXspf(const std::vector<MediaItemPtr>& items) {
    std::ostringstream sb;
    sb << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    sb << "<playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\">\n";
    sb << "  <title>Playlist</title>\n";
    sb << "  <trackList>\n";
    for (const auto& item : items) {
        sb << "    <track>\n";
        sb << "      <location>" << xmlEscape(item->mrlUtf8()) << "</location>\n";
        sb << "      <title>" << xmlEscape(toUtf8(item->title)) << "</title>\n";
        if (item->durationMs > 0) sb << "      <duration>" << item->durationMs << "</duration>\n";
        sb << "    </track>\n";
    }
    sb << "  </trackList>\n</playlist>\n";
    return sb.str();
}

} // namespace reel
