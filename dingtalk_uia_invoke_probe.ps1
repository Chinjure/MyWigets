# Probe Win11 tray UIA: find DingTalk tray icon element, verify Invoke, restore state.
# Assumes dock is stopped (taskbar stays hidden). Shows taskbar, scans, invokes, hides.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PT2 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$trayH = [PT2]::FindWindow("Shell_TrayWnd", $null)
[PT2]::ShowWindow($trayH, 5) | Out-Null
Start-Sleep -Milliseconds 2000
Write-Host ("taskbar visible={0}" -f ([PT2]::IsWindowVisible($trayH)))

$danPids = @{}
Get-Process -Name 'DingTalk' -ErrorAction SilentlyContinue | ForEach-Object { $danPids[$_.Id] = $true }

$root = [System.Windows.Automation.AutomationElement]::RootElement
$all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
Write-Host ("root descendants: {0}" -f $all.Count)
$targets = New-Object System.Collections.ArrayList
foreach ($e in $all) {
  $cls = $e.Current.ClassName
  if (-not $cls) { continue }
  if ($cls.Contains("SystemTray")) {
    $n = $e.Current.Name
    $pidv = $e.Current.ProcessId
    $ctrl = $e.Current.ControlType.ProgrammaticName
    Write-Host ("  SYSTEMTRAY cls=[{0}] name=[{1}] pid={2} type={3}" -f $cls, $n, $pidv, $ctrl)
    # capture suspicious ones for later invoke
    if ($danPids.ContainsKey([int]$pidv)) { [void]$targets.Add($e) }
    elseif ($n -and ($n -match '钉钉|DingTalk|dingtalk')) { [void]$targets.Add($e) }
  }
}
Write-Host ("candidate target count: {0}" -f $targets.Count)
foreach ($t in $targets) {
  Write-Host ("  candidate: name=[{0}] pid={1} cls={2}" -f $t.Current.Name, $t.Current.ProcessId, $t.Current.ClassName)
}
# invoke candidate
$did = $false
foreach ($t in $targets) {
  try {
    $inv = $t.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    if ($inv) { $inv.Invoke(); $did = $true; Write-Host "invoked via InvokePattern"; break }
  } catch {}
  try {
    $leg = $t.GetCurrentPattern([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern)
    if ($leg) { $leg.DoDefaultAction(); $did = $true; Write-Host "invoked via LegacyIAccessible"; break }
  } catch {}
}
Write-Host ("invoked: {0}" -f $did)
Start-Sleep -Milliseconds 1500
# check DingTalk windows now
$vis = @()
[System.Windows.Automation.AutomationElement]::RawViewWalker -eq $null | Out-Null
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class VW2 {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
}
"@
$visCount = 0
[VW2]::EnumWindows({ param($h,$lp)
  $wpid = 0; [VW2]::GetWindowThreadProcessId($h,[ref]$wpid) | Out-Null
  if ($danPids.ContainsKey([int]$wpid)) {
    if ([VW2]::IsWindowVisible($h)) {
      $cls = New-Object Text.StringBuilder 64; [VW2]::GetClassName($h,$cls,64) | Out-Null
      $r = New-Object VW2+RECT; [VW2]::GetWindowRect($h,[ref]$r) | Out-Null
      $w = $r.R - $r.L; $h2 = $r.B - $r.T
      if ($w -gt 0 -and $h2 -gt 20) { $visCount++; Write-Host ("  VISIBLE pid=$wpid cls={0} {1}x{2}" -f $cls,$w,$h2) }
    }
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
Write-Host ("visible DingTalk windows: {0}" -f $visCount)
