# Manual overflow flow: find overflow button in main tray UIA, invoke, find panel, enumerate
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class OF2 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr c, string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$tray = [OF2]::FindWindow("Shell_TrayWnd", $null)
if (-not [OF2]::IsWindowVisible($tray)) { [OF2]::ShowWindow($tray, 5) | Out-Null; Start-Sleep 2 }

# find bridges under tray
function FB($parent, [System.Collections.ArrayList]$acc) {
  $c = [OF2]::FindWindowEx($parent, [IntPtr]::Zero, $null, $null)
  while ($c -ne [IntPtr]::Zero) {
    $cls = New-Object Text.StringBuilder 128
    $null = [OF2]::GetClassName($c, $cls, 128)
    if ($cls.ToString() -eq "Windows.UI.Composition.DesktopWindowContentBridge") { [void]$acc.Add($c) }
    FB $c $acc
    $c = [OF2]::FindWindowEx($parent, $c, $null, $null)
  }
}
$bridges = New-Object System.Collections.ArrayList
FB $tray $bridges
Write-Host ("bridges: {0}" -f $bridges.Count)

# find overflow button (name contains 隐藏/hidden/overflow)
$btn = $null
foreach ($b in $bridges) {
  $el = [System.Windows.Automation.AutomationElement]::FromHandle($b)
  if (-not $el) { continue }
  $all = $el.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
  foreach ($e in $all) {
    $n = $e.Current.Name
    if ($n -and ($n -match "隐藏" -or $n -match "hidden" -or $n -match "overflow")) {
      Write-Host ("overflow button candidate: name=[{0}]" -f $n)
      if (-not $btn) { $btn = $e }
    }
  }
}
if (-not $btn) { Write-Host "NO overflow button found"; exit }
# invoke it
try {
  $p = $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
  if ($p) { $p.Invoke(); Write-Host "overflow button invoked" }
  else { Write-Host "button has no InvokePattern" }
} catch { Write-Host ("invoke error: {0}" -f $_.Exception.Message) }
Start-Sleep 1.5

# find panel window
$ovf = [OF2]::FindWindow("TopLevelWindowForOverflowXamlIsland", $null)
Write-Host ("overflow panel: {0}" -f $ovf)
if ($ovf -ne [IntPtr]::Zero) {
  $inner = [OF2]::FindWindowEx($ovf, [IntPtr]::Zero, "Windows.UI.Composition.DesktopWindowContentBridge", $null)
  Write-Host ("panel inner bridge: {0}" -f $inner)
  if ($inner -ne [IntPtr]::Zero) {
    $el = [System.Windows.Automation.AutomationElement]::FromHandle($inner)
    $kids = $el.FindAll([System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition)
    Write-Host ("panel children: {0}" -f $kids.Count)
    $i = 0
    foreach ($k in $kids) {
      $n = $k.Current.Name
      $pid = $k.Current.ProcessId
      $b = $k.Current.BoundingRectangle
      Write-Host ("  [{0}] name=[{1}] pid={2} {3}x{4}" -f $i, $n, $pid, [int]$b.Width, [int]$b.Height)
      $i++
    }
    # find weixin
    $wx = $kids | Where-Object { (Get-Process -Id $_.Current.ProcessId -ErrorAction SilentlyContinue).ProcessName -eq "Weixin" } | Select-Object -First 1
    if ($wx) { Write-Host "WEIXIN FOUND IN PANEL" } else { Write-Host "weixin not in panel children" }
  }
}
# close panel
if ($ovf -ne [IntPtr]::Zero) { [OF2]::ShowWindow($ovf, 0) | Out-Null }
