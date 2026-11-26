# Build Prompt: Windows Desktop Media Player

> Copy everything below the line into Claude Code (or another coding agent), run from an empty project folder.

---

## Goal

Build a Windows desktop media player that compiles into a distributable `.exe`. It must play roughly the same set of formats as VLC, and its layout and controls should feel familiar to VLC users. Use an **original name and icon**: no VLC cone or VLC branding. Working name: **"Reel Player"** (rename freely).

## Technology stack (required)

- **Language/UI:** C# on **.NET 8**, **WPF** (MVVM pattern, using `CommunityToolkit.Mvvm`).
- **Playback engine:** **libVLC 3.x** through **LibVLCSharp** + **LibVLCSharp.WPF** + **VideoLAN.LibVLC.Windows** (NuGet). Using libVLC itself is what gets us VLC-level format support: every demuxer, codec, and subtitle format VLC has, plus hardware decoding (DXVA2/D3D11VA).
- **Target:** `win-x64` (optionally `win-x86` and `win-arm64` as extra publish profiles).
- **Licensing:** libVLC is LGPL-2.1+. Load it dynamically (the default for LibVLCSharp), ship its DLLs and `plugins/` folder unmodified, and add a `THIRD-PARTY-NOTICES.txt` plus an About dialog that credit VideoLAN/libVLC and link to the LGPL.

## Format support (verify each in testing)

- **Containers:** MP4/M4V, MKV/MKA/WebM, AVI, MOV, WMV/ASF, FLV, MPEG-TS/M2TS/MTS, MPEG-PS/VOB, OGG/OGV, 3GP, RM/RMVB, MXF, NUT, DV
- **Video codecs:** H.264, H.265/HEVC, AV1, VP8/VP9, MPEG-1/2/4, DivX/Xvid, VC-1, WMV, Theora, ProRes, DNxHD, MJPEG
- **Audio:** AAC, MP3, FLAC, ALAC, Opus, Vorbis, AC-3, E-AC-3, DTS, TrueHD, WAV/PCM, WMA, APE, WavPack
- **Subtitles:** SRT, ASS/SSA (styled), VTT, SUB/IDX (VobSub), PGS, embedded MKV/MP4 tracks
- **Discs & streams:** DVD folders/ISO, Audio CD, unencrypted Blu-ray; HTTP/HTTPS, HLS (.m3u8), DASH, RTSP, RTMP, UDP/RTP, SMB paths
- **Playlists:** M3U/M3U8, PLS, XSPF, CUE

## UI: VLC-style layout

### Main window
1. **Menu bar** with these menus:
   - **Media:** Open File (Ctrl+O), Open Multiple Files, Open Folder (Ctrl+F), Open Disc (Ctrl+D), Open Network Stream (Ctrl+N), Open Recent (last 10, persisted), Save Playlist, Quit (Ctrl+Q)
   - **Playback:** Play/Pause, Stop, Previous, Next, Jump Forward/Backward, Jump to Time (Ctrl+T), Speed (faster/slower/normal), Chapter and Title submenus, Record
   - **Audio:** Audio Track, Audio Device, Stereo Mode, Volume Up/Down, Mute
   - **Video:** Video Track, Fullscreen, Always on Top, Aspect Ratio (Default, 16:9, 4:3, 1:1, 16:10, 2.21:1, 2.35:1, 2.39:1, 5:4), Crop, Deinterlace (Off/Auto/On + mode), Zoom, Take Snapshot
   - **Subtitle:** Add Subtitle File, Subtitle Track, Subtitle Delay
   - **Tools:** Effects and Filters (Ctrl+E), Track Synchronization, Media Information (Ctrl+I), Codec Information (Ctrl+J), Messages/Log (Ctrl+M), Preferences (Ctrl+P)
   - **View:** Playlist (Ctrl+L), Minimal Interface (Ctrl+H), Status Bar
   - **Help:** Keyboard Shortcuts, About
