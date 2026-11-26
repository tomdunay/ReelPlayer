using Windows.Media;
using WinMediaPlayer = Windows.Media.Playback.MediaPlayer;

namespace ReelPlayer.App.Services;

/// <summary>
/// System Media Transport Controls (Windows volume flyout, lock screen, hardware media keys).
/// An unpackaged desktop app gets an SMTC instance by borrowing one from a WinRT MediaPlayer
/// whose own command handling is switched off.
/// </summary>
public sealed class MediaControls : IDisposable
{
    private readonly WinMediaPlayer? _host;
    private readonly SystemMediaTransportControls? _smtc;

    public MediaControls()
    {
        try
        {
            _host = new WinMediaPlayer();
            _host.CommandManager.IsEnabled = false;
            _smtc = _host.SystemMediaTransportControls;
            _smtc.IsEnabled = true;
            _smtc.IsPlayEnabled = true;
            _smtc.IsPauseEnabled = true;
            _smtc.IsStopEnabled = true;
            _smtc.IsNextEnabled = true;
            _smtc.IsPreviousEnabled = true;
            _smtc.PlaybackStatus = MediaPlaybackStatus.Closed;
            _smtc.ButtonPressed += (_, e) => ButtonPressed?.Invoke(e.Button switch
            {
                SystemMediaTransportControlsButton.Play => "Play",
                SystemMediaTransportControlsButton.Pause => "Pause",
                SystemMediaTransportControlsButton.Stop => "Stop",
                SystemMediaTransportControlsButton.Next => "Next",
                SystemMediaTransportControlsButton.Previous => "Previous",
                _ => "",
            });
        }
        catch (Exception ex)
        {
            AppLog.Write(ex, "SMTC unavailable");
            _smtc = null;
        }
    }

    /// <summary>Raised on a background thread with Play, Pause, Stop, Next or Previous.</summary>
    public event Action<string>? ButtonPressed;

    public void UpdateStatus(bool playing, bool paused)
    {
        if (_smtc is null) return;
        try
        {
            _smtc.PlaybackStatus = playing ? MediaPlaybackStatus.Playing
                : paused ? MediaPlaybackStatus.Paused
                : MediaPlaybackStatus.Stopped;
        }
        catch { /* ignore */ }
    }

    public void UpdateMetadata(string title, string? artist, bool isVideo, string? artworkUrl)
    {
        if (_smtc is null) return;
        try
        {
            var du = _smtc.DisplayUpdater;
            du.ClearAll();
            if (isVideo)
            {
                du.Type = MediaPlaybackType.Video;
                du.VideoProperties.Title = title;
                if (!string.IsNullOrEmpty(artist)) du.VideoProperties.Subtitle = artist;
            }
            else
            {
                du.Type = MediaPlaybackType.Music;
                du.MusicProperties.Title = title;
                if (!string.IsNullOrEmpty(artist)) du.MusicProperties.Artist = artist;
            }
            if (!string.IsNullOrEmpty(artworkUrl) && Uri.TryCreate(artworkUrl, UriKind.Absolute, out var art))
                du.Thumbnail = Windows.Storage.Streams.RandomAccessStreamReference.CreateFromUri(art);
            du.Update();
        }
        catch (Exception ex)
        {
            AppLog.Write(ex, "SMTC metadata");
        }
    }

    public void UpdateTimeline(long positionMs, long lengthMs)
    {
        if (_smtc is null || lengthMs <= 0) return;
        try
        {
            _smtc.UpdateTimelineProperties(new SystemMediaTransportControlsTimelineProperties
            {
                StartTime = TimeSpan.Zero,
                MinSeekTime = TimeSpan.Zero,
                Position = TimeSpan.FromMilliseconds(Math.Clamp(positionMs, 0, lengthMs)),
                MaxSeekTime = TimeSpan.FromMilliseconds(lengthMs),
                EndTime = TimeSpan.FromMilliseconds(lengthMs),
            });
        }
        catch { /* ignore */ }
    }

    public void Clear()
    {
        if (_smtc is null) return;
        try
        {
            _smtc.PlaybackStatus = MediaPlaybackStatus.Closed;
            _smtc.DisplayUpdater.ClearAll();
            _smtc.DisplayUpdater.Update();
        }
        catch { /* ignore */ }
    }

    public void Dispose()
    {
        try { _host?.Dispose(); } catch { /* ignore */ }
    }
}
