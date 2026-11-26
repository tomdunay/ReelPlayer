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

    private const uint MONITOR_DEFAULTTONEAREST = 2;
    private const uint SWP_NOZORDER = 0x0004;
    private const uint SWP_NOACTIVATE = 0x0010;
    private const uint SWP_FRAMECHANGED = 0x0020;
    private const int SM_CXSIZEFRAME = 32;
    private const int SM_CXPADDEDBORDER = 92;

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorInfo
    {
        public int Size;
        public Rect Monitor;
        public Rect Work;
        public uint Flags;
    }

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);

    [DllImport("user32.dll")]
    private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hwnd, IntPtr insertAfter, int x, int y, int cx, int cy, uint flags);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern int GetSystemMetricsForDpi(int index, uint dpi);

    /// <summary>Expands a borderless maximized window to cover the full monitor (including the taskbar) without unmaximizing.</summary>
    public static void SetFullscreenBounds(IntPtr hwnd)
    {
        if (!TryGetMonitorInfo(hwnd, out var info)) return;
        var r = info.Monitor;
        SetWindowPos(hwnd, IntPtr.Zero, r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top, SWP_FRAMECHANGED | SWP_NOZORDER);
    }

    /// <summary>Reapplies normal maximized bounds (respecting the taskbar) after restoring window chrome, without unmaximizing.</summary>
    public static void RefreshMaximizedBounds(IntPtr hwnd)
    {
        if (!TryGetMonitorInfo(hwnd, out var info)) return;
        var dpi = GetDpiForWindow(hwnd);
        var border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        var r = info.Work;
        SetWindowPos(hwnd, IntPtr.Zero,
            r.Left - border, r.Top - border,
            (r.Right - r.Left) + border * 2, (r.Bottom - r.Top) + border * 2,
            SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    private static bool TryGetMonitorInfo(IntPtr hwnd, out MonitorInfo info)
    {
        info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
        var monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        return monitor != IntPtr.Zero && GetMonitorInfo(monitor, ref info);
    }

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
