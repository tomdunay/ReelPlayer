#include "core/SingleInstance.h"
#include "core/StrUtil.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace reel {

SingleInstance::SingleInstance(const std::wstring& appId) {
    wchar_t userBuf[256] = {};
    DWORD len = 256;
    GetUserNameW(userBuf, &len);
    std::wstring user(userBuf);
    std::replace(user.begin(), user.end(), L'\\', L'_');

    _pipePath = L"\\\\.\\pipe\\" + appId + L"-" + user + L"-pipe";
    std::wstring mutexName = L"Local\\" + appId + L"-" + user + L"-mutex";
    _mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    _isFirst = (_mutex != nullptr && GetLastError() != ERROR_ALREADY_EXISTS);
    _stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

SingleInstance::~SingleInstance() {
    stop();
    if (_stopEvent) CloseHandle(_stopEvent);
    if (_mutex) {
        if (_isFirst) ReleaseMutex(_mutex);
        CloseHandle(_mutex);
    }
}

bool SingleInstance::forward(const std::vector<std::wstring>& args) const {
    if (!WaitNamedPipeW(_pipePath.c_str(), 3000)) return false;
    HANDLE pipe = CreateFileW(_pipePath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;

    json arr = json::array();
    for (const auto& a : args) arr.push_back(toUtf8(a));
    std::string payload = arr.dump();

    DWORD written = 0;
    BOOL ok = WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    FlushFileBuffers(pipe);
    CloseHandle(pipe);
    return ok && written == payload.size();
}

void SingleInstance::startListening(std::function<void(std::vector<std::wstring>)> onArgs) {
    if (!_isFirst) return;
    _onArgs = std::move(onArgs);
    _thread = std::thread(&SingleInstance::listenLoop, this);
}

void SingleInstance::stop() {
    _stopping = true;
    if (_stopEvent) SetEvent(_stopEvent);
    if (_thread.joinable()) _thread.join();
}

void SingleInstance::listenLoop() {
    while (!_stopping) {
        HANDLE pipe = CreateNamedPipeW(
            _pipePath.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(200);
            continue;
        }

        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(pipe, &ov);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = { ov.hEvent, _stopEvent };
                DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wr != WAIT_OBJECT_0) {
                    CancelIo(pipe);
                    CloseHandle(ov.hEvent);
                    CloseHandle(pipe);
                    break;
                }
                connected = TRUE;
            } else if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }
        }
        CloseHandle(ov.hEvent);

        if (connected && !_stopping) {
            std::string data;
            char buf[2048];
            DWORD read = 0;
            OVERLAPPED rov = {};
            rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            while (!_stopping) {
                ResetEvent(rov.hEvent);
                BOOL ok = ReadFile(pipe, buf, sizeof(buf), &read, &rov);
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    HANDLE waits[2] = { rov.hEvent, _stopEvent };
                    if (WaitForMultipleObjects(2, waits, FALSE, 2000) != WAIT_OBJECT_0) break;
                    ok = GetOverlappedResult(pipe, &rov, &read, FALSE);
                }
                if (!ok || read == 0) break;
                data.append(buf, read);
            }
            CloseHandle(rov.hEvent);

            try {
                auto arr = json::parse(data);
                if (arr.is_array() && _onArgs) {
                    std::vector<std::wstring> args;
                    for (const auto& el : arr) {
                        if (el.is_string()) args.push_back(toWide(el.get<std::string>()));
                    }
                    _onArgs(std::move(args));
                }
            } catch (...) {}
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

} // namespace reel
