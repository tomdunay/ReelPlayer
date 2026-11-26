#include "core/PlayerService.h"
#include "core/StrUtil.h"
#include "core/TimeFormat.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace reel {

static std::wstring nowTimeStr() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

void LogBuffer::add(LogEntry entry) {
    std::function<void(const LogEntry&)> cb;
    {
        std::lock_guard lk(_mtx);
        _entries.push_back(entry);
        if (_entries.size() > _capacity) _entries.pop_front();
        cb = _onAdded;
    }
    if (cb) cb(entry);
}

std::vector<LogEntry> LogBuffer::snapshot() const {
    std::lock_guard lk(_mtx);
    return { _entries.begin(), _entries.end() };
}

void LogBuffer::clear() {
    std::lock_guard lk(_mtx);
    _entries.clear();
}

void LogBuffer::setOnAdded(std::function<void(const LogEntry&)> cb) {
    std::lock_guard lk(_mtx);
    _onAdded = std::move(cb);
}

static int parseHexColor(const std::wstring& hex) {
    std::wstring s = hex;
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() == 8) s.erase(0, 2);
    wchar_t* end = nullptr;
    unsigned long v = std::wcstoul(s.c_str(), &end, 16);
    return (end && end != s.c_str()) ? static_cast<int>(v & 0xFFFFFF) : 0xFFFFFF;
}

std::vector<std::string> PlayerService::buildOptions(const AppSettings& s) {
    std::string hw = "any";
    if (!s.hardwareDecoding || s.hwDecodeMode == L"Off") hw = "none";
    else if (s.hwDecodeMode == L"D3D11") hw = "d3d11va";
    else if (s.hwDecodeMode == L"DXVA2") hw = "dxva2";

    int fileCache = std::clamp(s.fileCachingMs, 50, 10000);
    int netCache = std::clamp(s.networkCachingMs, 100, 30000);

    std::vector<std::string> opts = {
        "--no-video-title-show",
        "--no-snapshot-preview",
        "--no-stats",
        "--avcodec-hw=" + hw,
        "--file-caching=" + std::to_string(fileCache),
        "--network-caching=" + std::to_string(netCache),
        std::string("--snapshot-format=") + (s.snapshotFormat == L"jpg" ? "jpg" : "png"),
        "--freetype-rel-fontsize=" + std::to_string(SettingsService::subtitleSizeToVlc(s.subtitleSize)),
        "--freetype-outline-thickness=" + std::to_string(SettingsService::subtitleOutlineToVlc(s.subtitleOutline)),
        "--freetype-color=" + std::to_string(parseHexColor(s.subtitleColor)),
    };
    if (s.fastSeek) opts.push_back("--input-fast-seek");
    auto lang = trim(s.subtitleLanguage);
    if (!lang.empty()) opts.push_back("--sub-language=" + toUtf8(lang));
    auto enc = trim(s.subtitleEncoding);
    if (!enc.empty()) opts.push_back("--subsdec-encoding=" + toUtf8(enc));
    if (!s.autoLoadSubtitles) opts.push_back("--no-sub-autodetect-file");
    return opts;
}

std::vector<std::string> PlayerService::videoFilterOptions(const std::string& transformType, bool sharpen, double sharpenSigma) {
    std::vector<std::string> filters;
    std::vector<std::string> opts;
    if (!transformType.empty()) {
        filters.push_back("transform");
        opts.push_back("--transform-type=" + transformType);
    }
    if (sharpen) {
        filters.push_back("sharpen");
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3g", sharpenSigma);
        opts.push_back(std::string("--sharpen-sigma=") + buf);
    }
    if (!filters.empty()) {
        std::string joined = filters[0];
        for (size_t i = 1; i < filters.size(); ++i) joined += ":" + filters[i];
        opts.insert(opts.begin(), "--video-filter=" + joined);
    }
    return opts;
}

std::wstring PlayerService::vlcVersion() {
    const char* v = libvlc_get_version();
    return v ? toWide(v) : L"3.0";
}

void PlayerService::vlcLogCallback(void* data, int level, const libvlc_log_t* ctx, const char* fmt, va_list args) {
    auto* self = static_cast<PlayerService*>(data);
    if (!self) return;
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    const char* mod = nullptr;
    if (ctx) libvlc_log_get_context(ctx, &mod, nullptr, nullptr);
    self->_log.add(LogEntry{ nowTimeStr(), level, mod ? toWide(mod) : L"", toWide(buf) });
}

