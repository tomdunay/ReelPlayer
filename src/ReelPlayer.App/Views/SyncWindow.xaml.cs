using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using ReelPlayer.App.ViewModels;

namespace ReelPlayer.App.Views;

public partial class SyncWindow : Window
{
    private readonly MainViewModel _vm;

    public SyncWindow(MainViewModel vm)
    {
        _vm = vm;
        InitializeComponent();
        Refresh();
    }

    private void Refresh()
    {
        AudioDelay.Text = _vm.AudioDelayMs.ToString();
        SubDelay.Text = _vm.SubtitleDelayMs.ToString();
    }

    private void Apply(object sender, RoutedEventArgs e)
    {
        if (long.TryParse(AudioDelay.Text, out var a) && a != _vm.AudioDelayMs) _vm.SetAudioDelay(a);
        if (long.TryParse(SubDelay.Text, out var s) && s != _vm.SubtitleDelayMs) _vm.SetSubtitleDelay(s);
        Refresh();
    }

    private void Box_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter) { Apply(sender, e); e.Handled = true; }
    }

    private void Step_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string tag }) return;
        var parts = tag.Split(':');
        var delta = long.Parse(parts[1]);
        if (parts[0] == "audio") _vm.ShiftAudioDelay(delta);
        else _vm.ShiftSubtitleDelay(delta);
        Refresh();
    }

    private void Reset_Click(object sender, RoutedEventArgs e)
    {
        _vm.SetAudioDelay(0);
        _vm.SetSubtitleDelay(0);
        Refresh();
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Close();
}
