using System.ComponentModel;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media.Animation;
using System.Windows.Shell;
using System.Windows.Threading;
using LibVLCSharp.Shared;
using Microsoft.Win32;
using ReelPlayer.App.Services;
using ReelPlayer.App.ViewModels;
using ReelPlayer.Core.Models;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.Views;

public partial class MainWindow : Window
{
    private readonly MainViewModel _vm;
    private HotkeyMap _hotkeys;
    private readonly DispatcherTimer _hideControlsTimer = new() { Interval = TimeSpan.FromSeconds(3) };
    private readonly DispatcherTimer _errorTimer = new() { Interval = TimeSpan.FromSeconds(6) };
    private readonly DispatcherTimer _resumeTimer = new() { Interval = TimeSpan.FromSeconds(10) };
    private Window? _overlayWindow;
    private Controls.ControlBar? _fullscreenControls;
    private PlaylistPanel? _playlistView;
    private bool _minimal;
    private Rect _restoreBounds;
    private WindowState _restoreState;
    private (MediaItem Item, long Position)? _resumeOffer;
    private Point _lastMouse;

    public MainWindow(MainViewModel vm)
    {
        _vm = vm;
        _hotkeys = new HotkeyMap(vm.Settings.Hotkeys);
        InitializeComponent();
        DataContext = vm;
        OverlayRoot.DataContext = vm;

        RestoreWindowPlacement();

        // Menus: one Click handler for everything, gestures shown from the hotkey map.
        MainMenu.AddHandler(MenuItem.ClickEvent, new RoutedEventHandler(Menu_Click));
        MainMenu.AddHandler(MenuItem.SubmenuOpenedEvent, new RoutedEventHandler(Menu_SubmenuOpened));
        ApplyGestureText(MainMenu.Items);

        DockedControls.ActionRequested += RunAction;

        vm.OsdRequested += ShowOsd;
        vm.ErrorRaised += ShowError;
        vm.ResumeOffered += OfferResume;
        vm.OpenFileRequested += () => RunAction("OpenFile");
        vm.PlayerReplaced += () => VideoView.MediaPlayer = _vm.Player;
        // Each time libVLC creates its video window (including after an engine rebuild), make its letterbox black.
        vm.Engine.VoutChanged += count =>
        {
            if (count > 0)
                Dispatcher.BeginInvoke(() => NativeMethods.BlackenVlcWindows(new System.Windows.Interop.WindowInteropHelper(this).Handle),
                    DispatcherPriority.Background);
        };
        vm.PropertyChanged += Vm_PropertyChanged;
        vm.Smtc.ButtonPressed += b => Dispatcher.BeginInvoke(() => OnMediaButton(b));

        _hideControlsTimer.Tick += (_, _) => HideFullscreenControls();
        _errorTimer.Tick += (_, _) => { ErrorBanner.Visibility = Visibility.Collapsed; _errorTimer.Stop(); };
        _resumeTimer.Tick += (_, _) => DismissResume();

        Loaded += OnLoaded;
        SourceInitialized += (_, _) =>
        {
            var hwnd = new System.Windows.Interop.WindowInteropHelper(this).Handle;
            NativeMethods.DisableBackdrop(hwnd);
            // Defer WinRT COM activation so it doesn't delay the first paint.
            Dispatcher.BeginInvoke(() => _vm.Smtc.Attach(hwnd), DispatcherPriority.Background);
        };
        PreviewKeyDown += Window_PreviewKeyDown;
        Drop += Window_Drop;
        DragOver += Window_DragOver;
        StateChanged += (_, _) => { if (WindowState == WindowState.Minimized && _vm.Settings.PauseOnMinimize) _vm.Pause(); };
        Closing += OnClosing;

        OverlayRoot.MouseLeftButtonDown += Overlay_MouseLeftButtonDown;
        OverlayRoot.MouseMove += Overlay_MouseMove;
        OverlayRoot.MouseWheel += (_, e) => _vm.ChangeVolume(e.Delta > 0 ? 5 : -5);
        OverlayRoot.Drop += Window_Drop;
        OverlayRoot.DragOver += Window_DragOver;
        OverlayRoot.ContextMenuOpening += Overlay_ContextMenuOpening;
        OverlayRoot.ContextMenu = new ContextMenu();

        SetPlaylistVisible(vm.Settings.ShowPlaylist);
        StatusBarPanel.Visibility = vm.Settings.ShowStatusBar ? Visibility.Visible : Visibility.Collapsed;
        Topmost = vm.Settings.AlwaysOnTop;
        UpdateTitle();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        // VideoView needs the MediaPlayer after it is loaded (its native window exists then).
        VideoView.MediaPlayer = _vm.Player;

        // The overlay content lives in a separate transparent window; forward its keys to us.
        _overlayWindow = GetWindow(OverlayRoot);
        if (_overlayWindow is not null && !ReferenceEquals(_overlayWindow, this))
        {
            _overlayWindow.PreviewKeyDown += Window_PreviewKeyDown;
            _overlayWindow.AllowDrop = true;
        }
    }

