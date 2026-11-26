using System.Globalization;

namespace ReelPlayer.Core.Services;

public static class TimeFormat
{
    /// <summary>Formats milliseconds as mm:ss, or h:mm:ss when an hour or longer.</summary>
    public static string Format(long ms)
    {
        if (ms < 0) ms = 0;
        var t = TimeSpan.FromMilliseconds(ms);
        return t.TotalHours >= 1
            ? $"{(int)t.TotalHours}:{t.Minutes:00}:{t.Seconds:00}"
            : $"{t.Minutes:00}:{t.Seconds:00}";
    }

    /// <summary>Parses "ss", "mm:ss" or "hh:mm:ss" (seconds may have a fraction) into milliseconds.</summary>
    public static bool TryParse(string? text, out long ms)
    {
        ms = 0;
        if (string.IsNullOrWhiteSpace(text)) return false;
        var parts = text.Trim().Split(':');
        if (parts.Length > 3) return false;

        double total = 0;
        for (var i = 0; i < parts.Length; i++)
        {
            var isLast = i == parts.Length - 1;
            if (isLast)
            {
                if (!double.TryParse(parts[i], NumberStyles.Float, CultureInfo.InvariantCulture, out var sec) || sec < 0)
                    return false;
                if (parts.Length > 1 && sec >= 60) return false;
                total = total * 60 + sec;
            }
            else
            {
                if (!int.TryParse(parts[i], NumberStyles.None, CultureInfo.InvariantCulture, out var v)) return false;
                if (i > 0 && v >= 60) return false;
                total = total * 60 + v;
            }
        }
        ms = (long)Math.Round(total * 1000);
        return true;
    }
}
