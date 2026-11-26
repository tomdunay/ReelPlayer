using System.Text.Json;

namespace ReelPlayer.Core.Services;

public sealed class AppSettings
{
    // Interface
    public string Theme { get; set; } = "Dark"; // Dark | Light | System
    public bool OneInstance { get; set; } = true;
    public bool EnqueueWhenRunning { get; set; }
    public bool ResumePlayback { get; set; } = true;
    public bool ShowStatusBar { get; set; } = true;
    public bool ShowPlaylist { get; set; }
    public bool AlwaysOnTop { get; set; }
    public bool ShowRemainingTime { get; set; }
    public bool PauseOnMinimize { get; set; }

    // Engine (applied at startup)
    public bool HardwareDecoding { get; set; } = true;
    public string? AudioDeviceId { get; set; }

    // Subtitles (applied at startup)
    public string SubtitleLanguage { get; set; } = "";
    public string SubtitleEncoding { get; set; } = "";
    public string SubtitleSize { get; set; } = "Normal"; // Smaller | Small | Normal | Large | Larger
    public string SubtitleColor { get; set; } = "#FFFFFF";
    public string SubtitleOutline { get; set; } = "Normal"; // None | Thin | Normal | Thick
    public bool AutoLoadSubtitles { get; set; } = true;

    // Snapshots
    public string SnapshotFolder { get; set; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyPictures), "ReelPlayer");
    public string SnapshotFormat { get; set; } = "png"; // png | jpg

    // Hotkeys: action -> "Gesture1|Gesture2". Only actions the user changed are stored.
    public Dictionary<string, string> Hotkeys { get; set; } = new();

    // State
    public int Volume { get; set; } = 100;
    public bool Muted { get; set; }
    public LoopMode Loop { get; set; }
    public bool Shuffle { get; set; }
    public double? WindowLeft { get; set; }
    public double? WindowTop { get; set; }
    public double WindowWidth { get; set; } = 960;
    public double WindowHeight { get; set; } = 600;
    public bool WindowMaximized { get; set; }
    public double PlaylistWidth { get; set; } = 320;
    public string? LastFolder { get; set; }
    public List<string> LastPlaylist { get; set; } = new();
    public int LastPlaylistIndex { get; set; } = -1;
}

public sealed class SettingsService
{
    public SettingsService(string? directory = null)
    {
        Directory = directory ?? AppDataDirectory;
        FilePath = Path.Combine(Directory, "settings.json");
    }

    public static string AppDataDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "ReelPlayer");

    public string Directory { get; }
    public string FilePath { get; }
    public AppSettings Current { get; private set; } = new();

    public AppSettings Load()
    {
        try
        {
            if (File.Exists(FilePath))
                Current = JsonSerializer.Deserialize(File.ReadAllText(FilePath), AppJsonContext.Default.AppSettings) ?? new AppSettings();
        }
        catch (Exception)
        {
            // Corrupt settings: keep a copy for inspection, start fresh.
            try { File.Copy(FilePath, FilePath + ".bad", overwrite: true); } catch { /* ignore */ }
            Current = new AppSettings();
        }
        return Current;
    }

    public void Save()
    {
        System.IO.Directory.CreateDirectory(Directory);
        var tmp = FilePath + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(Current, AppJsonContext.Default.AppSettings));
        File.Move(tmp, FilePath, overwrite: true);
    }

    public static int SubtitleSizeToVlc(string size) => size switch
    {
        "Smaller" => 20,
        "Small" => 18,
        "Large" => 12,
        "Larger" => 6,
        _ => 16,
    };

    public static int SubtitleOutlineToVlc(string outline) => outline switch
    {
        "None" => 0,
        "Thin" => 2,
        "Thick" => 6,
        _ => 4,
    };
}
