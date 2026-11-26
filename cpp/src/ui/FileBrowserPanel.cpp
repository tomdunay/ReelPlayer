#include "ui/FileBrowserPanel.h"
#include "core/MediaFiles.h"
#include "core/StrUtil.h"
#include "ui/Dialogs.h"
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

namespace reel {

namespace {
constexpr UINT_PTR TimerAnimId = 71;

constexpr int BTN_UP      = 1;
constexpr int BTN_BROWSE  = 2;
constexpr int BTN_REFRESH = 3;
constexpr int BTN_CLOSE   = 4;

constexpr UINT CTX_PLAY     = 601;
constexpr UINT CTX_DELETE   = 602;
constexpr UINT CTX_OPENROOT = 603;
constexpr UINT CTX_REVEAL   = 604;

constexpr float kHeaderH = 48.0f;
constexpr float kRowH    = 32.0f;
constexpr float kIndent  = 18.0f;

inline D2D1_COLOR_F rgba(uint32_t hex, float a = 1.0f) {
    return D2D1::ColorF(
        ((hex >> 16) & 0xFF) / 255.0f,
        ((hex >> 8) & 0xFF) / 255.0f,
        (hex & 0xFF) / 255.0f,
        a);
}

inline bool ptInRect(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

float panelDpiScale(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

bool isSkippedSystemDir(const std::wstring& name) {
    if (name.empty() || name[0] == L'.') return true;
    return _wcsicmp(name.c_str(), L"$RECYCLE.BIN") == 0 ||
           _wcsicmp(name.c_str(), L"System Volume Information") == 0 ||
           _wcsicmp(name.c_str(), L"Recovery") == 0 ||
           _wcsicmp(name.c_str(), L"Config.Msi") == 0;
}

std::wstring makeExtBadge(const std::wstring& filename) {
    fs::path p(filename);
    std::wstring ext = p.extension().wstring();
    if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
    for (auto& c : ext) c = static_cast<wchar_t>(towupper(c));
    if (ext.size() > 4) ext.resize(4);
    return ext;
}
} // namespace

FileBrowserPanel::FileBrowserPanel(D2DContext& ctx) : _ctx(ctx) {}

FileBrowserPanel::~FileBrowserPanel() {
    if (_hwnd && _animTimerActive) KillTimer(_hwnd, TimerAnimId);
    releaseTarget();
    if (_hwnd) DestroyWindow(_hwnd);
}

bool FileBrowserPanel::create(HWND parent, HINSTANCE hInst) {
    _parent = parent;
    static bool registered = false;
    const wchar_t* cls = L"ReelD2DFileBrowser";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &FileBrowserPanel::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        registered = true;
    }

    _hwnd = CreateWindowExW(0, cls, L"", WS_CHILD | WS_CLIPSIBLINGS,
                            0, 0, 280, 500, parent, nullptr, hInst, this);
    return _hwnd != nullptr;
}

void FileBrowserPanel::setDarkTheme(bool dark) {
    _dark = dark;
    redraw();
}

void FileBrowserPanel::setActiveFile(const std::wstring& path) {
    if (_activeFile == path) return;
    _activeFile = path;
    redraw();
}

void FileBrowserPanel::ensureTarget() {
    if (_rt || !_hwnd || !_ctx.d2d) return;
    RECT rc{};
    GetClientRect(_hwnd, &rc);
    if (SUCCEEDED(_ctx.d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom))),
            &_rt)) && _rt) {
        float dpi = panelDpiScale(_hwnd) * 96.0f;
        _rt->SetDpi(dpi, dpi);
        _rt->CreateSolidColorBrush(rgba(0xFFFFFF), &_brush);
    }
}

void FileBrowserPanel::releaseTarget() {
    if (_brush) { _brush->Release(); _brush = nullptr; }
    if (_rt) { _rt->Release(); _rt = nullptr; }
}

void FileBrowserPanel::redraw() {
    if (_hwnd && IsWindowVisible(_hwnd)) InvalidateRect(_hwnd, nullptr, FALSE);
}

void FileBrowserPanel::ensureAnimTimer() {
    if (!_qpcFreq) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        _qpcFreq = f.QuadPart > 0 ? f.QuadPart : 10000000;
    }
    if (!_animTimerActive && _hwnd) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        _lastAnimQpc = now.QuadPart;
        // 8ms interval avoids Windows 15.6ms timer quantization doubling 16ms -> 31.2ms (32 FPS)
        SetTimer(_hwnd, TimerAnimId, 8, nullptr);
        _animTimerActive = true;
    }
}

