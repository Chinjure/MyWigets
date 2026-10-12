# tests/dock_band_reserve_test.ps1
#
# Verifies the always-visible dock band reservation (src/dock_main.cpp
# KeepTopTargetWorkArea / KeepTopDockReserve, semantics in src/keep_top.h).
#
# Rules under test:
#   * auto-collapse OFF (dock always visible) -> the work area bottom reserves the
#     dock band, regardless of the "keep top bar" switch:
#       work area bottom = dock glass top - 2 logical px, so a maximized window
#       stops above the WS_EX_TOPMOST dock instead of being covered by it;
#   * auto-collapse ON (dock parks off-screen) -> no reservation (work area is the
#     whole monitor, a maximized window fills it);
#   * "keep top bar" ON additionally reserves the top bar height; with it OFF the
#     work area top stays at the monitor top.
#
# Measurement notes:
#   * the test process calls SetProcessDPIAware(), so work-area / window rects are
#     physical pixels, the same units the app uses;
#   * the dock glass top is derived from the dock window rect plus dock_main.cpp's
#     draw constants (same constants as tests/dock_hover_probe.ps1);
#   * the maximized window is a self-created WinForms form with ShowInTaskbar=true
#     (tool windows are excluded by the dock's "refit maximized windows" pass);
#     its DWM extended frame bounds give the visible content rect.
#
# Both paths are covered: maximizing first and then changing the config (the dock
# must refit the already-maximized window), and changing the config first (Windows
# lays the maximized window out against the new work area).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tests/dock_band_reserve_test.ps1
# Exit code = number of failed checks. KeepTop / AutoCollapse are restored at the end.

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class DB {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SystemParametersInfoW(uint a, uint b, out RECT r, uint f);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassNameW(h, s, 256); return s.ToString(); }
  public static IntPtr Find(string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr l) { if (Cls(h) == cls) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static RECT Wa() { RECT r; SystemParametersInfoW(0x0030, 0, out r, 0); return r; }
  public static RECT Frame(IntPtr h) {
    RECT r; DwmGetWindowAttribute(h, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/, out r, 16); return r;
  }
  public static string Rect(RECT r) { return string.Format("({0},{1},{2},{3}) {4}x{5}", r.L, r.T, r.R, r.B, r.R - r.L, r.B - r.T); }
}
'@

Add-Type -AssemblyName System.Windows.Forms

[void][DB]::SetProcessDPIAware()

$REG_TOPBAR = 'HKCU:\Software\DesktopSuite\TopBar'
$REG_DOCK   = 'HKCU:\Software\DesktopSuite\Dock'
$fails = 0

function Check($name, $cond, $detail) {
    if ($cond) { "PASS  $name  ($detail)" }
    else { $script:fails++; "FAIL  $name  ($detail)" }
}

function Reg-Dword($path, $name, $default) {
    if (-not (Test-Path $path)) { return $default }
    $v = (Get-ItemProperty -Path $path -Name $name -ErrorAction SilentlyContinue).$name
    if ($null -eq $v) { return $default }
    return [int]$v
}
function Set-Dword($path, $name, $value) {
    if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
    New-ItemProperty -Path $path -Name $name -Value $value -PropertyType DWord -Force | Out-Null
}

# The dock re-reads the registry every 2s (ReloadDockConfigFromRegistry / KeepTop
# poll), then recomputes the work area and refits maximized windows.
function Wait-DockSync { Start-Sleep -Milliseconds 3200 }

$dock = [DB]::Find('DesktopDockWindow')
if ($dock -eq [IntPtr]::Zero) { throw 'Dock is not running (DesktopDockWindow not found)' }
$bar = [DB]::Find('DesktopTopBarWindow')

$dpi = [DB]::GetDpiForWindow($dock)
$k = $dpi / 96.0
$SW = [DB]::GetSystemMetrics(0)
$SH = [DB]::GetSystemMetrics(1)
# dock_main.cpp drawing constants (same source as tests/dock_hover_probe.ps1)
$kShadowBottom = 7.0; $kPadTop = 5.0; $kIconSize = 44.0; $kPadBottom = 8.0
$gapReservePx = [int][math]::Round(2 * $k)   # keeptop::kMaxWindowToDockGapBase

function Get-DockRect { $r = New-Object DB+RECT; [void][DB]::GetWindowRect($dock, [ref]$r); return $r }
# Dock glass top (screen physical px): BodyRectFixed.Y plus the window top edge
function Get-GlassTop {
    $r = Get-DockRect
    $winH = $r.B - $r.T
    $contentH = ($kPadTop + $kIconSize + $kPadBottom) * $k
    return $r.T + [int][math]::Round($winH - $kShadowBottom * $k - $contentH)
}

$keepTop0 = Reg-Dword $REG_TOPBAR 'KeepTop' 0
$auto0    = Reg-Dword $REG_DOCK   'AutoCollapse' 1
"=== setup ==="
"screen        = ${SW}x${SH}  dpi=$dpi (k=$k)"
"registry      = KeepTop=$keepTop0 AutoCollapse=$auto0"
"dock window   = " + [DB]::Rect((Get-DockRect))
"work area     = " + [DB]::Rect([DB]::Wa())
""

