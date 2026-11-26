#include "win/WinInterop.h"
#include "core/MediaFiles.h"
#include "core/Settings.h"
#include "core/StrUtil.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace reel {

static std::mutex g_logMtx;

std::wstring AppLog::directory() {
    return (fs::path(SettingsService::appDataDirectory()) / L"logs").wstring();
}

void AppLog::write(const std::wstring& message) {
    try {
        std::lock_guard lk(g_logMtx);
        auto dir = directory();
        std::error_code ec;
        fs::create_directories(dir, ec);
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t fname[64];
        std::swprintf(fname, 64, L"reelplayer-%04u%02u%02u.log", st.wYear, st.wMonth, st.wDay);
        wchar_t tbuf[32];
        std::swprintf(tbuf, 32, L"%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::ofstream out(fs::path(dir) / fname, std::ios::binary | std::ios::app);
        std::string line = toUtf8(std::wstring(tbuf) + message + L"\r\n");
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
    } catch (...) {}
}

void AppLog::prune() {
    try {
        auto dir = directory();
        std::error_code ec;
        if (!fs::exists(dir, ec)) return;
        auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * 14);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == L".log") {
                if (entry.last_write_time(ec) < cutoff) fs::remove(entry.path(), ec);
            }
        }
    } catch (...) {}
}

void WinInterop::keepAwake(bool on) {
    SetThreadExecutionState(on
        ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
        : ES_CONTINUOUS);
}

void WinInterop::setAppDarkMode(bool dark) {
    // Undocumented uxtheme.dll ordinals 135 (SetPreferredAppMode) & 136 (FlushMenuThemes)
    // used by Windows 10 1903+ / Windows 11 apps to give native HMENU popups a dark theme.
    if (HMODULE ux = LoadLibraryW(L"uxtheme.dll")) {
        using FnSetPreferredAppMode = int(WINAPI*)(int);
        using FnFlushMenuThemes = void(WINAPI*)();
        auto setMode = reinterpret_cast<FnSetPreferredAppMode>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
        auto flush = reinterpret_cast<FnFlushMenuThemes>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
        if (setMode) setMode(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
        if (flush) flush();
    }
}

void WinInterop::setDarkTitleBar(HWND hwnd, bool dark) {
    if (!hwnd) return;
    BOOL val = dark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Win10 2004+ / Win11)
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val)))) {
        DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
    }
}

void WinInterop::setWindowRoundedAndBackdrop(HWND hwnd, bool acrylic) {
    if (!hwnd) return;
    // DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2 (Win11)
    DWORD corner = 2;
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
    if (acrylic) {
        // DWMWA_SYSTEMBACKDROP_TYPE = 38, DWMSBT_TRANSIENTWINDOW (Acrylic) = 3 (Win11 22H2+)
        DWORD backdrop = 3;
        DwmSetWindowAttribute(hwnd, 38, &backdrop, sizeof(backdrop));
    }
}

bool WinInterop::systemUsesLightTheme() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD val = 0, sz = sizeof(val);
    bool light = (RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS) && val != 0;
    RegCloseKey(key);
    return light;
}

static BOOL CALLBACK enumVlcChildProc(HWND hwnd, LPARAM) {
    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    if (wcsncmp(cls, L"VLC video main", 14) == 0) {
        SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
        EnableWindow(hwnd, FALSE); // let all mouse/wheel/click events pass straight to our video host
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    return TRUE;
}

void WinInterop::blackenVlcWindows(HWND topLevel) {
    if (topLevel) EnumChildWindows(topLevel, enumVlcChildProc, 0);
}

static bool regSetStr(HKEY root, const std::wstring& subkey, const wchar_t* name, const std::wstring& val) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    DWORD bytes = static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t));
    bool ok = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()), bytes) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

