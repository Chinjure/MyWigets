Add-Type @'
using System;
using System.Runtime.InteropServices;
public class HG {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
}
'@
[HG]::SetProcessDPIAware() | Out-Null
$p = Get-Process DesktopDock-x64 -ErrorAction Stop
$h = $p.MainWindowHandle

function Get-Rect {
    $r = New-Object HG+RECT
    [HG]::GetWindowRect($h, [ref]$r) | Out-Null
    return $r
}

$r0 = Get-Rect
Write-Host "initial rect w=" ($r0.Right-$r0.Left) " top=" $r0.Top " bottom=" $r0.Bottom

# move to physical screen bottom, inside dock horizontal span
$cx = [int](($r0.Left + $r0.Right)/2)
[HG]::SetCursorPos($cx, 1599) | Out-Null
Start-Sleep -Milliseconds 800
$r1 = Get-Rect
$cp = New-Object HG+POINT
[HG]::GetCursorPos([ref]$cp) | Out-Null
Write-Host "after set cursor=" $cp.X "," $cp.Y " rect w=" ($r1.Right-$r1.Left) " top=" $r1.Top " bottom=" $r1.Bottom
