[CmdletBinding()]
param(
    [string]$Goal = "",
    [string]$DeviceTestSummary = "",
    [string[]]$ScopePath = @(),
    [int]$MaxAutoFixAttempts = 2,
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$stateDir = Join-Path $repoRoot ".git\streamu-review"
$statePath = Join-Path $stateDir "last-review.json"

function Invoke-JsonCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Command
    )

    $raw = & $Command[0] $Command[1..($Command.Length - 1)]
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed: $($Command -join ' ')"
    }
    return $raw | ConvertFrom-Json
}

function New-ReviewTaskText {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Inspection,
        [Parameter(Mandatory = $true)]
        [string]$GoalText,
        [Parameter(Mandatory = $true)]
        [string]$DeviceSummary
    )

    $scopeLines = @()
    foreach ($changedFile in $Inspection.changed_files) {
        $scopeLines += "- $changedFile"
    }

    return @"
Review the current local working tree before remote sync.

Goal:
$GoalText

Device Test:
$DeviceSummary

Scope:
$($scopeLines -join "`n")

Raise only release-blocking issues.
If a required fix is purely mechanical and safe, classify it as AUTO_FIXABLE.
"@
}

function New-AutoFixTaskText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$GoalText,
        [Parameter(Mandatory = $true)]
        [string]$DeviceSummary,
        [Parameter(Mandatory = $true)]
        [string]$BaseSha,
        [Parameter(Mandatory = $true)]
        [string]$ReportPath
    )

    $reportText = Get-Content -Path $ReportPath -Raw -Encoding UTF8
    return @"
# Local Auto Fix Task
Base SHA: $BaseSha

Goal:
$GoalText

Device Test:
$DeviceSummary

Apply only the explicitly listed AUTO_FIXABLE findings below.
Do not add features, change user-visible behavior, or modify release metadata.

$reportText
"@
}

function Get-InspectCommand {
    $command = @(
        "py", "-3", ".github/scripts/github_flow.py",
        "inspect-local",
        "--repo-root", $repoRoot
    )
    foreach ($path in $ScopePath) {
        $command += "--scope-path"
        $command += $path
    }
    return ,$command
}

Push-Location $repoRoot
try {
    if ([string]::IsNullOrWhiteSpace($DeviceTestSummary)) {
        throw "DeviceTestSummary is required."
    }

    Get-Command tact -ErrorAction Stop | Out-Null

    $inspection = Invoke-JsonCommand -Command (Get-InspectCommand)

    if ($null -eq $inspection.changed_files -or $inspection.changed_files.Count -eq 0) {
        throw "No local changes found to review."
    }

    if ([string]::IsNullOrWhiteSpace($Goal)) {
        $Goal = "Review the current working tree changes on branch $($inspection.branch) before remote sync."
    }

    $autoFixAttempts = 0
    while ($true) {
        $localCheckCommand = @(
            "py", "-3", ".github/scripts/github_flow.py",
            "run-local-checks",
            "--repo-root", $repoRoot
        )
        foreach ($path in $ScopePath) {
            $localCheckCommand += "--scope-path"
            $localCheckCommand += $path
        }
        if ($SkipBuild) {
            $localCheckCommand += "--skip-build"
        }
        Invoke-JsonCommand -Command $localCheckCommand | Out-Null

        $inspection = Invoke-JsonCommand -Command (Get-InspectCommand)

        $reviewTask = New-ReviewTaskText -Inspection $inspection -GoalText $Goal -DeviceSummary $DeviceTestSummary
        $taktWorkflow = ".github/takt/workflows/$($inspection.selected_workflow)-ci.yaml"
        & tact --pipeline --skip-git --provider codex --model gpt-5.4 --workflow $taktWorkflow --task $reviewTask --quiet
        if ($LASTEXITCODE -ne 0) {
            throw "TAKT review failed."
        }

        $reviewResult = Invoke-JsonCommand -Command @(
            "py", "-3", ".github/scripts/github_flow.py",
            "parse-review-report",
            "--runs-root", ".takt/runs"
        )

        if ($reviewResult.decision -eq "AUTO_FIXABLE" -and $autoFixAttempts -lt $MaxAutoFixAttempts) {
            $baseSha = (git rev-parse HEAD).Trim()
            $autoFixTask = New-AutoFixTaskText `
                -GoalText $Goal `
                -DeviceSummary $DeviceTestSummary `
                -BaseSha $baseSha `
                -ReportPath $reviewResult.report_path
            & tact --pipeline --skip-git --provider codex --model gpt-5.4 --workflow ".github/takt/workflows/auto-fix-ci.yaml" --task $autoFixTask --quiet
            if ($LASTEXITCODE -ne 0) {
                throw "TAKT auto-fix failed."
            }
            $autoFixAttempts += 1
            continue
        }

        if (-not (Test-Path -LiteralPath $stateDir)) {
            New-Item -ItemType Directory -Path $stateDir | Out-Null
        }

        $state = [ordered]@{
            saved_at = (Get-Date).ToString("o")
            branch = $inspection.branch
            goal = $Goal
            device_test_summary = $DeviceTestSummary
            scope_paths = @($ScopePath)
            selected_workflow = $inspection.selected_workflow
            changed_files = @($inspection.changed_files)
            auto_fix_attempts = $autoFixAttempts
            decision = $reviewResult.decision
            summary = $reviewResult.summary
            user_decision_needed = [bool]$reviewResult.user_decision_needed
            user_prompt = $reviewResult.user_prompt
            report_path = $reviewResult.report_path
        }
        $state | ConvertTo-Json -Depth 5 | Set-Content -Path $statePath -Encoding UTF8

        if ($reviewResult.decision -eq "APPROVE") {
            Write-Output "Review approved. State saved to $statePath"
            break
        }
        if ($reviewResult.decision -eq "AUTO_FIXABLE") {
            throw "Review remained AUTO_FIXABLE after $autoFixAttempts auto-fix attempt(s)."
        }
        if ($reviewResult.decision -eq "NEEDS_DECISION") {
            throw "Review paused for decision: $($reviewResult.user_prompt)"
        }
        throw "Review failed with decision $($reviewResult.decision): $($reviewResult.summary)"
    }
}
finally {
    Pop-Location
}
