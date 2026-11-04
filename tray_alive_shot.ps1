# When island goes alive, screenshot bottom strip and dump names
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class SS {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint b, ref RECT r, uint f);
    public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$trayH = [SS]::FindWindow("Shell_TrayWnd", $null)
[SS]::ShowWindow($trayH, 5) | Out-Null
$wa = New-Object SS+RECT
$wa.L = 0; $wa.T = 0; $wa.R = 1707; $wa.B = 1065
[SS]::SystemParametersInfo(0x0030, 0, [ref]$wa, 0) | Out-Null
for ($t = 0; $t -lt 240; $t++) {
  Start-Sleep -Seconds 1
  $root = [System.Windows.Automation.AutomationElement]::RootElement
  $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, "Shell_TrayWnd")
  $trayEl = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
  if (-not $trayEl) { continue }
  $all = $trayEl.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
  $sys = 0
  foreach ($e in $all) { $c = $e.Current.ClassName; if ($c -and $c.Contains("SystemTray")) { $sys++ } }
  if ($sys -ge 5) {
    Write-Host ("ALIVE at t={0}s; screenshotting" -f $t)
    $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
    $crop = New-Object System.Drawing.Rectangle 0, 1010, $b.Width, 57
    $cropBmp = $bmp.Clone($crop, $bmp.PixelFormat)
    $cropBmp.Save("C:\Users\may\Desktop\clock\tray_alive_shot.png")
    $g.Dispose(); $bmp.Dispose(); $cropBmp.Dispose()
    Write-Host "saved"
    break
  }
}
Write-Host "end"
