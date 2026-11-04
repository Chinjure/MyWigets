# Timeline: from dead island, show taskbar, scan every 1s for 60s.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TL {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint b, ref RECT r, uint f);
    public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$trayH = [TL]::FindWindow("Shell_TrayWnd", $null)
# ensure hidden + restored workarea to taskbar values
[TL]::ShowWindow($trayH, 0) | Out-Null
$wa = New-Object TL+RECT
$wa.L = 0; $wa.T = 0; $wa.R = 1707; $wa.B = 1065
[TL]::SystemParametersInfo(0x0030, 0, [ref]$wa, 0) | Out-Null
Start-Sleep -Seconds 5
[TL]::ShowWindow($trayH, 5) | Out-Null
for ($t = 0; $t -lt 60; $t++) {
  Start-Sleep -Milliseconds 1000
  $root = [System.Windows.Automation.AutomationElement]::RootElement
  $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, "Shell_TrayWnd")
  $trayEl = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
  if (-not $trayEl) { Write-Host ("t=$t notfound"); continue }
  $all = $trayEl.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
  $sys = 0
  foreach ($e in $all) { $c = $e.Current.ClassName; if ($c -and $c.Contains("SystemTray")) { $sys++ } }
  if ($sys) { Write-Host ("t=$t sys=$sys") }
}
