using System.Globalization;
using System.Text;
using System.Xml.Linq;
using ReelPlayer.Core.Models;

namespace ReelPlayer.Core.Services;

/// <summary>Reads and writes M3U/M3U8, PLS and XSPF playlists.</summary>
public static class PlaylistParser
{
    private static readonly XNamespace Xspf = "http://xspf.org/ns/0/";

    public static readonly string[] Extensions = { ".m3u", ".m3u8", ".pls", ".xspf" };

    public static bool IsPlaylistFile(string path) =>
        Extensions.Contains(Path.GetExtension(path), StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// True for playlists we expand ourselves. HLS (.m3u8 containing #EXT-X- tags) is a stream,
    /// not a list of files, so it is left for libVLC to play directly.
    /// </summary>
    public static bool ShouldExpand(string path)
    {
        if (!IsPlaylistFile(path) || !File.Exists(path)) return false;
        if (!Path.GetExtension(path).StartsWith(".m3u", StringComparison.OrdinalIgnoreCase)) return true;
        using var reader = new StreamReader(path);
        for (var i = 0; i < 50 && reader.ReadLine() is { } line; i++)
            if (line.StartsWith("#EXT-X-", StringComparison.OrdinalIgnoreCase)) return false;
        return true;
    }

    public static List<MediaItem> Load(string path)
    {
        var text = File.ReadAllText(path);
        var baseDir = Path.GetDirectoryName(Path.GetFullPath(path)) ?? "";
        return Path.GetExtension(path).ToLowerInvariant() switch
        {
            ".pls" => ParsePls(text, baseDir),
            ".xspf" => ParseXspf(text, baseDir),
            _ => ParseM3U(text, baseDir),
        };
    }

    public static List<MediaItem> ParseM3U(string text, string baseDir)
    {
        var items = new List<MediaItem>();
        string? pendingTitle = null;
        long pendingDuration = 0;

        foreach (var raw in SplitLines(text))
        {
            var line = raw.Trim().TrimStart('﻿');
            if (line.Length == 0) continue;
            if (line.StartsWith("#EXTINF:", StringComparison.OrdinalIgnoreCase))
            {
                var body = line.Substring(8);
                var comma = body.IndexOf(',');
                var durPart = comma >= 0 ? body[..comma] : body;
                // Duration may be followed by attributes: #EXTINF:123 tvg-id="x",Title
                var space = durPart.IndexOf(' ');
                if (space >= 0) durPart = durPart[..space];
                pendingDuration = double.TryParse(durPart, NumberStyles.Float, CultureInfo.InvariantCulture, out var secs) && secs > 0
                    ? (long)(secs * 1000) : 0;
                pendingTitle = comma >= 0 ? body[(comma + 1)..].Trim() : null;
                continue;
            }
            if (line.StartsWith('#')) continue;

            items.Add(new MediaItem(Resolve(line, baseDir), pendingTitle, pendingDuration));
            pendingTitle = null;
            pendingDuration = 0;
        }
        return items;
    }

    public static List<MediaItem> ParsePls(string text, string baseDir)
    {
        var files = new SortedDictionary<int, string>();
        var titles = new Dictionary<int, string>();
        var lengths = new Dictionary<int, long>();

        foreach (var raw in SplitLines(text))
        {
            var line = raw.Trim();
            var eq = line.IndexOf('=');
            if (eq <= 0) continue;
            var key = line[..eq].Trim();
            var value = line[(eq + 1)..].Trim();

            if (TryIndexed(key, "File", out var n)) files[n] = value;
            else if (TryIndexed(key, "Title", out n)) titles[n] = value;
            else if (TryIndexed(key, "Length", out n) &&
                     long.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var secs) && secs > 0)
                lengths[n] = secs * 1000;
        }

        return files.Select(kv => new MediaItem(
            Resolve(kv.Value, baseDir),
            titles.GetValueOrDefault(kv.Key),
            lengths.GetValueOrDefault(kv.Key))).ToList();
    }

    public static List<MediaItem> ParseXspf(string text, string baseDir)
    {
        var doc = XDocument.Parse(text);
        var items = new List<MediaItem>();
        foreach (var track in doc.Descendants().Where(e => e.Name.LocalName == "track"))
        {
            var location = track.Elements().FirstOrDefault(e => e.Name.LocalName == "location")?.Value.Trim();
            if (string.IsNullOrEmpty(location)) continue;
            var title = track.Elements().FirstOrDefault(e => e.Name.LocalName == "title")?.Value.Trim();
            var durText = track.Elements().FirstOrDefault(e => e.Name.LocalName == "duration")?.Value.Trim();
            long.TryParse(durText, NumberStyles.Integer, CultureInfo.InvariantCulture, out var dur);
            items.Add(new MediaItem(Resolve(location, baseDir), title, dur));
        }
        return items;
    }

    public static void Save(string path, IEnumerable<MediaItem> items)
    {
        var list = items.ToList();
        if (Path.GetExtension(path).Equals(".xspf", StringComparison.OrdinalIgnoreCase))
            File.WriteAllText(path, ToXspf(list), new UTF8Encoding(false));
        else
            File.WriteAllText(path, ToM3U(list), new UTF8Encoding(false));
    }

    public static string ToM3U(IEnumerable<MediaItem> items)
    {
        var sb = new StringBuilder("#EXTM3U\n");
        foreach (var item in items)
        {
            var secs = item.DurationMs > 0 ? item.DurationMs / 1000 : -1;
            sb.Append(CultureInfo.InvariantCulture, $"#EXTINF:{secs},{item.Title}\n");
            sb.Append(item.Location).Append('\n');
        }
        return sb.ToString();
    }

    public static string ToXspf(IEnumerable<MediaItem> items)
    {
        var trackList = new XElement(Xspf + "trackList");
        foreach (var item in items)
        {
            var track = new XElement(Xspf + "track",
                new XElement(Xspf + "location", item.Mrl),
                new XElement(Xspf + "title", item.Title));
            if (item.DurationMs > 0) track.Add(new XElement(Xspf + "duration", item.DurationMs));
            trackList.Add(track);
        }
        var doc = new XDocument(new XDeclaration("1.0", "UTF-8", null),
            new XElement(Xspf + "playlist", new XAttribute("version", "1"),
                new XElement(Xspf + "title", "Playlist"), trackList));
        return doc.Declaration + "\n" + doc.Root;
    }

    private static string Resolve(string location, string baseDir)
    {
        if (location.Contains("://", StringComparison.Ordinal))
        {
            if (Uri.TryCreate(location, UriKind.Absolute, out var uri) && uri.IsFile) return uri.LocalPath;
            return location;
        }
        if (location.StartsWith(@"\\", StringComparison.Ordinal) || Path.IsPathFullyQualified(location)) return location;
        return Path.GetFullPath(Path.Combine(baseDir, location.Replace('/', Path.DirectorySeparatorChar)));
    }

    private static bool TryIndexed(string key, string prefix, out int index)
    {
        index = 0;
        return key.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) &&
               int.TryParse(key.AsSpan(prefix.Length), NumberStyles.Integer, CultureInfo.InvariantCulture, out index);
    }

    private static IEnumerable<string> SplitLines(string text) =>
        text.Split(new[] { "\r\n", "\n", "\r" }, StringSplitOptions.None);
}
