Add-Type @'
using System;
using System.Runtime.InteropServices;
public class W2 {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int n);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@
$p = Get-Process DesktopDock-x64 -ErrorAction Stop
$h = $p.MainWindowHandle
$r = New-Object W2+RECT
[W2]::GetWindowRect($h, [ref]$r) | Out-Null
$sb = New-Object System.Text.StringBuilder 128
[W2]::GetClassName($h, $sb, 128) | Out-Null
$cp = New-Object W2+POINT
[W2]::GetCursorPos([ref]$cp) | Out-Null
Write-Host ("hwnd={0} class={1} rect={2},{3} {4},{5}" -f $h, $sb.ToString(), $r.Left, $r.Top, $r.Right, $r.Bottom)
Write-Host ("cursor={0},{1}" -f $cp.X, $cp.Y)
