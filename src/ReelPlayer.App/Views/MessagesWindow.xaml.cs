using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using LibVLCSharp.Shared;
using Microsoft.Win32;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.Views;

public partial class MessagesWindow : Window
{
    private readonly LogBuffer _log;
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private int _pending;
    private LogLevel _minLevel = LogLevel.Notice;

    public MessagesWindow(LogBuffer log)
    {
        _log = log;
        InitializeComponent();
        LevelBox.SelectedIndex = 2;
        // libVLC logs from many threads and can be chatty: batch UI refreshes.
        _log.Added += OnAdded;
        _timer.Tick += (_, _) => { if (Interlocked.Exchange(ref _pending, 0) > 0) Reload(); };
        _timer.Start();
        Closed += (_, _) => { _log.Added -= OnAdded; _timer.Stop(); };
    }

    private void OnAdded(LogEntry entry) => Interlocked.Increment(ref _pending);

    private void Reload()
    {
        var atEnd = LogBox.VerticalOffset + LogBox.ViewportHeight >= LogBox.ExtentHeight - 4;
        var sb = new StringBuilder();
        foreach (var e in _log.Snapshot().Where(e => e.Level >= _minLevel).TakeLast(3000))
            sb.Append(e.Time.ToString("HH:mm:ss.fff")).Append(' ')
              .Append(LevelTag(e.Level)).Append(' ')
              .Append(e.Module).Append(": ").Append(e.Message).Append('\n');
        LogBox.Text = sb.ToString();
        if (atEnd) LogBox.ScrollToEnd();
    }

    private static string LevelTag(LogLevel l) => l switch
    {
        LogLevel.Error => "error",
        LogLevel.Warning => "warn ",
        LogLevel.Notice => "info ",
        _ => "debug",
    };

    private void LevelBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        _minLevel = LevelBox.SelectedIndex switch
        {
            0 => LogLevel.Error,
            1 => LogLevel.Warning,
            2 => LogLevel.Notice,
            _ => LogLevel.Debug,
        };
        Reload();
        LogBox.ScrollToEnd();
    }

    private void Clear_Click(object sender, RoutedEventArgs e)
    {
        _log.Clear();
        Reload();
    }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new SaveFileDialog { Filter = "Text files|*.txt", FileName = $"reelplayer-messages-{DateTime.Now:yyyyMMdd-HHmmss}.txt" };
        if (dlg.ShowDialog(this) == true) File.WriteAllText(dlg.FileName, LogBox.Text);
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Close();
}
