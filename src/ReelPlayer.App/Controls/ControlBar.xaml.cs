using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Shapes;
using ReelPlayer.App.ViewModels;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.Controls;

/// <summary>Seek bar + transport buttons + volume. Used docked under the video and as the fullscreen overlay.</summary>
public partial class ControlBar : UserControl
{
    private MainViewModel? _vm;
    private bool _trackDragging;
    private bool _thumbDragging;
    private DateTime _lastLiveSeek;
    private long _lastSeekMs = -1;

    public ControlBar()
    {
        InitializeComponent();
        DataContextChanged += (_, _) => Attach(DataContext as MainViewModel);

        // Slider with IsMoveToPointEnabled handles PreviewMouseLeftButtonDown in its class handler
        // (updating Value and setting e.Handled = true) before instance handlers run, so listen with handledEventsToo.
        SeekSlider.AddHandler(PreviewMouseLeftButtonDownEvent, new MouseButtonEventHandler(SeekSlider_PreviewMouseLeftButtonDown), handledEventsToo: true);
        SeekSlider.AddHandler(PreviewMouseLeftButtonUpEvent, new MouseButtonEventHandler(SeekSlider_PreviewMouseLeftButtonUp), handledEventsToo: true);
        SeekSlider.LostMouseCapture += (_, _) => { if (_trackDragging) { _trackDragging = false; EndSeek(); } };
        SeekSlider.AddHandler(Thumb.DragStartedEvent, new DragStartedEventHandler((_, _) => BeginThumbDrag()));
        SeekSlider.AddHandler(Thumb.DragCompletedEvent, new DragCompletedEventHandler((_, _) => { _thumbDragging = false; EndSeek(); }));
        SeekSlider.ValueChanged += SeekSlider_ValueChanged;
        SeekSlider.MouseMove += SeekSlider_MouseMove;
        SeekSlider.MouseLeave += (_, _) => SeekTip.IsOpen = false;
        SeekSlider.SizeChanged += (_, _) => DrawChapters();
    }

    /// <summary>Raised with an action id (PlayPause, Stop, Fullscreen, …) when a button is clicked.</summary>
    public event Action<string>? ActionRequested;

    private bool IsSeeking => _trackDragging || _thumbDragging;

    private void Attach(MainViewModel? vm)
    {
        if (_vm is not null) _vm.PropertyChanged -= Vm_PropertyChanged;
        _vm = vm;
        if (_vm is null) return;
        _vm.PropertyChanged += Vm_PropertyChanged;
        SeekSlider.Maximum = Math.Max(1, _vm.LengthMs);
        SeekSlider.Value = _vm.PositionMs;
    }

