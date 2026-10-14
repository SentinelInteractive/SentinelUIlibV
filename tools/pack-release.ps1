# pack-release.ps1: assemble the deploy-ready release zip (dist/SentinelUI-<Tag>.zip).
# Paths mirror tools\pack.ps1: binaries + CEF runtime flat, content under SentinelUI\, plugins under Plugins\.

param(
    [Parameter(Mandatory = $true)]
    [string]$Tag,

    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if ($Tag -notmatch '^[A-Za-z0-9._-]+$') { throw "Tag is not usable as a folder name: '$Tag'" }

$outRoot = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $repo $OutDir }
$stage = Join-Path $outRoot "SentinelUI-$Tag"
$zip = Join-Path $outRoot "SentinelUI-$Tag.zip"

# Wipe staging so a rerun can never pick up files from an earlier tag.
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
Remove-Item -Force $zip -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$asi = Join-Path $repo "native\host\build\SentinelUI.asi"
if (-not (Test-Path $asi)) { throw "Host not built (run tools\build-host.ps1): $asi" }
Copy-Item $asi $stage

# Helper exe + full CEF runtime, flat.
$helperOut = Join-Path $repo "native\helper\build\Release"
if (-not (Test-Path (Join-Path $helperOut "SentinelUI_Host.exe"))) { throw "Helper not built (run tools\build-helper.ps1): $helperOut" }
if (-not (Test-Path (Join-Path $helperOut "locales"))) { throw "CEF runtime incomplete (no locales folder): $helperOut" }
Get-ChildItem $helperOut | Where-Object { $_.Extension -notin @(".lib", ".exp", ".pdb") } |
    Copy-Item -Destination $stage -Recurse -Force

$contentOut = Join-Path $stage "SentinelUI\content"
New-Item -ItemType Directory -Force -Path $contentOut | Out-Null
foreach ($name in @("p1_demo.html", "p1_demo.js", "demo_hud.html", "demo_hud.js", "badge.html", "badge.js", "selftest_page.html")) {
    $src = Join-Path $repo "web\content\$name"
    if (-not (Test-Path $src)) { throw "Missing content file: $src" }
    Copy-Item $src $contentOut -Force
}

$pluginsOut = Join-Path $stage "Plugins"
New-Item -ItemType Directory -Force -Path $pluginsOut | Out-Null
$managedOut = Join-Path $repo "managed\SentinelUI.Bootstrap\bin\Release"
foreach ($name in @("SentinelUI.Bootstrap.dll", "SentinelUI.API.dll")) {
    $src = Join-Path $managedOut $name
    if (-not (Test-Path $src)) { throw "Missing managed output: $src" }
    Copy-Item $src $pluginsOut -Force
}

$cefStateFile = Join-Path $repo "native\third_party\cef-current.json"
if (-not (Test-Path $cefStateFile)) { throw "CEF state file not found (run tools\init-cef.ps1): $cefStateFile" }
$cefRoot = (Get-Content $cefStateFile -Raw | ConvertFrom-Json).path
$cefLicense = Join-Path $cefRoot "LICENSE.txt"
if (-not (Test-Path $cefLicense)) { throw "Missing CEF license: $cefLicense" }
Copy-Item $cefLicense (Join-Path $stage "LICENSE_CEF.txt")

foreach ($name in @("LICENSE", "README.md")) {
    $src = Join-Path $repo $name
    if (-not (Test-Path $src)) { throw "Missing release document: $src" }
    Copy-Item $src $stage -Force
}

Compress-Archive -Path $stage -DestinationPath $zip -Force
Write-Host "[pack-release] $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)" -ForegroundColor Green
