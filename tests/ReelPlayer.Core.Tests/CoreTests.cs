using ReelPlayer.Core.Models;
using ReelPlayer.Core.Services;

namespace ReelPlayer.Core.Tests;

public class TimeFormatTests
{
    [Theory]
    [InlineData(0, "00:00")]
    [InlineData(5_000, "00:05")]
    [InlineData(65_000, "01:05")]
    [InlineData(3_600_000, "1:00:00")]
    [InlineData(36_125_000, "10:02:05")]
    [InlineData(-10, "00:00")]
    public void Format(long ms, string expected) => Assert.Equal(expected, TimeFormat.Format(ms));

    [Theory]
    [InlineData("90", 90_000)]
    [InlineData("1:30", 90_000)]
    [InlineData("1:02:03", 3_723_000)]
    [InlineData("0:00:01.5", 1_500)]
    [InlineData(" 12:00 ", 720_000)]
    public void TryParse_valid(string text, long expected)
    {
        Assert.True(TimeFormat.TryParse(text, out var ms));
        Assert.Equal(expected, ms);
    }

    [Theory]
    [InlineData("")]
    [InlineData("abc")]
    [InlineData("1:75")]
    [InlineData("1:2:3:4")]
    [InlineData("-5")]
    public void TryParse_invalid(string text) => Assert.False(TimeFormat.TryParse(text, out _));
}

public class PlaylistParserTests
{
    private const string Base = @"C:\Media";

    [Fact]
    public void M3U_reads_titles_durations_relative_and_urls()
    {
        var text = "#EXTM3U\n#EXTINF:123,First Song\nmusic/a.mp3\n#EXTINF:-1 tvg-id=\"x\",Live\nhttps://example.com/live.m3u8\n\nC:\\Other\\b.mkv\n";
        var items = PlaylistParser.ParseM3U(text, Base);
        Assert.Equal(3, items.Count);
        Assert.Equal(@"C:\Media\music\a.mp3", items[0].Location);
        Assert.Equal("First Song", items[0].Title);
        Assert.Equal(123_000, items[0].DurationMs);
        Assert.Equal("https://example.com/live.m3u8", items[1].Location);
        Assert.Equal("Live", items[1].Title);
        Assert.Equal(0, items[1].DurationMs);
        Assert.Equal(@"C:\Other\b.mkv", items[2].Location);
        Assert.Equal("b.mkv", items[2].Title);
    }

    [Fact]
    public void M3U_converts_file_uris()
    {
        var items = PlaylistParser.ParseM3U("file:///C:/Videos/My%20Movie.mp4", Base);
        Assert.Equal(@"C:\Videos\My Movie.mp4", items.Single().Location);
    }

    [Fact]
    public void Pls_orders_by_index()
    {
        var text = "[playlist]\nFile2=two.mp3\nTitle2=Two\nFile1=one.mp3\nLength1=60\nNumberOfEntries=2\n";
        var items = PlaylistParser.ParsePls(text, Base);
        Assert.Equal(new[] { @"C:\Media\one.mp3", @"C:\Media\two.mp3" }, items.Select(i => i.Location));
        Assert.Equal(60_000, items[0].DurationMs);
        Assert.Equal("Two", items[1].Title);
    }

    [Fact]
    public void Xspf_reads_tracks()
    {
        var text = """
            <?xml version="1.0" encoding="UTF-8"?>
            <playlist version="1" xmlns="http://xspf.org/ns/0/"><trackList>
              <track><location>file:///C:/A/x.mkv</location><title>X</title><duration>5000</duration></track>
              <track><location>rel/y.mp4</location></track>
            </trackList></playlist>
            """;
        var items = PlaylistParser.ParseXspf(text, Base);
        Assert.Equal(@"C:\A\x.mkv", items[0].Location);
        Assert.Equal("X", items[0].Title);
        Assert.Equal(5000, items[0].DurationMs);
        Assert.Equal(@"C:\Media\rel\y.mp4", items[1].Location);
    }

