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
    private Media? _media;

    public PlayerService(IEnumerable<string> libVlcOptions)
    {
        LibVLC = new LibVLC(libVlcOptions.ToArray());
        LibVLC.Log += (_, e) => Log.Add(new LogEntry(DateTime.Now, e.Level, e.Module ?? "", e.Message ?? ""));

        Player = new MediaPlayer(LibVLC) { EnableKeyInput = false, EnableMouseInput = false };
        Player.Opening += (_, _) => Opening?.Invoke();
        Player.Playing += (_, _) => StateChanged?.Invoke();
        Player.Paused += (_, _) => StateChanged?.Invoke();
        Player.Stopped += (_, _) => StateChanged?.Invoke();
        Player.EndReached += (_, _) => EndReached?.Invoke();
        Player.EncounteredError += (_, _) => Error?.Invoke();
        Player.Buffering += (_, e) => Buffering?.Invoke(e.Cache);
        Player.LengthChanged += (_, e) => LengthChanged?.Invoke(e.Length);
        Player.ESAdded += (_, _) => TracksChanged?.Invoke();
        Player.ESDeleted += (_, _) => TracksChanged?.Invoke();
        Player.ESSelected += (_, _) => TracksChanged?.Invoke();
        Player.ChapterChanged += (_, _) => TracksChanged?.Invoke();
        Player.TitleChanged += (_, _) => TracksChanged?.Invoke();
        Player.Vout += (_, e) => VoutChanged?.Invoke(e.Count);
        Player.SnapshotTaken += (_, e) => SnapshotTaken?.Invoke(e.Filename);

        _worker = new Thread(WorkLoop) { IsBackground = true, Name = "libVLC worker" };
        _worker.Start();
    }

    public LibVLC LibVLC { get; }
    public MediaPlayer Player { get; }
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
        Player.Dispose();
        _media?.Dispose();
        LibVLC.Dispose();
    }
}