    // =====================================================================
    // Action dispatcher shared by menus, hotkeys, buttons and context menu
    // =====================================================================

    public void RunAction(string action)
    {
        try
        {
            if (action.StartsWith("set:", StringComparison.Ordinal)) { RunSetAction(action); return; }
            switch (action)
            {
                // Media
                case "OpenFile": OpenFiles(multiple: false); break;
                case "OpenMultiple": OpenFiles(multiple: true); break;
                case "OpenFolder": OpenFolder(); break;
                case "OpenDisc": ShowOpenDialog(OpenMediaWindow.TabDisc); break;
                case "OpenNetwork": ShowOpenDialog(OpenMediaWindow.TabNetwork); break;
                case "OpenCapture": ShowOpenDialog(OpenMediaWindow.TabCapture); break;
                case "SavePlaylist": SavePlaylist(); break;
                case "Quit": Close(); break;

                // Playback
                case "PlayPause": _vm.TogglePlayPause(); break;
                case "Stop": _vm.Stop(); break;
                case "Next": _vm.Next(); break;
                case "Previous": _vm.Previous(); break;
                case "NextFrame": _vm.NextFrame(); break;
                case "SeekBack10": _vm.SeekRelative(-10_000); break;
                case "SeekFwd10": _vm.SeekRelative(10_000); break;
                case "SeekBack3": _vm.SeekRelative(-3_000); break;
                case "SeekFwd3": _vm.SeekRelative(3_000); break;
                case "SeekBack60": _vm.SeekRelative(-60_000); break;
                case "SeekFwd60": _vm.SeekRelative(60_000); break;
                case "SeekBack300": _vm.SeekRelative(-300_000); break;
                case "SeekFwd300": _vm.SeekRelative(300_000); break;
                case "Faster": _vm.Faster(); break;
                case "Slower": _vm.Slower(); break;
                case "NormalSpeed": _vm.SetRate(1f); break;
                case "JumpToTime": JumpToTime(); break;
                case "Record": _vm.ToggleRecord(); break;
                case "CycleLoop":
                    _vm.Loop = _vm.Loop switch { LoopMode.Off => LoopMode.All, LoopMode.All => LoopMode.One, _ => LoopMode.Off };
                    ShowOsd(_vm.LoopTip);
                    break;
                case "ToggleShuffle":
                    _vm.Shuffle = !_vm.Shuffle;
                    ShowOsd(_vm.Shuffle ? "Random: on" : "Random: off");
                    break;

                // Audio
                case "VolumeUp": _vm.ChangeVolume(5); break;
                case "VolumeDown": _vm.ChangeVolume(-5); break;
                case "Mute": _vm.ToggleMute(); break;
                case "CycleAudio": _vm.CycleAudioTrack(); break;
                case "AudioDelayDown": _vm.ShiftAudioDelay(-50); break;
                case "AudioDelayUp": _vm.ShiftAudioDelay(50); break;

                // Video
                case "Fullscreen": SetFullscreen(!_vm.IsFullscreen); break;
                case "ExitFullscreen": if (_vm.IsFullscreen) SetFullscreen(false); break;
                case "AlwaysOnTop":
                    Topmost = _vm.Settings.AlwaysOnTop = !_vm.Settings.AlwaysOnTop;
                    ShowOsd(Topmost ? "Always on top: on" : "Always on top: off");
                    break;
                case "CycleAspect": _vm.SetAspect(_vm.AspectIndex + 1); break;
                case "CycleCrop": _vm.SetCrop(_vm.CropIndex + 1); break;
                case "CycleZoom": _vm.SetZoom(_vm.ZoomIndex + 1); break;
                case "Snapshot": _vm.TakeSnapshot(); break;
                case "RotateVideo": _vm.RotateVideo(); break;
                case "ShowTime":
                    ShowOsd(_vm.Engine.IsActive ? $"{TimeFormat.Format(_vm.Player.Time)} / {TimeFormat.Format(_vm.Player.Length)}" : "Stopped");
                    break;

                // Subtitles
                case "AddSubtitle": AddSubtitleFile(); break;
                case "CycleSubtitle": _vm.CycleSubtitleTrack(); break;
                case "SubDelayDown": _vm.ShiftSubtitleDelay(-50); break;
                case "SubDelayUp": _vm.ShiftSubtitleDelay(50); break;

                // Tools
                case "Effects": ShowSingleton(() => new EffectsWindow(_vm)); break;
                case "Sync": ShowSingleton(() => new SyncWindow(_vm)); break;
                case "MediaInfo": ShowSingleton(() => new MediaInfoWindow(_vm, codecTab: false), w => w.SelectTab(codec: false)); break;
                case "CodecInfo": ShowSingleton(() => new MediaInfoWindow(_vm, codecTab: true), w => w.SelectTab(codec: true)); break;
                case "Messages": ShowSingleton(() => new MessagesWindow(_vm.Engine.Log)); break;
                case "Preferences": ShowPreferences(); break;

                // View
                case "TogglePlaylist": SetPlaylistVisible(!_vm.IsPlaylistVisible); break;
                case "MinimalInterface": SetMinimal(!_minimal); break;
                case "ToggleStatusBar":
                    _vm.Settings.ShowStatusBar = !_vm.Settings.ShowStatusBar;
                    StatusBarPanel.Visibility = _vm.Settings.ShowStatusBar && !_minimal && !_vm.IsFullscreen ? Visibility.Visible : Visibility.Collapsed;
                    break;

                // Help
                case "Shortcuts": new ShortcutsWindow(_hotkeys) { Owner = this }.Show(); break;
                case "About": new AboutWindow(_vm.Engine.LibVLC) { Owner = this }.ShowDialog(); break;
            }
        }
        catch (Exception ex)
        {
            AppLog.Write(ex, "Action " + action);
            ShowError($"Something went wrong ({action}): {ex.Message}");
        }
    }

