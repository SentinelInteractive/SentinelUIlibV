# stress.ps1: SentinelUI session stress harness (acceptance matrix).
#
# Runs N full game sessions unattended: launch -> wait for host -> optional
# scenario -> sample RSS/handles -> graceful quit -> orphan/dump/log checks.
# Produces tools\stress-results\stress-<timestamp>.csv + .json and a summary.
#
# Usage:
#   pwsh -File tools\stress.ps1 -Sessions 20 -Scenario HudTicks -HoldSeconds 45
#   pwsh -File tools\stress.ps1 -Sessions 1 -Scenario CaptureAltTab -HoldSeconds 20
#   pwsh -File tools\stress.ps1 -DryRun
#
# Safety: the game is closed with WM_CLOSE only. It is never force-killed unless
# -ForceKillOnTimeout is passed, and a stuck session stops the run (so a broken
# build cannot burn through 20 launches).

[CmdletBinding()]
param(
    [string]$GameDir = "D:\SteamLibrary\steamapps\common\Grand Theft Auto V",
    [ValidateSet("RPH", "Steam", "Direct")]
    [string]$Launch = "RPH",
    [int]$Sessions = 20,
    [int]$HoldSeconds = 45,
    [ValidateSet("None", "LayerChurn", "CaptureAltTab", "Resize", "DeviceLost", "GameBar", "HudTicks")]
    [string]$Scenario = "HudTicks",
    [int]$ReadyTimeoutSeconds = 300,
    [int]$ExitTimeoutSeconds = 45,
    [int]$OrphanTimeoutSeconds = 45,
    [int]$SampleSeconds = 10,
    [int]$MaxConsecutiveFailures = 3,
    [ValidateSet("Enter", "Y", "Space")]
    [string]$QuitKey = "Enter",
    [ValidateSet("Kill", "Manual", "Confirm")]
    [string]$QuitMode = "Kill",
    [int]$ManualQuitTimeoutSeconds = 300,
    [switch]$ForceKillOnTimeout,
    [switch]$CleanupOrphans,
    [switch]$SkipStoryModeEnter,
    [switch]$AllowNoBootstrap,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "scenario-lib.ps1")

$SteamAppId = 271590
$HostLogDir = Join-Path $GameDir "SentinelUI\logs"
$ResultsDir = Join-Path $PSScriptRoot "stress-results"
$script:LastCefIds = @()

function Get-HostLogPath {
    Join-Path $HostLogDir ("sentinelui_host_{0}.log" -f (Get-Date -Format "yyyyMMdd"))
}

function Get-FileLengthSafe {
    param([string]$Path)
    try {
        if (Test-Path -LiteralPath $Path) { return (Get-Item -LiteralPath $Path).Length }
    } catch { Write-Verbose "Get-FileLengthSafe: cannot read length yet: $_" }
    return [long]0
}

function Read-LogDelta {
    param([string]$Path, [long]$FromOffset)
    for ($attempt = 0; $attempt -lt 5; $attempt++) {
        try {
            if (-not (Test-Path -LiteralPath $Path)) { return "" }
            $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
                                         [System.IO.FileAccess]::Read,
                                         [System.IO.FileShare]::ReadWrite)
            try {
                $start = $FromOffset
                if ($fs.Length -lt $FromOffset) { $start = 0 }  # log was rewritten/rotated
                if ($fs.Length -le $start) { return "" }
                [void]$fs.Seek($start, [System.IO.SeekOrigin]::Begin)
                $len = [int]($fs.Length - $start)
                $buf = New-Object byte[] $len
                [void]$fs.Read($buf, 0, $len)
                return [System.Text.Encoding]::UTF8.GetString($buf)
            } finally { $fs.Dispose() }
        } catch {
            Start-Sleep -Milliseconds 250
        }
    }
    return ""
}

