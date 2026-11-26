<#
.SYNOPSIS
  Builds a self-contained Reel Player release (no .NET install needed on the target PC).
.EXAMPLE
  .\publish.ps1                 # win-x64 → dist\ReelPlayer-win-x64\ and dist\ReelPlayer-win-x64.zip
  .\publish.ps1 -Runtime win-arm64
  .\publish.ps1 -Installer      # also compiles installer\ReelPlayer.iss with Inno Setup (iscc on PATH)
#>
param(
    [ValidateSet('win-x64', 'win-x86', 'win-arm64')]
    [string]$Runtime = 'win-x64',
    [switch]$Installer,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root "dist\ReelPlayer-$Runtime"
$zip = "$out.zip"

if (-not $SkipTests) {
    Write-Host '== Unit tests' -ForegroundColor Cyan
    dotnet test (Join-Path $root 'tests\ReelPlayer.Core.Tests') -c Release --nologo
    if ($LASTEXITCODE) { throw 'Unit tests failed.' }
}

Write-Host "== Publishing $Runtime" -ForegroundColor Cyan
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
dotnet publish (Join-Path $root 'src\ReelPlayer.App') -c Release -r $Runtime --self-contained true `
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true `
    -p:PublishReadyToRun=true `
    -p:DebugType=none -o $out --nologo
if ($LASTEXITCODE) { throw 'dotnet publish failed.' }

# libVLC must live next to the exe (it cannot be packed into the single file).
$arch = $Runtime.Split('-')[1]
$libvlc = Join-Path $out "libvlc\win-$arch"
if (-not (Test-Path (Join-Path $libvlc 'libvlc.dll'))) { throw "libVLC was not copied to $libvlc." }

Copy-Item (Join-Path $root 'THIRD-PARTY-NOTICES.txt') $out -Force
Copy-Item (Join-Path $root 'README.md') $out -Force -ErrorAction SilentlyContinue

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -CompressionLevel Optimal

$size = '{0:N0} MB' -f ((Get-ChildItem $out -Recurse | Measure-Object Length -Sum).Sum / 1MB)
Write-Host "== Done: $out ($size), $zip" -ForegroundColor Green

if ($Installer) {
    $iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
    if (-not $iscc) {
        $default = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
        if (Test-Path $default) { $iscc = $default } else { throw 'Inno Setup (iscc) not found: winget install JRSoftware.InnoSetup' }
    }
    & $iscc "/DSourceDir=$out" "/DArch=$arch" (Join-Path $root 'installer\ReelPlayer.iss')
    if ($LASTEXITCODE) { throw 'Installer build failed.' }
}
