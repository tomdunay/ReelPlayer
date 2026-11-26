#pragma once
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace reel {

inline std::string toUtf8(std::wstring_view ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), len, nullptr, nullptr);
    return out;
}

inline std::wstring toWide(std::string_view s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

inline std::wstring trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

inline std::string trimUtf8(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

inline std::wstring toLower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& ch : out) ch = static_cast<wchar_t>(std::towlower(ch));
    return out;
}

inline bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

inline bool startsWithIgnoreCase(std::wstring_view s, std::wstring_view prefix) {
    if (s.size() < prefix.size()) return false;
    return equalsIgnoreCase(s.substr(0, prefix.size()), prefix);
}

inline bool endsWithIgnoreCase(std::wstring_view s, std::wstring_view suffix) {
    if (s.size() < suffix.size()) return false;
    return equalsIgnoreCase(s.substr(s.size() - suffix.size()), suffix);
}

inline bool containsIgnoreCase(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) return true;
    auto h = toLower(haystack);
    auto n = toLower(needle);
    return h.find(n) != std::wstring::npos;
}

inline std::wstring getExtensionLower(std::wstring_view path) {
    size_t dot = path.find_last_of(L'.');
    size_t sep = path.find_last_of(L"\\/");
    if (dot == std::wstring_view::npos || (sep != std::wstring_view::npos && dot < sep)) return {};
    return toLower(path.substr(dot));
}

inline std::string percentEncodePath(std::string_view utf8Path) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8Path.size() + 16);
    for (unsigned char c : utf8Path) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

inline std::string percentDecode(std::string_view s) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

inline std::wstring localPathToFileUri(std::wstring_view path) {
    std::string u8 = toUtf8(path);
    std::replace(u8.begin(), u8.end(), '\\', '/');
    if (u8.starts_with("//")) {
        // UNC path \\server\share -> file://server/share
        return toWide("file:" + percentEncodePath(u8));
    }
    if (!u8.starts_with('/')) u8 = "/" + u8;
    return toWide("file://" + percentEncodePath(u8));
}

inline std::wstring fileUriToLocalPath(std::wstring_view uri) {
    if (!startsWithIgnoreCase(uri, L"file://")) return std::wstring(uri);
    std::string u8 = percentDecode(toUtf8(uri.substr(7)));
    if (u8.size() >= 3 && u8[0] == '/' && std::isalpha(static_cast<unsigned char>(u8[1])) && u8[2] == ':') {
        u8.erase(0, 1);
    } else if (!u8.empty() && u8[0] != '/') {
        u8 = "\\\\" + u8;
    }
    std::replace(u8.begin(), u8.end(), '/', '\\');
    return toWide(u8);
}

inline std::wstring getFullPath(const std::wstring& path) {
    if (path.empty()) return {};
    DWORD len = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (len == 0) return path;
    std::wstring out(len, L'\0');
    DWORD got = GetFullPathNameW(path.c_str(), len, out.data(), nullptr);
    if (got == 0 || got >= len) return path;
    out.resize(got);
    return out;
}

} // namespace reel
