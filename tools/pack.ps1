# pack.ps1 — assemble and deploy the SentinelUI runtime into a GTA V install.
#
# Canonical layout (ASI + CEF runtime live at the GAME ROOT — ASI loaders and the
# CEF delay-load helper both resolve from there; only the content cache/logs live
# in a subfolder, resolved by absolute path in host code):
#
#   <game>\                          (GTA V root)
#     SentinelUI.asi                 host module — ASI loader loads it like any ASI
#     SentinelUI_Worker.exe          CEF subprocess
#     libcef.dll *.pak icudtl.dat ... full CEF runtime (flat, next to the .asi)
#     locales\                       CEF locale packs
#     SentinelUI.API.dll             managed bridge (also copied to Plugins\)
#     SentinelUI\
#       content\p1_demo.html         P1 gate demo
#       logs\                        host + API logs (own files; no RPH spam)
#       cache\                       CEF profile/cache
#       LICENSE_CEF.txt
#   <game>\Plugins\
#     SentinelUI.Bootstrap.dll       RPH auto-load
#     SentinelUI.API.dll
#
# Revert: remove the listed files/dirs from the game root + Plugins\ dlls.

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
$pluginsDir = Join-Path $GameDir "Plugins"

Write-Host "[pack] Deploying to $GameDir" -ForegroundColor Cyan

# ---- clean previous versions (our own artifacts; safe to recreate) ----
$rootArtifacts = @("SentinelUI.asi", "SentinelUI_Worker.exe", "libcef.dll", "chrome_elf.dll",
                   "icudtl.dat", "resources.pak", "chrome_100_percent.pak", "chrome_200_percent.pak",
                   "v8_context_snapshot.bin", "libEGL.dll", "libGLESv2.dll", "d3dcompiler_47.dll",
                   "dxil.dll", "dxcompiler.dll", "vk_swiftshader.dll", "vk_swiftshader_icd.json",
                   "vulkan-1.dll", "locales")
foreach ($name in $rootArtifacts) {
    $p = Join-Path $GameDir $name
    if (Test-Path $p) { Remove-Item -Recurse -Force $p }
}
if (Test-Path $suiDir) { Remove-Item -Recurse -Force $suiDir }
New-Item -ItemType Directory -Force -Path (Join-Path $suiDir "content"), (Join-Path $suiDir "cache"), (Join-Path $suiDir "logs") | Out-Null

# ---- host module + worker + CEF runtime (flat at game root) ----
Get-ChildItem $hostOut | Where-Object { $_.Name -notin @("SentinelUI.lib", "SentinelUI.exp", "SentinelUI.pdb") } |
    Copy-Item -Destination $GameDir -Recurse -Force

# ---- content + license ----
Copy-Item (Join-Path $repo "web\content\p1_demo.html") (Join-Path $suiDir "content")
$cefLicense = Join-Path $cefRoot "LICENSE.txt"
if (Test-Path $cefLicense) { Copy-Item $cefLicense (Join-Path $suiDir "LICENSE_CEF.txt") } else { Write-Host "[pack] WARN cef LICENSE.txt not found" -ForegroundColor Yellow }

# ---- managed plugin (RPH auto-load) ----
$managedOut = Join-Path $repo "managed\SentinelUI.Bootstrap\bin\$ManagedConfig"
foreach ($file in @("SentinelUI.Bootstrap.dll", "SentinelUI.API.dll")) {
    $src = Join-Path $managedOut $file
    if (-not (Test-Path $src)) { throw "Missing managed output: $src" }
    Copy-Item $src $pluginsDir -Force
}
Copy-Item (Join-Path $managedOut "SentinelUI.API.dll") (Join-Path $GameDir "SentinelUI.API.dll") -Force

Write-Host "[pack] DONE." -ForegroundColor Green
Write-Host ""
Write-Host "Deployed (root):" -ForegroundColor Cyan
Get-ChildItem $GameDir -File | Where-Object { $_.Name -like "SentinelUI*" -or $_.Name -eq "libcef.dll" -or $_.Name -like "*.pak" -or $_.Name -eq "icudtl.dat" -or $_.Name -eq "v8_context_snapshot.bin" } | ForEach-Object { "  " + $_.Name }
Write-Host "Revert: remove the SentinelUI.* / libcef.dll / locales files from the game root,"
Write-Host "        the SentinelUI\ folder, and Plugins\SentinelUI.Bootstrap.dll + SentinelUI.API.dll" -ForegroundColor Yellow