# pack.ps1 — assemble and deploy the SentinelUI runtime into a GTA V install.
#
# Canonical layout (ASI + CEF runtime live at the GAME ROOT — ASI loaders and the
# CEF delay-load helper both resolve from there; only the content cache/logs live
# in a subfolder, resolved by absolute path in host code):
#
#   <game>\                          (GTA V root)
#     SentinelUI.asi                 ASI compositor — hooks Present/WndProc
#     SentinelUI_Host.exe            dedicated CEF process (spawned by the ASI)
#     libcef.dll *.pak icudtl.dat ... full CEF runtime (flat, next to the ASI)
#     locales\                       CEF locale packs
#     SentinelUI.API.dll             managed bridge (also copied to Plugins\)
#     SentinelUI\
#       content\p1_demo.html         P1 gate demo (+ selftest_page.html)
#       logs\                        host + helper + API logs (no RPH spam)
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
if (Get-Process -Name GTA5 -ErrorAction SilentlyContinue) {
    throw "GTA5 is running - close the game before deploying (runtime files and the CEF cache are locked)."
}
$hostOut = Join-Path $repo "native\host\build"
if (-not (Test-Path (Join-Path $hostOut "SentinelUI.asi"))) { throw "Host not built (run tools\build-host.ps1)." }

$cefState = Get-Content (Join-Path $repo "native\third_party\cef-current.json") -Raw | ConvertFrom-Json
$cefRoot = $cefState.path

$suiDir = Join-Path $GameDir "SentinelUI"
$pluginsDir = Join-Path $GameDir "Plugins"

Write-Host "[pack] Deploying to $GameDir" -ForegroundColor Cyan

# ---- failure-safe pre-step: remove only files we no longer ship or that block the
#      copy (payload copies below overwrite everything else; no delete-then-fail). ----
$stale = Join-Path $GameDir "SentinelUI_Worker.exe"
if (Test-Path $stale) { Remove-Item -Force $stale }
# Refresh content only - logs/cache belong to past sessions, and deleting the
# whole folder used to abort mid-way when the game still held the CEF cache open.
$contentDir = Join-Path $suiDir "content"
if (Test-Path $contentDir) { Remove-Item -Recurse -Force $contentDir }
New-Item -ItemType Directory -Force -Path $contentDir, (Join-Path $suiDir "cache"), (Join-Path $suiDir "logs") | Out-Null
Write-Host "[pack] copy phase (overwrites existing root files)..." -ForegroundColor DarkGray

# ---- host module (ASI compositor) ----
Copy-Item (Join-Path $repo "native\host\build\SentinelUI.asi") $GameDir

# ---- helper process + full CEF runtime (flat at game root) ----
$helperOut = Join-Path $repo "native\helper\build\Release"
if (-not (Test-Path (Join-Path $helperOut "SentinelUI_Host.exe"))) { throw "Helper not built (run tools\build-helper.ps1)." }
Get-ChildItem $helperOut | Where-Object { $_.Name -notin @("SentinelUI.lib", "SentinelUI.exp", "SentinelUI.pdb") } |
    Copy-Item -Destination $GameDir -Recurse -Force

# ---- content + license ----
Copy-Item (Join-Path $repo "web\content\p1_demo.html") (Join-Path $suiDir "content")
Copy-Item (Join-Path $repo "web\content\p1_demo.js") (Join-Path $suiDir "content")
Copy-Item (Join-Path $repo "web\content\demo_hud.html") (Join-Path $suiDir "content")
Copy-Item (Join-Path $repo "web\content\demo_hud.js") (Join-Path $suiDir "content")
Copy-Item (Join-Path $repo "web\content\selftest_page.html") (Join-Path $suiDir "content")
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