    [Fact]
    public void M3U_and_Xspf_round_trip()
    {
        var items = new[] { new MediaItem(@"C:\V\a b.mkv", "A B", 61_000), new MediaItem("https://h/s.m3u8", "Stream") };
        var m3u = PlaylistParser.ParseM3U(PlaylistParser.ToM3U(items), Base);
        Assert.Equal(items.Select(i => (i.Location, i.Title)), m3u.Select(i => (i.Location, i.Title)));
        Assert.Equal(61_000, m3u[0].DurationMs);

        var xspf = PlaylistParser.ParseXspf(PlaylistParser.ToXspf(items), Base);
        Assert.Equal(items.Select(i => (i.Location, i.Title)), xspf.Select(i => (i.Location, i.Title)));
    }

    [Fact]
    public void Hls_m3u8_is_not_expanded()
    {
        var dir = Directory.CreateTempSubdirectory();
        try
        {
            var hls = Path.Combine(dir.FullName, "stream.m3u8");
            File.WriteAllText(hls, "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\nseg1.ts\n");
            var list = Path.Combine(dir.FullName, "list.m3u8");
            File.WriteAllText(list, "#EXTM3U\na.mp3\n");
            Assert.False(PlaylistParser.ShouldExpand(hls));
            Assert.True(PlaylistParser.ShouldExpand(list));
        }
        finally { dir.Delete(true); }
    }
}

public class PlaylistTests
{
    private static Playlist Make(int n, LoopMode loop = LoopMode.Off, bool shuffle = false)
    {
        var p = new Playlist(new Random(42)) { Loop = loop, Shuffle = shuffle };
        p.AddRange(Enumerable.Range(1, n).Select(i => new MediaItem($@"C:\m\{i}.mp4")));
        return p;
    }

    [Fact]
    public void Sequential_next_stops_at_end_without_loop()
    {
        var p = Make(3);
        p.SetCurrent(2);
        Assert.Null(p.PeekNext(userInitiated: false));
    }

    [Fact]
    public void Loop_all_wraps()
    {
        var p = Make(3, LoopMode.All);
        p.SetCurrent(2);
        Assert.Same(p.Items[0], p.PeekNext(false));
        p.SetCurrent(0);
        Assert.Same(p.Items[2], p.PeekPrevious());
    }

    [Fact]
    public void Loop_one_repeats_automatically_but_next_button_advances()
    {
        var p = Make(3, LoopMode.One);
        p.SetCurrent(1);
        Assert.Same(p.Items[1], p.PeekNext(false));
        Assert.Same(p.Items[2], p.PeekNext(true));
    }

    [Fact]
    public void Shuffle_plays_every_item_once_per_round()
    {
        var p = Make(6, shuffle: true);
        var seen = new List<MediaItem>();
        while (p.PeekNext(false) is { } next)
        {
            seen.Add(next);
            p.SetCurrent(next);
            Assert.True(seen.Count <= 6, "shuffle without loop must end");
        }
        Assert.Equal(6, seen.Distinct().Count());
    }

    [Fact]
    public void Current_follows_item_when_reordered_and_clears_when_removed()
    {
        var p = Make(3);
        var b = p.Items[1];
        p.SetCurrent(b);
        p.Move(1, 0);
        Assert.Equal(0, p.CurrentIndex);
        Assert.True(b.IsCurrent);
        p.Remove(b);
        Assert.Null(p.Current);
        Assert.False(b.IsCurrent);
    }

    [Fact]
    public void Sort_by_title_descending()
    {
        var p = Make(3);
        p.Sort(i => i.Title, descending: true);
        Assert.Equal(new[] { "3.mp4", "2.mp4", "1.mp4" }, p.Items.Select(i => i.Title));
    }
}

public class HotkeyMapTests
{
    [Theory]
    [InlineData("shift+ctrl+left", "Ctrl+Shift+Left")]
    [InlineData("s", "S")]
    [InlineData("Control+Alt+Q", "Ctrl+Alt+Q")]
    [InlineData("esc", "Escape")]
    [InlineData("Ctrl++", "Ctrl++")]
    public void Normalize(string input, string expected) => Assert.Equal(expected, HotkeyMap.Normalize(input));

