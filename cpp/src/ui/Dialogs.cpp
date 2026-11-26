#include "ui/Dialogs.h"
#include "core/MediaFiles.h"
#include "core/StrUtil.h"
#include "core/TimeFormat.h"
#include "win/WinInterop.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace reel {

static float dialogDpiScale(HWND ref) {
    UINT dpi = ref ? GetDpiForWindow(ref) : 96;
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

static HFONT makeDialogFont(float scale) {
    int height = -static_cast<int>(std::lround(15.0f * scale));
    return CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

// Helper for simple modal Win32 dialog windows built in code (auto-scaled for Per-Monitor DPI).
struct ModalDialog {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HFONT font = nullptr;
    float scale = 1.0f;
    bool accepted = false;
    std::function<LRESULT(HWND, UINT, WPARAM, LPARAM)> handler;

    ~ModalDialog() {
        if (font) DeleteObject(font);
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        ModalDialog* d = nullptr;
        if (m == WM_NCCREATE) {
            d = static_cast<ModalDialog*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
            d->hwnd = h;
        } else {
            d = reinterpret_cast<ModalDialog*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (d && d->handler) {
            LRESULT res = d->handler(h, m, w, l);
            if (res != -1) return res;
        }
        if (m == WM_CLOSE) {
            DestroyWindow(h);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    bool create(HWND own, const wchar_t* title, int w, int h) {
        owner = own;
        scale = dialogDpiScale(owner);
        static bool reg = false;
        const wchar_t* cls = L"ReelModalDialog";
        if (!reg) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &ModalDialog::proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.lpszClassName = cls;
            RegisterClassExW(&wc);
            reg = true;
        }
        int sw = static_cast<int>(std::lround(w * scale));
        int sh = static_cast<int>(std::lround(h * scale));
        RECT orc{};
        if (owner) GetWindowRect(owner, &orc);
        else SystemParametersInfoW(SPI_GETWORKAREA, 0, &orc, 0);
        int x = orc.left + ((orc.right - orc.left) - sw) / 2;
        int y = orc.top + ((orc.bottom - orc.top) - sh) / 2;

        hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, title,
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
            x, y, sw, sh, owner, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd) return false;
        WinInterop::setDarkTitleBar(hwnd, true);
        return true;
    }

    bool runModal() {
        font = makeDialogFont(scale);
        struct ScaleCtx { HWND parent; HFONT font; float scale; };
        ScaleCtx ctx{ hwnd, font, scale };
        EnumChildWindows(hwnd, [](HWND c, LPARAM lp) -> BOOL {
            auto* sc = reinterpret_cast<ScaleCtx*>(lp);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(sc->font), TRUE);
            if (std::abs(sc->scale - 1.0f) > 0.01f) {
                RECT r{};
                GetWindowRect(c, &r);
                POINT pt{ r.left, r.top };
                ScreenToClient(sc->parent, &pt);
                int cw = r.right - r.left;
                int ch = r.bottom - r.top;
                MoveWindow(c,
                    static_cast<int>(std::lround(pt.x * sc->scale)),
                    static_cast<int>(std::lround(pt.y * sc->scale)),
                    static_cast<int>(std::lround(cw * sc->scale)),
                    static_cast<int>(std::lround(ch * sc->scale)),
                    TRUE);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

        if (owner) EnableWindow(owner, FALSE);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        MSG msg{};
        while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                break;
            }
            if (!IsDialogMessageW(hwnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (owner) {
            EnableWindow(owner, TRUE);
            SetForegroundWindow(owner);
        }
        return accepted;
    }
};

// ============================================================================
// Standard file / folder pickers
// ============================================================================

std::vector<std::wstring> Dialogs::openFiles(HWND owner, const std::wstring& initDir, bool multiple) {
    std::wstring filter = MediaFiles::win32MediaFilter();
    std::wstring buf(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
    ofn.lpstrTitle = multiple ? L"Select one or more files to open" : L"Select a file to open";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | (multiple ? OFN_ALLOWMULTISELECT : 0);
    if (!GetOpenFileNameW(&ofn)) return {};

    std::vector<std::wstring> out;
    const wchar_t* p = buf.c_str();
    std::wstring first(p);
    p += first.size() + 1;
    if (*p == L'\0') {
        out.push_back(first);
    } else {
        while (*p) {
            std::wstring name(p);
            out.push_back(first + L"\\" + name);
            p += name.size() + 1;
        }
    }
    return out;
}

std::optional<std::wstring> Dialogs::openFolder(HWND owner, const std::wstring& initDir) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg)
        return std::nullopt;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Open folder");
    if (!initDir.empty()) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initDir.c_str(), nullptr, IID_PPV_ARGS(&si))) && si) {
            dlg->SetFolder(si);
            si->Release();
        }
    }
    std::optional<std::wstring> result;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

std::optional<std::wstring> Dialogs::openSubtitle(HWND owner, const std::wstring& initDir) {
    std::wstring filter = MediaFiles::win32SubtitleFilter();
    std::wstring buf(4096, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
    ofn.lpstrTitle = L"Open subtitle file";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return std::wstring(buf.c_str());
}

std::optional<std::wstring> Dialogs::savePlaylist(HWND owner, const std::wstring& initDir) {
    std::wstring filter;
    filter += L"M3U8 playlist (*.m3u8)"; filter.push_back(L'\0'); filter += L"*.m3u8"; filter.push_back(L'\0');
    filter += L"M3U playlist (*.m3u)"; filter.push_back(L'\0'); filter += L"*.m3u"; filter.push_back(L'\0');
    filter += L"XSPF playlist (*.xspf)"; filter.push_back(L'\0'); filter += L"*.xspf"; filter.push_back(L'\0');
    filter.push_back(L'\0');

    std::wstring buf(4096, L'\0');
    wcscpy_s(buf.data(), buf.size(), L"playlist.m3u8");
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
    ofn.lpstrDefExt = L"m3u8";
    ofn.lpstrTitle = L"Save playlist";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    return std::wstring(buf.c_str());
}

// ============================================================================
// Open Network / Disc / Capture / Jump to Time
// ============================================================================

std::optional<MediaItemPtr> Dialogs::showOpenNetwork(HWND owner) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"Open Network Stream", 500, 175)) return std::nullopt;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    CreateWindowW(L"STATIC", L"Enter a network URL (http://, https://, rtsp://, rtmp://, udp://, smb://):",
                  WS_CHILD | WS_VISIBLE, 16, 16, 450, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                16, 42, 452, 26, dlg.hwnd, reinterpret_cast<HMENU>(101), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Play", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                  294, 90, 82, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                  386, 90, 82, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDCANCEL), hi, nullptr);

    // Prefill from clipboard if it looks like a URL
    if (OpenClipboard(dlg.hwnd)) {
        HANDLE hd = GetClipboardData(CF_UNICODETEXT);
        if (hd) {
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(hd))) {
                std::wstring clip = trim(p);
                if (clip.find(L"://") != std::wstring::npos && clip.find(L'\n') == std::wstring::npos)
                    SetWindowTextW(edit, clip.c_str());
                GlobalUnlock(hd);
            }
        }
        CloseClipboard();
    }

    std::wstring url;
    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && LOWORD(w) == IDOK) {
            wchar_t buf[2048] = {};
            GetWindowTextW(edit, buf, 2048);
            url = trim(buf);
            if (url.find(L"://") == std::wstring::npos && !url.starts_with(L"\\\\")) {
                MessageBoxW(h, L"Enter a URL with a scheme (e.g. https:// or rtsp://).", L"Open Network Stream", MB_OK | MB_ICONWARNING);
                return 0;
            }
            dlg.accepted = true;
            DestroyWindow(h);
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        return -1;
    };
    SetFocus(edit);
    if (!dlg.runModal() || url.empty()) return std::nullopt;
    return std::make_shared<MediaItem>(url);
}

