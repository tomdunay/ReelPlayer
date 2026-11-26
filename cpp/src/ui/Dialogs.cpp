#include "ui/Dialogs.h"
#include "core/MediaFiles.h"
#include "core/StrUtil.h"
#include "core/TimeFormat.h"
#include "win/WinInterop.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <uxtheme.h>
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
    static constexpr COLORREF kBgColor = RGB(24, 24, 31);
    static constexpr COLORREF kCtrlBgColor = RGB(34, 34, 44);
    static constexpr COLORREF kTextColor = RGB(244, 244, 246);

    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND keyCaptureHwnd = nullptr;
    HFONT font = nullptr;
    HBRUSH bgBrush = nullptr;
    HBRUSH ctrlBgBrush = nullptr;
    float scale = 1.0f;
    bool accepted = false;
    std::function<LRESULT(HWND, UINT, WPARAM, LPARAM)> handler;

    ~ModalDialog() {
        if (font) DeleteObject(font);
        if (bgBrush) DeleteObject(bgBrush);
        if (ctrlBgBrush) DeleteObject(ctrlBgBrush);
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
        if (d) {
            if (m == WM_ERASEBKGND && d->bgBrush) {
                HDC hdc = reinterpret_cast<HDC>(w);
                RECT rc{};
                GetClientRect(h, &rc);
                FillRect(hdc, &rc, d->bgBrush);
                return 1;
            }
            if (m == WM_CTLCOLORDLG && d->bgBrush) {
                return reinterpret_cast<LRESULT>(d->bgBrush);
            }
            if ((m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN) && d->bgBrush) {
                HDC hdc = reinterpret_cast<HDC>(w);
                HWND ctrl = reinterpret_cast<HWND>(l);
                wchar_t cls[32] = {};
                if (ctrl) GetClassNameW(ctrl, cls, 32);
                bool isEdit = (_wcsicmp(cls, L"Edit") == 0);
                SetTextColor(hdc, kTextColor);
                SetBkMode(hdc, OPAQUE);
                SetBkColor(hdc, isEdit ? kCtrlBgColor : kBgColor);
                return reinterpret_cast<LRESULT>(isEdit && d->ctrlBgBrush ? d->ctrlBgBrush : d->bgBrush);
            }
            if ((m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX) && d->ctrlBgBrush) {
                HDC hdc = reinterpret_cast<HDC>(w);
                SetTextColor(hdc, kTextColor);
                SetBkMode(hdc, OPAQUE);
                SetBkColor(hdc, kCtrlBgColor);
                return reinterpret_cast<LRESULT>(d->ctrlBgBrush);
            }
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
        bgBrush = CreateSolidBrush(kBgColor);
        ctrlBgBrush = CreateSolidBrush(kCtrlBgColor);
        static bool reg = false;
        const wchar_t* cls = L"ReelModalDialog";
        if (!reg) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &ModalDialog::proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
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
            wchar_t cls[32] = {};
            GetClassNameW(c, cls, 32);
            if (_wcsicmp(cls, L"Button") == 0) {
                LONG st = GetWindowLongW(c, GWL_STYLE) & BS_TYPEMASK;
                if (st == BS_CHECKBOX || st == BS_AUTOCHECKBOX || st == BS_3STATE || st == BS_AUTO3STATE ||
                    st == BS_RADIOBUTTON || st == BS_AUTORADIOBUTTON) {
                    SetWindowTheme(c, L"", L"");
                } else {
                    SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
                }
            } else {
                SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
            }
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
            if (keyCaptureHwnd && GetFocus() == keyCaptureHwnd &&
                (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) {
                SendMessageW(keyCaptureHwnd, msg.message, msg.wParam, msg.lParam);
                continue;
            }
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

static bool showHotkeyEditor(HWND owner, std::map<std::string, std::string>& overrides) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"Configure Hotkeys", 560, 440)) return false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    const auto& actions = HotkeyActions::all();
    std::map<std::string, std::string> draft = overrides;

    auto defaultJoined = [](const HotkeyAction& a) {
        std::string out;
        for (size_t i = 0; i < a.defaultGestures.size(); ++i) {
            if (i) out += "|";
            out += a.defaultGestures[i];
        }
        return out;
    };
    auto currentJoined = [&](const HotkeyAction& a) {
        auto it = draft.find(a.id);
        return it != draft.end() ? it->second : defaultJoined(a);
    };

    CreateWindowW(L"STATIC", L"Select an action, click the shortcut box, and press a key combo (Backspace to clear):",
                  WS_CHILD | WS_VISIBLE, 14, 12, 520, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_USETABSTOPS | WS_TABSTOP,
        14, 38, 518, 250, dlg.hwnd, reinterpret_cast<HMENU>(101), hi, nullptr);
    INT tabStop = 190;
    SendMessageW(list, LB_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(&tabStop));

    CreateWindowW(L"STATIC", L"Shortcut:", WS_CHILD | WS_VISIBLE, 14, 304, 70, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND capture = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP,
        88, 300, 240, 26, dlg.hwnd, reinterpret_cast<HMENU>(102), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Reset Action", WS_CHILD | WS_VISIBLE, 336, 299, 96, 28, dlg.hwnd, reinterpret_cast<HMENU>(103), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Reset All", WS_CHILD | WS_VISIBLE, 438, 299, 94, 28, dlg.hwnd, reinterpret_cast<HMENU>(104), hi, nullptr);

    CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 354, 352, 86, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 446, 352, 86, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDCANCEL), hi, nullptr);

    auto refreshList = [&](int selIdx) {
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& a : actions) {
            std::string g = currentJoined(a);
            std::wstring row = toWide(a.description) + L"\t" + (g.empty() ? L"(none)" : toWide(g));
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        }
        if (selIdx >= 0 && selIdx < static_cast<int>(actions.size()))
            SendMessageW(list, LB_SETCURSEL, selIdx, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
    };
    auto syncCapture = [&]() {
        int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(actions.size())) {
            std::string g = currentJoined(actions[sel]);
            SetWindowTextW(capture, g.empty() ? L"(none)" : toWide(g).c_str());
        } else {
            SetWindowTextW(capture, L"");
        }
    };
    refreshList(0);
    syncCapture();

    struct CaptureCtx {
        std::function<void(UINT, bool, bool, bool)> onKey;
        WNDPROC origProc = nullptr;
    };
    CaptureCtx capCtx;
    capCtx.onKey = [&](UINT vk, bool ctrl, bool alt, bool shift) {
        int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (sel < 0 || sel >= static_cast<int>(actions.size())) return;
        const auto& act = actions[sel];
        if ((vk == VK_BACK || vk == VK_DELETE) && !ctrl && !alt && !shift) {
            draft[act.id] = "";
            refreshList(sel);
            syncCapture();
            return;
        }
        if (auto g = HotkeyMap::gestureFromVk(vk, ctrl, alt, shift, false)) {
            if (*g == defaultJoined(act)) draft.erase(act.id);
            else draft[act.id] = *g;
            refreshList(sel);
            syncCapture();
        }
    };
    SetWindowLongPtrW(capture, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&capCtx));
    capCtx.origProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(capture, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(+[](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            auto* ctx = reinterpret_cast<CaptureCtx*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            if (ctx && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN)) {
                UINT vk = static_cast<UINT>(w);
                if (vk != VK_CONTROL && vk != VK_SHIFT && vk != VK_MENU && vk != VK_LWIN && vk != VK_RWIN) {
                    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    ctx->onKey(vk, ctrl, alt, shift);
                }
                return 0;
            }
            return ctx && ctx->origProc ? CallWindowProcW(ctx->origProc, h, m, w, l) : DefWindowProcW(h, m, w, l);
        })));
    dlg.keyCaptureHwnd = capture;

    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND) {
            int id = LOWORD(w);
            if (id == 101 && HIWORD(w) == LBN_SELCHANGE) { syncCapture(); return 0; }
            if (id == 103) {
                int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < static_cast<int>(actions.size())) {
                    draft.erase(actions[sel].id);
                    refreshList(sel);
                    syncCapture();
                }
                return 0;
            }
            if (id == 104) {
                int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
                draft.clear();
                refreshList(sel >= 0 ? sel : 0);
                syncCapture();
                return 0;
            }
            if (id == IDOK) {
                overrides = std::move(draft);
                dlg.accepted = true;
                DestroyWindow(h);
                return 0;
            }
            if (id == IDCANCEL) { DestroyWindow(h); return 0; }
        }
        return -1;
    };
    return dlg.runModal();
}