void PlayerService::vlcEventCallback(const libvlc_event_t* ev, void* userData) {
    auto* self = static_cast<PlayerService*>(userData);
    if (!self || !ev) return;
    if (ev->p_obj != self->_mp.load()) return;

    switch (ev->type) {
        case libvlc_MediaPlayerOpening:
            if (self->onOpening) self->onOpening();
            break;
        case libvlc_MediaPlayerPlaying:
        case libvlc_MediaPlayerPaused:
        case libvlc_MediaPlayerStopped:
            if (self->onStateChanged) self->onStateChanged();
            break;
        case libvlc_MediaPlayerEndReached:
            if (self->onEndReached) self->onEndReached();
            break;
        case libvlc_MediaPlayerEncounteredError:
            if (self->onError) self->onError();
            break;
        case libvlc_MediaPlayerBuffering:
            if (self->onBuffering) self->onBuffering(ev->u.media_player_buffering.new_cache);
            break;
        case libvlc_MediaPlayerLengthChanged:
            if (self->onLengthChanged) self->onLengthChanged(ev->u.media_player_length_changed.new_length);
            break;
        case libvlc_MediaPlayerESAdded:
        case libvlc_MediaPlayerESDeleted:
        case libvlc_MediaPlayerESSelected:
        case libvlc_MediaPlayerChapterChanged:
        case libvlc_MediaPlayerTitleChanged:
            if (self->onTracksChanged) self->onTracksChanged();
            break;
        case libvlc_MediaPlayerVout:
            if (self->onVoutChanged) self->onVoutChanged(ev->u.media_player_vout.new_count);
            break;
        case libvlc_MediaPlayerSnapshotTaken:
            if (self->onSnapshotTaken && ev->u.media_player_snapshot_taken.psz_filename)
                self->onSnapshotTaken(toWide(ev->u.media_player_snapshot_taken.psz_filename));
            break;
        default:
            break;
    }
}

PlayerService::EnginePair PlayerService::createEngine(const std::vector<std::string>& options) {
    std::vector<const char*> argv;
    argv.reserve(options.size());
    for (const auto& o : options) argv.push_back(o.c_str());

    libvlc_instance_t* lib = libvlc_new(static_cast<int>(argv.size()), argv.empty() ? nullptr : argv.data());
    if (!lib) throw std::runtime_error("libvlc_new failed. Ensure libvlc\\win-x64 is next to ReelPlayer.exe.");

    libvlc_log_set(lib, &PlayerService::vlcLogCallback, this);

    libvlc_media_player_t* mp = libvlc_media_player_new(lib);
    if (!mp) {
        libvlc_release(lib);
        throw std::runtime_error("libvlc_media_player_new failed.");
    }
    libvlc_video_set_key_input(mp, 0);
    libvlc_video_set_mouse_input(mp, 0);

    if (HWND h = _hwnd.load()) libvlc_media_player_set_hwnd(mp, h);

    libvlc_event_manager_t* em = libvlc_media_player_event_manager(mp);
    static constexpr libvlc_event_e events[] = {
        libvlc_MediaPlayerOpening, libvlc_MediaPlayerPlaying, libvlc_MediaPlayerPaused,
        libvlc_MediaPlayerStopped, libvlc_MediaPlayerEndReached, libvlc_MediaPlayerEncounteredError,
        libvlc_MediaPlayerBuffering, libvlc_MediaPlayerLengthChanged,
        libvlc_MediaPlayerESAdded, libvlc_MediaPlayerESDeleted, libvlc_MediaPlayerESSelected,
        libvlc_MediaPlayerChapterChanged, libvlc_MediaPlayerTitleChanged,
        libvlc_MediaPlayerVout, libvlc_MediaPlayerSnapshotTaken
    };
    for (auto e : events) libvlc_event_attach(em, e, &PlayerService::vlcEventCallback, this);
    return EnginePair{ lib, mp, nullptr };
}

PlayerService::PlayerService(const std::vector<std::string>& libVlcOptions, bool asyncInit)
    : _baseOptions(libVlcOptions) {
    if (asyncInit) {
        _worker = std::thread(&PlayerService::workLoop, this);
        post([this] {
            try {
                auto pair = createEngine(_baseOptions);
                _lib.store(pair.lib);
                _mp.store(pair.mp);
            } catch (const std::exception& ex) {
                if (onInitFailed) onInitFailed(toWide(ex.what()));
            }
        });
    } else {
        auto pair = createEngine(_baseOptions);
        _lib.store(pair.lib);
        _mp.store(pair.mp);
        _worker = std::thread(&PlayerService::workLoop, this);
    }
}

