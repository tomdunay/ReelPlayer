namespace ReelPlayer.Core.Services;

public sealed record HotkeyAction(string Id, string Description, string[] DefaultGestures);

/// <summary>All bindable actions with VLC-compatible default keys.</summary>
public static class HotkeyActions
{
    public static readonly IReadOnlyList<HotkeyAction> All = new HotkeyAction[]
    {
        new("PlayPause", "Play / Pause", new[] { "Space", "MediaPlayPause" }),
        new("Stop", "Stop", new[] { "S", "MediaStop" }),
        new("Next", "Next item", new[] { "N", "MediaNextTrack" }),
        new("Previous", "Previous item", new[] { "P", "MediaPreviousTrack" }),
        new("Fullscreen", "Toggle fullscreen", new[] { "F", "F11" }),
        new("ExitFullscreen", "Exit fullscreen", new[] { "Escape" }),
        new("SeekBack10", "Jump back 10 s", new[] { "Left" }),
        new("SeekFwd10", "Jump forward 10 s", new[] { "Right" }),
        new("SeekBack3", "Jump back 3 s", new[] { "Shift+Left" }),
        new("SeekFwd3", "Jump forward 3 s", new[] { "Shift+Right" }),
        new("SeekBack60", "Jump back 1 min", new[] { "Alt+Left" }),
        new("SeekFwd60", "Jump forward 1 min", new[] { "Alt+Right" }),
        new("SeekBack300", "Jump back 5 min", new[] { "Ctrl+Left" }),
        new("SeekFwd300", "Jump forward 5 min", new[] { "Ctrl+Right" }),
        new("VolumeUp", "Volume up", new[] { "Up", "Ctrl+Up" }),
        new("VolumeDown", "Volume down", new[] { "Down", "Ctrl+Down" }),
        new("Mute", "Mute", new[] { "M" }),
        new("Slower", "Slower", new[] { "[" }),
        new("Faster", "Faster", new[] { "]" }),
        new("NormalSpeed", "Normal speed", new[] { "=" }),
        new("NextFrame", "Next frame", new[] { "E" }),
        new("CycleAspect", "Cycle aspect ratio", new[] { "A" }),
        new("CycleCrop", "Cycle crop", new[] { "C" }),
        new("CycleZoom", "Cycle zoom", new[] { "Z" }),
        new("CycleAudio", "Cycle audio track", new[] { "B" }),
        new("CycleSubtitle", "Cycle subtitle track", new[] { "V" }),
        new("SubDelayDown", "Subtitle delay -50 ms", new[] { "G" }),
        new("SubDelayUp", "Subtitle delay +50 ms", new[] { "H" }),
        new("AudioDelayDown", "Audio delay -50 ms", new[] { "J" }),
        new("AudioDelayUp", "Audio delay +50 ms", new[] { "K" }),
        new("Snapshot", "Take snapshot", new[] { "Shift+S" }),
        new("ShowTime", "Show time", new[] { "T" }),
        new("CycleLoop", "Cycle loop mode", new[] { "L" }),
        new("TogglePlaylist", "Toggle playlist", new[] { "Ctrl+L" }),
        new("OpenFile", "Open file", new[] { "Ctrl+O" }),
        new("OpenMultiple", "Open multiple files", new[] { "Ctrl+Shift+O" }),
        new("OpenFolder", "Open folder", new[] { "Ctrl+F" }),
        new("OpenDisc", "Open disc", new[] { "Ctrl+D" }),
        new("OpenNetwork", "Open network stream", new[] { "Ctrl+N" }),
        new("OpenCapture", "Open capture device", new[] { "Ctrl+C" }),
        new("JumpToTime", "Jump to time", new[] { "Ctrl+T" }),
        new("Effects", "Effects and filters", new[] { "Ctrl+E" }),
        new("MediaInfo", "Media information", new[] { "Ctrl+I" }),
        new("CodecInfo", "Codec information", new[] { "Ctrl+J" }),
        new("Messages", "Messages", new[] { "Ctrl+M" }),
        new("Preferences", "Preferences", new[] { "Ctrl+P" }),
        new("MinimalInterface", "Minimal interface", new[] { "Ctrl+H" }),
        new("Quit", "Quit", new[] { "Ctrl+Q" }),
    };

    public static HotkeyAction? Find(string id) => All.FirstOrDefault(a => a.Id == id);
}

/// <summary>Resolves normalized gesture strings ("Ctrl+Shift+Left") to action ids.</summary>
public sealed class HotkeyMap
{
    private static readonly string[] ModifierOrder = { "Ctrl", "Alt", "Shift", "Win" };

    private readonly Dictionary<string, string> _gestureToAction = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, string[]> _actionToGestures = new();

    public HotkeyMap(IReadOnlyDictionary<string, string>? overrides = null)
    {
        foreach (var action in HotkeyActions.All)
        {
            var gestures = overrides is not null && overrides.TryGetValue(action.Id, out var custom)
                ? Split(custom)
                : action.DefaultGestures.Select(Normalize).ToArray();
            _actionToGestures[action.Id] = gestures;
            foreach (var g in gestures)
                _gestureToAction.TryAdd(g, action.Id);
        }
    }

    public string? Resolve(string gesture) =>
        _gestureToAction.TryGetValue(Normalize(gesture), out var a) ? a : null;

    public IReadOnlyList<string> GesturesFor(string actionId) =>
        _actionToGestures.TryGetValue(actionId, out var g) ? g : Array.Empty<string>();

    /// <summary>First gesture, for showing next to menu items.</summary>
    public string GestureText(string actionId) => GesturesFor(actionId).FirstOrDefault() ?? "";

    public static string[] Split(string joined) =>
        joined.Split('|', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
              .Select(Normalize).Where(g => g.Length > 0).ToArray();

    /// <summary>Canonical form: modifiers in Ctrl, Alt, Shift, Win order, then the key.</summary>
    public static string Normalize(string gesture)
    {
        if (string.IsNullOrWhiteSpace(gesture)) return "";
        // "+" alone (or ending in "++") is the plus key.
        var parts = new List<string>();
        var text = gesture.Trim();
        var key = "";
        if (text.EndsWith("++", StringComparison.Ordinal) || text == "+")
        {
            key = "+";
            text = text.Length > 1 ? text[..^2] : "";
        }
        foreach (var p in text.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            parts.Add(p);
        if (key.Length == 0 && parts.Count > 0) { key = parts[^1]; parts.RemoveAt(parts.Count - 1); }

        var mods = parts.Select(CanonicalModifier).Where(m => m is not null).Distinct()
                        .OrderBy(m => Array.IndexOf(ModifierOrder, m)).ToList();
        if (key.Length == 1) key = key.ToUpperInvariant();
        else if (key.Length > 1) key = char.ToUpperInvariant(key[0]) + key[1..];
        if (key.Equals("Esc", StringComparison.OrdinalIgnoreCase)) key = "Escape";
        return mods.Count == 0 ? key : string.Join("+", mods) + "+" + key;
    }

    private static string? CanonicalModifier(string m) => m.ToLowerInvariant() switch
    {
        "ctrl" or "control" => "Ctrl",
        "alt" => "Alt",
        "shift" => "Shift",
        "win" or "windows" => "Win",
        _ => null,
    };
}