function Wait-LogDelta {
    param(
        [string]$Path,
        [long]$FromOffset,
        [string]$Pattern,
        [int]$TimeoutSeconds,
        [string]$What
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $text = Read-LogDelta -Path $Path -FromOffset $FromOffset
        if ($text -match $Pattern) { return $text }
        Start-Sleep -Seconds 1
    }
    Write-Verbose "timed out after ${TimeoutSeconds}s waiting for: $What"
    return $null
}

function Get-ProcessSnapshot {
    Get-CimInstance -ClassName Win32_Process -ErrorAction SilentlyContinue |
        Select-Object ProcessId, ParentProcessId, Name
}

function Get-DescendantIds {
    param([int[]]$RootIds, $Snapshot)
    $children = @{}
    foreach ($p in $Snapshot) {
        $ppid = [int]$p.ParentProcessId
        if (-not $children.ContainsKey($ppid)) {
            $children[$ppid] = New-Object System.Collections.Generic.List[int]
        }
        $children[$ppid].Add([int]$p.ProcessId)
    }
    $result = New-Object System.Collections.Generic.List[int]
    $queue = New-Object System.Collections.Generic.Queue[int]
    foreach ($r in $RootIds) { $queue.Enqueue($r) }
    while ($queue.Count -gt 0) {
        $cur = $queue.Dequeue()
        if ($children.ContainsKey($cur)) {
            foreach ($c in $children[$cur]) {
                $result.Add($c)
                $queue.Enqueue($c)
            }
        }
    }
    return $result
}

function Get-RunSample {
    $snap = Get-ProcessSnapshot
    $procs = Get-Process -ErrorAction SilentlyContinue
    $gta = $procs | Where-Object { $_.Name -eq "GTA5" -and -not $_.HasExited } | Select-Object -First 1
    $helper = $procs | Where-Object { $_.Name -eq "SentinelUI_Host" -and -not $_.HasExited } | Select-Object -First 1

    $cefIds = @()
    if ($helper) { $cefIds = @(Get-DescendantIds -RootIds @([int]$helper.Id) -Snapshot $snap) }
    $script:LastCefIds = $cefIds

    $cefRss = 0
    $cefCount = 0
    foreach ($id in $cefIds) {
        $cp = $procs | Where-Object { $_.Id -eq $id } | Select-Object -First 1
        if ($cp) {
            $cefRss += $cp.WorkingSet64
            $cefCount++
        }
    }
    return [pscustomobject]@{
        Time         = Get-Date
        GtaPid       = $(if ($gta) { $gta.Id } else { 0 })
        GtaRssMB     = $(if ($gta) { [math]::Round($gta.WorkingSet64 / 1MB, 1) } else { 0 })
        GtaHandles   = $(if ($gta) { $gta.HandleCount } else { 0 })
        HelperPid    = $(if ($helper) { $helper.Id } else { 0 })
        HelperRssMB  = $(if ($helper) { [math]::Round($helper.WorkingSet64 / 1MB, 1) } else { 0 })
        CefCount     = $cefCount
        CefRssMB     = [math]::Round($cefRss / 1MB, 1)
    }
}

# NOTE: Close-GameWindow / Request-GameQuit live in scenario.ps1 (dot-sourced
# above) so they can also be used standalone to unstick a running game:
#   pwsh -File tools\scenario.ps1 -Scenario QuitGame

function Stop-SentinelLeftovers {
    $helper = @(Get-Process -Name "SentinelUI_Host" -ErrorAction SilentlyContinue |
                Where-Object { -not $_.HasExited })
    if ($helper.Count -eq 0) { return 0 }
    $snap = Get-ProcessSnapshot
    $ids = New-Object System.Collections.Generic.List[int]
    foreach ($h in $helper) {
        $ids.Add([int]$h.Id)
        foreach ($d in @(Get-DescendantIds -RootIds @([int]$h.Id) -Snapshot $snap)) { $ids.Add([int]$d) }
    }
    foreach ($id in $ids) {
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
    }
    return $ids.Count
}

