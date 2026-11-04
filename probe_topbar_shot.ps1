# Capture topbar screenshots + 9786 connection timeline
$cs = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TB {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public struct RECT { public int L, T, R, B; }
    public static IntPtr FindByClass(string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            var c = new StringBuilder(256); GetClassName(h, c, 256);
            if (c.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
Add-Type -TypeDefinition $cs
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

$hwnd = [TB]::FindByClass("DesktopTopBarWindow")
if ($hwnd -eq [IntPtr]::Zero) { Write-Host "topbar not found"; exit 1 }
$r = New-Object TB+RECT
[TB]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
Write-Host ("topbar rect = {0},{1} {2}x{3}" -f $r.L, $r.T, $w, $h)

$out = "C:\Users\may\Desktop\clock\logs\shots"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$log = @()
for ($i = 0; $i -lt 45; $i++) {
    $conn = (netstat -ano | Select-String "127.0.0.1:9786" | Select-String "ESTABLISHED" | Select-Object -First 1)
    $state = if ($conn) { "UP" } else { "DOWN" }
    $ts = Get-Date -Format "HH:mm:ss"
    $log += "$ts $state"
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $file = Join-Path $out ("tb_{0:d2}_{1}.png" -f $i, $state)
    $bmp.Save($file)
    $g.Dispose(); $bmp.Dispose()
    Start-Sleep -Milliseconds 2000
}
$log | Set-Content -Path "C:\Users\may\Desktop\clock\logs\topbar_timeline.txt"
Write-Host ($log -join "`n")
Write-Host "done"
