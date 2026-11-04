Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class DIAG {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, UIntPtr wp, IntPtr lp, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, UIntPtr wp, IntPtr lp);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
}
"@
function Get-WxMain {
  $script:w = [IntPtr]::Zero
  [DIAG]::EnumWindows({ param($h,$lp)
    $wpid = 0
    [DIAG]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
    $p = Get-Process -Id $wpid -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "Weixin") {
      $cls = New-Object Text.StringBuilder 64
      [DIAG]::GetClassName($h, $cls, 64) | Out-Null
      if ($cls.ToString() -eq "Qt51514QWindowIcon") {
        $r = New-Object DIAG+RECT
        [DIAG]::GetWindowRect($h, [ref]$r) | Out-Null
        if (($r.R - $r.L) -gt 400) { $script:w = $h }
      }
    }
    return $true
  }, [IntPtr]::Zero) | Out-Null
  return $script:w
}
function Show-State($tag) {
  $w = Get-WxMain
  if ($w -eq [IntPtr]::Zero) { Write-Host ("{0}: weixin main NOT FOUND" -f $tag); return }
  $fg = [DIAG]::GetForegroundWindow()
  $enabled = [DIAG]::IsWindowEnabled($w)
  $vis = [DIAG]::IsWindowVisible($w)
  $icon = [DIAG]::IsIconic($w)
  $res = [UIntPtr]::Zero
  $resp = [DIAG]::SendMessageTimeout($w, 0x0000, [UIntPtr]::Zero, [IntPtr]::Zero, 2, 500, [ref]$res)  # WM_NULL
  Write-Host ("{0}: vis={1} icon={2} enabled={3} fg={4} msgloopAlive={5}" -f $tag, $vis, $icon, $enabled, ($fg -eq $w), ($resp -ne [IntPtr]::Zero))
}
Show-State "BEFORE"
[DIAG]::SetCursorPos(960, 1020) | Out-Null
Start-Sleep 0.5
[DIAG]::mouse_event(0x02, 0, 0, 0, [UIntPtr]::Zero) | Out-Null
[DIAG]::mouse_event(0x04, 0, 0, 0, [UIntPtr]::Zero) | Out-Null
Start-Sleep 2
Show-State "AFTER-WAKE"
# test close: post WM_CLOSE
$w2 = Get-WxMain
if ($w2 -ne [IntPtr]::Zero) {
  [DIAG]::PostMessage($w2, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep 2
  Show-State "AFTER-WMCLOSE"
}
[DIAG]::SetCursorPos(400, 400) | Out-Null
