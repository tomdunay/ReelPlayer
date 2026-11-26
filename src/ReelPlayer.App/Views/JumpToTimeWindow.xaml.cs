using System.Windows;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.Views;

public partial class JumpToTimeWindow : Window
{
    private readonly long _lengthMs;

    public JumpToTimeWindow(long currentMs, long lengthMs)
    {
        _lengthMs = lengthMs;
        InitializeComponent();
        Prompt.Text = lengthMs > 0 ? $"Go to time (length {TimeFormat.Format(lengthMs)}):" : "Go to time:";
        TimeBox.Text = TimeFormat.Format(currentMs);
        Loaded += (_, _) => { TimeBox.Focus(); TimeBox.SelectAll(); };
    }

    public long TargetMs { get; private set; }

    private void Go_Click(object sender, RoutedEventArgs e)
    {
        if (!TimeFormat.TryParse(TimeBox.Text, out var ms) || (_lengthMs > 0 && ms > _lengthMs))
        {
            ErrorText.Visibility = Visibility.Visible;
            return;
        }
        TargetMs = ms;
        DialogResult = true;
    }
}
