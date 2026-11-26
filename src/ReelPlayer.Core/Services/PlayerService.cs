using System.Collections.Concurrent;
using System.Globalization;
using LibVLCSharp.Shared;
using ReelPlayer.Core.Models;

namespace ReelPlayer.Core.Services;

public sealed record LogEntry(DateTime Time, LogLevel Level, string Module, string Message);

/// <summary>Thread-safe ring buffer of libVLC log lines.</summary>
public sealed class LogBuffer
{
    private readonly LinkedList<LogEntry> _entries = new();
    private readonly int _capacity;

    public LogBuffer(int capacity = 5000) => _capacity = capacity;

    public event Action<LogEntry>? Added;

    public void Add(LogEntry entry)
    {
        lock (_entries)
        {
            _entries.AddLast(entry);
            if (_entries.Count > _capacity) _entries.RemoveFirst();
        }
        Added?.Invoke(entry);
    }

    public List<LogEntry> Snapshot()
    {
        lock (_entries) return _entries.ToList();
    }

    public void Clear()
    {
        lock (_entries) _entries.Clear();
    }
}

/// <summary>
/// Owns the single LibVLC + MediaPlayer. Heavy calls (open/stop) run on a dedicated worker thread so the
/// UI thread never blocks on libVLC, and nothing calls back into libVLC from inside a libVLC event.
/// Events are raised on libVLC/worker threads; subscribers must marshal to their UI thread.
/// </summary>
public sealed class PlayerService : IDisposable
{
    private readonly BlockingCollection<Action> _queue = new();
    private readonly Thread _worker;
    private readonly List<string> _baseOptions;
    private readonly List<(LibVLC Lib, MediaPlayer Player, Media? Media)> _retired = new();
    private Media? _media;

    public PlayerService(IEnumerable<string> libVlcOptions)
    {
        _baseOptions = libVlcOptions.ToList();
        (LibVLC, Player) = CreateEngine(_baseOptions);
        _worker = new Thread(WorkLoop) { IsBackground = true, Name = "libVLC worker" };
        _worker.Start();
    }

    /// <summary>Current engine. Both are replaced by <see cref="RebuildAsync"/>, so don't cache them.</summary>
    public LibVLC LibVLC { get; private set; }
    public MediaPlayer Player { get; private set; }
    public LogBuffer Log { get; } = new();
    public MediaItem? CurrentItem { get; private set; }
    public Media? CurrentMedia => _media;

    public event Action? Opening;
    public event Action? StateChanged;
    public event Action? EndReached;
    public event Action? Error;
    public event Action<float>? Buffering;
    public event Action<long>? LengthChanged;
    public event Action? TracksChanged;
    public event Action<int>? VoutChanged;
    public event Action<string>? SnapshotTaken;

    public VLCState State => Player.State;
    public bool IsActive => Player.State is VLCState.Playing or VLCState.Paused or VLCState.Opening or VLCState.Buffering;

    /// <summary>libVLC options derived from user settings. Some only take effect on restart.</summary>
    public static List<string> BuildOptions(AppSettings s)
    {
        var opts = new List<string>
        {
            "--no-video-title-show",
            "--no-snapshot-preview",
            "--no-stats",
            "--avcodec-hw=" + (s.HardwareDecoding ? "any" : "none"),
            "--snapshot-format=" + (s.SnapshotFormat == "jpg" ? "jpg" : "png"),
            "--freetype-rel-fontsize=" + SettingsService.SubtitleSizeToVlc(s.SubtitleSize),
            "--freetype-outline-thickness=" + SettingsService.SubtitleOutlineToVlc(s.SubtitleOutline),
            "--freetype-color=" + ParseColor(s.SubtitleColor),
        };
        if (!string.IsNullOrWhiteSpace(s.SubtitleLanguage)) opts.Add("--sub-language=" + s.SubtitleLanguage.Trim());
        if (!string.IsNullOrWhiteSpace(s.SubtitleEncoding)) opts.Add("--subsdec-encoding=" + s.SubtitleEncoding.Trim());
        // libVLC itself loads movie.srt / movie.en.ass etc. next to movie.mkv; this switch turns that off.
        if (!s.AutoLoadSubtitles) opts.Add("--no-sub-autodetect-file");
        return opts;
    }

    /// <summary>
    /// libVLC options for rotate/flip and sharpen. In libVLC 3 the video output only reads video filters from the
    /// LibVLC instance (a per-media ":video-filter" is ignored), so these go to <see cref="RebuildAsync"/>.
    /// </summary>
    public static List<string> VideoFilterOptions(string? transformType, bool sharpen, double sharpenSigma)
    {
        var filters = new List<string>();
        var opts = new List<string>();
        if (!string.IsNullOrEmpty(transformType))
        {
            filters.Add("transform");
            opts.Add("--transform-type=" + transformType);
        }
        if (sharpen)
        {
            filters.Add("sharpen");
            opts.Add("--sharpen-sigma=" + sharpenSigma.ToString("0.###", CultureInfo.InvariantCulture));
        }
        if (filters.Count > 0) opts.Insert(0, "--video-filter=" + string.Join(":", filters));
        return opts;
    }