    /// <summary>Handles dynamic menu entries tagged "set:Kind:Value".</summary>
    private void RunSetAction(string tag)
    {
        var parts = tag.Split(':', 3);
        if (parts.Length < 3) return;
        var value = parts[2];
        int.TryParse(value, out var n);
        switch (parts[1])
        {
            case "Title": _vm.SelectTitle(n); break;
            case "Chapter": _vm.SelectChapter(n); break;
            case "Speed": _vm.SetRate(MainViewModel.Rates[n]); break;
            case "AudioTrack": _vm.SelectAudioTrack(n); break;
            case "AudioDevice": _vm.SelectAudioDevice(value); break;
            case "Stereo": _vm.SetStereoMode(MainViewModel.StereoModes[n].Channel); break;
            case "VideoTrack": _vm.SelectVideoTrack(n); break;
            case "Zoom": _vm.SetZoom(n); break;
            case "Aspect": _vm.SetAspect(n); break;
            case "Crop": _vm.SetCrop(n); break;
            case "Deinterlace": _vm.SetDeinterlace(n); break;
            case "SubTrack": _vm.SelectSubtitleTrack(n); break;
        }
    }

    // =====================================================================
    // Menus
    // =====================================================================

    private void Menu_Click(object sender, RoutedEventArgs e)
    {
        if (e.OriginalSource is MenuItem { Tag: string tag, HasItems: false } && !tag.StartsWith("dyn:", StringComparison.Ordinal))
        {
            e.Handled = true;
            // Let the menu close before running (dialogs, fullscreen).
            Dispatcher.BeginInvoke(() => RunAction(tag), DispatcherPriority.Input);
        }
    }

    private void Menu_SubmenuOpened(object sender, RoutedEventArgs e)
    {
        if (e.OriginalSource is not MenuItem menu) return;
        if (menu.Tag is string tag && tag.StartsWith("dyn:", StringComparison.Ordinal))
            FillDynamicMenu(menu, tag[4..]);
        RefreshChecks(menu.Items);
    }

    private void ApplyGestureText(ItemCollection items)
    {
        foreach (var obj in items)
        {
            if (obj is not MenuItem mi) continue;
            if (mi.Tag is string tag && !tag.Contains(':')) mi.InputGestureText = _hotkeys.GestureText(tag);
            ApplyGestureText(mi.Items);
        }
    }

    private void RefreshChecks(ItemCollection items)
    {
        foreach (var obj in items)
        {
            if (obj is not MenuItem { Tag: string tag } mi) continue;
            bool? check = tag switch
            {
                "Fullscreen" => _vm.IsFullscreen,
                "AlwaysOnTop" => Topmost,
                "TogglePlaylist" => _vm.IsPlaylistVisible,
                "MinimalInterface" => _minimal,
                "ToggleStatusBar" => _vm.Settings.ShowStatusBar,
                "Record" => _vm.IsRecording,
                "Mute" => _vm.IsMuted,
                _ => null,
            };
            if (check is { } c) { mi.IsCheckable = false; mi.IsChecked = c; }

            var needsMedia = tag is "Record" or "Snapshot" or "NextFrame" or "JumpToTime" or "SeekFwd10" or "SeekBack10"
                or "AddSubtitle" or "SubDelayDown" or "SubDelayUp" or "Stop";
            if (needsMedia) mi.IsEnabled = _vm.Engine.IsActive;
        }
    }

