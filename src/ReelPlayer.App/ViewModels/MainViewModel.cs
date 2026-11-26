using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using LibVLCSharp.Shared;
using ReelPlayer.App.Services;
using ReelPlayer.Core.Models;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.ViewModels;

public sealed record TrackOption(int Id, string Name, bool IsSelected);
public sealed record ChapterMark(long StartMs, double Fraction, string Name);

/// <summary>Playback state and operations. UI-only concerns (dialogs, fullscreen, menus) live in MainWindow.</summary>
public sealed partial class MainViewModel : ObservableObject, IDisposable
{
    public static readonly float[] Rates = { 0.25f, 0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1f, 1.1f, 1.25f, 1.5f, 1.75f, 2f, 2.5f, 3f, 4f };
    public static readonly string[] AspectRatios = { "", "16:9", "4:3", "1:1", "16:10", "2.21:1", "2.35:1", "2.39:1", "5:4" };
    public static readonly string[] CropRatios = { "", "16:10", "16:9", "4:3", "1.85:1", "2.21:1", "2.35:1", "2.39:1", "5:3", "5:4", "1:1" };
    public static readonly (string Name, float Scale)[] Zooms =
        { ("Fit to window", 0f), ("1:4 Quarter", 0.25f), ("1:2 Half", 0.5f), ("1:1 Original", 1f), ("2:1 Double", 2f) };
    public static readonly (string Name, string? Mode)[] DeinterlaceModes =
    {
        ("Off", null), ("Blend", "blend"), ("Bob", "bob"), ("Discard", "discard"), ("Linear", "linear"), ("Mean", "mean"),
        ("X", "x"), ("Yadif", "yadif"), ("Yadif (2x)", "yadif2x"), ("Phosphor", "phosphor"), ("Film NTSC (IVTC)", "ivtc"),
    };
    public static readonly (string Name, AudioOutputChannel Channel)[] StereoModes =
    {
        ("Stereo", AudioOutputChannel.Stereo), ("Reverse stereo", AudioOutputChannel.RStereo),
        ("Left", AudioOutputChannel.Left), ("Right", AudioOutputChannel.Right), ("Dolby Surround", AudioOutputChannel.Dolbys),
    };
    public static readonly (string Name, string? Type)[] Transforms =
    {
        ("None", null), ("Rotate 90°", "90"), ("Rotate 180°", "180"), ("Rotate 270°", "270"),
        ("Flip horizontally", "hflip"), ("Flip vertically", "vflip"), ("Transpose", "transpose"), ("Anti-transpose", "antitranspose"),
    };

    private readonly Dispatcher _ui;
    private readonly DispatcherTimer _timer;
    private readonly DispatcherTimer _resumeTimer;
    private (MediaItem Item, long Position)? _resumeOffer;
    private int _consecutiveErrors;
    private bool _appliedForCurrentMedia;
    private int _timelineTick;
    private string? _recordPath;
    private Equalizer? _equalizer;
    private RestartState? _restart;
    private bool _rebuilding;
    private bool _rebuildAgain;
    private int _playGeneration;
    private string? _pendingFilterOsd;
    private bool _pauseWhenPictureShows;

    private sealed record RestartState(bool WasPaused, string? Osd, long AudioDelayUs, long SpuDelayUs);

    public MainViewModel(SettingsService settings, PlayerService engine, MediaControls smtc, ResumeStore? resume = null)
    {
        _ui = Dispatcher.CurrentDispatcher;
        SettingsService = settings;
        Engine = engine;
        Smtc = smtc;
        Resume = resume ?? new ResumeStore();

        Playlist = new Playlist { Loop = Settings.Loop, Shuffle = Settings.Shuffle };
        Volume = Math.Clamp(Settings.Volume, 0, 200);
        IsMuted = Settings.Muted;
        ShowRemaining = Settings.ShowRemainingTime;

        Engine.Opening += () => OnUi(() => { IsIdle = false; StatusText = "Opening…"; });
        Engine.StateChanged += () => OnUi(UpdateState);
        Engine.EndReached += () => OnUi(OnEndReached);
        Engine.Error += () => OnUi(OnError);
        Engine.Buffering += cache => OnUi(() => { if (cache < 99.5f && Engine.State is VLCState.Opening or VLCState.Buffering) StatusText = $"Buffering {cache:0}%"; });
        Engine.LengthChanged += len => OnUi(() => { LengthMs = len; if (Playlist.Current is { } c && len > 0) c.DurationMs = len; });
        Engine.TracksChanged += () => OnUi(RefreshTracks);
        Engine.VoutChanged += count => OnUi(() =>
        {
            HasVideo = count > 0;
            UpdateKeepAwake();
            if (count > 0) OnVoutForPendingPause();
        });
        Engine.SnapshotTaken += file => OnUi(() => ShowOsd("Snapshot saved: " + Path.GetFileName(file)));

        _timer = new DispatcherTimer(DispatcherPriority.Background, _ui) { Interval = TimeSpan.FromMilliseconds(250) };
        _timer.Tick += (_, _) => Tick();
        _timer.Start();

        _resumeTimer = new DispatcherTimer(DispatcherPriority.Background, _ui) { Interval = TimeSpan.FromSeconds(12) };
        _resumeTimer.Tick += (_, _) => DismissResume();

        // Restore last session's playlist without playing it.
        var restored = Settings.LastPlaylist.Select(l => new MediaItem(l)).ToList();
        Playlist.AddRange(restored);
        if (Settings.LastPlaylistIndex >= 0 && Settings.LastPlaylistIndex < Playlist.Items.Count)
            Playlist.SetCurrent(Settings.LastPlaylistIndex);
    }

