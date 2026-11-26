<#
.SYNOPSIS
  Builds and packages the native C++20 + Win32 + Direct2D Reel Player release (win-x64).
#>
param(
    [switch]$SkipTests,
    [switch]$Installer
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
$buildDir = Join-Path $root 'cpp\build'
$out = Join-Path $root 'dist\ReelPlayer-win-x64'
$zip = "$out.zip"

Write-Host '== Building native C++20 ReelPlayer (Release)' -ForegroundColor Cyan
cmake -S (Join-Path $root 'cpp') -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
cmake --build $buildDir
if ($LASTEXITCODE) { throw 'CMake build failed.' }

$vlcCache = Join-Path $env:USERPROFILE '.nuget\packages\videolan.libvlc.windows\3.0.24\build\x64'
if (-not $SkipTests) {
    Write-Host '== Running native C++ unit tests' -ForegroundColor Cyan
    $oldPath = $env:PATH
    $env:PATH = "$vlcCache;$env:PATH"
    & (Join-Path $buildDir 'ReelPlayerCoreTests.exe')
    $testExit = $LASTEXITCODE
    $env:PATH = $oldPath
    if ($testExit) { throw 'CoreTests failed.' }
}

Write-Host '== Packaging dist\ReelPlayer-win-x64' -ForegroundColor Cyan
# Preserve trimmed libvlc folder if already present in dist\, otherwise copy from NuGet cache.
$tmpVlc = Join-Path $root 'dist\_libvlc_stage'
if (Test-Path $tmpVlc) { Remove-Item $tmpVlc -Recurse -Force }
$existingTrimmed = Join-Path $out 'libvlc\win-x64'
if (Test-Path (Join-Path $existingTrimmed 'libvlc.dll')) {
    Move-Item $existingTrimmed $tmpVlc
} elseif (Test-Path (Join-Path $out 'libvlc.dll')) {
    New-Item -ItemType Directory -Path $tmpVlc | Out-Null
    foreach ($name in @('libvlc.dll', 'libvlccore.dll', 'plugins', 'lua', 'hrtfs')) {
        $src = Join-Path $out $name
        if (Test-Path $src) { Move-Item $src $tmpVlc }
    }
} else {
    Copy-Item $vlcCache $tmpVlc -Recurse -Force
}

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Path $out | Out-Null

Copy-Item (Join-Path $buildDir 'ReelPlayer.exe') $out -Force
foreach ($name in @('libvlc.dll', 'libvlccore.dll', 'plugins', 'lua', 'hrtfs')) {
    $src = Join-Path $tmpVlc $name
    if (Test-Path $src) { Move-Item $src $out }
}
Remove-Item $tmpVlc -Recurse -Force -ErrorAction SilentlyContinue

Copy-Item (Join-Path $root 'THIRD-PARTY-NOTICES.txt') $out -Force
Copy-Item (Join-Path $root 'README.md') $out -Force -ErrorAction SilentlyContinue

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -CompressionLevel Optimal

if ($Installer) {
    $iscc = (Get-Command iscc -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
    if (-not $iscc) {
        foreach ($cand in @(
            "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
            "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
        )) {
            if (Test-Path $cand) { $iscc = $cand; break }
        }
    }
    if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) not found. Install Inno Setup 6 or add iscc to PATH.' }
    Write-Host '== Building Inno Setup installer' -ForegroundColor Cyan
    & $iscc "/DSourceDir=$out" "/DArch=x64" (Join-Path $root 'installer\ReelPlayer.iss')
    if ($LASTEXITCODE) { throw 'Inno Setup build failed.' }
}

$exeSize = '{0:N2} MB' -f ((Get-Item (Join-Path $out 'ReelPlayer.exe')).Length / 1MB)
$dirSize = '{0:N0} MB' -f ((Get-ChildItem $out -Recurse | Measure-Object Length -Sum).Sum / 1MB)
Write-Host "== Done: $out ($dirSize total, ReelPlayer.exe = $exeSize), $zip" -ForegroundColor Green
