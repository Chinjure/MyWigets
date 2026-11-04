# Probe: show taskbar, wait N seconds, then scan UIA for SystemTray elements.
# Repeats: waits 3s, 10s, 20s increments and reports state each time.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TU {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int hh, uint f);
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$trayH = [TU]::FindWindow("Shell_TrayWnd", $null)
[TU]::ShowWindow($trayH, 5) | Out-Null
Write-Host ("shown visible={0}" -f ([TU]::IsWindowVisible($trayH)))

function Scan($label) {
  $root = [System.Windows.Automation.AutomationElement]::RootElement
  $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, "Shell_TrayWnd")
  $trayEl = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
  if (-not $trayEl) { Write-Host ("[$label] Shell_TrayWnd UIA: NOT FOUND"); return }
  $all = $trayEl.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
  $sys = 0
  foreach ($e in $all) { $c = $e.Current.ClassName; if ($c -and $c.Contains("SystemTray")) { $sys++ } }
  Write-Host ("[$label] trayEl ok, descendants={0} systemtray={1}" -f $all.Count, $sys)
}
Scan "t+0"
Start-Sleep -Seconds 5
Scan "t+5"
Start-Sleep -Seconds 10
Scan "t+15"
Start-Sleep -Seconds 15
Scan "t+30"
# force layout refresh trick
[TU]::SetWindowPos($trayH, [IntPtr]::Zero, 0, 1064, 1707, 48, 0x0014) | Out-Null
Start-Sleep -Seconds 3
Scan "after-setpos"