2. **Video area:** black background, centered artwork or logo when idle. Double-click toggles fullscreen. Right-click opens a context menu that mirrors the main menus. Mouse wheel changes volume.
3. **Seek bar:** full-width slider with elapsed time on the left and total/remaining time on the right (click to toggle). Hover tooltip shows the timestamp. Click to jump; drag to scrub. Show chapter markers when present.
4. **Control bar** (bottom, left to right): Play/Pause (large), Previous, Stop, Next | Fullscreen, Extended Settings, Toggle Playlist, Loop (off/all/one), Shuffle | spacer | Mute button + volume slider (0–200%, with a percentage label).
5. **Status bar** (optional): current file name, playback speed, state.

### Fullscreen mode
- Controller overlay (seek bar + controls) appears at the bottom on mouse move and auto-hides after 3 s along with the cursor.
- Esc or double-click exits.
- Multi-monitor: go fullscreen on the monitor that contains the window.

### Playlist panel
- Dockable side panel, or swaps with the video area like VLC does.
- Columns: Title, Duration, Path. Supports drag-and-drop reordering, Delete key, double-click to play, search filter, and Add/Remove/Clear/Sort buttons.
- Remembers the last playlist between sessions.

### Dialogs
- **Open Media** (tabbed: File / Disc / Network / Capture Device), with an option to attach a subtitle file.
- **Effects and Filters:** 10-band audio equalizer with presets, video adjustments (brightness, contrast, hue, saturation, gamma), rotate/flip, sharpen.
- **Track Synchronization:** audio delay and subtitle delay in ms, subtitle speed.
- **Media Information:** metadata tab (title, artist, album, art) and codec tab (per-stream codec, resolution, frame rate, bitrate, channels, sample rate).
- **Preferences** (Simple view): interface language placeholder, theme (Light/Dark/System), one instance only, enqueue files when already running, resume playback position, hardware decoding on/off, audio output device, default subtitle language and encoding, subtitle font size/color/outline, snapshot folder and format, editable hotkeys. Persist to `%APPDATA%\ReelPlayer\settings.json`.
- **Messages:** live libVLC log, filterable by verbosity.

## Keyboard shortcuts (VLC-compatible defaults, all rebindable)

| Key | Action | Key | Action |
|---|---|---|---|
| Space | Play/Pause | F / F11 / double-click | Fullscreen |
| Esc | Exit fullscreen | S | Stop |
| N / P | Next / Previous | ← / → | Seek −10 s / +10 s |
| Shift+← / → | Seek −3 s / +3 s | Alt+← / → | Seek −1 min / +1 min |
| Ctrl+← / → | Seek −5 min / +5 min | ↑ / ↓ or Ctrl+↑ / ↓ | Volume ±5% |
| M | Mute | [ / ] / = | Slower / Faster / Normal speed |
| E | Next frame | A | Cycle aspect ratio |
| C | Cycle crop | Z | Cycle zoom |
| B | Cycle audio track | V | Cycle subtitle track |
| G / H | Subtitle delay ∓50 ms | J / K | Audio delay ∓50 ms |
| Shift+S | Snapshot | T | Show time on OSD |
| L | Cycle loop | Ctrl+L | Toggle playlist |

Show an **on-screen display** (OSD) for volume, seek position, speed, track changes, and similar actions. It fades out after about 1.5 s. libVLC's marquee works, or use a WPF overlay.

## Behavior requirements

- Drag-and-drop files, folders, and URLs onto the window (adds and plays).
- Command line: `ReelPlayer.exe [--fullscreen] [--start-time=SECONDS] file1 file2 url...`
- **Single instance:** a second launch forwards its arguments to the running instance over a named pipe (enqueue or play, depending on the setting).
- **Resume playback:** remember the position for files longer than 5 minutes and offer "Continue from 12:34?" when one is reopened.
- **Auto-load subtitles:** load a matching `.srt`/`.ass` file with the same base name.
- Use Windows media keys and the System Media Transport Controls (title and artwork in the Windows volume flyout).
- Use a taskbar thumbnail toolbar (Prev/Play-Pause/Next) and a taskbar progress indicator.
- Keep the PC from sleeping during video playback (`SetThreadExecutionState`).
- Save window size and position, volume, and the last folder.
- Optional file associations: an opt-in Preferences button that registers through `HKCU\Software\Classes` (no admin rights) and opens Windows Default Apps settings.
- **Dark theme by default**, plus a light theme. Use vector icons (Segoe Fluent Icons / Segoe MDL2 Assets), not bitmaps.
- Handle errors gracefully: an unplayable file shows a non-modal error and skips to the next playlist item. Never crash on corrupt media. Log to `%APPDATA%\ReelPlayer\logs`.

