Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Coord {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", SetLastError=true)] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
}
'@
$p = Get-Process DesktopDock-x64 -ErrorAction Stop
$h = $p.MainWindowHandle
function Show($label) {
  $r = New-Object Coord+RECT
  [Coord]::GetWindowRect($h, [ref]$r) | Out-Null
  $c = New-Object Coord+POINT
  [Coord]::GetCursorPos([ref]$c) | Out-Null
  Write-Host $label " screen=" [Coord]::GetSystemMetrics(0) "x" [Coord]::GetSystemMetrics(1) " rect=" $r.Left "," $r.Top " - " $r.Right "," $r.Bottom " w=" ($r.Right-$r.Left) " cursor=" $c.X "," $c.Y
}
Show "before-Aware"
[Coord]::SetProcessDPIAware() | Out-Null
Show "after-SetProcessDPIAware"
[Coord]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
Show "after-AwarenessV2"
