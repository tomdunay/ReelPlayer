using System.Windows;
using System.Windows.Threading;
using Microsoft.Win32;
using ReelPlayer.App.Services;
using ReelPlayer.App.ViewModels;
using ReelPlayer.App.Views;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App;

public partial class App : Application
{
    private readonly SettingsService _settings;
    private readonly CommandLine _cmd;
    private readonly SingleInstance? _instance;
    private readonly Task<PlayerService> _engineTask;
    private readonly Task<ResumeStore> _resumeTask;
    private PlayerService? _engine;
    private MediaControls? _smtc;
    private MainViewModel? _vm;

    public App(SettingsService settings, CommandLine cmd, SingleInstance? instance,
               Task<PlayerService> engineTask, Task<ResumeStore> resumeTask)
    {
        _settings = settings;
        _cmd = cmd;
        _instance = instance;
        _engineTask = engineTask;
        _resumeTask = resumeTask;
    }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += OnDispatcherUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += (_, ex) =>
            AppLog.Write(ex.ExceptionObject as Exception ?? new Exception("Unknown error"), "Unhandled");
        TaskScheduler.UnobservedTaskException += (_, ex) => { AppLog.Write(ex.Exception, "Task"); ex.SetObserved(); };

        ApplyTheme(_settings.Current.Theme);

        try
        {
            _engine = _engineTask.GetAwaiter().GetResult();
        }
        catch (Exception ex)
        {
            AppLog.Write(ex, "libVLC init");
            MessageBox.Show(
                "Reel Player could not start its playback engine (libVLC).\n\n" + ex.Message +
                "\n\nMake sure the 'libvlc' folder is next to ReelPlayer.exe.",
                "Reel Player", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown(1);
            return;
        }

        _smtc = new MediaControls();
        _vm = new MainViewModel(_settings, _engine, _smtc, _resumeTask.GetAwaiter().GetResult());
        var window = new MainWindow(_vm);
        MainWindow = window;
        window.Show();
        window.HandleCommandLine(_cmd, fromOtherInstance: false);

        if (_instance is { IsFirstInstance: true })
        {
            _instance.ArgumentsReceived += args => Dispatcher.BeginInvoke(() =>
                window.HandleCommandLine(CommandLine.Parse(args), fromOtherInstance: true));
        }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        try
        {
            _vm?.Dispose();
            _smtc?.Dispose();
            _engine?.Dispose();
        }
        catch (Exception ex)
        {
            AppLog.Write(ex, "Shutdown");
        }
        _instance?.Dispose();
        base.OnExit(e);
    }

    /// <summary>Applies "Dark", "Light" or "System" using WPF's built-in Fluent theme.</summary>
    public static void ApplyTheme(string theme)
    {
        var mode = theme switch
        {
            "Light" => ThemeMode.Light,
            "System" => SystemUsesLightTheme() ? ThemeMode.Light : ThemeMode.Dark,
            _ => ThemeMode.Dark,
        };
        if (Current is not null) Current.ThemeMode = mode;
    }

    private static bool SystemUsesLightTheme()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            return key?.GetValue("AppsUseLightTheme") is int v && v != 0;
        }
        catch
        {
            return false;
        }
    }

    private void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        AppLog.Write(e.Exception, "UI");
        e.Handled = true; // keep playing; the error is in the log
    }
}
