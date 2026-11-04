Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class MIN {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, UIntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, UIntPtr wp, IntPtr lp, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
}
"@
function Get-Wx {
  $script:w = [IntPtr]::Zero
  [MIN]::EnumWindows({ param($h,$lp)
    $wpid = 0
    [MIN]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
    $p = Get-Process -Id $wpid -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "Weixin") {
      $cls = New-Object Text.StringBuilder 64
      [MIN]::GetClassName($h, $cls, 64) | Out-Null
      if ($cls.ToString() -eq "Qt51514QWindowIcon") {
        $r = New-Object MIN+RECT
        [MIN]::GetWindowRect($h, [ref]$r) | Out-Null
        if (($r.R - $r.L) -gt 400) { $script:w = $h }
      }
    }
    return $true
  }, [IntPtr]::Zero) | Out-Null
  return $script:w
}
function Check($tag) {
  $w = Get-Wx
  if ($w -eq [IntPtr]::Zero) { Write-Host ("{0}: NOT FOUND" -f $tag); return }
  $r = New-Object MIN+RECT
  [MIN]::GetWindowRect($w, [ref]$r) | Out-Null
  $lp = [IntPtr](($r.T + 15 -band 0xFFFF) -shl 16 -bor (($r.R - 30) -band 0xFFFF))
  $res = [UIntPtr]::Zero
  [MIN]::SendMessageTimeout($w, 0x0084, [UIntPtr]::Zero, $lp, 2, 800, [ref]$res) | Out-Null
  Write-Host ("{0}: vis={1} icon={2} hitCloseArea=0x{3:X} (0x14=HTCLOSE normal)" -f $tag, ([MIN]::IsWindowVisible($w)), ([MIN]::IsIconic($w)), $res.ToUInt64())
}
Check "state"
# minimize it via SC_MINIMIZE
$w = Get-Wx
if ($w -ne [IntPtr]::Zero) {
  $null = [MIN]::SendMessage($w, 0x0112, [UIntPtr]::new(0xF020), [IntPtr]::Zero)
  Start-Sleep 1
  Check "after-minimize"
  # restore via ShowWindow (dock's path)
  [MIN]::ShowWindow($w, 9) | Out-Null  # SW_RESTORE
  Start-Sleep 1
  Check "after-restore"
  # test close now
  $null = [MIN]::SendMessage($w, 0x0112, [UIntPtr]::new(0xF060), [IntPtr]::Zero)
  Start-Sleep 2
  Check "after-SC_CLOSE"
}
