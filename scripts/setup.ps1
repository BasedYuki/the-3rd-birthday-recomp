# One-time setup on a fresh clone, then a full build:
#   .\scripts\setup.ps1 [-Msys C:\msys64]
#
# Needs: your own disc image of the game (ULUS10567, *.iso) in the project folder; MSYS2
# (https://www.msys2.org); Python 3 on PATH. (Players don't need this: the release package's
# launcher builds the game itself.)
param([string]$Msys = '', [string]$Game = 'ULUS10567')

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

# 4. Decrypted EBOOT, straight from the disc image (runtime/third_party/eboot_decrypt)
$elf = Join-Path $root "dumps\${Game}_EBOOT.elf"
if (-not (Test-Path $elf)) {
    New-Item -ItemType Directory -Force (Split-Path $elf) | Out-Null
    $env:MSYSTEM = 'UCRT64'; $env:CHERE_INVOKING = '1'
    Push-Location $root
    try {
        & "$Msys\usr\bin\bash.exe" -lc ('ED=runtime/third_party/eboot_decrypt; T=$(mktemp -d); ' +
            'for c in AES SHA1 bn ec kirk_engine amctrl; do gcc -O2 -w -c $ED/libkirk/$c.c -o $T/$c.o; done; ' +
            'g++ -std=c++17 -O2 -w -I$ED/shim -I$ED $ED/eboot_decrypt.cpp $ED/PrxDecrypter.cpp $T/*.o -static -s -o $T/ed.exe && ' +
            '$T/ed.exe "$1" "$2"') _ $iso.FullName $elf
    } finally { Pop-Location }
    if (-not (Test-Path $elf)) { Write-Error 'could not decrypt the game program from the disc image' }
    "decrypted EBOOT: $elf"
}

# 5. Build
$env:MSYSTEM = 'UCRT64'; $env:CHERE_INVOKING = '1'; $env:MSYS2_PATH_TYPE = 'inherit'; $env:GAME = $Game
Push-Location $root
try { & "$Msys\usr\bin\bash.exe" -lc 'bash scripts/build.sh' } finally { Pop-Location }
if ($LASTEXITCODE) { Write-Error 'build failed (see build output)' }
"Done. Start build\$Game\$Game.exe (double-click), or make a portable copy with scripts\package.ps1."
