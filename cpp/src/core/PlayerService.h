#pragma once
#include "core/MediaItem.h"
#include "core/Settings.h"
#include <vlc/vlc.h>
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace reel {

struct LogEntry {
    std::wstring timeStr;
    int level = 0; // libvlc_log_level
    std::wstring module;
    std::wstring message;
};

class LogBuffer {
public:
    explicit LogBuffer(size_t capacity = 5000) : _capacity(capacity) {}

    void add(LogEntry entry);
    std::vector<LogEntry> snapshot() const;
    void clear();
    void setOnAdded(std::function<void(const LogEntry&)> cb);

private:
    mutable std::mutex _mtx;
    std::deque<LogEntry> _entries;
    size_t _capacity;
    std::function<void(const LogEntry&)> _onAdded;
};

struct TrackOption {
    int id = -1;
    std::wstring name;
    bool selected = false;
};

struct AudioDeviceOption {
    std::string id;
    std::wstring name;
    bool selected = false;
};

struct ChapterMark {
    int64_t startMs = 0;
    double fraction = 0.0;
    std::wstring name;
};

struct MediaTrackInfo {
    std::wstring type;
    std::wstring codec;
    std::wstring details;
    std::wstring language;
};

struct MediaStatsInfo {
    int readBytes = 0;
    float inputBitrate = 0;
    int demuxReadBytes = 0;
    float demuxBitrate = 0;
    int decodedVideo = 0;
    int displayedPictures = 0;
    int lostPictures = 0;
    int decodedAudio = 0;
    int playedAbuffers = 0;
    int lostAbuffers = 0;
};

class PlayerService {
public:
    explicit PlayerService(const std::vector<std::string>& libVlcOptions, bool asyncInit = false);
    ~PlayerService();

    PlayerService(const PlayerService&) = delete;
    PlayerService& operator=(const PlayerService&) = delete;

    static std::vector<std::string> buildOptions(const AppSettings& s);
    static std::vector<std::string> videoFilterOptions(const std::string& transformType, bool sharpen, double sharpenSigma);
    static std::wstring vlcVersion();

    LogBuffer& log() { return _log; }
    MediaItemPtr currentItem() const;

    void setHwnd(HWND hwnd);

    // Events (raised on libVLC/worker threads; UI must PostMessage to marshal)
    std::function<void(std::wstring)> onInitFailed;
    std::function<void()> onOpening;
    std::function<void()> onStateChanged;
    std::function<void()> onEndReached;
    std::function<void()> onError;
    std::function<void(float)> onBuffering;
    std::function<void(int64_t)> onLengthChanged;
    std::function<void()> onTracksChanged;
    std::function<void(int)> onVoutChanged;
    std::function<void(std::wstring)> onSnapshotTaken;

    libvlc_state_t state() const;
    bool isActive() const;
    bool isSeekable() const;
    int64_t timeMs() const;
    int64_t lengthMs() const;
    float rate() const;
    int volume() const;
    bool isMuted() const;
    int64_t audioDelayUs() const;
    int64_t spuDelayUs() const;
    int videoTrackCount() const;
    unsigned voutCount() const;
    int audioChannel() const;

    void open(const MediaItemPtr& item, int64_t startMs = 0, const std::vector<std::string>& extraOptions = {});
    void stop();
    void stopKeepItem();
    void pause();
    void resume();
    void setTimeMs(int64_t ms, bool asyncCoalesced = false);
    void nextFrame();
    void setVolume(int vol);
    void setMute(bool mute);
    void setRate(float r);
    void setAudioDelayUs(int64_t us);
    void setSpuDelayUs(int64_t us);
    void addSubtitle(const std::wstring& path);

    std::vector<TrackOption> audioTracks() const;
    std::vector<TrackOption> subtitleTracks() const;
    std::vector<TrackOption> videoTracks() const;
    std::vector<TrackOption> titles() const;
    std::vector<ChapterMark> chapters() const;
    std::vector<AudioDeviceOption> audioDevices() const;

    void setAudioTrack(int id);
    void setSubtitleTrack(int id);
    void setVideoTrack(int id);
    void setTitle(int id);
    void setChapter(int index);
    void setAudioDevice(const std::string& id);
    void setAudioChannel(int channel);

    void setAspectRatio(const char* aspect);
    void setCropGeometry(const char* crop);
    void setScale(float scale);
    void setDeinterlace(const char* mode);

    void setAdjust(bool enabled, float brightness, float contrast, float hue, float saturation, float gamma);
    bool takeSnapshot(const std::wstring& filePath);

    // Equalizer
    static std::vector<std::wstring> equalizerPresets();
    static std::vector<std::wstring> equalizerBandLabels();
    static void loadEqualizerPresetValues(unsigned preset, float& outPreamp, float outBands[10]);
    void applyEqualizer(bool enabled, float preamp, const float bands[10]);

    // Metadata / Info
    std::wstring meta(libvlc_meta_t type) const;
    std::vector<MediaTrackInfo> mediaTracks() const;
    MediaStatsInfo mediaStats() const;
    double videoFps() const;

    // Engine rebuild for transform/sharpen video filters
    std::future<bool> rebuildAsync(const std::vector<std::string>& extraOptions);
    void releaseRetired();

    void post(std::function<void()> fn);

private:
    struct EnginePair {
        libvlc_instance_t* lib = nullptr;
        libvlc_media_player_t* mp = nullptr;
        libvlc_media_t* media = nullptr;
    };

    EnginePair createEngine(const std::vector<std::string>& options);
    void disposeRetired();
    void workLoop();

    static void vlcLogCallback(void* data, int level, const libvlc_log_t* ctx, const char* fmt, va_list args);
    static void vlcEventCallback(const libvlc_event_t* ev, void* userData);

    std::vector<std::string> _baseOptions;
    std::atomic<libvlc_instance_t*> _lib{ nullptr };
    std::atomic<libvlc_media_player_t*> _mp{ nullptr };
    std::atomic<libvlc_media_t*> _media{ nullptr };
    std::atomic<HWND> _hwnd{ nullptr };
    std::atomic<int64_t> _pendingSeekMs{ -1 };
    libvlc_equalizer_t* _eq = nullptr;

    std::vector<EnginePair> _retired;
    mutable std::mutex _itemMtx;
    MediaItemPtr _currentItem;

    LogBuffer _log;

    std::mutex _qMtx;
    std::condition_variable _qCv;
    std::deque<std::function<void()>> _queue;
    bool _stopping = false;
    std::thread _worker;
};

} // namespace reel
