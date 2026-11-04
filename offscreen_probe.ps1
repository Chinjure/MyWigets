# Experiment: move taskbar off-screen instead of hiding -> is UIA tree still available?
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class OS {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr c, string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$trayH = [OS]::FindWindow("Shell_TrayWnd", $null)
# 1. make sure taskbar is shown first
if (-not [OS]::IsWindowVisible($trayH)) { [OS]::ShowWindow($trayH, 5) | Out-Null; Start-Sleep 2 }
$r = New-Object OS+RECT
[OS]::GetWindowRect($trayH, [ref]$r) | Out-Null
Write-Host ("taskbar rect before: {0},{1}-{2},{3}" -f $r.L, $r.T, $r.R, $r.B)
$screenH = [OS]::GetSystemMetrics(1)
# 2. move taskbar off-screen (below bottom)
[OS]::SetWindowPos($trayH, [IntPtr]::Zero, 0, $screenH + 200, ($r.R - $r.L), ($r.B - $r.T), 0x0014) | Out-Null
Start-Sleep 2
[OS]::GetWindowRect($trayH, [ref]$r) | Out-Null
Write-Host ("taskbar rect after move: {0},{1}-{2},{3} visible={4}" -f $r.L, $r.T, $r.R, $r.B, ([OS]::IsWindowVisible($trayH)))
# 3. check TrayNotifyWnd bridges
$tn = [OS]::FindWindowEx($trayH, [IntPtr]::Zero, "TrayNotifyWnd", $null)
Write-Host ("TrayNotifyWnd={0}" -f $tn)
function Find-Bridges2($parent, [System.Collections.ArrayList]$acc) {
  $child = [OS]::FindWindowEx($parent, [IntPtr]::Zero, $null, $null)
  while ($child -ne [IntPtr]::Zero) {
    $cls = New-Object Text.StringBuilder 128
    $null = [OS]::GetClassName($child, $cls, 128)
    if ($cls.ToString() -eq "Windows.UI.Composition.DesktopWindowContentBridge") { [void]$acc.Add($child) }
    Find-Bridges2 $child $acc
    $child = [OS]::FindWindowEx($parent, $child, $null, $null)
  }
}
$bridges = New-Object System.Collections.ArrayList
Find-Bridges2 $tn $bridges
Write-Host ("bridges: {0}" -f $bridges.Count)
# 4. UIA enumerate icons
$total = 0
foreach ($b in $bridges) {
  try {
    $el = [System.Windows.Automation.AutomationElement]::FromHandle($b)
    if ($el) {
      $all = $el.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
      $total += $all.Count
      foreach ($e in $all) {
        $n = $e.Current.Name
        if ($n -and $n -match "Weixin") { Write-Host ("  FOUND weixin icon: name=[{0}] pid={1}" -f $n, $e.Current.ProcessId) }
      }
    }
  } catch {}
}
Write-Host ("total UIA elements: {0}" -f $total)
# 5. restore taskbar to original position
[OS]::SetWindowPos($trayH, [IntPtr]::Zero, 0, ($screenH - 48), 1707, 48, 0x0014) | Out-Null
Start-Sleep 1
[OS]::GetWindowRect($trayH, [ref]$r) | Out-Null
Write-Host ("taskbar restored: {0},{1}-{2},{3}" -f $r.L, $r.T, $r.R, $r.B)
