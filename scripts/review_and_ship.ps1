[CmdletBinding()]
param(
    [string]$Goal = "",
    [string]$DeviceTestSummary = "",
    [string[]]$ScopePath = @(),
    [string]$BaseBranch = "main",
    [string]$CommitMessage = "chore: sync reviewed changes",
    [string]$PullRequestTitle = "",
    [int]$MaxAutoFixAttempts = 2,
    [switch]$SkipBuild,
    [switch]$Release,
    [switch]$Publish
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$reviewScript = Join-Path $PSScriptRoot "review_local.ps1"
$syncScript = Join-Path $PSScriptRoot "sync_remote.ps1"
$releaseScript = Join-Path $PSScriptRoot "release_remote.ps1"

$reviewExtraArgs = @()
if ($SkipBuild) {
    $reviewExtraArgs += "-SkipBuild"
}
foreach ($path in $ScopePath) {
    $reviewExtraArgs += "-ScopePath"
    $reviewExtraArgs += $path
}

& $reviewScript -Goal $Goal -DeviceTestSummary $DeviceTestSummary -MaxAutoFixAttempts $MaxAutoFixAttempts @reviewExtraArgs
if ($LASTEXITCODE -ne 0) {
    throw "Local review step failed."
}

$syncExtraArgs = @()
if ($Release) {
    $syncExtraArgs += "-ReleaseRequested"
}

& $syncScript -BaseBranch $BaseBranch -CommitMessage $CommitMessage -PullRequestTitle $PullRequestTitle @syncExtraArgs
if ($LASTEXITCODE -ne 0) {
    throw "Remote sync step failed."
}

if ($Release) {
    $releaseExtraArgs = @()
    if ($Publish) {
        $releaseExtraArgs += "-Publish"
    }

    & $releaseScript @releaseExtraArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Release step failed."
    }
}
