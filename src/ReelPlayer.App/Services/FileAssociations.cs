using System.Diagnostics;
using Microsoft.Win32;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.Services;

/// <summary>
/// Per-user (HKCU, no admin) registration so Reel Player appears in "Open with" and Default Apps.
/// Windows does not let apps silently become the default; the user confirms in Settings.
/// </summary>
public static class FileAssociations
{
    private const string ProgId = "ReelPlayer.Media";
    private const string AppKeyName = "ReelPlayer";

    public static void Register()
    {
        var exe = Environment.ProcessPath ?? throw new InvalidOperationException("Unknown executable path.");
        var open = $"\"{exe}\" \"%1\"";
        var extensions = MediaFiles.Video.Concat(MediaFiles.Audio).Concat(MediaFiles.Playlists).Distinct().ToList();

        using (var prog = Registry.CurrentUser.CreateSubKey($@"Software\Classes\{ProgId}"))
        {
            prog.SetValue("", "Reel Player media file");
            using (var icon = prog.CreateSubKey("DefaultIcon")) icon.SetValue("", $"\"{exe}\",0");
            using (var cmd = prog.CreateSubKey(@"shell\open\command")) cmd.SetValue("", open);
        }

        // Applications\ReelPlayer.exe makes it show up in "Open with".
        using (var app = Registry.CurrentUser.CreateSubKey($@"Software\Classes\Applications\{Path.GetFileName(exe)}"))
        {
            app.SetValue("FriendlyAppName", "Reel Player");
            using (var cmd = app.CreateSubKey(@"shell\open\command")) cmd.SetValue("", open);
            using var types = app.CreateSubKey("SupportedTypes");
            foreach (var ext in extensions) types.SetValue(ext, "");
        }

        // Capabilities + RegisteredApplications make it show up in Settings > Default apps.
        var capPath = $@"Software\{AppKeyName}\Capabilities";
        using (var caps = Registry.CurrentUser.CreateSubKey(capPath))
        {
            caps.SetValue("ApplicationName", "Reel Player");
            caps.SetValue("ApplicationDescription", "Plays almost any video or audio file.");
            using var assoc = caps.CreateSubKey("FileAssociations");
            foreach (var ext in extensions) assoc.SetValue(ext, ProgId);
        }
        using (var reg = Registry.CurrentUser.CreateSubKey(@"Software\RegisteredApplications"))
            reg.SetValue(AppKeyName, capPath);

        foreach (var ext in extensions)
        {
            using var key = Registry.CurrentUser.CreateSubKey($@"Software\Classes\{ext}\OpenWithProgids");
            key.SetValue(ProgId, Array.Empty<byte>(), RegistryValueKind.None);
        }

        NativeMethods.SHChangeNotify(NativeMethods.SHCNE_ASSOCCHANGED, 0, IntPtr.Zero, IntPtr.Zero);
    }

    public static void Unregister()
    {
        Registry.CurrentUser.DeleteSubKeyTree($@"Software\Classes\{ProgId}", false);
        Registry.CurrentUser.DeleteSubKeyTree($@"Software\Classes\Applications\{Path.GetFileName(Environment.ProcessPath ?? "ReelPlayer.exe")}", false);
        Registry.CurrentUser.DeleteSubKeyTree($@"Software\{AppKeyName}", false);
        using (var reg = Registry.CurrentUser.OpenSubKey(@"Software\RegisteredApplications", writable: true))
            reg?.DeleteValue(AppKeyName, false);
        foreach (var ext in MediaFiles.Video.Concat(MediaFiles.Audio).Concat(MediaFiles.Playlists).Distinct())
        {
            using var key = Registry.CurrentUser.OpenSubKey($@"Software\Classes\{ext}\OpenWithProgids", writable: true);
            key?.DeleteValue(ProgId, false);
        }
        NativeMethods.SHChangeNotify(NativeMethods.SHCNE_ASSOCCHANGED, 0, IntPtr.Zero, IntPtr.Zero);
    }

    public static void OpenDefaultAppsSettings()
    {
        try
        {
            Process.Start(new ProcessStartInfo("ms-settings:defaultapps") { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            AppLog.Write(ex, "Open default apps");
        }
    }
}
