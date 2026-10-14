# PSScriptAnalyzer settings: https://learn.microsoft.com/powershell/utility-modules/psscriptanalyzer/overview
# Run: Invoke-ScriptAnalyzer -Path tools -Settings ./PSScriptAnalyzerSettings.psd1

@{
    Severity = @('Error', 'Warning')

    ExcludeRules = @(
        # The tools/ scripts are interactive console frontends; Write-Host is the
        # intended output, not a logging bug.
        'PSAvoidUsingWriteHost'
        # Internal helpers run unattended; there is no -WhatIf/-Confirm contract to honor.
        'PSUseShouldProcessForStateChangingFunctions'
        # Names describe batches of items (ids, leftovers, results); the singular reads wrong.
        'PSUseSingularNouns'
    )
}
