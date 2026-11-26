// Plays every media file in a folder through libVLC (dummy audio/video output) and reports
// whether it opened, which streams were found, and whether frames/samples were decoded.
// Usage: dotnet run --project tools/FormatProbe -- <folder> [report.md]

using System.Text;
using LibVLCSharp.Shared;

var folder = args.Length > 0 ? args[0] : Path.Combine("tests", "samples");
var report = args.Length > 1 ? args[1] : null;
var skip = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ".srt", ".ass", ".vtt", ".txt", ".m3u8", ".m3u", ".pls", ".xspf" };

Core.Initialize();
using var libvlc = new LibVLC("--vout=dummy", "--aout=dummy", "--no-video-title-show", "--avcodec-hw=none");
string? lastError = null;
libvlc.Log += (_, e) => { if (e.Level == LogLevel.Error) lastError = $"{e.Module}: {e.Message}"; };

var rows = new List<string>();
int pass = 0, fail = 0;
foreach (var file in Directory.EnumerateFiles(folder).Where(f => !skip.Contains(Path.GetExtension(f))).OrderBy(f => f))
{
    lastError = null;
    using var media = new Media(libvlc, new Uri(Path.GetFullPath(file)));
    using var player = new MediaPlayer(media);
    var started = new ManualResetEventSlim();
    var failed = false;
    player.Playing += (_, _) => started.Set();
    player.EncounteredError += (_, _) => { failed = true; started.Set(); };

    player.Play();
    started.Wait(TimeSpan.FromSeconds(8));
    Thread.Sleep(1500);

    var stats = media.Statistics;
    var streams = media.Tracks.Select(t =>
    {
        var codec = media.CodecDescription(t.TrackType, t.Codec);
        return t.TrackType switch
        {
            TrackType.Video => $"V:{codec} {t.Data.Video.Width}x{t.Data.Video.Height}",
            TrackType.Audio => $"A:{codec} {t.Data.Audio.Channels}ch",
            TrackType.Text => $"S:{codec}",
            _ => $"?:{codec}",
        };
    }).ToList();
    var hasVideo = media.Tracks.Any(t => t.TrackType == TrackType.Video);
    var hasAudio = media.Tracks.Any(t => t.TrackType == TrackType.Audio);
    var decodedOk = (!hasVideo || stats.DecodedVideo > 0) && (!hasAudio || stats.DecodedAudio > 0) && (hasVideo || hasAudio);
    var ok = started.IsSet && !failed && decodedOk;
    player.Stop();

    if (ok) pass++; else fail++;
    var note = ok ? "" : (failed ? "error " : "") + (lastError ?? (started.IsSet ? "nothing decoded" : "timeout"));
    rows.Add($"| {Path.GetFileName(file)} | {(ok ? "PASS" : "FAIL")} | {string.Join("; ", streams)} | {stats.DecodedVideo} | {stats.DecodedAudio} | {note.Replace("|", "/")} |");
    Console.WriteLine($"{(ok ? "PASS" : "FAIL"),-5} {Path.GetFileName(file),-28} {string.Join("; ", streams)} {note}");
}

var md = new StringBuilder()
    .AppendLine($"libVLC {libvlc.Version} — {pass} passed, {fail} failed ({DateTime.Now:yyyy-MM-dd HH:mm})")
    .AppendLine()
    .AppendLine("| File | Result | Streams | Video blocks decoded | Audio blocks decoded | Note |")
    .AppendLine("|---|---|---|---|---|---|");
foreach (var r in rows) md.AppendLine(r);
Console.WriteLine();
Console.WriteLine($"{pass} passed, {fail} failed");
if (report is not null) File.WriteAllText(report, md.ToString());
return fail == 0 ? 0 : 1;
