# scenario.ps1 — CLI wrapper for single in-session scenarios.
#
#   pwsh -File tools\scenario.ps1 -Scenario CaptureAltTab
#   pwsh -File tools\scenario.ps1 -Scenario QuitGame        # close a running game
#
# The scenario implementations live in scenario-lib.ps1 (shared with
# tools\stress.ps1). Input is injected with SendInput only while the GTA window
# is verified foreground, so a scenario can never type into another application.

[CmdletBinding()]
param(
    [ValidateSet("LayerChurn", "CaptureAltTab", "Resize", "DeviceLost", "GameBar", "HudTicks", "QuitGame", "All")]
    [string]$Scenario = "All",
    [int]$ProcessId = 0,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "scenario-lib.ps1")

try {
    $proc = Get-GtaProcess -Id $ProcessId
    Invoke-Scenario -Name $Scenario -Process $proc -DryRun:$DryRun
    exit 0
} catch {
    Write-Error $_
    exit 1
}
