# Testing

## 1. Unit tests

```powershell
dotnet test tests/ReelPlayer.Core.Tests
```

These cover time parsing and formatting, the M3U/PLS/XSPF playlist reader and writer (including telling HLS streams apart from real playlists), loop and shuffle order, hotkey parsing and conflicts, resume rules, settings round-trips, the command line, and natural sort.
The latest run: **51 passed, 0 failed**.

## 2. Format matrix (headless libVLC playback)

```powershell
.\tests\make-samples.ps1 -Include4K      # needs ffmpeg on PATH; writes tests\samples\ (git-ignored)
dotnet run --project tools/FormatProbe -c Release -- tests\samples TESTING-results.md
# against the trimmed libVLC that actually ships:
dotnet run --project tools/FormatProbe -c Release -- --libvlc dist\ReelPlayer-win-x64\libvlc\win-x64 tests\samples
```

The trimmed libVLC in `dist\` passes all 41 samples. Recording, the equalizer, image adjust, deinterlacing, aspect/crop/zoom, speed, delays and snapshots were also checked against both the full and the trimmed libVLC, and produced identical output and identical log errors.

`FormatProbe` plays every sample through the same libVLC build the app ships, with dummy audio and video output. A file **passes** if it reaches the Playing state with no error and decodes at least one video block (when it has video) and one audio block (when it has audio).

Encrypted Blu-ray discs, ISOs and real disc drives, and live RTSP/RTMP/HLS servers are not in the automated matrix. They use the same libVLC access modules as VLC 3.0.24.

### Latest results

libVLC 3.0.24 Vetinari — 41 passed, 0 failed (2026-11-26 07:24)

| File | Result | Streams | Video blocks decoded | Audio blocks decoded | Note |
|---|---|---|---|---|---|
| audio_alac.m4a | PASS | A:Apple Lossless Audio Codec 1ch | 0 | 60 |  |
| audio_truehd.mka | PASS | A:TrueHD Audio 1ch | 0 | 6177 |  |
| audio.aac | PASS | A:MPEG AAC Audio 1ch | 0 | 254 |  |
| audio.ac3 | PASS | A:A52 Audio (aka AC3) 0ch | 0 | 5 |  |
| audio.aiff | PASS | A:PCM S16 BE 1ch | 0 | 50 |  |
| audio.flac | PASS | A:FLAC (Free Lossless Audio Codec) 1ch | 0 | 55 |  |
| audio.mp3 | PASS | A:MPEG Audio layer 3 1ch | 0 | 211 |  |
| audio.ogg | PASS | A:Vorbis Audio 1ch | 0 | 273 |  |
| audio.opus | PASS | A:Opus Audio 1ch | 0 | 284 |  |
| audio.wav | PASS | A:PCM S16 LE 1ch | 0 | 100 |  |
| audio.wma | PASS | A:Windows Media Audio 2 1ch | 0 | 112 |  |
| audio.wv | PASS | A:WavPack 1ch | 0 | 7 |  |
| av1_flac.mkv | PASS | V:AOMedia's AV1 Video 640x360; A:FLAC (Free Lossless Audio Codec) 1ch | 78 | 54 |  |
| chapters.mkv | PASS | V:H264 - MPEG-4 AVC (part 10) 640x360; A:MPEG AAC Audio 1ch | 109 | 238 |  |
| dnxhr_pcm.mov | PASS | V:DNxHD 640x360; A:PCM S16 LE 1ch | 99 | 236 |  |
| dv_pcm.dv | PASS | V:DV Video 720x576; A:PCM S16 LE 2ch | 111 | 128 |  |
| ffv1_pcm.nut | PASS | V:FF video codec 1 640x360; A:PCM S16 LE 1ch | 99 | 232 |  |
| flv1_mp3.flv | PASS | A:MPEG Audio layer 3 1ch; V:Flash Video 640x360 | 100 | 188 |  |
| h264_aac.3gp | PASS | V:H264 - MPEG-4 AVC (part 10) 352x288; A:MPEG AAC Audio 1ch | 101 | 80 |  |
| h264_aac.flv | PASS | V:H264 - MPEG-4 AVC (part 10) 640x360; A:MPEG AAC Audio 1ch | 101 | 218 |  |
| h264_aac.mp4 | PASS | V:H264 - MPEG-4 AVC (part 10) 640x360; A:MPEG AAC Audio 1ch | 101 | 236 |  |
| h264_ac3.m2ts | PASS | V:H264 - MPEG-4 AVC (part 10) 640x360; A:A52 Audio (aka AC3) 1ch | 70 | 72 |  |
| h264_dts.mkv | PASS | V:H264 - MPEG-4 AVC (part 10) 640x360; A:DTS Audio 1ch | 99 | 431 |  |
| h264_eac3.mkv | PASS | V:H264 - MPEG-4 AVC (part 10) 640x360; A:A/52 B Audio (aka E-AC3) 1ch | 99 | 142 |  |
| hevc_opus.mkv | PASS | V:MPEG-H Part2/HEVC (H.265) 640x360; A:Opus Audio 1ch | 105 | 234 |  |
| hevc10_4k.mkv | PASS | V:MPEG-H Part2/HEVC (H.265) 3840x2160 | 115 | 0 |  |
| mjpeg_pcm.mkv | PASS | V:Motion JPEG Video 640x360; A:PCM S16 LE 1ch | 102 | 234 |  |
| mpeg1_mp2.mpg | PASS | V:MPEG-1/2 Video 640x360; A:MPEG Audio layer 1/2 1ch | 76 | 169 |  |
| mpeg2_ac3.vob | PASS | V:MPEG-1/2 Video 640x360; A:A52 Audio (aka AC3) 1ch | 50 | 75 |  |
| mpeg2_mp2.ts | PASS | V:MPEG-1/2 Video 640x360; A:MPEG Audio layer 1/2 1ch | 36 | 187 |  |
| mpeg2_pcm.mxf | PASS | V:MPEG-2 Video 640x360; A:PCM S16 LE 1ch | 71 | 120 |  |
| mpeg4_mp3.avi | PASS | V:MPEG-4 Video 640x360; A:MPEG Audio layer 3 1ch | 96 | 206 |  |
| prores_pcm.mov | PASS | V:Apple ProRes 422 Standard 640x360; A:PCM S16 LE 1ch | 103 | 258 |  |
| rv20_ac3.rm | PASS | V:RealVideo G2 (2.0) 640x360; A:A52 Audio (aka AC3) 1ch | 102 | 154 |  |
| subs_embedded_ass.mkv | PASS | V:H264 - MPEG-4 AVC (part 10) 640x360; A:MPEG AAC Audio 1ch; S:SubStation Alpha subtitles | 101 | 220 |  |
| subs_embedded_movtext.mp4 | PASS | V:H264 - MPEG-4 AVC (part 10) 640x360; A:MPEG AAC Audio 1ch; S: | 99 | 236 |  |
| subs_external.mp4 | PASS | V:H264 - MPEG-4 AVC (part 10) 640x360; S:WEBVTT subtitles; S:Text subtitles with various tags | 87 | 0 |  |
| theora_vorbis.ogv | PASS | V:Xiph.org's Theora Video 640x360; A:Vorbis Audio 1ch | 76 | 187 |  |
| vp8_vorbis.webm | PASS | V:Google/On2's VP8 Video 640x360; A:Vorbis Audio 1ch | 99 | 231 |  |
| vp9_opus.webm | PASS | V:Google/On2's VP9 Video 640x360; A:Opus Audio 1ch | 79 | 236 |  |
| wmv2_wma.wmv | PASS | V:Windows Media Video 8 640x360; A:Windows Media Audio 2 1ch | 94 | 110 |  |

`hevc10_4k.mkv` is a 3840x2160 10-bit HEVC clip. The probe runs with `--avcodec-hw=none`, so this pass is a software decode. In the app, hardware decoding (DXVA2/D3D11VA) is on by default.

## 3. Manual UI checks

Each of these was checked by running the app (Debug build and the published `dist\ReelPlayer-win-x64\ReelPlayer.exe`) and taking screenshots:

| Area | Result |
|---|---|
| Opening a file from the command line, then playback, seek bar, time display and chapter markers | OK |
| Menu bar (Media, Playback, Audio, Video, Subtitle, Tools, View, Help), shortcut text in menus, dynamic submenus | OK |
| Fullscreen: controller overlay shown over the video, hides itself, Esc to exit, window size restored afterwards | OK |
| Playlist panel (Ctrl+L): columns, current item highlighted, reopening a file reuses its entry | OK |
| Effects and Filters (Ctrl+E): 10-band equalizer and presets | OK |
| Codec information (Ctrl+J): per-stream codec, resolution, frame rate, channels, sample rate | OK |
| Stop (S): idle screen | OK |
| Resume bar: "Continue playback from mm:ss?" | OK |
| External `.srt` loaded automatically and rendered | OK |
| Published self-contained build starts in about 2 s (tested on the build PC, which has .NET installed; a clean PC is not tested yet) | OK |

Not exercised by hand: real disc drives, capture devices, network streams, the SMTC media flyout, taskbar thumbnail buttons, file-association registration, and the Inno Setup installer (Inno Setup is not installed on the build machine).
