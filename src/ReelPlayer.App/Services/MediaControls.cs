using System.Runtime.InteropServices;

namespace ReelPlayer.App.Services;

/// <summary>
/// System Media Transport Controls (Windows volume flyout, lock screen, hardware media keys) for the main window.
/// This talks to the few WinRT interfaces it needs directly through their COM vtables, instead of referencing the
/// ~50 MB Windows SDK projection. IIDs and slot numbers come from Windows.Media.winmd (slots 0-5 are
/// IUnknown + IInspectable).
/// </summary>
public sealed unsafe class MediaControls : IDisposable
{
    private static readonly Guid IID_Interop = new("ddb0472d-c911-4a1f-86d9-dc3d71a95f5a");
    private static readonly Guid IID_Smtc = new("99fa3ff4-1742-42a6-902e-087d41f965ec");
    private static readonly Guid IID_Smtc2 = new("ea98d2f6-7f3c-4af2-a586-72889808efb1");
    private static readonly Guid IID_Timeline = new("5125316a-c3a2-475b-8507-93534dc88f15");
    private static readonly Guid IID_StreamRefStatics = new("857309dc-3fbf-4e7d-986f-ef3b1a07a964");
    private static readonly Guid IID_UriFactory = new("44a9796f-723e-4fdf-a218-033e75b0c084");

    // ISystemMediaTransportControls
    private const int SlotPutPlaybackStatus = 7, SlotGetDisplayUpdater = 8, SlotPutIsEnabled = 11, SlotPutIsPlayEnabled = 13,
        SlotPutIsStopEnabled = 15, SlotPutIsPauseEnabled = 17, SlotPutIsPreviousEnabled = 25, SlotPutIsNextEnabled = 27,
        SlotAddButtonPressed = 32, SlotRemoveButtonPressed = 33;
    // ISystemMediaTransportControls2
    private const int SlotUpdateTimeline = 12;
    // ISystemMediaTransportControlsDisplayUpdater
    private const int SlotPutType = 7, SlotPutThumbnail = 11, SlotGetMusicProps = 12, SlotGetVideoProps = 13, SlotClearAll = 16, SlotUpdate = 17;
    // IMusicDisplayProperties / IVideoDisplayProperties
    private const int SlotPutTitle = 7, SlotPutMusicArtist = 11, SlotPutVideoSubtitle = 9;
    // ISystemMediaTransportControlsTimelineProperties: put_StartTime, put_EndTime, put_MinSeekTime, put_MaxSeekTime, put_Position
    private const int SlotPutStart = 7, SlotPutEnd = 9, SlotPutMinSeek = 11, SlotPutMaxSeek = 13, SlotPutPosition = 15;

    private const int StatusClosed = 0, StatusStopped = 2, StatusPlaying = 3, StatusPaused = 4;
    private const int TypeMusic = 1, TypeVideo = 2;

    private IntPtr _smtc;     // ISystemMediaTransportControls*
    private IntPtr _smtc2;    // ISystemMediaTransportControls2*
    private IntPtr _updater;  // ISystemMediaTransportControlsDisplayUpdater*
    private IntPtr _handler;  // CCW for the ButtonPressed delegate
    private long _token;
    private ButtonPressedHandler? _handlerObject;

    /// <summary>Raised on a background thread with Play, Pause, Stop, Next or Previous.</summary>
    public event Action<string>? ButtonPressed;

    /// <summary>Connects to the controls for a top-level window. Safe to skip: every other call is then a no-op.</summary>
    public void Attach(IntPtr hwnd)
    {
        if (_smtc != IntPtr.Zero) return;
        try
        {
            var factory = GetActivationFactory("Windows.Media.SystemMediaTransportControls", IID_Interop);
            try
            {
                var iid = IID_Smtc;
                IntPtr smtc;
                // ISystemMediaTransportControlsInterop::GetForWindow(HWND, REFIID, void**) at slot 6 (IInspectable base).
                Check(((delegate* unmanaged[Stdcall]<IntPtr, IntPtr, Guid*, IntPtr*, int>)Slot(factory, 6))(factory, hwnd, &iid, &smtc));
                _smtc = smtc;
            }
            finally { Marshal.Release(factory); }

            SetBool(_smtc, SlotPutIsEnabled, true);
            SetBool(_smtc, SlotPutIsPlayEnabled, true);
            SetBool(_smtc, SlotPutIsPauseEnabled, true);
            SetBool(_smtc, SlotPutIsStopEnabled, true);
            SetBool(_smtc, SlotPutIsNextEnabled, true);
            SetBool(_smtc, SlotPutIsPreviousEnabled, true);
            SetInt(_smtc, SlotPutPlaybackStatus, StatusClosed);

            IntPtr updater;
            Check(((delegate* unmanaged[Stdcall]<IntPtr, IntPtr*, int>)Slot(_smtc, SlotGetDisplayUpdater))(_smtc, &updater));
            _updater = updater;

            if (Marshal.QueryInterface(_smtc, IID_Smtc2, out var smtc2) == 0) _smtc2 = smtc2;

            _handlerObject = new ButtonPressedHandler(OnButton);
            _handler = Marshal.GetComInterfaceForObject(_handlerObject, typeof(ITypedEventHandlerButtonPressed));
            long token;
            Check(((delegate* unmanaged[Stdcall]<IntPtr, IntPtr, long*, int>)Slot(_smtc, SlotAddButtonPressed))(_smtc, _handler, &token));
            _token = token;
        }
        catch (Exception ex)
        {
            AppLog.Write(ex, "SMTC unavailable");
            Dispose();
        }
    }

