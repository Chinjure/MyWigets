Add-Type @'
using System;
using System.Runtime.InteropServices;
public class BC {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
}
'@
[BC]::SetProcessDPIAware() | Out-Null
$sh = [int][BC]::GetSystemMetrics(1)
$a = $sh - 2
$b = $sh - 1
$c = $sh
$d = $sh + 1
$ys = @($a, $b, $c, $d)
foreach ($y in $ys) {
  [BC]::SetCursorPos(500, $y) | Out-Null
  Start-Sleep -Milliseconds 100
  $pt = New-Object BC+POINT
  [BC]::GetCursorPos([ref]$pt) | Out-Null
  Write-Host ("set y=" + $y + " got y=" + $pt.Y)
}
