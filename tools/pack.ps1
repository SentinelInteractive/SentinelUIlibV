# pack.ps1: assemble and deploy the SentinelUI runtime into a GTA V install.
#
# Canonical layout (ASI + CEF runtime live at the GAME ROOT: ASI loaders and the
# CEF delay-load helper both resolve from there; only the content cache/logs live
# in a subfolder, resolved by absolute path in host code).
#
# LOAD-ORDER CONSTRAINT (2026-09-11): the ASI must stay in the game root so
# ScriptHookV loads it at process start, BEFORE RPH installs its Present hook.
# Moving it into SentinelUI\ and cold-loading from Bootstrap made RPH's hook
# guard revert our Present hook and crash the D3D chain (AV in dxgi!Present).
# Do not relocate without replacing the Present hook with an RPH-native path.
#
#   <game>\                          (GTA V root)
#     SentinelUI.asi                 ASI compositor: hooks Present/WndProc (SHV-loaded)
#     SentinelUI_Host.exe            dedicated CEF process (spawned by the ASI)
#     libcef.dll *.pak icudtl.dat ... full CEF runtime (flat, next to the ASI)
#     locales\                       CEF locale packs
#     SentinelUI.API.dll             managed bridge (also copied to Plugins\)
#     SentinelUI\
#       content\p1_demo.html         overlay demo page (+ selftest_page.html)
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
    [string]$ManagedConfig = "Debug"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if (-not (Test-Path $GameDir)) { throw "Game dir not found: $GameDir" }
if (Get-Process -Name GTA5 -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited }) {
    throw "GTA5 is running - close the game before deploying (runtime files and the CEF cache are locked)."
}
$hostOut = Join-Path $repo "native\host\build"
if (-not (Test-Path (Join-Path $hostOut "SentinelUI.asi"))) { throw "Host not built (run tools\build-host.ps1)." }

$cefState = Get-Content (Join-Path $repo "native\third_party\cef-current.json") -Raw | ConvertFrom-Json
$cefRoot = $cefState.path

$suiDir = Join-Path $GameDir "SentinelUI"
$pluginsDir = Join-Path $GameDir "Plugins"

Write-Host "[pack] Deploying to $GameDir" -ForegroundColor Cyan

# Failure-safe pre-step: remove only files we no longer ship or that block the
# copy (payload copies below overwrite everything else; no delete-then-fail).
$stale = Join-Path $GameDir "SentinelUI_Worker.exe"
if (Test-Path $stale) { Remove-Item -Force $stale }
# Refresh content only: logs/cache belong to past sessions, and deleting the
# whole folder used to abort mid-way when the game still held the CEF cache open.
$contentDir = Join-Path $suiDir "content"
if (Test-Path $contentDir) { Remove-Item -Recurse -Force $contentDir }
New-Item -ItemType Directory -Force -Path $contentDir, (Join-Path $suiDir "cache"), (Join-Path $suiDir "logs") | Out-Null
Write-Host "[pack] copy phase (overwrites existing root files)..." -ForegroundColor DarkGray

# Host module (ASI compositor): must be at the root for SHV (see header).
Copy-Item (Join-Path $repo "native\host\build\SentinelUI.asi") $GameDir

# Helper process + full CEF runtime (flat at game root).
$helperOut = Join-Path $repo "native\helper\build\Release"
if (-not (Test-Path (Join-Path $helperOut "SentinelUI_Host.exe"))) { throw "Helper not built (run tools\build-helper.ps1)." }

# Guard: the failed 2026-09-11 subfolder experiment left a second runtime in
# $suiDir. Bootstrap.ResolveHostPath probes <game>\SentinelUI\SentinelUI.asi
# first, so a stale copy there would be loaded next to the SHV-loaded root ASI
# (double hooks/helper). Remove any leftovers before deploying.
if (Test-Path (Join-Path $suiDir "SentinelUI.asi")) {
    Write-Host "[pack] removing stale subfolder runtime (experiment leftovers in $suiDir)" -ForegroundColor Yellow
    foreach ($name in @("SentinelUI.asi", "SentinelUI_Host.exe")) {
        Remove-Item -Force -LiteralPath (Join-Path $suiDir $name) -ErrorAction SilentlyContinue
    }
    Get-ChildItem $helperOut -File | ForEach-Object {
        Remove-Item -Force -LiteralPath (Join-Path $suiDir $_.Name) -ErrorAction SilentlyContinue
    }
    Remove-Item -Recurse -Force -LiteralPath (Join-Path $suiDir "locales") -ErrorAction SilentlyContinue
}

Get-ChildItem $helperOut | Where-Object { $_.Name -notin @("SentinelUI.lib", "SentinelUI.exp", "SentinelUI.pdb") } |
    Copy-Item -Destination $GameDir -Recurse -Force

# Content + license.
Copy-Item (Join-Path $repo "web\content\p1_demo.html") (Join-Path $suiDir "content")
Copy-Item (Join-Path $repo "web\content\p1_demo.js") (Join-Path $suiDir "content")
Copy-Item (Join-Path $repo "web\content\badge.html") (Join-Path $suiDir "content")
Copy-Item (Join-Path $repo "web\content\badge.js") (Join-Path $suiDir "content")
Copy-Item (Join-Path $repo "web\content\demo_hud.html") (Join-Path $suiDir "content")
Copy-Item (Join-Path $repo "web\content\demo_hud.js") (Join-Path $suiDir "content")
Copy-Item (Join-Path $repo "web\content\selftest_page.html") (Join-Path $suiDir "content")
$cefLicense = Join-Path $cefRoot "LICENSE.txt"
if (Test-Path $cefLicense) { Copy-Item $cefLicense (Join-Path $suiDir "LICENSE_CEF.txt") } else { Write-Host "[pack] WARN cef LICENSE.txt not found" -ForegroundColor Yellow }

# Managed plugin (RPH auto-load).
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
Write-Host "Revert: remove the SentinelUI.* / libcef.dll / locales files from the game root," -ForegroundColor Yellow
Write-Host "        the SentinelUI\ folder, and Plugins\SentinelUI.Bootstrap.dll + SentinelUI.API.dll" -ForegroundColor Yellow
