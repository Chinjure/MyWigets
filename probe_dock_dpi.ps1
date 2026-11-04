Add-Type @'
using System;
using System.Runtime.InteropServices;
public class D {
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
$p = Get-Process DesktopDock-x64 -ErrorAction Stop
$h = $p.MainWindowHandle
$dpi = [D]::GetDpiForWindow($h)
$r = New-Object D+RECT
[D]::GetWindowRect($h, [ref]$r) | Out-Null
Write-Host ("dpi={0} rect={1},{2} {3},{4} w={5} h={6}" -f $dpi, $r.Left, $r.Top, $r.Right, $r.Bottom, $r.Right-$r.Left, $r.Bottom-$r.Top)
