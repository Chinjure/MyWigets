Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class DR {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
}
"@
$dock = $null
[DR]::EnumWindows({ param($h,$lp)
  $cls = New-Object Text.StringBuilder 64
  [DR]::GetClassName($h, $cls, 64) | Out-Null
  if ($cls.ToString() -eq "DesktopDockWindow") {
    $r = New-Object DR+RECT
    [DR]::GetWindowRect($h, [ref]$r) | Out-Null
    $script:dockRect = $r
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
Write-Host ("dock rect: L={0} R={1} W={2}" -f $script:dockRect.L, $script:dockRect.R, ($script:dockRect.R - $script:dockRect.L))
# from log: first slot left=37 (window coords), last slot cx=703, n=9
$firstLeft = 37
$lastCx = 703
$n = 9
$firstCx = $firstLeft + 22
$step = ($lastCx - $firstCx) / ($n - 1)
for ($i = 0; $i -lt $n; $i++) {
  $cxWin = $firstCx + $i * $step
  $sx = [int]($script:dockRect.L + $cxWin)
  Write-Host ("icon {0}: winX={1:N0} screenX={2}" -f ($i+1), $cxWin, $sx)
}