void FileBrowserPanel::tickAnimations() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    float dt = (_lastAnimQpc > 0 && _qpcFreq > 0)
        ? static_cast<float>(now.QuadPart - _lastAnimQpc) / static_cast<float>(_qpcFreq)
        : (1.0f / 60.0f);
    _lastAnimQpc = now.QuadPart;
    dt = std::clamp(dt, 0.001f, 0.05f);

    bool active = false;
    bool treeChanged = false;

    // Frame-rate-independent smooth folder expand/collapse
    float folderFactor = 1.0f - std::exp(-16.0f * dt);
    std::function<void(Node*)> stepNode = [&](Node* n) {
        if (!n) return;
        float target = n->expanded ? 1.0f : 0.0f;
        float diff = target - n->expandAnim;
        if (std::abs(diff) > 0.004f) {
            n->expandAnim += diff * folderFactor;
            if (std::abs(target - n->expandAnim) <= 0.006f) n->expandAnim = target;
            else active = true;
            treeChanged = true;
        } else if (n->expandAnim != target) {
            n->expandAnim = target;
            treeChanged = true;
        }
        if (n->expandAnim > 0.001f || n->expanded) {
            for (auto& ch : n->children) stepNode(ch.get());
        }
    };
    if (_root) {
        for (auto& ch : _root->children) stepNode(ch.get());
    }

    // Frame-rate-independent silky smooth scroll easing
    float scrollFactor = 1.0f - std::exp(-18.0f * dt);
    float sdiff = _targetScrollY - _scrollY;
    if (std::abs(sdiff) > 0.15f) {
        _scrollY += sdiff * scrollFactor;
        if (std::abs(_targetScrollY - _scrollY) <= 0.2f) _scrollY = _targetScrollY;
        else active = true;
    } else {
        _scrollY = _targetScrollY;
    }

    if (treeChanged) rebuildFlatRows();
    clampScroll();
    // Present immediately so WM_PAINT isn't delayed behind other low-priority messages
    if (_hwnd && IsWindowVisible(_hwnd)) {
        RedrawWindow(_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }

    if (!active && _animTimerActive && _hwnd) {
        KillTimer(_hwnd, TimerAnimId);
        _animTimerActive = false;
    }
}

void FileBrowserPanel::loadChildren(Node* dirNode) {
    if (!dirNode || !dirNode->isDir || dirNode->loaded) return;
    dirNode->loaded = true;
    dirNode->children.clear();

    std::vector<std::pair<std::wstring, std::wstring>> dirs;
    std::vector<std::pair<std::wstring, std::wstring>> videos;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path(dirNode->path), fs::directory_options::skip_permission_denied, ec)) {
        std::error_code e2;
        std::wstring name = entry.path().filename().wstring();
        if (entry.is_directory(e2)) {
            if (!isSkippedSystemDir(name)) dirs.emplace_back(name, entry.path().wstring());
        } else if (entry.is_regular_file(e2)) {
            if (MediaFiles::isVideo(name)) videos.emplace_back(name, entry.path().wstring());
        }
    }

    auto cmp = [](const auto& a, const auto& b) { return MediaFiles::naturalLess(a.first, b.first); };
    std::sort(dirs.begin(), dirs.end(), cmp);
    std::sort(videos.begin(), videos.end(), cmp);

    for (const auto& [name, full] : dirs) {
        auto n = std::make_unique<Node>();
        n->name = name;
        n->path = full;
        n->isDir = true;
        n->parent = dirNode;
        dirNode->children.push_back(std::move(n));
    }
    for (const auto& [name, full] : videos) {
        auto n = std::make_unique<Node>();
        n->name = name;
        n->path = full;
        n->extBadge = makeExtBadge(name);
        n->isDir = false;
        n->loaded = true;
        n->parent = dirNode;
        dirNode->children.push_back(std::move(n));
    }
}

void FileBrowserPanel::populateDrives() {
    _currentDir.clear();
    _currentDirNode = nullptr;
    _selectedNode = nullptr;
    _hoverRow = -1;
    _scrollY = _targetScrollY = 0.0f;

    _root = std::make_unique<Node>();
    _root->name = L"This PC";
    _root->isDir = true;
    _root->loaded = true;
    _root->expanded = true;
    _root->expandAnim = 1.0f;

    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t drv[4] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0' };
        UINT type = GetDriveTypeW(drv);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
        std::wstring label(drv);
        wchar_t volName[128] = {};
        if (GetVolumeInformationW(drv, volName, 128, nullptr, nullptr, nullptr, nullptr, 0) && volName[0]) {
            label = std::wstring(volName) + L" (" + drv[0] + L":)";
        }
        auto n = std::make_unique<Node>();
        n->name = label;
        n->path = drv;
        n->isDir = true;
        n->isDrive = true;
        n->parent = _root.get();
        _root->children.push_back(std::move(n));
    }
    rebuildFlatRows();
    redraw();
}

void FileBrowserPanel::scrollToNode(Node* target, float topPaddingRows) {
    if (!target) return;
    for (const auto& r : _rows) {
        if (r.node == target) {
            float desired = std::max(0.0f, r.y - topPaddingRows * kRowH);
            _scrollY = _targetScrollY = desired;
            clampScroll();
            return;
        }
    }
}

