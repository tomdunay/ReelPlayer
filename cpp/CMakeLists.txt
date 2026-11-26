cmake_minimum_required(VERSION 3.24)
project(ReelPlayer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json REQUIRED)

# Locate libVLC 3.0.24 headers and import library from the local NuGet cache or LIBVLC_SDK_DIR.
if(NOT DEFINED LIBVLC_SDK_DIR)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}/.nuget/packages/videolan.libvlc.windows/3.0.24/build/x64" LIBVLC_SDK_DIR)
endif()
if(NOT EXISTS "${LIBVLC_SDK_DIR}/include/vlc/vlc.h")
    message(FATAL_ERROR "libVLC SDK not found at ${LIBVLC_SDK_DIR}. Pass -DLIBVLC_SDK_DIR=...")
endif()

# Core library (pure C++20 + Win32, zero UI framework dependencies)
add_library(reelplayer_core STATIC
    src/core/TimeFormat.cpp
    src/core/MediaFiles.cpp
    src/core/Playlist.cpp
    src/core/PlaylistParser.cpp
    src/core/Settings.cpp
    src/core/ResumeStore.cpp
    src/core/Hotkeys.cpp
    src/core/CommandLine.cpp
    src/core/SingleInstance.cpp
    src/core/PlayerService.cpp
)
target_include_directories(reelplayer_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBVLC_SDK_DIR}/include
)
target_link_libraries(reelplayer_core PUBLIC
    nlohmann_json::nlohmann_json
    "${LIBVLC_SDK_DIR}/libvlc.lib"
    shlwapi
    shell32
)
target_compile_definitions(reelplayer_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

# Main native Win32 + Direct2D executable
add_executable(ReelPlayer WIN32
    src/main.cpp
    src/win/WinInterop.cpp
    src/win/MediaControls.cpp
    src/ui/D2DControls.cpp
    src/ui/MainWindow.cpp
    src/ui/Dialogs.cpp
    resources/app.rc
)
target_link_libraries(ReelPlayer PRIVATE
    reelplayer_core
    d2d1
    dwrite
    dwmapi
    windowscodecs
    ole32
    oleaut32
    uuid
    shell32
    shlwapi
    comctl32
    comdlg32
    user32
    gdi32
    uxtheme
)

if(MINGW)
    set(COMMON_FLAGS -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    target_compile_options(reelplayer_core PRIVATE ${COMMON_FLAGS})
    target_compile_options(ReelPlayer PRIVATE ${COMMON_FLAGS})
    # Statically link MinGW C++/GCC/winpthread runtimes so ReelPlayer.exe has zero extra DLL dependencies.
    target_link_options(ReelPlayer PRIVATE -s -municode -static-libgcc -static-libstdc++ -Wl,--gc-sections)
    target_link_libraries(ReelPlayer PRIVATE -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
endif()

# Standalone native C++ unit tests
enable_testing()
add_executable(ReelPlayerCoreTests tests/CoreTests.cpp)
target_link_libraries(ReelPlayerCoreTests PRIVATE reelplayer_core)
if(MINGW)
    target_compile_options(ReelPlayerCoreTests PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    target_link_options(ReelPlayerCoreTests PRIVATE -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
endif()
add_test(NAME CoreTests COMMAND ReelPlayerCoreTests)
