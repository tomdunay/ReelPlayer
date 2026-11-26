using System.Collections.Specialized;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using ReelPlayer.Core.Models;
using ReelPlayer.Core.Services;

namespace ReelPlayer.App.Views;

public partial class PlaylistPanel : UserControl
{
    private const string InternalDragFormat = "ReelPlayer.PlaylistItems";
    private Playlist? _playlist;
    private Point _dragStart;
    private bool _dragArmed;

    public PlaylistPanel() => InitializeComponent();

    public event Action<MediaItem>? ItemActivated;
    public event Action<string>? ActionRequested;
    public event Action<string[]>? FilesDropped;

    public void Attach(Playlist playlist)
    {
        _playlist = playlist;
        List.ItemsSource = playlist.Items;
        playlist.Items.CollectionChanged += (_, _) => UpdateCount();
        UpdateCount();
    }

    public void FocusList()
    {
        List.Focus();
        if (_playlist?.Current is { } cur) List.ScrollIntoView(cur);
    }

    private void UpdateCount() =>
        CountText.Text = _playlist is null ? "" : $"{_playlist.Items.Count} item{(_playlist.Items.Count == 1 ? "" : "s")}";

    private IEnumerable<MediaItem> Selected => List.SelectedItems.Cast<MediaItem>().ToList();

    private void SearchBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        var view = CollectionViewSource.GetDefaultView(List.ItemsSource);
        if (view is null) return;
        var q = SearchBox.Text.Trim();
        view.Filter = q.Length == 0
            ? null
            : o => o is MediaItem m &&
                   (m.Title.Contains(q, StringComparison.OrdinalIgnoreCase) ||
                    m.Location.Contains(q, StringComparison.OrdinalIgnoreCase));
    }

    private void ClearSearch_Click(object sender, RoutedEventArgs e)
    {
        SearchBox.Clear();
        SearchBox.Focus();
    }

    private void List_MouseDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (ItemFromPoint(e.GetPosition(List)) is { } item) ItemActivated?.Invoke(item);
    }

    private void List_KeyDown(object sender, KeyEventArgs e)
    {
        switch (e.Key)
        {
            case Key.Delete:
                RemoveSelected();
                e.Handled = true;
                break;
            case Key.Enter when List.SelectedItem is MediaItem item:
                ItemActivated?.Invoke(item);
                e.Handled = true;
                break;
        }
    }

    private void RemoveSelected()
    {
        if (_playlist is null) return;
        var index = List.SelectedIndex;
        foreach (var item in Selected) _playlist.Remove(item);
        if (List.Items.Count > 0) List.SelectedIndex = Math.Min(Math.Max(index, 0), List.Items.Count - 1);
    }

    private void Remove_Click(object sender, RoutedEventArgs e) => RemoveSelected();

    private void Clear_Click(object sender, RoutedEventArgs e) => _playlist?.Clear();

    private void Action_Click(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string action }) ActionRequested?.Invoke(action);
    }

    private void Sort_Click(object sender, RoutedEventArgs e)
    {
        SortButton.ContextMenu.PlacementTarget = SortButton;
        SortButton.ContextMenu.IsOpen = true;
    }

    private void SortItem_Click(object sender, RoutedEventArgs e)
    {
        if (_playlist is null || sender is not MenuItem { Tag: string mode }) return;
        switch (mode)
        {
            case "title": _playlist.Sort(i => i.Title, false); break;
            case "title-desc": _playlist.Sort(i => i.Title, true); break;
            case "duration": _playlist.Sort(i => i.DurationMs, false); break;
            case "path": _playlist.Sort(i => i.Location, false); break;
            case "random":
                var rng = new Random();
                _playlist.Sort(_ => rng.Next(), false);
                break;
        }
    }

    // ----- Drag to reorder, drop files to enqueue -----

    private void List_PreviewMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        _dragStart = e.GetPosition(List);
        _dragArmed = ItemFromPoint(_dragStart) is not null;
    }

    private void List_PreviewMouseMove(object sender, MouseEventArgs e)
    {
        if (!_dragArmed || e.LeftButton != MouseButtonState.Pressed) return;
        var pos = e.GetPosition(List);
        if (Math.Abs(pos.X - _dragStart.X) < SystemParameters.MinimumHorizontalDragDistance &&
            Math.Abs(pos.Y - _dragStart.Y) < SystemParameters.MinimumVerticalDragDistance) return;
        _dragArmed = false;
        if (SearchBox.Text.Length > 0) return; // reordering a filtered view would be confusing
        var items = Selected.ToList();
        if (items.Count == 0) return;
        DragDrop.DoDragDrop(List, new DataObject(InternalDragFormat, items), DragDropEffects.Move);
    }

    private void List_DragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(InternalDragFormat) ? DragDropEffects.Move
            : e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy
            : DragDropEffects.None;
        e.Handled = true;
    }

    private void List_Drop(object sender, DragEventArgs e)
    {
        e.Handled = true;
        if (_playlist is null) return;

        if (e.Data.GetData(InternalDragFormat) is List<MediaItem> moving)
        {
            var target = ItemFromPoint(e.GetPosition(List));
            var targetIndex = target is null ? _playlist.Items.Count - 1 : _playlist.Items.IndexOf(target);
            if (targetIndex < 0) return;

            // Preserve original relative order regardless of drag direction.
            var ordered = moving
                .Select(i => (Item: i, Index: _playlist.Items.IndexOf(i)))
                .Where(x => x.Index >= 0)
                .OrderBy(x => x.Index)
                .Select(x => x.Item)
                .ToList();

            var insertAt = targetIndex;
            foreach (var item in ordered)
            {
                var from = _playlist.Items.IndexOf(item);
                if (from < 0) continue;
                var dest = Math.Clamp(insertAt, 0, _playlist.Items.Count - 1);
                if (from < dest)
                    _playlist.Move(from, dest);
                else
                {
                    _playlist.Move(from, dest);
                    insertAt++;
                }
            }
            List.SelectedItems.Clear();
            foreach (var item in ordered) List.SelectedItems.Add(item);
        }
        else if (e.Data.GetData(DataFormats.FileDrop) is string[] files)
        {
            FilesDropped?.Invoke(files);
        }
    }

    private MediaItem? ItemFromPoint(Point p)
    {
        var hit = List.InputHitTest(p) as DependencyObject;
        while (hit is not null and not ListViewItem)
            hit = hit is Visual or System.Windows.Media.Media3D.Visual3D ? VisualTreeHelper.GetParent(hit) : LogicalTreeHelper.GetParent(hit);
        return (hit as ListViewItem)?.DataContext as MediaItem;
    }
}
