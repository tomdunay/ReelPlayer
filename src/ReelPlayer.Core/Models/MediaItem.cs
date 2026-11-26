using System.ComponentModel;
using System.Runtime.CompilerServices;
using ReelPlayer.Core.Services;

namespace ReelPlayer.Core.Models;

/// <summary>One entry in the playlist: a local file, a URL, or a disc/device MRL.</summary>
public sealed class MediaItem : INotifyPropertyChanged
{
    private string _title;
    private long _durationMs;
    private bool _isCurrent;

    public MediaItem(string location, string? title = null, long durationMs = 0)
    {
        Location = NormalizeLocation(location);
        _title = string.IsNullOrWhiteSpace(title) ? DefaultTitle(Location) : title!;
        _durationMs = durationMs;
    }

    /// <summary>Absolute file path, or an MRL such as https://..., dvd:///D:/, dshow://.</summary>
    public string Location { get; }

    /// <summary>Extra libVLC media options (":option=value") applied when this item is opened.</summary>
    public List<string> Options { get; } = new();

    /// <summary>External subtitle file to attach when opened.</summary>
    public string? SubtitleFile { get; set; }

    public string Title
    {
        get => _title;
        set { if (_title != value) { _title = value; OnPropertyChanged(); } }
    }

    public long DurationMs
    {
        get => _durationMs;
        set
        {
            if (_durationMs != value)
            {
                _durationMs = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(DurationText));
            }
        }
    }

    public string DurationText => _durationMs > 0 ? TimeFormat.Format(_durationMs) : "--:--";

    /// <summary>True while this item is the playlist's current item (used for highlighting).</summary>
    public bool IsCurrent
    {
        get => _isCurrent;
        set { if (_isCurrent != value) { _isCurrent = value; OnPropertyChanged(); } }
    }

    public bool IsFile => IsLocalPath(Location);

    /// <summary>MRL to hand to libVLC.</summary>
    public string Mrl => IsFile ? new Uri(Location).AbsoluteUri : Location;

    public static bool IsLocalPath(string location) =>
        !location.Contains("://", StringComparison.Ordinal) &&
        (Path.IsPathFullyQualified(location) || location.StartsWith(@"\\", StringComparison.Ordinal));

    private static string NormalizeLocation(string location)
    {
        location = location.Trim().Trim('"');
        if (location.StartsWith("file:", StringComparison.OrdinalIgnoreCase) &&
            Uri.TryCreate(location, UriKind.Absolute, out var uri) && uri.IsFile)
        {
            return uri.LocalPath;
        }
        return location;
    }

    private static string DefaultTitle(string location)
    {
        if (IsLocalPath(location))
        {
            var name = Path.GetFileName(location.TrimEnd('\\', '/'));
            return string.IsNullOrEmpty(name) ? location : name;
        }
        if (Uri.TryCreate(location, UriKind.Absolute, out var uri))
        {
            var last = uri.Segments.Length > 0 ? Uri.UnescapeDataString(uri.Segments[^1]).Trim('/') : "";
            return string.IsNullOrEmpty(last) ? location : last;
        }
        return location;
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    public override string ToString() => Title;
}
