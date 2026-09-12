# deploy.ps1 — build + install the SuiSample callout into GTA V.
param(
    [string]$GameDir = "D:\SteamLibrary\steamapps\common\Grand Theft Auto V",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $GameDir)) { throw "Game dir not found: $GameDir" }
if (Get-Process -Name GTA5 -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited }) {
    throw "GTA5 is running - close the game before deploying the callout."
}

if (-not $SkipBuild) {
    Write-Host "[deploy] building..." -ForegroundColor DarkGray
    dotnet build (Join-Path $PSScriptRoot "SuiSample.csproj") -c $Config -p:GameDir="$GameDir" -v quiet --nologo
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$out = Join-Path $PSScriptRoot "bin\$Config"
$target = Join-Path $GameDir "Plugins\LSPDFR"
New-Item -ItemType Directory -Force -Path $target | Out-Null

foreach ($file in @("SuiSample.dll")) {
    $src = Join-Path $out $file
    if (-not (Test-Path $src)) { throw "missing build output: $src" }
    Copy-Item $src $target -Force
    Write-Host "[deploy] $file -> $target"
}

# The API resolves from the game root (SentinelUI.API.dll) via ApiResolver; keep the
# LSPDFR folder clean so LSPDFR does not scan the API assembly as a callout candidate.
$stale = Join-Path $target "SentinelUI.API.dll"
if (Test-Path $stale) { Remove-Item -Force $stale; Write-Host "[deploy] removed stale SentinelUI.API.dll from $target" }

Write-Host "[deploy] DONE. In-game: go on duty, then F4 console -> ForceCallout SuiSample" -ForegroundColor Green
Write-Host "Uninstall: remove SuiSample.dll from $target" -ForegroundColor Yellow