PlayerService::~PlayerService() {
    post([this] {
        if (auto* mp = _mp.load()) libvlc_media_player_stop(mp);
    });
    {
        std::lock_guard lk(_qMtx);
        _stopping = true;
    }
    _qCv.notify_all();
    if (_worker.joinable()) _worker.join();

    disposeRetired();
    if (_eq) { libvlc_audio_equalizer_release(_eq); _eq = nullptr; }
    if (auto* mp = _mp.exchange(nullptr)) libvlc_media_player_release(mp);
    if (auto* m = _media.exchange(nullptr)) libvlc_media_release(m);
    if (auto* lib = _lib.exchange(nullptr)) {
        libvlc_log_unset(lib);
        libvlc_release(lib);
    }
}

void PlayerService::setHwnd(HWND hwnd) {
    _hwnd.store(hwnd);
    if (auto* mp = _mp.load()) libvlc_media_player_set_hwnd(mp, hwnd);
}

MediaItemPtr PlayerService::currentItem() const {
    std::lock_guard lk(_itemMtx);
    return _currentItem;
}

libvlc_state_t PlayerService::state() const {
    auto* mp = _mp.load();
    return mp ? libvlc_media_player_get_state(mp) : libvlc_NothingSpecial;
}

bool PlayerService::isActive() const {
    auto s = state();
    return s == libvlc_Playing || s == libvlc_Paused || s == libvlc_Opening || s == libvlc_Buffering;
}

bool PlayerService::isSeekable() const {
    auto* mp = _mp.load();
    return mp && libvlc_media_player_is_seekable(mp) != 0;
}

int64_t PlayerService::timeMs() const {
    auto* mp = _mp.load();
    return mp ? libvlc_media_player_get_time(mp) : 0;
}

int64_t PlayerService::lengthMs() const {
    auto* mp = _mp.load();
    return mp ? libvlc_media_player_get_length(mp) : 0;
}

float PlayerService::rate() const {
    auto* mp = _mp.load();
    return mp ? libvlc_media_player_get_rate(mp) : 1.0f;
}

int PlayerService::volume() const {
    auto* mp = _mp.load();
    return mp ? libvlc_audio_get_volume(mp) : 100;
}

bool PlayerService::isMuted() const {
    auto* mp = _mp.load();
    return mp && libvlc_audio_get_mute(mp) > 0;
}

int64_t PlayerService::audioDelayUs() const {
    auto* mp = _mp.load();
    return mp ? libvlc_audio_get_delay(mp) : 0;
}

int64_t PlayerService::spuDelayUs() const {
    auto* mp = _mp.load();
    return mp ? libvlc_video_get_spu_delay(mp) : 0;
}

int PlayerService::videoTrackCount() const {
    auto* mp = _mp.load();
    return mp ? libvlc_video_get_track_count(mp) : 0;
}

unsigned PlayerService::voutCount() const {
    auto* mp = _mp.load();
    return mp ? libvlc_media_player_has_vout(mp) : 0;
}

int PlayerService::audioChannel() const {
    auto* mp = _mp.load();
    return mp ? libvlc_audio_get_channel(mp) : libvlc_AudioChannel_Stereo;
}

void PlayerService::open(const MediaItemPtr& item, int64_t startMs, const std::vector<std::string>& extraOptions) {
    {
        std::lock_guard lk(_itemMtx);
        _currentItem = item;
    }
    post([this, item, startMs, extraOptions] {
        auto* lib = _lib.load();
        auto* mp = _mp.load();
        if (!lib || !mp || !item) return;

        std::string mrl = item->mrlUtf8();
        libvlc_media_t* media = libvlc_media_new_location(lib, mrl.c_str());
        if (!media) return;

        for (const auto& o : item->options) libvlc_media_add_option(media, toUtf8(o).c_str());
        for (const auto& o : extraOptions) libvlc_media_add_option(media, o.c_str());
        if (startMs > 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), ":start-time=%.3f", startMs / 1000.0);
            libvlc_media_add_option(media, buf);
        }
        if (!item->subtitleFile.empty()) {
            std::string subUri = toUtf8(localPathToFileUri(item->subtitleFile));
            libvlc_media_slaves_add(media, libvlc_media_slave_type_subtitle, 4, subUri.c_str());
        }

        libvlc_media_player_set_media(mp, media);
        if (HWND h = _hwnd.load()) libvlc_media_player_set_hwnd(mp, h);
        libvlc_media_player_play(mp);

        if (auto* old = _media.exchange(media)) libvlc_media_release(old);
    });
}

