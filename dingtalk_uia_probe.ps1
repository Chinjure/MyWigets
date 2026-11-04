# Probe: find DingTalk tray icon element in Win11 XAML tray via UIA
# Shows taskbar temporarily (dock normally hides it), scans, then re-hides.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PT {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$trayH = [PT]::FindWindow("Shell_TrayWnd", $null)
if ($trayH -eq [IntPtr]::Zero) { Write-Host "no taskbar"; exit 1 }
$wasVisible = [PT]::IsWindowVisible($trayH)
if (-not $wasVisible) { [PT]::ShowWindow($trayH, 5) | Out-Null; Start-Sleep -Milliseconds 1200 }
Write-Host ("taskbar hwnd={0} wasVisible={1} nowVisible={2}" -f $trayH, $wasVisible, ([PT]::IsWindowVisible($trayH)))

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, "Shell_TrayWnd")
$trayEl = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if (-not $trayEl) { Write-Host "Shell_TrayWnd element not found"; exit 1 }

$all = $trayEl.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
Write-Host ("Shell_TrayWnd descendants: {0}" -f $all.Count)
$danPids = @(4636,5268,5704,8096,13320,14300)
foreach ($e in $all) {
  $cls = $e.Current.ClassName
  if ($cls -and $cls.Contains("SystemTray")) {
    $n = $e.Current.Name
    $pidv = $e.Current.ProcessId
    $ctrl = $e.Current.ControlType.ProgrammaticName
    Write-Host ("  SYSTEMTRAY class=[{0}] name=[{1}] pid={2} type={3}" -f $cls, $n, $pidv, $ctrl)
  } elseif ($e.Current.ProcessId -in $danPids) {
    $cls = $e.Current.ClassName
    $n = $e.Current.Name
    Write-Host ("  DINGPID class=[{0}] name=[{1}] pid={2}" -f $cls, $n, $e.Current.ProcessId)
  }
}
if (-not $wasVisible) { [PT]::ShowWindow($trayH, 0) | Out-Null; Write-Host "taskbar re-hidden" }
