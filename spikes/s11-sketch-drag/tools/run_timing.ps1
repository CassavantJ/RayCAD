# Spike S11 timing runs (Windows). Runs the benchmark pinned to one P-core, several times in a row, and logs the
# machine-wide CPU load every 15 s while it runs. Output goes to ..\results\.
# Usage: powershell -File tools\run_timing.ps1 [-Runs 3] [-Mask 0x4000] [-Tag timing] [-BenchArgs "..."]
param(
    [int]$Runs = 3,
    [string]$Mask = "4000",  # hex affinity mask for cmd's start /affinity; CPU 14 is a P-core on the i9-13900K
    [string]$Tag = "timing",
    [string]$BenchArgs = "--rows 1,2,3,4 --cases 0,1,2,3,4,5 --modes 3 --steps 100 --reps 1 --methods 0x1D"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\s11_bench.exe"
$out = Join-Path $root "results"
New-Item -ItemType Directory -Force $out | Out-Null

$loadFile = Join-Path $out "$Tag`_load.txt"
$sampler = Start-Job -ArgumentList $loadFile -ScriptBlock {
    param($f)
    while ($true) {
        $v = (Get-Counter '\Processor(_Total)\% Processor Time').CounterSamples[0].CookedValue
        "{0:yyyy-MM-ddTHH:mm:ss} {1,6:N1}" -f (Get-Date), $v | Add-Content $f
        Start-Sleep -Seconds 15
    }
}
try {
    for ($i = 1; $i -le $Runs; $i++) {
        $f = Join-Path $out "$Tag`_run$i.txt"
        "== $Tag run $i, start $(Get-Date -Format s), affinity mask 0x$Mask, args: $BenchArgs" | Set-Content $f
        cmd /c "start /b /wait /affinity $Mask `"`" `"$exe`" $BenchArgs >> `"$f`" 2>&1"
        "== end $(Get-Date -Format s)" | Add-Content $f
    }
}
finally {
    Stop-Job $sampler; Remove-Job $sampler
}
