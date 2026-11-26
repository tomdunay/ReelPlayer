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
    private const string AppId = "ReelPlayer";

    private SingleInstance? _instance;
    private PlayerService? _engine;
    private MediaControls? _smtc;
    private MainViewModel? _vm;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += OnDispatcherUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += (_, ex) =>
            AppLog.Write(ex.ExceptionObject as Exception ?? new Exception("Unknown error"), "Unhandled");
        TaskScheduler.UnobservedTaskException += (_, ex) => { AppLog.Write(ex.Exception, "Task"); ex.SetObserved(); };
        AppLog.Prune();

        var settings = new SettingsService();
        settings.Load();
        var cmd = CommandLine.Parse(e.Args);

        if (settings.Current.OneInstance)
        {
            _instance = new SingleInstance(AppId);
            if (!_instance.IsFirstInstance)
            {
                if (_instance.Forward(e.Args)) { Shutdown(); return; }
                // First instance not answering (hung or closing): carry on as a normal instance.
            }
            else
            {
                _instance.StartListening();
            }
        }

        ApplyTheme(settings.Current.Theme);

        try
        {
            LibVLCSharp.Shared.Core.Initialize();
            _engine = new PlayerService(PlayerService.BuildOptions(settings.Current));
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
        _vm = new MainViewModel(settings, _engine, _smtc);
        var window = new MainWindow(_vm);
        MainWindow = window;
        window.Show();
        window.HandleCommandLine(cmd, fromOtherInstance: false);

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
