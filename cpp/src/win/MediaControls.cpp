#include "win/MediaControls.h"
#include "win/WinInterop.h"
#include <unknwn.h>
#include <algorithm>
#include <atomic>

namespace reel {

// Dynamic combase.dll imports (avoids import-lib differences across MinGW/MSVC).
namespace {
using HSTRING_RAW = void*;
using FnRoGetActivationFactory = HRESULT(WINAPI*)(HSTRING_RAW, REFIID, void**);
using FnRoActivateInstance = HRESULT(WINAPI*)(HSTRING_RAW, void**);
using FnWindowsCreateString = HRESULT(WINAPI*)(LPCWSTR, UINT32, HSTRING_RAW*);
using FnWindowsDeleteString = HRESULT(WINAPI*)(HSTRING_RAW);

struct CombaseApi {
    FnRoGetActivationFactory getFactory = nullptr;
    FnRoActivateInstance activate = nullptr;
    FnWindowsCreateString createStr = nullptr;
    FnWindowsDeleteString deleteStr = nullptr;
    bool loaded = false;

    static const CombaseApi& get() {
        static CombaseApi api = [] {
            CombaseApi a;
            if (HMODULE m = LoadLibraryW(L"combase.dll")) {
                a.getFactory = reinterpret_cast<FnRoGetActivationFactory>(GetProcAddress(m, "RoGetActivationFactory"));
                a.activate = reinterpret_cast<FnRoActivateInstance>(GetProcAddress(m, "RoActivateInstance"));
                a.createStr = reinterpret_cast<FnWindowsCreateString>(GetProcAddress(m, "WindowsCreateString"));
                a.deleteStr = reinterpret_cast<FnWindowsDeleteString>(GetProcAddress(m, "WindowsDeleteString"));
                a.loaded = a.getFactory && a.activate && a.createStr && a.deleteStr;
            }
            return a;
        }();
        return api;
    }
};

inline HRESULT RoGetActivationFactory(HSTRING_RAW cls, REFIID iid, void** out) {
    auto& api = CombaseApi::get();
    return api.loaded ? api.getFactory(cls, iid, out) : E_FAIL;
}
inline HRESULT RoActivateInstance(HSTRING_RAW cls, void** out) {
    auto& api = CombaseApi::get();
    return api.loaded ? api.activate(cls, out) : E_FAIL;
}
inline HRESULT WindowsCreateString(LPCWSTR src, UINT32 len, HSTRING_RAW* out) {
    auto& api = CombaseApi::get();
    return api.loaded ? api.createStr(src, len, out) : E_FAIL;
}
inline HRESULT WindowsDeleteString(HSTRING_RAW hs) {
    auto& api = CombaseApi::get();
    return api.loaded ? api.deleteStr(hs) : S_OK;
}
} // namespace

namespace {

const GUID IID_Interop = { 0xddb0472d, 0xc911, 0x4a1f, { 0x86, 0xd9, 0xdc, 0x3d, 0x71, 0xa9, 0x5f, 0x5a } };
const GUID IID_Smtc = { 0x99fa3ff4, 0x1742, 0x42a6, { 0x90, 0x2e, 0x08, 0x7d, 0x41, 0xf9, 0x65, 0xec } };
const GUID IID_Smtc2 = { 0xea98d2f6, 0x7f3c, 0x4af2, { 0xa5, 0x86, 0x72, 0x88, 0x98, 0x08, 0xef, 0xb1 } };
const GUID IID_Timeline = { 0x5125316a, 0xc3a2, 0x475b, { 0x85, 0x07, 0x93, 0x53, 0x4d, 0xc8, 0x8f, 0x15 } };
const GUID IID_ButtonHandler = { 0x0557e996, 0x7b23, 0x5bae, { 0xaa, 0x81, 0xea, 0x0d, 0x67, 0x11, 0x43, 0xa4 } };

constexpr int SlotPutPlaybackStatus = 7, SlotGetDisplayUpdater = 8, SlotPutIsEnabled = 11, SlotPutIsPlayEnabled = 13,
    SlotPutIsStopEnabled = 15, SlotPutIsPauseEnabled = 17, SlotPutIsPreviousEnabled = 25, SlotPutIsNextEnabled = 27,
    SlotAddButtonPressed = 32, SlotRemoveButtonPressed = 33;
constexpr int SlotUpdateTimeline = 12;
constexpr int SlotPutType = 7, SlotGetMusicProps = 12, SlotGetVideoProps = 13, SlotClearAll = 16, SlotUpdate = 17;
constexpr int SlotPutTitle = 7, SlotPutMusicArtist = 11, SlotPutVideoSubtitle = 9;
constexpr int SlotPutStart = 7, SlotPutEnd = 9, SlotPutMinSeek = 11, SlotPutMaxSeek = 13, SlotPutPosition = 15;
constexpr int StatusClosed = 0, StatusStopped = 2, StatusPlaying = 3, StatusPaused = 4;
constexpr int TypeMusic = 1, TypeVideo = 2;

inline void* vtableSlot(void* obj, int index) {
    return (*reinterpret_cast<void***>(obj))[index];
}

inline void comRelease(void* p) {
    if (p) static_cast<IUnknown*>(p)->Release();
}

struct ScopedHString {
    HSTRING_RAW hs = nullptr;
    explicit ScopedHString(const std::wstring& s) {
        WindowsCreateString(s.c_str(), static_cast<UINT32>(s.size()), &hs);
    }
    ~ScopedHString() { if (hs) WindowsDeleteString(hs); }
};

inline HRESULT call0(void* obj, int slot) {
    using Fn = HRESULT(WINAPI*)(void*);
    return reinterpret_cast<Fn>(vtableSlot(obj, slot))(obj);
}
inline HRESULT setInt(void* obj, int slot, int v) {
    using Fn = HRESULT(WINAPI*)(void*, int);
    return reinterpret_cast<Fn>(vtableSlot(obj, slot))(obj, v);
}
inline HRESULT setLong(void* obj, int slot, int64_t v) {
    using Fn = HRESULT(WINAPI*)(void*, int64_t);
    return reinterpret_cast<Fn>(vtableSlot(obj, slot))(obj, v);
}
inline HRESULT setBool(void* obj, int slot, bool v) {
    using Fn = HRESULT(WINAPI*)(void*, uint8_t);
    return reinterpret_cast<Fn>(vtableSlot(obj, slot))(obj, v ? 1 : 0);
}
inline HRESULT setStr(void* obj, int slot, const std::wstring& s) {
    ScopedHString hs(s);
    using Fn = HRESULT(WINAPI*)(void*, HSTRING_RAW);
    return reinterpret_cast<Fn>(vtableSlot(obj, slot))(obj, hs.hs);
}

class SmtcButtonHandler final : public IUnknown {
public:
    explicit SmtcButtonHandler(std::function<void(int)> cb) : _cb(std::move(cb)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ButtonHandler) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --_refs;
        if (r == 0) delete this;
        return r;
    }
    virtual HRESULT STDMETHODCALLTYPE Invoke(void* /*sender*/, void* args) {
        if (args && _cb) {
            int btn = -1;
            using GetBtnFn = HRESULT(WINAPI*)(void*, int*);
            if (SUCCEEDED(reinterpret_cast<GetBtnFn>(vtableSlot(args, 6))(args, &btn))) {
                _cb(btn);
            }
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> _refs{ 1 };
    std::function<void(int)> _cb;
};

} // namespace

MediaControls::~MediaControls() { dispose(); }

void MediaControls::attach(HWND hwnd) {
    if (_smtc || !hwnd) return;
    ScopedHString cls(L"Windows.Media.SystemMediaTransportControls");
    void* factory = nullptr;
    if (FAILED(RoGetActivationFactory(cls.hs, IID_Interop, &factory)) || !factory) return;

    using GetForWindowFn = HRESULT(WINAPI*)(void*, HWND, REFIID, void**);
    HRESULT hr = reinterpret_cast<GetForWindowFn>(vtableSlot(factory, 6))(factory, hwnd, IID_Smtc, &_smtc);
    comRelease(factory);
    if (FAILED(hr) || !_smtc) { dispose(); return; }

    setBool(_smtc, SlotPutIsEnabled, true);
    setBool(_smtc, SlotPutIsPlayEnabled, true);
    setBool(_smtc, SlotPutIsPauseEnabled, true);
    setBool(_smtc, SlotPutIsStopEnabled, true);
    setBool(_smtc, SlotPutIsNextEnabled, true);
    setBool(_smtc, SlotPutIsPreviousEnabled, true);
    setInt(_smtc, SlotPutPlaybackStatus, StatusClosed);

    using GetPtrFn = HRESULT(WINAPI*)(void*, void**);
    if (FAILED(reinterpret_cast<GetPtrFn>(vtableSlot(_smtc, SlotGetDisplayUpdater))(_smtc, &_updater))) {
        dispose();
        return;
    }
    static_cast<IUnknown*>(_smtc)->QueryInterface(IID_Smtc2, &_smtc2);

    auto* handler = new SmtcButtonHandler([this](int btn) {
        if (!onButtonPressed) return;
        switch (btn) {
            case 0: onButtonPressed("Play"); break;
            case 1: onButtonPressed("Pause"); break;
            case 2: onButtonPressed("Stop"); break;
            case 6: onButtonPressed("Next"); break;
            case 7: onButtonPressed("Previous"); break;
            default: break;
        }
    });
    _handler = handler;
    using AddHandlerFn = HRESULT(WINAPI*)(void*, void*, int64_t*);
    reinterpret_cast<AddHandlerFn>(vtableSlot(_smtc, SlotAddButtonPressed))(_smtc, _handler, &_token);
}

void MediaControls::updateStatus(bool playing, bool paused) {
    if (!_smtc) return;
    setInt(_smtc, SlotPutPlaybackStatus, playing ? StatusPlaying : (paused ? StatusPaused : StatusStopped));
}

void MediaControls::updateMetadata(const std::wstring& title, const std::wstring& artist, bool isVideo, const std::wstring& /*artworkUrl*/) {
    if (!_updater) return;
    call0(_updater, SlotClearAll);
    setInt(_updater, SlotPutType, isVideo ? TypeVideo : TypeMusic);

    void* props = nullptr;
    using GetPtrFn = HRESULT(WINAPI*)(void*, void**);
    if (SUCCEEDED(reinterpret_cast<GetPtrFn>(vtableSlot(_updater, isVideo ? SlotGetVideoProps : SlotGetMusicProps))(_updater, &props)) && props) {
        setStr(props, SlotPutTitle, title);
        if (!artist.empty()) setStr(props, isVideo ? SlotPutVideoSubtitle : SlotPutMusicArtist, artist);
        comRelease(props);
    }
    call0(_updater, SlotUpdate);
}

void MediaControls::updateTimeline(int64_t positionMs, int64_t lengthMs) {
    if (!_smtc2 || lengthMs <= 0) return;
    ScopedHString cls(L"Windows.Media.SystemMediaTransportControlsTimelineProperties");
    void* insp = nullptr;
    if (FAILED(RoActivateInstance(cls.hs, &insp)) || !insp) return;
    void* tl = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(insp)->QueryInterface(IID_Timeline, &tl)) && tl) {
        constexpr int64_t TicksPerMs = 10000;
        setLong(tl, SlotPutStart, 0);
        setLong(tl, SlotPutMinSeek, 0);
        setLong(tl, SlotPutPosition, std::clamp<int64_t>(positionMs, 0, lengthMs) * TicksPerMs);
        setLong(tl, SlotPutMaxSeek, lengthMs * TicksPerMs);
        setLong(tl, SlotPutEnd, lengthMs * TicksPerMs);
        using UpdFn = HRESULT(WINAPI*)(void*, void*);
        reinterpret_cast<UpdFn>(vtableSlot(_smtc2, SlotUpdateTimeline))(_smtc2, tl);
        comRelease(tl);
    }
    comRelease(insp);
}

void MediaControls::clear() {
    if (!_smtc || !_updater) return;
    setInt(_smtc, SlotPutPlaybackStatus, StatusClosed);
    call0(_updater, SlotClearAll);
    call0(_updater, SlotUpdate);
}

void MediaControls::dispose() {
    if (_smtc && _token != 0) {
        using RemFn = HRESULT(WINAPI*)(void*, int64_t);
        reinterpret_cast<RemFn>(vtableSlot(_smtc, SlotRemoveButtonPressed))(_smtc, _token);
    }
    _token = 0;
    comRelease(_handler); _handler = nullptr;
    comRelease(_updater); _updater = nullptr;
    comRelease(_smtc2); _smtc2 = nullptr;
    comRelease(_smtc); _smtc = nullptr;
}

} // namespace reel
