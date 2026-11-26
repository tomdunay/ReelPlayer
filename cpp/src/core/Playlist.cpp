#include "core/Playlist.h"
#include <algorithm>

namespace reel {

Playlist::Playlist(uint32_t seed) : _rng(seed ? seed : std::random_device{}()) {}

int Playlist::currentIndex() const {
    if (!_current) return -1;
    for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
        if (_items[i] == _current) return i;
    }
    return -1;
}

int Playlist::addRange(const std::vector<MediaItemPtr>& items) {
    int first = -1;
    for (const auto& item : items) {
        if (first < 0) first = static_cast<int>(_items.size());
        _items.push_back(item);
        if (_shuffle) _shuffleBag.push_back(item);
    }
    if (first >= 0) notify();
    return first;
}

void Playlist::setCurrent(const MediaItemPtr& item) {
    if (item == _current) return;
    if (_current) _current->isCurrent = false;
    _current = item;
    if (_current) {
        _current->isCurrent = true;
        std::erase(_shuffleBag, _current);
    }
    notify();
}

void Playlist::setCurrentIndex(int index) {
    if (index >= 0 && index < static_cast<int>(_items.size()))
        setCurrent(_items[index]);
    else
        setCurrent(nullptr);
}

MediaItemPtr Playlist::peekNext(bool userInitiated) {
    if (_items.empty()) return nullptr;
    if (!_current) return _shuffle ? randomFromBag(true) : _items[0];
    if (_loop == LoopMode::One && !userInitiated) return _current;

    if (_shuffle) {
        bool refill = _loop != LoopMode::Off || userInitiated;
        return randomFromBag(refill);
    }

    int next = currentIndex() + 1;
    if (next < static_cast<int>(_items.size())) return _items[next];
    return _loop != LoopMode::Off ? _items[0] : nullptr;
}

MediaItemPtr Playlist::peekPrevious() {
    if (_items.empty() || !_current) return nullptr;
    int prev = currentIndex() - 1;
    if (prev >= 0) return _items[prev];
    return _loop == LoopMode::All ? _items.back() : nullptr;
}

void Playlist::remove(const MediaItemPtr& item) {
    std::erase(_items, item);
    std::erase(_shuffleBag, item);
    if (item == _current) setCurrent(nullptr);
    else notify();
}

void Playlist::clear() {
    _items.clear();
    _shuffleBag.clear();
    setCurrent(nullptr);
    notify();
}

void Playlist::move(int oldIndex, int newIndex) {
    int n = static_cast<int>(_items.size());
    if (oldIndex == newIndex || oldIndex < 0 || newIndex < 0 || oldIndex >= n || newIndex >= n) return;
    auto item = std::move(_items[oldIndex]);
    _items.erase(_items.begin() + oldIndex);
    _items.insert(_items.begin() + newIndex, std::move(item));
    notify();
}

void Playlist::shuffleOrder() {
    std::shuffle(_items.begin(), _items.end(), _rng);
    notify();
}

MediaItemPtr Playlist::randomFromBag(bool refill) {
    std::erase_if(_shuffleBag, [this](const MediaItemPtr& p) {
        return std::find(_items.begin(), _items.end(), p) == _items.end();
    });
    if (_shuffleBag.empty()) {
        if (!refill) return nullptr;
        for (const auto& i : _items) {
            if (i != _current) _shuffleBag.push_back(i);
        }
        if (_shuffleBag.empty()) return _current;
    }
    std::uniform_int_distribution<size_t> dist(0, _shuffleBag.size() - 1);
    return _shuffleBag[dist(_rng)];
}

} // namespace reel
