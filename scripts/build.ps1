# Build the port from Windows PowerShell via the project's MSYS2 (Vs assets\msys64, junction K:\msys64).
#   .\scripts\build.ps1            incremental
#   .\scripts\build.ps1 -Regen     regenerate C from the EBOOT
param([switch]$Regen, [int]$Jobs = 6, [string]$Game = 'ULUS10567')

$root = Split-Path $PSScriptRoot -Parent
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
$env:MSYS2_PATH_TYPE = 'inherit'   # keep Windows python on PATH
$env:GAME = $Game
$env:JOBS = "$Jobs"
Push-Location $root
try {
    $flag = if ($Regen) { '--regen' } else { '' }
    & K:\msys64\usr\bin\bash.exe -lc "bash scripts/build.sh $flag"
    exit $LASTEXITCODE
} finally { Pop-Location }