function Get-DumpCount {
    if (-not (Test-Path -LiteralPath $HostLogDir)) { return 0 }
    return @(Get-ChildItem -LiteralPath $HostLogDir -Filter "*.dmp" -ErrorAction SilentlyContinue).Count
}

function Export-Results {
    param($Sessions, [string]$Stamp)
    [void](New-Item -ItemType Directory -Path $ResultsDir -Force)
    $csv = Join-Path $ResultsDir ("stress-{0}.csv" -f $Stamp)
    $json = Join-Path $ResultsDir ("stress-{0}.json" -f $Stamp)
    $Sessions | Export-Csv -LiteralPath $csv -NoTypeInformation -Encoding UTF8
    $Sessions | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $json -Encoding UTF8
    return @{ Csv = $csv; Json = $json }
}

# Preflight

$gameExe = Join-Path $GameDir "GTA5.exe"
$hostAsi = Join-Path $GameDir "SentinelUI.asi"
$helperExe = Join-Path $GameDir "SentinelUI_Host.exe"
$rphExe = Join-Path $GameDir "RagePluginHook.exe"
$rphLog = Join-Path $GameDir "RagePluginHook.log"

$preflightErrors = New-Object System.Collections.Generic.List[string]
if (-not (Test-Path -LiteralPath $GameDir)) { $preflightErrors.Add("Game dir not found: $GameDir") }
if (-not (Test-Path -LiteralPath $gameExe)) { $preflightErrors.Add("GTA5.exe not found in $GameDir") }
if (-not (Test-Path -LiteralPath $hostAsi)) { $preflightErrors.Add("SentinelUI.asi not deployed in $GameDir") }
if (-not (Test-Path -LiteralPath $helperExe)) { $preflightErrors.Add("SentinelUI_Host.exe not deployed in $GameDir") }
if ($Launch -eq "RPH" -and -not (Test-Path -LiteralPath $rphExe)) {
    $preflightErrors.Add("RagePluginHook.exe not found; use -Launch Steam/Direct (but then Bootstrap may not load).")
}
if ($Launch -eq "Steam" -and -not (Get-Process -Name "steam" -ErrorAction SilentlyContinue)) {
    $preflightErrors.Add("Steam is not running (required for -Launch Steam).")
}
# A terminating zombie entry (HasExited=true) must not block the run; only a
# live game does. ScriptHookV auto-loads the root ASI even without RPH (that is
# required for hook order: our Present hook must exist before RPH's). Sessions
# where Bootstrap never loads are overlay-only and failed below unless
# -AllowNoBootstrap is passed.
$runningGame = @(Get-Process -Name "GTA5" -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited })
if ($runningGame.Count -gt 0) {
    $preflightErrors.Add("GTA5 is already running. Close it before starting the stress run.")
}
$leftover = @(Get-Process -Name "SentinelUI_Host" -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited })
if ($leftover.Count -gt 0) {
    if ($CleanupOrphans) {
        $killed = Stop-SentinelLeftovers
        Write-Host "[preflight] cleaned $killed leftover SentinelUI/CEF process(es)." -ForegroundColor Yellow
    } else {
        $preflightErrors.Add("$($leftover.Count) leftover SentinelUI/CEF process(es) (helper browser + children). Re-run with -CleanupOrphans or kill them first.")
    }
}

if ($preflightErrors.Count -gt 0) {
    foreach ($e in $preflightErrors) { Write-Host "[preflight] $e" -ForegroundColor Red }
    throw "Preflight failed; nothing launched."
}

