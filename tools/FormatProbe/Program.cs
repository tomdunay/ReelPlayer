// Plays every media file in a folder through libVLC (dummy audio/video output) and reports
// whether it opened, which streams were found, and whether frames/samples were decoded.
//
//   dotnet run --project tools/FormatProbe -- [--libvlc DIR] <samples folder> [report.md]
//   dotnet run --project tools/FormatProbe -- --libvlc DIR --cache-gen
//
// --libvlc DIR   use the libVLC in DIR (e.g. the trimmed copy in dist\) instead of the NuGet package's
// --cache-gen    write DIR\plugins\plugins.dat so libVLC doesn't have to load every plugin at startup

using System.Text;
using LibVLCSharp.Shared;

var rest = new List<string>();
string? libvlcDir = null;
var cacheGen = false;
for (var i = 0; i < args.Length; i++)
{
    if (args[i] == "--libvlc" && i + 1 < args.Length) libvlcDir = Path.GetFullPath(args[++i]);
    else if (args[i] == "--cache-gen") cacheGen = true;
    else rest.Add(args[i]);
}

if (libvlcDir is null) Core.Initialize();
else Core.Initialize(libvlcDir);

if (cacheGen)
{
    if (libvlcDir is null) { Console.Error.WriteLine("--cache-gen needs --libvlc DIR"); return 2; }
    var cache = Path.Combine(libvlcDir, "plugins", "plugins.dat");
    File.Delete(cache);
    using (new LibVLC("--reset-plugins-cache", "--no-video-title-show")) { }
    if (!File.Exists(cache)) { Console.Error.WriteLine("libVLC did not write " + cache); return 1; }
    Console.WriteLine($"Wrote {cache} ({new FileInfo(cache).Length / 1024} KB)");
    return 0;
}

var folder = rest.Count > 0 ? rest[0] : Path.Combine("tests", "samples");
var report = rest.Count > 1 ? rest[1] : null;
var skip = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ".srt", ".ass", ".vtt", ".txt", ".m3u8", ".m3u", ".pls", ".xspf" };

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
    .AppendLine($"libVLC {libvlc.Version}{(libvlcDir is null ? "" : " (" + libvlcDir + ")")} — {pass} passed, {fail} failed ({DateTime.Now:yyyy-MM-dd HH:mm})")
    .AppendLine()
    .AppendLine("| File | Result | Streams | Video blocks decoded | Audio blocks decoded | Note |")
    .AppendLine("|---|---|---|---|---|---|");
foreach (var r in rows) md.AppendLine(r);
Console.WriteLine();
Console.WriteLine($"{pass} passed, {fail} failed");
if (report is not null) File.WriteAllText(report, md.ToString());
return fail == 0 ? 0 : 1;
