# probe_menu_drive.ps1 -- drive the Dock's real context menu end to end
#   * WM_RBUTTONUP posted to the dock window with the client coords of its blank
#     area (left shadow margin, x=10) -> ShowBlankContextMenu runs, TrackPopupMenu
#     blocks the dock thread with the real Win32 menu
#   * Down x3 + Enter picks the 3rd entry (entries: auto-collapse, run log,
#     KEEP TOP, exit) -> the menu command path is exercised end to end
#   * the effect is read back from the keep-top registry flag
# The flag is restored at the end.
#
# Harness notes (learned the hard way):
#   * injected mouse moves update the cursor but never reach the dock's
#     WH_MOUSE_LL hook in this VM, so the dock cannot be "woken" by injection;
#     posting WM_RBUTTONUP straight to the window sidesteps the hook entirely.
#   * the dock bar is centred, so the horizontal centre is usually an ICON; the
#     left shadow margin (client x ~10) is reliably blank.
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class DM {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", SetLastError=true)] public static extern bool SystemParametersInfoW(uint a, uint b, out RECT r, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(string cls) { return FindWindowW(cls, null); }
  public static string Cls(IntPtr h) { var sb = new StringBuilder(128); GetClassNameW(h, sb, 128); return sb.ToString(); }
  public static string Rect(RECT r) { return string.Format("({0},{1},{2},{3})", r.L, r.T, r.R, r.B); }
  public static RECT Wa() { RECT r; SystemParametersInfoW(0x0030, 0, out r, 0); return r; }
  public static IntPtr PackPoint(int x, int y) { return (IntPtr)((y << 16) | (x & 0xFFFF)); }
  public static void Down() { keybd_event(0x28, 0, 0, UIntPtr.Zero); keybd_event(0x28, 0, 2, UIntPtr.Zero); System.Threading.Thread.Sleep(120); }
  public static void Enter() { keybd_event(0x0D, 0, 0, UIntPtr.Zero); keybd_event(0x0D, 0, 2, UIntPtr.Zero); System.Threading.Thread.Sleep(300); }
  public static void Esc() { keybd_event(0x1B, 0, 0, UIntPtr.Zero); keybd_event(0x1B, 0, 2, UIntPtr.Zero); System.Threading.Thread.Sleep(200); }
}
'@ -Language CSharp

$REG = 'HKCU:\Software\DesktopSuite\TopBar'
function Get-KeepTop { $v = (Get-ItemProperty -Path $REG -Name 'KeepTop' -ErrorAction SilentlyContinue).KeepTop; if ($null -eq $v) { return 0 } return [int]$v }
function Set-KeepTop([int]$on) {
    if (-not (Test-Path $REG)) { New-Item -Path $REG -Force | Out-Null }
    New-ItemProperty -Path $REG -Name 'KeepTop' -Value $on -PropertyType DWord -Force | Out-Null
}

$dock = [DM]::Find('DesktopDockWindow')
if ($dock -eq [IntPtr]::Zero) { "dock not running"; exit 1 }
$r = New-Object DM+RECT; [void][DM]::GetWindowRect($dock, [ref]$r)
"dock = " + [DM]::Rect($r) + "  size = " + ($r.R - $r.L) + "x" + ($r.B - $r.T)

$blankX = 10                     # left shadow margin: never an icon
$blankY = [int](($r.B - $r.T) * 0.7)
"posting WM_RBUTTONUP client=($blankX,$blankY)"
[void][DM]::PostMessageW($dock, 0x0205, [IntPtr]::Zero, [DM]::PackPoint($blankX, $blankY))
Start-Sleep -Milliseconds 1000
"foreground = " + [DM]::Cls([DM]::GetForegroundWindow()) + "  (DesktopDockWindow = the menu is up)"

$before = Get-KeepTop
"KeepTop before = $before"
[DM]::Down()   # entry 1: auto-collapse
[DM]::Down()   # entry 2: run log
[DM]::Down()   # entry 3: KEEP TOP
Start-Sleep -Milliseconds 200
[DM]::Enter()
Start-Sleep -Milliseconds 1500
$after = Get-KeepTop
"KeepTop after  = $after"
if ($after -ne $before) {
    "PASS  dock blank-area menu entry toggled keep-top ($before -> $after)"
} else {
    "FAIL  keep-top flag unchanged (menu selection did not land on the entry)"
    [DM]::Esc()
}
"work area = " + [DM]::Rect([DM]::Wa())
Set-KeepTop $before
Start-Sleep -Milliseconds 3000
"restored KeepTop = " + (Get-KeepTop) + "  work area = " + [DM]::Rect([DM]::Wa())
