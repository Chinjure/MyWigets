Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Seq {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
}
'@
[Seq]::SetProcessDPIAware() | Out-Null
$p = Get-Process DesktopDock-x64 -ErrorAction Stop
$h = $p.MainWindowHandle

function Get-Rect {
  $r = New-Object Seq+RECT
  [Seq]::GetWindowRect($h, [ref]$r) | Out-Null
  return $r
}
function ShowCursor {
  $c = New-Object Seq+POINT
  [Seq]::GetCursorPos([ref]$c) | Out-Null
  return $c
}

Write-Host ("SM=" + [Seq]::GetSystemMetrics(0) + "x" + [Seq]::GetSystemMetrics(1))
$r0 = Get-Rect
$c0 = ShowCursor
Write-Host ("idle rect=" + $r0.Left + "," + $r0.Top + "-" + $r0.Right + "," + $r0.Bottom + " w=" + ($r0.Right-$r0.Left) + " cursor=" + $c0.X + "," + $c0.Y)

# Move to the middle of glass body (top+30)
$cx = [int](($r0.Left+$r0.Right)/2)
$insideY = $r0.Top + 30
[Seq]::SetCursorPos($cx, $insideY) | Out-Null
Start-Sleep -Milliseconds 700
$r1 = Get-Rect
$c1 = ShowCursor
Write-Host ("inside cursor=" + $c1.X + "," + $c1.Y + " rect=" + $r1.Left + "," + $r1.Top + "-" + $r1.Right + "," + $r1.Bottom + " w=" + ($r1.Right-$r1.Left))

# Move to gap at bottom (physical screen bottom - 1)
$bottomY = [Seq]::GetSystemMetrics(1) - 1
[Seq]::SetCursorPos($cx, $bottomY) | Out-Null
Start-Sleep -Milliseconds 700
$r2 = Get-Rect
$c2 = ShowCursor
Write-Host ("gap cursor=" + $c2.X + "," + $c2.Y + " rect=" + $r2.Left + "," + $r2.Top + "-" + $r2.Right + "," + $r2.Bottom + " w=" + ($r2.Right-$r2.Left))

# Move far away
[Seq]::SetCursorPos(100, 100) | Out-Null
Start-Sleep -Milliseconds 700
$r3 = Get-Rect
Write-Host ("away w=" + ($r3.Right-$r3.Left))
