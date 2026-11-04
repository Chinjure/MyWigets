# Probe: UIA tree of TrayNotifyWnd directly while taskbar is HIDDEN
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class PH2 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr c, string cls, string name);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$trayH = [PH2]::FindWindow("Shell_TrayWnd", $null)
Write-Host ("taskbar hwnd={0} visible={1}" -f $trayH, ([PH2]::IsWindowVisible($trayH)))
$tn = [PH2]::FindWindowEx($trayH, [IntPtr]::Zero, "TrayNotifyWnd", $null)
Write-Host ("TrayNotifyWnd={0}" -f $tn)

# 1. direct UIA from TrayNotifyWnd handle
$tnEl = [System.Windows.Automation.AutomationElement]::FromHandle($tn)
$all1 = $tnEl.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
Write-Host ("TrayNotifyWnd direct UIA descendants: {0}" -f $all1.Count)
$i = 0
foreach ($e in $all1) {
  $n = $e.Current.Name
  if ($n) { Write-Host ("   [{0}] name=[{1}] pid={2} type={3}" -f $i, $n, $e.Current.ProcessId, $e.Current.ControlType.ProgrammaticName); $i++ }
}

# 2. from Shell_TrayWnd direct UIA
$trayEl = [System.Windows.Automation.AutomationElement]::FromHandle($trayH)
$all2 = $trayEl.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
Write-Host ("Shell_TrayWnd direct UIA descendants: {0}" -f $all2.Count)
