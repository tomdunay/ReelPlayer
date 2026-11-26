using System.Globalization;

namespace ReelPlayer.Core.Services;

/// <summary>ReelPlayer.exe [--fullscreen] [--start-time=SECONDS] file1 file2 url…</summary>
public sealed record CommandLine(IReadOnlyList<string> Locations, bool Fullscreen, long StartTimeMs)
{
    public static CommandLine Parse(IEnumerable<string> args)
    {
        var locations = new List<string>();
        var fullscreen = false;
        long startMs = 0;

        foreach (var raw in args)
        {
            var arg = raw.Trim();
            if (arg.Length == 0) continue;

            if (arg is "--fullscreen" or "-f" or "/fullscreen")
            {
                fullscreen = true;
            }
            else if (arg.StartsWith("--start-time=", StringComparison.OrdinalIgnoreCase))
            {
                var value = arg["--start-time=".Length..];
                if (double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var secs) && secs > 0)
                    startMs = (long)(secs * 1000);
                else if (TimeFormat.TryParse(value, out var ms))
                    startMs = ms;
            }
            else if (arg.StartsWith("--", StringComparison.Ordinal))
            {
                // Unknown switch: ignore rather than treating it as a file.
            }
            else
            {
                locations.Add(MakeAbsolute(arg));
            }
        }

        return new CommandLine(locations, fullscreen, startMs);
    }

    private static string MakeAbsolute(string location)
    {
        if (location.Contains("://", StringComparison.Ordinal)) return location;
        try { return Path.GetFullPath(location); }
        catch (Exception) { return location; }
    }
}
