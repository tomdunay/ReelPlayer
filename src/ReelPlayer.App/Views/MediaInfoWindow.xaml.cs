using System.Text;
using System.Windows;
using System.Windows.Threading;
using LibVLCSharp.Shared;
using ReelPlayer.App.ViewModels;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.Views;

public partial class MediaInfoWindow : Window
{
    public sealed record Row(string Key, string Value);
    public sealed record Stream(string Header, IReadOnlyList<Row> Rows);

    private readonly MainViewModel _vm;
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(1) };
    private Media? _shownMedia;

    public MediaInfoWindow(MainViewModel vm, bool codecTab)
    {
        _vm = vm;
        InitializeComponent();
        SelectTab(codecTab);
        _timer.Tick += (_, _) => Refresh();
        _timer.Start();
        Closed += (_, _) => _timer.Stop();
        Refresh();
    }

    public void SelectTab(bool codec) => Tabs.SelectedIndex = codec ? 1 : 0;

    private void Refresh()
    {
        var media = _vm.Engine.CurrentMedia;
        if (media is null || !_vm.Engine.IsActive)
        {
            GeneralList.ItemsSource = new[] { new Row("Status", "No media is playing.") };
            CodecList.ItemsSource = null;
            StatsList.ItemsSource = null;
            LocationBox.Text = "";
            _shownMedia = null;
            return;
        }

        // Metadata and streams rarely change; rebuild them when the media changes or streams appear.
        var tracks = media.Tracks;
        if (!ReferenceEquals(media, _shownMedia) || (CodecList.ItemsSource as IList<Stream>)?.Count != tracks.Length)
        {
            _shownMedia = media;
            LocationBox.Text = _vm.Playlist.Current?.Location ?? media.Mrl;
            GeneralList.ItemsSource = BuildGeneral(media);
            CodecList.ItemsSource = BuildStreams(media, tracks);
        }
        StatsList.ItemsSource = BuildStats(media.Statistics);
    }

    private IReadOnlyList<Row> BuildGeneral(Media media)
    {
        var rows = new List<Row>();
        void Add(string key, string? value)
        {
            if (!string.IsNullOrWhiteSpace(value)) rows.Add(new Row(key, value));
        }
        Add("Title", media.Meta(MetadataType.Title) ?? _vm.Title);
        Add("Artist", media.Meta(MetadataType.Artist));
        Add("Album", media.Meta(MetadataType.Album));
        Add("Album artist", media.Meta(MetadataType.AlbumArtist));
        Add("Genre", media.Meta(MetadataType.Genre));
        Add("Track number", media.Meta(MetadataType.TrackNumber));
        Add("Date", media.Meta(MetadataType.Date));
        Add("Description", media.Meta(MetadataType.Description));
        Add("Copyright", media.Meta(MetadataType.Copyright));
        Add("Publisher", media.Meta(MetadataType.Publisher));
        Add("Encoded by", media.Meta(MetadataType.EncodedBy));
        Add("Language", media.Meta(MetadataType.Language));
        Add("Now playing", media.Meta(MetadataType.NowPlaying));
        Add("Artwork", media.Meta(MetadataType.ArtworkURL));
        Add("Duration", _vm.LengthMs > 0 ? TimeFormat.Format(_vm.LengthMs) : "Unknown (live?)");
        return rows;
    }

    private static IReadOnlyList<Stream> BuildStreams(Media media, MediaTrack[] tracks)
    {
        var list = new List<Stream>();
        var index = 0;
        foreach (var t in tracks)
        {
            var rows = new List<Row>
            {
                new("Codec", $"{media.CodecDescription(t.TrackType, t.Codec)} ({FourCC(t.Codec)})"),
            };
            if (!string.IsNullOrEmpty(t.Language)) rows.Add(new Row("Language", t.Language));
            if (!string.IsNullOrEmpty(t.Description)) rows.Add(new Row("Description", t.Description));
            switch (t.TrackType)
            {
                case TrackType.Video:
                    var v = t.Data.Video;
                    rows.Add(new Row("Resolution", $"{v.Width}x{v.Height}"));
                    if (v.FrameRateDen > 0) rows.Add(new Row("Frame rate", $"{(double)v.FrameRateNum / v.FrameRateDen:0.###} fps"));
                    if (v.SarDen > 0 && v.SarNum != v.SarDen) rows.Add(new Row("Sample aspect ratio", $"{v.SarNum}:{v.SarDen}"));
                    if (v.Orientation != VideoOrientation.TopLeft) rows.Add(new Row("Orientation", v.Orientation.ToString()));
                    break;
                case TrackType.Audio:
                    var a = t.Data.Audio;
                    rows.Add(new Row("Channels", a.Channels switch { 1 => "Mono", 2 => "Stereo", 6 => "5.1", 8 => "7.1", _ => a.Channels.ToString() }));
                    rows.Add(new Row("Sample rate", $"{a.Rate} Hz"));
                    break;
                case TrackType.Text:
                    if (!string.IsNullOrEmpty(t.Data.Subtitle.Encoding)) rows.Add(new Row("Encoding", t.Data.Subtitle.Encoding));
                    break;
            }
            if (t.Bitrate > 0) rows.Add(new Row("Bitrate", $"{t.Bitrate / 1000} kb/s"));
            var kind = t.TrackType switch { TrackType.Video => "Video", TrackType.Audio => "Audio", TrackType.Text => "Subtitle", _ => "Data" };
            list.Add(new Stream($"Stream {index++} — {kind}", rows));
        }
        if (list.Count == 0) list.Add(new Stream("No streams reported yet", Array.Empty<Row>()));
        return list;
    }

    private static IReadOnlyList<Row> BuildStats(MediaStats s) => new List<Row>
    {
        new("Input bitrate", $"{s.InputBitrate * 8000:0} kb/s"),
        new("Demux bitrate", $"{s.DemuxBitrate * 8000:0} kb/s"),
        new("Read", $"{s.ReadBytes / 1024.0 / 1024.0:0.0} MiB"),
        new("Demux corrupted", s.DemuxCorrupted.ToString()),
        new("Discontinuities", s.DemuxDiscontinuity.ToString()),
        new("Decoded video blocks", s.DecodedVideo.ToString()),
        new("Displayed frames", s.DisplayedPictures.ToString()),
        new("Lost frames", s.LostPictures.ToString()),
        new("Decoded audio blocks", s.DecodedAudio.ToString()),
        new("Played audio buffers", s.PlayedAudioBuffers.ToString()),
        new("Lost audio buffers", s.LostAudioBuffers.ToString()),
    };

    private static string FourCC(uint codec)
    {
        var sb = new StringBuilder(4);
        for (var i = 0; i < 4; i++)
        {
            var c = (char)((codec >> (8 * i)) & 0xFF);
            sb.Append(char.IsControl(c) ? ' ' : c);
        }
        return sb.ToString().Trim();
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Close();
}
