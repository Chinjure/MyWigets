# Probe: dump Shell_TrayWnd child window tree while taskbar visible
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class TW3 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public struct RECT { public int L, T, R, B; }
}
"@
$trayH = [TW3]::FindWindow("Shell_TrayWnd", $null)
[TW3]::ShowWindow($trayH, 5) | Out-Null
Start-Sleep -Milliseconds 3000

function DumpWnd($h, $depth) {
  $cls = New-Object Text.StringBuilder 64
  [TW3]::GetClassName($h, $cls, 64) | Out-Null
  $vis = [TW3]::IsWindowVisible($h)
  $r = New-Object TW3+RECT
  [TW3]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L
  $h2 = $r.B - $r.T
  $indent = ''
  for ($i = 0; $i -lt $depth; $i++) { $indent += '  ' }
  Write-Host ($indent + $cls.ToString() + ' vis=' + $vis + ' ' + $w + 'x' + $h2)
  $child = [TW3]::GetWindow($h, 5)
  while ($child -ne [IntPtr]::Zero) {
    DumpWnd $child ($depth + 1)
    $child = [TW3]::GetWindow($child, 2)
  }
}
DumpWnd $trayH 0