void FileBrowserPanel::expandPathInTree(const std::wstring& dir) {
    if (!_root) populateDrives();
    std::error_code ec;
    fs::path norm = fs::weakly_canonical(fs::path(dir), ec);
    if (ec || norm.empty()) norm = fs::path(dir).lexically_normal();

    // Collect ancestor chain from drive root down to target directory
    std::vector<fs::path> chain;
    for (fs::path cur = norm; !cur.empty(); ) {
        chain.push_back(cur);
        fs::path par = cur.parent_path();
        if (par == cur) break;
        cur = par;
    }
    std::reverse(chain.begin(), chain.end());

    Node* curNode = _root.get();
    for (const auto& step : chain) {
        std::wstring stepStr = step.wstring();
        Node* match = nullptr;
        for (auto& ch : curNode->children) {
            if (!ch->isDir) continue;
            // Compare paths case-insensitively (ignoring trailing backslash except drive root)
            std::wstring a = ch->path, b = stepStr;
            while (a.size() > 3 && (a.back() == L'\\' || a.back() == L'/')) a.pop_back();
            while (b.size() > 3 && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
            if (_wcsicmp(a.c_str(), b.c_str()) == 0) {
                match = ch.get();
                break;
            }
        }
        if (!match) break;
        if (!match->loaded) loadChildren(match);
        match->expanded = true;
        match->expandAnim = 1.0f;
        curNode = match;
    }

    _currentDir = curNode && curNode != _root.get() ? curNode->path : dir;
    _currentDirNode = (curNode && curNode != _root.get()) ? curNode : nullptr;
    _selectedNode = _currentDirNode;

    // Also select the currently playing video node if it's inside this folder
    Node* scrollTarget = _currentDirNode;
    if (_currentDirNode && !_activeFile.empty()) {
        for (auto& ch : _currentDirNode->children) {
            if (!ch->isDir && _wcsicmp(ch->path.c_str(), _activeFile.c_str()) == 0) {
                _selectedNode = ch.get();
                break;
            }
        }
    }

    rebuildFlatRows();
    // Scroll once viewport height is known so the opened movie folder sits cleanly near the top
    // with 2 parent tree rows visible above it.
    _pendingScrollNode = scrollTarget;
    scrollToNode(scrollTarget, 2.0f);
}

void FileBrowserPanel::openDirectory(const std::wstring& dir) {
    // Copy before populateDrives() clears _currentDir (prevents aliasing when called as openDirectory(_currentDir))
    std::wstring targetDir = dir;
    std::error_code ec;
    if (targetDir.empty() || !fs::exists(fs::path(targetDir), ec) || !fs::is_directory(fs::path(targetDir), ec)) {
        populateDrives();
        return;
    }
    populateDrives();
    expandPathInTree(targetDir);
    if (_cb.onDirectoryChanged && !_currentDir.empty()) _cb.onDirectoryChanged(_currentDir);
    redraw();
}

void FileBrowserPanel::refresh() {
    if (_currentDir.empty()) populateDrives();
    else openDirectory(_currentDir);
}

void FileBrowserPanel::navigateUp() {
    if (_currentDir.empty()) return;
    fs::path p(_currentDir);
    fs::path parent = p.parent_path();
    if (parent.empty() || parent == p) {
        populateDrives();
    } else {
        openDirectory(parent.wstring());
    }
}

void FileBrowserPanel::browseForFolder() {
    if (auto folder = Dialogs::openFolder(_parent ? _parent : _hwnd, _currentDir)) {
        openDirectory(*folder);
    }
}

void FileBrowserPanel::flattenChildren(Node* parent, int depth, float scaleAlpha, float& y) {
    for (auto& ch : parent->children) {
        float h = kRowH * scaleAlpha;
        if (h >= 0.5f) {
            _rows.push_back(FlatRow{ ch.get(), depth, y, h, scaleAlpha });
            y += h;
        }
        if (ch->isDir && ch->expandAnim > 0.005f && !ch->children.empty()) {
            flattenChildren(ch.get(), depth + 1, scaleAlpha * ch->expandAnim, y);
        }
    }
}

void FileBrowserPanel::rebuildFlatRows() {
    _rows.clear();
    float y = 6.0f;
    if (_root) flattenChildren(_root.get(), 0, 1.0f, y);
    _totalContentH = y + 8.0f;
}

void FileBrowserPanel::clampScroll() {
    float maxScroll = std::max(0.0f, _totalContentH - _lastViewH);
    _targetScrollY = std::clamp(_targetScrollY, 0.0f, maxScroll);
    _scrollY = std::clamp(_scrollY, 0.0f, maxScroll);
}

void FileBrowserPanel::toggleFolder(Node* node) {
    if (!node || !node->isDir) return;
    if (!node->expanded) {
        if (!node->loaded) loadChildren(node);
        node->expanded = true;
        _currentDir = node->path;
        _currentDirNode = node;
        if (_cb.onDirectoryChanged) _cb.onDirectoryChanged(node->path);
    } else {
        node->expanded = false;
    }
    ensureAnimTimer();
    rebuildFlatRows();
    redraw();
}

void FileBrowserPanel::deleteNode(Node* node) {
    if (!node || node->isDir) return;
    std::wstring path = node->path;
    std::wstring name = node->name;
    std::wstring prompt = L"Move \"" + name + L"\" to the Recycle Bin?";
    if (MessageBoxW(_parent ? _parent : _hwnd, prompt.c_str(), L"Delete Video File",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    if (_cb.onBeforeDelete) _cb.onBeforeDelete(path);

    std::wstring doubleNull = path;
    doubleNull.push_back(L'\0');
    doubleNull.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.hwnd = _parent ? _parent : _hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = doubleNull.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    int res = SHFileOperationW(&op);
    if (res == 0 && !op.fAnyOperationsAborted) {
        if (_selectedNode == node) _selectedNode = nullptr;
        if (Node* par = node->parent) {
            auto& v = par->children;
            v.erase(std::remove_if(v.begin(), v.end(), [node](const std::unique_ptr<Node>& p) { return p.get() == node; }), v.end());
        }
        rebuildFlatRows();
        clampScroll();
        redraw();
        if (_cb.onFileDeleted) _cb.onFileDeleted(path);
    } else {
        MessageBoxW(_parent ? _parent : _hwnd,
                    L"Could not delete the file. It may be in use by another program.",
                    L"Delete Video File", MB_OK | MB_ICONERROR);
    }
}

void FileBrowserPanel::showContextMenu(Node* node, int screenX, int screenY) {
    if (!node) return;
    HMENU menu = CreatePopupMenu();
    if (node->isDir) {
        AppendMenuW(menu, MF_STRING, CTX_OPENROOT, L"Open as Root Folder");
        AppendMenuW(menu, MF_STRING, CTX_REVEAL, L"Show in File Explorer");
    } else {
        AppendMenuW(menu, MF_STRING, CTX_PLAY, L"Play Video");
        AppendMenuW(menu, MF_STRING, CTX_REVEAL, L"Show in File Explorer");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CTX_DELETE, L"Delete Video (Move to Recycle Bin)...\tDel");
    }
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenX, screenY, 0, _hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == CTX_PLAY && !node->isDir) {
        if (_cb.onPlayFile) _cb.onPlayFile(node->path);
    } else if (cmd == CTX_OPENROOT && node->isDir) {
        openDirectory(node->path);
    } else if (cmd == CTX_REVEAL) {
        std::wstring arg = L"/select,\"" + node->path + L"\"";
        ShellExecuteW(_hwnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (cmd == CTX_DELETE && !node->isDir) {
        deleteNode(node);
    }
}

void FileBrowserPanel::computeHeaderLayout(float innerW) {
    _headerBtns.clear();
    const float btnSz = 26.0f;
    const float y = (kHeaderH - btnSz) * 0.5f;
    float rx = innerW - 8.0f;
    auto push = [&](int id, const wchar_t* glyph, const wchar_t* tip) {
        rx -= btnSz;
        _headerBtns.push_back({ D2D1::RectF(rx, y, rx + btnSz, y + btnSz), id, glyph, tip });
        rx -= 2.0f;
    };
    push(BTN_CLOSE,   L"\uE711", L"Close");
    push(BTN_REFRESH, L"\uE72C", L"Refresh");
    push(BTN_BROWSE,  L"\uE8B7", L"Open Folder");
    push(BTN_UP,      L"\uE74A", L"Up");
}

int FileBrowserPanel::hitTestRow(float y) const {
    if (y < kHeaderH) return -1;
    float cy = (y - kHeaderH) + _scrollY;
    for (int i = 0; i < static_cast<int>(_rows.size()); ++i) {
        if (cy >= _rows[i].y && cy < _rows[i].y + _rows[i].height) return i;
    }
    return -1;
}

D2D1_RECT_F FileBrowserPanel::deleteBtnRectForRow(const FlatRow& row, float innerW) const {
    float top = kHeaderH + row.y - _scrollY;
    float cy = top + row.height * 0.5f;
    return D2D1::RectF(innerW - 32.0f, cy - 11.0f, innerW - 10.0f, cy + 11.0f);
}

void FileBrowserPanel::drawChevron(ID2D1SolidColorBrush* br, float cx, float cy, float angleDeg, float alpha) {
    D2D1_MATRIX_3X2_F oldTf;
    _rt->GetTransform(&oldTf);
    _rt->SetTransform(D2D1::Matrix3x2F::Rotation(angleDeg, D2D1::Point2F(cx, cy)) * oldTf);
    br->SetColor(_dark ? rgba(0xA1A1AA, alpha) : rgba(0x52525B, alpha));
    _rt->DrawLine(D2D1::Point2F(cx - 2.0f, cy - 3.5f), D2D1::Point2F(cx + 2.0f, cy), br, 1.5f);
    _rt->DrawLine(D2D1::Point2F(cx + 2.0f, cy), D2D1::Point2F(cx - 2.0f, cy + 3.5f), br, 1.5f);
    _rt->SetTransform(oldTf);
}

void FileBrowserPanel::drawFolderIcon(ID2D1SolidColorBrush* br, float x, float y, bool expanded, bool isDrive, float alpha) {
    if (isDrive) {
        // Sleek drive badge
        br->SetColor(rgba(0x38BDF8, 0.90f * alpha));
        _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y + 2.0f, x + 16.0f, y + 13.0f), 2.5f, 2.5f), br);
        br->SetColor(rgba(0x0F172A, 0.85f * alpha));
        _rt->FillRectangle(D2D1::RectF(x + 2.5f, y + 8.5f, x + 13.5f, y + 10.5f), br);
        br->SetColor(rgba(0x14B8A6, alpha));
        _rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 12.0f, y + 5.5f), 1.3f, 1.3f), br);
        return;
    }
    // Warm golden-amber rounded folder vector icon
    br->SetColor(rgba(expanded ? 0xFBBF24 : 0xF59E0B, 0.92f * alpha));
    _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y + 1.0f, x + 7.5f, y + 5.0f), 1.5f, 1.5f), br);
    _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y + 3.0f, x + 16.0f, y + 13.5f), 2.2f, 2.2f), br);
    if (expanded) {
        // Subtle open-flap highlight
        br->SetColor(rgba(0xFDE68A, 0.55f * alpha));
        _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 1.2f, y + 5.5f, x + 14.8f, y + 12.5f), 1.5f, 1.5f), br);
    }
}