void PlayerService::stop() {
    {
        std::lock_guard lk(_itemMtx);
        _currentItem = nullptr;
    }
    post([this] {
        if (auto* mp = _mp.load()) libvlc_media_player_stop(mp);
    });
}

void PlayerService::stopKeepItem() {
    post([this] {
        if (auto* mp = _mp.load()) libvlc_media_player_stop(mp);
    });
}

void PlayerService::pause() {
    post([this] {
        if (auto* mp = _mp.load()) libvlc_media_player_set_pause(mp, 1);
    });
}

void PlayerService::resume() {
    post([this] {
        if (auto* mp = _mp.load()) libvlc_media_player_set_pause(mp, 0);
    });
}

void PlayerService::setTimeMs(int64_t ms, bool asyncCoalesced) {
    if (asyncCoalesced) {
        bool alreadyPending = (_pendingSeekMs.exchange(ms) >= 0);
        if (!alreadyPending) {
            post([this] {
                int64_t target = _pendingSeekMs.exchange(-1);
                if (target >= 0) {
                    if (auto* mp = _mp.load()) libvlc_media_player_set_time(mp, target);
                }
            });
        }
        return;
    }
    _pendingSeekMs.store(-1);
    if (auto* mp = _mp.load()) libvlc_media_player_set_time(mp, ms);
}

void PlayerService::nextFrame() {
    if (auto* mp = _mp.load()) libvlc_media_player_next_frame(mp);
}

void PlayerService::setVolume(int vol) {
    if (auto* mp = _mp.load()) libvlc_audio_set_volume(mp, vol);
}

void PlayerService::setMute(bool mute) {
    if (auto* mp = _mp.load()) libvlc_audio_set_mute(mp, mute ? 1 : 0);
}

void PlayerService::setRate(float r) {
    if (auto* mp = _mp.load()) libvlc_media_player_set_rate(mp, r);
}

void PlayerService::setAudioDelayUs(int64_t us) {
    if (auto* mp = _mp.load()) libvlc_audio_set_delay(mp, us);
}

void PlayerService::setSpuDelayUs(int64_t us) {
    if (auto* mp = _mp.load()) libvlc_video_set_spu_delay(mp, us);
}

void PlayerService::addSubtitle(const std::wstring& path) {
    post([this, path] {
        if (auto* mp = _mp.load()) {
            std::string uri = toUtf8(localPathToFileUri(path));
            libvlc_media_player_add_slave(mp, libvlc_media_slave_type_subtitle, uri.c_str(), true);
        }
    });
}

static std::vector<TrackOption> collectTracks(libvlc_track_description_t* head, int current) {
    std::vector<TrackOption> out;
    for (auto* p = head; p; p = p->p_next) {
        std::wstring name = p->psz_name ? toWide(p->psz_name) : (L"Track " + std::to_wstring(p->i_id));
        out.push_back(TrackOption{ p->i_id, std::move(name), p->i_id == current });
    }
    if (head) libvlc_track_description_list_release(head);
    return out;
}

std::vector<TrackOption> PlayerService::audioTracks() const {
    auto* mp = _mp.load();
    if (!mp) return {};
    return collectTracks(libvlc_audio_get_track_description(mp), libvlc_audio_get_track(mp));
}

std::vector<TrackOption> PlayerService::subtitleTracks() const {
    auto* mp = _mp.load();
    if (!mp) return {};
    return collectTracks(libvlc_video_get_spu_description(mp), libvlc_video_get_spu(mp));
}

std::vector<TrackOption> PlayerService::videoTracks() const {
    auto* mp = _mp.load();
    if (!mp) return {};
    return collectTracks(libvlc_video_get_track_description(mp), libvlc_video_get_track(mp));
}

std::vector<TrackOption> PlayerService::titles() const {
    auto* mp = _mp.load();
    if (!mp) return {};
    return collectTracks(libvlc_video_get_title_description(mp), libvlc_media_player_get_title(mp));
}

