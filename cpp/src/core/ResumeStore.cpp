#include "core/ResumeStore.h"
#include "core/Settings.h"
#include "core/StrUtil.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace reel {

static int64_t utcNowSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

ResumeStore::ResumeStore(const std::wstring& directory) {
    std::wstring dir = directory.empty() ? SettingsService::appDataDirectory() : directory;
    _filePath = (fs::path(dir) / L"resume.json").wstring();
    std::error_code ec;
    if (!fs::exists(_filePath, ec)) return;
    try {
        std::ifstream in(fs::path(_filePath), std::ios::binary);
        json j = json::parse(in);
        if (!j.is_object()) return;
        for (auto& [k, v] : j.items()) {
            if (!v.is_object()) continue;
            Entry e;
            if (v.contains("PositionMs") && v["PositionMs"].is_number_integer())
                e.positionMs = v["PositionMs"].get<int64_t>();
            if (v.contains("SavedEpochSec") && v["SavedEpochSec"].is_number_integer())
                e.savedEpochSec = v["SavedEpochSec"].get<int64_t>();
            else
                e.savedEpochSec = utcNowSec();
            std::wstring loc = toWide(k);
            _entries[toLower(loc)] = { loc, e };
        }
    } catch (...) {
        // start empty
    }
}

std::optional<int64_t> ResumeStore::get(const std::wstring& location) const {
    auto it = _entries.find(toLower(location));
    if (it == _entries.end()) return std::nullopt;
    return it->second.second.positionMs;
}

void ResumeStore::update(const std::wstring& location, int64_t positionMs, int64_t lengthMs) {
    std::wstring key = toLower(location);
    if (isResumable(positionMs, lengthMs)) {
        _entries[key] = { location, Entry{ positionMs, utcNowSec() } };
    } else {
        _entries.erase(key);
    }

    if (_entries.size() > MaxEntries) {
        std::vector<std::pair<std::wstring, int64_t>> all;
        all.reserve(_entries.size());
        for (const auto& [k, pair] : _entries) all.emplace_back(k, pair.second.savedEpochSec);
        std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        size_t removeCount = _entries.size() - MaxEntries;
        for (size_t i = 0; i < removeCount; ++i) _entries.erase(all[i].first);
    }
}

void ResumeStore::forget(const std::wstring& location) {
    _entries.erase(toLower(location));
}

bool ResumeStore::save() const {
    try {
        std::error_code ec;
        fs::create_directories(fs::path(_filePath).parent_path(), ec);
        json j = json::object();
        for (const auto& [k, pair] : _entries) {
            j[toUtf8(pair.first)] = {
                { "PositionMs", pair.second.positionMs },
                { "SavedEpochSec", pair.second.savedEpochSec }
            };
        }
        std::wstring tmp = _filePath + L".tmp";
        {
            std::ofstream out(fs::path(tmp), std::ios::binary | std::ios::trunc);
            if (!out) return false;
            std::string dump = j.dump();
            out.write(dump.data(), static_cast<std::streamsize>(dump.size()));
        }
        return MoveFileExW(tmp.c_str(), _filePath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    } catch (...) {
        return false;
    }
}

} // namespace reel