    [Fact]
    public void Defaults_match_vlc()
    {
        var map = new HotkeyMap();
        Assert.Equal("PlayPause", map.Resolve("Space"));
        Assert.Equal("Snapshot", map.Resolve("Shift+S"));
        Assert.Equal("Stop", map.Resolve("S"));
        Assert.Equal("SeekFwd300", map.Resolve("Ctrl+Right"));
        Assert.Equal("Faster", map.Resolve("]"));
        Assert.Equal("TogglePlaylist", map.Resolve("Ctrl+L"));
        Assert.Equal("RotateVideo", map.Resolve("Ctrl+R"));
        Assert.Equal("Ctrl+O", map.GestureText("OpenFile"));
    }

    [Fact]
    public void Defaults_have_no_conflicts()
    {
        var all = HotkeyActions.All.SelectMany(a => a.DefaultGestures.Select(HotkeyMap.Normalize)).ToList();
        Assert.Equal(all.Count, all.Distinct(StringComparer.OrdinalIgnoreCase).Count());
    }

    [Fact]
    public void Overrides_replace_defaults()
    {
        var map = new HotkeyMap(new Dictionary<string, string> { ["PlayPause"] = "K | Ctrl+Space" });
        Assert.Equal("PlayPause", map.Resolve("k"));
        Assert.Equal("PlayPause", map.Resolve("Ctrl+Space"));
        Assert.Null(map.Resolve("Space"));
    }
}

public class ResumeStoreTests
{
    [Theory]
    [InlineData(60_000, 10 * 60_000, true)]
    [InlineData(5_000, 10 * 60_000, false)]          // too close to the start
    [InlineData(10 * 60_000 - 5_000, 10 * 60_000, false)] // essentially finished
    [InlineData(60_000, 3 * 60_000, false)]           // short file
    public void IsResumable(long pos, long len, bool expected) => Assert.Equal(expected, ResumeStore.IsResumable(pos, len));

    [Fact]
    public void Update_save_and_reload()
    {
        var dir = Directory.CreateTempSubdirectory();
        try
        {
            var store = new ResumeStore(dir.FullName);
            store.Update(@"C:\a.mkv", 120_000, 3_600_000);
            store.Update(@"C:\b.mkv", 2_000, 3_600_000);
            store.Save();

            var reloaded = new ResumeStore(dir.FullName);
            Assert.Equal(120_000, reloaded.Get(@"c:\A.MKV"));
            Assert.Null(reloaded.Get(@"C:\b.mkv"));

            reloaded.Update(@"C:\a.mkv", 3_599_000, 3_600_000);
            Assert.Null(reloaded.Get(@"C:\a.mkv"));
        }
        finally { dir.Delete(true); }
    }
}

public class SettingsTests
{
    [Fact]
    public void Round_trip()
    {
        var dir = Directory.CreateTempSubdirectory();
        try
        {
            var svc = new SettingsService(dir.FullName);
            svc.Load();
            svc.Current.Volume = 135;
            svc.Current.Loop = LoopMode.One;
            svc.Current.Hotkeys["Mute"] = "Ctrl+M";
            svc.Save();

            var again = new SettingsService(dir.FullName);
            var s = again.Load();
            Assert.Equal(135, s.Volume);
            Assert.Equal(LoopMode.One, s.Loop);
            Assert.Equal("Ctrl+M", s.Hotkeys["Mute"]);
        }
        finally { dir.Delete(true); }
    }

    [Fact]
    public void Corrupt_file_falls_back_to_defaults()
    {
        var dir = Directory.CreateTempSubdirectory();
        try
        {
            File.WriteAllText(Path.Combine(dir.FullName, "settings.json"), "{ not json");
            var s = new SettingsService(dir.FullName).Load();
            Assert.Equal(100, s.Volume);
        }
        finally { dir.Delete(true); }
    }

    [Fact]
    public void Engine_options_reflect_settings()
    {
        var s = new AppSettings { HardwareDecoding = false, SubtitleColor = "#FFFF00", SubtitleLanguage = "en", AutoLoadSubtitles = false };
        var opts = PlayerService.BuildOptions(s);
        Assert.Contains("--avcodec-hw=none", opts);
        Assert.Contains("--freetype-color=" + 0xFFFF00, opts);
        Assert.Contains("--sub-language=en", opts);
        Assert.Contains("--no-sub-autodetect-file", opts);
    }

