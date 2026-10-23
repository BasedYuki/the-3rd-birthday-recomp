# Make a portable folder (and zip) of the built game for playing on another PC.
#   .\scripts\package.ps1             -> dist\The3rdBirthday-<date>\ and .zip
#
# The package holds the executable, the recompiled image (code derived from YOUR copy of the
# game: for your own use, do not share it), SDL3.dll, fonts, VFPU tables and launch.cfg. It
# does not include the disc image: put your .iso in the package folder (or a folder above it),
# or set iso=<path> in launch.cfg. Saves and settings stay in the package folder.
param([string]$Game = 'ULUS10567', [switch]$NoZip)

$root = Split-Path $PSScriptRoot -Parent
$src = Join-Path $root "build\$Game"
foreach ($f in "$Game.exe", 'image.bin', 'init.trace', 'launch.cfg', 'SDL3.dll') {
    if (-not (Test-Path (Join-Path $src $f))) { Write-Error "missing $src\$f - build first (scripts\build.ps1)"; exit 1 }
}
$name = "The3rdBirthday-" + (Get-Date -Format 'yyyy-MM-dd')
$dst = Join-Path $root "dist\$name"
if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
New-Item -ItemType Directory -Force $dst | Out-Null
foreach ($f in "$Game.exe", 'image.bin', 'init.trace', 'launch.cfg', 'SDL3.dll') {
    Copy-Item (Join-Path $src $f) $dst
}
Copy-Item -Recurse (Join-Path $src 'font') $dst
New-Item -ItemType Directory -Force (Join-Path $dst 'assets') | Out-Null
Copy-Item -Recurse (Join-Path $root 'runtime\assets\vfpu') (Join-Path $dst 'assets')
# launch.cfg: no machine-specific iso= path in a package
(Get-Content (Join-Path $dst 'launch.cfg')) | Where-Object { $_ -notmatch '^iso=' } |
    Set-Content -Encoding ascii (Join-Path $dst 'launch.cfg')
@"
The 3rd Birthday - PC port (static recompilation), built $(Get-Date -Format 'yyyy-MM-dd')

1. Put your own disc image of The 3rd Birthday (USA, ULUS10567) next to $Game.exe
   (any name ending in .iso), or write its path into launch.cfg as:  iso=D:\Games\3rd.iso
2. Double-click $Game.exe.

Esc opens the settings menu (controls, camera, resolution, 60 fps, aspect ratio, fullscreen).
Saves go to memstick\PSP\SAVEDATA, settings to *.cfg, screenshots (F12) to screenshots\ -
all inside this folder. Logs: run.err.
Needs a Vulkan graphics driver (any GPU from the last ~8 years).

This folder contains code derived from the game. It is for your own use with your own copy
of the game; do not share it.
"@ | Set-Content -Encoding utf8 (Join-Path $dst 'README.txt')
if (-not $NoZip) {
    $zip = "$dst.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $dst -DestinationPath $zip
    "package: $zip"
}
"folder:  $dst"
