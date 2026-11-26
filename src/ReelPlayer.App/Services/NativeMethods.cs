using System.Runtime.InteropServices;

namespace ReelPlayer.App.Services;

internal static class NativeMethods
{
    [Flags]
    private enum ExecutionState : uint
    {
        SystemRequired = 0x00000001,
        DisplayRequired = 0x00000002,
        Continuous = 0x80000000,
    }

    [DllImport("kernel32.dll")]
    private static extern uint SetThreadExecutionState(ExecutionState flags);

    /// <summary>Keeps the display and system awake while video plays. Must be called from one thread (UI).</summary>
    public static void KeepAwake(bool on) =>
        SetThreadExecutionState(on
            ? ExecutionState.Continuous | ExecutionState.SystemRequired | ExecutionState.DisplayRequired
            : ExecutionState.Continuous);

    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(int wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);

    public const int SHCNE_ASSOCCHANGED = 0x08000000;

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

    [DllImport("dwmapi.dll")]
    private static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref Margins margins);

    [StructLayout(LayoutKind.Sequential)]
    private struct Margins { public int Left, Right, Top, Bottom; }

    private const int DWMWA_SYSTEMBACKDROP_TYPE = 38;
    private const int DWMSBT_NONE = 1;

    /// <summary>
    /// Turns off the Mica backdrop WPF's Fluent theme puts behind the window. Under a backdrop DWM treats GDI-black
    /// pixels of child windows as transparent, which made libVLC's black letterbox show the backdrop's grey.
    /// </summary>
    public static void DisableBackdrop(IntPtr hwnd)
    {
        var none = DWMSBT_NONE;
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref none, sizeof(int));
        var margins = new Margins();
        DwmExtendFrameIntoClientArea(hwnd, ref margins);
    }

    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hwnd, System.Text.StringBuilder name, int max);

    [DllImport("user32.dll", EntryPoint = "SetClassLongPtrW")]
    private static extern IntPtr SetClassLongPtr(IntPtr hwnd, int index, IntPtr value);

    [DllImport("gdi32.dll")]
    private static extern IntPtr GetStockObject(int obj);

    [DllImport("user32.dll")]
    private static extern bool InvalidateRect(IntPtr hwnd, IntPtr rect, bool erase);

    private const int GCLP_HBRBACKGROUND = -10;
    private const int BLACK_BRUSH = 4;

    /// <summary>
    /// libVLC's "VLC video main" container window fills the video area but only the picture is drawn, and its class
    /// has no background brush, so letterbox bars show whatever is behind it. Give that (libVLC-private) class a
    /// black brush. Returns how many windows were updated.
    /// </summary>
    public static int BlackenVlcWindows(IntPtr topLevel)
    {
        var count = 0;
        EnumChildWindows(topLevel, (hwnd, _) =>
        {
            var name = new System.Text.StringBuilder(256);
            GetClassName(hwnd, name, name.Capacity);
            if (name.ToString().StartsWith("VLC video main", StringComparison.Ordinal))
            {
                SetClassLongPtr(hwnd, GCLP_HBRBACKGROUND, GetStockObject(BLACK_BRUSH));
                InvalidateRect(hwnd, IntPtr.Zero, true);
                count++;
            }
            return true;
        }, IntPtr.Zero);
        return count;
    }
}
