# build-hoststub.ps1 — compile the fake host (SentinelUI.asi) used by the managed probe.
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
$src = Join-Path $repo "native\hoststub\hoststub.c"
$outDir = Join-Path $repo "native\hoststub\build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$out = Join-Path $outDir "SentinelUI.asi"

$cmd = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /LD "$src" /Fe:"$out" /link
exit /b %errorlevel%
"@
$tmp = Join-Path $env:TEMP "sentinelui_build_hoststub.cmd"
$cmd | Set-Content -Path $tmp -Encoding ASCII
try {
    cmd /c $tmp
    if ($LASTEXITCODE -ne 0) { throw "cl failed with exit code $LASTEXITCODE" }
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}
Write-Host "[build-hoststub] OK: $out" -ForegroundColor Green