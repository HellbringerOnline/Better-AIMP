param([Parameter(Mandatory)][string]$Sdk, [string]$Zig = "zig", [string]$Python = "python")
$ErrorActionPreference = "Stop"
$headers = Join-Path (Resolve-Path -LiteralPath $Sdk).Path "Sources/Cpp"
if (!(Test-Path -LiteralPath (Join-Path $headers "apiPlugin.h"))) {
    throw "Укажите папку распакованного AIMP SDK."
}
$dist = Join-Path $PSScriptRoot "dist"
$plugin = Join-Path $dist "aimp_spectrum_probe"
New-Item -ItemType Directory -Path $plugin -Force | Out-Null
$binary = Join-Path $plugin "aimp_spectrum_probe.dll"
& $Zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -shared -Wall -Wextra -isystem $headers (Join-Path $PSScriptRoot "probe.cpp") -o $binary -lgdi32 -luser32 -lole32
if ($LASTEXITCODE -ne 0) { throw "Сборка плагина завершилась ошибкой." }
& $Python (Join-Path $PSScriptRoot "build-skin-layouts.py") $plugin
if ($LASTEXITCODE -ne 0) { throw "Сборка обложек завершилась ошибкой." }
$archive = Join-Path $dist "Better-AIMP-win-x64.zip"
Compress-Archive -LiteralPath $binary,(Join-Path $plugin "layouts") -DestinationPath $archive -Force
Get-Item -LiteralPath $archive
