<#
.SYNOPSIS
    Bump the MAJOR.MINOR version in CMakeLists.txt.

.DESCRIPTION
    Patch is derived automatically from the git commit count at build time,
    so this script only sets MAJOR.MINOR (patch is forced to .0 in the file).

.EXAMPLE
    .\scripts\bump-version.ps1 0.3
#>
param(
    [string]$Version
)

$ErrorActionPreference = "Stop"

if (-not $Version) {
    [Console]::Error.WriteLine("usage: scripts\bump-version.ps1 <MAJOR.MINOR>  (e.g. 0.3)")
    exit 1
}

if ($Version -notmatch '^[0-9]+\.[0-9]+$') {
    [Console]::Error.WriteLine("error: version must be MAJOR.MINOR -- patch is auto-set from git commit count")
    exit 1
}

$Major, $Minor = $Version.Split('.')

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root       = Resolve-Path (Join-Path $ScriptDir "..")
$CMakeLists = Join-Path $Root "CMakeLists.txt"

$content = Get-Content -Raw $CMakeLists
$updated = $content -replace '(    VERSION )[0-9]+\.[0-9]+\.[0-9]+', "`${1}$Major.$Minor.0"

if ($updated -eq $content) {
    [Console]::Error.WriteLine("error: no 'VERSION X.Y.Z' line found in $CMakeLists")
    exit 1
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($CMakeLists, $updated, $Utf8NoBom)

$CommitCount = (& git -C $Root rev-list --count HEAD 2>$null)
if (-not $CommitCount) { $CommitCount = "?" }

Write-Host "version: $Major.$Minor.$CommitCount  (patch will update on every new commit)"
