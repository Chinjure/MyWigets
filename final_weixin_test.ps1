Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class FT {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
}
"@
function WeixinMain {
  $script:info = "none"
  [FT]::EnumWindows({ param($h,$lp)
    $wpid = 0
    [FT]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
    $p = Get-Process -Id $wpid -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "Weixin") {
      $cls = New-Object Text.StringBuilder 64
      [FT]::GetClassName($h, $cls, 64) | Out-Null
      if ($cls.ToString() -eq "Qt51514QWindowIcon") {
        $r = New-Object FT+RECT
        [FT]::GetWindowRect($h, [ref]$r) | Out-Null
        if (($r.R - $r.L) -gt 400) {
          $script:info = ("visible={0} {1}x{2}" -f ([FT]::IsWindowVisible($h)), ($r.R-$r.L), ($r.B-$r.T))
        }
      }
    }
    return $true
  }, [IntPtr]::Zero) | Out-Null
  return $script:info
}
$tray = [FT]::FindWindow("Shell_TrayWnd", $null)
Write-Host ("start: taskbar visible={0}" -f ([FT]::IsWindowVisible($tray)))
Write-Host ("start: weixin main = {0}" -f (WeixinMain))
[FT]::SetCursorPos(960, 1020) | Out-Null
Start-Sleep 0.5
[FT]::mouse_event(0x02, 0, 0, 0, [UIntPtr]::Zero) | Out-Null
[FT]::mouse_event(0x04, 0, 0, 0, [UIntPtr]::Zero) | Out-Null
# watch over 10s
foreach ($i in 1..10) {
  Start-Sleep 1
  $vis = [FT]::IsWindowVisible($tray)
  $m = WeixinMain
  Write-Host ("t+{0}s: taskbar visible={1}  weixin main={2}" -f $i, $vis, $m)
  if ($m -match "visible=True" -and -not $vis) { Write-Host "RESULT: weixin opened AND taskbar hidden again"; break }
}
[FT]::SetCursorPos(400, 400) | Out-Null