    private void Vm_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_vm is null) return;
        switch (e.PropertyName)
        {
            case nameof(MainViewModel.PositionMs) when !IsSeeking:
                SeekSlider.Value = Math.Min(_vm.PositionMs, SeekSlider.Maximum);
                break;
            case nameof(MainViewModel.LengthMs):
                SeekSlider.Maximum = Math.Max(1, _vm.LengthMs);
                DrawChapters();
                break;
            case nameof(MainViewModel.ChapterFractions):
            case nameof(MainViewModel.AbLoopStartMs):
            case nameof(MainViewModel.AbLoopEndMs):
                DrawChapters();
                break;
        }
    }

    private void SeekSlider_PreviewMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (!e.Handled || _vm is null) return; // Thumb clicks are not handled by Slider; DragStarted handles those.
        _trackDragging = true;
        _vm.IsUserSeeking = true;
        var ms = (long)SeekSlider.Value;
        _lastSeekMs = ms;
        _lastLiveSeek = DateTime.UtcNow;
        _vm.Seek(ms);
        SeekSlider.CaptureMouse();
    }

    private void SeekSlider_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (!_trackDragging) return;
        _trackDragging = false;
        if (SeekSlider.IsMouseCaptured) SeekSlider.ReleaseMouseCapture();
        EndSeek();
    }

    private void BeginThumbDrag()
    {
        _thumbDragging = true;
        _lastSeekMs = (long)SeekSlider.Value;
        if (_vm is not null) _vm.IsUserSeeking = true;
    }

    private void EndSeek()
    {
        if (IsSeeking || _vm is null) return;
        var ms = (long)SeekSlider.Value;
        if (ms != _lastSeekMs) _vm.Seek(ms);
        _lastSeekMs = -1;
        _vm.IsUserSeeking = false;
    }

    private void SeekSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!IsSeeking || _vm is null) return;
        var ms = (long)e.NewValue;
        _vm.UpdateTimeTexts(ms, _vm.LengthMs);
        // Live scrubbing, throttled so libVLC is not flooded with seeks.
        if ((DateTime.UtcNow - _lastLiveSeek).TotalMilliseconds > 120)
        {
            _lastLiveSeek = DateTime.UtcNow;
            _lastSeekMs = ms;
            _vm.Seek(ms);
        }
    }

    private void SeekSlider_MouseMove(object sender, MouseEventArgs e)
    {
        if (_vm is null || _vm.LengthMs <= 0 || SeekSlider.ActualWidth <= 0)
        {
            SeekTip.IsOpen = false;
            return;
        }
        var x = e.GetPosition(SeekSlider).X;
        if (_trackDragging && e.LeftButton == MouseButtonState.Pressed)
        {
            const double thumbHalf = 6;
            var usable = SeekSlider.ActualWidth - thumbHalf * 2;
            if (usable > 0)
                SeekSlider.Value = Math.Clamp((x - thumbHalf) / usable, 0, 1) * SeekSlider.Maximum;
        }
        var frac = Math.Clamp(x / SeekSlider.ActualWidth, 0, 1);
        var hoverMs = (long)(frac * _vm.LengthMs);
        SeekTipText.Text = TimeFormat.Format(hoverMs);
        var chapter = _vm.ChapterNameAt(hoverMs);
        if (!string.IsNullOrEmpty(chapter))
        {
            SeekTipChapter.Text = chapter;
            SeekTipChapter.Visibility = Visibility.Visible;
        }
        else
        {
            SeekTipChapter.Visibility = Visibility.Collapsed;
        }
        SeekTip.HorizontalOffset = x - 22;
        SeekTip.IsOpen = true;
    }

    private void DrawChapters()
    {
        ChapterCanvas.Children.Clear();
        if (_vm is null || SeekSlider.ActualWidth <= 0) return;
        const double thumbHalf = 6; // Track keeps the thumb inside, so usable width is narrower.
        var usable = SeekSlider.ActualWidth - thumbHalf * 2;
        if (usable <= 0) return;

        var markBrush = (System.Windows.Media.Brush)FindResource("ChapterMarkBrush");
        foreach (var f in _vm.ChapterFractions)
        {
            var mark = new Rectangle { Width = 2, Height = 8, Fill = markBrush };
            Canvas.SetLeft(mark, thumbHalf + f * usable - 1);
            Canvas.SetTop(mark, 5);
            ChapterCanvas.Children.Add(mark);
        }

        // Draw A-B loop range highlight and endpoint ticks when active.
        if (_vm.LengthMs > 0 && _vm.AbLoopStartMs is { } startMs)
        {
            var aFrac = Math.Clamp((double)startMs / _vm.LengthMs, 0, 1);
            var aX = thumbHalf + aFrac * usable;
            if (_vm.AbLoopEndMs is { } endMs)
            {
                var bFrac = Math.Clamp((double)endMs / _vm.LengthMs, 0, 1);
                var bX = thumbHalf + bFrac * usable;
                if (bX > aX)
                {
                    var span = new Rectangle
                    {
                        Width = bX - aX, Height = 4, RadiusX = 2, RadiusY = 2,
                        Fill = markBrush, Opacity = 0.55,
                    };
                    Canvas.SetLeft(span, aX);
                    Canvas.SetTop(span, 7);
                    ChapterCanvas.Children.Add(span);
                }
                var bTick = new Rectangle { Width = 3, Height = 12, RadiusX = 1, RadiusY = 1, Fill = markBrush };
                Canvas.SetLeft(bTick, bX - 1.5);
                Canvas.SetTop(bTick, 3);
                ChapterCanvas.Children.Add(bTick);
            }
            var aTick = new Rectangle { Width = 3, Height = 12, RadiusX = 1, RadiusY = 1, Fill = markBrush };
            Canvas.SetLeft(aTick, aX - 1.5);
            Canvas.SetTop(aTick, 3);
            ChapterCanvas.Children.Add(aTick);
        }
    }

    private void Action_Click(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string action }) ActionRequested?.Invoke(action);
    }

    private void TotalTime_Click(object sender, MouseButtonEventArgs e)
    {
        if (_vm is not null) _vm.ShowRemaining = !_vm.ShowRemaining;
    }

    private void VolumeSlider_MouseWheel(object sender, MouseWheelEventArgs e)
    {
        _vm?.ChangeVolume(e.Delta > 0 ? 5 : -5);
        e.Handled = true;
    }
}