std::optional<MediaItemPtr> Dialogs::showOpenDisc(HWND owner) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"Open Disc", 460, 220)) return std::nullopt;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    HWND rDvd = CreateWindowW(L"BUTTON", L"DVD", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, 16, 16, 80, 22, dlg.hwnd, reinterpret_cast<HMENU>(201), hi, nullptr);
    HWND rBd = CreateWindowW(L"BUTTON", L"Blu-ray", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 110, 16, 90, 22, dlg.hwnd, reinterpret_cast<HMENU>(202), hi, nullptr);
    HWND rCd = CreateWindowW(L"BUTTON", L"Audio CD", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 215, 16, 100, 22, dlg.hwnd, reinterpret_cast<HMENU>(203), hi, nullptr);
    SendMessageW(rDvd, BM_SETCHECK, BST_CHECKED, 0);

    CreateWindowW(L"STATIC", L"Drive, folder (VIDEO_TS / BDMV) or ISO:", WS_CHILD | WS_VISIBLE, 16, 48, 300, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"D:\\", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 72, 340, 26, dlg.hwnd, reinterpret_cast<HMENU>(101), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 364, 72, 72, 26, dlg.hwnd, reinterpret_cast<HMENU>(102), hi, nullptr);
    HWND noMenus = CreateWindowW(L"BUTTON", L"No disc menus", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 108, 160, 22, dlg.hwnd, reinterpret_cast<HMENU>(103), hi, nullptr);

    CreateWindowW(L"BUTTON", L"Play", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 260, 142, 84, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 352, 142, 84, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDCANCEL), hi, nullptr);

    MediaItemPtr result;
    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && LOWORD(w) == 102) {
            if (auto folder = openFolder(h, L"")) SetWindowTextW(edit, folder->c_str());
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(w) == IDOK) {
            wchar_t buf[1024] = {};
            GetWindowTextW(edit, buf, 1024);
            std::wstring p = trim(buf);
            if (p.empty()) return 0;
            std::wstring target = p;
            std::replace(target.begin(), target.end(), L'\\', L'/');
            if (!target.ends_with(L'/') && !endsWithIgnoreCase(target, L".iso")) target += L"/";
            bool nm = SendMessageW(noMenus, BM_GETCHECK, 0, 0) == BST_CHECKED;
            std::wstring mrl, name;
            if (SendMessageW(rCd, BM_GETCHECK, 0, 0) == BST_CHECKED) { mrl = L"cdda:///" + target; name = L"Audio CD"; }
            else if (SendMessageW(rBd, BM_GETCHECK, 0, 0) == BST_CHECKED) { mrl = L"bluray:///" + target; name = L"Blu-ray"; }
            else { mrl = (nm ? L"dvdsimple:///" : L"dvd:///") + target; name = L"DVD"; }
            result = std::make_shared<MediaItem>(mrl, name + L" (" + p + L")");
            if (SendMessageW(rBd, BM_GETCHECK, 0, 0) == BST_CHECKED && nm) result->options.push_back(L":no-bluray-menu");
            dlg.accepted = true;
            DestroyWindow(h);
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        return -1;
    };
    if (!dlg.runModal() || !result) return std::nullopt;
    return result;
}