# ---- self-created maximized window: follows config changes, so the dock's
# ---- "refit already-maximized windows" path is exercised as well ----
$form = New-Object System.Windows.Forms.Form
$form.Text = 'DockBandReserveProbe'
$form.StartPosition = 'Manual'
$form.Left = 60; $form.Top = 60; $form.Width = 700; $form.Height = 480
$form.Show()
for ($i = 0; $i -lt 6; $i++) { [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 80 }
$form.WindowState = 'Maximized'
for ($i = 0; $i -lt 8; $i++) { [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 100 }
$fh = $form.Handle
function Get-FormFrame { return [DB]::Frame($fh) }

try {
    "=== P1 keepTop=0 autoCollapse=0 -> the dock band must be reserved ==="
    Set-Dword $REG_TOPBAR 'KeepTop' 0
    Set-Dword $REG_DOCK   'AutoCollapse' 0
    Wait-DockSync
    $wa = [DB]::Wa()
    $glass = Get-GlassTop
    $fr = Get-FormFrame
    "work area     = " + [DB]::Rect($wa)
    "dock window   = " + [DB]::Rect((Get-DockRect))
    "glass top     = $glass (expect work area bottom = glass - $gapReservePx)"
    "max window    = " + [DB]::Rect($fr)

    Check "P1 no top reserve when keep-top is off" ($wa.T -eq 0) ("wa.top=" + $wa.T)
    Check "P1 dock band reserved below the screen bottom" ($wa.B -le ($SH - 40)) ("wa.bottom=" + $wa.B + " screenH=" + $SH)
    Check "P1 work area bottom == dock glass top - reserve gap" ([math]::Abs($wa.B - ($glass - $gapReservePx)) -le 3) ("wa.bottom=" + $wa.B + " glass=" + $glass + " expected=" + ($glass - $gapReservePx))
    # The reserved band must be the dock's own band: the work area bottom cuts
    # through the dock window rect, below its top edge (which carries the
    # transparent shadow/growth margin) and above the visible glass top.
    Check "P1 work area bottom cuts through the dock band" ((($wa.B -gt (Get-DockRect).T) -and ($wa.B -lt $glass))) ("wa.bottom=" + $wa.B + " dock.top=" + (Get-DockRect).T + " glass=" + $glass)
    Check "P1 maximized window stops at the work area bottom" ([math]::Abs($fr.B - $wa.B) -le 3) ("frame.bottom=" + $fr.B + " wa.bottom=" + $wa.B)
    Check "P1 maximized window does not reach the dock glass" ($fr.B -le ($glass - $gapReservePx)) ("frame.bottom=" + $fr.B + " glass=" + $glass)
    Check "P1 maximized window spans the full width" (($fr.L -le 1) -and ($fr.R -ge ($SW - 1))) ("frame=" + [DB]::Rect($fr))
    ""

    "=== P2 autoCollapse=1 (dock parks off-screen) -> no reserve, window refits full ==="
    Set-Dword $REG_DOCK 'AutoCollapse' 1
    Wait-DockSync
    $wa = [DB]::Wa()
    $fr = Get-FormFrame
    "work area     = " + [DB]::Rect($wa)
    "max window    = " + [DB]::Rect($fr)
    Check "P2 work area back to the whole monitor" (($wa.T -eq 0) -and ($wa.B -eq $SH) -and ($wa.L -eq 0) -and ($wa.R -eq $SW)) ("wa=" + [DB]::Rect($wa))
    Check "P2 already-maximized window refits to the full screen" (($fr.T -le 1) -and ($fr.B -ge ($SH - 1))) ("frame=" + [DB]::Rect($fr))
    ""

    "=== P3 keepTop=1 autoCollapse=0 -> top bar + dock band reserved together ==="
    Set-Dword $REG_DOCK 'AutoCollapse' 0
    Set-Dword $REG_TOPBAR 'KeepTop' 1
    Wait-DockSync
    $wa = [DB]::Wa()
    $barH = 0
    if ($bar -ne [IntPtr]::Zero) {
        $br = New-Object DB+RECT; [void][DB]::GetWindowRect($bar, [ref]$br); $barH = $br.B - $br.T
    }
    $glass = Get-GlassTop
    $fr = Get-FormFrame
    "bar height    = $barH"
    "work area     = " + [DB]::Rect($wa)
    "max window    = " + [DB]::Rect($fr)
    Check "P3 work area top == top bar height" ($wa.T -eq $barH) ("wa.top=" + $wa.T + " barH=" + $barH)
    Check "P3 dock band still reserved" ([math]::Abs($wa.B - ($glass - $gapReservePx)) -le 3) ("wa.bottom=" + $wa.B + " expected=" + ($glass - $gapReservePx))
    Check "P3 maximized window between bar and dock" (($fr.T -ge ($barH - 3)) -and ([math]::Abs($fr.B - $wa.B) -le 3)) ("frame=" + [DB]::Rect($fr) + " wa=" + [DB]::Rect($wa))
    ""
}
finally {
    if ($form) { $form.Close(); [System.Windows.Forms.Application]::DoEvents() }
    Set-Dword $REG_TOPBAR 'KeepTop' $keepTop0
    Set-Dword $REG_DOCK   'AutoCollapse' $auto0
    Start-Sleep -Milliseconds 3200
    "=== cleanup ==="
    "registry restored: KeepTop=$keepTop0 AutoCollapse=$auto0"
    "work area     = " + [DB]::Rect([DB]::Wa())
}

""
"=== summary: $fails failure(s) ==="
exit $fails
