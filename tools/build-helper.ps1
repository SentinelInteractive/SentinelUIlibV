# build-helper.ps1: configure + build SentinelUI_Host.exe (dedicated CEF process).
# Mirrors tools/build-host.ps1 (vswhere -> vcvars64 -> Ninja + MSVC).

param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio with the C++ workload found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$ninjaDir = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$linkExe = Get-ChildItem (Join-Path $vs "VC\Tools\MSVC\*\bin\Hostx64\x64\link.exe") -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
$linkPath = $linkExe.FullName

$stateFile = Join-Path $repo "native\third_party\cef-current.json"
$cef = (Get-Content $stateFile -Raw | ConvertFrom-Json).path
if (-not $cef -or -not (Test-Path (Join-Path $cef "cmake"))) { throw "CEF not extracted (run tools\init-cef.ps1)." }
$cef = $cef -replace '\\', '/'

$buildDir = Join-Path $repo "native\helper\build"
Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue

$cmd = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
set "CEF_ROOT=$cef"
set "PATH=$ninjaDir;%PATH%"
cmake -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DCMAKE_LINKER=$linkPath" -DUSE_SANDBOX=OFF -B "$buildDir" -S "$repo\native\helper"
if errorlevel 1 exit /b 1
cmake --build "$buildDir"
exit /b %errorlevel%
"@
$tmp = Join-Path $env:TEMP "sentinelui_build_helper.cmd"
$cmd | Set-Content -Path $tmp -Encoding ASCII
try {
    cmd /c $tmp
    if ($LASTEXITCODE -ne 0) { throw "helper build failed with exit code $LASTEXITCODE" }
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}
Write-Host "[build-helper] DONE. Output at $buildDir\$Config" -ForegroundColor Green