std::vector<ChapterMark> PlayerService::chapters() const {
    auto* mp = _mp.load();
    if (!mp) return {};
    int64_t len = libvlc_media_player_get_length(mp);
    libvlc_chapter_description_t** descs = nullptr;
    int count = libvlc_media_player_get_full_chapter_descriptions(mp, -1, &descs);
    std::vector<ChapterMark> out;
    if (count > 0 && descs) {
        for (int i = 0; i < count; ++i) {
            int64_t off = std::max<int64_t>(0, descs[i]->i_time_offset);
            double frac = len > 0 ? std::clamp(static_cast<double>(off) / static_cast<double>(len), 0.0, 1.0) : 0.0;
            std::wstring name = (descs[i]->psz_name && descs[i]->psz_name[0])
                ? toWide(descs[i]->psz_name)
                : (L"Chapter " + std::to_wstring(i + 1));
            out.push_back(ChapterMark{ off, frac, std::move(name) });
        }
        libvlc_chapter_descriptions_release(descs, count);
    }
    return out;
}

std::vector<AudioDeviceOption> PlayerService::audioDevices() const {
    auto* mp = _mp.load();
    if (!mp) return {};
    char* cur = libvlc_audio_output_device_get(mp);
    std::string curId = cur ? cur : "";
    if (cur) libvlc_free(cur);

    std::vector<AudioDeviceOption> out;
    auto* head = libvlc_audio_output_device_enum(mp);
    for (auto* p = head; p; p = p->p_next) {
        std::string id = p->psz_device ? p->psz_device : "";
        std::wstring name = (p->psz_description && p->psz_description[0]) ? toWide(p->psz_description) : L"Default";
        out.push_back(AudioDeviceOption{ id, std::move(name), id == curId });
    }
    if (head) libvlc_audio_output_device_list_release(head);
    return out;
}

void PlayerService::setAudioTrack(int id) { if (auto* mp = _mp.load()) libvlc_audio_set_track(mp, id); }
void PlayerService::setSubtitleTrack(int id) { if (auto* mp = _mp.load()) libvlc_video_set_spu(mp, id); }
void PlayerService::setVideoTrack(int id) { if (auto* mp = _mp.load()) libvlc_video_set_track(mp, id); }
void PlayerService::setTitle(int id) { if (auto* mp = _mp.load()) libvlc_media_player_set_title(mp, id); }
void PlayerService::setChapter(int index) { if (auto* mp = _mp.load()) libvlc_media_player_set_chapter(mp, index); }
void PlayerService::setAudioDevice(const std::string& id) {
    if (auto* mp = _mp.load()) libvlc_audio_output_device_set(mp, nullptr, id.c_str());
}
void PlayerService::setAudioChannel(int channel) { if (auto* mp = _mp.load()) libvlc_audio_set_channel(mp, channel); }

void PlayerService::setAspectRatio(const char* aspect) { if (auto* mp = _mp.load()) libvlc_video_set_aspect_ratio(mp, aspect); }
void PlayerService::setCropGeometry(const char* crop) { if (auto* mp = _mp.load()) libvlc_video_set_crop_geometry(mp, crop); }
void PlayerService::setScale(float scale) { if (auto* mp = _mp.load()) libvlc_video_set_scale(mp, scale); }
void PlayerService::setDeinterlace(const char* mode) { if (auto* mp = _mp.load()) libvlc_video_set_deinterlace(mp, mode); }

void PlayerService::setAdjust(bool enabled, float brightness, float contrast, float hue, float saturation, float gamma) {
    auto* mp = _mp.load();
    if (!mp) return;
    libvlc_video_set_adjust_int(mp, libvlc_adjust_Enable, enabled ? 1 : 0);
    if (!enabled) return;
    libvlc_video_set_adjust_float(mp, libvlc_adjust_Brightness, brightness);
    libvlc_video_set_adjust_float(mp, libvlc_adjust_Contrast, contrast);
    libvlc_video_set_adjust_float(mp, libvlc_adjust_Hue, hue);
    libvlc_video_set_adjust_float(mp, libvlc_adjust_Saturation, saturation);
    libvlc_video_set_adjust_float(mp, libvlc_adjust_Gamma, gamma);
}

bool PlayerService::takeSnapshot(const std::wstring& filePath) {
    auto* mp = _mp.load();
    if (!mp) return false;
    return libvlc_video_take_snapshot(mp, 0, toUtf8(filePath).c_str(), 0, 0) == 0;
}

