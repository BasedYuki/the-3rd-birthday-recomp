# Make the release package: dist\The3rdBirthday-<version>\ and its .zip.
#   .\scripts\package.ps1 [-NoZip]
#
# The package holds NO game code or data. It contains the runtime (ULUS10567.exe: renderer,
# audio, input, saves - none of the game's code), the launcher, the libraries they need, and the
# kit (compiler, Python, translation tools) with which the launcher builds game.dll from the
# player's own disc image on first start ("Prepare game"). game.dll, image.bin and launch.cfg
# are made on the player's PC and are never part of the package.
param([string]$Game = 'ULUS10567', [switch]$NoZip)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$src = Join-Path $root "build\$Game"
$version = (Get-Content -Raw (Join-Path $root 'VERSION')).Trim()
$files = "$Game.exe", 'The 3rd Birthday.exe', 'SDL3.dll', 'libiconv-2.dll', 'vulkan-1.dll'
foreach ($f in $files) {
    if (-not (Test-Path (Join-Path $src $f))) { Write-Error "missing $src\$f - build first (scripts\build.ps1)" }
}
$name = "The3rdBirthday-$version"
$dst = Join-Path $root "dist\$name"
if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
New-Item -ItemType Directory -Force $dst | Out-Null
foreach ($f in $files) { Copy-Item (Join-Path $src $f) $dst }
Copy-Item -Recurse (Join-Path $src 'font') $dst
New-Item -ItemType Directory -Force (Join-Path $dst 'assets') | Out-Null
Copy-Item -Recurse (Join-Path $root 'runtime\assets\vfpu') (Join-Path $dst 'assets')
Copy-Item -Recurse (Join-Path $root 'unlocks') $dst
New-Item -ItemType Directory -Force (Join-Path $dst 'mods') | Out-Null
Copy-Item (Join-Path $root 'mods\README.md') (Join-Path $dst 'mods')
Copy-Item -Recurse (Join-Path $root 'mods\example_wider_fov') (Join-Path $dst 'mods')
Copy-Item (Join-Path $root 'docs\CHANGELOG.md') $dst

# the kit, built with MSYS2's tools into the package
$env:MSYSTEM = 'UCRT64'; $env:CHERE_INVOKING = '1'
Push-Location $root
try { & K:\msys64\usr\bin\bash.exe -lc "bash scripts/make_kit.sh 'dist/$name/kit'" | Select-Object -Last 1 } finally { Pop-Location }
if (-not (Test-Path (Join-Path $dst 'kit\tools\prepare.py'))) { Write-Error 'kit was not built' }

# no game code may slip in
foreach ($f in 'image.bin', 'game.dll', 'launch.cfg', 'init.trace') {
    if (Test-Path (Join-Path $dst $f)) { Write-Error "$f must not be in the package" }
}
@"
The 3rd Birthday - PC port v$version

You need your own disc image of The 3rd Birthday (USA, ULUS10567), as an .iso file.
Nothing from the game is included here.

1. Extract this folder somewhere permanent (for example C:\Games\The3rdBirthday).
2. Start "The 3rd Birthday.exe" (the launcher).
   If Windows says "Windows protected your PC": click "More info", then "Run anyway"
   (the program isn't signed).
3. Choose your .iso if it was not found (it is looked for in this folder and up to three
   folders above).
4. Press "Prepare game": the game is built once from your disc, on your PC (about 3-10
   minutes; nothing is downloaded). Then press Play.

The launcher also has the settings, your saves (backup, restore, Unlock all extras) and the
mods. Its background art is read from your disc image.

Esc in the game opens the settings (controls, camera, resolution, 60 fps, aspect ratio...).
Saves: memstick\PSP\SAVEDATA (backups in memstick\BACKUP). Settings: *.cfg. Logs: run.err.
Mods: mods\README.md. Changes: CHANGELOG.md.
Needs Windows 10/11 64-bit and a GPU with Vulkan.

Not tested yet (see the README on GitHub):
- So far it has only run on Windows 11 with an NVIDIA GPU; AMD/Intel GPUs and Windows 10 are untested.
- Antivirus programs may block "Prepare game" (it runs a compiler and makes game.dll);
  if it fails, allow this folder in your antivirus.
- Extract somewhere you can write to (not Program Files).
- Only the US disc image (ULUS10567) as a plain .iso; no EU/JP versions, no .cso/.chd.
- The whole game has not been played start to finish yet.
Problems: https://github.com/BasedYuki/the-3rd-birthday-recomp/issues (attach run.err)

Support the project (optional): https://ko-fi.com/yuki_visuals7

Licenses: this project GPL-2.0-or-later; the kit's compiler, Python and decrypter keep their
own licenses (kit\licenses).
"@ | Set-Content -Encoding utf8 (Join-Path $dst 'README.txt')

if (-not $NoZip) {
    $zip = "$dst.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $dst -DestinationPath $zip -CompressionLevel Optimal
    "package: $zip  ($([math]::Round((Get-Item $zip).Length / 1MB)) MB)"
}
"folder:  $dst"
