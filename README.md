# Reel Player

A Windows media player with a VLC-style interface. Playback runs on **libVLC 3**, the engine inside VLC, so it supports the same formats: almost every container and codec, subtitles, DVD/Blu-ray/Audio CD, and network streams (HTTP/HLS/DASH/RTSP/RTMP/UDP/SMB).

<img src="src/ReelPlayer.App/Assets/logo.png" width="96" alt="Reel Player logo">

## Features

- **Interface:** menu bar (Media · Playback · Audio · Video · Subtitle · Tools · View · Help), seek bar with hover time and chapter marks, and a control bar. The playlist panel can be docked and supports search, drag-to-reorder and sorting. There is a status bar, a minimal mode (Ctrl+H) and a dark/light/system theme (WPF Fluent).
- **Fullscreen:** a controller overlay that hides itself after 3 seconds, and an on-screen display for volume, seeking, speed, tracks and delays.
- **Playback:** speed from 0.25× to 4×, frame stepping, jump to time, titles and chapters, loop (off/all/one), shuffle, resuming long files where you left off, and recording to `.ts`.
- **Audio and video:** track selection, output device, stereo mode, and volume up to 200%. Also aspect ratio, crop, zoom, deinterlacing (10 modes), snapshots, a 10-band equalizer with presets, video adjustments (brightness, contrast, hue, saturation, gamma), and rotate/flip/sharpen.
- **Subtitles:** a matching subtitle file loads automatically. You can add a subtitle file, switch tracks, adjust delay, and set language, encoding, size, colour and outline.
- **Opening media:** files, folders and playlists (M3U/M3U8/PLS/XSPF/CUE), discs, network URLs, DirectShow capture devices and desktop capture.
- **Tools:** Media Information (metadata, codecs, live statistics), Track Synchronization, Messages (libVLC log with a verbosity filter), and Preferences with editable hotkeys.
- **Windows integration:**
  - Drag and drop.
  - Single instance: files you open later go to the running player.
  - Media keys and the Windows media flyout (SMTC).
  - Taskbar progress and Previous/Play/Next thumbnail buttons.
  - Prevents sleep while video plays.
  - Optional per-user file associations (no admin rights needed).
- **Keyboard:** VLC-compatible default keys, all of which can be changed (Help › Keyboard Shortcuts).

## Build

Requirements: Windows 10 1809+ and the [.NET 10 SDK](https://dotnet.microsoft.com/download). ffmpeg is needed only to generate test samples.

```powershell
dotnet build                                   # Debug build: src\ReelPlayer.App\bin\Debug\...\ReelPlayer.exe
dotnet test tests\ReelPlayer.Core.Tests        # unit tests
.\publish.ps1                                  # release: dist\ReelPlayer-win-x64\ + .zip
.\publish.ps1 -Runtime win-arm64               # other architectures: win-x86, win-arm64
.\publish.ps1 -Installer                       # also builds dist\ReelPlayer-Setup-x64.exe (needs Inno Setup 6)
```

The release is **self-contained**, so the target PC needs no .NET install. It contains:

```
ReelPlayer.exe              ~85 MB  single file: app + .NET runtime + WPF (compressed)
libvlc\win-x64\             ~100 MB libvlc.dll, libvlccore.dll, plugins\ (≈400 files)
THIRD-PARTY-NOTICES.txt
```

libVLC has to stay as loose files next to the exe because it loads its plugins from disk. Distribute the whole folder (or the zip or installer), not just the exe.

## Usage

```
ReelPlayer.exe [--fullscreen] [--start-time=SECONDS|hh:mm:ss] file1 file2 url…
```

Settings, resume positions and logs are stored in `%APPDATA%\ReelPlayer\`.

## Architecture

```
ReelPlayer.slnx
├─ src/ReelPlayer.Core/          UI-independent logic (net10.0)
│   ├─ Models/MediaItem          playlist entry (file path or MRL + options)
│   └─ Services/
│       PlayerService            owns LibVLC + MediaPlayer; open/stop on a dedicated worker thread
│       Playlist                 order, current item, loop/shuffle rules
│       PlaylistParser           M3U/PLS/XSPF read & write (HLS is passed to libVLC as a stream)
│       SettingsService          settings.json (atomic save)
│       ResumeStore, Hotkeys, CommandLine, MediaFiles, SingleInstance (mutex + named pipe)
├─ src/ReelPlayer.App/           WPF UI (net10.0-windows)
│   ├─ ViewModels/MainViewModel  playback state and commands (CommunityToolkit.Mvvm)
│   ├─ Views/MainWindow          menus, hotkeys, fullscreen, OSD; one action dispatcher for all of them
│   ├─ Views/*Window             Open Media, Effects, Sync, Media Info, Messages, Preferences, …
│   ├─ Controls/ControlBar       seek bar + transport (docked and fullscreen instances)
│   └─ Services/                 SMTC, file associations, logging, Win32 interop
├─ tests/ReelPlayer.Core.Tests/  xUnit
├─ tests/make-samples.ps1        ffmpeg test-clip generator
└─ tools/FormatProbe/            headless libVLC check of every sample (see TESTING.md)
```

Implementation notes:

- **WPF airspace:** the video is a native window, so WPF can't draw over it directly. Overlays (OSD, fullscreen controller, error and resume bars) are placed inside `VideoView`'s content, which LibVLCSharp renders in a transparent window above the video.
- **Threading:** libVLC events are marshalled to the UI thread, and nothing calls libVLC from inside its own callbacks. `Play`/`Stop` run on a worker thread so the UI never blocks.
- **Per-media options:** rotate/flip, sharpen and recording are libVLC per-media options. Changing them reopens the current item at the same position.

## Licensing

Reel Player's own code is yours to license. It uses **libVLC** and **LibVLCSharp** under the **LGPL-2.1-or-later**. Their binaries are loaded dynamically and shipped unmodified, which keeps the LGPL obligations simple: include the notices (done in `THIRD-PARTY-NOTICES.txt` and Help › About) and point to the source. Reel Player is not affiliated with VideoLAN, and its name and icon are original.
