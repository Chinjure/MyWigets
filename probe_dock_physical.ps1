Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Ph {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
}
'@
[Ph]::SetProcessDPIAware() | Out-Null
$p = Get-Process DesktopDock-x64 -ErrorAction Stop
$h = $p.MainWindowHandle
$r = New-Object Ph+RECT
[Ph]::GetWindowRect($h, [ref]$r) | Out-Null
$cp = New-Object Ph+POINT
[Ph]::GetCursorPos([ref]$cp) | Out-Null
Write-Host ("SM_CXSCREEN={0} SM_CYSCREEN={1}" -f [Ph]::GetSystemMetrics(0), [Ph]::GetSystemMetrics(1))
Write-Host ("hwnd={0} rect={1},{2} {3},{4} w={5} h={6}" -f $h, $r.Left, $r.Top, $r.Right, $r.Bottom, $r.Right-$r.Left, $r.Bottom-$r.Top)
Write-Host ("cursor={0},{1}" -f $cp.X, $cp.Y)
