# Probe: is UIA tree of DesktopWindowContentBridge accessible while taskbar is HIDDEN (dock running)?
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class PH {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr c, string cls, string name);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$trayH = [PH]::FindWindow("Shell_TrayWnd", $null)
Write-Host ("taskbar hwnd={0} visible={1}" -f $trayH, ([PH]::IsWindowVisible($trayH)))
$tn = [PH]::FindWindowEx($trayH, [IntPtr]::Zero, "TrayNotifyWnd", $null)
Write-Host ("TrayNotifyWnd={0}" -f $tn)
if ($tn -eq [IntPtr]::Zero) { Write-Host "no TrayNotifyWnd"; exit }

# recursive find DesktopWindowContentBridge under TrayNotifyWnd
function Find-Bridges($parent, [System.Collections.ArrayList]$acc) {
  $child = [PH]::FindWindowEx($parent, [IntPtr]::Zero, $null, $null)
  while ($child -ne [IntPtr]::Zero) {
    $cls = New-Object Text.StringBuilder 128
    $null = [PH]::GetClassName($child, $cls, 128)
    if ($cls.ToString() -eq "Windows.UI.Composition.DesktopWindowContentBridge") { [void]$acc.Add($child) }
    Find-Bridges $child $acc
    $child = [PH]::FindWindowEx($parent, $child, $null, $null)
  }
}
$bridges = New-Object System.Collections.ArrayList
Find-Bridges $tn $bridges
Write-Host ("bridges under TrayNotifyWnd: {0}" -f $bridges.Count)

$total = 0
foreach ($b in $bridges) {
  try {
    $el = [System.Windows.Automation.AutomationElement]::FromHandle($b)
    if ($el) {
      $all = $el.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
      $total += $all.Count
      Write-Host ("bridge {0}: descendants={1}" -f $b, $all.Count)
      $i = 0
      foreach ($e in $all) {
        $n = $e.Current.Name
        if ($n -and $i -lt 8) { Write-Host ("   name=[{0}] pid={1}" -f $n, $e.Current.ProcessId); $i++ }
      }
    }
  } catch {}
}
Write-Host ("TOTAL elements: {0}" -f $total)
