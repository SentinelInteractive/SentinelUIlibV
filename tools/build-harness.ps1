# build-harness.ps1: configure + build the CEF OSR validation harness with the MSVC toolchain.
# Resolves the VS install (vswhere) and the pinned CEF distribution, then drives Ninja.
# Internal-only: the OSR validation harness sources are not part of this repo.

param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# Locate VS toolchain.
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found (Visual Studio Installer missing)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio with the C++ workload found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$ninjaDir = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs" }
if (-not (Test-Path (Join-Path $ninjaDir "ninja.exe"))) { throw "ninja.exe not found under $ninjaDir" }
# Explicit MSVC linker path (guards against MinGW on PATH being picked up by CMake).
$linkExe = Get-ChildItem (Join-Path $vs "VC\Tools\MSVC\*\bin\Hostx64\x64\link.exe") -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $linkExe) { throw "link.exe not found under VC\Tools\MSVC (C++ workload missing?)" }
$linkPath = $linkExe.FullName

# A stale/invalid build dir poisons compiler+linker detection; always start clean.
$buildDir = Join-Path $repo "native\harness\build"
Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue

# Locate the pinned CEF distribution.
$stateFile = Join-Path $repo "native\third_party\cef-current.json"
$cef = $null
if (Test-Path $stateFile) {
    $cef = (Get-Content $stateFile -Raw | ConvertFrom-Json).path
}
if (-not $cef -or -not (Test-Path (Join-Path $cef "cmake"))) {
    $candidates = Get-ChildItem (Join-Path $repo "native\third_party\cef") -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName "cmake") }
    if ($candidates) { $cef = ($candidates | Select-Object -First 1).FullName }
}
if (-not $cef) { throw "No extracted CEF distribution found (run tools\init-cef.ps1 first)." }
# CEF's CMake macros require forward slashes (backslashes break their quoted literals).
$cef = $cef -replace '\\', '/'
Write-Host "[build-harness] CEF: $cef" -ForegroundColor Cyan

# Emit a .cmd wrapper (robust quoting for spaces) and run it.
$cmdBody = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
set "CEF_ROOT=$cef"
set "PATH=$ninjaDir;%PATH%"
where cl >nul
if errorlevel 1 exit /b 1
cmake -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DCMAKE_LINKER=$linkPath" -B "$buildDir" -S "$repo\native\harness"
if errorlevel 1 exit /b 1
cmake --build "$buildDir"
exit /b %errorlevel%
"@
$tmp = Join-Path $env:TEMP "sentinelui_build_harness.cmd"
$cmdBody | Set-Content -Path $tmp -Encoding ASCII
try {
    cmd /c $tmp
    if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}
Write-Host "[build-harness] DONE." -ForegroundColor Green