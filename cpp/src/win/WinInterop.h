#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <cstdint>
#include <string>

namespace reel {

class AppLog {
public:
    static std::wstring directory();
    static void write(const std::wstring& message);
    static void prune();
};

class WinInterop {
public:
    static void keepAwake(bool on);
    static void setAppDarkMode(bool dark);
    static void setDarkTitleBar(HWND hwnd, bool dark);
    static bool systemUsesLightTheme();
    static void blackenVlcWindows(HWND topLevel);

    static bool registerFileAssociations();
    static void unregisterFileAssociations();
    static void openDefaultAppsSettings();
};

class TaskbarController {
public:
    static constexpr UINT ThumbPrevId = 9001;
    static constexpr UINT ThumbPlayId = 9002;
    static constexpr UINT ThumbNextId = 9003;

    ~TaskbarController();
    void attach(HWND hwnd);
    void updateProgress(bool active, bool paused, int64_t posMs, int64_t lenMs);
    void updatePlayState(bool playing);

private:
    HICON makeGlyphIcon(bool isPlay, bool isPrev, bool isNext);

    HWND _hwnd = nullptr;
    ITaskbarList3* _tb = nullptr;
    HICON _icoPrev = nullptr;
    HICON _icoPlay = nullptr;
    HICON _icoPause = nullptr;
    HICON _icoNext = nullptr;
    bool _buttonsAdded = false;
};

} // namespace reel
