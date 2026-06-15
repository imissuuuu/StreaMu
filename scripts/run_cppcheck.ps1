[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Path = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$cppcheck = Get-Command cppcheck -ErrorAction SilentlyContinue
if ($null -eq $cppcheck) {
    $fallback = "C:\Program Files\Cppcheck\cppcheck.exe"
    if (Test-Path -LiteralPath $fallback) {
        $cppcheck = Get-Item -LiteralPath $fallback
    }
}
if ($null -eq $cppcheck) {
    Write-Output "cppcheck not found; skipping"
    exit 0
}

$targetPaths = @($Path | Where-Object { $_ })
if ($targetPaths.Count -eq 0) {
    $targetPaths = @(
        git -C $repoRoot diff --name-only -- source include
        git -C $repoRoot diff --cached --name-only -- source include
        git -C $repoRoot ls-files --others --exclude-standard -- source include
    ) | Where-Object { $_ }
}

$cppFiles = @(
    $targetPaths |
        ForEach-Object { $_ -replace "/", "\" } |
        Sort-Object -Unique |
        Where-Object {
            $_ -match '^(source|include)\\' -and
            $_ -match '\.(c|cc|cpp|cxx|h|hpp)$' -and
            (Test-Path -LiteralPath (Join-Path $repoRoot $_))
        }
)

if ($cppFiles.Count -eq 0) {
    Write-Output "no C/C++ files to check"
    exit 0
}

$suppressionPath = Join-Path $repoRoot "scripts\cppcheck-suppressions.txt"
$arguments = @(
    "--quiet",
    "--inline-suppr",
    "--enable=warning,style,performance,portability",
    "--language=c++",
    "--platform=unix32",
    "--check-level=normal",
    "--suppress=missingInclude",
    "--suppress=missingIncludeSystem",
    "--suppress=useStlAlgorithm",
    "--suppress=normalCheckLevelMaxBranches",
    "--suppress=checkLevelNormal",
    "--suppress=toomanyconfigs",
    "--suppress=checkersReport",
    "--suppressions-list=$suppressionPath",
    "-IC:/devkitPro/libctru/include",
    "-IC:/devkitPro/portlibs/3ds/include",
    "-Iinclude",
    "-Iinclude/network",
    "-Iinclude/ui",
    "-Isource",
    "-Isource/ui"
)
$arguments += $cppFiles

Push-Location $repoRoot
try {
    & $cppcheck.FullName @arguments
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