std::optional<MediaItemPtr> Dialogs::showOpenCapture(HWND owner) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"Open Capture Device", 440, 210)) return std::nullopt;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    HWND rDshow = CreateWindowW(L"BUTTON", L"DirectShow device", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, 16, 16, 160, 22, dlg.hwnd, reinterpret_cast<HMENU>(201), hi, nullptr);
    HWND rDesk = CreateWindowW(L"BUTTON", L"Desktop screen", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 190, 16, 140, 22, dlg.hwnd, reinterpret_cast<HMENU>(202), hi, nullptr);
    SendMessageW(rDshow, BM_SETCHECK, BST_CHECKED, 0);

    CreateWindowW(L"STATIC", L"Video device (or FPS for desktop):", WS_CHILD | WS_VISIBLE, 16, 48, 260, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND vdev = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 70, 392, 24, dlg.hwnd, reinterpret_cast<HMENU>(101), hi, nullptr);
    CreateWindowW(L"STATIC", L"Audio device (DirectShow):", WS_CHILD | WS_VISIBLE, 16, 100, 260, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND adev = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 122, 392, 24, dlg.hwnd, reinterpret_cast<HMENU>(102), hi, nullptr);

    CreateWindowW(L"BUTTON", L"Play", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 240, 152, 80, 26, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 328, 152, 80, 26, dlg.hwnd, reinterpret_cast<HMENU>(IDCANCEL), hi, nullptr);

    MediaItemPtr result;
    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && LOWORD(w) == IDOK) {
            wchar_t vb[256] = {}, ab[256] = {};
            GetWindowTextW(vdev, vb, 256); GetWindowTextW(adev, ab, 256);
            if (SendMessageW(rDesk, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                result = std::make_shared<MediaItem>(L"screen://", L"Desktop");
                std::wstring fps = trim(vb);
                result->options.push_back(L":screen-fps=" + (fps.empty() ? L"30" : fps));
            } else {
                std::wstring vn = trim(vb);
                result = std::make_shared<MediaItem>(L"dshow://", vn.empty() ? L"Capture device" : vn);
                result->options.push_back(L":dshow-vdev=" + vn);
                result->options.push_back(L":dshow-adev=" + trim(ab));
                result->options.push_back(L":live-caching=300");
            }
            dlg.accepted = true;
            DestroyWindow(h);
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        return -1;
    };
    if (!dlg.runModal() || !result) return std::nullopt;
    return result;
}

