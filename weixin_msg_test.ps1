Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class MT {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, UIntPtr wp, IntPtr lp, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, UIntPtr wp, IntPtr lp);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
}
"@
function Get-Wx {
  $script:w = [IntPtr]::Zero
  [MT]::EnumWindows({ param($h,$lp)
    $wpid = 0
    [MT]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
    $p = Get-Process -Id $wpid -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "Weixin") {
      $cls = New-Object Text.StringBuilder 64
      [MT]::GetClassName($h, $cls, 64) | Out-Null
      if ($cls.ToString() -eq "Qt51514QWindowIcon") {
        $r = New-Object MT+RECT
        [MT]::GetWindowRect($h, [ref]$r) | Out-Null
        if (($r.R - $r.L) -gt 400 -and [MT]::IsWindowVisible($h)) { $script:w = $h }
      }
    }
    return $true
  }, [IntPtr]::Zero) | Out-Null
  return $script:w
}
$w = Get-Wx
if ($w -eq [IntPtr]::Zero) { Write-Host "weixin not visible"; exit }
$r = New-Object MT+RECT
[MT]::GetWindowRect($w, [ref]$r) | Out-Null
$res = [UIntPtr]::Zero
# 1. WM_NCHITTEST at close button area (screen coords as lParam)
$lp = [IntPtr](($r.T + 15 -bor (($r.R - 25) -shl 16)))
$resp = [MT]::SendMessageTimeout($w, 0x0084, [UIntPtr]::Zero, $lp, 2, 1000, [ref]$res)
Write-Host ("WM_NCHITTEST at close area: responded={0} result=0x{1:X} (20=HTCLOSE 1=HTCLIENT 0=none)" -f ($resp -ne [IntPtr]::Zero), $res.ToUInt64())
# 2. WM_NCHITTEST at client area
$lp2 = [IntPtr](($r.T + 400 -bor (($r.L + 500) -shl 16)))
$res2 = [UIntPtr]::Zero
$resp2 = [MT]::SendMessageTimeout($w, 0x0084, [UIntPtr]::Zero, $lp2, 2, 1000, [ref]$res2)
Write-Host ("WM_NCHITTEST at client: responded={0} result=0x{1:X}" -f ($resp2 -ne [IntPtr]::Zero), $res2.ToUInt64())
# 3. WM_SYSCOMMAND SC_CLOSE
[MT]::SendMessage($w, 0x0112, [UIntPtr]0xF060, [IntPtr]::Zero) | Out-Null
Start-Sleep 2
$w2 = Get-Wx
if ($w2 -eq [IntPtr]::Zero) { Write-Host "SC_CLOSE: window HIDDEN - close works via syscommand" } else { Write-Host "SC_CLOSE: window STILL VISIBLE" }
