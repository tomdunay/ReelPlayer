using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.Win32;
using ReelPlayer.App.Services;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.Views;

public partial class PreferencesWindow : Window
{
    public sealed partial class HotkeyRow : ObservableObject
    {
        public required string Id { get; init; }
        public required string Description { get; init; }
        [ObservableProperty] public partial string Gestures { get; set; } = "";
    }

    private static readonly string[] Encodings =
    {
        "", "UTF-8", "Windows-1250", "Windows-1251", "Windows-1252", "Windows-1253", "Windows-1254", "Windows-1255",
        "Windows-1256", "Windows-1257", "Windows-1258", "ISO-8859-1", "ISO-8859-2", "ISO-8859-15", "KOI8-R",
        "GB18030", "Big5", "Shift_JIS", "EUC-KR", "CP949",
    };

    private readonly SettingsService _service;
    private readonly List<HotkeyRow> _hotkeyRows;

    public PreferencesWindow(SettingsService service)
    {
        _service = service;
        InitializeComponent();
        foreach (var e in Encodings) SubEncoding.Items.Add(e.Length == 0 ? "(automatic)" : e);

        var s = service.Current;
        var map = new HotkeyMap(s.Hotkeys);
        _hotkeyRows = HotkeyActions.All
            .Select(a => new HotkeyRow { Id = a.Id, Description = a.Description, Gestures = string.Join(" | ", map.GesturesFor(a.Id)) })
            .ToList();
        HotkeyList.ItemsSource = _hotkeyRows;
        Load(s);
    }

    /// <summary>True when a changed option only applies after restarting (libVLC options).</summary>
    public bool RestartRequired { get; private set; }

    private void Load(AppSettings s)
    {
        ThemeBox.SelectedIndex = s.Theme switch { "Light" => 1, "System" => 2, _ => 0 };
        OneInstance.IsChecked = s.OneInstance;
        Enqueue.IsChecked = s.EnqueueWhenRunning;
        Resume.IsChecked = s.ResumePlayback;
        PauseOnMinimize.IsChecked = s.PauseOnMinimize;
        AutoAdvanceFolder.IsChecked = s.AutoAdvanceFolder;
        StartFullscreen.IsChecked = s.StartFullscreen;
        HwDecode.IsChecked = s.HardwareDecoding;
        SnapshotFolder.Text = s.SnapshotFolder;
        SnapshotFormat.SelectedIndex = s.SnapshotFormat == "jpg" ? 1 : 0;
        AutoSubs.IsChecked = s.AutoLoadSubtitles;
        SubLanguage.Text = s.SubtitleLanguage;
        SubEncoding.Text = string.IsNullOrEmpty(s.SubtitleEncoding) ? "(automatic)" : s.SubtitleEncoding;
        SelectByContent(SubSize, s.SubtitleSize);
        SelectByContent(SubOutline, s.SubtitleOutline);
        SubColor.SelectedItem = SubColor.Items.Cast<ComboBoxItem>()
            .FirstOrDefault(i => string.Equals((string)i.Tag, s.SubtitleColor, StringComparison.OrdinalIgnoreCase)) ?? SubColor.Items[0];
    }

    private static void SelectByContent(ComboBox box, string value) =>
        box.SelectedItem = box.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (string)i.Content == value) ?? box.Items[0];

    private static string Selected(ComboBox box) => (string)((ComboBoxItem)box.SelectedItem).Content;

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        var s = _service.Current;
        var before = PlayerService.BuildOptions(s);
        var beforeOneInstance = s.OneInstance;

        s.Theme = Selected(ThemeBox);
        s.OneInstance = OneInstance.IsChecked == true;
        s.EnqueueWhenRunning = Enqueue.IsChecked == true;
        s.ResumePlayback = Resume.IsChecked == true;
        s.PauseOnMinimize = PauseOnMinimize.IsChecked == true;
        s.AutoAdvanceFolder = AutoAdvanceFolder.IsChecked == true;
        s.StartFullscreen = StartFullscreen.IsChecked == true;
        s.HardwareDecoding = HwDecode.IsChecked == true;
        s.SnapshotFolder = SnapshotFolder.Text.Trim();
        s.SnapshotFormat = Selected(SnapshotFormat);
        s.AutoLoadSubtitles = AutoSubs.IsChecked == true;
        s.SubtitleLanguage = SubLanguage.Text.Trim();
        var enc = (SubEncoding.Text ?? "").Trim();
        s.SubtitleEncoding = enc == "(automatic)" ? "" : enc;
        s.SubtitleSize = Selected(SubSize);
        s.SubtitleOutline = Selected(SubOutline);
        s.SubtitleColor = (string)((ComboBoxItem)SubColor.SelectedItem).Tag;

        // Store only hotkeys that differ from the defaults.
        s.Hotkeys.Clear();
        foreach (var row in _hotkeyRows)
        {
            var normalized = string.Join("|", HotkeyMap.Split(row.Gestures));
            var defaults = string.Join("|", HotkeyActions.Find(row.Id)!.DefaultGestures.Select(HotkeyMap.Normalize));
            if (!string.Equals(normalized, defaults, StringComparison.OrdinalIgnoreCase))
                s.Hotkeys[row.Id] = normalized;
        }

        RestartRequired = !before.SequenceEqual(PlayerService.BuildOptions(s)) || s.OneInstance != beforeOneInstance;
        DialogResult = true;
    }

    private void Reset_Click(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show(this, "Reset all preferences to their defaults?", "Reset Preferences",
                MessageBoxButton.OKCancel, MessageBoxImage.Question) != MessageBoxResult.OK) return;
        Load(new AppSettings());
        ResetHotkeys_Click(sender, e);
    }

    private void ResetHotkeys_Click(object sender, RoutedEventArgs e)
    {
        foreach (var row in _hotkeyRows)
            row.Gestures = string.Join(" | ", HotkeyActions.Find(row.Id)!.DefaultGestures.Select(HotkeyMap.Normalize));
    }

    /// <summary>Records the pressed combination instead of typing characters.</summary>
    private void HotkeyBox_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (sender is not TextBox { DataContext: HotkeyRow row }) return;
        if (e.Key is Key.Tab) return;
        e.Handled = true;
        if (e.Key is Key.Back or Key.Delete && Keyboard.Modifiers == ModifierKeys.None)
        {
            row.Gestures = "";
            return;
        }
        if (MainWindow.GestureFromKey(e) is { } gesture)
            row.Gestures = gesture;
    }

    private void BrowseSnapshot_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog { InitialDirectory = SnapshotFolder.Text };
        if (dlg.ShowDialog(this) == true) SnapshotFolder.Text = dlg.FolderName;
    }

    private void Associate_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            FileAssociations.Register();
            FileAssociations.OpenDefaultAppsSettings();
        }
        catch (Exception ex)
        {
            AppLog.Write(ex, "Register associations");
            MessageBox.Show(this, ex.Message, "File associations", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void Unassociate_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            FileAssociations.Unregister();
            MessageBox.Show(this, "Reel Player's file type registration was removed.", "File associations");
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, ex.Message, "File associations", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }
}
