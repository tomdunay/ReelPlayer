#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace reel {

class ResumeStore {
public:
    static constexpr int64_t MinLengthMs = 5 * 60 * 1000;
    static constexpr int64_t MinPositionMs = 10 * 1000;
    static constexpr int64_t EndMarginMs = 15 * 1000;
    static constexpr size_t MaxEntries = 300;

    struct Entry {
        int64_t positionMs = 0;
        int64_t savedEpochSec = 0;
    };

    explicit ResumeStore(const std::wstring& directory = {});

    static bool isResumable(int64_t positionMs, int64_t lengthMs) {
        return lengthMs >= MinLengthMs && positionMs >= MinPositionMs && positionMs <= lengthMs - EndMarginMs;
    }

    std::optional<int64_t> get(const std::wstring& location) const;
    void update(const std::wstring& location, int64_t positionMs, int64_t lengthMs);
    void forget(const std::wstring& location);
    bool save() const;

private:
    std::wstring _filePath;
    // Keyed by lowercased location for case-insensitive lookup; original path kept for JSON output.
    std::unordered_map<std::wstring, std::pair<std::wstring, Entry>> _entries;
};

} // namespace reel
