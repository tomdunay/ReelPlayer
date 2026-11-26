#pragma once
#include "ui/D2DControls.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace reel {

enum class FileSortMode { Name = 0, Size = 1, DateCreated = 2 };

struct FileBrowserCallbacks {
    std::function<void(const std::wstring&)> onPlayFile;
    std::function<void(const std::wstring&)> onBeforeDelete;
    std::function<void(const std::wstring&)> onFileDeleted;
    std::function<void(const std::wstring&)> onDirectoryChanged;
    std::function<void(int)> onWidthChanged; // new width in DIPs
    std::function<void(FileSortMode, bool)> onSortChanged;
    std::function<void()> onCloseRequested;
};

/// Pure Direct2D left-hand resizable directory tree with smooth folder expand/collapse animations,
/// custom vector icons, extension badges, inline delete button, and video-only filtering.
class FileBrowserPanel {
public:
    static constexpr int MinWidthDip = 200;
    static constexpr int MaxWidthDip = 640;
    static constexpr int SplitterDip = 5;

    explicit FileBrowserPanel(D2DContext& ctx);
    ~FileBrowserPanel();

    bool create(HWND parent, HINSTANCE hInst);
    HWND hwnd() const { return _hwnd; }

    void setCallbacks(FileBrowserCallbacks cb) { _cb = std::move(cb); }
    void setDarkTheme(bool dark);
    void setSort(FileSortMode mode, bool ascending);
    void setActiveFile(const std::wstring& path);
    void openDirectory(const std::wstring& dir);
    void refresh();
    const std::wstring& currentDirectory() const { return _currentDir; }

private:
    struct Node {
        std::wstring name;
        std::wstring path;
        std::wstring extBadge; // e.g. "MP4", "MKV"
        std::wstring sizeText; // e.g. "1.4 GB", "428 MB"
        uint64_t sizeBytes = 0;
        uint64_t createdTime = 0; // FILETIME as 64-bit ticks
        bool isDir = false;
        bool isDrive = false;
        bool loaded = false;
        bool expanded = false;
        float expandAnim = 0.0f; // 0 = collapsed, 1 = expanded
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
    };

    struct FlatRow {
        Node* node = nullptr;
        int depth = 0;
        float y = 0.0f;
        float height = 0.0f;
        float alpha = 1.0f;
    };

    struct HeaderBtn {
        D2D1_RECT_F rc{};
        int id = 0;
        const wchar_t* glyph = L"";
        const wchar_t* tip = L"";
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMsg(UINT msg, WPARAM wp, LPARAM lp);

    void ensureTarget();
    void releaseTarget();
    void redraw();
    void ensureAnimTimer();
    void tickAnimations();

    void populateDrives();
    void loadChildren(Node* dirNode);
    void expandPathInTree(const std::wstring& dir);
    void scrollToNode(Node* target, float topPaddingRows = 2.0f);
    void rebuildFlatRows();
    void flattenChildren(Node* parent, int depth, float scaleAlpha, float& y);

    void toggleFolder(Node* node);
    void deleteNode(Node* node);
    void navigateUp();
    void browseForFolder();
    void showSortMenu(int screenX, int screenY);
    void sortNodeRecursive(Node* node);
    void showContextMenu(Node* node, int screenX, int screenY);

    void computeHeaderLayout(float innerW);
    void paint();
    void drawFolderIcon(ID2D1SolidColorBrush* br, float x, float y, bool expanded, bool isDrive, float alpha);
    void drawVideoIcon(ID2D1SolidColorBrush* br, float x, float y, bool active, float alpha);
    void drawChevron(ID2D1SolidColorBrush* br, float cx, float cy, float angleDeg, float alpha);
    int hitTestRow(float y) const;
    D2D1_RECT_F deleteBtnRectForRow(const FlatRow& row, float innerW) const;
    void clampScroll();

    D2DContext& _ctx;
    HWND _hwnd = nullptr;
    HWND _parent = nullptr;
    ID2D1HwndRenderTarget* _rt = nullptr;
    ID2D1SolidColorBrush* _brush = nullptr;

    bool _dark = true;
    FileSortMode _sortMode = FileSortMode::Name;
    bool _sortAsc = true;
    bool _animTimerActive = false;
    bool _mouseInside = false;
    bool _splitterHover = false;
    bool _draggingSplitter = false;
    bool _draggingScroll = false;
    int _dragStartScreenX = 0;
    int _dragStartWidthPx = 0;
    float _scrollDragStartY = 0.0f;
    float _scrollDragStartOffset = 0.0f;

    float _mouseX = -1.0f;
    float _mouseY = -1.0f;
    int _hoverRow = -1;
    bool _hoverDeleteBtn = false;
    int _hoverHeaderBtn = -1;
    Node* _selectedNode = nullptr;

    float _scrollY = 0.0f;
    float _targetScrollY = 0.0f;
    float _totalContentH = 0.0f;
    float _lastViewH = 400.0f;
    float _lastInnerW = 275.0f;
    int64_t _lastAnimQpc = 0;
    int64_t _qpcFreq = 0;

    std::wstring _currentDir;
    std::wstring _activeFile;
    Node* _currentDirNode = nullptr;
    Node* _pendingScrollNode = nullptr;
    std::unique_ptr<Node> _root;
    std::vector<FlatRow> _rows;
    std::vector<HeaderBtn> _headerBtns;
    FileBrowserCallbacks _cb;
};

} // namespace reel
