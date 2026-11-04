Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class MT2 {
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
  [MT2]::EnumWindows({ param($h,$lp)
    $wpid = 0
    [MT2]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
    $p = Get-Process -Id $wpid -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "Weixin") {
      $cls = New-Object Text.StringBuilder 64
      [MT2]::GetClassName($h, $cls, 64) | Out-Null
      if ($cls.ToString() -eq "Qt51514QWindowIcon") {
        $r = New-Object MT2+RECT
        [MT2]::GetWindowRect($h, [ref]$r) | Out-Null
        if (($r.R - $r.L) -gt 400 -and [MT2]::IsWindowVisible($h)) { $script:w = $h }
      }
    }
    return $true
  }, [IntPtr]::Zero) | Out-Null
  return $script:w
}
$w = Get-Wx
if ($w -eq [IntPtr]::Zero) { Write-Host "weixin not visible"; exit }
$r = New-Object MT2+RECT
[MT2]::GetWindowRect($w, [ref]$r) | Out-Null
Write-Host ("rect: {0},{1}-{2},{3}" -f $r.L, $r.T, $r.R, $r.B)
function Hit($x, $y) {
  $lp = [IntPtr](($y -band 0xFFFF) -shl 16 -bor ($x -band 0xFFFF))
  $res = [UIntPtr]::Zero
  $resp = [MT2]::SendMessageTimeout($w, 0x0084, [UIntPtr]::Zero, $lp, 2, 1000, [ref]$res)
  return ("responded={0} hit=0x{1:X}" -f ($resp -ne [IntPtr]::Zero), $res.ToUInt64())
}
Write-Host ("close btn area ({0},{1}): {2}" -f ($r.R-30), ($r.T+15), (Hit ($r.R-30) ($r.T+15)))
Write-Host ("client area ({0},{1}): {2}" -f ($r.L+300), ($r.T+300), (Hit ($r.L+300) ($r.T+300)))
# SC_CLOSE via SendMessage
$null = [MT2]::SendMessage($w, 0x0112, [UIntPtr]::new(0xF060), [IntPtr]::Zero)
Start-Sleep 2
$w2 = Get-Wx
if ($w2 -eq [IntPtr]::Zero) { Write-Host "SC_CLOSE: window HIDDEN - close works" } else { Write-Host "SC_CLOSE: window STILL VISIBLE" }
