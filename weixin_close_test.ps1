Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class XT {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
}
"@
function Get-WxRect {
  $script:rc = $null
  [XT]::EnumWindows({ param($h,$lp)
    $wpid = 0
    [XT]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
    $p = Get-Process -Id $wpid -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "Weixin") {
      $cls = New-Object Text.StringBuilder 64
      [XT]::GetClassName($h, $cls, 64) | Out-Null
      if ($cls.ToString() -eq "Qt51514QWindowIcon") {
        $r = New-Object XT+RECT
        [XT]::GetWindowRect($h, [ref]$r) | Out-Null
        if (($r.R - $r.L) -gt 400 -and [XT]::IsWindowVisible($h)) { $script:rc = $r }
      }
    }
    return $true
  }, [IntPtr]::Zero) | Out-Null
  return $script:rc
}
$rc = Get-WxRect
if (-not $rc) { Write-Host "weixin main not visible"; exit }
Write-Host ("weixin rect: {0},{1}-{2},{3}" -f $rc.L, $rc.T, $rc.R, $rc.B)
# click close button (X) at top-right, inset ~25px
$cx = $rc.R - 25
$cy = $rc.T + 15
Write-Host ("clicking close btn at ({0},{1})" -f $cx, $cy)
[XT]::SetCursorPos($cx, $cy) | Out-Null
Start-Sleep 0.5
[XT]::mouse_event(0x02, 0, 0, 0, [UIntPtr]::Zero) | Out-Null
[XT]::mouse_event(0x04, 0, 0, 0, [UIntPtr]::Zero) | Out-Null
Start-Sleep 2
$rc2 = Get-WxRect
if ($rc2) { Write-Host "RESULT: window STILL VISIBLE (close button no effect)" } else { Write-Host "RESULT: window HIDDEN (close works)" }
[XT]::SetCursorPos(400, 400) | Out-Null
