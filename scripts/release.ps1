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
$log = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'docs\CHANGELOG.md')   # PS 5.1 reads ANSI otherwise
$m = [regex]::Match($log, "(?ms)^## $([regex]::Escape($Version))\b.*?(?=^## |\z)")
if (-not $m.Success) { Write-Error "docs/CHANGELOG.md has no '## $Version' section yet - write it first" }
$notes = Join-Path $root "dist\release-notes-$Version.md"
New-Item -ItemType Directory -Force (Join-Path $root 'dist') | Out-Null
$body = $m.Value.Trim() + "`n`n---`n**how to play:** download ``The3rdBirthday-$Version.zip``, extract it and open ``The 3rd Birthday.exe``." +
        " pick your own iso of the game (US version, ULUS10567) and press **Prepare game**. it builds the game on your pc" +
        " once, takes a few minutes. no game files are included, you need your own copy.`n`n" +
        "if windows says ""Windows protected your PC"" just click **More info** then **Run anyway** (the exe isn't signed).`n"
[IO.File]::WriteAllText($notes, $body, (New-Object Text.UTF8Encoding $false))   # UTF-8 without a BOM
"notes: $notes"

# 2. build + local package
& (Join-Path $PSScriptRoot 'build.ps1') -Game $Game
if ($LASTEXITCODE) { Write-Error 'build failed' }
& (Join-Path $PSScriptRoot 'package.ps1') -Game $Game

# 3. publish (only when asked)
if ($Publish) {
    Push-Location $root
    $ErrorActionPreference = 'Continue'   # git writes progress to stderr; check exit codes instead
    try {
        git add VERSION docs/CHANGELOG.md
        git commit -m "Release v$Version" | Out-Null
        git tag "v$Version"
        git push origin HEAD "v$Version" 2>&1 | Out-Null
        if ($LASTEXITCODE) { Write-Error 'git push failed' -ErrorAction Stop }
        gh release create "v$Version" (Join-Path $root "dist\The3rdBirthday-$Version.zip") --title "v$Version" --notes-file $notes
    } finally { Pop-Location }
    "published v$Version"
} else {
    "not published (add -Publish to create the GitHub release)"
}