    private void OnButton(int button) => ButtonPressed?.Invoke(button switch
    {
        0 => "Play",
        1 => "Pause",
        2 => "Stop",
        6 => "Next",
        7 => "Previous",
        _ => "",
    });

    public void UpdateStatus(bool playing, bool paused)
    {
        if (_smtc == IntPtr.Zero) return;
        Try(() => SetInt(_smtc, SlotPutPlaybackStatus, playing ? StatusPlaying : paused ? StatusPaused : StatusStopped));
    }

    public void UpdateMetadata(string title, string? artist, bool isVideo, string? artworkUrl)
    {
        if (_updater == IntPtr.Zero) return;
        Try(() =>
        {
            Call(_updater, SlotClearAll);
            SetInt(_updater, SlotPutType, isVideo ? TypeVideo : TypeMusic);

            IntPtr props;
            Check(((delegate* unmanaged[Stdcall]<IntPtr, IntPtr*, int>)Slot(_updater, isVideo ? SlotGetVideoProps : SlotGetMusicProps))(_updater, &props));
            try
            {
                SetString(props, SlotPutTitle, title);
                if (!string.IsNullOrEmpty(artist)) SetString(props, isVideo ? SlotPutVideoSubtitle : SlotPutMusicArtist, artist);
            }
            finally { Marshal.Release(props); }

            if (!string.IsNullOrEmpty(artworkUrl) && Uri.TryCreate(artworkUrl, UriKind.Absolute, out _))
                SetThumbnail(artworkUrl);

            Call(_updater, SlotUpdate);
        }, "SMTC metadata");
    }

    public void UpdateTimeline(long positionMs, long lengthMs)
    {
        if (_smtc2 == IntPtr.Zero || lengthMs <= 0) return;
        Try(() =>
        {
            var inspectable = ActivateInstance("Windows.Media.SystemMediaTransportControlsTimelineProperties");
            try
            {
                Check(Marshal.QueryInterface(inspectable, IID_Timeline, out var timeline));
                try
                {
                    const long TicksPerMs = TimeSpan.TicksPerMillisecond;
                    SetLong(timeline, SlotPutStart, 0);
                    SetLong(timeline, SlotPutMinSeek, 0);
                    SetLong(timeline, SlotPutPosition, Math.Clamp(positionMs, 0, lengthMs) * TicksPerMs);
                    SetLong(timeline, SlotPutMaxSeek, lengthMs * TicksPerMs);
                    SetLong(timeline, SlotPutEnd, lengthMs * TicksPerMs);
                    Check(((delegate* unmanaged[Stdcall]<IntPtr, IntPtr, int>)Slot(_smtc2, SlotUpdateTimeline))(_smtc2, timeline));
                }
                finally { Marshal.Release(timeline); }
            }
            finally { Marshal.Release(inspectable); }
        });
    }

    public void Clear()
    {
        if (_smtc == IntPtr.Zero) return;
        Try(() =>
        {
            SetInt(_smtc, SlotPutPlaybackStatus, StatusClosed);
            Call(_updater, SlotClearAll);
            Call(_updater, SlotUpdate);
        });
    }

    private void SetThumbnail(string url)
    {
        var uriFactory = GetActivationFactory("Windows.Foundation.Uri", IID_UriFactory);
        var streamStatics = GetActivationFactory("Windows.Storage.Streams.RandomAccessStreamReference", IID_StreamRefStatics);
        IntPtr uri = IntPtr.Zero, reference = IntPtr.Zero;
        var hs = CreateHString(url);
        try
        {
            // IUriRuntimeClassFactory::CreateUri(HSTRING, IUriRuntimeClass**) then
            // IRandomAccessStreamReferenceStatics::CreateFromUri(IUriRuntimeClass*, IRandomAccessStreamReference**).
            Check(((delegate* unmanaged[Stdcall]<IntPtr, IntPtr, IntPtr*, int>)Slot(uriFactory, 6))(uriFactory, hs, &uri));
            Check(((delegate* unmanaged[Stdcall]<IntPtr, IntPtr, IntPtr*, int>)Slot(streamStatics, 7))(streamStatics, uri, &reference));
            Check(((delegate* unmanaged[Stdcall]<IntPtr, IntPtr, int>)Slot(_updater, SlotPutThumbnail))(_updater, reference));
        }
        finally
        {
            WindowsDeleteString(hs);
            if (reference != IntPtr.Zero) Marshal.Release(reference);
            if (uri != IntPtr.Zero) Marshal.Release(uri);
            Marshal.Release(streamStatics);
            Marshal.Release(uriFactory);
        }
    }

