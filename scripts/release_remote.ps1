[CmdletBinding()]
param(
    [switch]$Publish
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

Push-Location $repoRoot
try {
    $state = Get-RepoState
    if ($state.decision -ne "APPROVE") {
        throw "Latest local review decision is $($state.decision). Release is allowed only after APPROVE."
    }
    if ($state.ci_conclusion -ne "success") {
        throw "Remote CI has not completed successfully yet."
    }

    $plan = py -3 .github/scripts/github_flow.py prepare-release-local --repo-root $repoRoot | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) {
        throw "prepare-release-local failed."
    }

    $repo = (gh repo view --json nameWithOwner | ConvertFrom-Json).nameWithOwner
    $existingRelease = $null
    try {
        $existingRelease = gh api "repos/$repo/releases/tags/$($plan.tag_name)" | ConvertFrom-Json
    }
    catch {
        $existingRelease = $null
    }

    if ($null -ne $existingRelease) {
        if (-not [bool]$existingRelease.draft) {
            throw "Published release already exists for $($plan.tag_name)."
        }
        gh release edit $plan.tag_name --draft --title $plan.release_title --notes-file $plan.release_notes_path --target $plan.target_sha | Out-Null
    }
    else {
        gh release create $plan.tag_name --draft --title $plan.release_title --notes-file $plan.release_notes_path --target $plan.target_sha | Out-Null
    }

    $assetArgs = @($plan.asset_paths)
    & gh release upload $plan.tag_name @assetArgs --clobber
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to upload release assets."
    }

    if ($Publish) {
        $draftRelease = gh api "repos/$repo/releases/tags/$($plan.tag_name)" | ConvertFrom-Json
        gh api --method PATCH "repos/$repo/releases/$($draftRelease.id)" --field draft=false | Out-Null
    }

    $state | Add-Member -NotePropertyName release_tag -NotePropertyValue ([string]$plan.tag_name) -Force
    $state | Add-Member -NotePropertyName release_publish_requested -NotePropertyValue ([bool]$Publish) -Force
    Save-RepoState -State $state

    Write-Output "Release prepared: $($plan.tag_name)"
}
finally {
    Pop-Location
}
