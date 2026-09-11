# scenario-lib.ps1 — in-session scenario library for the SentinelUI P1/P2 stress
# matrix. NO param() block on purpose: this file is dot-sourced by stress.ps1 and
# by scenario.ps1, and a param block would clobber the caller's variables
# ($Scenario / $DryRun) when dot-sourced.
#
# Public functions: Get-GtaProcess, Test-ProcessRunning, Get-GtaWindowHandle,
# Assert-Foreground, Focus-Gta, Write-Marker, Send-Tap, Send-Combo,
# Close-GameWindow, Request-GameQuit, Invoke-Scenario.
#
# Scenarios: LayerChurn | CaptureAltTab | Resize | DeviceLost | GameBar | HudTicks | QuitGame | All
#    LayerChurn    F11/F9 churn (layer create/show/hide, compositing transitions)
#    CaptureAltTab F10 capture on -> alt-tab away/back -> focus must auto-release
#    Resize        Alt+Enter fullscreen round-trip (ResizeBuffers + viewport sync)
#    DeviceLost    Win+Ctrl+Shift+B GPU driver reset (DXGI device-lost recovery)
#    GameBar       Win+Alt+R Game Bar capture start/stop
#    HudTicks      F11 demo HUD left open (30-min RSS soak feed)
#    QuitGame      Alt+F4 + quit-confirmation handling (also unsticks a stuck game)
#    All           LayerChurn, CaptureAltTab, Resize, DeviceLost, GameBar

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not ("SuiInput" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SuiInput {
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public InputUnion U; }

    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion {
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public MOUSEINPUT mi;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT {
        public ushort wVk; public ushort wScan; public uint dwFlags;
        public uint time; public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx; public int dy; public uint mouseData; public uint dwFlags;
        public uint time; public IntPtr dwExtraInfo;
    }

    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

    [DllImport("user32.dll")] public static extern IntPtr GetMessageExtraInfo();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, IntPtr dwExtraInfo);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

    public const uint INPUT_KEYBOARD = 1;
    public const uint INPUT_MOUSE = 0;
    public const uint KEYEVENTF_EXTENDEDKEY = 1;
    public const uint KEYEVENTF_KEYUP = 2;
    public const uint WM_CLOSE = 0x0010;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;

    // Software click at absolute screen coordinates (injected left down/up).
    public static void ClickAt(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(80);
        INPUT[] a = new INPUT[2];
        a[0].type = INPUT_MOUSE;
        a[0].U.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        a[1].type = INPUT_MOUSE;
        a[1].U.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, a, Marshal.SizeOf(typeof(INPUT)));
    }

    // SetForegroundWindow is blocked for background processes (our harness is one).
    // AttachThreadInput + an Alt nudge are the standard workaround; retry a few
    // times because the window manager can be slow right after game boot.
    public static bool ForceForeground(IntPtr hWnd) {
        if (hWnd == IntPtr.Zero) return false;
        for (int attempt = 0; attempt < 5; attempt++) {
            if (GetForegroundWindow() == hWnd) return true;
            ShowWindow(hWnd, 9);   // SW_RESTORE
            BringWindowToTop(hWnd);
            uint fgPid, targetPid;
            uint fgThread = GetWindowThreadProcessId(GetForegroundWindow(), out fgPid);
            uint targetThread = GetWindowThreadProcessId(hWnd, out targetPid);
            uint thisThread = GetCurrentThreadId();
            bool a1 = false, a2 = false;
            try {
                if (fgThread != 0 && fgThread != thisThread) a1 = AttachThreadInput(thisThread, fgThread, true);
                if (targetThread != 0 && targetThread != thisThread) a2 = AttachThreadInput(thisThread, targetThread, true);
                keybd_event(0x12, 0, 0, IntPtr.Zero);                  // Alt down
                SetForegroundWindow(hWnd);
                keybd_event(0x12, 0, KEYEVENTF_KEYUP, IntPtr.Zero);    // Alt up
            } finally {
                if (a2) AttachThreadInput(thisThread, targetThread, false);
                if (a1) AttachThreadInput(thisThread, fgThread, false);
            }
            System.Threading.Thread.Sleep(250);
        }
        return GetForegroundWindow() == hWnd;
    }

    private static void One(ushort vk, bool up, bool extended) {
        INPUT[] a = new INPUT[1];
        a[0].type = INPUT_KEYBOARD;
        a[0].U.ki.wVk = vk;
        a[0].U.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        a[0].U.ki.dwExtraInfo = GetMessageExtraInfo();
        SendInput(1, a, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void KeyDown(ushort vk, bool extended) { One(vk, false, extended); }
    public static void KeyUp(ushort vk, bool extended) { One(vk, true, extended); }
    public static void Tap(ushort vk, bool extended) {
        One(vk, false, extended);
        System.Threading.Thread.Sleep(30);
        One(vk, true, extended);
    }
}
'@
}