void FileBrowserPanel::drawVideoIcon(ID2D1SolidColorBrush* br, float x, float y, bool active, float alpha) {
    // Modern rounded teal/slate video frame badge with crisp play glyph
    br->SetColor(active ? rgba(0x14B8A6, alpha) : rgba(0x14B8A6, 0.22f * alpha));
    _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y + 1.0f, x + 16.0f, y + 13.5f), 3.0f, 3.0f), br);
    br->SetColor(active ? rgba(0x2DD4BF, alpha) : rgba(0x14B8A6, 0.72f * alpha));
    _rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 0.5f, y + 1.5f, x + 15.5f, y + 13.0f), 2.8f, 2.8f), br, 1.0f);

    // Mini play triangle inside the video badge
    br->SetColor(active ? rgba(0x0B0E11, alpha) : rgba(0x2DD4BF, alpha));
    float cx = x + 6.5f, cy = y + 7.25f;
    _rt->DrawLine(D2D1::Point2F(cx, cy - 3.0f), D2D1::Point2F(cx, cy + 3.0f), br, 1.5f);
    _rt->DrawLine(D2D1::Point2F(cx, cy - 3.0f), D2D1::Point2F(cx + 4.5f, cy), br, 1.4f);
    _rt->DrawLine(D2D1::Point2F(cx, cy + 3.0f), D2D1::Point2F(cx + 4.5f, cy), br, 1.4f);
}

