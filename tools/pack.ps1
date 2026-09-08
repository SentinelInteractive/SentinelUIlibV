# pack.ps1 — assemble and deploy the SentinelUI runtime into a GTA V install.
#
# Layout produced (<game> = GTA V root, default D:\SteamLibrary\steamapps\common\Grand Theft Auto V):
#   <game>\SentinelUI\
#     SentinelUI.asi            host module (loaded ONLY by the RPH Bootstrap plugin)
#     SentinelUI_Worker.exe     CEF subprocess
#     libcef.dll, *.pak, icudtl.dat, ...  full CEF runtime (colocated with the host)
#     content\p1_demo.html      P1 gate demo page
#     cache\  logs\             runtime dirs
#     LICENSE.txt               CEF BSD license
#   <game>\Plugins\
#     SentinelUI.Bootstrap.dll  RPH auto-load
#     SentinelUI.API.dll        managed bridge (dev runtime; NuGet in P3)
#
# Reversible: remove the SentinelUI\ folder + the two Plugins\ dlls.

param(
    [string]$GameDir = "D:\SteamLibrary\steamapps\common\Grand Theft Auto V",
    [ValidateSet("Debug", "Release")]
    [string]$HostConfig = "Release",
    [ValidateSet("Debug", "Release")]
    [string]$ManagedConfig = "Debug"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if (-not (Test-Path $GameDir)) { throw "Game dir not found: $GameDir" }
$hostOut = Join-Path $repo "native\host\build\$HostConfig"
if (-not (Test-Path (Join-Path $hostOut "SentinelUI.asi"))) { throw "Host not built (run tools\build-host.ps1)." }

$cefState = Get-Content (Join-Path $repo "native\third_party\cef-current.json") -Raw | ConvertFrom-Json
$cefRoot = $cefState.path

$suiDir = Join-Path $GameDir "SentinelUI"
$contentDir = Join-Path $suiDir "content"
$pluginsDir = Join-Path $GameDir "Plugins"

Write-Host "[pack] Deploying to $GameDir" -ForegroundColor Cyan

# ---- clean previous runtime (our own folder; safe) ----
if (Test-Path $suiDir) { Remove-Item -Recurse -Force $suiDir }
New-Item -ItemType Directory -Force -Path $contentDir, (Join-Path $suiDir "cache"), (Join-Path $suiDir "logs") | Out-Null

# ---- host module + worker ----
Copy-Item (Join-Path $hostOut "SentinelUI.asi") $suiDir
Copy-Item (Join-Path $hostOut "SentinelUI_Worker.exe") $suiDir

# ---- CEF runtime (everything staged next to the host by CMake COPY_FILES) ----
Get-ChildItem $hostOut -File | Where-Object { $_.Name -notin @("SentinelUI.asi", "SentinelUI_Worker.exe", "SentinelUI.lib", "SentinelUI.exp", "SentinelUI.pdb") } |
    Copy-Item -Destination $suiDir -Force
if (Test-Path (Join-Path $hostOut "locales")) {
    Copy-Item (Join-Path $hostOut "locales") -Destination $suiDir -Recurse -Force
}

# ---- content + license ----
Copy-Item (Join-Path $repo "web\content\p1_demo.html") $contentDir
$cefLicense = Join-Path $cefRoot "LICENSE.txt"
if (Test-Path $cefLicense) { Copy-Item $cefLicense (Join-Path $suiDir "LICENSE_CEF.txt") } else { Write-Host "[pack] WARN cef LICENSE.txt not found" -ForegroundColor Yellow }

# ---- managed plugin (RPH auto-load) ----
$managedOut = Join-Path $repo "managed\SentinelUI.Bootstrap\bin\$ManagedConfig"
foreach ($file in @("SentinelUI.Bootstrap.dll", "SentinelUI.API.dll")) {
    $src = Join-Path $managedOut $file
    if (-not (Test-Path $src)) { throw "Missing managed output: $src" }
    Copy-Item $src $pluginsDir -Force
}

Write-Host "[pack] DONE." -ForegroundColor Green
Write-Host ""
Write-Host "Deployed:" -ForegroundColor Cyan
Get-ChildItem $suiDir -Recurse -File | ForEach-Object { "  " + $_.FullName.Substring($GameDir.Length) }
Write-Host ""
Write-Host "Revert: remove <game>\SentinelUI\ and delete Plugins\SentinelUI.Bootstrap.dll + Plugins\SentinelUI.API.dll" -ForegroundColor Yellow