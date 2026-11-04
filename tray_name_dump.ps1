# Dump tray button names to UTF-8 file (console codepage mangles Chinese)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TN {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
}
"@
$trayH = [TN]::FindWindow("Shell_TrayWnd", $null)
[TN]::ShowWindow($trayH, 5) | Out-Null
Start-Sleep -Milliseconds 500
$sb = New-Object System.Text.StringBuilder
$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, "Shell_TrayWnd")
$trayEl = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if ($trayEl) {
  $all = $trayEl.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
  foreach ($e in $all) {
    $c = $e.Current.ClassName
    if ($c -and $c.Contains("SystemTray")) {
      [void]$sb.AppendLine(("cls=" + $c + " | name=[" + $e.Current.Name + "] | pid=" + $e.Current.ProcessId))
    }
  }
}
[System.IO.File]::WriteAllText("C:\Users\may\Desktop\clock\tray_names.txt", $sb.ToString(), [System.Text.Encoding]::UTF8)
Write-Host "written"
