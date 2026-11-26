using System.IO;
using System.Runtime.InteropServices;
using ReelPlayer.App.Services;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App;

public static class Program
{
    private const string AppId = "ReelPlayer";

    [STAThread]
    public static int Main(string[] args)
    {
        var settings = new SettingsService();
        settings.Load();
        var cmd = CommandLine.Parse(args);

        SingleInstance? instance = null;
        if (settings.Current.OneInstance)
        {
            instance = new SingleInstance(AppId);
            if (!instance.IsFirstInstance)
            {
                if (instance.Forward(args))
                {
                    instance.Dispose();
                    return 0;
                }
                // First instance not answering (hung or closing): carry on as a normal instance.
            }
            else
            {
                instance.StartListening();
            }
        }

        // Start libVLC, resume store, and log pruning in parallel before WPF initializes.
        var libVlcOptions = PlayerService.BuildOptions(settings.Current);
        var engineTask = Task.Run(() =>
        {
            InitializeLibVlc();
            return new PlayerService(libVlcOptions);
        });
        var resumeTask = Task.Run(() => new ResumeStore());
        _ = Task.Run(AppLog.Prune);

        var app = new App(settings, cmd, instance, engineTask, resumeTask);
        app.InitializeComponent();
        return app.Run();
    }

    private static void InitializeLibVlc()
    {
        var arch = RuntimeInformation.ProcessArchitecture switch
        {
            Architecture.Arm64 => "win-arm64",
            Architecture.X86 => "win-x86",
            _ => "win-x64",
        };
        var dir = Path.Combine(AppContext.BaseDirectory, "libvlc", arch);
        if (Directory.Exists(dir))
            LibVLCSharp.Shared.Core.Initialize(dir);
        else
            LibVLCSharp.Shared.Core.Initialize();
    }
}
