# Run the built port. Logs go to build\<game>\run.err / run.out.
#   .\scripts\run.ps1                 run until the window is closed
#   .\scripts\run.ps1 -Seconds 60     stop after 60 s and print the log tail
param([int]$Seconds = 0, [string]$Game = 'ULUS10567')

$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root "build\$Game"
$env:PATH = "K:\msys64\ucrt64\bin;$env:PATH"
$env:PSP_ISO = (Get-ChildItem $root -Filter *.iso | Select-Object -First 1).FullName
$env:SR_MEMSTICK = Join-Path $out 'memstick'
$env:PSP_VFPU_TABLES = (Join-Path $root 'ppsspp\assets\vfpu') -replace '\\', '/'
if (-not (Test-Path $env:PSP_VFPU_TABLES)) {
    $env:PSP_VFPU_TABLES = (Join-Path $root 'assets\vfpu') -replace '\\', '/'   # PPSSPP still in repo root
}
New-Item -ItemType Directory -Force $env:SR_MEMSTICK | Out-Null

$p = Start-Process -FilePath (Join-Path $out "$Game.exe") -WorkingDirectory $out -PassThru `
    -ArgumentList '--image', 'image.bin', '08804000', '08AF9774', 'init.trace', 'none', '--gui' `
    -RedirectStandardOutput (Join-Path $out 'run.out') -RedirectStandardError (Join-Path $out 'run.err')

if ($Seconds -gt 0) {
    if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill(); "stopped after $Seconds s" }
    else { "exited ($($p.ExitCode))" }
} else { $p.WaitForExit() }
Get-Content (Join-Path $out 'run.err') -Tail 30