void FileBrowserPanel::paint() {
    ensureTarget();
    if (!_rt || !_brush) return;

    float dpi = panelDpiScale(_hwnd) * 96.0f;
    _rt->SetDpi(dpi, dpi);
    D2D1_SIZE_F sz = _rt->GetSize();
    float w = sz.width;
    float h = sz.height;
    if (w < 20.0f || h < 20.0f) return;

    float innerW = std::max(20.0f, w - static_cast<float>(SplitterDip));
    _lastInnerW = innerW;
    _lastViewH = std::max(1.0f, h - kHeaderH);
    if (_pendingScrollNode) {
        Node* target = _pendingScrollNode;
        _pendingScrollNode = nullptr;
        scrollToNode(target, 2.0f);
    }
    clampScroll();
    computeHeaderLayout(innerW);

    _rt->BeginDraw();

    const D2D1_COLOR_F bgCol     = _dark ? rgba(0x131319) : rgba(0xF6F7F9);
    const D2D1_COLOR_F hdrCol    = _dark ? rgba(0x181820) : rgba(0xECEEF2);
    const D2D1_COLOR_F fgCol     = _dark ? rgba(0xF4F4F6) : rgba(0x18181B);
    const D2D1_COLOR_F mutedCol  = _dark ? rgba(0x8E8E9A) : rgba(0x64646E);
    const D2D1_COLOR_F hairline  = _dark ? rgba(0xFFFFFF, 0.08f) : rgba(0x000000, 0.10f);
    const D2D1_COLOR_F teal      = rgba(0x14B8A6);
    const D2D1_COLOR_F tealBright= rgba(0x2DD4BF);

    _rt->Clear(bgCol);
    ID2D1SolidColorBrush* br = _brush;

    // 1. Header surface
    br->SetColor(hdrCol);
    _rt->FillRectangle(D2D1::RectF(0, 0, innerW, kHeaderH), br);
    br->SetColor(hairline);
    _rt->DrawLine(D2D1::Point2F(0, kHeaderH - 0.5f), D2D1::Point2F(innerW, kHeaderH - 0.5f), br, 1.0f);

    // Header title + subtle video/folder subtitle
    std::wstring title = L"This PC";
    if (!_currentDir.empty()) {
        fs::path p(_currentDir);
        title = p.filename().wstring();
        if (title.empty()) title = _currentDir;
    }
    int vidCount = 0, dirCount = 0;
    Node* statsNode = _currentDirNode ? _currentDirNode : _root.get();
    if (statsNode) {
        for (const auto& ch : statsNode->children) {
            if (ch->isDir) ++dirCount;
            else ++vidCount;
        }
    }
    std::wstring sub;
    if (_currentDir.empty()) sub = L"Select a drive or folder";
    else {
        sub = std::to_wstring(vidCount) + (vidCount == 1 ? L" video" : L" videos");
        if (dirCount > 0) sub += L"  \u00B7  " + std::to_wstring(dirCount) + (dirCount == 1 ? L" folder" : L" folders");
    }

    float titleRight = _headerBtns.empty() ? (innerW - 10.0f) : (_headerBtns.back().rc.left - 6.0f);
    _rt->PushAxisAlignedClip(D2D1::RectF(10.0f, 4.0f, std::max(12.0f, titleRight), kHeaderH - 4.0f), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    br->SetColor(fgCol);
    _ctx.fontUiBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    _rt->DrawText(title.c_str(), static_cast<UINT32>(title.size()), _ctx.fontUiBold,
                  D2D1::RectF(12.0f, 6.0f, std::max(14.0f, titleRight), 25.0f), br);
    br->SetColor(mutedCol);
    _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    _rt->DrawText(sub.c_str(), static_cast<UINT32>(sub.size()), _ctx.fontSmall,
                  D2D1::RectF(12.0f, 25.0f, std::max(14.0f, titleRight), 42.0f), br);
    _rt->PopAxisAlignedClip();

    // Header icon buttons
    for (int i = 0; i < static_cast<int>(_headerBtns.size()); ++i) {
        const auto& b = _headerBtns[i];
        bool hov = (i == _hoverHeaderBtn);
        if (hov) {
            br->SetColor(_dark ? rgba(0xFFFFFF, 0.12f) : rgba(0x000000, 0.08f));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(b.rc, 5.0f, 5.0f), br);
        }
        br->SetColor(hov ? fgCol : mutedCol);
        _rt->DrawText(b.glyph, 1, _ctx.fontIconSmall, b.rc, br);
    }

    // 2. Scrollable Tree Viewport
    _rt->PushAxisAlignedClip(D2D1::RectF(0, kHeaderH, innerW, h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (_rows.empty()) {
        br->SetColor(mutedCol);
        _ctx.fontUi->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        const wchar_t* emptyMsg = L"No video files in this folder";
        _rt->DrawText(emptyMsg, static_cast<UINT32>(wcslen(emptyMsg)), _ctx.fontUi,
                      D2D1::RectF(16.0f, kHeaderH + 40.0f, innerW - 16.0f, kHeaderH + 72.0f), br);
        _ctx.fontUi->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    for (int i = 0; i < static_cast<int>(_rows.size()); ++i) {
        const auto& row = _rows[i];
        float top = kHeaderH + row.y - _scrollY;
        float bot = top + row.height;
        if (bot < kHeaderH || top > h) continue;

        Node* n = row.node;
        float a = row.alpha;
        bool isPlaying = (!n->isDir && !_activeFile.empty() && _wcsicmp(n->path.c_str(), _activeFile.c_str()) == 0);
        bool isSelected = (n == _selectedNode);
        bool isHovered = (i == _hoverRow);

        // Row background pill
        D2D1_RECT_F pill = D2D1::RectF(6.0f, top + 1.5f, innerW - 8.0f, bot - 1.5f);
        if (isPlaying) {
            br->SetColor(rgba(0x14B8A6, (isHovered ? 0.22f : 0.15f) * a));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(pill, 6.0f, 6.0f), br);
            // Left teal active indicator bar
            br->SetColor(rgba(0x14B8A6, a));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(6.0f, top + 6.0f, 9.0f, bot - 6.0f), 1.5f, 1.5f), br);
        } else if (isHovered) {
            br->SetColor(_dark ? rgba(0xFFFFFF, 0.08f * a) : rgba(0x000000, 0.06f * a));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(pill, 6.0f, 6.0f), br);
        } else if (isSelected) {
            br->SetColor(rgba(0x14B8A6, 0.10f * a));
            _rt->FillRoundedRectangle(D2D1::RoundedRect(pill, 6.0f, 6.0f), br);
        }

        // Subtle vertical tree indent guide lines
        if (row.depth > 0) {
            br->SetColor(_dark ? rgba(0xFFFFFF, 0.06f * a) : rgba(0x000000, 0.08f * a));
            for (int d = 0; d < row.depth; ++d) {
                float gx = 17.0f + d * kIndent;
                _rt->DrawLine(D2D1::Point2F(gx, top), D2D1::Point2F(gx, bot), br, 1.0f);
            }
        }

        float x = 12.0f + row.depth * kIndent;
        float cy = (top + bot) * 0.5f;

        if (n->isDir) {
            drawChevron(br, x + 5.0f, cy, 90.0f * n->expandAnim, a);
            drawFolderIcon(br, x + 14.0f, cy - 7.2f, n->expanded, n->isDrive, a);
            float textX = x + 36.0f;
            float textR = innerW - 14.0f;
            br->SetColor(_dark ? rgba(0xEAEAF0, a) : rgba(0x18181B, a));
            _rt->PushAxisAlignedClip(D2D1::RectF(textX, top, std::max(textX + 2.0f, textR), bot), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            _rt->DrawText(n->name.c_str(), static_cast<UINT32>(n->name.size()), _ctx.fontUi,
                          D2D1::RectF(textX, top, std::max(textX + 4.0f, textR), bot), br);
            _rt->PopAxisAlignedClip();
        } else {
            drawVideoIcon(br, x + 12.0f, cy - 7.2f, isPlaying, a);
            float textX = x + 34.0f;
            float rightPad = innerW - 12.0f;

            // Show inline Delete icon button on hover, otherwise show crisp format badge (MP4 / MKV)
            if (isHovered && row.height >= 24.0f) {
                D2D1_RECT_F delRc = deleteBtnRectForRow(row, innerW);
                if (_hoverDeleteBtn) {
                    br->SetColor(rgba(0xEF4444, 0.25f * a));
                    _rt->FillRoundedRectangle(D2D1::RoundedRect(delRc, 5.0f, 5.0f), br);
                }
                br->SetColor(_hoverDeleteBtn ? rgba(0xF87171, a) : rgba(0xA1A1AA, a));
                _rt->DrawText(L"\uE74D", 1, _ctx.fontIconSmall, delRc, br);
                rightPad = delRc.left - 4.0f;
            } else if (!n->extBadge.empty() && innerW > 180.0f) {
                float badgeW = 34.0f;
                D2D1_RECT_F brc = D2D1::RectF(innerW - 12.0f - badgeW, cy - 8.5f, innerW - 12.0f, cy + 8.5f);
                br->SetColor(isPlaying ? rgba(0x14B8A6, 0.28f * a) : (_dark ? rgba(0xFFFFFF, 0.06f * a) : rgba(0x000000, 0.06f * a)));
                _rt->FillRoundedRectangle(D2D1::RoundedRect(brc, 4.0f, 4.0f), br);
                br->SetColor(isPlaying ? tealBright : mutedCol);
                _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                _rt->DrawText(n->extBadge.c_str(), static_cast<UINT32>(n->extBadge.size()), _ctx.fontSmall, brc, br);
                _ctx.fontSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                rightPad = brc.left - 6.0f;
            }

            br->SetColor(isPlaying ? rgba(0xFFFFFF, a) : (_dark ? rgba(0xD4D4DC, a) : rgba(0x27272A, a)));
            IDWriteTextFormat* fmt = isPlaying ? _ctx.fontUiBold : _ctx.fontUi;
            _rt->PushAxisAlignedClip(D2D1::RectF(textX, top, std::max(textX + 2.0f, rightPad), bot), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            _rt->DrawText(n->name.c_str(), static_cast<UINT32>(n->name.size()), fmt,
                          D2D1::RectF(textX, top, std::max(textX + 4.0f, rightPad), bot), br);
            _rt->PopAxisAlignedClip();
        }
    }

    // 3. Sleek Overlay Scrollbar
    if (_totalContentH > _lastViewH + 1.0f) {
        float thumbH = std::max(28.0f, (_lastViewH / _totalContentH) * _lastViewH);
        float maxScroll = _totalContentH - _lastViewH;
        float thumbY = kHeaderH + (_scrollY / maxScroll) * (_lastViewH - thumbH);
        float sbW = (_draggingScroll || (_mouseInside && _mouseX >= innerW - 10.0f && _mouseX < innerW)) ? 6.0f : 3.5f;
        br->SetColor(_dark ? rgba(0xFFFFFF, _draggingScroll ? 0.40f : 0.22f) : rgba(0x000000, 0.25f));
        _rt->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(innerW - sbW - 2.0f, thumbY + 2.0f, innerW - 2.0f, thumbY + thumbH - 2.0f),
            sbW * 0.5f, sbW * 0.5f), br);
    }

    _rt->PopAxisAlignedClip();

    // 4. Right-edge Resizable Splitter
    br->SetColor((_splitterHover || _draggingSplitter) ? rgba(0x14B8A6, 0.55f) : (_dark ? rgba(0x1C1C24) : rgba(0xE2E4EA)));
    _rt->FillRectangle(D2D1::RectF(innerW, 0, w, h), br);
    br->SetColor((_splitterHover || _draggingSplitter) ? teal : hairline);
    _rt->DrawLine(D2D1::Point2F(innerW + 0.5f, 0), D2D1::Point2F(innerW + 0.5f, h), br, 1.0f);

    if (_rt->EndDraw() == D2DERR_RECREATE_TARGET) releaseTarget();
}

