# COLD wake measurement with proper work area restoration (simulating dock's ShowTaskbar flow)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TC {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint b, ref RECT r, uint f);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$trayH = [TC]::FindWindow("Shell_TrayWnd", $null)
# 1. hide taskbar (dock-style) + restore work area to taskbar-occupied value
[TC]::ShowWindow($trayH, 0) | Out-Null
$wa = New-Object TC+RECT
$wa.L = 0; $wa.T = 0; $wa.R = 1707; $wa.B = 1065
[TC]::SystemParametersInfo(0x0030, 0, [ref]$wa, 0) | Out-Null  # SPI_SETWORKAREA
Write-Host "hidden + workarea restored; waiting 60s cold"
Start-Sleep -Seconds 60

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
[TC]::ShowWindow($trayH, 5) | Out-Null
Write-Host "shown"
Scan "cold+0"
Start-Sleep -Seconds 1
Scan "cold+1"
Start-Sleep -Seconds 2
Scan "cold+3"
Start-Sleep -Seconds 5
Scan "cold+8"
Start-Sleep -Seconds 10
Scan "cold+18"
Start-Sleep -Seconds 20
Scan "cold+38"
