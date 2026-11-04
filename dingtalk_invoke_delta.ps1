# Experiment: UIA Invoke on DingTalk tray button vs window-set delta.
# NO screenshots. Steps: show taskbar -> wait for island -> baseline windows ->
# invoke -> wait -> delta windows -> report. Then hide taskbar back.
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class EX {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
    public struct POINT { public int X, Y; }
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$trayH = [EX]::FindWindow("Shell_TrayWnd", $null)
[EX]::ShowWindow($trayH, 5) | Out-Null
# gesture: pointer to bottom edge (auto-hide show) — same as user's manual access
$orig = New-Object EX+POINT
[EX]::GetCursorPos([ref]$orig) | Out-Null
$screenH = [EX]::GetSystemMetrics(1)
[EX]::SetCursorPos([int](1707/2), $screenH - 1) | Out-Null
Start-Sleep -Milliseconds 1200

function FindDingButton {
  $root = [System.Windows.Automation.AutomationElement]::RootElement
  $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, "Shell_TrayWnd")
  $trayEl = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
  if (-not $trayEl) { return $null }
  $all = $trayEl.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
  foreach ($e in $all) {
    $c = $e.Current.ClassName
    if ($c -and $c.Contains("SystemTray")) {
      $n = $e.Current.Name
      if ($n -eq '钉钉') { return $e }
    }
  }
  return $null
}

$btn = $null
for ($i = 0; $i -lt 60 -and -not $btn; $i++) {
  Start-Sleep -Milliseconds 500
  $btn = FindDingButton
  if ($btn) { break }
  Write-Host ("waiting island... " + ($i * 0.5) + "s")
}
if (-not $btn) { Write-Host "ISLAND NEVER ALIVE - abort"; [EX]::ShowWindow($trayH, 0) | Out-Null; exit 1 }
Write-Host ("btn found: pid={0} name=[{1}] rect={2}" -f $btn.Current.ProcessId, $btn.Current.Name, $btn.Current.BoundingRectangle)

# baseline: record visible non-zero DingTalk windows
$danPids = @{}
Get-Process -Name 'DingTalk' -ErrorAction SilentlyContinue | ForEach-Object { $danPids[$_.Id] = $true }
function GetDingWindows {
  $out = New-Object System.Collections.ArrayList
  [EX]::EnumWindows({ param($h, $lp)
    $wpid = 0; [EX]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
    if ($danPids.ContainsKey([int]$wpid)) {
      $cls = New-Object Text.StringBuilder 64; [EX]::GetClassName($h, $cls, 64) | Out-Null
      $r = New-Object EX+RECT; [EX]::GetWindowRect($h, [ref]$r) | Out-Null
      $w = $r.R - $r.L; $h2 = $r.B - $r.T
      if ($w -gt 0 -and $h2 -gt 0) {
        $t = New-Object Text.StringBuilder 128; [EX]::GetWindowText($h, $t, 128) | Out-Null
        [void]$out.Add(("{0}|{1}|{2}x{3}|vis={4}|title=[{5}]" -f $h, $cls, $w, $h2, ([EX]::IsWindowVisible($h)), $t))
      }
    }
    return $true
  }, [IntPtr]::Zero) | Out-Null
  return $out
}
$before = GetDingWindows
Write-Host ("=== BEFORE invoke: {0} windows ===" -f $before.Count)
$before | ForEach-Object { Write-Host ("  " + $_) }

# INVOKE via InvokePattern first; if unavailable -> LegacyIAccessible(DoDefaultAction)
$how = 'none'
try {
  $inv = $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
  if ($inv) { $inv.Invoke(); $how = 'InvokePattern' }
} catch {}
if ($how -eq 'none') {
  try {
    $leg = $btn.GetCurrentPattern([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern)
    if ($leg) { $leg.DoDefaultAction(); $how = 'LegacyIAccessible' }
  } catch {}
}
Write-Host ("invoked via: {0}" -f $how)
Start-Sleep -Seconds 4
$after = GetDingWindows
Write-Host ("=== AFTER invoke: {0} windows ===" -f $after.Count)
$after | ForEach-Object { Write-Host ("  " + $_) }
Write-Host ("=== NEW windows ===")
$after | Where-Object { $before -notcontains $_ } | ForEach-Object { Write-Host ("  NEW: " + $_) }
Write-Host ("=== GONE windows ===")
$before | Where-Object { $after -notcontains $_ } | ForEach-Object { Write-Host ("  GONE: " + $_) }
# restore: hide taskbar (auto-hide setting is user-side; just hide window)
[EX]::ShowWindow($trayH, 0) | Out-Null
[EX]::SetCursorPos($orig.X, $orig.Y) | Out-Null
Write-Host "done (taskbar hidden back, cursor restored)"