LRESULT CALLBACK FileBrowserPanel::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    FileBrowserPanel* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<FileBrowserPanel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->_hwnd = hwnd;
    } else {
        self = reinterpret_cast<FileBrowserPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handleMsg(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT FileBrowserPanel::handleMsg(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(_hwnd, &ps);
            paint();
            EndPaint(_hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (_rt) {
                _rt->Resize(D2D1::SizeU(std::max<UINT>(1, LOWORD(lp)), std::max<UINT>(1, HIWORD(lp))));
            }
            redraw();
            return 0;
        case WM_TIMER:
            if (wp == TimerAnimId) { tickAnimations(); return 0; }
            break;
        case WM_MOUSEMOVE: {
            if (!_mouseInside) {
                _mouseInside = true;
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, _hwnd, 0 };
                TrackMouseEvent(&tme);
            }
            float scale = panelDpiScale(_hwnd);
            _mouseX = static_cast<float>(GET_X_LPARAM(lp)) / scale;
            _mouseY = static_cast<float>(GET_Y_LPARAM(lp)) / scale;

            if (_draggingSplitter) {
                POINT pt{};
                GetCursorPos(&pt);
                int newPx = _dragStartWidthPx + (pt.x - _dragStartScreenX);
                int newDip = std::clamp(static_cast<int>(std::lround(newPx / scale)), MinWidthDip, MaxWidthDip);
                if (_cb.onWidthChanged) _cb.onWidthChanged(newDip);
                return 0;
            }
            if (_draggingScroll && _totalContentH > _lastViewH) {
                float thumbH = std::max(28.0f, (_lastViewH / _totalContentH) * _lastViewH);
                float track = std::max(1.0f, _lastViewH - thumbH);
                float dy = _mouseY - _scrollDragStartY;
                _targetScrollY = _scrollDragStartOffset + (dy / track) * (_totalContentH - _lastViewH);
                _scrollY = _targetScrollY;
                clampScroll();
                redraw();
                return 0;
            }

            bool prevSplit = _splitterHover;
            _splitterHover = (_mouseX >= _lastInnerW - 2.0f);
            _hoverHeaderBtn = -1;
            _hoverRow = -1;
            _hoverDeleteBtn = false;

            if (!_splitterHover) {
                if (_mouseY < kHeaderH) {
                    for (int i = 0; i < static_cast<int>(_headerBtns.size()); ++i) {
                        if (ptInRect(_headerBtns[i].rc, _mouseX, _mouseY)) { _hoverHeaderBtn = i; break; }
                    }
                } else {
                    _hoverRow = hitTestRow(_mouseY);
                    if (_hoverRow >= 0 && _hoverRow < static_cast<int>(_rows.size()) && !_rows[_hoverRow].node->isDir) {
                        _hoverDeleteBtn = ptInRect(deleteBtnRectForRow(_rows[_hoverRow], _lastInnerW), _mouseX, _mouseY);
                    }
                }
            }

            if (_splitterHover) SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            else if (_hoverHeaderBtn >= 0 || _hoverRow >= 0) SetCursor(LoadCursorW(nullptr, IDC_HAND));
            else SetCursor(LoadCursorW(nullptr, IDC_ARROW));

            if (prevSplit != _splitterHover || _hoverHeaderBtn >= 0 || _hoverRow >= 0) redraw();
            return 0;
        }
        case WM_MOUSELEAVE:
            _mouseInside = false;
            _splitterHover = false;
            _hoverHeaderBtn = -1;
            _hoverRow = -1;
            _hoverDeleteBtn = false;
            redraw();
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(_hwnd);
            float scale = panelDpiScale(_hwnd);
            float x = static_cast<float>(GET_X_LPARAM(lp)) / scale;
            float y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;

            if (x >= _lastInnerW - 2.0f) {
                _draggingSplitter = true;
                POINT pt{};
                GetCursorPos(&pt);
                _dragStartScreenX = pt.x;
                RECT rc{};
                GetClientRect(_hwnd, &rc);
                _dragStartWidthPx = rc.right - rc.left;
                SetCapture(_hwnd);
                return 0;
            }
            if (y >= kHeaderH && x >= _lastInnerW - 10.0f && _totalContentH > _lastViewH) {
                _draggingScroll = true;
                _scrollDragStartY = y;
                _scrollDragStartOffset = _scrollY;
                SetCapture(_hwnd);
                return 0;
            }
            if (y < kHeaderH) {
                for (const auto& b : _headerBtns) {
                    if (ptInRect(b.rc, x, y)) {
                        if (b.id == BTN_UP) navigateUp();
                        else if (b.id == BTN_BROWSE) browseForFolder();
                        else if (b.id == BTN_REFRESH) refresh();
                        else if (b.id == BTN_CLOSE && _cb.onCloseRequested) _cb.onCloseRequested();
                        return 0;
                    }
                }
                return 0;
            }
            int idx = hitTestRow(y);
            if (idx >= 0 && idx < static_cast<int>(_rows.size())) {
                Node* n = _rows[idx].node;
                _selectedNode = n;
                if (n->isDir) {
                    toggleFolder(n);
                } else {
                    if (ptInRect(deleteBtnRectForRow(_rows[idx], _lastInnerW), x, y)) {
                        deleteNode(n);
                        return 0;
                    }
                    fs::path p(n->path);
                    if (_cb.onDirectoryChanged && p.has_parent_path()) _cb.onDirectoryChanged(p.parent_path().wstring());
                    if (_cb.onPlayFile) _cb.onPlayFile(n->path);
                    redraw();
                }
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            float scale = panelDpiScale(_hwnd);
            float y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            int idx = hitTestRow(y);
            if (idx >= 0 && idx < static_cast<int>(_rows.size()) && _rows[idx].node->isDir) {
                openDirectory(_rows[idx].node->path);
            }
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (_draggingSplitter || _draggingScroll) {
                _draggingSplitter = false;
                _draggingScroll = false;
                if (msg == WM_LBUTTONUP) ReleaseCapture();
                redraw();
                return 0;
            }
            break;
        case WM_RBUTTONUP: {
            float scale = panelDpiScale(_hwnd);
            float y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            int idx = hitTestRow(y);
            if (idx >= 0 && idx < static_cast<int>(_rows.size())) {
                _selectedNode = _rows[idx].node;
                redraw();
                POINT pt{};
                GetCursorPos(&pt);
                showContextMenu(_rows[idx].node, pt.x, pt.y);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            _targetScrollY -= (static_cast<float>(delta) / 120.0f) * (kRowH * 3.0f);
            clampScroll();
            // Kick off immediate first eased frame so scroll response feels zero-latency
            _scrollY += (_targetScrollY - _scrollY) * 0.18f;
            ensureAnimTimer();
            if (_hwnd && IsWindowVisible(_hwnd)) {
                RedrawWindow(_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_DELETE && _selectedNode && !_selectedNode->isDir) {
                deleteNode(_selectedNode);
                return 0;
            }
            break;
    }
    return DefWindowProcW(_hwnd, msg, wp, lp);
}

} // namespace reel