std::optional<int64_t> Dialogs::showJumpToTime(HWND owner, int64_t curMs, int64_t lenMs) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"Jump to Time", 340, 160)) return std::nullopt;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    std::wstring prompt = L"Go to time (e.g. 1:23:45, 12:30, or 90) — Length: " + TimeFormat::format(lenMs);
    CreateWindowW(L"STATIC", prompt.c_str(), WS_CHILD | WS_VISIBLE, 16, 14, 300, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", TimeFormat::format(curMs).c_str(),
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 40, 294, 26, dlg.hwnd, reinterpret_cast<HMENU>(101), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Go", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 140, 82, 80, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 230, 82, 80, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDCANCEL), hi, nullptr);

    int64_t target = 0;
    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && LOWORD(w) == IDOK) {
            wchar_t buf[64] = {};
            GetWindowTextW(edit, buf, 64);
            if (!TimeFormat::tryParse(buf, target)) {
                MessageBoxW(h, L"Enter a valid time like 1:23:45, 12:30 or 90.", L"Jump to Time", MB_OK | MB_ICONWARNING);
                return 0;
            }
            dlg.accepted = true;
            DestroyWindow(h);
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        return -1;
    };
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    if (!dlg.runModal()) return std::nullopt;
    return target;
}

// ============================================================================
// Effects & Filters (Equalizer + Video Adjust + Rotate/Sharpen)
// ============================================================================

