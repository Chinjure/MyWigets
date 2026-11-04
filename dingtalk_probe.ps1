Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class DP {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern long GetWindowLongPtr(IntPtr h, int idx);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
}
"@
Get-Process | Where-Object { $_.ProcessName -match "dingtalk|DingTalk|ding" } | Select-Object ProcessName,Id,Path | Format-Table -AutoSize
$procs = Get-Process | Where-Object { $_.ProcessName -match "dingtalk|DingTalk|ding" }
$pidSet = @{}
foreach ($p in $procs) { $pidSet[$p.Id] = $p.ProcessName }
[DP]::EnumWindows({ param($h,$lp)
  $wpid = 0
  [DP]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
  if ($pidSet.ContainsKey([int]$wpid)) {
    $cls = New-Object Text.StringBuilder 64
    [DP]::GetClassName($h, $cls, 64) | Out-Null
    $vis = [DP]::IsWindowVisible($h)
    $icon = [DP]::IsIconic($h)
    $ex = [DP]::GetWindowLongPtr($h, -20)
    $tool = (($ex -band 0x80) -ne 0)
    $owner = [DP]::GetWindow($h, 4)
    $r = New-Object DP+RECT
    [DP]::GetWindowRect($h, [ref]$r) | Out-Null
    $t = New-Object Text.StringBuilder 128
    [DP]::GetWindowText($h, $t, 128) | Out-Null
    $w = $r.R - $r.L; $h2 = $r.B - $r.T
    Write-Host ("proc={0}({1}) cls={2} vis={3} icon={4} tool={5} owner={6} {7}x{8} title=[{9}]" -f $pidSet[[int]$wpid], $wpid, $cls, $vis, $icon, $tool, $owner, $w, $h2, $t)
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