    public void Dispose()
    {
        try
        {
            if (_smtc != IntPtr.Zero && _token != 0)
                ((delegate* unmanaged[Stdcall]<IntPtr, long, int>)Slot(_smtc, SlotRemoveButtonPressed))(_smtc, _token);
        }
        catch { /* ignore */ }
        foreach (var p in new[] { _handler, _updater, _smtc2, _smtc })
            if (p != IntPtr.Zero) Marshal.Release(p);
        _handler = _updater = _smtc2 = _smtc = IntPtr.Zero;
        _token = 0;
        _handlerObject = null;
    }

    // ----- COM / WinRT plumbing -----

    private static void* Slot(IntPtr comObject, int index) => (*(void***)comObject)[index];

    private static void Check(int hr)
    {
        if (hr < 0) Marshal.ThrowExceptionForHR(hr);
    }

    private static void Call(IntPtr obj, int slot) =>
        Check(((delegate* unmanaged[Stdcall]<IntPtr, int>)Slot(obj, slot))(obj));

    private static void SetInt(IntPtr obj, int slot, int value) =>
        Check(((delegate* unmanaged[Stdcall]<IntPtr, int, int>)Slot(obj, slot))(obj, value));

    private static void SetLong(IntPtr obj, int slot, long value) =>
        Check(((delegate* unmanaged[Stdcall]<IntPtr, long, int>)Slot(obj, slot))(obj, value));

    // WinRT 'boolean' is one byte.
    private static void SetBool(IntPtr obj, int slot, bool value) =>
        Check(((delegate* unmanaged[Stdcall]<IntPtr, byte, int>)Slot(obj, slot))(obj, value ? (byte)1 : (byte)0));

    private static void SetString(IntPtr obj, int slot, string value)
    {
        var hs = CreateHString(value);
        try { Check(((delegate* unmanaged[Stdcall]<IntPtr, IntPtr, int>)Slot(obj, slot))(obj, hs)); }
        finally { WindowsDeleteString(hs); }
    }

    private static IntPtr GetActivationFactory(string className, Guid iid)
    {
        var hs = CreateHString(className);
        try
        {
            Check(RoGetActivationFactory(hs, ref iid, out var factory));
            return factory;
        }
        finally { WindowsDeleteString(hs); }
    }

    private static IntPtr ActivateInstance(string className)
    {
        var hs = CreateHString(className);
        try
        {
            Check(RoActivateInstance(hs, out var instance));
            return instance;
        }
        finally { WindowsDeleteString(hs); }
    }

    private static IntPtr CreateHString(string s)
    {
        Check(WindowsCreateString(s, s.Length, out var hs));
        return hs;
    }

    private static void Try(Action action, string? context = null)
    {
        try { action(); }
        catch (Exception ex) { if (context is not null) AppLog.Write(ex, context); }
    }

    [DllImport("combase.dll")]
    private static extern int RoGetActivationFactory(IntPtr activatableClassId, ref Guid iid, out IntPtr factory);

    [DllImport("combase.dll")]
    private static extern int RoActivateInstance(IntPtr activatableClassId, out IntPtr instance);

    [DllImport("combase.dll", CharSet = CharSet.Unicode)]
    private static extern int WindowsCreateString(string source, int length, out IntPtr hstring);

    [DllImport("combase.dll")]
    private static extern int WindowsDeleteString(IntPtr hstring);

    /// <summary>TypedEventHandler&lt;SystemMediaTransportControls, SystemMediaTransportControlsButtonPressedEventArgs&gt;.</summary>
    [ComVisible(true)]
    [Guid("0557e996-7b23-5bae-aa81-ea0d671143a4")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface ITypedEventHandlerButtonPressed
    {
        [PreserveSig]
        int Invoke(IntPtr sender, IntPtr args);
    }

    [ComVisible(true)]
    [ClassInterface(ClassInterfaceType.None)]
    public sealed class ButtonPressedHandler : ITypedEventHandlerButtonPressed
    {
        private readonly Action<int> _onButton;

        public ButtonPressedHandler(Action<int> onButton) => _onButton = onButton;

        public int Invoke(IntPtr sender, IntPtr args)
        {
            try
            {
                // ISystemMediaTransportControlsButtonPressedEventArgs::get_Button at slot 6.
                int button;
                var hr = ((delegate* unmanaged[Stdcall]<IntPtr, int*, int>)Slot(args, 6))(args, &button);
                if (hr >= 0) _onButton(button);
            }
            catch { /* never throw across COM */ }
            return 0;
        }
    }
}