void Dialogs::showEffects(HWND owner, EffectsState& state,
                          std::function<void()> onEqChanged,
                          std::function<void()> onAdjustChanged,
                          std::function<void()> onFiltersChanged) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"Effects and Filters", 600, 440)) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);

    // Equalizer section
    HWND eqChk = CreateWindowW(L"BUTTON", L"Enable 10-band Equalizer", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 12, 200, 22, dlg.hwnd, reinterpret_cast<HMENU>(301), hi, nullptr);
    SendMessageW(eqChk, BM_SETCHECK, state.eqEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    CreateWindowW(L"STATIC", L"Preset:", WS_CHILD | WS_VISIBLE, 230, 15, 50, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND presetBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 282, 11, 200, 220, dlg.hwnd, reinterpret_cast<HMENU>(302), hi, nullptr);
    SendMessageW(presetBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(custom)"));
    for (const auto& p : PlayerService::equalizerPresets()) SendMessageW(presetBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.c_str()));
    SendMessageW(presetBox, CB_SETCURSEL, state.eqPreset + 1, 0);

    HWND preampSlider = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_VERT, 18, 42, 34, 110, dlg.hwnd, reinterpret_cast<HMENU>(310), hi, nullptr);
    SendMessageW(preampSlider, TBM_SETRANGE, TRUE, MAKELPARAM(-200, 200));
    SendMessageW(preampSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(-state.eqPreamp * 10));
    CreateWindowW(L"STATIC", L"Pre", WS_CHILD | WS_VISIBLE | SS_CENTER, 16, 154, 38, 18, dlg.hwnd, nullptr, hi, nullptr);

    auto labels = PlayerService::equalizerBandLabels();
    HWND bandSliders[10]{};
    for (int i = 0; i < 10; ++i) {
        int x = 70 + i * 50;
        bandSliders[i] = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_VERT, x, 42, 34, 110, dlg.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(320 + i)), hi, nullptr);
        SendMessageW(bandSliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(-200, 200));
        SendMessageW(bandSliders[i], TBM_SETPOS, TRUE, static_cast<LPARAM>(-state.eqBands[i] * 10));
        std::wstring lbl = i < static_cast<int>(labels.size()) ? labels[i] : L"";
        CreateWindowW(L"STATIC", lbl.c_str(), WS_CHILD | WS_VISIBLE | SS_CENTER, x - 4, 154, 42, 18, dlg.hwnd, nullptr, hi, nullptr);
    }

    // Video Adjust section
    HWND adjChk = CreateWindowW(L"BUTTON", L"Image Adjust", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 184, 140, 22, dlg.hwnd, reinterpret_cast<HMENU>(401), hi, nullptr);
    SendMessageW(adjChk, BM_SETCHECK, state.adjustEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    auto makeHSlider = [&](int y, const wchar_t* name, int id, int mn, int mx, int cur) {
        CreateWindowW(L"STATIC", name, WS_CHILD | WS_VISIBLE, 16, y + 3, 76, 20, dlg.hwnd, nullptr, hi, nullptr);
        HWND s = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ, 94, y, 220, 26, dlg.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi, nullptr);
        SendMessageW(s, TBM_SETRANGE, TRUE, MAKELPARAM(mn, mx));
        SendMessageW(s, TBM_SETPOS, TRUE, cur);
        return s;
    };
    HWND sBri = makeHSlider(212, L"Brightness:", 410, 0, 200, static_cast<int>(state.brightness * 100));
    HWND sCon = makeHSlider(240, L"Contrast:", 411, 0, 200, static_cast<int>(state.contrast * 100));
    HWND sHue = makeHSlider(268, L"Hue:", 412, -180, 180, static_cast<int>(state.hue));
    HWND sSat = makeHSlider(296, L"Saturation:", 413, 0, 300, static_cast<int>(state.saturation * 100));
    HWND sGam = makeHSlider(324, L"Gamma:", 414, 10, 500, static_cast<int>(state.gamma * 100));

    // Geometry / Sharpen
    CreateWindowW(L"STATIC", L"Transform:", WS_CHILD | WS_VISIBLE, 340, 188, 80, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND tfBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 340, 210, 220, 200, dlg.hwnd, reinterpret_cast<HMENU>(501), hi, nullptr);
    static constexpr const wchar_t* tfNames[] = { L"None", L"Rotate 90°", L"Rotate 180°", L"Rotate 270°", L"Flip horizontally", L"Flip vertically", L"Transpose", L"Anti-transpose" };
    for (auto* n : tfNames) SendMessageW(tfBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
    SendMessageW(tfBox, CB_SETCURSEL, state.transformIndex, 0);

    HWND shpChk = CreateWindowW(L"BUTTON", L"Sharpen", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 340, 248, 120, 22, dlg.hwnd, reinterpret_cast<HMENU>(502), hi, nullptr);
    SendMessageW(shpChk, BM_SETCHECK, state.sharpen ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND shpSlider = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ, 340, 274, 220, 26, dlg.hwnd, reinterpret_cast<HMENU>(503), hi, nullptr);
    SendMessageW(shpSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendMessageW(shpSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(state.sharpenSigma * 100));
    CreateWindowW(L"BUTTON", L"Apply Filters", WS_CHILD | WS_VISIBLE, 340, 312, 110, 28, dlg.hwnd, reinterpret_cast<HMENU>(504), hi, nullptr);

    CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 480, 362, 86, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);

    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if (m == WM_COMMAND) {
            int id = LOWORD(w);
            if (id == IDOK || id == IDCANCEL) { DestroyWindow(h); return 0; }
            if (id == 301) { state.eqEnabled = SendMessageW(eqChk, BM_GETCHECK, 0, 0) == BST_CHECKED; if (onEqChanged) onEqChanged(); return 0; }
            if (id == 302 && HIWORD(w) == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(presetBox, CB_GETCURSEL, 0, 0)) - 1;
                if (sel >= 0) {
                    state.eqPreset = sel;
                    PlayerService::loadEqualizerPresetValues(static_cast<unsigned>(sel), state.eqPreamp, state.eqBands);
                    SendMessageW(preampSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(-state.eqPreamp * 10));
                    for (int i = 0; i < 10; ++i) SendMessageW(bandSliders[i], TBM_SETPOS, TRUE, static_cast<LPARAM>(-state.eqBands[i] * 10));
                    state.eqEnabled = true;
                    SendMessageW(eqChk, BM_SETCHECK, BST_CHECKED, 0);
                    if (onEqChanged) onEqChanged();
                }
                return 0;
            }
            if (id == 401) { state.adjustEnabled = SendMessageW(adjChk, BM_GETCHECK, 0, 0) == BST_CHECKED; if (onAdjustChanged) onAdjustChanged(); return 0; }
            if (id == 504) {
                state.transformIndex = std::max(0, static_cast<int>(SendMessageW(tfBox, CB_GETCURSEL, 0, 0)));
                state.sharpen = SendMessageW(shpChk, BM_GETCHECK, 0, 0) == BST_CHECKED;
                state.sharpenSigma = SendMessageW(shpSlider, TBM_GETPOS, 0, 0) / 100.0;
                if (onFiltersChanged) onFiltersChanged();
                return 0;
            }
        }
        if (m == WM_VSCROLL || m == WM_HSCROLL) {
            HWND src = reinterpret_cast<HWND>(l);
            if (src == preampSlider) { state.eqPreamp = -SendMessageW(preampSlider, TBM_GETPOS, 0, 0) / 10.0f; if (onEqChanged) onEqChanged(); return 0; }
            for (int i = 0; i < 10; ++i) {
                if (src == bandSliders[i]) {
                    state.eqBands[i] = -SendMessageW(bandSliders[i], TBM_GETPOS, 0, 0) / 10.0f;
                    state.eqPreset = -1;
                    SendMessageW(presetBox, CB_SETCURSEL, 0, 0);
                    if (onEqChanged) onEqChanged();
                    return 0;
                }
            }
            if (src == sBri || src == sCon || src == sHue || src == sSat || src == sGam) {
                state.brightness = SendMessageW(sBri, TBM_GETPOS, 0, 0) / 100.0f;
                state.contrast = SendMessageW(sCon, TBM_GETPOS, 0, 0) / 100.0f;
                state.hue = static_cast<float>(SendMessageW(sHue, TBM_GETPOS, 0, 0));
                state.saturation = SendMessageW(sSat, TBM_GETPOS, 0, 0) / 100.0f;
                state.gamma = SendMessageW(sGam, TBM_GETPOS, 0, 0) / 100.0f;
                if (onAdjustChanged) onAdjustChanged();
                return 0;
            }
        }
        return -1;
    };
    dlg.runModal();
}

