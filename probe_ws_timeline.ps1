# Sample the extension<->topbar WS link state; report gaps.
param([int]$Seconds = 180, [int]$IntervalMs = 2000)
$log = @()
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    $conn = (netstat -ano | Select-String "127.0.0.1:9786" | Select-String "ESTABLISHED" | Select-Object -First 1)
    $state = if ($conn) { "UP" } else { "DOWN" }
    $log += ((Get-Date -Format "HH:mm:ss") + " " + $state)
    Start-Sleep -Milliseconds $IntervalMs
}
$log | Set-Content -Path "C:\Users\may\Desktop\clock\logs\ws_timeline.txt"
$up = ($log | Select-String " UP$").Count
$down = ($log | Select-String " DOWN$").Count
Write-Host ("samples={0} UP={1} DOWN={2}" -f $log.Count, $up, $down)
# print state transitions with elapsed gaps
$prev = $null; $prevT = $null
foreach ($line in $log) {
    $parts = $line -split ' '
    if ($parts[1] -ne $prev) {
        if ($prev -ne $null) { Write-Host ("  -> {0} at {1} (after {2} samples)" -f $parts[1], $parts[0], $n) }
        $prev = $parts[1]; $n = 0
    }
    $n++
}
Write-Host ("  final state {0} for {1} samples" -f $prev, $n)
