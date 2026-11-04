# Scan dock icon positions by clicking and reading dock log
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class SC {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
}
"@
$log = "C:\Users\may\Desktop\clock\logs\dock.log"
$pos = (Get-Item $log).Length
function NewLog {
  $f = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
  $f.Seek($pos, 'Begin') | Out-Null
  $r = New-Object System.IO.StreamReader($f, [System.Text.Encoding]::UTF8)
  $txt = $r.ReadToEnd()
  $r.Close(); $f.Close()
  $script:pos = (Get-Item $log).Length
  return $txt
}
foreach ($x in @(1100, 1150, 1200, 1250, 1300)) {
  [SC]::SetCursorPos($x, 1015) | Out-Null
  Start-Sleep 0.3
  [SC]::mouse_event(0x02, 0, 0, 0, [UIntPtr]::Zero) | Out-Null
  [SC]::mouse_event(0x04, 0, 0, 0, [UIntPtr]::Zero) | Out-Null
  Start-Sleep 1
  $new = NewLog
  if ($new.Trim()) { Write-Host ("x={0} -> {1}" -f $x, ($new.Trim() -replace "`n", " | ")) }
  else { Write-Host ("x={0} -> (no dock action)" -f $x) }
}
[SC]::SetCursorPos(400, 400) | Out-Null