# Virtual-key codes used by the scenarios.
$script:VK = @{
    F9       = 0x78
    F10      = 0x79
    F11      = 0x7A
    ENTER    = 0x0D
    TAB      = 0x09
    ALT      = 0x12
    CONTROL  = 0x11
    SHIFT    = 0x10
    LWIN     = 0x5B
    B        = 0x42
    R        = 0x52
}

function Test-ProcessRunning {
    param([int]$Id)
    # Get-Process -Id can fail (or lie) for terminating/protected game processes;
    # enumerating and filtering by HasExited is reliable for GTA5/RPH.
    return [bool](Get-Process -ErrorAction SilentlyContinue |
                  Where-Object { -not $_.HasExited -and $_.Id -eq $Id })
}

function Get-GtaProcess {
    param([int]$Id = 0)
    if ($Id -gt 0) {
        $p = Get-Process -Id $Id -ErrorAction SilentlyContinue
        if (-not $p) { throw "Process $Id not found." }
        return $p
    }
    $p = Get-Process -Name GTA5 -ErrorAction SilentlyContinue |
         Where-Object { -not $_.HasExited } | Select-Object -First 1
    if (-not $p) { throw "GTA5.exe is not running." }
    return $p
}

function Get-GtaWindowHandle {
    param($Process)
    # Process.MainWindowHandle is unreliable when the game is higher-integrity
    # or the window class changed; the game window class is stable ("grcWindow").
    $h = [SuiInput]::FindWindow("grcWindow", $null)
    if ($h -eq [IntPtr]::Zero) { $h = $Process.MainWindowHandle }
    if ($h -eq [IntPtr]::Zero) {
        throw "GTA window not found (process may still be loading)."
    }
    return $h
}

function Assert-Foreground {
    param($Process)
    $gta = Get-GtaWindowHandle -Process $Process
    $fg = [SuiInput]::GetForegroundWindow()
    if ($fg -ne $gta) {
        throw "GTA window is not foreground (foreground=$fg, gta=$gta). Aborting to avoid injecting input elsewhere."
    }
}

function Focus-Gta {
    param($Process)
    $gta = Get-GtaWindowHandle -Process $Process
    if ([SuiInput]::ForceForeground($gta)) { return }
    throw "Could not bring the GTA window to the foreground."
}

function Write-Marker {
    param($Process, [string]$Text)
    try {
        $gameDir = Split-Path -Parent $Process.Path
        $dir = Join-Path $gameDir "SentinelUI\logs"
        if (-not (Test-Path $dir)) { [void](New-Item -ItemType Directory -Path $dir -Force) }
        $line = "[{0:yyyy-MM-dd HH:mm:ss.fff}] {1}`r`n" -f (Get-Date), $Text
        [System.IO.File]::AppendAllText((Join-Path $dir "stress_markers.log"), $line)
    } catch {
        Write-Warning "marker write failed: $_"
    }
}