    private (LibVLC, MediaPlayer) CreateEngine(IEnumerable<string> options)
    {
        var lib = new LibVLC(options.ToArray());
        lib.Log += (_, e) => Log.Add(new LogEntry(DateTime.Now, e.Level, e.Module ?? "", e.Message ?? ""));

        var mp = new MediaPlayer(lib) { EnableKeyInput = false, EnableMouseInput = false };
        // After a rebuild the retired player may still raise events; only the current one is forwarded.
        bool Current() => ReferenceEquals(mp, Player);
        mp.Opening += (_, _) => { if (Current()) Opening?.Invoke(); };
        mp.Playing += (_, _) => { if (Current()) StateChanged?.Invoke(); };
        mp.Paused += (_, _) => { if (Current()) StateChanged?.Invoke(); };
        mp.Stopped += (_, _) => { if (Current()) StateChanged?.Invoke(); };
        mp.EndReached += (_, _) => { if (Current()) EndReached?.Invoke(); };
        mp.EncounteredError += (_, _) => { if (Current()) Error?.Invoke(); };
        mp.Buffering += (_, e) => { if (Current()) Buffering?.Invoke(e.Cache); };
        mp.LengthChanged += (_, e) => { if (Current()) LengthChanged?.Invoke(e.Length); };
        mp.ESAdded += (_, _) => { if (Current()) TracksChanged?.Invoke(); };
        mp.ESDeleted += (_, _) => { if (Current()) TracksChanged?.Invoke(); };
        mp.ESSelected += (_, _) => { if (Current()) TracksChanged?.Invoke(); };
        mp.ChapterChanged += (_, _) => { if (Current()) TracksChanged?.Invoke(); };
        mp.TitleChanged += (_, _) => { if (Current()) TracksChanged?.Invoke(); };
        mp.Vout += (_, e) => { if (Current()) VoutChanged?.Invoke(e.Count); };
        mp.SnapshotTaken += (_, e) => { if (Current()) SnapshotTaken?.Invoke(e.Filename); };
        return (lib, mp);
    }

    /// <summary>
    /// Stops playback and swaps in a fresh LibVLC + MediaPlayer created with the base options plus
    /// <paramref name="extraOptions"/>. The old pair is kept until <see cref="ReleaseRetired"/>, so the caller can
    /// first move its video surface to the new <see cref="Player"/>.
    /// </summary>
    public Task RebuildAsync(IEnumerable<string> extraOptions)
    {
        var options = _baseOptions.Concat(extraOptions).ToList();
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Post(() =>
        {
            try
            {
                Player.Stop();
                var (lib, mp) = CreateEngine(options);
                _retired.Add((LibVLC, Player, _media));
                _media = null;
                LibVLC = lib;
                Player = mp;
                done.SetResult();
            }
            catch (Exception ex)
            {
                done.SetException(ex);
            }
        });
        return done.Task;
    }

    /// <summary>Disposes engines replaced by <see cref="RebuildAsync"/>.</summary>
    public void ReleaseRetired() => Post(DisposeRetired);

    private void DisposeRetired()
    {
        foreach (var (lib, mp, media) in _retired)
        {
            mp.Dispose();
            media?.Dispose();
            lib.Dispose();
        }
        _retired.Clear();
    }

    private static int ParseColor(string hex)
    {
        hex = (hex ?? "").TrimStart('#');
        if (hex.Length == 8) hex = hex[2..]; // drop alpha
        return int.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var v) ? v & 0xFFFFFF : 0xFFFFFF;
    }

    /// <summary>Opens and plays <paramref name="item"/>, optionally from <paramref name="startMs"/>.</summary>
    public void Open(MediaItem item, long startMs = 0, IEnumerable<string>? extraOptions = null)
    {
        var options = extraOptions?.ToList() ?? new List<string>();
        CurrentItem = item;
        Post(() =>
        {
            var media = new Media(LibVLC, item.Mrl, FromType.FromLocation);
            foreach (var o in item.Options) media.AddOption(o);
            foreach (var o in options) media.AddOption(o);
            if (startMs > 0)
                media.AddOption(":start-time=" + (startMs / 1000.0).ToString("0.###", CultureInfo.InvariantCulture));
            if (!string.IsNullOrEmpty(item.SubtitleFile))
                media.AddSlave(MediaSlaveType.Subtitle, 4, new Uri(item.SubtitleFile).AbsoluteUri);

            Player.Play(media);
            var old = _media;
            _media = media;
            old?.Dispose();
        });
    }

    public void Stop()
    {
        CurrentItem = null;
        Post(() => Player.Stop());
    }

    /// <summary>Stops without forgetting the current item (used before restarting it with new options).</summary>
    public void StopKeepItem() => Post(() => Player.Stop());

    public void Pause() => Player.SetPause(true);
    public void Resume() => Player.SetPause(false);

    public void AddSubtitle(string path) =>
        Post(() => Player.AddSlave(MediaSlaveType.Subtitle, new Uri(path).AbsoluteUri, true));

    /// <summary>Runs work on the libVLC worker thread (serialized with open/stop).</summary>
    public void Post(Action action)
    {
        if (!_queue.IsAddingCompleted) _queue.Add(action);
    }

    private void WorkLoop()
    {
        foreach (var action in _queue.GetConsumingEnumerable())
        {
            try { action(); }
            catch (Exception ex) { Log.Add(new LogEntry(DateTime.Now, LogLevel.Error, "reelplayer", ex.Message)); }
        }
    }

    public void Dispose()
    {
        Post(() => Player.Stop());
        _queue.CompleteAdding();
        _worker.Join(3000);
        DisposeRetired();
        Player.Dispose();
        _media?.Dispose();
        LibVLC.Dispose();
    }
}
