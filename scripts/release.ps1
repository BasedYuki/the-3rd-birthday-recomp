# Make a release: version, build, package, release notes; optionally publish on GitHub.
#   .\scripts\release.ps1 -Version 0.9.1             set VERSION, build, package, write notes
#   .\scripts\release.ps1 -Version 0.9.1 -Publish    ... and create the GitHub release (tag v0.9.1)
#
# A published release has the notes (this version's section of docs/CHANGELOG.md) and the
# package dist\The3rdBirthday-<version>.zip: runtime, launcher and kit, NO game code or data
# (package.ps1 checks). Players unzip it and the launcher builds game.dll from their own disc.
param([Parameter(Mandatory)][string]$Version, [switch]$Publish, [string]$Game = 'ULUS10567')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($Version -notmatch '^\d+\.\d+\.\d+$') { Write-Error "version must look like 1.2.3" }

# 1. version + changelog section
Set-Content -NoNewline -Encoding ascii (Join-Path $root 'VERSION') $Version
$log = Get-Content -Raw (Join-Path $root 'docs\CHANGELOG.md')
$m = [regex]::Match($log, "(?ms)^## $([regex]::Escape($Version))\b.*?(?=^## |\z)")
if (-not $m.Success) { Write-Error "docs/CHANGELOG.md has no '## $Version' section yet - write it first" }
$notes = Join-Path $root "dist\release-notes-$Version.md"
New-Item -ItemType Directory -Force (Join-Path $root 'dist') | Out-Null
$body = $m.Value.Trim() + "`n`n---`n**Download** ``The3rdBirthday-$Version.zip``, extract it, start ``The 3rd Birthday.exe``," +
        " choose your own disc image of the game (USA, ULUS10567) and press **Prepare game**: the game is built once" +
        " on your PC (3-10 minutes). No game files are included.`n"
Set-Content -Encoding utf8 $notes $body
"notes: $notes"

# 2. build + local package
& (Join-Path $PSScriptRoot 'build.ps1') -Game $Game
if ($LASTEXITCODE) { Write-Error 'build failed' }
& (Join-Path $PSScriptRoot 'package.ps1') -Game $Game

# 3. publish (only when asked)
if ($Publish) {
    Push-Location $root
    try {
        git add VERSION docs/CHANGELOG.md
        git commit -m "Release v$Version" | Out-Null
        git tag "v$Version"
        git push origin HEAD "v$Version"
        gh release create "v$Version" (Join-Path $root "dist\The3rdBirthday-$Version.zip") --title "v$Version" --notes-file $notes
    } finally { Pop-Location }
    "published v$Version"
} else {
    "not published (add -Publish to create the GitHub release)"
}