bool WinInterop::registerFileAssociations() {
    wchar_t exeBuf[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) == 0) return false;
    std::wstring exe(exeBuf);
    std::wstring exeName = fs::path(exe).filename().wstring();
    std::wstring openCmd = L"\"" + exe + L"\" \"%1\"";
    const std::wstring progId = L"ReelPlayer.Media";

    std::vector<std::wstring> exts = MediaFiles::videoExtensions();
    exts.insert(exts.end(), MediaFiles::audioExtensions().begin(), MediaFiles::audioExtensions().end());
    exts.insert(exts.end(), MediaFiles::playlistExtensions().begin(), MediaFiles::playlistExtensions().end());

    regSetStr(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId, L"", L"Reel Player media file");
    regSetStr(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId + L"\\DefaultIcon", L"", L"\"" + exe + L"\",0");
    regSetStr(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId + L"\\shell\\open\\command", L"", openCmd);

    std::wstring appKey = L"Software\\Classes\\Applications\\" + exeName;
    regSetStr(HKEY_CURRENT_USER, appKey, L"FriendlyAppName", L"Reel Player");
    regSetStr(HKEY_CURRENT_USER, appKey + L"\\shell\\open\\command", L"", openCmd);
    for (const auto& ext : exts) regSetStr(HKEY_CURRENT_USER, appKey + L"\\SupportedTypes", ext.c_str(), L"");

    std::wstring capPath = L"Software\\ReelPlayer\\Capabilities";
    regSetStr(HKEY_CURRENT_USER, capPath, L"ApplicationName", L"Reel Player");
    regSetStr(HKEY_CURRENT_USER, capPath, L"ApplicationDescription", L"Plays almost any video or audio file.");
    for (const auto& ext : exts) {
        regSetStr(HKEY_CURRENT_USER, capPath + L"\\FileAssociations", ext.c_str(), progId);
        HKEY k = nullptr;
        std::wstring owp = L"Software\\Classes\\" + ext + L"\\OpenWithProgids";
        if (RegCreateKeyExW(HKEY_CURRENT_USER, owp.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(k, progId.c_str(), 0, REG_NONE, nullptr, 0);
            RegCloseKey(k);
        }
    }
    regSetStr(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"ReelPlayer", capPath);
    SHChangeNotify(SHCNE_ASSOCCHANGED, 0, nullptr, nullptr);
    return true;
}

void WinInterop::unregisterFileAssociations() {
    wchar_t exeBuf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    std::wstring exeName = fs::path(exeBuf).filename().wstring();
    if (exeName.empty()) exeName = L"ReelPlayer.exe";

    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\ReelPlayer.Media");
    RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\Applications\\" + exeName).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ReelPlayer");

    HKEY reg = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_WRITE, &reg) == ERROR_SUCCESS) {
        RegDeleteValueW(reg, L"ReelPlayer");
        RegCloseKey(reg);
    }
    std::vector<std::wstring> exts = MediaFiles::videoExtensions();
    exts.insert(exts.end(), MediaFiles::audioExtensions().begin(), MediaFiles::audioExtensions().end());
    exts.insert(exts.end(), MediaFiles::playlistExtensions().begin(), MediaFiles::playlistExtensions().end());
    for (const auto& ext : exts) {
        HKEY k = nullptr;
        std::wstring owp = L"Software\\Classes\\" + ext + L"\\OpenWithProgids";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, owp.c_str(), 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, L"ReelPlayer.Media");
            RegCloseKey(k);
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, 0, nullptr, nullptr);
}

void WinInterop::openDefaultAppsSettings() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
}

// ----- TaskbarController -----

TaskbarController::~TaskbarController() {
    if (_tb) { _tb->Release(); _tb = nullptr; }
    if (_icoPrev) DestroyIcon(_icoPrev);
    if (_icoPlay) DestroyIcon(_icoPlay);
    if (_icoPause) DestroyIcon(_icoPause);
    if (_icoNext) DestroyIcon(_icoNext);
}

