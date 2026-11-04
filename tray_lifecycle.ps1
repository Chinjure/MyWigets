# Long observation: taskbar visible throughout, scan every 2s for 180s, report transitions.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class LT {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint b, ref RECT r, uint f);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$trayH = [LT]::FindWindow("Shell_TrayWnd", $null)
[LT]::ShowWindow($trayH, 5) | Out-Null
$wa = New-Object LT+RECT
$wa.L = 0; $wa.T = 0; $wa.R = 1707; $wa.B = 1065
[LT]::SystemParametersInfo(0x0030, 0, [ref]$wa, 0) | Out-Null
$last = -1
for ($t = 0; $t -lt 90; $t++) {
  Start-Sleep -Seconds 2
  $root = [System.Windows.Automation.AutomationElement]::RootElement
  $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, "Shell_TrayWnd")
  $trayEl = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
  $sys = -1
  if ($trayEl) {
    $all = $trayEl.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    $sys = 0
    foreach ($e in $all) { $c = $e.Current.ClassName; if ($c -and $c.Contains("SystemTray")) { $sys++ } }
  }
  if ($sys -ne $last) { Write-Host ("t={0}s state={1} (vis={2})" -f ($t*2), $sys, ([LT]::IsWindowVisible($trayH))); $last = $sys }
}
Write-Host "done"