Write-Host "[preflight] OK" -ForegroundColor Green
# A terminated-but-unreaped GTA5 entry means Steam/RGL may still consider the
# game running and silently refuse to start a new session (observed 2026-09-10:
# RPH launch produced no game process while a zombie entry existed). Not fatal -
# warn so a failed launch is understood instead of waited out.
$zombieGame = @(Get-Process -Name "GTA5" -ErrorAction SilentlyContinue | Where-Object { $_.HasExited })
if ($zombieGame.Count -gt 0) {
    Write-Host "[preflight] warning: a terminated GTA5 process entry is still registered. Steam/RGL may" -ForegroundColor Yellow
    Write-Host "           treat the game as running and refuse to launch; restart Steam/RGL if sessions hang." -ForegroundColor Yellow
}
Write-Host ("  game    : {0}" -f $GameDir)
Write-Host ("  launch  : {0}" -f $Launch)
Write-Host ("  sessions: {0} x {1}s hold, scenario={2}" -f $Sessions, $HoldSeconds, $Scenario)
Write-Host ("  results : {0}" -f $ResultsDir)

if ($DryRun) {
    Write-Host "[dry-run] would launch/close the game $Sessions time(s). No actions taken." -ForegroundColor Cyan
    exit 0
}

# Session loop

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$sessionResults = New-Object System.Collections.Generic.List[object]
$consecutiveFailures = 0