    private void FillDynamicMenu(MenuItem menu, string kind)
    {
        menu.Items.Clear();
        void Add(string header, string value, bool isChecked = false, bool enabled = true) =>
            menu.Items.Add(new MenuItem { Header = header, Tag = $"set:{kind}:{value}", IsChecked = isChecked, IsEnabled = enabled });
        void AddTracks(IReadOnlyList<TrackOption> tracks)
        {
            foreach (var t in tracks) Add(t.Name, t.Id.ToString(), t.IsSelected);
        }

        var active = _vm.Engine.IsActive;
        switch (kind)
        {
            case "Title": if (active) AddTracks(_vm.Titles()); break;
            case "Chapter": if (active) AddTracks(_vm.Chapters()); break;
            case "Speed":
                var faster = new MenuItem { Header = "_Faster", Tag = "Faster", InputGestureText = _hotkeys.GestureText("Faster") };
                var normal = new MenuItem { Header = "N_ormal Speed", Tag = "NormalSpeed", InputGestureText = _hotkeys.GestureText("NormalSpeed") };
                var slower = new MenuItem { Header = "_Slower", Tag = "Slower", InputGestureText = _hotkeys.GestureText("Slower") };
                menu.Items.Add(faster); menu.Items.Add(normal); menu.Items.Add(slower);
                menu.Items.Add(new Separator());
                for (var i = 0; i < MainViewModel.Rates.Length; i++)
                    Add($"{MainViewModel.Rates[i]:0.##}x", i.ToString(), Math.Abs(MainViewModel.Rates[i] - _vm.Rate) < 0.001f);
                break;
            case "AudioTrack": if (active) AddTracks(_vm.AudioTracks()); break;
            case "AudioDevice":
                foreach (var d in _vm.AudioDevices()) Add(EscapeAccessKey(d.Name), d.Id, d.Selected);
                break;
            case "Stereo":
                var channel = active ? _vm.Player.Channel : AudioOutputChannel.Stereo;
                for (var i = 0; i < MainViewModel.StereoModes.Length; i++)
                    Add(MainViewModel.StereoModes[i].Name, i.ToString(), MainViewModel.StereoModes[i].Channel == channel, active);
                break;
            case "VideoTrack": if (active) AddTracks(_vm.VideoTracks()); break;
            case "SubTrack":
                if (active) AddTracks(_vm.SubtitleTracks());
                break;
            case "Zoom":
                for (var i = 0; i < MainViewModel.Zooms.Length; i++) Add(MainViewModel.Zooms[i].Name, i.ToString(), i == _vm.ZoomIndex);
                break;
            case "Aspect":
                for (var i = 0; i < MainViewModel.AspectRatios.Length; i++)
                    Add(i == 0 ? "Default" : MainViewModel.AspectRatios[i], i.ToString(), i == _vm.AspectIndex);
                break;
            case "Crop":
                for (var i = 0; i < MainViewModel.CropRatios.Length; i++)
                    Add(i == 0 ? "Default" : MainViewModel.CropRatios[i], i.ToString(), i == _vm.CropIndex);
                break;
            case "Deinterlace":
                for (var i = 0; i < MainViewModel.DeinterlaceModes.Length; i++)
                    Add(MainViewModel.DeinterlaceModes[i].Name, i.ToString(), i == _vm.DeinterlaceIndex);
                break;
        }

        if (menu.Items.Count == 0) menu.Items.Add(new MenuItem { Header = "(empty)", IsEnabled = false });
    }

    private static string EscapeAccessKey(string s) => s.Replace("_", "__");

    /// <summary>Right-click menu on the video mirrors the main menu (rebuilt each time so state is fresh).</summary>
    private void Overlay_ContextMenuOpening(object sender, ContextMenuEventArgs e)
    {
        var cm = OverlayRoot.ContextMenu!;
        cm.Items.Clear();
        foreach (var top in new[] { "PlayPause", "Stop", "Previous", "Next", "Fullscreen" })
            cm.Items.Add(new MenuItem { Header = MenuHeaderFor(top), Tag = top, InputGestureText = _hotkeys.GestureText(top) });
        cm.Items.Add(new Separator());
        foreach (var obj in MainMenu.Items)
            if (obj is MenuItem mi) cm.Items.Add(CloneMenu(mi));
        cm.AddHandler(MenuItem.ClickEvent, new RoutedEventHandler(Menu_Click));
        cm.AddHandler(MenuItem.SubmenuOpenedEvent, new RoutedEventHandler(Menu_SubmenuOpened));
        RefreshChecks(cm.Items);
    }

    private static string MenuHeaderFor(string action) => action switch
    {
        "PlayPause" => "Play / Pause",
        "Fullscreen" => "Fullscreen",
        _ => action,
    };

    private static object CloneMenu(object source)
    {
        if (source is Separator) return new Separator();
        var mi = (MenuItem)source;
        var copy = new MenuItem { Header = mi.Header, Tag = mi.Tag, InputGestureText = mi.InputGestureText };
        foreach (var child in mi.Items) copy.Items.Add(CloneMenu(child));
        return copy;
    }

    // =====================================================================
    // Keyboard
    // =====================================================================

    private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        // Leave typing and list navigation alone.
        var focused = Keyboard.FocusedElement as DependencyObject;
        if (focused is TextBox or PasswordBox or ComboBox) return;
        // An open menu handles its own keys; a merely focused menu-bar header (after tapping Alt) does not block hotkeys.
        if (focused is MenuItem { IsSubmenuOpen: true } or MenuItem { Parent: MenuItem } || IsInside<ContextMenu>(focused)) return;
        if (focused is MenuItem && Keyboard.Modifiers == ModifierKeys.None) return; // plain keys navigate the menu bar
        var inList = IsInside<ListView>(focused);
        if (inList && e.Key is Key.Up or Key.Down or Key.Enter or Key.Delete or Key.Home or Key.End or Key.PageUp or Key.PageDown or Key.Space)
            return;