# RPH launches GTA5 with custom arguments; Steam always asks for confirmation
# ("Launch Game with custom arguments" -> Continue). The dialog is an in-client
# CEF overlay (no separate window, UIA sees nothing), so detect the solid blue
# Continue button by pixel scan in the middle band of the Steam window and
# click its center. Returns $true when a click was issued.
function Approve-SteamLaunchDialog {
    $sw = Get-Process -Name "steamwebhelper" -ErrorAction SilentlyContinue |
          Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
    if (-not $sw) {
        $sw = Get-Process -Name "steam" -ErrorAction SilentlyContinue |
              Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
    }
    if (-not $sw) { return $false }
    $h = $sw.MainWindowHandle
    $r = New-Object SuiInput+RECT
    if (-not [SuiInput]::GetWindowRect($h, [ref]$r)) { return $false }
    $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
    if ($w -lt 500 -or $ht -lt 350) { return $false }

    Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue
    $bmp = New-Object System.Drawing.Bitmap($w, $ht)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
    $g.Dispose()
    $minX = [int]::MaxValue; $minY = [int]::MaxValue; $maxX = -1; $maxY = -1; $count = 0
    for ($y = [int]($ht * 0.5); $y -lt [int]($ht * 0.9); $y += 4) {
        for ($x = [int]($w * 0.4); $x -lt [int]($w * 0.9); $x += 4) {
            $c = $bmp.GetPixel($x, $y)
            if ($c.B -gt 170 -and ($c.B - $c.R) -gt 40 -and $c.G -gt 90) {
                $count++
                if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    $bmp.Dispose()
    $bw = $maxX - $minX; $bh = $maxY - $minY
    # Button-shaped blob only (avoids clicking a large blue banner).
    if ($count -lt 30 -or $bw -lt 60 -or $bw -gt 400 -or $bh -gt 120) { return $false }
    [SuiInput]::ClickAt($r.Left + [int](($minX + $maxX) / 2), $r.Top + [int](($minY + $maxY) / 2))
    return $true
}

function Send-Tap {
    param([int]$Vk, [switch]$Extended)
    [SuiInput]::Tap([ushort]$Vk, [bool]$Extended)
}

function Send-Combo {
    param(
        [int[]]$Modifiers,
        [int]$Vk,
        [int[]]$ExtendedModifiers = @()
    )
    $held = @()
    foreach ($m in $Modifiers) {
        $ext = $ExtendedModifiers -contains $m
        [SuiInput]::KeyDown([ushort]$m, [bool]$ext)
        $held += , @($m, $ext)
        Start-Sleep -Milliseconds 30
    }
    Start-Sleep -Milliseconds 50
    Send-Tap -Vk $Vk
    Start-Sleep -Milliseconds 50
    for ($i = $held.Count - 1; $i -ge 0; $i--) {
        [SuiInput]::KeyUp([ushort]$held[$i][0], [bool]$held[$i][1])
        Start-Sleep -Milliseconds 30
    }
}

function Close-GameWindow {
    param($Process)
    $h = [IntPtr]::Zero
    try { $h = Get-GtaWindowHandle -Process $Process } catch { }
    if ($h -ne [IntPtr]::Zero) {
        [void][SuiInput]::PostMessage($h, [SuiInput]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
        return $true
    }
    # Graceful fallback: taskkill without /F posts WM_CLOSE.
    & taskkill.exe /PID $Process.Id 2>$null | Out-Null
    return $false
}

# GTA V does not exit on Alt+F4 alone: it shows an "are you sure you want to
# quit?" prompt. Send the exit chord, then repeat the confirm key until the
# process disappears. Keys are only sent while the GTA window is verifiably
# foreground, so this can never type into another application.
function Request-GameQuit {
    param(
        $Process,
        [string]$QuitKey = "Enter",
        [int]$TimeoutSeconds = 45,
        [switch]$ForceKill
    )
    $keyCode = switch ($QuitKey) { "Enter" { 0x0D } "Y" { 0x59 } "Space" { 0x20 } }
    $nudges = 0
    $sentExitChord = $false
    $handle = [IntPtr]::Zero
    try { $handle = Get-GtaWindowHandle -Process $Process } catch { }

    if ($handle -ne [IntPtr]::Zero) {
        if ([SuiInput]::ForceForeground($handle)) {
            Send-Combo -Modifiers @($script:VK.ALT) -Vk 0x73   # Alt+F4
            $sentExitChord = $true
        }
    }
    if (-not $sentExitChord) {
        # No foreground window we can trust: ask the OS to close it instead.
        try { Close-GameWindow -Process $Process | Out-Null } catch { }
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 1
        if (-not (Test-ProcessRunning -Id $Process.Id)) {
            return [pscustomobject]@{ Exited = $true; Forced = $false; Nudges = $nudges }
        }
        # Keep confirming the quit prompt until it takes. Re-check foreground
        # every time; never inject keys into whatever else is focused.
        try {
            $h = [IntPtr]::Zero
            try { $h = Get-GtaWindowHandle -Process $Process } catch { }
            if ($h -ne [IntPtr]::Zero) {
                if ([SuiInput]::ForceForeground($h)) {
                    Send-Tap -Vk $keyCode
                    $nudges++
                }
            }
        } catch { }
    }

    if (Test-ProcessRunning -Id $Process.Id) {
        if ($ForceKill) {
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
            if (Test-ProcessRunning -Id $Process.Id) {
                & taskkill.exe /F /PID $Process.Id 2>$null | Out-Null
            }
            Start-Sleep -Seconds 2
            $still = Test-ProcessRunning -Id $Process.Id
            return [pscustomobject]@{ Exited = (-not $still); Forced = $true; Nudges = $nudges }
        }
        return [pscustomobject]@{ Exited = $false; Forced = $false; Nudges = $nudges }
    }
    return [pscustomobject]@{ Exited = $true; Forced = $false; Nudges = $nudges }
}

function Invoke-Scenario {
    param(
        [Parameter(Mandatory)][ValidateSet("LayerChurn", "CaptureAltTab", "Resize", "DeviceLost", "GameBar", "HudTicks", "QuitGame", "All")]
        [string]$Name,
        [Parameter(Mandatory)]$Process,
        [switch]$DryRun
    )

    $steps = New-Object System.Collections.Generic.List[object]
    switch ($Name) {
        "LayerChurn" {
            # 5 churn cycles: F11 create/show, F9 hide, F9 show, F9 hide, F9 show.
            for ($i = 1; $i -le 5; $i++) {
                $steps.Add(@{ Label = "F11 (cycle $i)"; Vk = $script:VK.F11 })
                $steps.Add(@{ Label = "F9 hide"; Vk = $script:VK.F9 })
                $steps.Add(@{ Label = "F9 show"; Vk = $script:VK.F9 })
            }
        }
        "CaptureAltTab" {
            # Capture on -> focus loss must auto-release (new behavior) -> return.
            $steps.Add(@{ Label = "F10 capture on"; Vk = $script:VK.F10 })
            $steps.Add(@{ Label = "Alt+Tab away"; Combo = @($script:VK.ALT); Vk = $script:VK.TAB; Refocus = $false })
            $steps.Add(@{ Label = "Alt+Tab back"; Combo = @($script:VK.ALT); Vk = $script:VK.TAB; Refocus = $true })
        }
        "Resize" {
            $steps.Add(@{ Label = "Alt+Enter fullscreen/windowed"; Combo = @($script:VK.ALT); Vk = $script:VK.ENTER; Post = 3 })
            $steps.Add(@{ Label = "Alt+Enter back"; Combo = @($script:VK.ALT); Vk = $script:VK.ENTER; Post = 3 })
        }
        "DeviceLost" {
            $steps.Add(@{ Label = "F9 show before reset"; Vk = $script:VK.F9 })
            $steps.Add(@{ Label = "Win+Ctrl+Shift+B GPU driver reset"; Combo = @($script:VK.LWIN, $script:VK.CONTROL, $script:VK.SHIFT); Vk = $script:VK.B; ExtendedCombo = @($script:VK.LWIN); Post = 8 })
            $steps.Add(@{ Label = "F9 hide (force recomposite)"; Vk = $script:VK.F9 })
            $steps.Add(@{ Label = "F9 show"; Vk = $script:VK.F9; Post = 3 })
        }
        "GameBar" {
            $steps.Add(@{ Label = "Win+Alt+R start recording"; Combo = @($script:VK.LWIN, $script:VK.ALT); Vk = $script:VK.R; ExtendedCombo = @($script:VK.LWIN); Post = 12 })
            $steps.Add(@{ Label = "Win+Alt+R stop recording"; Combo = @($script:VK.LWIN, $script:VK.ALT); Vk = $script:VK.R; ExtendedCombo = @($script:VK.LWIN); Post = 3 })
        }
        "HudTicks" {
            $steps.Add(@{ Label = "F11 demo HUD open"; Vk = $script:VK.F11 })
        }
        "QuitGame" {
            if ($DryRun) {
                Write-Host "  (dry-run) would close GTA (Alt+F4 + confirm key until exit)"
                return
            }
            $r = Request-GameQuit -Process $Process -TimeoutSeconds 60
            Write-Host ("[scenario:QuitGame] exited={0} forced={1} nudges={2}" -f $r.Exited, $r.Forced, $r.Nudges)
            return
        }
        "All" {
            Invoke-Scenario -Name LayerChurn -Process $Process -DryRun:$DryRun
            Invoke-Scenario -Name CaptureAltTab -Process $Process -DryRun:$DryRun
            Invoke-Scenario -Name Resize -Process $Process -DryRun:$DryRun
            Invoke-Scenario -Name DeviceLost -Process $Process -DryRun:$DryRun
            Invoke-Scenario -Name GameBar -Process $Process -DryRun:$DryRun
            return
        }
    }

    Write-Host "[scenario:$Name] $($steps.Count) step(s)" -ForegroundColor Cyan
    foreach ($step in $steps) {
        if ($DryRun) {
            Write-Host ("  (dry-run) " + $step['Label'])
            continue
        }
        try { Focus-Gta -Process $Process } catch { }
        Assert-Foreground -Process $Process
        Write-Marker -Process $Process -Text "scenario=$Name step=$($step['Label'])"
        Write-Host ("  -> " + $step['Label'])
        if ($step['Combo']) {
            $extMods = @()
            if ($step['ExtendedCombo']) { $extMods = $step['ExtendedCombo'] }
            Send-Combo -Modifiers $step['Combo'] -Vk $step['Vk'] -ExtendedModifiers $extMods
        } else {
            Send-Tap -Vk $step['Vk']
        }
        if ($step['Refocus'] -eq $true) {
            Start-Sleep -Milliseconds 800
            Focus-Gta -Process $Process
        } elseif ($step['Refocus'] -eq $false) {
            Start-Sleep -Seconds 1
        } else {
            Start-Sleep -Milliseconds 400
        }
        $post = 0
        if ($step['Post']) { $post = [int]$step['Post'] }
        if ($post -gt 0) { Start-Sleep -Seconds $post }
    }
    Write-Marker -Process $Process -Text "scenario=$Name done"
    Write-Host "[scenario:$Name] done" -ForegroundColor Green
}