// ============================================================================
// Sync / MediaInfo / Messages / Preferences / Shortcuts / About
// ============================================================================

void Dialogs::showSync(HWND owner, PlayerService& engine,
                       std::function<void(int64_t)> onAudioDelayMs,
                       std::function<void(int64_t)> onSubDelayMs) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"Track Synchronization", 360, 190)) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    CreateWindowW(L"STATIC", L"Audio delay (ms):", WS_CHILD | WS_VISIBLE, 16, 20, 140, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND aEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(engine.audioDelayUs() / 1000).c_str(),
                                 WS_CHILD | WS_VISIBLE, 165, 16, 150, 26, dlg.hwnd, reinterpret_cast<HMENU>(101), hi, nullptr);
    CreateWindowW(L"STATIC", L"Subtitle delay (ms):", WS_CHILD | WS_VISIBLE, 16, 56, 140, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND sEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(engine.spuDelayUs() / 1000).c_str(),
                                 WS_CHILD | WS_VISIBLE, 165, 52, 150, 26, dlg.hwnd, reinterpret_cast<HMENU>(102), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 150, 106, 80, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE, 238, 106, 80, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDCANCEL), hi, nullptr);

    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && LOWORD(w) == IDOK) {
            wchar_t ab[64] = {}, sb[64] = {};
            GetWindowTextW(aEdit, ab, 64); GetWindowTextW(sEdit, sb, 64);
            if (onAudioDelayMs) onAudioDelayMs(std::wcstoll(ab, nullptr, 10));
            if (onSubDelayMs) onSubDelayMs(std::wcstoll(sb, nullptr, 10));
            DestroyWindow(h);
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        return -1;
    };
    dlg.runModal();
}

