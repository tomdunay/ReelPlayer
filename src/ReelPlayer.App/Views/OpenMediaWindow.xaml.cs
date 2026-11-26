using System.IO;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using ReelPlayer.Core.Models;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.Views;

public partial class OpenMediaWindow : Window
{
    public const int TabFile = 0, TabDisc = 1, TabNetwork = 2, TabCapture = 3;

    private readonly AppSettings _settings;

    public OpenMediaWindow(AppSettings settings, int tab)
    {
        _settings = settings;
        InitializeComponent();
        Tabs.SelectedIndex = tab;

        foreach (var d in DriveInfo.GetDrives().Where(d => d.DriveType == DriveType.CDRom))
            DriveBox.Items.Add(d.Name);
        if (DriveBox.Items.Count > 0) DriveBox.SelectedIndex = 0;

        foreach (var url in settings.Recent.Where(r => r.Contains("://", StringComparison.Ordinal)))
            UrlBox.Items.Add(url);
        if (Clipboard.ContainsText() && Clipboard.GetText().Trim() is var clip && clip.Contains("://") && !clip.Contains('\n'))
            UrlBox.Text = clip;

        Loaded += (_, _) =>
        {
            if (tab == TabNetwork) UrlBox.Focus();
        };
    }

    public List<MediaItem> Result { get; } = new();
    public bool EnqueueOnly { get; private set; }

    private void AddFiles_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Multiselect = true,
            Filter = MediaFiles.DialogFilter(),
            InitialDirectory = _settings.LastFolder is { } f && Directory.Exists(f) ? f : null,
        };
        if (dlg.ShowDialog(this) != true) return;
        foreach (var file in dlg.FileNames) FileList.Items.Add(file);
        _settings.LastFolder = Path.GetDirectoryName(dlg.FileNames[0]);
    }

    private void RemoveFiles_Click(object sender, RoutedEventArgs e)
    {
        foreach (var item in FileList.SelectedItems.Cast<object>().ToList()) FileList.Items.Remove(item);
    }

    private void BrowseSubtitle_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = MediaFiles.SubtitleDialogFilter() };
        if (dlg.ShowDialog(this) == true) SubtitleBox.Text = dlg.FileName;
    }

    private void BrowseDisc_Click(object sender, RoutedEventArgs e)
    {
        // Folders (VIDEO_TS / BDMV) or an ISO image.
        var choice = MessageBox.Show(this, "Open a disc folder?\n\nYes = folder (VIDEO_TS / BDMV)\nNo = ISO image file",
            "Open Disc", MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
        if (choice == MessageBoxResult.Yes)
        {
            var dlg = new OpenFolderDialog();
            if (dlg.ShowDialog(this) == true) DriveBox.Text = dlg.FolderName;
        }
        else if (choice == MessageBoxResult.No)
        {
            var dlg = new OpenFileDialog { Filter = "Disc images|*.iso|All files|*.*" };
            if (dlg.ShowDialog(this) == true) DriveBox.Text = dlg.FileName;
        }
    }

    private void CaptureMode_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (DshowPanel is null) return;
        var desktop = CaptureMode.SelectedIndex == 1;
        DshowPanel.Visibility = desktop ? Visibility.Collapsed : Visibility.Visible;
        DesktopPanel.Visibility = desktop ? Visibility.Visible : Visibility.Collapsed;
    }

    private void Play_Click(object sender, RoutedEventArgs e) => Finish(enqueue: false);
    private void Enqueue_Click(object sender, RoutedEventArgs e) => Finish(enqueue: true);

    private void Finish(bool enqueue)
    {
        Result.Clear();
        try
        {
            BuildResult();
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, ex.Message, "Open Media", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (Result.Count == 0)
        {
            MessageBox.Show(this, "Nothing to open yet.", "Open Media", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        if (!string.IsNullOrWhiteSpace(StartTimeBox.Text))
        {
            if (!TimeFormat.TryParse(StartTimeBox.Text, out var ms))
            {
                MessageBox.Show(this, "Start time must look like 1:23:45, 12:30 or 90.", "Open Media", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            Result[0].Options.Add(":start-time=" + (ms / 1000.0).ToString("0.###", System.Globalization.CultureInfo.InvariantCulture));
        }

        EnqueueOnly = enqueue;
        DialogResult = true;
    }

    private void BuildResult()
    {
        switch (Tabs.SelectedIndex)
        {
            case TabFile:
                foreach (string f in FileList.Items) Result.Add(new MediaItem(f));
                if (UseSubtitle.IsChecked == true && File.Exists(SubtitleBox.Text) && Result.Count > 0)
                    Result[0].SubtitleFile = SubtitleBox.Text;
                break;

            case TabDisc:
                var path = (DriveBox.Text ?? "").Trim();
                if (path.Length == 0) throw new InvalidOperationException("Choose a disc drive, folder or ISO image.");
                var target = path.Replace('\\', '/');
                if (!target.EndsWith('/') && !target.EndsWith(".iso", StringComparison.OrdinalIgnoreCase)) target += "/";
                int.TryParse(DiscTitle.Text, out var title);
                int.TryParse(DiscChapter.Text, out var chapter);

                string mrl, name;
                if (DiscCd.IsChecked == true)
                {
                    mrl = "cdda:///" + target;
                    name = "Audio CD";
                }
                else if (DiscBluray.IsChecked == true)
                {
                    mrl = "bluray:///" + target;
                    name = "Blu-ray";
                }
                else
                {
                    mrl = (NoMenus.IsChecked == true ? "dvdsimple:///" : "dvd:///") + target;
                    name = "DVD";
                }
                if (DiscCd.IsChecked != true && (title > 0 || chapter > 0))
                    mrl += $"#{Math.Max(title, 0)}" + (chapter > 0 ? $":{chapter}" : "");
                var item = new MediaItem(mrl, $"{name} ({path})");
                if (DiscCd.IsChecked == true && title > 0) item.Options.Add($":cdda-track={title}");
                if (DiscBluray.IsChecked == true && NoMenus.IsChecked == true) item.Options.Add(":no-bluray-menu");
                Result.Add(item);
                break;

            case TabNetwork:
                var url = (UrlBox.Text ?? "").Trim();
                if (url.Length == 0) throw new InvalidOperationException("Enter a network URL.");
                if (!url.Contains("://", StringComparison.Ordinal) && !url.StartsWith(@"\\", StringComparison.Ordinal))
                    throw new InvalidOperationException("The URL needs a scheme, e.g. https:// or rtsp://");
                Result.Add(new MediaItem(url));
                break;

            case TabCapture:
                if (CaptureMode.SelectedIndex == 1)
                {
                    var desktop = new MediaItem("screen://", "Desktop");
                    var fps = double.TryParse(DesktopFps.Text, System.Globalization.NumberStyles.Float,
                        System.Globalization.CultureInfo.InvariantCulture, out var f) && f > 0 ? f : 30;
                    desktop.Options.Add(":screen-fps=" + fps.ToString(System.Globalization.CultureInfo.InvariantCulture));
                    Result.Add(desktop);
                }
                else
                {
                    var dev = new MediaItem("dshow://", string.IsNullOrWhiteSpace(VideoDevice.Text) ? "Capture device" : VideoDevice.Text.Trim());
                    dev.Options.Add(":dshow-vdev=" + VideoDevice.Text.Trim());
                    dev.Options.Add(":dshow-adev=" + AudioDevice.Text.Trim());
                    dev.Options.Add(":live-caching=300");
                    Result.Add(dev);
                }
                break;
        }
    }
}