    public SettingsService SettingsService { get; }
    public AppSettings Settings => SettingsService.Current;
    public PlayerService Engine { get; }
    public MediaPlayer Player => Engine.Player;
    public MediaControls Smtc { get; }
    public ResumeStore Resume { get; }
    public Playlist Playlist { get; }

    // ----- Observable state -----

    [ObservableProperty] public partial string Title { get; set; } = "Reel Player";
    [ObservableProperty] public partial string StatusText { get; set; } = "Stopped";
    [ObservableProperty] public partial bool IsIdle { get; set; } = true;
    [ObservableProperty] public partial bool HasVideo { get; set; }
    [ObservableProperty] public partial bool IsSeekable { get; set; }
    [ObservableProperty] public partial bool IsRecording { get; set; }
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsNonNormalRate), nameof(RateText))]
    public partial float Rate { get; set; } = 1f;

    public bool IsNonNormalRate => Math.Abs(Rate - 1f) > 0.001f;
    public string RateText => $"{Rate:0.##}×";
    [ObservableProperty] public partial long LengthMs { get; set; }
    [ObservableProperty] public partial string ElapsedText { get; set; } = "--:--";
    [ObservableProperty] public partial string TotalText { get; set; } = "--:--";
    [ObservableProperty] public partial IReadOnlyList<double> ChapterFractions { get; set; } = Array.Empty<double>();
    public IReadOnlyList<ChapterMark> ChapterMarks { get; private set; } = Array.Empty<ChapterMark>();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsAbLoopActive), nameof(AbLoopText))]
    public partial long? AbLoopStartMs { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsAbLoopActive), nameof(AbLoopText))]
    public partial long? AbLoopEndMs { get; set; }

    public bool IsAbLoopActive => AbLoopStartMs.HasValue;
    public string AbLoopText => AbLoopStartMs is not { } a ? ""
        : AbLoopEndMs is { } b ? $"A-B {TimeFormat.Format(a)}–{TimeFormat.Format(b)}"
        : $"A {TimeFormat.Format(a)}…";

    public string? ChapterNameAt(long ms)
    {
        if (ChapterMarks.Count == 0) return null;
        string? name = null;
        foreach (var ch in ChapterMarks)
        {
            if (ch.StartMs <= ms) name = ch.Name;
            else break;
        }
        return name;
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PlayPauseIcon), nameof(PlayPauseTip))]
    public partial bool IsPlaying { get; set; }

    [ObservableProperty] public partial bool IsPaused { get; set; }

    /// <summary>Position bound to the seek bar. Setting it from the UI seeks.</summary>
    [ObservableProperty] public partial long PositionMs { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(VolumeIcon), nameof(VolumeText))]
    public partial int Volume { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(VolumeIcon))]
    public partial bool IsMuted { get; set; }

    [ObservableProperty] public partial bool ShowRemaining { get; set; }

    // Window-level UI state that the control bar reflects.
    [ObservableProperty] public partial bool IsPlaylistVisible { get; set; }
    [ObservableProperty] public partial bool HasResumeOffer { get; set; }
    [ObservableProperty] public partial string ResumeOfferText { get; set; } = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(FullscreenIcon))]
    public partial bool IsFullscreen { get; set; }

    public string FullscreenIcon => IsFullscreen ? "" : "";

    /// <summary>True while the user drags the seek thumb; the timer then leaves PositionMs alone.</summary>
    public bool IsUserSeeking { get; set; }

    public string PlayPauseIcon => IsPlaying ? "" : "";
    public string PlayPauseTip => IsPlaying ? "Pause" : "Play";
    public string VolumeText => $"{Volume}%";
    public string VolumeIcon => IsMuted || Volume == 0 ? "" : Volume < 34 ? "" : Volume < 67 ? "" : "";

    public LoopMode Loop
    {
        get => Playlist.Loop;
        set
        {
            Playlist.Loop = value;
            Settings.Loop = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(LoopIcon));
            OnPropertyChanged(nameof(LoopTip));
            OnPropertyChanged(nameof(IsLoopOn));
        }
    }

    public bool IsLoopOn => Loop != LoopMode.Off;
    public string LoopIcon => Loop == LoopMode.One ? "" : "";
    public string LoopTip => Loop switch { LoopMode.All => "Loop all", LoopMode.One => "Loop one", _ => "Loop off" };

    public bool Shuffle
    {
        get => Playlist.Shuffle;
        set { Playlist.Shuffle = value; Settings.Shuffle = value; OnPropertyChanged(); }
    }

    // Current picture settings (indexes into the static tables).
    public int AspectIndex { get; private set; }
    public int CropIndex { get; private set; }
    public int ZoomIndex { get; private set; }
    public int DeinterlaceIndex { get; private set; }
    public int TransformIndex { get; private set; }
    public bool Sharpen { get; private set; }
    public double SharpenSigma { get; private set; } = 0.05;

    // Equalizer state (kept here so the Effects dialog can be reopened).
    public bool EqEnabled { get; private set; }
    public int EqPreset { get; private set; } = -1;
    public float EqPreamp { get; private set; } = 12f;
    public float[] EqBands { get; } = new float[10];

    // Video adjust state.
    public bool AdjustEnabled { get; private set; }
    public float Brightness { get; private set; } = 1f;
    public float Contrast { get; private set; } = 1f;
    public float Hue { get; private set; }
    public float Saturation { get; private set; } = 1f;
    public float Gamma { get; private set; } = 1f;

    public long AudioDelayMs => Player.AudioDelay / 1000;
    public long SubtitleDelayMs => Player.SpuDelay / 1000;

    // ----- Events for the window -----

    public event Action<string>? OsdRequested;
    public event Action<string>? ErrorRaised;
    public event Action? OpenFileRequested;

    /// <summary>Raised on the UI thread after <see cref="Engine"/> created a new MediaPlayer.</summary>
    public event Action? PlayerReplaced;

    public void ShowOsd(string text) => OsdRequested?.Invoke(text);

    // ----- Opening -----

    /// <summary>Adds files, folders, playlists or URLs. Subtitles are attached to the current video instead.</summary>
    public void OpenLocations(IEnumerable<string> locations, bool play = true, long startMs = 0)
    {
        var list = locations.Where(l => !string.IsNullOrWhiteSpace(l)).ToList();
        var subtitles = list.Where(l => File.Exists(l) && MediaFiles.IsSubtitle(l)).ToList();
        list = list.Except(subtitles).ToList();

        if (subtitles.Count > 0 && Engine.IsActive)
            foreach (var s in subtitles) AddSubtitle(s);

        var items = list.SelectMany(Expand).ToList();
        if (items.Count == 0) return;

        foreach (var l in list.Where(l => Directory.Exists(l)))
            Settings.LastFolder = l;
        if (list.FirstOrDefault(File.Exists) is { } file)
            Settings.LastFolder = Path.GetDirectoryName(file);

        OpenItems(items, play, startMs);
    }

    public void OpenItems(IReadOnlyList<MediaItem> items, bool play, long startMs = 0)
    {
        if (items.Count == 0) return;
        // Re-opening something already in the playlist reuses that entry instead of adding a duplicate.
        var resolved = items.Select(i =>
            i.Options.Count == 0 && i.SubtitleFile is null &&
            Playlist.Items.FirstOrDefault(e => string.Equals(e.Location, i.Location, StringComparison.OrdinalIgnoreCase)) is { } existing
                ? existing : i).ToList();
        Playlist.AddRange(resolved.Where(i => !Playlist.Items.Contains(i)).Distinct());
        items = resolved;
        if (play)
            PlayItem(items[0], startMs);
        else
            ShowOsd(items.Count == 1 ? $"Added: {items[0].Title}" : $"Added {items.Count} items");
    }

    private static IEnumerable<MediaItem> Expand(string location)
    {
        if (Directory.Exists(location))
            return MediaFiles.EnumerateFolder(location).Select(f => new MediaItem(f));
        if (PlaylistParser.ShouldExpand(location))
        {
            try { return PlaylistParser.Load(location); }
            catch (Exception ex) { AppLog.Write(ex, "Playlist " + location); }
        }
        return new[] { new MediaItem(location) };
    }

    public void PlayItem(MediaItem item, long startMs = 0, bool offerResume = true)
    {
        SaveResumePosition();
        _playGeneration++;
        ClearAbLoop(quiet: true);
        Playlist.SetCurrent(item);
        _appliedForCurrentMedia = false;
        _restart = null;
        _pauseWhenPictureShows = false;
        if (IsRecording) { IsRecording = false; _recordPath = null; }

        Title = item.Title;
        IsIdle = false;
        PositionMs = startMs;
        LengthMs = item.DurationMs;
        ChapterMarks = Array.Empty<ChapterMark>();
        ChapterFractions = Array.Empty<double>();

        Engine.Open(item, startMs, BuildExtraOptions());

        DismissResume();
        if (offerResume && startMs == 0 && Settings.ResumePlayback && item.IsFile && Resume.Get(item.Location) is { } pos)
        {
            _resumeOffer = (item, pos);
            ResumeOfferText = $"Continue {TimeFormat.Format(pos)}";
            HasResumeOffer = true;
            _resumeTimer.Stop();
            _resumeTimer.Start();
        }
    }

    public void AcceptResume()
    {
        if (_resumeOffer is { } offer && ReferenceEquals(offer.Item, Playlist.Current))
            Seek(offer.Position);
        DismissResume();
    }

    public void DismissResume()
    {
        _resumeTimer.Stop();
        _resumeOffer = null;
        HasResumeOffer = false;
    }

    /// <summary>Reopens the current item at the current time (needed when per-media options change).</summary>
    public void RestartCurrent(string? osd = null)
    {
        if (Playlist.Current is not { } item || !Engine.IsActive) return;
        var pos = Player.Time;
        _appliedForCurrentMedia = false;
        _restart = CaptureRestart(osd);
        Engine.Open(item, Math.Max(0, pos), BuildExtraOptions());
    }

    /// <summary>What a reopen must preserve: pause state and A/V delays. It also replaces the title OSD.</summary>
    private RestartState CaptureRestart(string? osd) =>
        new(Engine.State == VLCState.Paused, osd, Player.AudioDelay, Player.SpuDelay);

    /// <summary>
    /// Rotate/flip and sharpen need a new libVLC engine (see <see cref="PlayerService.VideoFilterOptions"/>).
    /// Rebuilds it, then reopens whatever was playing at the same position. Rapid repeated changes are coalesced.
    /// </summary>
    private async void ApplyVideoFilters(string? osd)
    {
        if (_rebuilding)
        {
            _rebuildAgain = true;
            _pendingFilterOsd = osd;
            return;
        }
        _rebuilding = true;
        var gen = _playGeneration;
        var item = Engine.IsActive ? Playlist.Current : null;
        var pos = item is null ? 0 : Math.Max(0, Player.Time);
        var restart = CaptureRestart(osd);
        try
        {
            do
            {
                _rebuildAgain = false;
                await Engine.RebuildAsync(PlayerService.VideoFilterOptions(Transforms[TransformIndex].Type, Sharpen, SharpenSigma));
                restart = restart with { Osd = _pendingFilterOsd ?? restart.Osd };
                _pendingFilterOsd = null;
            } while (_rebuildAgain);

            PlayerReplaced?.Invoke();   // the window moves its video surface to the new player
            Engine.ReleaseRetired();
            if (item is not null && gen == _playGeneration)
            {
                _appliedForCurrentMedia = false;
                _restart = restart;
                Engine.Open(item, pos, BuildExtraOptions());
            }
        }
        catch (Exception ex)
        {
            AppLog.Write(ex, "Rebuild engine");
            ErrorRaised?.Invoke("Could not apply the video filter: " + ex.Message);
        }
        finally
        {
            _rebuilding = false;
        }
    }

    private List<string> BuildExtraOptions()
    {
        var opts = new List<string>();
        if (IsRecording && _recordPath is not null)
        {
            var dst = _recordPath.Replace("'", "\\'");
            opts.Add($":sout=#duplicate{{dst=display,dst=std{{access=file,mux=ts,dst='{dst}'}}}}");
            opts.Add(":sout-keep");
        }
        return opts;
    }

    // ----- Transport -----

    public void TogglePlayPause()
    {
        switch (Engine.State)
        {
            case VLCState.Playing:
                Engine.Pause();
                break;
            case VLCState.Paused:
                Engine.Resume();
                break;
            default:
                if (Playlist.Current is { } cur) PlayItem(cur);
                else if (Playlist.PeekNext(true) is { } first) PlayItem(first);
                else OpenFileRequested?.Invoke();
                break;
        }
    }

    public void Play()
    {
        if (Engine.State == VLCState.Paused) Engine.Resume();
        else if (Engine.State != VLCState.Playing) TogglePlayPause();
    }

    public void Pause()
    {
        if (Engine.State == VLCState.Playing) Engine.Pause();
    }

    public void Stop()
    {
        SaveResumePosition();
        _playGeneration++;
        DismissResume();
        ClearAbLoop(quiet: true);
        Engine.StopKeepItem();
        IsRecording = false;
        PositionMs = 0;
        ElapsedText = "--:--";
        IsIdle = true;
    }

    public void Next()
    {
        if (Playlist.PeekNext(userInitiated: true) is { } next) { PlayItem(next); return; }
        if (TryPlaySibling(+1)) return;
        ShowOsd("End of playlist");
    }

    public void Previous()
    {
        if (Playlist.PeekPrevious() is { } prev) { PlayItem(prev); return; }
        if (TryPlaySibling(-1)) return;
        if (Playlist.Current is { } cur) { if (Engine.IsActive) Seek(0); else PlayItem(cur); }
    }

    private bool TryPlaySibling(int offset)
    {
        if (!Settings.AutoAdvanceFolder || Playlist.Current is not { IsFile: true } cur) return false;
        if (MediaFiles.FindSiblingMedia(cur.Location, offset) is not { } sibling) return false;
        OpenItems(new[] { new MediaItem(sibling) }, play: true);
        return true;
    }

    /// <summary>Cycles A-B loop: Off → set A → set B (looping) → Off.</summary>
    public void CycleAbLoop()
    {
        if (!Engine.IsActive || !Player.IsSeekable) { ShowOsd("Play seekable media to use A-B loop"); return; }
        var now = Math.Max(0, Player.Time);
        if (!AbLoopStartMs.HasValue)
        {
            AbLoopStartMs = now;
            AbLoopEndMs = null;
            ShowOsd($"A-B loop: point A set ({TimeFormat.Format(now)})");
        }
        else if (!AbLoopEndMs.HasValue)
        {
            if (now <= AbLoopStartMs.Value + 250)
            {
                ShowOsd("Point B must be after point A");
                return;
            }
            AbLoopEndMs = now;
            Seek(AbLoopStartMs.Value);
            ShowOsd($"A-B loop: {TimeFormat.Format(AbLoopStartMs.Value)} – {TimeFormat.Format(now)}");
        }
        else
        {
            ClearAbLoop();
        }
    }

    public void ClearAbLoop(bool quiet = false)
    {
        if (!AbLoopStartMs.HasValue && !AbLoopEndMs.HasValue) return;
        AbLoopStartMs = null;
        AbLoopEndMs = null;
        if (!quiet) ShowOsd("A-B loop: off");
    }

    public void Seek(long ms)
    {
        if (!Engine.IsActive || !Player.IsSeekable) return;
        var len = Player.Length;
        ms = Math.Max(0, len > 0 ? Math.Min(ms, len - 500) : ms);
        Player.Time = ms;
        PositionMs = ms;
        UpdateTimeTexts(ms, len);
        Smtc.UpdateTimeline(ms, len);
    }

    public void SeekRelative(long deltaMs)
    {
        if (!Engine.IsActive) return;
        if (!Player.IsSeekable) { ShowOsd("This media is not seekable"); return; }
        var target = Player.Time + deltaMs;
        Seek(target);
        ShowOsd($"{TimeFormat.Format(Math.Max(0, target))} / {TimeFormat.Format(Player.Length)}");
    }

    public void NextFrame()
    {
        if (Engine.IsActive) Player.NextFrame();
    }

    // ----- Audio -----

    partial void OnVolumeChanged(int value)
    {
        Settings.Volume = value;
        if (Engine.IsActive) Player.Volume = value;
    }

    partial void OnIsMutedChanged(bool value)
    {
        Settings.Muted = value;
        if (Engine.IsActive) Player.Mute = value;
    }

    public void ChangeVolume(int delta)
    {
        Volume = Math.Clamp(Volume + delta, 0, 200);
        if (IsMuted && delta > 0) IsMuted = false;
        ShowOsd($"Volume {Volume}%");
    }

    public void ToggleMute()
    {
        IsMuted = !IsMuted;
        ShowOsd(IsMuted ? "Muted" : $"Volume {Volume}%");
    }

    public void SetRate(float rate)
    {
        rate = Math.Clamp(rate, 0.25f, 4f);
        Rate = rate;
        if (Engine.IsActive) Player.SetRate(rate);
        ShowOsd($"Speed: {rate:0.00}x");
    }

    public void Faster() => SetRate(Rates.FirstOrDefault(r => r > Rate + 0.001f, Rates[^1]));
    public void Slower() => SetRate(Rates.LastOrDefault(r => r < Rate - 0.001f, Rates[0]));

    public void ShiftAudioDelay(long deltaMs) => SetAudioDelay(AudioDelayMs + deltaMs);
    public void ShiftSubtitleDelay(long deltaMs) => SetSubtitleDelay(SubtitleDelayMs + deltaMs);

    public void SetAudioDelay(long ms)
    {
        Player.SetAudioDelay(ms * 1000);
        ShowOsd($"Audio delay: {ms} ms");
    }

    public void SetSubtitleDelay(long ms)
    {
        Player.SetSpuDelay(ms * 1000);
        ShowOsd($"Subtitle delay: {ms} ms");
    }

    public IReadOnlyList<TrackOption> AudioTracks() => Describe(Player.AudioTrackDescription, Player.AudioTrack);
    public IReadOnlyList<TrackOption> SubtitleTracks() => Describe(Player.SpuDescription, Player.Spu);
    public IReadOnlyList<TrackOption> VideoTracks() => Describe(Player.VideoTrackDescription, Player.VideoTrack);
    public IReadOnlyList<TrackOption> Titles() => Describe(Player.TitleDescription, Player.Title);

    public IReadOnlyList<TrackOption> Chapters()
    {
        if (!Engine.IsActive) return Array.Empty<TrackOption>();
        var current = Player.Chapter;
        return Player.FullChapterDescriptions(-1)
            .Select((c, i) => new TrackOption(i, $"{(string.IsNullOrWhiteSpace(c.Name) ? $"Chapter {i + 1}" : c.Name)}  ({TimeFormat.Format(c.TimeOffset)})", i == current))
            .ToList();
    }

    public IReadOnlyList<(string Id, string Name, bool Selected)> AudioDevices()
    {
        var current = Player.OutputDevice;
        return Player.AudioOutputDeviceEnum
            .Select(d => (d.DeviceIdentifier, string.IsNullOrEmpty(d.Description) ? "Default" : d.Description,
                          string.Equals(d.DeviceIdentifier, current, StringComparison.Ordinal)))
            .ToList();
    }

    private static List<TrackOption> Describe(LibVLCSharp.Shared.Structures.TrackDescription[] tracks, int current) =>
        tracks.Select(t => new TrackOption(t.Id, t.Name ?? $"Track {t.Id}", t.Id == current)).ToList();

    public void SelectAudioTrack(int id) { Player.SetAudioTrack(id); OsdTrack("Audio", AudioTracks(), id); }
    public void SelectSubtitleTrack(int id) { Player.SetSpu(id); OsdTrack("Subtitle", SubtitleTracks(), id); }
    public void SelectVideoTrack(int id) { Player.SetVideoTrack(id); OsdTrack("Video", VideoTracks(), id); }
    public void SelectTitle(int id) => Player.Title = id;
    public void SelectChapter(int index) => Player.Chapter = index;

    public void SelectAudioDevice(string id)
    {
        Settings.AudioDeviceId = id;
        Player.SetOutputDevice(id, null!);
    }

    public void SetStereoMode(AudioOutputChannel channel)
    {
        Player.SetChannel(channel);
        ShowOsd("Stereo mode: " + StereoModes.First(s => s.Channel == channel).Name);
    }

    public void CycleAudioTrack() => Cycle(AudioTracks(), SelectAudioTrack, includeDisabled: false);
    public void CycleSubtitleTrack() => Cycle(SubtitleTracks(), SelectSubtitleTrack, includeDisabled: true);

    private void Cycle(IReadOnlyList<TrackOption> tracks, Action<int> select, bool includeDisabled)
    {
        var list = includeDisabled ? tracks : tracks.Where(t => t.Id >= 0).ToList();
        if (list.Count == 0) { ShowOsd("No tracks available"); return; }
        var idx = list.ToList().FindIndex(t => t.IsSelected);
        select(list[(idx + 1) % list.Count].Id);
    }

    private void OsdTrack(string kind, IReadOnlyList<TrackOption> tracks, int id) =>
        ShowOsd($"{kind} track: {tracks.FirstOrDefault(t => t.Id == id)?.Name ?? "Disabled"}");

    public void AddSubtitle(string path)
    {
        Engine.AddSubtitle(path);
        ShowOsd("Subtitle added: " + Path.GetFileName(path));
    }

    // ----- Video -----

    public void SetAspect(int index)
    {
        AspectIndex = (index % AspectRatios.Length + AspectRatios.Length) % AspectRatios.Length;
        var v = AspectRatios[AspectIndex];
        Player.AspectRatio = v.Length == 0 ? null : v;
        ShowOsd("Aspect ratio: " + (v.Length == 0 ? "Default" : v));
    }

    public void SetCrop(int index)
    {
        CropIndex = (index % CropRatios.Length + CropRatios.Length) % CropRatios.Length;
        var v = CropRatios[CropIndex];
        Player.CropGeometry = v.Length == 0 ? null : v;
        ShowOsd("Crop: " + (v.Length == 0 ? "Default" : v));
    }

    public void SetZoom(int index)
    {
        ZoomIndex = (index % Zooms.Length + Zooms.Length) % Zooms.Length;
        Player.Scale = Zooms[ZoomIndex].Scale;
        ShowOsd("Zoom: " + Zooms[ZoomIndex].Name);
    }

    public void SetDeinterlace(int index)
    {
        DeinterlaceIndex = index;
        Player.SetDeinterlace(DeinterlaceModes[index].Mode!);
        ShowOsd("Deinterlace: " + DeinterlaceModes[index].Name);
    }

    public void SetVideoFilters(int transformIndex, bool sharpen, double sigma, string? osd = null)
    {
        var changed = transformIndex != TransformIndex || sharpen != Sharpen || Math.Abs(sigma - SharpenSigma) > 0.0001;
        TransformIndex = transformIndex;
        Sharpen = sharpen;
        SharpenSigma = sigma;
        if (changed) ApplyVideoFilters(osd);
    }

    /// <summary>Steps the picture 0° → 90° → 180° → 270° → 0° (a flip/transpose setting goes to 90°).</summary>
    public void RotateVideo()
    {
        var next = TransformIndex switch { 1 => 2, 2 => 3, 3 => 0, _ => 1 };
        var label = "Rotation: " + (next == 0 ? "none" : Transforms[next].Name.Replace("Rotate ", ""));
        SetVideoFilters(next, Sharpen, SharpenSigma, label);
        // When active, ApplyVideoFilters displays the OSD once the rebuilt engine starts; otherwise show it now.
        if (!Engine.IsActive) ShowOsd(label);
    }

    public void SetAdjust(bool enabled, float brightness, float contrast, float hue, float saturation, float gamma)
    {
        AdjustEnabled = enabled;
        Brightness = brightness; Contrast = contrast; Hue = hue; Saturation = saturation; Gamma = gamma;
        ApplyAdjust();
    }

    private void ApplyAdjust()
    {
        Player.SetAdjustInt(VideoAdjustOption.Enable, AdjustEnabled ? 1 : 0);
        if (!AdjustEnabled) return;
        Player.SetAdjustFloat(VideoAdjustOption.Brightness, Brightness);
        Player.SetAdjustFloat(VideoAdjustOption.Contrast, Contrast);
        Player.SetAdjustFloat(VideoAdjustOption.Hue, Hue);
        Player.SetAdjustFloat(VideoAdjustOption.Saturation, Saturation);
        Player.SetAdjustFloat(VideoAdjustOption.Gamma, Gamma);
    }

    public void TakeSnapshot()
    {
        if (!Engine.IsActive || !HasVideo) { ShowOsd("No video to snapshot"); return; }
        try
        {
            var folder = Engine.CurrentItem is { IsFile: true } item &&
                         Path.GetDirectoryName(item.Location) is { Length: > 0 } videoDir &&
                         Directory.Exists(videoDir)
                ? videoDir
                : Settings.SnapshotFolder;
            Directory.CreateDirectory(folder);
            var rawName = Engine.CurrentItem is { IsFile: true } f ? Path.GetFileNameWithoutExtension(f.Location) : Title;
            var stem = string.Concat(rawName.Split(Path.GetInvalidFileNameChars()));
            if (stem.Length > 60) stem = stem[..60];
            var ext = Settings.SnapshotFormat == "jpg" ? "jpg" : "png";
            var file = Path.Combine(folder,
                $"{stem}-{TimeFormat.Format(Player.Time).Replace(':', '.')}-{DateTime.Now:HHmmss}.{ext}");
            if (!Player.TakeSnapshot(0, file, 0, 0)) ShowOsd("Snapshot failed");
        }
        catch (Exception ex)
        {
            AppLog.Write(ex, "Snapshot");
            ShowOsd("Snapshot failed: " + ex.Message);
        }
    }

    public void ToggleRecord()
    {
        if (!Engine.IsActive) { ShowOsd("Nothing to record"); return; }
        if (!IsRecording)
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "ReelPlayer");
            Directory.CreateDirectory(dir);
            _recordPath = Path.Combine(dir, $"Recording-{DateTime.Now:yyyyMMdd-HHmmss}.ts");
            IsRecording = true;
            RestartCurrent();
            ShowOsd("Recording to " + Path.GetFileName(_recordPath));
        }
        else
        {
            IsRecording = false;
            RestartCurrent();
            ShowOsd("Recording stopped: " + Path.GetFileName(_recordPath));
            _recordPath = null;
        }
    }

    // ----- Equalizer -----

    public static IReadOnlyList<string> EqualizerPresets()
    {
        using var eq = new Equalizer();
        return Enumerable.Range(0, (int)eq.PresetCount).Select(i => eq.PresetName((uint)i) ?? $"Preset {i + 1}").ToList();
    }

    public static IReadOnlyList<string> EqualizerBandLabels()
    {
        using var eq = new Equalizer();
        return Enumerable.Range(0, (int)eq.BandCount).Select(i =>
        {
            var f = eq.BandFrequency((uint)i);
            return f >= 1000 ? $"{f / 1000:0.#}k" : $"{f:0}";
        }).ToList();
    }

    public void SetEqualizerEnabled(bool enabled)
    {
        EqEnabled = enabled;
        ApplyEqualizer();
    }

    /// <summary>Loads a preset and returns its band values so the dialog can update its sliders.</summary>
    public float[] LoadEqualizerPreset(int preset)
    {
        EqPreset = preset;
        using var eq = new Equalizer((uint)preset);
        EqPreamp = eq.Preamp;
        for (uint i = 0; i < EqBands.Length && i < eq.BandCount; i++) EqBands[i] = eq.Amp(i);
        ApplyEqualizer();
        return EqBands.ToArray();
    }

    public void SetEqualizerBand(int band, float amp)
    {
        EqBands[band] = amp;
        EqPreset = -1;
        ApplyEqualizer();
    }

    public void SetEqualizerPreamp(float amp)
    {
        EqPreamp = amp;
        ApplyEqualizer();
    }

    private void ApplyEqualizer()
    {
        if (!EqEnabled)
        {
            Player.UnsetEqualizer();
            return;
        }
        _equalizer ??= new Equalizer();
        _equalizer.SetPreamp(EqPreamp);
        for (uint i = 0; i < EqBands.Length && i < _equalizer.BandCount; i++) _equalizer.SetAmp(EqBands[i], i);
        Player.SetEqualizer(_equalizer);
    }

    // ----- Engine callbacks (UI thread) -----

    private void OnUi(Action action) => _ui.BeginInvoke(action);

    private void UpdateState()
    {
        var state = Engine.State;
        IsPlaying = state == VLCState.Playing;
        IsPaused = state == VLCState.Paused;
        IsSeekable = Engine.IsActive && Player.IsSeekable;
        StatusText = state switch
        {
            VLCState.Playing => "Playing",
            VLCState.Paused => "Paused",
            VLCState.Opening => "Opening…",
            VLCState.Buffering => "Buffering…",
            VLCState.Error => "Error",
            _ => "Stopped",
        };
        if (state is VLCState.Stopped or VLCState.Ended or VLCState.NothingSpecial)
        {
            IsIdle = true;
            HasVideo = false;
        }

        if (state == VLCState.Playing && !_appliedForCurrentMedia)
        {
            _appliedForCurrentMedia = true;
            _consecutiveErrors = 0;
            OnMediaStarted();
        }

        UpdateKeepAwake();
        Smtc.UpdateStatus(IsPlaying, IsPaused);
    }

    /// <summary>Re-applies per-player settings once a new media is actually playing.</summary>
    private void OnMediaStarted()
    {
        Player.Volume = Volume;
        Player.Mute = IsMuted;
        if (Math.Abs(Rate - 1f) > 0.001f) Player.SetRate(Rate);
        if (!string.IsNullOrEmpty(Settings.AudioDeviceId)) Player.SetOutputDevice(Settings.AudioDeviceId, null!);
        if (AdjustEnabled) ApplyAdjust();
        if (EqEnabled) ApplyEqualizer();
        if (DeinterlaceIndex > 0) Player.SetDeinterlace(DeinterlaceModes[DeinterlaceIndex].Mode!);

        var media = Engine.CurrentMedia;
        var item = Playlist.Current;
        string? artist = null, artwork = null;
        if (media is not null)
        {
            var metaTitle = media.Meta(MetadataType.NowPlaying) is { Length: > 0 } np ? np : media.Meta(MetadataType.Title);
            artist = media.Meta(MetadataType.Artist);
            artwork = media.Meta(MetadataType.ArtworkURL);
            // Streams usually have better titles in metadata than in their URL; files keep their name
            // unless metadata looks deliberate (differs from the file name).
            if (item is not null && !string.IsNullOrWhiteSpace(metaTitle) &&
                (!item.IsFile || !string.Equals(Path.GetFileNameWithoutExtension(item.Location), metaTitle, StringComparison.OrdinalIgnoreCase)) &&
                !string.Equals(metaTitle, Path.GetFileName(item.Location), StringComparison.OrdinalIgnoreCase))
            {
                item.Title = metaTitle;
            }
        }
        Title = item?.Title ?? Title;
        Smtc.UpdateMetadata(Title, artist, Player.VideoTrackCount > 0 || HasVideo, artwork);
        // A rebuilt engine starts with default picture settings; re-apply the user's.
        if (AspectIndex > 0) Player.AspectRatio = AspectRatios[AspectIndex];
        if (CropIndex > 0) Player.CropGeometry = CropRatios[CropIndex];
        if (ZoomIndex > 0) Player.Scale = Zooms[ZoomIndex].Scale;

        if (_restart is { } restart)
        {
            _restart = null;
            if (restart.AudioDelayUs != 0) Player.SetAudioDelay(restart.AudioDelayUs);
            if (restart.SpuDelayUs != 0) Player.SetSpuDelay(restart.SpuDelayUs);
            if (restart.WasPaused) PauseOncePictureShows();
            if (restart.Osd is not null) ShowOsd(restart.Osd);
        }
        else
        {
            ShowOsd(Title);
        }
        RefreshTracks();
    }

    /// <summary>
    /// Restores the paused state after a reopen. Pausing straight away leaves the video area black (nothing has been
    /// drawn yet), so play muted until libVLC has a video output and a frame on screen, then pause.
    /// </summary>
    private void PauseOncePictureShows()
    {
        if (Player.VideoTrackCount == 0)
        {
            Player.SetPause(true);
            return;
        }
        Player.Mute = true;
        _pauseWhenPictureShows = true;
        if (Player.VoutCount > 0) OnVoutForPendingPause();
    }

    private void OnVoutForPendingPause()
    {
        if (!_pauseWhenPictureShows) return;
        _pauseWhenPictureShows = false;
        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
        timer.Tick += (_, _) =>
        {
            timer.Stop();
            Player.SetPause(true);
            Player.Mute = IsMuted;
        };
        timer.Start();
    }

    private void OnEndReached()
    {
        if (Playlist.Current is { IsFile: true } done) Resume.Forget(done.Location);
        if (Playlist.PeekNext(userInitiated: false) is { } next)
        {
            PlayItem(next);
        }
        else if (Playlist.Items.Count == 1 && Playlist.Loop == LoopMode.Off && TryPlaySibling(+1))
        {
            return;
        }
        else
        {
            Engine.StopKeepItem();
            IsIdle = true;
            PositionMs = 0;
            Smtc.UpdateStatus(false, false);
        }
    }

    private void OnError()
    {
        var item = Playlist.Current;
        var detail = Engine.Log.Snapshot().LastOrDefault(e => e.Level == LogLevel.Error)?.Message;
        var message = $"Can't play \"{item?.Title ?? "media"}\"" + (string.IsNullOrEmpty(detail) ? "." : $": {detail}");
        AppLog.Write(message + (item is null ? "" : $" [{item.Location}]"));
        ErrorRaised?.Invoke(message);

        _consecutiveErrors++;
        IsIdle = true;
        if (_consecutiveErrors < Playlist.Items.Count && Playlist.PeekNext(userInitiated: true) is { } next && !ReferenceEquals(next, item))
            PlayItem(next);
        else
            _consecutiveErrors = 0;
    }

    private void RefreshTracks()
    {
        if (!Engine.IsActive) return;
        HasVideo = HasVideo || Player.VideoTrackCount > 0;
        var len = Player.Length;
        if (len > 0)
        {
            var chapters = Player.FullChapterDescriptions(-1);
            ChapterMarks = chapters.Length > 0
                ? chapters.Select((c, i) => new ChapterMark(
                    Math.Max(0, c.TimeOffset),
                    Math.Clamp((double)c.TimeOffset / len, 0, 1),
                    string.IsNullOrWhiteSpace(c.Name) ? $"Chapter {i + 1}" : c.Name)).ToList()
                : Array.Empty<ChapterMark>();
            ChapterFractions = chapters.Length > 1
                ? chapters.Skip(1).Select(c => (double)c.TimeOffset / len).Where(f => f > 0 && f < 1).ToList()
                : Array.Empty<double>();
        }
    }

    private void Tick()
    {
        if (_rebuilding || !Engine.IsActive) return;
        var len = Player.Length;
        var time = Player.Time;
        if (len > 0 && len != LengthMs) LengthMs = len;
        IsSeekable = Player.IsSeekable;
        if (!IsUserSeeking && time >= 0)
        {
            PositionMs = time;
            if (AbLoopStartMs is { } start && AbLoopEndMs is { } end && time >= end)
            {
                Seek(start);
                return;
            }
        }
        UpdateTimeTexts(IsUserSeeking ? PositionMs : time, len);

        if (IsPlaying && ++_timelineTick % 4 == 0) Smtc.UpdateTimeline(time, len);
    }

    public void UpdateTimeTexts(long time, long len)
    {
        ElapsedText = TimeFormat.Format(time);
        TotalText = len <= 0 ? "--:--" : ShowRemaining ? "-" + TimeFormat.Format(len - time) : TimeFormat.Format(len);
    }

    partial void OnShowRemainingChanged(bool value)
    {
        Settings.ShowRemainingTime = value;
        UpdateTimeTexts(PositionMs, LengthMs);
    }

    private void UpdateKeepAwake() => NativeMethods.KeepAwake(IsPlaying && HasVideo);

    public void SaveResumePosition()
    {
        if (!Settings.ResumePlayback) return;
        if (Engine.CurrentItem is { IsFile: true } item && Engine.IsActive)
            Resume.Update(item.Location, Player.Time, Player.Length);
    }

    public void SaveState()
    {
        SaveResumePosition();
        Resume.Save();
        Settings.LastPlaylist = Playlist.Items
            .Where(i => !i.Location.StartsWith("dshow:", StringComparison.OrdinalIgnoreCase))
            .Select(i => i.Location).ToList();
        Settings.LastPlaylistIndex = Playlist.CurrentIndex;
        try { SettingsService.Save(); }
        catch (Exception ex) { AppLog.Write(ex, "Save settings"); }
    }

    public void Dispose()
    {
        _timer.Stop();
        _resumeTimer.Stop();
        NativeMethods.KeepAwake(false);
        _equalizer?.Dispose();
    }
}
