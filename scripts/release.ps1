# Make a release: version, build, package, release notes; optionally publish on GitHub.
#   .\scripts\release.ps1 -Version 0.9.1             set VERSION, build, package, write notes
#   .\scripts\release.ps1 -Version 0.9.1 -Publish    ... and create the GitHub release (tag v0.9.1)
#
# What a published release contains: the release notes (this version's section of
# docs/CHANGELOG.md) and GitHub's source archives. It does NOT upload the built game:
# build\<game>\ holds code recompiled from the game, which only the person who owns the disc
# may have; players build it from their own copy (scripts\setup.ps1). The update notice in
# the game and launcher then points everyone at the new version.
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
$body = $m.Value.Trim() + "`n`n---`nBuild it from your own copy of the game: see the README (``scripts\setup.ps1``)." +
        " Already set up? Pull and run ``scripts\build.ps1``.`n"
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
        gh release create "v$Version" --title "v$Version" --notes-file $notes
    } finally { Pop-Location }
    "published v$Version"
} else {
    "not published (add -Publish to create the GitHub release)"
}