void Dialogs::showMediaInfo(HWND owner, PlayerService& engine) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"Media & Codec Information", 540, 420)) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    std::wostringstream ss;
    auto item = engine.currentItem();
    ss << L"Title: " << (item ? item->title : L"(none)") << L"\r\n";
    ss << L"Location: " << (item ? item->location : L"(none)") << L"\r\n";
    if (auto a = engine.meta(libvlc_meta_Artist); !a.empty()) ss << L"Artist: " << a << L"\r\n";
    if (auto al = engine.meta(libvlc_meta_Album); !al.empty()) ss << L"Album: " << al << L"\r\n";
    ss << L"\r\n--- Streams / Codecs ---\r\n";
    for (const auto& t : engine.mediaTracks()) {
        ss << L"[" << t.type << L"] " << t.codec;
        if (!t.details.empty()) ss << L" — " << t.details;
        if (!t.language.empty()) ss << L" (" << t.language << L")";
        ss << L"\r\n";
    }
    auto st = engine.mediaStats();
    ss << L"\r\n--- Statistics ---\r\n";
    ss << L"Video decoded: " << st.decodedVideo << L"  |  Displayed: " << st.displayedPictures << L"  |  Lost: " << st.lostPictures << L"\r\n";
    ss << L"Audio decoded: " << st.decodedAudio << L"  |  Played: " << st.playedAbuffers << L"  |  Lost: " << st.lostAbuffers << L"\r\n";

    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ss.str().c_str(),
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
        14, 14, 496, 316, dlg.hwnd, nullptr, hi, nullptr);
    CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 426, 342, 84, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
    (void)edit;
    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) { DestroyWindow(h); return 0; }
        return -1;
    };
    dlg.runModal();
}

void Dialogs::showMessages(HWND owner, LogBuffer& log) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"Messages (libVLC Log)", 640, 440)) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    std::wostringstream ss;
    for (const auto& e : log.snapshot()) {
        const wchar_t* lvl = e.level >= LIBVLC_ERROR ? L"ERR" : (e.level == LIBVLC_WARNING ? L"WRN" : (e.level == LIBVLC_NOTICE ? L"INF" : L"DBG"));
        ss << e.timeStr << L" [" << lvl << L"] " << e.module << L": " << e.message << L"\r\n";
    }
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ss.str().c_str(),
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL,
        14, 14, 596, 336, dlg.hwnd, reinterpret_cast<HMENU>(101), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE, 14, 360, 80, 28, dlg.hwnd, reinterpret_cast<HMENU>(102), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 526, 360, 84, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);

    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && LOWORD(w) == 102) { log.clear(); SetDlgItemTextW(h, 101, L""); return 0; }
        if (m == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) { DestroyWindow(h); return 0; }
        return -1;
    };
    dlg.runModal();
}