std::vector<std::wstring> PlayerService::equalizerPresets() {
    std::vector<std::wstring> out;
    unsigned count = libvlc_audio_equalizer_get_preset_count();
    for (unsigned i = 0; i < count; ++i) {
        const char* name = libvlc_audio_equalizer_get_preset_name(i);
        out.push_back(name ? toWide(name) : (L"Preset " + std::to_wstring(i + 1)));
    }
    return out;
}

std::vector<std::wstring> PlayerService::equalizerBandLabels() {
    std::vector<std::wstring> out;
    unsigned count = libvlc_audio_equalizer_get_band_count();
    for (unsigned i = 0; i < count; ++i) {
        float f = libvlc_audio_equalizer_get_band_frequency(i);
        wchar_t buf[32];
        if (f >= 1000.0f) std::swprintf(buf, 32, L"%.1fk", f / 1000.0f);
        else std::swprintf(buf, 32, L"%.0f", f);
        out.push_back(buf);
    }
    return out;
}

void PlayerService::loadEqualizerPresetValues(unsigned preset, float& outPreamp, float outBands[10]) {
    auto* eq = libvlc_audio_equalizer_new_from_preset(preset);
    if (!eq) return;
    outPreamp = libvlc_audio_equalizer_get_preamp(eq);
    unsigned bands = libvlc_audio_equalizer_get_band_count();
    for (unsigned i = 0; i < 10 && i < bands; ++i) outBands[i] = libvlc_audio_equalizer_get_amp_at_index(eq, i);
    libvlc_audio_equalizer_release(eq);
}

void PlayerService::applyEqualizer(bool enabled, float preamp, const float bands[10]) {
    auto* mp = _mp.load();
    if (!mp) return;
    if (!enabled) {
        libvlc_media_player_set_equalizer(mp, nullptr);
        return;
    }
    if (!_eq) _eq = libvlc_audio_equalizer_new();
    libvlc_audio_equalizer_set_preamp(_eq, preamp);
    unsigned count = libvlc_audio_equalizer_get_band_count();
    for (unsigned i = 0; i < 10 && i < count; ++i) libvlc_audio_equalizer_set_amp_at_index(_eq, bands[i], i);
    libvlc_media_player_set_equalizer(mp, _eq);
}

std::wstring PlayerService::meta(libvlc_meta_t type) const {
    auto* m = _media.load();
    if (!m) return {};
    char* s = libvlc_media_get_meta(m, type);
    if (!s) return {};
    std::wstring out = toWide(s);
    libvlc_free(s);
    return out;
}

std::vector<MediaTrackInfo> PlayerService::mediaTracks() const {
    std::vector<MediaTrackInfo> out;
    auto* m = _media.load();
    if (!m) return out;
    libvlc_media_track_t** tracks = nullptr;
    unsigned count = libvlc_media_tracks_get(m, &tracks);
    for (unsigned i = 0; i < count; ++i) {
        const auto* t = tracks[i];
        const char* desc = libvlc_media_get_codec_description(t->i_type, t->i_codec);
        MediaTrackInfo info;
        info.codec = desc ? toWide(desc) : L"Unknown";
        info.language = t->psz_language ? toWide(t->psz_language) : L"";
        if (t->i_type == libvlc_track_video && t->video) {
            info.type = L"Video";
            wchar_t buf[128];
            double fps = t->video->i_frame_rate_den > 0
                ? static_cast<double>(t->video->i_frame_rate_num) / t->video->i_frame_rate_den : 0.0;
            std::swprintf(buf, 128, L"%ux%u, %.2f fps", t->video->i_width, t->video->i_height, fps);
            info.details = buf;
        } else if (t->i_type == libvlc_track_audio && t->audio) {
            info.type = L"Audio";
            wchar_t buf[128];
            std::swprintf(buf, 128, L"%u ch, %u Hz", t->audio->i_channels, t->audio->i_rate);
            info.details = buf;
        } else if (t->i_type == libvlc_track_text) {
            info.type = L"Subtitle";
            if (t->psz_description) info.details = toWide(t->psz_description);
        } else {
            info.type = L"Other";
        }
        out.push_back(std::move(info));
    }
    if (tracks) libvlc_media_tracks_release(tracks, count);
    return out;
}