bool Dialogs::showSubtitleAppearance(HWND owner, AppSettings& s) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"Subtitle Appearance & Defaults", 450, 310)) return false;
    HINSTANCE hi = GetModuleHandleW(nullptr);

    static constexpr const wchar_t* kSizes[] = { L"Smaller", L"Small", L"Normal", L"Large", L"Larger" };
    static constexpr const wchar_t* kOutlines[] = { L"None", L"Thin", L"Normal", L"Thick" };
    static constexpr struct { const wchar_t* label; const wchar_t* hex; } kColors[] = {
        { L"White",      L"#FFFFFF" },
        { L"Yellow",     L"#FFD166" },
        { L"Cyan",       L"#67E8F9" },
        { L"Light Green",L"#86EFAC" },
        { L"Light Gray", L"#D4D4D8" },
    };

    CreateWindowW(L"STATIC", L"Font size:", WS_CHILD | WS_VISIBLE, 16, 18, 130, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND sizeBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 152, 14, 150, 160, dlg.hwnd, reinterpret_cast<HMENU>(101), hi, nullptr);
    int sizeIdx = 2;
    for (int i = 0; i < 5; ++i) {
        SendMessageW(sizeBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kSizes[i]));
        if (s.subtitleSize == kSizes[i]) sizeIdx = i;
    }
    SendMessageW(sizeBox, CB_SETCURSEL, sizeIdx, 0);

    CreateWindowW(L"STATIC", L"Text color:", WS_CHILD | WS_VISIBLE, 16, 54, 130, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND colorBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 152, 50, 150, 160, dlg.hwnd, reinterpret_cast<HMENU>(102), hi, nullptr);
    int colorIdx = 0;
    for (int i = 0; i < 5; ++i) {
        SendMessageW(colorBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kColors[i].label));
        if (_wcsicmp(s.subtitleColor.c_str(), kColors[i].hex) == 0) colorIdx = i;
    }
    SendMessageW(colorBox, CB_SETCURSEL, colorIdx, 0);

    CreateWindowW(L"STATIC", L"Outline thickness:", WS_CHILD | WS_VISIBLE, 16, 90, 130, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND outBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 152, 86, 150, 160, dlg.hwnd, reinterpret_cast<HMENU>(103), hi, nullptr);
    int outIdx = 2;
    for (int i = 0; i < 4; ++i) {
        SendMessageW(outBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kOutlines[i]));
        if (s.subtitleOutline == kOutlines[i]) outIdx = i;
    }
    SendMessageW(outBox, CB_SETCURSEL, outIdx, 0);

    CreateWindowW(L"STATIC", L"Preferred language:", WS_CHILD | WS_VISIBLE, 16, 128, 130, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND langEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", s.subtitleLanguage.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 152, 124, 260, 26, dlg.hwnd, reinterpret_cast<HMENU>(104), hi, nullptr);

    CreateWindowW(L"STATIC", L"Text encoding:", WS_CHILD | WS_VISIBLE, 16, 164, 130, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND encEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", s.subtitleEncoding.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 152, 160, 260, 26, dlg.hwnd, reinterpret_cast<HMENU>(105), hi, nullptr);

    CreateWindowW(L"STATIC", L"Tip: Leave language/encoding blank for auto-detect (e.g. en,fr / UTF-8).",
                  WS_CHILD | WS_VISIBLE, 16, 196, 405, 20, dlg.hwnd, nullptr, hi, nullptr);

    CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 240, 232, 84, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 332, 232, 84, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDCANCEL), hi, nullptr);

    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && LOWORD(w) == IDOK) {
            int si = std::clamp<int>(static_cast<int>(SendMessageW(sizeBox, CB_GETCURSEL, 0, 0)), 0, 4);
            int ci = std::clamp<int>(static_cast<int>(SendMessageW(colorBox, CB_GETCURSEL, 0, 0)), 0, 4);
            int oi = std::clamp<int>(static_cast<int>(SendMessageW(outBox, CB_GETCURSEL, 0, 0)), 0, 3);
            wchar_t lb[128] = {}, eb[128] = {};
            GetWindowTextW(langEdit, lb, 128);
            GetWindowTextW(encEdit, eb, 128);
            s.subtitleSize = kSizes[si];
            s.subtitleColor = kColors[ci].hex;
            s.subtitleOutline = kOutlines[oi];
            s.subtitleLanguage = trim(lb);
            s.subtitleEncoding = trim(eb);
            dlg.accepted = true;
            DestroyWindow(h);
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        return -1;
    };
    return dlg.runModal();
}