    [Fact]
    public void Video_filter_options()
    {
        Assert.Empty(PlayerService.VideoFilterOptions(null, false, 0.05));
        Assert.Equal(new[] { "--video-filter=transform", "--transform-type=90" },
            PlayerService.VideoFilterOptions("90", false, 0.05));
        Assert.Equal(new[] { "--video-filter=transform:sharpen", "--transform-type=hflip", "--sharpen-sigma=0.5" },
            PlayerService.VideoFilterOptions("hflip", true, 0.5));
    }
}

public class CommandLineAndFilesTests
{
    [Fact]
    public void Parses_switches_and_locations()
    {
        var cmd = CommandLine.Parse(new[] { "--fullscreen", "--start-time=90.5", @"C:\a.mkv", "https://x/y", "--bogus" });
        Assert.True(cmd.Fullscreen);
        Assert.Equal(90_500, cmd.StartTimeMs);
        Assert.Equal(new[] { @"C:\a.mkv", "https://x/y" }, cmd.Locations);
    }

    [Fact]
    public void Start_time_accepts_clock_format()
    {
        Assert.Equal(3_723_000, CommandLine.Parse(new[] { "--start-time=1:02:03" }).StartTimeMs);
    }

    [Fact]
    public void Natural_sort()
    {
        var sorted = new[] { "ep10.mkv", "ep2.mkv", "Ep1.mkv" }.OrderBy(x => x, NaturalStringComparer.Instance);
        Assert.Equal(new[] { "Ep1.mkv", "ep2.mkv", "ep10.mkv" }, sorted);
    }

    [Theory]
    [InlineData(@"C:\v\movie.MKV", true)]
    [InlineData(@"C:\v\song.flac", true)]
    [InlineData(@"C:\v\notes.txt", false)]
    public void Recognises_media(string path, bool expected) => Assert.Equal(expected, MediaFiles.IsMedia(path));

    [Fact]
    public void MediaItem_titles_and_mrls()
    {
        var file = new MediaItem(@"C:\Films\Big Movie.mkv");
        Assert.True(file.IsFile);
        Assert.Equal("Big Movie.mkv", file.Title);
        Assert.Equal("file:///C:/Films/Big%20Movie.mkv", file.Mrl);

        var url = new MediaItem("https://example.com/path/clip.mp4?x=1");
        Assert.False(url.IsFile);
        Assert.Equal("clip.mp4", url.Title);
        Assert.Equal("https://example.com/path/clip.mp4?x=1", url.Mrl);

        var disc = new MediaItem("dvd:///D:/");
        Assert.False(disc.IsFile);
    }

    [Fact]
    public void FindSiblingMedia_steps_naturally_and_skips_non_media()
    {
        var dir = Directory.CreateTempSubdirectory();
        try
        {
            var f1 = Path.Combine(dir.FullName, "ep1.mkv");
            var f2 = Path.Combine(dir.FullName, "ep2.mkv");
            var f10 = Path.Combine(dir.FullName, "ep10.mkv");
            var note = Path.Combine(dir.FullName, "ep3.txt");
            var pl = Path.Combine(dir.FullName, "ep4.m3u8");
            File.WriteAllText(f1, ""); File.WriteAllText(f2, ""); File.WriteAllText(f10, "");
            File.WriteAllText(note, ""); File.WriteAllText(pl, "");

            Assert.Equal(f2, MediaFiles.FindSiblingMedia(f1, +1));
            Assert.Equal(f10, MediaFiles.FindSiblingMedia(f2, +1));
            Assert.Null(MediaFiles.FindSiblingMedia(f10, +1));
            Assert.Equal(f2, MediaFiles.FindSiblingMedia(f10, -1));
            Assert.Null(MediaFiles.FindSiblingMedia(f1, -1));
        }
        finally { dir.Delete(true); }
    }
}