HICON TaskbarController::makeGlyphIcon(bool isPlay, bool isPrev, bool isNext) {
    const int sz = 24;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    uint32_t* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP colorBmp = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!colorBmp || !bits) return nullptr;

    std::memset(bits, 0, sz * sz * 4);
    auto setPx = [&](int x, int y, uint8_t a = 255) {
        if (x >= 0 && x < sz && y >= 0 && y < sz) {
            bits[y * sz + x] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(a) << 16) |
                               (static_cast<uint32_t>(a) << 8) | static_cast<uint32_t>(a);
        }
    };
    if (isPlay) {
        for (int y = 4; y <= 19; ++y) {
            double half = std::abs(y - 11.5);
            double maxX = 19.5 - half * 1.65;
            for (int x = 6; x <= static_cast<int>(maxX); ++x) setPx(x, y);
            double frac = maxX - std::floor(maxX);
            if (frac > 0.15) setPx(static_cast<int>(maxX) + 1, y, static_cast<uint8_t>(frac * 255));
        }
    } else if (isPrev) {
        for (int y = 5; y <= 18; ++y) { setPx(4, y); setPx(5, y); setPx(6, y); }
        for (int y = 5; y <= 18; ++y) {
            double d = std::abs(y - 11.5);
            double minX = 8.5 + d * 1.45;
            for (int x = static_cast<int>(std::ceil(minX)); x <= 19; ++x) setPx(x, y);
        }
    } else if (isNext) {
        for (int y = 5; y <= 18; ++y) { setPx(17, y); setPx(18, y); setPx(19, y); }
        for (int y = 5; y <= 18; ++y) {
            double d = std::abs(y - 11.5);
            double maxX = 15.5 - d * 1.45;
            for (int x = 4; x <= static_cast<int>(maxX); ++x) setPx(x, y);
        }
    } else { // Pause
        for (int y = 4; y <= 19; ++y) {
            for (int x = 5; x <= 9; ++x) setPx(x, y);
            for (int x = 14; x <= 18; ++x) setPx(x, y);
        }
    }

    HBITMAP maskBmp = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = colorBmp;
    ii.hbmMask = maskBmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon;
}

void TaskbarController::attach(HWND hwnd) {
    _hwnd = hwnd;
    if (_tb) return;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_tb))) || !_tb)
        return;
    if (FAILED(_tb->HrInit())) {
        _tb->Release();
        _tb = nullptr;
        return;
    }
    _icoPrev = makeGlyphIcon(false, true, false);
    _icoPlay = makeGlyphIcon(true, false, false);
    _icoPause = makeGlyphIcon(false, false, false);
    _icoNext = makeGlyphIcon(false, false, true);

    THUMBBUTTON btns[3]{};
    btns[0].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    btns[0].iId = ThumbPrevId;
    btns[0].hIcon = _icoPrev;
    wcscpy_s(btns[0].szTip, L"Previous");
    btns[0].dwFlags = THBF_ENABLED;

    btns[1].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    btns[1].iId = ThumbPlayId;
    btns[1].hIcon = _icoPlay;
    wcscpy_s(btns[1].szTip, L"Play");
    btns[1].dwFlags = THBF_ENABLED;

    btns[2].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    btns[2].iId = ThumbNextId;
    btns[2].hIcon = _icoNext;
    wcscpy_s(btns[2].szTip, L"Next");
    btns[2].dwFlags = THBF_ENABLED;

    if (SUCCEEDED(_tb->ThumbBarAddButtons(_hwnd, 3, btns))) _buttonsAdded = true;
}

void TaskbarController::updateProgress(bool active, bool paused, int64_t posMs, int64_t lenMs) {
    if (!_tb || !_hwnd) return;
    if (!active || lenMs <= 0) {
        _tb->SetProgressState(_hwnd, TBPF_NOPROGRESS);
        return;
    }
    _tb->SetProgressState(_hwnd, paused ? TBPF_PAUSED : TBPF_NORMAL);
    ULONGLONG cur = static_cast<ULONGLONG>(std::clamp<int64_t>(posMs, 0, lenMs));
    _tb->SetProgressValue(_hwnd, cur, static_cast<ULONGLONG>(lenMs));
}

void TaskbarController::updatePlayState(bool playing) {
    if (!_tb || !_hwnd || !_buttonsAdded) return;
    THUMBBUTTON btn{};
    btn.dwMask = THB_ICON | THB_TOOLTIP;
    btn.iId = ThumbPlayId;
    btn.hIcon = playing ? _icoPause : _icoPlay;
    wcscpy_s(btn.szTip, playing ? L"Pause" : L"Play");
    _tb->ThumbBarUpdateButtons(_hwnd, 1, &btn);
}

} // namespace reel
