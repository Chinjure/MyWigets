# Overflow tray probe: show taskbar, open overflow panel, UIA enumerate, find weixin, invoke
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class OV {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr c, string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

# ensure taskbar visible
$tray = [OV]::FindWindow("Shell_TrayWnd", $null)
if (-not [OV]::IsWindowVisible($tray)) { [OV]::ShowWindow($tray, 5) | Out-Null; Start-Sleep 2 }
Write-Host "taskbar shown"

# open overflow panel
$ovf = [OV]::FindWindow("TopLevelWindowForOverflowXamlIsland", $null)
Write-Host ("overflow window: {0} visible={1}" -f $ovf, ([OV]::IsWindowVisible($ovf)))
if ($ovf -eq [IntPtr]::Zero) { Write-Host "no overflow window"; exit }
if (-not [OV]::IsWindowVisible($ovf)) { [OV]::ShowWindow($ovf, 4) | Out-Null; Start-Sleep 1 }

# enumerate its content bridge
$inner = [OV]::FindWindowEx($ovf, [IntPtr]::Zero, "Windows.UI.Composition.DesktopWindowContentBridge", $null)
Write-Host ("inner bridge: {0}" -f $inner)
$el = [System.Windows.Automation.AutomationElement]::FromHandle($inner)
$children = $el.FindAll([System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition)
Write-Host ("children: {0}" -f $children.Count)
$i = 0
foreach ($c in $children) {
  $n = $c.Current.Name
  $pid = $c.Current.ProcessId
  $b = $c.Current.BoundingRectangle
  Write-Host ("  [{0}] name=[{1}] pid={2} bounds={3},{4} {5}x{6} type={7}" -f $i, $n, $pid, [int]$b.X, [int]$b.Y, [int]$b.Width, [int]$b.Height, $c.Current.ControlType.ProgrammaticName)
  $i++
}
# find weixin by process name
$wx = $children | Where-Object { (Get-Process -Id $_.Current.ProcessId -ErrorAction SilentlyContinue).ProcessName -eq "Weixin" } | Select-Object -First 1
if ($wx) {
  Write-Host "FOUND weixin icon in overflow, invoking..."
  try {
    $inv = $wx.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    if ($inv) { $inv.Invoke(); Write-Host "InvokePattern OK" }
    else { Write-Host "no InvokePattern" }
  } catch { Write-Host ("invoke err: {0}" -f $_.Exception.Message) }
  Start-Sleep 3
  $wxProcs = Get-Process Weixin -ErrorAction SilentlyContinue
  Write-Host ("weixin procs: {0}" -f ($wxProcs.Count))
} else {
  Write-Host "weixin icon NOT in overflow children"
}
# close overflow
[OV]::ShowWindow($ovf, 0) | Out-Null