        var gesture = GestureFromKey(e);
        if (gesture is null) return;
        if (_hotkeys.Resolve(gesture) is { } action)
        {
            e.Handled = true;
            RunAction(action);
        }
    }

    private static bool IsInside<T>(DependencyObject? d) where T : DependencyObject
    {
        while (d is not null)
        {
            if (d is T) return true;
            d = d is System.Windows.Media.Visual ? System.Windows.Media.VisualTreeHelper.GetParent(d) : LogicalTreeHelper.GetParent(d);
        }
        return false;
    }

    /// <summary>Turns a key event into the gesture format used by <see cref="HotkeyMap"/>.</summary>
    public static string? GestureFromKey(KeyEventArgs e)
    {
        var key = e.Key switch { Key.System => e.SystemKey, Key.ImeProcessed => e.ImeProcessedKey, _ => e.Key };
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt or Key.LeftShift or Key.RightShift
            or Key.LWin or Key.RWin or Key.None or Key.DeadCharProcessed) return null;

        var mods = Keyboard.Modifiers;
        var prefix = "";
        if (mods.HasFlag(ModifierKeys.Control)) prefix += "Ctrl+";
        if (mods.HasFlag(ModifierKeys.Alt)) prefix += "Alt+";
        if (mods.HasFlag(ModifierKeys.Shift)) prefix += "Shift+";
        if (mods.HasFlag(ModifierKeys.Windows)) prefix += "Win+";
        return prefix + KeyName(key);
    }

    private static string KeyName(Key key) => key switch
    {
        Key.OemOpenBrackets => "[",
        Key.OemCloseBrackets => "]",
        Key.OemPlus => "=",
        Key.OemMinus => "-",
        Key.Add => "+",
        Key.Subtract => "-",
        Key.OemComma => ",",
        Key.OemPeriod => ".",
        Key.OemQuestion => "/",
        Key.OemSemicolon => ";",
        Key.OemQuotes => "'",
        Key.OemTilde => "`",
        Key.OemPipe => "\\",
        Key.Return => "Enter",
        Key.Prior => "PageUp",
        Key.Next => "PageDown",
        Key.Escape => "Escape",
        >= Key.D0 and <= Key.D9 => ((char)('0' + (key - Key.D0))).ToString(),
        >= Key.NumPad0 and <= Key.NumPad9 => "Num" + (key - Key.NumPad0),
        _ => key.ToString(),
    };

    private void OnMediaButton(string button)
    {
        switch (button)
        {
            case "Play": _vm.Play(); break;
            case "Pause": _vm.Pause(); break;
            case "Stop": _vm.Stop(); break;
            case "Next": _vm.Next(); break;
            case "Previous": _vm.Previous(); break;
        }
    }

    // =====================================================================
    // Opening media
    // =====================================================================

    private void OpenFiles(bool multiple)
    {
        var dlg = new OpenFileDialog
        {
            Title = multiple ? "Select one or more files to open" : "Select a file to open",
            Filter = MediaFiles.DialogFilter(),
            Multiselect = true,
            InitialDirectory = ValidFolder(_vm.Settings.LastFolder),
        };
        if (dlg.ShowDialog(this) != true) return;
        _vm.OpenLocations(dlg.FileNames, play: true);
    }

    private void OpenFolder()
    {
        var dlg = new OpenFolderDialog { Title = "Open folder", InitialDirectory = ValidFolder(_vm.Settings.LastFolder) };
        if (dlg.ShowDialog(this) != true) return;
        _vm.OpenLocations(new[] { dlg.FolderName }, play: true);
    }

    private static string ValidFolder(string? folder) =>
        folder is not null && Directory.Exists(folder) ? folder : Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);

    private void ShowOpenDialog(int tab)
    {
        var dlg = new OpenMediaWindow(_vm.Settings, tab) { Owner = this };
        if (dlg.ShowDialog() == true && dlg.Result.Count > 0)
            _vm.OpenItems(dlg.Result, play: !dlg.EnqueueOnly);
    }

    private void SavePlaylist()
    {
        if (_vm.Playlist.Items.Count == 0) { ShowOsd("Playlist is empty"); return; }
        var dlg = new SaveFileDialog
        {
            Title = "Save playlist",
            Filter = "M3U8 playlist|*.m3u8|M3U playlist|*.m3u|XSPF playlist|*.xspf",
            FileName = "playlist.m3u8",
            InitialDirectory = ValidFolder(_vm.Settings.LastFolder),
        };
        if (dlg.ShowDialog(this) != true) return;
        PlaylistParser.Save(dlg.FileName, _vm.Playlist.Items);
        ShowOsd("Playlist saved");
    }

    private void AddSubtitleFile()
    {
        if (!_vm.Engine.IsActive) { ShowOsd("Play something first"); return; }
        var dlg = new OpenFileDialog
        {
            Title = "Open subtitles",
            Filter = MediaFiles.SubtitleDialogFilter(),
            InitialDirectory = _vm.Playlist.Current is { IsFile: true } cur ? Path.GetDirectoryName(cur.Location) : ValidFolder(_vm.Settings.LastFolder),
        };
        if (dlg.ShowDialog(this) == true) _vm.AddSubtitle(dlg.FileName);
    }

    private void JumpToTime()
    {
        if (!_vm.Engine.IsActive) return;
        var dlg = new JumpToTimeWindow(_vm.Player.Time, _vm.Player.Length) { Owner = this };
        if (dlg.ShowDialog() == true) _vm.Seek(dlg.TargetMs);
    }

    private void ShowPreferences()
    {
        var dlg = new PreferencesWindow(_vm.SettingsService) { Owner = this };
        if (dlg.ShowDialog() != true) return;
        _hotkeys = new HotkeyMap(_vm.Settings.Hotkeys);
        ApplyGestureText(MainMenu.Items);
        App.ApplyTheme(_vm.Settings.Theme);
        _vm.SaveState();
        if (dlg.RestartRequired) ShowOsd("Some changes apply after restarting Reel Player");
    }

    private readonly Dictionary<Type, Window> _singletons = new();

    private void ShowSingleton<T>(Func<T> create, Action<T>? onExisting = null) where T : Window
    {
        if (_singletons.TryGetValue(typeof(T), out var existing) && existing.IsLoaded)
        {
            onExisting?.Invoke((T)existing);
            existing.Activate();
            return;
        }
        var w = create();
        w.Owner = this;
        _singletons[typeof(T)] = w;
        w.Show();
    }

    // =====================================================================
    // Drag & drop
    // =====================================================================

    private void Window_DragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop) || e.Data.GetDataPresent(DataFormats.UnicodeText)
            ? DragDropEffects.Copy : DragDropEffects.None;
        e.Handled = true;
    }

    private void Window_Drop(object sender, DragEventArgs e)
    {
        e.Handled = true;
        if (e.Data.GetData(DataFormats.FileDrop) is string[] files && files.Length > 0)
            _vm.OpenLocations(files, play: true);
        else if (e.Data.GetData(DataFormats.UnicodeText) is string text && text.Contains("://", StringComparison.Ordinal))
            _vm.OpenLocations(text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries), play: true);
        Activate();
    }

    // =====================================================================
    // Fullscreen, minimal mode, playlist
    // =====================================================================

    private void SetFullscreen(bool on)
    {
        if (on == _vm.IsFullscreen) return;
        _vm.IsFullscreen = on;
        if (on)
        {
            _restoreState = WindowState;
            // When maximized, Left/Top/Width/Height describe the maximized frame; RestoreBounds is the normal size.
            _restoreBounds = WindowState == WindowState.Maximized && !RestoreBounds.IsEmpty
                ? RestoreBounds
                : new Rect(Left, Top, Width, Height);
            MainMenu.Visibility = Visibility.Collapsed;
            StatusBarPanel.Visibility = Visibility.Collapsed;
            DockedControls.Visibility = Visibility.Collapsed;
            PlaylistColumn.Width = new GridLength(0);
            if (_playlistView is not null) _playlistView.Visibility = Visibility.Collapsed;
            PlaylistSplitter.Visibility = Visibility.Collapsed;
            EnsureFullscreenControls();
            WindowStyle = WindowStyle.None;
            ResizeMode = ResizeMode.NoResize;
            if (WindowState == WindowState.Maximized)
            {
                // Stay maximized and expand directly to the full monitor (avoids shrinking to Normal first).
                var hwnd = new System.Windows.Interop.WindowInteropHelper(this).Handle;
                if (hwnd != IntPtr.Zero) NativeMethods.SetFullscreenBounds(hwnd);
            }
            else
            {
                WindowState = WindowState.Maximized;
            }
            ShowFullscreenControls();
        }
        else
        {
            FullscreenControlsHost.Visibility = Visibility.Collapsed;
            _hideControlsTimer.Stop();
            OverlayRoot.Cursor = Cursors.Arrow;
            ApplyChromeVisibility();
            SetPlaylistVisible(_vm.IsPlaylistVisible);
            WindowStyle = WindowStyle.SingleBorderWindow;
            ResizeMode = ResizeMode.CanResize;
            if (_restoreState == WindowState.Maximized)
            {
                // Stay maximized and reapply work-area bounds directly (no Normal → Maximized shrink/grow).
                var hwnd = new System.Windows.Interop.WindowInteropHelper(this).Handle;
                if (hwnd != IntPtr.Zero) NativeMethods.RefreshMaximizedBounds(hwnd);
            }
            else
            {
                WindowState = WindowState.Normal;
                Left = _restoreBounds.Left; Top = _restoreBounds.Top; Width = _restoreBounds.Width; Height = _restoreBounds.Height;
            }
        }
    }

    private void SetMinimal(bool on)
    {
        _minimal = on;
        ApplyChromeVisibility();
        ShowOsd(on ? $"Minimal interface ({_hotkeys.GestureText("MinimalInterface")} to restore)" : "Normal interface");
    }

    private void ApplyChromeVisibility()
    {
        if (_vm.IsFullscreen) return;
        MainMenu.Visibility = _minimal ? Visibility.Collapsed : Visibility.Visible;
        StatusBarPanel.Visibility = !_minimal && _vm.Settings.ShowStatusBar ? Visibility.Visible : Visibility.Collapsed;
        DockedControls.Visibility = Visibility.Visible;
    }

    private void EnsureFullscreenControls()
    {
        if (_fullscreenControls is not null) return;
        _fullscreenControls = new Controls.ControlBar
        {
            Foreground = (System.Windows.Media.Brush)FindResource("OverlayTextBrush"),
        };
        _fullscreenControls.ActionRequested += RunAction;
        FullscreenControlsHost.Child = _fullscreenControls;
    }

    private PlaylistPanel EnsurePlaylistView()
    {
        if (_playlistView is not null) return _playlistView;
        _playlistView = new PlaylistPanel();
        Grid.SetColumn(_playlistView, 2);
        _playlistView.Attach(_vm.Playlist);
        _playlistView.ItemActivated += item => _vm.PlayItem(item);
        _playlistView.ActionRequested += RunAction;
        _playlistView.FilesDropped += files => _vm.OpenLocations(files, play: false);
        ContentGrid.Children.Add(_playlistView);
        return _playlistView;
    }

    private void SetPlaylistVisible(bool visible)
    {
        _vm.IsPlaylistVisible = visible;
        _vm.Settings.ShowPlaylist = visible;
        if (_vm.IsFullscreen) return;
        if (visible)
        {
            var panel = EnsurePlaylistView();
            PlaylistColumn.Width = new GridLength(Math.Clamp(_vm.Settings.PlaylistWidth, 200, 900));
            panel.Visibility = PlaylistSplitter.Visibility = Visibility.Visible;
            panel.FocusList();
        }
        else
        {
            if (PlaylistColumn.ActualWidth > 0) _vm.Settings.PlaylistWidth = PlaylistColumn.ActualWidth;
            PlaylistColumn.Width = new GridLength(0);
            if (_playlistView is not null) _playlistView.Visibility = Visibility.Collapsed;
            PlaylistSplitter.Visibility = Visibility.Collapsed;
        }
    }

    private void Overlay_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.OriginalSource is not Grid) return; // clicks on buttons/bars inside the overlay
        if (e.ClickCount == 2)
        {
            RunAction("Fullscreen");
            e.Handled = true;
        }
    }

    private void Overlay_MouseMove(object sender, MouseEventArgs e)
    {
        var p = e.GetPosition(OverlayRoot);
        if (Math.Abs(p.X - _lastMouse.X) < 2 && Math.Abs(p.Y - _lastMouse.Y) < 2) return;
        _lastMouse = p;
        if (_vm.IsFullscreen) ShowFullscreenControls();
    }

    private void ShowFullscreenControls()
    {
        FullscreenControlsHost.Visibility = Visibility.Visible;
        OverlayRoot.Cursor = Cursors.Arrow;
        _hideControlsTimer.Stop();
        _hideControlsTimer.Start();
    }

    private void HideFullscreenControls()
    {
        _hideControlsTimer.Stop();
        if (!_vm.IsFullscreen) return;
        // Keep the bar while the pointer is on it.
        if (FullscreenControlsHost.IsMouseOver) { _hideControlsTimer.Start(); return; }
        FullscreenControlsHost.Visibility = Visibility.Collapsed;
        OverlayRoot.Cursor = Cursors.None;
    }

    // =====================================================================
    // OSD, errors, resume
    // =====================================================================

    private void ShowOsd(string text)
    {
        OsdText.Text = text;
        var anim = new DoubleAnimationUsingKeyFrames();
        anim.KeyFrames.Add(new DiscreteDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        anim.KeyFrames.Add(new DiscreteDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(1200))));
        anim.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(1600))));
        OsdText.BeginAnimation(OpacityProperty, anim);
    }

    private void ShowError(string message)
    {
        ErrorText.Text = message;
        ErrorBanner.Visibility = Visibility.Visible;
        _errorTimer.Stop();
        _errorTimer.Start();
    }

    private void ErrorClose_Click(object sender, RoutedEventArgs e) => ErrorBanner.Visibility = Visibility.Collapsed;

    private void OfferResume(MediaItem item, long position)
    {
        _resumeOffer = (item, position);
        ResumeText.Text = $"Continue playback from {TimeFormat.Format(position)}?";
        ResumeBar.Visibility = Visibility.Visible;
        _resumeTimer.Stop();
        _resumeTimer.Start();
    }

    private void ResumeContinue_Click(object sender, RoutedEventArgs e)
    {
        if (_resumeOffer is { } offer && ReferenceEquals(offer.Item, _vm.Playlist.Current))
            _vm.Seek(offer.Position);
        DismissResume();
    }

    private void ResumeDismiss_Click(object sender, RoutedEventArgs e) => DismissResume();

    private void DismissResume()
    {
        _resumeTimer.Stop();
        _resumeOffer = null;
        ResumeBar.Visibility = Visibility.Collapsed;
    }

    // =====================================================================
    // Title, taskbar
    // =====================================================================

    private void Vm_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(MainViewModel.Title):
            case nameof(MainViewModel.IsIdle):
                UpdateTitle();
                break;
            case nameof(MainViewModel.IsPlaying):
            case nameof(MainViewModel.IsPaused):
                ThumbPlayButton.ImageSource = (System.Windows.Media.ImageSource)FindResource(_vm.IsPlaying ? "ThumbPause" : "ThumbPlay");
                ThumbPlayButton.Description = _vm.IsPlaying ? "Pause" : "Play";
                UpdateTaskbarProgress();
                break;
            case nameof(MainViewModel.PositionMs):
                UpdateTaskbarProgress();
                break;
        }
    }

    private void UpdateTitle() =>
        Title = _vm.IsIdle ? "Reel Player" : $"{_vm.Title} - Reel Player";

    private void UpdateTaskbarProgress()
    {
        if (!_vm.Engine.IsActive || _vm.LengthMs <= 0)
        {
            Taskbar.ProgressState = TaskbarItemProgressState.None;
            return;
        }
        Taskbar.ProgressState = _vm.IsPaused ? TaskbarItemProgressState.Paused : TaskbarItemProgressState.Normal;
        Taskbar.ProgressValue = Math.Clamp((double)_vm.PositionMs / _vm.LengthMs, 0, 1);
    }

    private void ThumbPrev_Click(object? sender, EventArgs e) => _vm.Previous();
    private void ThumbPlay_Click(object? sender, EventArgs e) => _vm.TogglePlayPause();
    private void ThumbNext_Click(object? sender, EventArgs e) => _vm.Next();

    // =====================================================================
    // Window placement & shutdown
    // =====================================================================

    private void RestoreWindowPlacement()
    {
        var s = _vm.Settings;
        var work = SystemParameters.WorkArea;
        Width = Math.Clamp(s.WindowWidth, MinWidth, Math.Max(MinWidth, work.Width));
        Height = Math.Clamp(s.WindowHeight, MinHeight, Math.Max(MinHeight, work.Height));
        if (s.WindowLeft is { } left && s.WindowTop is { } top &&
            left < SystemParameters.VirtualScreenLeft + SystemParameters.VirtualScreenWidth - 100 &&
            top < SystemParameters.VirtualScreenTop + SystemParameters.VirtualScreenHeight - 100 &&
            left + Width > SystemParameters.VirtualScreenLeft + 100 && top > SystemParameters.VirtualScreenTop - 20)
        {
            WindowStartupLocation = WindowStartupLocation.Manual;
            // Keep the whole frame on the primary work area if it would otherwise hang off it.
            Left = left + Width > work.Right && work.Contains(new Point(left, top)) ? Math.Max(work.Left, work.Right - Width) : left;
            Top = top + Height > work.Bottom && work.Contains(new Point(left, top)) ? Math.Max(work.Top, work.Bottom - Height) : top;
        }
        else
        {
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
        }
        if (s.WindowMaximized) WindowState = WindowState.Maximized;
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        var s = _vm.Settings;
        var bounds = _vm.IsFullscreen ? _restoreBounds : RestoreBounds;
        if (!bounds.IsEmpty)
        {
            s.WindowLeft = bounds.Left; s.WindowTop = bounds.Top;
            s.WindowWidth = bounds.Width; s.WindowHeight = bounds.Height;
        }
        s.WindowMaximized = _vm.IsFullscreen ? _restoreState == WindowState.Maximized : WindowState == WindowState.Maximized;
        if (_vm.IsPlaylistVisible && PlaylistColumn.ActualWidth > 0) s.PlaylistWidth = PlaylistColumn.ActualWidth;
        _vm.SaveState();
        foreach (var w in _singletons.Values) if (w.IsLoaded) w.Close();
    }

    /// <summary>Called for files passed on the command line or forwarded from a second instance.</summary>
    public void HandleCommandLine(CommandLine cmd, bool fromOtherInstance)
    {
        if (cmd.Locations.Count > 0)
        {
            var play = !fromOtherInstance || !_vm.Settings.EnqueueWhenRunning;
            _vm.OpenLocations(cmd.Locations, play, cmd.StartTimeMs);
        }
        if (cmd.Fullscreen && !_vm.IsFullscreen) SetFullscreen(true);
        if (fromOtherInstance)
        {
            if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
            Activate();
            Topmost = true;
            Topmost = _vm.Settings.AlwaysOnTop;
        }
    }
}
