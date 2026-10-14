# init-cef.ps1: download + extract a pinned CEF distribution for the SentinelUI native host.
#
# Usage:
#   .\init-cef.ps1 -CefVersion "144.0.34+g8fc21c8+chromium-144.0.7559.261"
#   .\init-cef.ps1 -CefVersion "151.3.24+g2384915+chromium-151.0.7922.174"
#
# Behavior:
#   * Resolves the standard-distribution download URL from the live index
#     (https://cef-builds.spotifycdn.com/index.json) and verifies SHA-1.
#   * Downloads to <repo>/native/third_party/_downloads/, extracts to
#     <repo>/native/third_party/cef/<CefVersion>/  (tar.bz2 via bsdtar).
#   * Writes <repo>/native/third_party/cef-current.json (the path the build scripts read).
#   * Does not modify version.json (that pin is updated deliberately once a build validates the new CEF).

# The CEF build index publishes SHA-1 checksums only; verifying them is the
# point of this script, so the broken-algorithm warning does not apply here.
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingBrokenHashAlgorithms', '', Justification = 'The CEF build index publishes SHA-1 checksums only.')]
param(
    [Parameter(Mandatory = $true)]
    [string]$CefVersion,

    [switch]$Force
)

$ErrorActionPreference = "Stop"
# Windows PowerShell 5.1 defaults to TLS 1.0, which modern CDNs refuse.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$downloads = Join-Path $repoRoot "native\third_party\_downloads"
$cefRoot = Join-Path $repoRoot "native\third_party\cef"
$outDir = Join-Path $cefRoot $CefVersion
# Local state file read by the native build scripts; the tracked pin is the
# "cef" entry in the repo-root version.json.
$stateFile = Join-Path (Join-Path $repoRoot "native\third_party") "cef-current.json"

$dbg = { param($m) Write-Host "[init-cef] $m" -ForegroundColor Cyan }

if (Test-Path (Join-Path $outDir "include")) {
    & $dbg "Already extracted: $outDir"
    if (-not $Force) {
        & $dbg "Found in-place; nothing to do. ($(Join-Path $outDir 'include') exists)"
        exit 0
    }
}

# 1. Resolve the standard distribution file entry from the live index.
& $dbg "Querying cef-builds index for $CefVersion (windows64, standard)..."
$index = ((Invoke-WebRequest -Uri "https://cef-builds.spotifycdn.com/index.json" -TimeoutSec 90).Content | ConvertFrom-Json)
$entry = $index.windows64.versions | Where-Object { $_.cef_version -eq $CefVersion } | Select-Object -First 1
if (-not $entry) { throw "CEF version '$CefVersion' not found in the windows64 index." }
$std = $entry.files | Where-Object { $_.type -eq "standard" } | Select-Object -First 1
if (-not $std) { throw "No 'standard' distribution for '$CefVersion'." }

$url = "https://cef-builds.spotifycdn.com/" + $std.name
$expectedSha1 = $std.sha1
$localArchive = Join-Path $downloads $std.name
New-Item -ItemType Directory -Force -Path $downloads | Out-Null

# 2. Download (skip if present and SHA-1 matches).
if ((Test-Path $localArchive) -and -not $Force) {
    $hash = (Get-FileHash -Algorithm SHA1 -Path $localArchive).Hash.ToLowerInvariant()
    if ($hash -eq $expectedSha1) {
        & $dbg "Archive already downloaded and verified."
    } else {
        & $dbg "Archive present but SHA-1 mismatch (got $hash, want $expectedSha1); re-downloading."
        Remove-Item $localArchive -Force
    }
}
if (-not (Test-Path $localArchive)) {
    & $dbg "Downloading $($std.name) ($([math]::Round($std.size/1MB,0)) MB)..."
    Invoke-WebRequest -Uri $url -OutFile $localArchive -TimeoutSec 3600
    $hash = (Get-FileHash -Algorithm SHA1 -Path $localArchive).Hash.ToLowerInvariant()
    if ($hash -ne $expectedSha1) { throw "SHA-1 verification failed after download." }
    & $dbg "Download verified (sha1 $hash)."
}

# 3. Extract into native/third_party/cef/<version>.
& $dbg "Extracting to $outDir ..."
New-Item -ItemType Directory -Force -Path $cefRoot | Out-Null
tar -xjf $localArchive -C $cefRoot
if ($LASTEXITCODE -ne 0) { throw "tar extraction failed with exit code $LASTEXITCODE" }

$extracted = Get-ChildItem $cefRoot -Directory | Where-Object { $_.Name -like "cef_binary_$CefVersion*" } | Select-Object -First 1
if (-not $extracted) { throw "Extraction produced no cef_binary_* folder (check name)." }
if ($extracted.FullName -ne $outDir) {
    & $dbg "Renaming $($extracted.Name) -> $CefVersion"
    Rename-Item $extracted.FullName $CefVersion
}

# 4. Write the state file.
$state = [ordered]@{
    cefVersion      = $CefVersion
    chromiumVersion = $entry.chromium_version
    channel         = $entry.channel
    standardFile    = $std.name
    sha1            = $expectedSha1
    path            = $outDir
    extractedAt     = (Get-Date).ToString("o")
}
$state | ConvertTo-Json | Set-Content -Path $stateFile -Encoding UTF8
& $dbg "Done. State:"
Get-Content $stateFile
exit 0