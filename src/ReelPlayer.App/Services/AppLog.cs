using System.IO;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.Services;

/// <summary>Appends app errors to %APPDATA%\ReelPlayer\logs\reelplayer-yyyyMMdd.log.</summary>
public static class AppLog
{
    private static readonly object Gate = new();

    public static string Directory => Path.Combine(SettingsService.AppDataDirectory, "logs");

    public static void Write(string message)
    {
        try
        {
            lock (Gate)
            {
                System.IO.Directory.CreateDirectory(Directory);
                var file = Path.Combine(Directory, $"reelplayer-{DateTime.Now:yyyyMMdd}.log");
                File.AppendAllText(file, $"{DateTime.Now:HH:mm:ss.fff} {message}{Environment.NewLine}");
            }
        }
        catch
        {
            // Logging must never take the app down.
        }
    }

    public static void Write(Exception ex, string context) => Write($"[{context}] {ex}");

    /// <summary>Removes log files older than two weeks.</summary>
    public static void Prune()
    {
        try
        {
            if (!System.IO.Directory.Exists(Directory)) return;
            foreach (var f in System.IO.Directory.EnumerateFiles(Directory, "*.log"))
                if (File.GetLastWriteTimeUtc(f) < DateTime.UtcNow.AddDays(-14)) File.Delete(f);
        }
        catch { /* ignore */ }
    }
}
