#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace reel {

class SingleInstance {
public:
    explicit SingleInstance(const std::wstring& appId);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    bool isFirstInstance() const { return _isFirst; }
    bool forward(const std::vector<std::wstring>& args) const;
    void startListening(std::function<void(std::vector<std::wstring>)> onArgs);
    void stop();

private:
    void listenLoop();

    std::wstring _pipePath;
    HANDLE _mutex = nullptr;
    HANDLE _stopEvent = nullptr;
    bool _isFirst = true;
    std::atomic<bool> _stopping{ false };
    std::thread _thread;
    std::function<void(std::vector<std::wstring>)> _onArgs;
};

} // namespace reel
