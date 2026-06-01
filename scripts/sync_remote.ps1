[CmdletBinding()]
param(
    [string]$BaseBranch = "main",
    [string]$CommitMessage = "chore: sync reviewed changes",
    [string]$PullRequestTitle = "",
    [switch]$ReleaseRequested
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$statePath = Join-Path $repoRoot ".git\streamu-review\last-review.json"

function Get-RepoState {
    if (-not (Test-Path -LiteralPath $statePath)) {
        throw "Review state not found. Run scripts/review_local.ps1 first."
    }
    return Get-Content -Path $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Save-RepoState {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$State
    )

    $State | ConvertTo-Json -Depth 6 | Set-Content -Path $statePath -Encoding UTF8
}

function Get-PublishableFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Paths
    )

    $pathsJson = $Paths | ConvertTo-Json -Compress
    $result = & py -3 .github/scripts/github_flow.py classify-publishable --paths-json $pathsJson
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to classify publishable files."
    }
    return $result | ConvertFrom-Json
}

function Find-OrCreatePullRequest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Branch,
        [Parameter(Mandatory = $true)]
        [string]$TargetBase,
        [Parameter(Mandatory = $true)]
        [string]$Title,
        [Parameter(Mandatory = $true)]
        [string]$BodyFile
    )

    $existing = gh pr list --head $Branch --base $TargetBase --json number,url --limit 1 | ConvertFrom-Json
    if ($existing.Count -gt 0) {
        $prNumber = [int]$existing[0].number
        gh pr edit $prNumber --title $Title --body-file $BodyFile | Out-Null
    }
    else {
        gh pr create --base $TargetBase --head $Branch --title $Title --body-file $BodyFile | Out-Null
    }

    $resolvedPullRequest = gh pr list --head $Branch --base $TargetBase --json number,url --limit 1 | ConvertFrom-Json
    if ($resolvedPullRequest.Count -eq 0) {
        throw "Pull request could not be resolved for branch $Branch."
    }
    return $resolvedPullRequest[0]
}

function Wait-ForCiRun {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Branch,
        [Parameter(Mandatory = $true)]
        [string]$HeadSha
    )

    for ($index = 0; $index -lt 30; $index += 1) {
        $runs = gh run list --workflow "PR CI" --branch $Branch --json databaseId,headSha,status,conclusion --limit 10 | ConvertFrom-Json
        foreach ($run in $runs) {
            if ($run.headSha -eq $HeadSha) {
                gh run watch $run.databaseId --exit-status
                return $run.databaseId
            }
        }
        Start-Sleep -Seconds 3
    }

    throw "Timed out waiting for PR CI run for $HeadSha."
}

Push-Location $repoRoot
try {
    $state = Get-RepoState
    if ($state.decision -ne "APPROVE") {
        throw "Latest local review decision is $($state.decision). Sync is allowed only after APPROVE."
    }

    $classification = Get-PublishableFiles -Paths @($state.changed_files)
    if ($null -ne $classification.blocked_paths -and $classification.blocked_paths.Count -gt 0) {
        throw "Blocked local-only paths are present in review state: $($classification.blocked_paths -join ', ')"
    }

    $filesToStage = @($classification.publishable_paths)
    if ($filesToStage.Count -eq 0) {
        throw "No publishable files are available to sync."
    }

    if ($filesToStage.Count -gt 0) {
        git add -- @filesToStage
    }

    $stagedOutput = git diff --cached --name-only
    if (-not [string]::IsNullOrWhiteSpace($stagedOutput)) {
        git commit -m $CommitMessage
    }

    $branch = (git rev-parse --abbrev-ref HEAD).Trim()
    $headSha = (git rev-parse HEAD).Trim()
    if ([string]::IsNullOrWhiteSpace($PullRequestTitle)) {
        $PullRequestTitle = (git log -1 --pretty=%s).Trim()
    }

    git push -u origin $branch

    $prBody = @"
## Summary

- Local review decision: APPROVE
- Review summary: $($state.summary)

## Device Test
- [x] Device test passed
- Result summary: $($state.device_test_summary)

## Maintainer Notes
- Local review workflow: $($state.selected_workflow)
- Local auto-fix attempts: $($state.auto_fix_attempts)
- Merge target: $BaseBranch
- Release behavior: a merge to main triggers release automation when VERSION and release notes indicate a new release
"@

    $safeBranchName = $branch.Replace("/", "-").Replace("\", "-")
    $bodyFile = Join-Path $env:TEMP "streamu-pr-body-$safeBranchName.md"
    $prBody | Set-Content -Path $bodyFile -Encoding UTF8
    $pullRequest = Find-OrCreatePullRequest -Branch $branch -TargetBase $BaseBranch -Title $PullRequestTitle -BodyFile $bodyFile
    $ciRunId = Wait-ForCiRun -Branch $branch -HeadSha $headSha

    $state | Add-Member -NotePropertyName pr_number -NotePropertyValue ([int]$pullRequest.number) -Force
    $state | Add-Member -NotePropertyName pr_url -NotePropertyValue ([string]$pullRequest.url) -Force
    $state | Add-Member -NotePropertyName commit_sha -NotePropertyValue $headSha -Force
    $state | Add-Member -NotePropertyName ci_run_id -NotePropertyValue ([int]$ciRunId) -Force
    $state | Add-Member -NotePropertyName ci_conclusion -NotePropertyValue "success" -Force
    $state | Add-Member -NotePropertyName release_requested -NotePropertyValue ([bool]$ReleaseRequested) -Force
    Save-RepoState -State $state

    Write-Output "Synced to $($pullRequest.url)"
}
finally {
    Pop-Location
}
