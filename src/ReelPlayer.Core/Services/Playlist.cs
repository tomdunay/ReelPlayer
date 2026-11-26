using System.Collections.ObjectModel;
using ReelPlayer.Core.Models;

namespace ReelPlayer.Core.Services;

public enum LoopMode { Off, All, One }

/// <summary>Ordered list of media plus the "what plays next" rules (loop, shuffle).</summary>
public sealed class Playlist
{
    private readonly Random _rng;
    private readonly List<MediaItem> _shuffleBag = new();
    private MediaItem? _current;
    private bool _shuffle;

    public Playlist(Random? rng = null) => _rng = rng ?? new Random();

    public ObservableCollection<MediaItem> Items { get; } = new();

    public LoopMode Loop { get; set; }

    public bool Shuffle
    {
        get => _shuffle;
        set { _shuffle = value; _shuffleBag.Clear(); }
    }

    public MediaItem? Current => _current;

    public int CurrentIndex => _current is null ? -1 : Items.IndexOf(_current);

    public event EventHandler? CurrentChanged;

    /// <summary>Adds items and returns the index of the first one added (or -1 when none).</summary>
    public int AddRange(IEnumerable<MediaItem> items)
    {
        var first = -1;
        foreach (var item in items)
        {
            if (first < 0) first = Items.Count;
            Items.Add(item);
            if (_shuffle) _shuffleBag.Add(item);
        }
        return first;
    }

    public void SetCurrent(MediaItem? item)
    {
        if (ReferenceEquals(item, _current)) return;
        if (_current is not null) _current.IsCurrent = false;
        _current = item;
        if (_current is not null)
        {
            _current.IsCurrent = true;
            _shuffleBag.Remove(_current);
        }
        CurrentChanged?.Invoke(this, EventArgs.Empty);
    }

    public void SetCurrent(int index) => SetCurrent(index >= 0 && index < Items.Count ? Items[index] : null);

    /// <summary>
    /// The item to play after the current one, or null to stop.
    /// <paramref name="userInitiated"/> is true for the Next button (ignores Loop One).
    /// </summary>
    public MediaItem? PeekNext(bool userInitiated)
    {
        if (Items.Count == 0) return null;
        if (_current is null) return _shuffle ? RandomFromBag(refill: true) : Items[0];
        if (Loop == LoopMode.One && !userInitiated) return _current;

        if (_shuffle)
        {
            var refill = Loop != LoopMode.Off || userInitiated;
            return RandomFromBag(refill);
        }

        var next = CurrentIndex + 1;
        if (next < Items.Count) return Items[next];
        return Loop != LoopMode.Off ? Items[0] : null;
    }

    public MediaItem? PeekPrevious()
    {
        if (Items.Count == 0 || _current is null) return null;
        var prev = CurrentIndex - 1;
        if (prev >= 0) return Items[prev];
        return Loop == LoopMode.All ? Items[^1] : null;
    }

    public void Remove(MediaItem item)
    {
        Items.Remove(item);
        _shuffleBag.Remove(item);
        if (ReferenceEquals(item, _current)) SetCurrent(null);
    }

    public void Clear()
    {
        Items.Clear();
        _shuffleBag.Clear();
        SetCurrent(null);
    }

    public void Move(int oldIndex, int newIndex)
    {
        if (oldIndex == newIndex || oldIndex < 0 || newIndex < 0 || oldIndex >= Items.Count || newIndex >= Items.Count) return;
        Items.Move(oldIndex, newIndex);
    }

    public void Sort<TKey>(Func<MediaItem, TKey> key, bool descending = false)
    {
        var sorted = descending ? Items.OrderByDescending(key).ToList() : Items.OrderBy(key).ToList();
        for (var i = 0; i < sorted.Count; i++)
        {
            var oldIndex = Items.IndexOf(sorted[i]);
            if (oldIndex != i) Items.Move(oldIndex, i);
        }
    }

    private MediaItem? RandomFromBag(bool refill)
    {
        _shuffleBag.RemoveAll(i => !Items.Contains(i));
        if (_shuffleBag.Count == 0)
        {
            if (!refill) return null;
            _shuffleBag.AddRange(Items.Where(i => !ReferenceEquals(i, _current)));
            if (_shuffleBag.Count == 0) return _current; // single-item playlist
        }
        return _shuffleBag[_rng.Next(_shuffleBag.Count)];
    }
}
