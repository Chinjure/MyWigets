Add-Type @'
using System;
using System.Runtime.InteropServices;
public class D2 {
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
}
'@
$p = Get-Process DesktopDock-x64 -ErrorAction Stop
$h = $p.MainWindowHandle
$dpi = [D2]::GetDpiForWindow($h)
$r = New-Object D2+RECT
[D2]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left
$hh = $r.Bottom - $r.Top
Write-Host ("dpi={0} rect left={1} top={2} right={3} bottom={4} w={5} h={6}" -f $dpi, $r.Left, $r.Top, $r.Right, $r.Bottom, $w, $hh)
