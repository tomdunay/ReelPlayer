#include "core/CommandLine.h"
#include "core/PlayerService.h"
#include "core/ResumeStore.h"
#include "core/Settings.h"
#include "core/SingleInstance.h"
#include "ui/MainWindow.h"
#include "win/WinInterop.h"
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <filesystem>
#include <future>
#include <memory>
#include <vector>

namespace fs = std::filesystem;

static void setupLibVlcDllPath() {
    wchar_t exeBuf[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) == 0) return;
    fs::path dir = fs::path(exeBuf).parent_path() / L"libvlc" / L"win-x64";
    std::error_code ec;
    if (fs::exists(dir / L"libvlc.dll", ec)) {
        SetDllDirectoryW(dir.c_str());
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // 1. Parse command line
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> rawArgs;
    for (int i = 1; i < argc; ++i) rawArgs.emplace_back(argv[i]);
    if (argv) LocalFree(argv);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 2. Load settings & check SingleInstance BEFORE initializing Direct2D or libVLC (~3 ms!)
    reel::SettingsService settings;
    settings.load();
    auto cmd = reel::CommandLine::parse(rawArgs);

    std::unique_ptr<reel::SingleInstance> instance;
    if (settings.current().oneInstance) {
        instance = std::make_unique<reel::SingleInstance>(L"ReelPlayer");
        if (!instance->isFirstInstance()) {
            if (instance->forward(rawArgs)) {
                CoUninitialize();
                return 0;
            }
        }
    }

    setupLibVlcDllPath();

    // 3. Start libVLC init asynchronously on PlayerService's worker thread so the window paints in < 25 ms!
    auto vlcOpts = reel::PlayerService::buildOptions(settings.current());
    auto engine = std::make_unique<reel::PlayerService>(vlcOpts, true);
    auto resumeFut = std::async(std::launch::async, []() {
        return std::make_unique<reel::ResumeStore>();
    });
    std::thread([] { reel::AppLog::prune(); }).detach();

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    reel::D2DContext d2d;
    if (!d2d.init()) {
        MessageBoxW(nullptr, L"Could not initialize Direct2D / DirectWrite.", L"Reel Player", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    auto resume = resumeFut.get();

    int exitCode = 0;
    {
        reel::MainWindow win(hInst, d2d, settings, *engine, *resume);
        if (!win.create()) {
            d2d.shutdown();
            CoUninitialize();
            return 1;
        }

        if (instance && instance->isFirstInstance()) {
            HWND hwnd = win.hwnd();
            instance->startListening([hwnd, &win](std::vector<std::wstring> forwarded) {
                win.postForwardedArgs(std::move(forwarded));
                PostMessageW(hwnd, reel::MainWindow::WM_APP_FORWARDED_ARGS, 0, 0);
            });
        }

        win.showAndHandleCommandLine(cmd);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = static_cast<int>(msg.wParam);
    }

    instance.reset();
    engine.reset();
    d2d.shutdown();
    CoUninitialize();
    return exitCode;
}
