# Test: click dock weixin icon -> weixin main window should become visible
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class WC {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
}
"@
function Get-WeixinMainState {
  $main = $null
  [WC]::EnumWindows({ param($h,$lp)
    $wpid = 0
    [WC]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
    $p = Get-Process -Id $wpid -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "Weixin") {
      $cls = New-Object Text.StringBuilder 64
      [WC]::GetClassName($h, $cls, 64) | Out-Null
      if ($cls.ToString() -eq "Qt51514QWindowIcon") {
        $r = New-Object WC+RECT
        [WC]::GetWindowRect($h, [ref]$r) | Out-Null
        if (($r.R - $r.L) -gt 400 -and ($r.B - $r.T) -gt 300) {
          $script:mainVis = [WC]::IsWindowVisible($h)
          $script:mainRect = $r
        }
      }
    }
    return $true
  }, [IntPtr]::Zero) | Out-Null
  if ($script:mainVis -eq $null) { return "main window not found" }
  return ("main window visible={0} rect={1},{2}-{3},{4}" -f $script:mainVis, $script:mainRect.L, $script:mainRect.T, $script:mainRect.R, $script:mainRect.B)
}
Write-Host ("BEFORE: {0}" -f (Get-WeixinMainState))
# click dock weixin icon (icon 5 of 9, dock centered at 852)
[WC]::SetCursorPos(1011, 1020) | Out-Null
Start-Sleep 0.3
[WC]::mouse_event(0x02, 0, 0, 0, [UIntPtr]::Zero) | Out-Null
[WC]::mouse_event(0x04, 0, 0, 0, [UIntPtr]::Zero) | Out-Null
Start-Sleep 2
Write-Host ("AFTER:  {0}" -f (Get-WeixinMainState))
# move mouse away
[WC]::SetCursorPos(400, 400) | Out-Null