double PlayerService::videoFps() const {
    auto* m = _media.load();
    if (!m) return 0.0;
    libvlc_media_track_t** tracks = nullptr;
    unsigned count = libvlc_media_tracks_get(m, &tracks);
    double fps = 0.0;
    for (unsigned i = 0; i < count; ++i) {
        if (tracks[i]->i_type == libvlc_track_video && tracks[i]->video &&
            tracks[i]->video->i_frame_rate_den > 0 && tracks[i]->video->i_frame_rate_num > 0) {
            fps = static_cast<double>(tracks[i]->video->i_frame_rate_num) / tracks[i]->video->i_frame_rate_den;
            break;
        }
    }
    if (tracks) libvlc_media_tracks_release(tracks, count);
    return (fps >= 1.0 && fps <= 240.0) ? fps : 0.0;
}

bool PlayerService::videoSize(unsigned& outW, unsigned& outH) const {
    outW = 0;
    outH = 0;
    if (auto* mp = _mp.load()) {
        if (libvlc_video_get_size(mp, 0, &outW, &outH) == 0 && outW > 0 && outH > 0) return true;
    }
    auto* m = _media.load();
    if (!m) return false;
    libvlc_media_track_t** tracks = nullptr;
    unsigned count = libvlc_media_tracks_get(m, &tracks);
    bool ok = false;
    for (unsigned i = 0; i < count; ++i) {
        if (tracks[i]->i_type == libvlc_track_video && tracks[i]->video &&
            tracks[i]->video->i_width > 0 && tracks[i]->video->i_height > 0) {
            outW = tracks[i]->video->i_width;
            outH = tracks[i]->video->i_height;
            ok = true;
            break;
        }
    }
    if (tracks) libvlc_media_tracks_release(tracks, count);
    return ok;
}

MediaStatsInfo PlayerService::mediaStats() const {
    MediaStatsInfo out{};
    auto* m = _media.load();
    if (!m) return out;
    libvlc_media_stats_t st{};
    if (libvlc_media_get_stats(m, &st)) {
        out.readBytes = st.i_read_bytes;
        out.inputBitrate = st.f_input_bitrate;
        out.demuxReadBytes = st.i_demux_read_bytes;
        out.demuxBitrate = st.f_demux_bitrate;
        out.decodedVideo = st.i_decoded_video;
        out.displayedPictures = st.i_displayed_pictures;
        out.lostPictures = st.i_lost_pictures;
        out.decodedAudio = st.i_decoded_audio;
        out.playedAbuffers = st.i_played_abuffers;
        out.lostAbuffers = st.i_lost_abuffers;
    }
    return out;
}

std::future<bool> PlayerService::rebuildAsync(const std::vector<std::string>& extraOptions) {
    auto opts = _baseOptions;
    opts.insert(opts.end(), extraOptions.begin(), extraOptions.end());
    auto prom = std::make_shared<std::promise<bool>>();
    auto fut = prom->get_future();
    post([this, opts = std::move(opts), prom] {
        try {
            if (auto* mp = _mp.load()) libvlc_media_player_stop(mp);
            auto next = createEngine(opts);
            EnginePair old{ _lib.exchange(next.lib), _mp.exchange(next.mp), _media.exchange(nullptr) };
            _retired.push_back(old);
            prom->set_value(true);
        } catch (...) {
            prom->set_value(false);
        }
    });
    return fut;
}

void PlayerService::releaseRetired() {
    post([this] { disposeRetired(); });
}

void PlayerService::disposeRetired() {
    for (auto& r : _retired) {
        if (r.mp) libvlc_media_player_release(r.mp);
        if (r.media) libvlc_media_release(r.media);
        if (r.lib) {
            libvlc_log_unset(r.lib);
            libvlc_release(r.lib);
        }
    }
    _retired.clear();
}

void PlayerService::post(std::function<void()> fn) {
    {
        std::lock_guard lk(_qMtx);
        if (_stopping) return;
        _queue.push_back(std::move(fn));
    }
    _qCv.notify_one();
}

void PlayerService::workLoop() {
    while (true) {
        std::function<void()> fn;
        {
            std::unique_lock lk(_qMtx);
            _qCv.wait(lk, [this] { return _stopping || !_queue.empty(); });
            if (_queue.empty() && _stopping) break;
            fn = std::move(_queue.front());
            _queue.pop_front();
        }
        try {
            fn();
        } catch (const std::exception& ex) {
            _log.add(LogEntry{ nowTimeStr(), LIBVLC_ERROR, L"reelplayer", toWide(ex.what()) });
        }
    }
}

} // namespace reel
