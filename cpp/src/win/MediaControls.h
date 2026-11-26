#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace reel {

/// Direct raw-COM WinRT System Media Transport Controls (Windows media flyout + hardware media keys).
class MediaControls {
public:
    MediaControls() = default;
    ~MediaControls();

    MediaControls(const MediaControls&) = delete;
    MediaControls& operator=(const MediaControls&) = delete;

    std::function<void(const char*)> onButtonPressed;

    void attach(HWND hwnd);
    void updateStatus(bool playing, bool paused);
    void updateMetadata(const std::wstring& title, const std::wstring& artist, bool isVideo, const std::wstring& artworkUrl);
    void updateTimeline(int64_t positionMs, int64_t lengthMs);
    void clear();
    void dispose();

private:
    void* _smtc = nullptr;
    void* _smtc2 = nullptr;
    void* _updater = nullptr;
    void* _handler = nullptr;
    int64_t _token = 0;
};

} // namespace reel
