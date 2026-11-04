# Measure XAML tray island resurrection: hide taskbar (+workarea), wait, show, scan at intervals.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TS {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetRectEmpty(out RECT r);
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint action, uint p, ref RECT r, uint flags);
    public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$trayH = [TS]::FindWindow("Shell_TrayWnd", $null)
function Scan($label) {
  $root = [System.Windows.Automation.AutomationElement]::RootElement
  $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, "Shell_TrayWnd")
  $trayEl = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
  if (-not $trayEl) { Write-Host ("[$label] NOT FOUND"); return }
  $all = $trayEl.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
  $sys = 0
  foreach ($e in $all) { $c = $e.Current.ClassName; if ($c -and $c.Contains("SystemTray")) { $sys++ } }
  Write-Host ("[$label] descendants={0} systemtray={1}" -f $all.Count, $sys)
}
# -- phase 0: currently taskbar visible; hide it like the dock does --
[TS]::ShowWindow($trayH, 0) | Out-Null
Write-Host "taskbar hidden, waiting 30s (warm-ish)"
Start-Sleep -Seconds 30
[TS]::ShowWindow($trayH, 5) | Out-Null
Write-Host "shown again"
Scan "warm+0"
Start-Sleep -Seconds 2
Scan "warm+2"
Start-Sleep -Seconds 5
Scan "warm+7"
Start-Sleep -Seconds 10
Scan "warm+17"
Start-Sleep -Seconds 20
Scan "warm+37"
Start-Sleep -Seconds 30
Scan "warm+67"
