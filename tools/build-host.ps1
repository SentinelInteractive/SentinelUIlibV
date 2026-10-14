# build-host.ps1: build the in-game ASI compositor (SentinelUI.asi).
# vswhere -> vcvars64 -> Ninja + MSVC. No CEF in this module anymore.

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

$buildDir = Join-Path $repo "native\host\build"
Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue

$cmd = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
set "PATH=$ninjaDir;%PATH%"
cmake -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DCMAKE_LINKER=$linkPath" -B "$buildDir" -S "$repo\native\host"
if errorlevel 1 exit /b 1
cmake --build "$buildDir"
exit /b %errorlevel%
"@
$tmp = Join-Path $env:TEMP "sentinelui_build_host.cmd"
$cmd | Set-Content -Path $tmp -Encoding ASCII
try {
    cmd /c $tmp
    if ($LASTEXITCODE -ne 0) { throw "host build failed with exit code $LASTEXITCODE" }
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}
Write-Host "[build-host] DONE. Output at $buildDir\$Config" -ForegroundColor Green