<#
.SYNOPSIS
  Generates a matrix of short test clips (containers x codecs, subtitles, playlists) with ffmpeg.
.EXAMPLE
  .\tests\make-samples.ps1            # writes to tests\samples
  .\tests\make-samples.ps1 -Include4K # also a 4K HEVC 10-bit clip (slower)
#>
param(
    [string]$OutDir = (Join-Path $PSScriptRoot 'samples'),
    [switch]$Include4K
)
$ErrorActionPreference = 'Stop'
$env:SVT_LOG = "1"  # quiet the AV1 encoder banner
$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $ffmpeg) { throw 'ffmpeg not found on PATH (winget install Gyan.FFmpeg).' }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$v = '-f lavfi -i testsrc2=size=640x360:rate=25:duration=3'
$a = '-f lavfi -i sine=frequency=440:sample_rate=48000:duration=3'
$ok = 0; $failed = @()

function Make([string]$name, [string]$ffArgs) {
    $out = Join-Path $OutDir $name
    $cmd = "-hide_banner -loglevel error -y $ffArgs `"$out`""
    $p = Start-Process -FilePath $ffmpeg -ArgumentList $cmd -NoNewWindow -Wait -PassThru
    if ($p.ExitCode -eq 0 -and (Test-Path $out)) { $script:ok++; Write-Host "  ok    $name" }
    else { $script:failed += $name; Write-Host "  FAIL  $name" -ForegroundColor Red }
}

Write-Host 'Video containers / codecs'
Make 'h264_aac.mp4'          "$v $a -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest"
Make 'hevc_opus.mkv'         "$v $a -c:v libx265 -preset ultrafast -x265-params log-level=error -c:a libopus -shortest"
Make 'vp9_opus.webm'         "$v $a -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -c:a libopus -shortest"
Make 'vp8_vorbis.webm'       "$v $a -c:v libvpx -deadline realtime -c:a libvorbis -shortest"
Make 'av1_flac.mkv'          "$v $a -c:v libsvtav1 -preset 12 -c:a flac -shortest"
Make 'mpeg4_mp3.avi'         "$v $a -c:v mpeg4 -vtag xvid -q:v 5 -c:a libmp3lame -shortest"
Make 'prores_pcm.mov'        "$v $a -c:v prores_ks -profile:v 0 -c:a pcm_s16le -shortest"
Make 'dnxhr_pcm.mov'         "$v $a -c:v dnxhd -profile:v 1 -pix_fmt yuv422p -c:a pcm_s16le -shortest"
Make 'wmv2_wma.wmv'          "$v $a -c:v wmv2 -c:a wmav2 -shortest"
Make 'h264_aac.flv'          "$v $a -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest"
Make 'flv1_mp3.flv'          "$v $a -c:v flv1 -c:a libmp3lame -ar 44100 -shortest"
Make 'mpeg2_mp2.ts'          "$v $a -c:v mpeg2video -c:a mp2 -shortest"
Make 'h264_ac3.m2ts'         "$v $a -c:v libx264 -pix_fmt yuv420p -c:a ac3 -f mpegts -mpegts_m2ts_mode 1 -shortest"
Make 'mpeg1_mp2.mpg'         "$v $a -c:v mpeg1video -c:a mp2 -f mpeg -shortest"
Make 'mpeg2_ac3.vob'         "$v $a -c:v mpeg2video -c:a ac3 -f vob -shortest"
Make 'theora_vorbis.ogv'     "$v $a -c:v libtheora -c:a libvorbis -shortest"
Make 'h264_aac.3gp'          "-f lavfi -i testsrc2=size=352x288:rate=25:duration=3 $a -c:v libx264 -pix_fmt yuv420p -c:a aac -ar 16000 -ac 1 -shortest"
Make 'mpeg2_pcm.mxf'         "$v $a -c:v mpeg2video -b:v 5M -c:a pcm_s16le -shortest"
Make 'dv_pcm.dv'             "-f lavfi -i testsrc2=size=720x576:rate=25:duration=3 $a -c:v dvvideo -pix_fmt yuv420p -c:a pcm_s16le -ar 48000 -ac 2 -shortest"
Make 'ffv1_pcm.nut'          "$v $a -c:v ffv1 -c:a pcm_s16le -shortest"
Make 'mjpeg_pcm.mkv'         "$v $a -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -c:a pcm_s16le -shortest"
Make 'rv20_ac3.rm'           "$v $a -c:v rv20 -c:a ac3 -shortest"
Make 'h264_eac3.mkv'         "$v $a -c:v libx264 -pix_fmt yuv420p -c:a eac3 -shortest"
Make 'h264_dts.mkv'          "$v $a -c:v libx264 -pix_fmt yuv420p -c:a dca -strict -2 -shortest"

Write-Host 'Audio only'
Make 'audio.mp3'   "$a -c:a libmp3lame"
Make 'audio.aac'   "$a -c:a aac"
Make 'audio_alac.m4a' "$a -c:a alac"
Make 'audio.flac'  "$a -c:a flac"
Make 'audio.opus'  "$a -c:a libopus"
Make 'audio.ogg'   "$a -c:a libvorbis"
Make 'audio.wav'   "$a -c:a pcm_s16le"
Make 'audio.wma'   "$a -c:a wmav2"
Make 'audio.ac3'   "$a -c:a ac3"
Make 'audio.wv'    "$a -c:a wavpack"
Make 'audio.aiff'  "$a -c:a pcm_s16be"
Make 'audio_truehd.mka' "$a -c:a truehd -strict -2"

Write-Host 'Subtitles'
$srt = Join-Path $OutDir 'subs_external.srt'
@"
1
00:00:00,200 --> 00:00:02,800
External SRT subtitle

"@ | ForEach-Object { [IO.File]::WriteAllText($srt, $_) }
Make 'subs_external.mp4' "$v -c:v libx264 -pix_fmt yuv420p"
$ass = Join-Path $OutDir 'subs_ass_source.ass'
@"
[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,32,&H0000FFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,0,2,10,10,20,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.20,0:00:02.80,Default,,0,0,0,,{\i1}Styled ASS{\i0} subtitle
"@ | ForEach-Object { [IO.File]::WriteAllText($ass, $_) }
Make 'subs_embedded_ass.mkv' "$v $a -i `"$ass`" -map 0:v -map 1:a -map 2:s -c:v libx264 -pix_fmt yuv420p -c:a aac -c:s ass -shortest"
Make 'subs_embedded_movtext.mp4' "$v $a -i `"$srt`" -map 0:v -map 1:a -map 2:s -c:v libx264 -pix_fmt yuv420p -c:a aac -c:s mov_text -shortest"
Make 'subs_external.vtt' "-i `"$srt`""

Write-Host 'Chapters'
$meta = Join-Path $OutDir 'chapters.txt'
@"
;FFMETADATA1
[CHAPTER]
TIMEBASE=1/1000
START=0
END=1000
title=Opening
[CHAPTER]
TIMEBASE=1/1000
START=1000
END=2000
title=Middle
[CHAPTER]
TIMEBASE=1/1000
START=2000
END=3000
title=Ending
"@ | ForEach-Object { [IO.File]::WriteAllText($meta, $_) }
Make 'chapters.mkv' "$v $a -i `"$meta`" -map 0:v -map 1:a -map_metadata 2 -map_chapters 2 -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest"

if ($Include4K) {
    Write-Host '4K HEVC 10-bit'
    Make 'hevc10_4k.mkv' "-f lavfi -i testsrc2=size=3840x2160:rate=30:duration=4 -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le -x265-params log-level=error"
}

Write-Host 'Playlists'
$m3u = "#EXTM3U`n#EXTINF:3,H.264 MP4`nh264_aac.mp4`n#EXTINF:3,VP9 WebM`nvp9_opus.webm`n#EXTINF:3,FLAC`naudio.flac`n"
[IO.File]::WriteAllText((Join-Path $OutDir 'playlist.m3u8'), $m3u)
"[playlist]`nFile1=mpeg4_mp3.avi`nTitle1=AVI`nLength1=3`nFile2=audio.mp3`nTitle2=MP3`nNumberOfEntries=2`nVersion=2`n" |
    ForEach-Object { [IO.File]::WriteAllText((Join-Path $OutDir 'playlist.pls'), $_) }
@"
<?xml version="1.0" encoding="UTF-8"?>
<playlist version="1" xmlns="http://xspf.org/ns/0/">
  <trackList>
    <track><location>theora_vorbis.ogv</location><title>Theora</title></track>
    <track><location>audio.opus</location><title>Opus</title></track>
  </trackList>
</playlist>
"@ | ForEach-Object { [IO.File]::WriteAllText((Join-Path $OutDir 'playlist.xspf'), $_) }

Write-Host ""
Write-Host "$ok files created in $OutDir" -ForegroundColor Green
if ($failed.Count) { Write-Host ("Failed: " + ($failed -join ', ')) -ForegroundColor Yellow }
