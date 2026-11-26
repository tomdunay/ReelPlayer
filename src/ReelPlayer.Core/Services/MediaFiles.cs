namespace ReelPlayer.Core.Services;

/// <summary>File extensions the player recognises when scanning folders and building file dialogs.</summary>
public static class MediaFiles
{
    public static readonly string[] Video =
    {
        ".3g2", ".3gp", ".3gp2", ".amv", ".asf", ".avi", ".bik", ".divx", ".drc", ".dv", ".f4v", ".flv", ".gvi",
        ".gxf", ".m1v", ".m2t", ".m2ts", ".m2v", ".m4v", ".mkv", ".mov", ".mp2v", ".mp4", ".mp4v", ".mpe", ".mpeg",
        ".mpeg1", ".mpeg2", ".mpeg4", ".mpg", ".mpv2", ".mts", ".mtv", ".mxf", ".nsv", ".nut", ".nuv", ".ogm",
        ".ogv", ".ogx", ".ps", ".rec", ".rm", ".rmvb", ".rpl", ".thp", ".tod", ".ts", ".tts", ".vob", ".vro",
        ".webm", ".wm", ".wmv", ".wtv", ".xesc", ".y4m", ".hevc", ".h264", ".h265", ".264", ".265", ".ivf", ".iso",
    };

    public static readonly string[] Audio =
    {
        ".3ga", ".a52", ".aac", ".ac3", ".adt", ".adts", ".aif", ".aifc", ".aiff", ".alac", ".amr", ".ape", ".au",
        ".awb", ".caf", ".dts", ".eac3", ".flac", ".it", ".m4a", ".m4b", ".m4p", ".mid", ".mka", ".mlp", ".mod",
        ".mp1", ".mp2", ".mp3", ".mpa", ".mpc", ".oga", ".ogg", ".oma", ".opus", ".qcp", ".ra", ".rmi", ".s3m",
        ".spx", ".thd", ".tta", ".voc", ".vqf", ".w64", ".wav", ".wma", ".wv", ".xa", ".xm",
    };

    public static readonly string[] Subtitles =
    {
        ".srt", ".ass", ".ssa", ".vtt", ".sub", ".idx", ".smi", ".sami", ".txt", ".usf", ".jss", ".psb", ".rt", ".mpl2", ".dks",
    };

    public static readonly string[] Playlists = { ".m3u", ".m3u8", ".pls", ".xspf", ".cue" };

    private static readonly HashSet<string> MediaSet =
        new(Video.Concat(Audio).Concat(Playlists), StringComparer.OrdinalIgnoreCase);

    public static bool IsMedia(string path) => MediaSet.Contains(Path.GetExtension(path));

    public static bool IsSubtitle(string path) =>
        Subtitles.Contains(Path.GetExtension(path), StringComparer.OrdinalIgnoreCase);

    /// <summary>Media files in a folder (recursive), sorted naturally by path.</summary>
    public static IEnumerable<string> EnumerateFolder(string folder)
    {
        IEnumerable<string> files;
        try
        {
            files = Directory.EnumerateFiles(folder, "*", new EnumerationOptions
            {
                RecurseSubdirectories = true,
                IgnoreInaccessible = true,
                AttributesToSkip = FileAttributes.Hidden | FileAttributes.System,
            }).Where(f => IsMedia(f) && !f.EndsWith(".cue", StringComparison.OrdinalIgnoreCase)).ToList();
        }
        catch (Exception)
        {
            return Array.Empty<string>();
        }
        return files.OrderBy(f => f, NaturalStringComparer.Instance);
    }

    public static string DialogFilter()
    {
        static string Pattern(IEnumerable<string> exts) => string.Join(";", exts.Select(e => "*" + e));
        return $"Media files|{Pattern(Video.Concat(Audio).Concat(Playlists))}" +
               $"|Video files|{Pattern(Video)}" +
               $"|Audio files|{Pattern(Audio)}" +
               $"|Playlists|{Pattern(Playlists)}" +
               "|All files|*.*";
    }

    public static string SubtitleDialogFilter() =>
        $"Subtitle files|{string.Join(";", Subtitles.Select(e => "*" + e))}|All files|*.*";
}

/// <summary>Orders "ep2" before "ep10".</summary>
public sealed class NaturalStringComparer : IComparer<string>
{
    public static readonly NaturalStringComparer Instance = new();

    public int Compare(string? x, string? y)
    {
        if (ReferenceEquals(x, y)) return 0;
        if (x is null) return -1;
        if (y is null) return 1;
        int i = 0, j = 0;
        while (i < x.Length && j < y.Length)
        {
            if (char.IsDigit(x[i]) && char.IsDigit(y[j]))
            {
                var si = i; while (i < x.Length && char.IsDigit(x[i])) i++;
                var sj = j; while (j < y.Length && char.IsDigit(y[j])) j++;
                var a = x[si..i].TrimStart('0');
                var b = y[sj..j].TrimStart('0');
                if (a.Length != b.Length) return a.Length.CompareTo(b.Length);
                var c = string.CompareOrdinal(a, b);
                if (c != 0) return c;
            }
            else
            {
                var c = char.ToUpperInvariant(x[i]).CompareTo(char.ToUpperInvariant(y[j]));
                if (c != 0) return c;
                i++; j++;
            }
        }
        return (x.Length - i).CompareTo(y.Length - j);
    }
}
