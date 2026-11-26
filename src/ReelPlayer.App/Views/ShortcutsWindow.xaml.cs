using System.Windows;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.Views;

public partial class ShortcutsWindow : Window
{
    public ShortcutsWindow(HotkeyMap map)
    {
        InitializeComponent();
        List.ItemsSource = HotkeyActions.All
            .Select(a => new { a.Description, Keys = string.Join(", ", map.GesturesFor(a.Id)) })
            .Where(r => r.Keys.Length > 0)
            .ToList();
    }
}
