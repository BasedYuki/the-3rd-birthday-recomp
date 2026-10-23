# One-time setup on a fresh clone, then a full build:
#   .\scripts\setup.ps1 [-Msys C:\msys64] [-Ppsspp <folder with PPSSPPWindows64.exe>]
#
# Needs: your own disc image of the game (ULUS10567, *.iso) in the project folder; MSYS2
# (https://www.msys2.org); Python 3 on PATH; PPSSPP (https://www.ppsspp.org), used once to
# decrypt the game's EBOOT (it writes a decrypted copy when its "dump decrypted EBOOT"
# option is on, which this script switches on for that one run and back off).
param([string]$Msys = '', [string]$Ppsspp = '', [string]$Game = 'ULUS10567')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

# 1. MSYS2 and the UCRT64 packages
if (-not $Msys) {
    foreach ($c in 'K:\msys64', 'C:\msys64', (Join-Path $root 'Vs assets\msys64')) {
        if (Test-Path "$c\usr\bin\bash.exe") { $Msys = $c; break }
    }
}
if (-not $Msys -or -not (Test-Path "$Msys\usr\bin\bash.exe")) {
    Write-Host "MSYS2 not found. Install it from https://www.msys2.org (default C:\msys64), then run this again."
    exit 1
}
"MSYS2: $Msys"
$pkgs = 'mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-sdl3 ' +
        'mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader mingw-w64-ucrt-x86_64-shaderc make'
& "$Msys\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm $pkgs"
if ($LASTEXITCODE) { Write-Error 'pacman failed' }

# 2. Python packages for the recompiler tools
python -m pip install --quiet capstone
if ($LASTEXITCODE) { Write-Host 'note: pip install capstone failed (only tools/disasm.py needs it)' }

# 3. The disc image
$iso = Get-ChildItem $root -Filter *.iso | Select-Object -First 1
if (-not $iso) { Write-Host "Put your disc image of the game (*.iso) into $root and run this again."; exit 1 }
"disc image: $($iso.Name)"

# 4. Decrypted EBOOT through PPSSPP
$elf = Join-Path $root "dumps\${Game}_EBOOT.elf"
if (-not (Test-Path $elf)) {
    if (-not $Ppsspp) { $Ppsspp = $root }
    $exe = Join-Path $Ppsspp 'PPSSPPWindows64.exe'
    if (-not (Test-Path $exe)) { Write-Host "PPSSPP not found in $Ppsspp (use -Ppsspp <folder>)."; exit 1 }
    $ini = Join-Path $Ppsspp 'memstick\PSP\SYSTEM\ppsspp.ini'
    if (-not (Test-Path $ini)) { $ini = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'PPSSPP\PSP\SYSTEM\ppsspp.ini' }
    $dumpdir = Join-Path (Split-Path $ini -Parent) 'DUMP'
    $orig = if (Test-Path $ini) { Get-Content -Raw $ini } else { $null }
    if ($orig) {
        $t = if ($orig -match '(?m)^DumpFileTypes = ') { $orig -replace '(?m)^DumpFileTypes = .*$', 'DumpFileTypes = 1' }
             else { $orig -replace '(?m)^\[General\]\s*$', "[General]`r`nDumpFileTypes = 1" }
        Set-Content -NoNewline -Encoding utf8 $ini $t
    }
    "decrypting the EBOOT with PPSSPP (its window opens for a few seconds)..."
    $p = Start-Process $exe -ArgumentList "`"$($iso.FullName)`"" -PassThru
    $dump = Join-Path $dumpdir "${Game}_EBOOT.BIN"
    for ($i = 0; $i -lt 120 -and -not (Test-Path $dump); $i++) { Start-Sleep 1 }
    Start-Sleep 2
    if (-not $p.HasExited) { $p.Kill() }
    if ($orig) { Set-Content -NoNewline -Encoding utf8 $ini $orig }
    if (-not (Test-Path $dump)) {
        Write-Host "PPSSPP did not write $dump. In PPSSPP: Settings > Tools > Developer tools > 'Dump decrypted EBOOT.BIN on game boot', start the game once, then copy that file to $elf."
        exit 1
    }
    New-Item -ItemType Directory -Force (Split-Path $elf) | Out-Null
    Copy-Item $dump $elf
    "decrypted EBOOT: $elf"
}

# 5. Build
$env:MSYSTEM = 'UCRT64'; $env:CHERE_INVOKING = '1'; $env:MSYS2_PATH_TYPE = 'inherit'; $env:GAME = $Game
Push-Location $root
try { & "$Msys\usr\bin\bash.exe" -lc 'bash scripts/build.sh' } finally { Pop-Location }
if ($LASTEXITCODE) { Write-Error 'build failed (see build output)' }
"Done. Start build\$Game\$Game.exe (double-click), or make a portable copy with scripts\package.ps1."