for ($index = 1; $index -le $Sessions; $index++) {
    Write-Host ""
    Write-Host ("=== session {0}/{1} ===" -f $index, $Sessions) -ForegroundColor Cyan

    $hostLog = Get-HostLogPath
    $logOffset = Get-FileLengthSafe -Path $hostLog
    $rphOffset = Get-FileLengthSafe -Path $rphLog
    $dumpsBefore = Get-DumpCount
    $started = Get-Date
    $notes = New-Object System.Collections.Generic.List[string]

    $result = [pscustomobject]@{
        Index           = $index
        StartedAt       = $started.ToString("s")
        QuitMode        = $QuitMode
        ReadySeconds    = $null
        CleanExit       = $false
        Forced          = $false
        Orphans         = 0
        DumpsAdded      = 0
        GtaRssStartMB   = $null
        GtaRssEndMB     = $null
        HelperRssEndMB  = $null
        CefRssEndMB     = $null
        RssGrowthMB     = $null
        ErrorsInLog     = 0
        DeviceLost      = 0
        KilledFocus     = 0
        CaptureToggles  = 0
        RphBootstrapOk  = $false
        RphUnloadOk     = $false
        Pass            = $false
        Notes           = ""
    }

    try {
        # 1. Launch. RPH is the default. ScriptHookV loads SentinelUI.asi from the
        # game root at process start (required: our Present hook must precede
        # RPH's); Bootstrap then attaches and owns shutdown. Launching without RPH
        # yields a headless host with no Bootstrap; session invalid unless
        # -AllowNoBootstrap opts into overlay-only checks.
        switch ($Launch) {
            "RPH" {
                Start-Process -FilePath $rphExe -WorkingDirectory $GameDir | Out-Null
            }
            "Steam" {
                Start-Process "steam://rungameid/$SteamAppId" | Out-Null
            }
            "Direct" {
                Start-Process -FilePath $gameExe -WorkingDirectory $GameDir | Out-Null
            }
        }

        # 2. Wait for the game process (RPH -> RGL -> game can take >2 minutes).
        # Steam pops a "Launch Game with custom arguments" confirmation for
        # RPH's launch; approve it automatically while waiting.
        $gta = $null
        $procDeadline = (Get-Date).AddSeconds(300)
        $lastDialogCheck = [datetime]::MinValue
        while ((Get-Date) -lt $procDeadline) {
            if (((Get-Date) - $lastDialogCheck).TotalSeconds -ge 5) {
                $lastDialogCheck = Get-Date
                if (Approve-SteamLaunchDialog) {
                    Write-Host "  Steam launch dialog approved"
                }
            }
            $gta = Get-Process -Name "GTA5" -ErrorAction SilentlyContinue |
                   Where-Object { -not $_.HasExited } | Select-Object -First 1
            if ($gta) { break }
            Start-Sleep -Seconds 2
        }
        if (-not $gta) {
            $z = @(Get-Process -Name "GTA5" -ErrorAction SilentlyContinue | Where-Object { $_.HasExited })
            $hint = $(if ($z.Count -gt 0) {
                " A terminated GTA5 entry is still registered - restart Steam/RGL and retry."
            } else { "" })
            throw "GTA5.exe did not start within 300s.$hint"
        }

        # 3. Wait for the host (SHV auto-loads the ASI as soon as the game process
        # starts, so this alone does NOT prove RPH/Bootstrap are up).
        $ready = Wait-LogDelta -Path $hostLog -FromOffset $logOffset -Pattern "Host ready" `
                               -TimeoutSeconds $ReadyTimeoutSeconds -What "Host ready"
        if (-not $ready) { throw "Host never reached 'Host ready' within ${ReadyTimeoutSeconds}s." }
        $result.ReadySeconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
        Write-Host ("  host ready after {0}s" -f $result.ReadySeconds)

        # 3a. RPH rewrites RagePluginHook.log on launch; wait for the plugin to
        # load there. NOTE: RPH loads plugins when the game session starts
        # (story mode) - that needs real input, which GTA ignores from automation.
        # Without Bootstrap there is no managed bridge and no graceful shutdown;
        # such sessions are only valid for overlay/lifecycle checks, which is what
        # -AllowNoBootstrap opts into.
        if ($AllowNoBootstrap) {
            Write-Host "  bootstrap wait skipped (-AllowNoBootstrap): headless-host session" -ForegroundColor Yellow
        } else {
            $bootstrap = Wait-LogDelta -Path $rphLog -FromOffset $rphOffset `
                                       -Pattern "\[SentinelUI\] Bootstrap ready" `
                                       -TimeoutSeconds $ReadyTimeoutSeconds -What "Bootstrap ready"
            if (-not $bootstrap) {
                throw "RPH Bootstrap never loaded within ${ReadyTimeoutSeconds}s (start story mode; or use -AllowNoBootstrap for overlay-only sessions)."
            }
            $result.RphBootstrapOk = $true
            Write-Host "  bootstrap ready"
        }

        # 3b. GTA lands on the story-mode menu; Enter starts the last used mode.
        # If your install auto-loads story mode, pass -SkipStoryModeEnter.
        if (-not $SkipStoryModeEnter) {
            Start-Sleep -Seconds 3
            try {
                Set-GtaForeground -Process $gta
                Send-Tap -Vk 0x0D
                Start-Sleep -Seconds 3
                Send-Tap -Vk 0x0D
                Write-Host "  story-mode key sent"
            } catch {
                Write-Host "  story-mode key skipped (could not foreground)" -ForegroundColor Yellow
            }
            Start-Sleep -Seconds 15
        }

        # 4. Settle, then baseline sample.
        Start-Sleep -Seconds 10
        $sample = Get-RunSample
        $result.GtaRssStartMB = $sample.GtaRssMB
        Write-Host ("  baseline: GTA {0} MB, helper {1} MB, CEF {2} proc / {3} MB" -f `
                    $sample.GtaRssMB, $sample.HelperRssMB, $sample.CefCount, $sample.CefRssMB)

        # 5. Scenario (optional).
        if ($Scenario -ne "None") {
            Invoke-Scenario -Name $Scenario -Process $gta
        }

        # 6. Hold + sample.
        $holdDeadline = (Get-Date).AddSeconds($HoldSeconds)
        while ((Get-Date) -lt $holdDeadline) {
            $sample = Get-RunSample
            Start-Sleep -Seconds $SampleSeconds
        }
        $sample = Get-RunSample
        $result.GtaRssEndMB = $sample.GtaRssMB
        $result.HelperRssEndMB = $sample.HelperRssMB
        $result.CefRssEndMB = $sample.CefRssMB
        $result.RssGrowthMB = [math]::Round($sample.GtaRssMB - $result.GtaRssStartMB, 1)

        # 7. Quit. GTA V's own UI ignores synthetic input (its input path is
        # DirectInput/RawInput; our overlay hotkeys work only because our hook
        # reads window messages directly). Pick a mode:
        #   Kill    - terminate the process; verifies the helper pipe-down guard
        #   Manual  - wait for the operator to close the game (clean-shutdown check)
        #   Confirm - best-effort Alt+F4 + confirm keys (only if injected input works)
        switch ($QuitMode) {
            "Kill" {
                Stop-Process -Id $gta.Id -Force -ErrorAction SilentlyContinue
                Start-Sleep -Seconds 1
                if (Test-ProcessRunning -Id $gta.Id) {
                    & taskkill.exe /F /PID $gta.Id 2>$null | Out-Null
                }
                $result.Forced = $true
                $notes.Add("force-killed (QuitMode=Kill)")
            }
            "Manual" {
                Write-Host "  QuitMode=Manual: close GTA V now (Alt+F4 + confirm); waiting..." -ForegroundColor Yellow
                $manualDeadline = (Get-Date).AddSeconds($ManualQuitTimeoutSeconds)
                while ((Get-Date) -lt $manualDeadline -and (Test-ProcessRunning -Id $gta.Id)) {
                    Start-Sleep -Seconds 3
                }
                if (Test-ProcessRunning -Id $gta.Id) {
                    throw "Game not closed within ${ManualQuitTimeoutSeconds}s (QuitMode=Manual)."
                }
                $result.CleanExit = $true
            }
            "Confirm" {
                # Release capture first if the scenario left it on (captured F4 is
                # swallowed by the page).
                $preQuitDelta = Read-LogDelta -Path $hostLog -FromOffset $logOffset
                $captureMatches = [regex]::Matches($preQuitDelta, "Capture -> (on|off)")
                if ($captureMatches.Count -gt 0 -and $captureMatches[$captureMatches.Count - 1].Groups[1].Value -eq "on") {
                    Send-Tap -Vk $script:VK.F10
                    Start-Sleep -Milliseconds 400
                    $notes.Add("capture released before quit")
                }
                $quit = Request-GameQuit -Process $gta -QuitKey $QuitKey `
                                         -TimeoutSeconds $ExitTimeoutSeconds -ForceKill:$ForceKillOnTimeout
                if ($quit.Forced) {
                    $result.Forced = $true
                    $notes.Add("game force-killed after quit timeout")
                }
                if (-not $quit.Exited) {
                    throw "GTA5 did not exit within ${ExitTimeoutSeconds}s (use -ForceKillOnTimeout to kill)."
                }
                if ($quit.Nudges -gt 0) { $notes.Add("quit confirmed after $($quit.Nudges) key nudge(s)") }
                $result.CleanExit = $true
            }
        }

        # 8. Orphan check (helper + the CEF children captured in the last sample).
        $orphanDeadline = (Get-Date).AddSeconds($OrphanTimeoutSeconds)
        $alive = @()
        while ((Get-Date) -lt $orphanDeadline) {
            $alive = @()
            $h = Get-Process -Name "SentinelUI_Host" -ErrorAction SilentlyContinue |
                 Where-Object { -not $_.HasExited }
            if ($h) { $alive += $h.Id }
            foreach ($id in $script:LastCefIds) {
                if (Get-Process -Id $id -ErrorAction SilentlyContinue) { $alive += $id }
            }
            if ($alive.Count -eq 0) { break }
            Start-Sleep -Seconds 2
        }
        $result.Orphans = $alive.Count
        if ($alive.Count -gt 0) { $notes.Add("orphans: $($alive -join ',')") }

        # 9. Log evidence.
        $hostDelta = Read-LogDelta -Path $hostLog -FromOffset $logOffset
        $result.ErrorsInLog = ([regex]::Matches($hostDelta, "\[ERROR")).Count
        $result.DeviceLost = ([regex]::Matches($hostDelta, "device lost")).Count
        $result.KilledFocus = ([regex]::Matches($hostDelta, "WM_KILLFOCUS")).Count
        $result.CaptureToggles = ([regex]::Matches($hostDelta, "Capture ->")).Count
        $rphDelta = Read-LogDelta -Path $rphLog -FromOffset $rphOffset
        $result.RphBootstrapOk = ($rphDelta -match "\[SentinelUI\] Bootstrap ready")
        $result.RphUnloadOk = ($rphDelta -match "\[SentinelUI\] Bootstrap unloaded")

        # 10. Dumps.
        $result.DumpsAdded = (Get-DumpCount) - $dumpsBefore
        if ($result.DumpsAdded -gt 0) { $notes.Add("$($result.DumpsAdded) new dump(s)") }

        $quitVerified = $(if ($QuitMode -eq "Kill") {
            $true   # orphan cleanup is the acceptance check for the kill path
        } else {
            $result.CleanExit -and $result.RphUnloadOk
        })
        $bootstrapOk = $(if ($AllowNoBootstrap) { $true } else { $result.RphBootstrapOk })
        $result.Pass = ($bootstrapOk -and $quitVerified -and
                        $result.Orphans -eq 0 -and $result.DumpsAdded -eq 0)
    } catch {
        $notes.Add("FAILED: $_")
        # Best effort: close the game we launched so the next session can start.
        try {
            $g = Get-Process -Name "GTA5" -ErrorAction SilentlyContinue |
                 Where-Object { -not $_.HasExited } | Select-Object -First 1
            if ($g) {
                Close-GameWindow -Process $g | Out-Null
                Start-Sleep -Seconds 5
            }
        } catch { Write-Verbose "cleanup close failed: $_" }
    }

    $result.Notes = ($notes -join "; ")
    $sessionResults.Add($result)

    $color = $(if ($result.Pass) { "Green" } else { "Red" })
    Write-Host ("  result: pass={0} cleanExit={1} orphans={2} dumps+={3} rss {4}->{5} MB" -f `
                $result.Pass, $result.CleanExit, $result.Orphans, $result.DumpsAdded,
                $result.GtaRssStartMB, $result.GtaRssEndMB) -ForegroundColor $color

    if ($result.Pass) {
        $consecutiveFailures = 0
    } else {
        $consecutiveFailures++
        if ($consecutiveFailures -ge $MaxConsecutiveFailures) {
            Write-Host "[stress] $consecutiveFailures consecutive failures; stopping the run." -ForegroundColor Red
            break
        }
    }
}

# Summary

$exported = Export-Results -Sessions $sessionResults -Stamp $stamp
$passed = @($sessionResults | Where-Object { $_.Pass }).Count
$total = $sessionResults.Count

Write-Host ""
Write-Host "=== summary ===" -ForegroundColor Cyan
$sessionResults | Format-Table Index, ReadySeconds, CleanExit, Forced, Orphans, DumpsAdded,
    GtaRssStartMB, GtaRssEndMB, HelperRssEndMB, CefRssEndMB, RssGrowthMB, Pass -AutoSize

if ($total -gt 0) {
    Write-Host ("pass {0}/{1}; RSS end-to-end {2} -> {3} MB" -f `
                $passed, $total, $sessionResults[0].GtaRssEndMB, $sessionResults[$total - 1].GtaRssEndMB)
}
Write-Host ("csv : {0}" -f $exported.Csv)
Write-Host ("json: {0}" -f $exported.Json)

if ($passed -eq $total -and $total -eq $Sessions) {
    Write-Host "STRESS RESULT: PASS" -ForegroundColor Green
    exit 0
} else {
    Write-Host "STRESS RESULT: FAIL" -ForegroundColor Red
    exit 1
}
