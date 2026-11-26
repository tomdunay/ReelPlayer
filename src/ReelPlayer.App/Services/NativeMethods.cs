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
}
