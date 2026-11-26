using System.Diagnostics;
using System.Reflection;
using System.Windows;
using System.Windows.Navigation;
using LibVLCSharp.Shared;

namespace ReelPlayer.App.Views;

public partial class AboutWindow : Window
{
    public AboutWindow(LibVLC libVlc)
    {
        InitializeComponent();
        var version = Assembly.GetExecutingAssembly().GetName().Version;
        VersionText.Text = $"Version {version?.ToString(3) ?? "1.0.0"} · {(Environment.Is64BitProcess ? "64-bit" : "32-bit")}";
        LibVlcText.Text = $"libVLC {libVlc.Version}";
    }

    private void Link_RequestNavigate(object sender, RequestNavigateEventArgs e)
    {
        Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) { UseShellExecute = true });
        e.Handled = true;
    }
}
