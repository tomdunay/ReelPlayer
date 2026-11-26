using System.Text.Json;

namespace ReelPlayer.Core.Services;

/// <summary>Remembers where the user stopped in long files.</summary>
public sealed class ResumeStore
{
    public const long MinLengthMs = 5 * 60 * 1000;
    public const long MinPositionMs = 10 * 1000;
    public const long EndMarginMs = 15 * 1000;
    public const int MaxEntries = 300;

    private readonly string _filePath;
    private Dictionary<string, Entry> _entries = new(StringComparer.OrdinalIgnoreCase);

    public sealed record Entry(long PositionMs, DateTime SavedUtc);

    public ResumeStore(string? directory = null)
    {
        _filePath = Path.Combine(directory ?? SettingsService.AppDataDirectory, "resume.json");
        try
        {
            if (File.Exists(_filePath))
            {
                var loaded = JsonSerializer.Deserialize<Dictionary<string, Entry>>(File.ReadAllText(_filePath));
                if (loaded is not null) _entries = new Dictionary<string, Entry>(loaded, StringComparer.OrdinalIgnoreCase);
            }
        }
        catch { /* start empty */ }
    }

    public static bool IsResumable(long positionMs, long lengthMs) =>
        lengthMs >= MinLengthMs && positionMs >= MinPositionMs && positionMs <= lengthMs - EndMarginMs;

    public long? Get(string location) =>
        _entries.TryGetValue(location, out var e) ? e.PositionMs : null;

    /// <summary>Stores the position, or forgets it when the file was (nearly) finished or is too short.</summary>
    public void Update(string location, long positionMs, long lengthMs)
    {
        if (IsResumable(positionMs, lengthMs))
            _entries[location] = new Entry(positionMs, DateTime.UtcNow);
        else
            _entries.Remove(location);

        if (_entries.Count > MaxEntries)
        {
            foreach (var old in _entries.OrderBy(kv => kv.Value.SavedUtc).Take(_entries.Count - MaxEntries).ToList())
                _entries.Remove(old.Key);
        }
    }

    public void Forget(string location) => _entries.Remove(location);

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_filePath)!);
            File.WriteAllText(_filePath, JsonSerializer.Serialize(_entries));
        }
        catch { /* non-fatal */ }
    }
}
