# Test: PostMessage WM_LBUTTONDBLCLK to weixin tray message window
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class PMT {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, UIntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
}
"@
function Find-WeixinTrayWnd {
  $script:trayWnd = [IntPtr]::Zero
  [PMT]::EnumWindows({ param($h,$lp)
    $wpid = 0
    [PMT]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
    $p = Get-Process -Id $wpid -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "Weixin") {
      $cls = New-Object Text.StringBuilder 96
      [PMT]::GetClassName($h, $cls, 96) | Out-Null
      if ($cls.ToString() -match "TrayIconMessage|WxTray") { $script:trayWnd = $h }
    }
    return $true
  }, [IntPtr]::Zero) | Out-Null
  return $script:trayWnd
}
function WeixinMainCount {
  $script:n = 0
  [PMT]::EnumWindows({ param($h,$lp)
    $wpid = 0
    [PMT]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
    $p = Get-Process -Id $wpid -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "Weixin") {
      $cls = New-Object Text.StringBuilder 64
      [PMT]::GetClassName($h, $cls, 64) | Out-Null
      if ($cls.ToString() -eq "Qt51514QWindowIcon") {
        $r = New-Object PMT+RECT
        [PMT]::GetWindowRect($h, [ref]$r) | Out-Null
        if (($r.R - $r.L) -gt 400 -and [PMT]::IsWindowVisible($h)) { $script:n++ }
      }
    }
    return $true
  }, [IntPtr]::Zero) | Out-Null
  return $script:n
}
$w = Find-WeixinTrayWnd
Write-Host ("weixin tray msg window: {0}" -f $w)
Write-Host ("main windows visible BEFORE: {0}" -f (WeixinMainCount))
if ($w -ne [IntPtr]::Zero) {
  foreach ($msg in @(0x0203, 0x0201, 0x0202, 0x0200)) {  # DBLCLK, LBUTTONDOWN, LBUTTONUP, MOUSEMOVE
    [PMT]::PostMessage($w, $msg, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep 0.4
  }
  Start-Sleep 2
  Write-Host ("main windows visible AFTER:  {0}" -f (WeixinMainCount))
}