bool Dialogs::showPreferences(HWND owner, SettingsService& settings, bool& outRestartRequired) {
    outRestartRequired = false;
    ModalDialog dlg;
    if (!dlg.create(owner, L"Preferences", 500, 430)) return false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    auto& s = settings.current();

    CreateWindowW(L"STATIC", L"Theme:", WS_CHILD | WS_VISIBLE, 16, 18, 90, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND themeBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 14, 140, 120, dlg.hwnd, reinterpret_cast<HMENU>(101), hi, nullptr);
    SendMessageW(themeBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dark"));
    SendMessageW(themeBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Light"));
    SendMessageW(themeBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"System"));
    SendMessageW(themeBox, CB_SETCURSEL, s.theme == L"Light" ? 1 : (s.theme == L"System" ? 2 : 0), 0);

    auto mkChk = [&](int y, const wchar_t* text, int id, bool val) {
        HWND c = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, y, 450, 22, dlg.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi, nullptr);
        SendMessageW(c, BM_SETCHECK, val ? BST_CHECKED : BST_UNCHECKED, 0);
        return c;
    };
    HWND cOne = mkChk(48, L"Allow only one instance (restart required)", 201, s.oneInstance);
    HWND cRes = mkChk(74, L"Continue playback where I left off (files > 5 min)", 203, s.resumePlayback);
    HWND cMin = mkChk(100, L"Pause playback when minimized", 204, s.pauseOnMinimize);
    HWND cAdv = mkChk(126, L"Step to next/previous media file in the same folder", 205, s.autoAdvanceFolder);
    HWND cFs  = mkChk(152, L"Start in fullscreen", 206, s.startFullscreen);
    HWND cHw  = mkChk(184, L"Hardware-accelerated decoding (DXVA2 / D3D11, restart required)", 207, s.hardwareDecoding);
    HWND cSub = mkChk(210, L"Automatically load subtitle files next to the video", 208, s.autoLoadSubtitles);

    CreateWindowW(L"STATIC", L"Snapshot format:", WS_CHILD | WS_VISIBLE, 16, 268, 110, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND fmtBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 130, 264, 90, 90, dlg.hwnd, reinterpret_cast<HMENU>(209), hi, nullptr);
    SendMessageW(fmtBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"png"));
    SendMessageW(fmtBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"jpg"));
    SendMessageW(fmtBox, CB_SETCURSEL, s.snapshotFormat == L"jpg" ? 1 : 0, 0);

    CreateWindowW(L"BUTTON", L"Register File Associations...", WS_CHILD | WS_VISIBLE, 16, 304, 190, 28, dlg.hwnd, reinterpret_cast<HMENU>(301), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Remove Registration", WS_CHILD | WS_VISIBLE, 216, 304, 150, 28, dlg.hwnd, reinterpret_cast<HMENU>(302), hi, nullptr);

    CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 294, 352, 86, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 388, 352, 86, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDCANCEL), hi, nullptr);

    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && LOWORD(w) == 301) {
            WinInterop::registerFileAssociations();
            WinInterop::openDefaultAppsSettings();
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(w) == 302) {
            WinInterop::unregisterFileAssociations();
            MessageBoxW(h, L"Reel Player's file type registration was removed.", L"File Associations", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(w) == IDOK) {
            auto beforeOpts = PlayerService::buildOptions(s);
            bool beforeOne = s.oneInstance;
            int ti = static_cast<int>(SendMessageW(themeBox, CB_GETCURSEL, 0, 0));
            s.theme = ti == 1 ? L"Light" : (ti == 2 ? L"System" : L"Dark");
            s.oneInstance = SendMessageW(cOne, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s.resumePlayback = SendMessageW(cRes, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s.pauseOnMinimize = SendMessageW(cMin, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s.autoAdvanceFolder = SendMessageW(cAdv, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s.startFullscreen = SendMessageW(cFs, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s.hardwareDecoding = SendMessageW(cHw, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s.autoLoadSubtitles = SendMessageW(cSub, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s.snapshotFormat = SendMessageW(fmtBox, CB_GETCURSEL, 0, 0) == 1 ? L"jpg" : L"png";
            outRestartRequired = (beforeOpts != PlayerService::buildOptions(s)) || (s.oneInstance != beforeOne);
            dlg.accepted = true;
            DestroyWindow(h);
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        return -1;
    };
    return dlg.runModal();
}

void Dialogs::showShortcuts(HWND owner, const HotkeyMap& hotkeys) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"Keyboard Shortcuts", 480, 440)) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    std::wostringstream ss;
    for (const auto& a : HotkeyActions::all()) {
        auto g = hotkeys.gesturesFor(a.id);
        std::string joined;
        for (size_t i = 0; i < g.size(); ++i) { if (i) joined += " | "; joined += g[i]; }
        ss << toWide(a.description) << L"  —  " << toWide(joined) << L"\r\n";
    }
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ss.str().c_str(),
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
        14, 14, 436, 336, dlg.hwnd, nullptr, hi, nullptr);
    CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 366, 360, 84, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) { DestroyWindow(h); return 0; }
        return -1;
    };
    dlg.runModal();
}

void Dialogs::showAbout(HWND owner) {
    std::wstring msg = L"Reel Player 1.0.0 (Native C++20 + Win32 + Direct2D)\n\n"
                       L"Playback engine: libVLC " + PlayerService::vlcVersion() + L" (LGPL-2.1-or-later)\n"
                       L"Not affiliated with VideoLAN.";
    MessageBoxW(owner, msg.c_str(), L"About Reel Player", MB_OK | MB_ICONINFORMATION);
}

} // namespace reel
