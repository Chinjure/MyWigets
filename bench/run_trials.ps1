# run_trials.ps1 - 交替运行 pulse / sandbox 两版挂起策略的功耗基准
#
# 用法（Windows PowerShell 5.1）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File bench\run_trials.ps1 -Reps 3
#
# 每个 trial 约 55s（8s 空闲 + 启动/4s + 6s 活跃 + 30s 挂起复核 + 5s 收尾空闲）。
# 场景随机排序、两版交替，降低背景负载漂移带来的偏差。
param(
    [int]$Reps = 3,
    [int]$TrialBase = 100,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$bench = $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $bench 'results_final' }
$art = Join-Path $bench 'artifacts'

foreach ($f in 'victim.exe', 'bench_pulse.exe', 'bench_sandbox.exe') {
    $p = Join-Path $art $f
    if (-not (Test-Path $p)) { throw "missing $p; build first" }
}

if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$scenarios = @(
    [pscustomobject]@{ name = 'control'; same = 4; cross = 0; exitMs = 0 },
    [pscustomobject]@{ name = 'cross';   same = 2; cross = 2; exitMs = 0 },
    [pscustomobject]@{ name = 'orphan';  same = 2; cross = 2; exitMs = 800 }
)
$versions = @('pulse', 'sandbox')
$trial = $TrialBase
$log = @()

for ($rep = 1; $rep -le $Reps; ++$rep) {
    $order = @($scenarios | Sort-Object { Get-Random })
    for ($si = 0; $si -lt $order.Count; ++$si) {
        $sc = $order[$si]
        $vorder = if ((($rep + $si) % 2) -eq 0) { @('pulse', 'sandbox') } else { @('sandbox', 'pulse') }
        foreach ($v in $vorder) {
            ++$trial
            $wd = Join-Path $bench ("work\r{0}_{1}_{2}" -f $rep, $sc.name, $v)
            $appDir = Join-Path $wd 'app'
            $extDir = Join-Path $wd 'ext'
            Remove-Item -Recurse -Force $wd -ErrorAction SilentlyContinue
            New-Item -ItemType Directory -Force -Path $appDir, $extDir | Out-Null
            Copy-Item (Join-Path $art 'victim.exe') (Join-Path $appDir 'victim.exe') -Force
            Copy-Item (Join-Path $art 'victim.exe') (Join-Path $appDir 'helper.exe') -Force
            Copy-Item (Join-Path $art 'victim.exe') (Join-Path $extDir 'helper.exe') -Force

            $argv = @(
                '--version', $v,
                '--scenario', $sc.name,
                '--trial', "$trial",
                '--app-dir', $appDir,
                '--ext-dir', $extDir,
                '--out-dir', $OutDir,
                '--adopt', $(if ($v -eq 'sandbox') { '1' } else { '0' }),
                '--same', "$($sc.same)",
                '--cross', "$($sc.cross)"
            )
            if ($sc.exitMs -gt 0) { $argv += @('--exit-after-ms', "$($sc.exitMs)") }

            $start = Get-Date
            Write-Host ("[{0:HH:mm:ss}] trial {1} rep={2} scenario={3} version={4}" -f `
                $start, $trial, $rep, $sc.name, $v)
            & (Join-Path $art "bench_$v.exe") @argv 2>&1 | Out-Null
            $code = $LASTEXITCODE
            $elapsed = ((Get-Date) - $start).TotalSeconds
            Write-Host ("            exit={0} elapsed={1:N1}s" -f $code, $elapsed)

            $log += [pscustomobject]@{
                trial = $trial
                rep = $rep
                scenario = $sc.name
                version = $v
                exit = $code
                started = $start.ToString('s')
                seconds = [math]::Round($elapsed, 1)
                same = $sc.same
                cross = $sc.cross
                exit_after_ms = $sc.exitMs
            }
            if ($code -ne 0) { throw "trial $trial failed with exit $code" }
            Start-Sleep -Seconds 2
        }
    }
}

$log | Export-Csv -Path (Join-Path $OutDir 'run_log.csv') -NoTypeInformation -Encoding UTF8
Write-Host "done: $($log.Count) trials -> $OutDir" -ForegroundColor Green
