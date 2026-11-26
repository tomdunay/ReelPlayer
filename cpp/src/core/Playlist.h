#pragma once
#include "core/MediaItem.h"
#include <algorithm>
#include <functional>
#include <random>
#include <vector>

namespace reel {

enum class LoopMode { Off, All, One };

class Playlist {
public:
    explicit Playlist(uint32_t seed = 0);

    const std::vector<MediaItemPtr>& items() const { return _items; }
    std::vector<MediaItemPtr>& itemsMut() { return _items; }

    LoopMode loop() const { return _loop; }
    void setLoop(LoopMode m) { _loop = m; }

    bool shuffle() const { return _shuffle; }
    void setShuffle(bool on) { _shuffle = on; _shuffleBag.clear(); }

    MediaItemPtr current() const { return _current; }
    int currentIndex() const;

    std::function<void()> onChanged;

    int addRange(const std::vector<MediaItemPtr>& items);
    void setCurrent(const MediaItemPtr& item);
    void setCurrentIndex(int index);

    MediaItemPtr peekNext(bool userInitiated);
    MediaItemPtr peekPrevious();

    void remove(const MediaItemPtr& item);
    void clear();
    void move(int oldIndex, int newIndex);

    template <typename Comp>
    void sort(Comp&& comp) {
        std::stable_sort(_items.begin(), _items.end(), std::forward<Comp>(comp));
        notify();
    }

    void shuffleOrder();

private:
    MediaItemPtr randomFromBag(bool refill);
    void notify() { if (onChanged) onChanged(); }

    std::mt19937 _rng;
    std::vector<MediaItemPtr> _items;
    std::vector<MediaItemPtr> _shuffleBag;
    MediaItemPtr _current;
    LoopMode _loop = LoopMode::Off;
    bool _shuffle = false;
};

} // namespace reel
