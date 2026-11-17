# Spike S2 start-up benchmark harness (Windows PowerShell 5.1 or later, no admin rights needed).
#
#   powershell -ExecutionPolicy Bypass -File bench.ps1                 warm starts, 20 runs per variant, interleaved
#   powershell -ExecutionPolicy Bypass -File bench.ps1 -Set diag       diagnostic variants (where the time goes)
#   powershell -ExecutionPolicy Bypass -File bench.ps1 -PrepareCold    make one private copy of the deploy folder per variant
#   powershell -ExecutionPolicy Bypass -File bench.ps1 -Cold           FIRST thing after a reboot: one run per variant from its private copy
#   powershell -ExecutionPolicy Bypass -File bench.ps1 -Fresh 5        first launch from a freshly copied deploy folder (NOT a cold start), 5 copies
#
# Every run starts the shell, which exits by itself right after its first frame. A run that has not exited
# after 40 s is killed and recorded as a failure.
param(
    [int]$Runs = 20,
    [int]$Warmup = 3,
    [string]$Set = 'main',
    [string]$Tag = '',
    [switch]$PrepareCold,
    [switch]$Cold,
    [int]$Fresh = 0,
    [switch]$HighPriority   # the shell raises itself to High priority class at main(); use when other jobs load the machine
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$deploy = Join-Path $root 'build\deploy'
$coldRoot = Join-Path $root 'build\cold'
$results = Join-Path $root 'results'
New-Item -ItemType Directory -Force $results | Out-Null

$noCache = @{ QML_DISABLE_DISK_CACHE = '1' }
# name; exe; extra arguments; environment. "LOOSE" is replaced by the loose QML folder of the deploy folder in use.
$variantSets = @{
    main = @(
        @{ n = 'base_aot';            exe = 'shell_base.exe';   a = @();                 env = @{} },
        @{ n = 'delay_aot';           exe = 'shell_delay.exe';  a = @();                 env = @{} },
        @{ n = 'noocct_aot';          exe = 'shell_noocct.exe'; a = @();                 env = @{} },
        @{ n = 'base_loose_nocache';  exe = 'shell_base.exe';   a = @('--loose', 'LOOSE'); env = $noCache },
        @{ n = 'delay_loose_nocache'; exe = 'shell_delay.exe';  a = @('--loose', 'LOOSE'); env = $noCache },
        @{ n = 'base_loose_qmlc';     exe = 'shell_base.exe';   a = @('--loose', 'LOOSE'); env = @{} },
        @{ n = 'delay_loose_qmlc';    exe = 'shell_delay.exe';  a = @('--loose', 'LOOSE'); env = @{} },
        @{ n = 'base_aot_nocache';    exe = 'shell_base.exe';   a = @();                 env = $noCache },
        @{ n = 'delay_aot_useocct';   exe = 'shell_delay.exe';  a = @('--use-occt');     env = @{} },
        @{ n = 'base_aot_useocct';    exe = 'shell_base.exe';   a = @('--use-occt');     env = @{} }
    )
    # Order matters after a reboot: the first variant also pays for cold Windows system DLLs.
    cold = @(
        @{ n = 'delay_aot';           exe = 'shell_delay.exe';  a = @();                 env = @{} },
        @{ n = 'base_aot';            exe = 'shell_base.exe';   a = @();                 env = @{} },
        @{ n = 'noocct_aot';          exe = 'shell_noocct.exe'; a = @();                 env = @{} },
        @{ n = 'delay_loose_nocache'; exe = 'shell_delay.exe';  a = @('--loose', 'LOOSE'); env = $noCache },
        @{ n = 'delay_aot_useocct';   exe = 'shell_delay.exe';  a = @('--use-occt');     env = @{} }
    )
    fresh = @(
        @{ n = 'base_aot';            exe = 'shell_base.exe';   a = @();                 env = @{} },
        @{ n = 'delay_aot';           exe = 'shell_delay.exe';  a = @();                 env = @{} }
    )
    diag = @(
        @{ n = 'delay_aot';           exe = 'shell_delay.exe';  a = @();                 env = @{} },
        @{ n = 'delay_software';      exe = 'shell_delay.exe';  a = @();                 env = @{ QT_QUICK_BACKEND = 'software' } },
        @{ n = 'delay_d3d12';         exe = 'shell_delay.exe';  a = @();                 env = @{ QSG_RHI_BACKEND = 'd3d12' } },
        @{ n = 'delay_opengl';        exe = 'shell_delay.exe';  a = @();                 env = @{ QSG_RHI_BACKEND = 'opengl' } },
        @{ n = 'delay_vulkan';        exe = 'shell_delay.exe';  a = @();                 env = @{ QSG_RHI_BACKEND = 'vulkan' } },
        @{ n = 'delay_basicloop';     exe = 'shell_delay.exe';  a = @();                 env = @{ QSG_RENDER_LOOP = 'basic' } },
        @{ n = 'delay_emptywindow';   exe = 'shell_delay.exe';  a = @('--loose', 'EMPTY'); env = @{} },
        @{ n = 'delay_warp';          exe = 'shell_delay.exe';  a = @();                 env = @{ QSG_RHI_PREFER_SOFTWARE_RENDERER = '1' } }
    )
}
# What the QML content costs: the loose files with one part of the UI removed (made by make_exp.sh), .qmlc cache warm.
$variantSets['parts'] = @(
    @{ n = 'full';         exe = 'shell_delay.exe'; a = @('--loose', 'EXP:full');        env = @{} },
    @{ n = 'no_menubar';   exe = 'shell_delay.exe'; a = @('--loose', 'EXP:no_menubar');  env = @{} },
    @{ n = 'no_palette';   exe = 'shell_delay.exe'; a = @('--loose', 'EXP:no_palette');  env = @{} },
    @{ n = 'no_ribbon';    exe = 'shell_delay.exe'; a = @('--loose', 'EXP:no_ribbon');   env = @{} },
    @{ n = 'no_props';     exe = 'shell_delay.exe'; a = @('--loose', 'EXP:no_props');    env = @{} },
    @{ n = 'no_sidepanel'; exe = 'shell_delay.exe'; a = @('--loose', 'EXP:no_sidepanel'); env = @{} },
    @{ n = 'no_timeline';  exe = 'shell_delay.exe'; a = @('--loose', 'EXP:no_timeline'); env = @{} },
    @{ n = 'no_start';     exe = 'shell_delay.exe'; a = @('--loose', 'EXP:no_start');    env = @{} },
    @{ n = 'viewport_only'; exe = 'shell_delay.exe'; a = @('--loose', 'EXP:viewport_only'); env = @{} }
)
if ($variantSets.ContainsKey('qmltc') -eq $false -and (Test-Path (Join-Path $deploy 'shell_qmltc.exe'))) {
    $variantSets['main'] += @{ n = 'qmltc'; exe = 'shell_qmltc.exe'; a = @(); env = @{} }
}
if ($PrepareCold -or $Cold) { $Set = 'cold' }
if ($Fresh -gt 0) { $Set = 'fresh' }
$variants = $variantSets[$Set]
if (-not $variants) { throw "unknown set $Set" }

function Invoke-Shell([string]$dir, $v, [string]$label, [string]$outFile) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $dir $v.exe
    $psi.WorkingDirectory = $dir
    $psi.UseShellExecute = $false
    $argList = @('--label', $label, '--out', "`"$outFile`"")
    foreach ($x in $v.a) {
        if ($x -eq 'LOOSE') { $argList += "`"$(Join-Path $dir 'qml_loose')`"" }
        elseif ($x -eq 'EMPTY') { $argList += "`"$(Join-Path $root 'qml_empty')`"" }
        elseif ($x -like 'EXP:*') { $argList += "`"$(Join-Path $root ('build\exp\' + $x.Substring(4)))`"" }
        else { $argList += $x }
    }
    $psi.Arguments = $argList -join ' '
    foreach ($k in $v.env.Keys) { $psi.EnvironmentVariables[$k] = $v.env[$k] }
    if ($HighPriority) { $psi.EnvironmentVariables['S2_HIGH_PRIORITY'] = '1' }
    $before = if (Test-Path $outFile) { (Get-Content $outFile).Count } else { 0 }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit(40000)) { $p.Kill(); $p.WaitForExit(); return $null }
    $wall = $sw.Elapsed.TotalMilliseconds
    if ($p.ExitCode -ne 0) { return $null }
    $lines = @(Get-Content $outFile)
    if ($lines.Count -le $before) { return $null }
    $rec = [ordered]@{ variant = $v.n; label = $label; wall_ms = [math]::Round($wall, 1) }
    foreach ($kv in $lines[-1].Split(' ')) {
        $i = $kv.IndexOf('=')
        if ($i -gt 0 -and $kv.Substring(0, $i) -ne 'label') { $rec[$kv.Substring(0, $i)] = $kv.Substring($i + 1) }
    }
    return [pscustomobject]$rec
}