bool Dialogs::showPreferences(HWND owner, SettingsService& settings, bool& outRestartRequired) {
    outRestartRequired = false;
    ModalDialog dlg;
    if (!dlg.create(owner, L"Preferences", 520, 540)) return false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    auto& s = settings.current();
    auto pendingHotkeys = s.hotkeys;
    AppSettings pendingSub = s;

    CreateWindowW(L"STATIC", L"Theme:", WS_CHILD | WS_VISIBLE, 16, 18, 90, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND themeBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 110, 14, 140, 120, dlg.hwnd, reinterpret_cast<HMENU>(101), hi, nullptr);
    SendMessageW(themeBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dark"));
    SendMessageW(themeBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Light"));
    SendMessageW(themeBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"System"));
    SendMessageW(themeBox, CB_SETCURSEL, s.theme == L"Light" ? 1 : (s.theme == L"System" ? 2 : 0), 0);

    auto mkChk = [&](int y, const wchar_t* text, int id, bool val) {
        HWND c = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, y, 470, 22, dlg.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi, nullptr);
        SendMessageW(c, BM_SETCHECK, val ? BST_CHECKED : BST_UNCHECKED, 0);
        return c;
    };
    HWND cOne  = mkChk(48,  L"Allow only one instance (restart required)", 201, s.oneInstance);
    HWND cRes  = mkChk(74,  L"Continue playback where I left off (files > 5 min)", 203, s.resumePlayback);
    HWND cMin  = mkChk(100, L"Pause playback when minimized", 204, s.pauseOnMinimize);
    HWND cAdv  = mkChk(126, L"Step to next/previous media file in the same folder", 205, s.autoAdvanceFolder);
    HWND cFs   = mkChk(152, L"Start in fullscreen", 206, s.startFullscreen);
    HWND cFit  = mkChk(178, L"Auto-fit window size to video aspect ratio", 212, s.autoFitWindow);
    HWND cClk  = mkChk(204, L"Single-click video to Play / Pause", 213, s.clickToPause);
    HWND cSub  = mkChk(230, L"Automatically load subtitle files next to the video", 208, s.autoLoadSubtitles);
    HWND cFast = mkChk(256, L"Fast keyframe seeking while scrubbing (restart required)", 210, s.fastSeek);

    CreateWindowW(L"STATIC", L"Hardware decode:", WS_CHILD | WS_VISIBLE, 16, 292, 122, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND hwBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 144, 288, 130, 140, dlg.hwnd, reinterpret_cast<HMENU>(207), hi, nullptr);
    SendMessageW(hwBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto"));
    SendMessageW(hwBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"D3D11"));
    SendMessageW(hwBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"DXVA2"));
    SendMessageW(hwBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Off"));
    int hwIdx = (s.hwDecodeMode == L"D3D11") ? 1 : (s.hwDecodeMode == L"DXVA2" ? 2 : ((!s.hardwareDecoding || s.hwDecodeMode == L"Off") ? 3 : 0));
    SendMessageW(hwBox, CB_SETCURSEL, hwIdx, 0);

    CreateWindowW(L"STATIC", L"Caching profile:", WS_CHILD | WS_VISIBLE, 16, 326, 122, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND cacheBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 144, 322, 260, 140, dlg.hwnd, reinterpret_cast<HMENU>(211), hi, nullptr);
    SendMessageW(cacheBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Low latency (150 / 500 ms)"));
    SendMessageW(cacheBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Balanced (300 / 1000 ms)"));
    SendMessageW(cacheBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"High buffer (600 / 2500 ms)"));
    int cacheIdx = (s.fileCachingMs <= 200) ? 0 : (s.fileCachingMs >= 500 ? 2 : 1);
    SendMessageW(cacheBox, CB_SETCURSEL, cacheIdx, 0);

    CreateWindowW(L"STATIC", L"Snapshot format:", WS_CHILD | WS_VISIBLE, 16, 362, 122, 20, dlg.hwnd, nullptr, hi, nullptr);
    HWND fmtBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 144, 358, 90, 90, dlg.hwnd, reinterpret_cast<HMENU>(209), hi, nullptr);
    SendMessageW(fmtBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"png"));
    SendMessageW(fmtBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"jpg"));
    SendMessageW(fmtBox, CB_SETCURSEL, s.snapshotFormat == L"jpg" ? 1 : 0, 0);

    CreateWindowW(L"BUTTON", L"Subtitle Appearance...", WS_CHILD | WS_VISIBLE, 248, 356, 156, 28, dlg.hwnd, reinterpret_cast<HMENU>(304), hi, nullptr);

    CreateWindowW(L"BUTTON", L"Configure Hotkeys...", WS_CHILD | WS_VISIBLE, 16, 400, 150, 28, dlg.hwnd, reinterpret_cast<HMENU>(303), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Register File Types...", WS_CHILD | WS_VISIBLE, 174, 400, 160, 28, dlg.hwnd, reinterpret_cast<HMENU>(301), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Remove Registration", WS_CHILD | WS_VISIBLE, 342, 400, 150, 28, dlg.hwnd, reinterpret_cast<HMENU>(302), hi, nullptr);

    CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 314, 456, 86, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 408, 456, 86, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDCANCEL), hi, nullptr);

    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && LOWORD(w) == 304) {
            showSubtitleAppearance(h, pendingSub);
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(w) == 303) {
            showHotkeyEditor(h, pendingHotkeys);
            return 0;
        }
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
            s.autoFitWindow = SendMessageW(cFit, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s.clickToPause = SendMessageW(cClk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s.autoLoadSubtitles = SendMessageW(cSub, BM_GETCHECK, 0, 0) == BST_CHECKED;
            s.fastSeek = SendMessageW(cFast, BM_GETCHECK, 0, 0) == BST_CHECKED;

            s.subtitleSize = pendingSub.subtitleSize;
            s.subtitleColor = pendingSub.subtitleColor;
            s.subtitleOutline = pendingSub.subtitleOutline;
            s.subtitleLanguage = pendingSub.subtitleLanguage;
            s.subtitleEncoding = pendingSub.subtitleEncoding;

            int hwi = static_cast<int>(SendMessageW(hwBox, CB_GETCURSEL, 0, 0));
            s.hwDecodeMode = (hwi == 1) ? L"D3D11" : (hwi == 2 ? L"DXVA2" : (hwi == 3 ? L"Off" : L"Auto"));
            s.hardwareDecoding = (s.hwDecodeMode != L"Off");

            int ci = static_cast<int>(SendMessageW(cacheBox, CB_GETCURSEL, 0, 0));
            if (ci == 0) { s.fileCachingMs = 150; s.networkCachingMs = 500; }
            else if (ci == 2) { s.fileCachingMs = 600; s.networkCachingMs = 2500; }
            else { s.fileCachingMs = 300; s.networkCachingMs = 1000; }

            s.snapshotFormat = SendMessageW(fmtBox, CB_GETCURSEL, 0, 0) == 1 ? L"jpg" : L"png";
            s.hotkeys = std::move(pendingHotkeys);
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

void Dialogs::showUserGuide(HWND owner) {
    ModalDialog dlg;
    if (!dlg.create(owner, L"How to Use Reel Player & All Its Features", 680, 540)) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);

    static constexpr const wchar_t* kGuide =
        L"REEL PLAYER — USER GUIDE & FEATURE OVERVIEW\r\n"
        L"================================================================\r\n\r\n"
        L"1. OPENING MEDIA\r\n"
        L"  • Open File (Ctrl+O) / Multiple Files (Ctrl+Shift+O) / Folder (Ctrl+F):\r\n"
        L"    Plays virtually any video or audio format (MP4, MKV, WebM, AVI, MOV,\r\n"
        L"    FLAC, MP3, Opus, etc.) powered by libVLC.\r\n"
        L"  • Drag & Drop: Drop any media file or subtitle file directly onto the window.\r\n"
        L"  • Folder Stepping: Next (N) and Previous (P) automatically step through\r\n"
        L"    sibling media files in the same folder.\r\n"
        L"  • Network Stream (Ctrl+N): Paste an http(s)://, rtsp://, rtmp://, or\r\n"
        L"    smb:// URL (auto-fills from your clipboard if it contains a URL).\r\n"
        L"  • Open Disc (Ctrl+D) & Capture Device (Ctrl+C): Play DVD/Blu-ray/CD\r\n"
        L"    folders or ISOs, DirectShow capture devices, or your desktop screen.\r\n\r\n"
        L"2. PLAYBACK, SEEKING & THUMBNAIL PREVIEWS\r\n"
        L"  • Play / Pause: Space, single-click the video (configurable in Preferences),\r\n"
        L"    or click the center play button.\r\n"
        L"  • Seek Bar Previews: Hover anywhere on the seek bar to see the exact\r\n"
        L"    timestamp, chapter name (for MKV/MP4 chapters), and an aspect-ratio-\r\n"
        L"    accurate video frame thumbnail (pre-warmed in the background when\r\n"
        L"    ffmpeg is installed).\r\n"
        L"  • Fast Scrubbing: Dragging the seek bar uses fast keyframe seeking for\r\n"
        L"    instant scrubbing on 4K/HEVC files, then lands accurately on release.\r\n"
        L"  • Keyboard Seeking: Shift+Left/Right (±3s), Alt+Left/Right (±10s),\r\n"
        L"    Ctrl+Left/Right (±1 min), Ctrl+Alt+Left/Right (±5 min), Ctrl+T to\r\n"
        L"    jump to an exact timestamp.\r\n"
        L"  • Frame Stepping: Press E for next frame, Shift+E for previous frame.\r\n"
        L"  • Playback Speed: Press ] / [ to speed up or slow down, = to reset 1.0x.\r\n"
        L"  • Click Total Time: Toggles between total duration and remaining time.\r\n"
        L"  • Resume Playback: For files longer than 5 minutes, Reel Player remembers\r\n"
        L"    where you left off and shows a one-click Continue pill.\r\n\r\n"
        L"3. A-B LOOP & LOSSLESS CLIP EXPORT\r\n"
        L"  • Press Shift+L once to mark Point A, again to mark Point B (loops\r\n"
        L"    continuously between A and B), and a third time to clear.\r\n"
        L"  • Export A-B Clip (Ctrl+Shift+E or click the Export pill): Losslessly\r\n"
        L"    trims the A-B segment next to the original video using ffmpeg (-c copy)\r\n"
        L"    with zero re-encoding quality loss.\r\n\r\n"
        L"4. FULLSCREEN, PICTURE-IN-PICTURE & WINDOW MODES\r\n"
        L"  • Fullscreen (F or double-click video): Smoothly fades the floating\r\n"
        L"    control bar in on mouse movement and keeps it visible while hovering\r\n"
        L"    or scrubbing. Press Esc or F to exit.\r\n"
        L"  • Picture-in-Picture / Mini Player (Shift+P, Ctrl+Shift+P, or PiP button):\r\n"
        L"    Opens a compact, borderless, always-on-top corner player. Drag the\r\n"
        L"    video anywhere on screen, hover for controls, and press Esc or\r\n"
        L"    double-click to return to normal view.\r\n"
        L"  • Fit Window to Video (Ctrl+1): Sizes the window to match the video's native\r\n"
        L"    aspect ratio without black bars (also automatic on open in Preferences).\r\n"
        L"  • Minimal Interface (Ctrl+H): Hides the top menu bar and bottom bar.\r\n"
        L"  • Always on Top (View menu): Keeps the player above other windows.\r\n\r\n"
        L"5. TRACKS, SUBTITLES & RIGHT-CLICK MENU\r\n"
        L"  • Right-click anywhere on the video for quick access to Video Tracks,\r\n"
        L"    Audio Tracks, Subtitle Tracks, Playback controls, PiP, and Media Info.\r\n"
        L"  • Quick Cycling: Press B to cycle audio tracks, V to cycle subtitles.\r\n"
        L"  • Sidecar Subtitles: Matching .srt/.ass/.vtt files next to the video\r\n"
        L"    load automatically; drop a subtitle file or use Subtitle > Add Subtitle\r\n"
        L"    File to load another.\r\n"
        L"  • Subtitle Appearance & Live Size: Press Alt+Up / Alt+Down to resize\r\n"
        L"    subtitles on the fly, or open Subtitle > Subtitle Appearance... to\r\n"
        L"    customize font size, color, outline thickness, language, and encoding.\r\n"
        L"  • Track Synchronization (Tools > Track Synchronization): Adjust audio\r\n"
        L"    delay (J / K) or subtitle delay (G / H) in milliseconds.\r\n\r\n"
        L"6. AUDIO/VIDEO EFFECTS, SNAPSHOTS & RECORDING\r\n"
        L"  • Effects & Filters (Ctrl+E): 10-band Audio Equalizer with presets,\r\n"
        L"    Image Adjust (Brightness, Contrast, Hue, Saturation, Gamma), plus\r\n"
        L"    Rotate/Flip transforms and Sharpen filter.\r\n"
        L"  • Video Geometry: Cycle Aspect Ratio (A), Crop (C), Zoom (Z), Rotate\r\n"
        L"    90 degrees (R), and Deinterlace modes from the Video menu.\r\n"
        L"  • Snapshot (Shift+S): Saves a PNG or JPG frame to Pictures\\ReelPlayer.\r\n"
        L"  • Record (Shift+R): Records the live stream or segment to disk.\r\n\r\n"
        L"7. PREFERENCES, CUSTOM HOTKEYS & PERFORMANCE TUNING\r\n"
        L"  • Open Preferences (Ctrl+P) to configure:\r\n"
        L"      - Theme (Dark / Light / System)\r\n"
        L"      - Hardware Decoding (Auto, D3D11, DXVA2, or Off)\r\n"
        L"      - Caching Profile (Low latency, Balanced, or High buffer for NAS)\r\n"
        L"      - Fast Keyframe Seeking & Snapshot Format (PNG / JPG)\r\n"
        L"      - File Associations (register Reel Player in Windows 'Open with')\r\n"
        L"  • Configure Hotkeys: Click 'Configure Hotkeys...' in Preferences to\r\n"
        L"    rebind any action, clear it with Backspace, or reset to defaults.\r\n"
        L"  • Hover Tooltips: Hover any button on the control bar to see its name\r\n"
        L"    and current keyboard shortcut.\r\n\r\n"
        L"8. PORTABLE MODE, TASKBAR & DIAGNOSTICS\r\n"
        L"  • Portable Mode: Create an empty file named 'portable.txt' next to\r\n"
        L"    ReelPlayer.exe to store settings, resume history, and logs inside a\r\n"
        L"    local 'data\\' folder instead of AppData.\r\n"
        L"  • Windows Taskbar: Shows live playback progress on the taskbar icon\r\n"
        L"    and Previous / Play-Pause / Next buttons on taskbar hover, plus full\r\n"
        L"    Windows hardware media key support.\r\n"
        L"  • Media & Codec Information (Ctrl+I) and Messages (Ctrl+M) under Tools\r\n"
        L"    let you inspect stream codecs, dropped frames, and libVLC logs.\r\n";

    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", kGuide,
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
        14, 14, 636, 434, dlg.hwnd, nullptr, hi, nullptr);
    CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        566, 460, 84, 28, dlg.hwnd, reinterpret_cast<HMENU>(IDOK), hi, nullptr);

    dlg.handler = [&](HWND h, UINT m, WPARAM w, LPARAM) -> LRESULT {
        if (m == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) { DestroyWindow(h); return 0; }
        return -1;
    };
    dlg.runModal();
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