## Architecture

```
ReelPlayer.sln
 ├─ src/ReelPlayer.App/          WPF app: Views, ViewModels, Themes, Converters, App.xaml
 ├─ src/ReelPlayer.Core/         PlayerService (libVLC wrapper), PlaylistService, SettingsService,
 │                               HotkeyService, ResumeService, SingleInstanceService, Models
 └─ tests/ReelPlayer.Core.Tests/ xUnit tests: playlist parsing (M3U/PLS/XSPF), settings round-trip,
                                 time formatting, hotkey mapping, resume logic
```

- `PlayerService` owns a single `LibVLC` and `MediaPlayer` instance. Expose observable state (Position, Length, State, Volume, Tracks, Chapters) and marshal libVLC events to the UI thread.
- **Avoid the WPF "airspace" problem:** `LibVLCSharp.WPF.VideoView` hosts an HWND, so WPF content cannot draw over it. Put overlays (OSD, fullscreen controls) inside `VideoView`'s content, which LibVLCSharp renders in a layered window. Never use plain WPF elements stacked on top.
- Never call libVLC methods from inside libVLC event callbacks (that deadlocks). Dispatch them with `ThreadPool` or `Dispatcher` instead.

## Build & packaging

1. `dotnet build` must succeed with zero warnings as errors in Release.
2. Provide `publish.ps1`, which runs:
   ```
   dotnet publish src/ReelPlayer.App -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:PublishReadyToRun=true
   ```
   Output goes to `dist/ReelPlayer-win-x64/`. Users need no .NET runtime installed.
   - **Note:** libVLC's `plugins/` folder (about 300 files) must sit next to the exe; it cannot be embedded in a single file. So the deliverable is `ReelPlayer.exe` plus `libvlc.dll`, `libvlccore.dll`, and `plugins/`, zipped as `ReelPlayer-win-x64.zip`.
   - Trim plugins you don't need (for example, keep `access`, `codec`, `demux`, `video_output`, `audio_output`, `packetizer`, `video_filter`, `audio_filter`, `subtitle`, `misc`, `stream_filter`, `video_chroma`, `audio_mixer`, `spu`, `text_renderer`, `services_discovery`, `meta_engine`, `playlist`) using the `VlcWindowsX64ExcludeFiles` MSBuild item. Document what was excluded and confirm format support still works afterward.
3. **Optional installer:** an Inno Setup script (`installer/ReelPlayer.iss`) that produces `ReelPlayer-Setup.exe` with Start Menu shortcut, uninstaller, and optional file associations.
4. Embed an app icon (`.ico` with 16/32/48/256 sizes, original design), version info, and an application manifest (PerMonitorV2 DPI aware, long path aware).

## Deliverables & acceptance criteria

- [ ] Solution builds and `publish.ps1` produces a working zip on a clean Windows 10/11 machine.
- [ ] Plays a test matrix generated with ffmpeg (provide `tests/make-samples.ps1`, which creates short clips for each container/codec listed above, including subtitle tracks). Document results in `TESTING.md`.
- [ ] Every menu item and every hotkey in the table works.
- [ ] Fullscreen overlay auto-hides; OSD appears over video.
- [ ] Opens an HLS URL and an RTSP URL.
- [ ] 4K HEVC 10-bit plays smoothly with hardware decoding on.
- [ ] Unit tests pass (`dotnet test`).
- [ ] `README.md` covers build steps, architecture overview, and licensing notes.

## Working instructions for the agent

- Work in milestones and commit after each:
  1. Skeleton + playback of one file
  2. Controls + seek + volume
  3. Menus + hotkeys + OSD
  4. Fullscreen overlay
  5. Playlist
  6. Dialogs (Open, Effects, Sync, Info, Prefs, Messages)
  7. Single instance, resume, SMTC, taskbar
  8. Theming polish
  9. Packaging + installer
  10. Test matrix + docs
- After each milestone, build and run the app, take a screenshot, and fix what you see before moving on.
- Prefer libVLC's built-in features (equalizer, video adjust filters, track APIs, snapshot, `--sub-*` options) over reimplementing them.
- Ask me before adding any dependency beyond those listed.
