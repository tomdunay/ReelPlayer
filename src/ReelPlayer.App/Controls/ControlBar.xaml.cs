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
    private bool _mouseSeeking;
    private bool _thumbDragging;
    private DateTime _lastLiveSeek;

    public ControlBar()
    {
        InitializeComponent();
        DataContextChanged += (_, _) => Attach(DataContext as MainViewModel);

        SeekSlider.PreviewMouseLeftButtonDown += (_, _) => BeginSeek(ref _mouseSeeking);
        SeekSlider.PreviewMouseLeftButtonUp += (_, _) => { if (_mouseSeeking) { _mouseSeeking = false; EndSeek(); } };
        SeekSlider.AddHandler(Thumb.DragStartedEvent, new DragStartedEventHandler((_, _) => BeginSeek(ref _thumbDragging)));
        SeekSlider.AddHandler(Thumb.DragCompletedEvent, new DragCompletedEventHandler((_, _) => { _thumbDragging = false; EndSeek(); }));
        SeekSlider.ValueChanged += SeekSlider_ValueChanged;
        SeekSlider.MouseMove += SeekSlider_MouseMove;
        SeekSlider.MouseLeave += (_, _) => SeekTip.IsOpen = false;
        SeekSlider.SizeChanged += (_, _) => DrawChapters();
    }

    /// <summary>Raised with an action id (PlayPause, Stop, Fullscreen, …) when a button is clicked.</summary>
    public event Action<string>? ActionRequested;

    private bool IsSeeking => _mouseSeeking || _thumbDragging;

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
                DrawChapters();
                break;
        }
    }

    private void BeginSeek(ref bool flag)
    {
        flag = true;
        if (_vm is not null) _vm.IsUserSeeking = true;
    }

    private void EndSeek()
    {
        if (IsSeeking || _vm is null) return;
        _vm.Seek((long)SeekSlider.Value);
        _vm.IsUserSeeking = false;
    }

    private void SeekSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!IsSeeking || _vm is null) return;
        _vm.UpdateTimeTexts((long)e.NewValue, _vm.LengthMs);
        // Live scrubbing, throttled so libVLC is not flooded with seeks.
        if ((DateTime.UtcNow - _lastLiveSeek).TotalMilliseconds > 120)
        {
            _lastLiveSeek = DateTime.UtcNow;
            _vm.Seek((long)e.NewValue);
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
        var frac = Math.Clamp(x / SeekSlider.ActualWidth, 0, 1);
        SeekTipText.Text = TimeFormat.Format((long)(frac * _vm.LengthMs));
        SeekTip.HorizontalOffset = x - 22;
        SeekTip.IsOpen = true;
    }

    private void DrawChapters()
    {
        ChapterCanvas.Children.Clear();
        if (_vm is null || SeekSlider.ActualWidth <= 0) return;
        const double thumbHalf = 6; // Track keeps the thumb inside, so usable width is narrower.
        var usable = SeekSlider.ActualWidth - thumbHalf * 2;
        foreach (var f in _vm.ChapterFractions)
        {
            var mark = new Rectangle { Width = 2, Height = 8, Fill = (System.Windows.Media.Brush)FindResource("ChapterMarkBrush") };
            Canvas.SetLeft(mark, thumbHalf + f * usable - 1);
            Canvas.SetTop(mark, 5);
            ChapterCanvas.Children.Add(mark);
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