function Get-Median($values) {
    $s = @($values | Sort-Object)
    if ($s.Count -eq 0) { return [double]::NaN }
    if ($s.Count % 2) { return $s[($s.Count - 1) / 2] }
    return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2
}

if ($PrepareCold) {
    if (Test-Path $coldRoot) { Remove-Item -Recurse -Force $coldRoot }
    foreach ($v in $variants) {
        $dst = Join-Path $coldRoot $v.n
        New-Item -ItemType Directory -Force $dst | Out-Null
        Copy-Item -Recurse -Force (Join-Path $deploy '*') $dst
    }
    Write-Host "Prepared $($variants.Count) private deploy copies under $coldRoot. Reboot, log in, wait 2 minutes, then run: bench.ps1 -Cold"
    return
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ($Cold) {
    if (-not (Test-Path $coldRoot)) { throw 'Run bench.ps1 -PrepareCold before the reboot.' }
    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
    $csv = Join-Path $results "cold-$stamp.csv"
    $raw = Join-Path $results "cold-$stamp.raw.txt"
    $rows = @()
    foreach ($v in $variants) {
        $dir = Join-Path $coldRoot $v.n
        # QML disk cache and pipeline cache for this copy go to build\cold\s2cache (see main.cpp).
        $r = Invoke-Shell $dir $v 'cold' $raw
        if ($r) { $rows += $r; Write-Host ("{0,-22} cold  frame {1,8} ms  main {2,8} ms" -f $v.n, $r.frame_ms, $r.main_ms) }
        else { Write-Host "$($v.n) FAILED" }
        $r2 = Invoke-Shell $dir $v 'second' $raw
        if ($r2) { $rows += $r2; Write-Host ("{0,-22} 2nd   frame {1,8} ms  main {2,8} ms" -f $v.n, $r2.frame_ms, $r2.main_ms) }
    }
    $rows | Export-Csv -NoTypeInformation $csv
    Write-Host "Minutes since boot: $([math]::Round(((Get-Date) - $boot).TotalMinutes, 1)). Results: $csv"
    Write-Host 'Only the first variant is fully cold for Windows system DLLs (d3d11, dwrite, GPU driver); each variant is cold for its own Qt and OCCT files.'
    return
}

if ($Fresh -gt 0) {
    # First launch from a folder whose files were just copied and never executed, then a second launch.
    # The copy leaves the file data in the Windows file cache, so this is NOT a cold start. It shows the
    # cost of the first launch after an install or update (image mapping, Defender scan of new binaries).
    $freshRoot = Join-Path $root 'build\fresh'
    $csv = Join-Path $results "fresh-$stamp.csv"
    $raw = Join-Path $results "fresh-$stamp.raw.txt"
    $rows = @()
    for ($i = 1; $i -le $Fresh; $i++) {
        foreach ($v in $variants) {
            $dir = Join-Path $freshRoot "$($v.n)_$i\deploy"
            New-Item -ItemType Directory -Force $dir | Out-Null
            Copy-Item -Recurse -Force (Join-Path $deploy '*') $dir
            foreach ($label in 'first', 'second') {
                $r = Invoke-Shell $dir $v $label $raw
                if ($r) { $rows += $r; Write-Host ("{0,-12} copy {1} {2,-6} frame {3,8} ms  main {4,8} ms" -f $v.n, $i, $label, $r.frame_ms, $r.main_ms) }
                else { Write-Host "$($v.n) copy $i $label FAILED" }
            }
        }
    }
    Remove-Item -Recurse -Force $freshRoot
    $rows | Export-Csv -NoTypeInformation $csv
    foreach ($v in $variants) {
        foreach ($label in 'first', 'second') {
            $g = @($rows | Where-Object { $_.variant -eq $v.n -and $_.label -eq $label })
            if ($g.Count -eq 0) { continue }
            $f = @($g | ForEach-Object { [double]$_.frame_ms })
            Write-Host ("{0,-12} {1,-6} n={2} median {3:F0} worst {4:F0} best {5:F0}  main median {6:F0}" -f $v.n, $label, $g.Count, (Get-Median $f),
                ($f | Measure-Object -Maximum).Maximum, ($f | Measure-Object -Minimum).Minimum, (Get-Median @($g | ForEach-Object { [double]$_.main_ms })))
        }
    }
    Write-Host "CSV: $csv"
    return
}

$suffix = if ($Tag) { "-$Tag" } else { '' }
$csv = Join-Path $results "warm-$Set$suffix-$stamp.csv"
$raw = Join-Path $results "warm-$Set$suffix-$stamp.raw.txt"
$summaryFile = Join-Path $results "warm-$Set$suffix-$stamp.summary.txt"
function Get-CpuLoad { [math]::Round(((Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 3).CounterSamples | Measure-Object CookedValue -Average).Average) }
$loadBefore = Get-CpuLoad
$scratch = Join-Path $root 'build\warmup.raw.txt'
for ($i = 0; $i -lt $Warmup; $i++) { foreach ($v in $variants) { Invoke-Shell $deploy $v 'warmup' $scratch | Out-Null } }
$rows = @()
$failed = @{}
for ($i = 1; $i -le $Runs; $i++) {
    foreach ($v in $variants) {
        $r = Invoke-Shell $deploy $v "run$i" $raw
        if ($r) { $rows += $r } else { $failed[$v.n] = 1 + [int]$failed[$v.n] }
    }
    Write-Host "round $i/$Runs"
}
$rows | Export-Csv -NoTypeInformation $csv
$out = @()
$out += ('{0,-22} {1,3} {2,8} {3,8} {4,8} | {5,7} {6,7} {7,7} {8,7} | {9,7} {10,5} {11,5} {12,5}' -f 'variant', 'n', 'median', 'worst', 'best', 'main', 'app', 'qml', 'occtuse', 'wall', 'mods', 'occt', 'fail')
foreach ($v in $variants) {
    $g = @($rows | Where-Object { $_.variant -eq $v.n })
    if ($g.Count -eq 0) { $out += ('{0,-22} no successful runs ({1} failed)' -f $v.n, [int]$failed[$v.n]); continue }
    $f = @($g | ForEach-Object { [double]$_.frame_ms })
    $out += ('{0,-22} {1,3} {2,8:F0} {3,8:F0} {4,8:F0} | {5,7:F0} {6,7:F0} {7,7:F0} {8,7:F0} | {9,7:F0} {10,5} {11,5} {12,5}' -f $v.n, $g.Count,
        (Get-Median $f), ($f | Measure-Object -Maximum).Maximum, ($f | Measure-Object -Minimum).Minimum,
        (Get-Median @($g | ForEach-Object { [double]$_.main_ms })), (Get-Median @($g | ForEach-Object { [double]$_.app_ms })),
        (Get-Median @($g | ForEach-Object { [double]$_.qml_ms })), (Get-Median @($g | ForEach-Object { [double]$_.occt_use_ms })),
        (Get-Median @($g | ForEach-Object { [double]$_.wall_ms })), $g[0].mods_frame, $g[0].occt_mods_frame, [int]$failed[$v.n])
}
$out += "Total CPU load from other processes: $loadBefore % before, $(Get-CpuLoad) % after (3 s samples). Runs per variant: $Runs, interleaved, after $Warmup discarded warm-up rounds. High priority: $([bool]$HighPriority)."
$out += 'All times in ms since OS process creation. median/worst/best = first frame swapped. main/app/qml = medians of the stage stamps.'
$out | Set-Content $summaryFile
$out | ForEach-Object { Write-Host $_ }
Write-Host "CSV: $csv"
