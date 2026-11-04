Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Ph2 {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
}
'@
[Ph2]::SetProcessDPIAware() | Out-Null
$p = Get-Process DesktopDock-x64 -ErrorAction Stop
$h = $p.MainWindowHandle
$r = New-Object Ph2+RECT
[Ph2]::GetWindowRect($h, [ref]$r) | Out-Null
$cp = New-Object Ph2+POINT
[Ph2]::GetCursorPos([ref]$cp) | Out-Null
Write-Host "SM_CXSCREEN=" ([Ph2]::GetSystemMetrics(0)) " SM_CYSCREEN=" ([Ph2]::GetSystemMetrics(1))
Write-Host "rect left=" $r.Left " top=" $r.Top " right=" $r.Right " bottom=" $r.Bottom " w=" ($r.Right-$r.Left) " h=" ($r.Bottom-$r.Top)
Write-Host "cursor=" $cp.X "," $cp.Y
