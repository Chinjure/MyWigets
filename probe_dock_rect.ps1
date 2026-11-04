Add-Type @'
using System;
using System.Runtime.InteropServices;
public class W {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@
$h = [W]::FindWindow('DesktopDockWindow', $null)
$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
Write-Host ("HWND={0} rect={1},{2} {3},{4}" -f $h, $r.Left, $r.Top, $r.Right, $r.Bottom)
$p = New-Object W+POINT
[W]::GetCursorPos([ref]$p) | Out-Null
Write-Host ("cursor={0},{1}" -f $p.X, $p.Y)
