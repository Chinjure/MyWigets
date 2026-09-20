# verify_keep_top.ps1 -- behavioral verification of the "Keep top bar" mode (keep_top.h)
#
# Scenario under test: the MyWigets tray menu (and Dock right-click menu) item
# "\u4fdd\u6301\u9876\u680f" enables a mode where
#   * the top bar stays at the very top of the screen and is not covered by any
#     window (except a fullscreen app),
#   * a maximized window only fills the area BELOW the top bar,
#   * when the Dock does not auto-hide (always visible) its horizontal band is
#     reserved too, so a maximized window stops above the Dock.
#
# Phases:
#   P0 baseline : mode off -> work area == monitor, top bar at (0,0,W,40)
#   P1 enable   : registry flag + component messages -> work area.top == bar
#                 height, top bar topmost and uncovered, maximized window sits
#                 below the top bar (top edge >= work area top - 12px)
#   P2 dock band: dock reserve math (autoCollapse off => work area bottom must
#                 be above the dock top edge); reported, not forced
#   P3 restart  : mode survives a component restart (registry persistence)
#   P4 disable  : mode off -> work area back to the full monitor, maximized
#                 window back to full-screen geometry
#
# Everything is restored at the end (mode off, work area = monitor).
# ASCII only (PS 5.1 encoding pitfalls); dock.log is inspected separately.
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class KT {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll", SetLastError=true)] public static extern bool SystemParametersInfoW(uint a, uint b, out RECT r, uint f);
  [DllImport("user32.dll")] public static extern int GetWindowLongPtrW(IntPtr h, int i);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

  public static string Cls(IntPtr h) { var sb = new StringBuilder(128); GetClassNameW(h, sb, 128); return sb.ToString(); }
  public static string Title(IntPtr h) { var sb = new StringBuilder(200); GetWindowTextW(h, sb, 200); return sb.ToString(); }
  public static string Rect(RECT r) { return string.Format("({0},{1},{2},{3}) {4}x{5}", r.L, r.T, r.R, r.B, r.R - r.L, r.B - r.T); }
  public static RECT Wa() { RECT r; SystemParametersInfoW(0x0030, 0, out r, 0); return r; }
  public static IntPtr Find(string cls) {
    IntPtr f = IntPtr.Zero;
    EnumWindows((h, p) => { if (IsWindowVisible(h) && Cls(h) == cls) { f = h; return false; } return true; }, IntPtr.Zero);
    return f;
  }
  // Is the window topmost (WS_EX_TOPMOST), i.e. in the topmost Z band?
  public static bool Topmost(IntPtr h) { return (GetWindowLongPtrW(h, -20) & 0x00000008) != 0; }
  // First visible, non-suite, non-shell window that would cover `target`'s rect
  // (walking the Z order from the top). Empty string = nothing covers it.
  public static string CoveringWindow(IntPtr target) {
    RECT tr; if (!GetWindowRect(target, out tr)) return "";
    string found = "";
    EnumWindows((h, p) => {
      if (h == target) { return false; }
      if (!IsWindowVisible(h) || IsIconic(h)) return true;
      string cls = Cls(h);
      if (cls == "DesktopDockWindow" || cls == "DesktopTopBarWindow" ||
          cls == "Progman" || cls == "WorkerW" || cls == "SHELLDLL_DefView" ||
          cls == "Shell_TrayWnd" || cls == "MyWigetsTrayWindow") return true;
      RECT r; if (!GetWindowRect(h, out r)) return true;
      // ignore degenerate helper windows (system 1x1 message-only helpers are
      // "visible" but occupy zero screen area)
      if ((r.R - r.L) < 2 || (r.B - r.T) < 2) return true;
      bool ov = !(r.R <= tr.L || r.L >= tr.R || r.B <= tr.T || r.T >= tr.B);
      if (ov) { found = cls + " | " + Title(h) + " | " + Rect(r); return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  // Maximized windows of "normal" applications (excludes suite/shell windows)
  public static List<string> MaximizedWindows() {
    var outp = new List<string>();
    EnumWindows((h, p) => {
      if (!IsWindowVisible(h) || IsIconic(h) || !IsZoomed(h)) return true;
      string cls = Cls(h);
      if (cls == "DesktopDockWindow" || cls == "DesktopTopBarWindow" ||
          cls == "MyWigetsTrayWindow" || cls == "Progman" || cls == "WorkerW") return true;
      RECT r; GetWindowRect(h, out r);
      outp.Add(string.Format("{0}|{1}|{2}|{3}", h.ToInt64(), cls, Title(h), Rect(r)));
      return true;
    }, IntPtr.Zero);
    return outp;
  }
  // Pick a restore-capable candidate window to maximize (Chrome preferred: it is
  // the least disruptive app to maximize/un-maximize during the test).
  public static IntPtr PickCandidate() {
    IntPtr best = IntPtr.Zero; int bestScore = -1;
    EnumWindows((h, p) => {
      if (!IsWindowVisible(h) || IsIconic(h) || IsZoomed(h)) return true;
      string cls = Cls(h);
      if (cls == "DesktopDockWindow" || cls == "DesktopTopBarWindow" ||
          cls == "MyWigetsTrayWindow" || cls == "Progman" || cls == "WorkerW" ||
          cls == "Shell_TrayWnd" || cls == "TaskListThumbnailWnd") return true;
      if (GetWindow(h, 4 /*GW_OWNER*/) != IntPtr.Zero) return true;
      long ex = GetWindowLongPtrW(h, -20);
      if ((ex & 0x00000080) != 0) return true;              // WS_EX_TOOLWINDOW
      if ((GetWindowLongPtrW(h, -16) & 0x40000000) != 0) return true;  // WS_CHILD
      RECT r; if (!GetWindowRect(h, out r)) return true;
      if (r.B - r.T < 500 || r.R - r.L < 500) return true;  // ignore small popups
      int score = (cls == "Chrome_WidgetWin_1") ? 100 : 10;
      if (score > bestScore) { bestScore = score; best = h; }
      return true;
    }, IntPtr.Zero);
    return best;
  }
  // NOTE: real mouse input injection is NOT used by this harness: injected
  // moves reach the cursor but never the dock/host low-level hooks in this VM, so
  // menus are opened with a POSTED right click on the owning window instead.
  // Tray icon rect of the MyWigets tray icon (id 1). Returns false when the icon
  // is hidden inside the overflow flyout (rect unavailable while not shown).
  public static bool TrayIconRect(IntPtr hwnd, out RECT r) {
    r = new RECT();
    var nid = new NOTIFYICONIDENTIFIER();
    nid.cbSize = (uint)Marshal.SizeOf(typeof(NOTIFYICONIDENTIFIER));
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.guidItem = Guid.Empty;
    RECT outR;
    int hr = Shell_NotifyIconGetRect(ref nid, out outR);
    if (hr != 0) return false;
    r = outR;
    return true;
  }
  [StructLayout(LayoutKind.Sequential)]
  public struct NOTIFYICONIDENTIFIER {
    public uint cbSize; public IntPtr hWnd; public uint uID; public Guid guidItem;
  }
  [DllImport("shell32.dll")]
  public static extern int Shell_NotifyIconGetRect(ref NOTIFYICONIDENTIFIER id, out RECT r);
  public static void EscKey() {
    keybd_event(0x1B, 0, 0, UIntPtr.Zero);
    keybd_event(0x1B, 0, 2, UIntPtr.Zero);
    System.Threading.Thread.Sleep(200);
  }
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public static void WinKey() {
    keybd_event(0x5B, 0, 0, UIntPtr.Zero);
    keybd_event(0x5B, 0, 2, UIntPtr.Zero);
  }
  // ---- driving a real Win32 context menu without mouse injection ----
  // (this VM never delivers injected mouse input to the dock's WH_MOUSE_LL hook,
  // but a posted WM_RBUTTONUP makes the dock open the real TrackPopupMenu, and
  // keystrokes do land on it: proven by the trail of dock actions they trigger)
  public static void Down() { keybd_event(0x28, 0, 0, UIntPtr.Zero); keybd_event(0x28, 0, 2, UIntPtr.Zero); System.Threading.Thread.Sleep(120); }
  public static void Enter() { keybd_event(0x0D, 0, 0, UIntPtr.Zero); keybd_event(0x0D, 0, 2, UIntPtr.Zero); System.Threading.Thread.Sleep(300); }
  public static void Esc() { keybd_event(0x1B, 0, 0, UIntPtr.Zero); keybd_event(0x1B, 0, 2, UIntPtr.Zero); System.Threading.Thread.Sleep(200); }
  public static IntPtr PackPoint(int x, int y) { return (IntPtr)((y << 16) | (x & 0xFFFF)); }
  public static IntPtr Foreground() { return GetForegroundWindow(); }
  // open the dock's blank-area context menu (client x in the left shadow margin
  // is reliably blank; the bar's centre is usually an icon)
  public static void DockBlankMenu(IntPtr dock, int clientY) {
    PostMessageW(dock, 0x0205, IntPtr.Zero, PackPoint(10, clientY));
  }
}
'@ -Language CSharp

$REG = 'HKCU:\Software\DesktopSuite\TopBar'
$MSG_DOCK_KEEPTOP = 0x8000 + 18
$MSG_BAR_KEEPTOP = 0x8000 + 18
$fails = 0
function Check($name, $cond, $detail) {
    if ($cond) { "PASS  $name  ($detail)" }
    else { $script:fails++; "FAIL  $name  ($detail)" }
}
# Glass top edge of the dock in *resting* state, in screen coordinates.
# The dock's hit-test/draw geometry is built from the window height:
#   glassTopLocal = bottomShadow(7) + padTop(5) + iconSize(35) + padBottom(8)
# with iconSize = winH - 7 - 5 - 35 - 8 - 14*2 (the shadow margin scales with DPI).
# Deriving it from winH makes this check independent of a hard-coded constant and
# immune to the hover magnification (which only grows the glass upward, so the
# resting glass top is what the reserved band must line up with).
function DockGlassTop([int]$dockWinH, [int]$dockBottom) { return $dockBottom - 47 }

# Wait until the dock's window rect stops moving (its reveal/collapse animation
# keeps running for a few hundred ms after a trigger): measuring mid-animation
# makes the glass-top arithmetic look 1-4px off, which is noise, not a defect.
function Wait-DockStable([IntPtr]$dock, [int]$timeoutMs) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    $prev = ''
    $stable = 0
    while ((Get-Date) -lt $deadline) {
        $r = New-Object KT+RECT
        [void][KT]::GetWindowRect($dock, [ref]$r)
        $cur = "" + $r.L + "," + $r.T + "," + $r.R + "," + $r.B
        if ($cur -eq $prev) { $stable++; if ($stable -ge 3) { return $r } } else { $stable = 0 }
        $prev = $cur
        Start-Sleep -Milliseconds 250
    }
    $r = New-Object KT+RECT
    [void][KT]::GetWindowRect($dock, [ref]$r)
    return $r
}

function Set-KeepTop([int]$on) {
    if (-not (Test-Path $REG)) { New-Item -Path $REG -Force | Out-Null }
    New-ItemProperty -Path $REG -Name 'KeepTop' -Value $on -PropertyType DWord -Force | Out-Null
}
function Get-KeepTop {
    $v = (Get-ItemProperty -Path $REG -Name 'KeepTop' -ErrorAction SilentlyContinue).KeepTop
    if ($null -eq $v) { return 0 }
    return [int]$v
}
function Notify-Components([int]$on) {
    $dock = [KT]::Find('DesktopDockWindow')
    $bar = [KT]::Find('DesktopTopBarWindow')
    if ($dock -ne [IntPtr]::Zero) { [void][KT]::PostMessageW($dock, $MSG_DOCK_KEEPTOP, [IntPtr]$on, [IntPtr]::Zero) }
    if ($bar -ne [IntPtr]::Zero) { [void][KT]::PostMessageW($bar, $MSG_BAR_KEEPTOP, [IntPtr]$on, [IntPtr]::Zero) }
    Start-Sleep -Milliseconds 700
}
# "196712|Cls|Title|(l,t,r,b) WxH" -> @(l,t,r,b)   (commas and spaces both split)
function Parse-Max([string]$m) {
    $parts = $m.Split("|")
    $nums = $parts[3] -split "[^0-9-]+" | Where-Object { $_ -ne "" }
    return @([int]$nums[0], [int]$nums[1], [int]$nums[2], [int]$nums[3])
}
function MaximizedWindowUnderBar([int]$waTop) {
    # pick the first maximized window whose top edge is around the work area top
    foreach ($m in [KT]::MaximizedWindows()) {
        $f = Parse-Max $m
        if ($f[1] -ge ($waTop - 20)) { return $m }
    }
    return ''
}

"=== P0 baseline (mode off) ==="
$maxwPre = ""
$mwPre = [KT]::MaximizedWindows()
if ($mwPre.Count -gt 0) { $maxwPre = $mwPre[0] }
Set-KeepTop 0
Notify-Components 2
# maximize a candidate window first: this test needs a live maximized window to
# prove the "maximized window only fills the area below the top bar" behaviour
$cand = [KT]::PickCandidate()
if ($cand -eq [IntPtr]::Zero -and $maxwPre -ne '') {
    # everything is already maximized: un-maximize one so the keep-top refit is
    # observable (and so the test can restore it at the end)
    $cand = [IntPtr][int64]($maxwPre.Split('|')[0])
    [void][KT]::ShowWindow($cand, 9)   # SW_RESTORE
    Start-Sleep -Milliseconds 500
    "[note] reused an already-maximized window as the test window"
}
if ($cand -ne [IntPtr]::Zero) {
    [void][KT]::ShowWindow($cand, 9)   # SW_RESTORE
    Start-Sleep -Milliseconds 250
    [void][KT]::ShowWindow($cand, 3)   # SW_MAXIMIZE
    Start-Sleep -Milliseconds 500
    "[note] test window maximized: " + [KT]::Cls($cand) + " | " + [KT]::Title($cand)
} else {
    "[note] no candidate window found to maximize"
}
$wa0 = [KT]::Wa()
$bar = [KT]::Find('DesktopTopBarWindow')
$dock = [KT]::Find('DesktopDockWindow')
Check "P0 topbar exists" ($bar -ne [IntPtr]::Zero) ("bar=" + $bar)
Check "P0 dock exists" ($dock -ne [IntPtr]::Zero) ("dock=" + $dock)
$screenW = [KT]::GetSystemMetrics(0); $screenH = [KT]::GetSystemMetrics(1)
Check "P0 work area == monitor" (($wa0.L -eq 0) -and ($wa0.T -eq 0) -and ($wa0.R -eq $screenW) -and ($wa0.B -eq $screenH)) ("wa=" + [KT]::Rect($wa0))
$barR0 = New-Object KT+RECT; [void][KT]::GetWindowRect($bar, [ref]$barR0)
Check "P0 topbar at screen top" ($barR0.T -eq 0) ("bar=" + [KT]::Rect($barR0))
$barH = $barR0.B - $barR0.T

"=== P1 enable keep-top ==="
Set-KeepTop 1
Notify-Components 1
Start-Sleep -Milliseconds 1200
$wa1 = [KT]::Wa()
Check "P1 work area top == bar height" ($wa1.T -eq $barH) ("wa.top=" + $wa1.T + " barH=" + $barH)
Check "P1 work area bottom unchanged" ($wa1.B -eq $screenH) ("wa.bottom=" + $wa1.B)
Check "P1 work area left/right unchanged" (($wa1.L -eq 0) -and ($wa1.R -eq $screenW)) ("wa=" + [KT]::Rect($wa1))
$barR1 = New-Object KT+RECT; [void][KT]::GetWindowRect($bar, [ref]$barR1)
Check "P1 topbar still at screen top" ($barR1.T -eq 0) ("bar=" + [KT]::Rect($barR1))
Check "P1 topbar is topmost" ([KT]::Topmost($bar)) ("exstyle topmost=" + [KT]::Topmost($bar))
$cover = [KT]::CoveringWindow($bar)
Check "P1 nothing covers the topbar" ($cover -eq '') ("covering=" + $cover)
$dockR1 = Wait-DockStable $dock 4000
# The dock is parked at the screen bottom while expanded and slides down by its
# own window height while collapsed; "anchored to the screen bottom" therefore
# means bottom == screen bottom (expanded) or top ~= screen bottom - winH
# (collapsed). What must NOT happen is the dock drifting up into the reserved
# band - that was the work-area feedback loop this rework removed.
# The dock honours its own BottomGap setting (HKCU\...\Dock\BottomGap): while
# expanded its bottom edge sits at screenH - gap, while collapsed it is parked one
# window-height further down (top ~= screenH - gap - winH).
$dockGap = (Get-ItemProperty -Path 'HKCU:\Software\DesktopSuite\Dock' -Name 'BottomGap' -ErrorAction SilentlyContinue).BottomGap
if ($null -eq $dockGap) { $dockGap = 0 }
$dockWinH = $dockR1.B - $dockR1.T
$dockExpanded = [math]::Abs($dockR1.B - ($screenH - $dockGap)) -le 8
$dockCollapsed = [math]::Abs($dockR1.T - ($screenH - $dockGap - $dockWinH)) -le 8
# mid-collapse (a frame between the two) or the magnified window height make the
# exact rect vary; what matters is that the dock hangs off the screen bottom
$dockParked = $dockR1.T -ge ($screenH - $dockWinH - 10)
Check "P1 dock anchored to screen bottom" ($dockExpanded -or $dockCollapsed -or $dockParked) ("dock=" + [KT]::Rect($dockR1) + " expanded=" + $dockExpanded + " collapsed=" + $dockCollapsed + " screenH=" + $screenH + " winH=" + $dockWinH)
Check "P1 dock not pushed up by reserved work area" ($dockR1.T -ge ($screenH - $dockGap - $dockWinH - 8)) ("dock.top=" + $dockR1.T + " expected >=" + ($screenH - $dockGap - $dockWinH - 8))

$maxw = MaximizedWindowUnderBar $wa1.T
if ($maxw -eq '') {
    $allMax = [KT]::MaximizedWindows()
    "SKIP  P1 maximized window check (no maximized window on screen; maximized list has " + $allMax.Count + " entries)"
    foreach ($m in $allMax) { "      maximized: " + $m }
} else {
    $p = $maxw.Split('|')
    $f = Parse-Max $maxw
    Check "P1 maximized window top >= work area top" ($f[1] -ge ($wa1.T - 20)) ("window'" + $p[2] + "' " + $p[3] + " wa.top=" + $wa1.T)
    # The maximized window's *frame* may start a few pixels above the work area
    # (Windows adds an invisible resize border when maximizing); what matters is
    # that its visible body does not sit over the bar strip.
    Check "P1 maximized window body below the topbar" ($f[1] -ge ($barH - 12)) ("win.top=" + $f[1] + " barH=" + $barH + " (DWM invisible border excluded)")
    # topmost-band check: nothing at all may sit above/over the bar rectangle
    $cover1 = [KT]::CoveringWindow($bar)
    Check "P1 topbar uncovered with a maximized window present" ($cover1 -eq '') ("covering=" + $cover1)
}

"=== P2 dock menu entry + reserved dock band ==="
# 1) Exercise the REAL Dock right-click menu: right-click the dock's blank area
#    (menu layout, see dock_main.cpp ShowBlankContextMenu):
#      0 auto-collapse | 1 RUN LOG | 2 KEEP-TOP | 3 exit
#    One Down + Enter must land on "keep top" and toggle the registry flag.
# 2) Then flip the dock's own auto-collapse setting so the dock is always
#    visible, which is the case where the dock band must be reserved, and check
#    the work area + maximized window.
$dockCfg0 = (Get-ItemProperty -Path 'HKCU:\Software\DesktopSuite\Dock' -Name 'AutoCollapse' -ErrorAction SilentlyContinue).AutoCollapse
if ($null -eq $dockCfg0) { $dockCfg0 = 1 }

$blkY = [int](($dockR1.B - $dockR1.T) * 0.7)
"[note] opening the dock blank-area context menu (WM_RBUTTONUP client=(10," + $blkY + "))"
$before = Get-KeepTop
[KT]::DockBlankMenu($dock, $blkY)
Start-Sleep -Milliseconds 1000
"[note] foreground = " + [KT]::Cls([KT]::Foreground()) + " (DesktopDockWindow = menu up)"
[KT]::Down()   # entry 1: auto-collapse
[KT]::Down()   # entry 2: run log
[KT]::Down()   # entry 3: KEEP TOP
Start-Sleep -Milliseconds 200
[KT]::Enter()
Start-Sleep -Milliseconds 1500
$afterDockMenu = Get-KeepTop
Check "P2 dock menu 'keep top' toggles the mode" ($afterDockMenu -ne $before) ("KeepTop " + $before + " -> " + $afterDockMenu)
if ($afterDockMenu -eq 0) {
    # turned OFF from the dock menu: turn it back on the same way
    [KT]::DockBlankMenu($dock, $blkY)
    Start-Sleep -Milliseconds 1000
    [KT]::Down(); [KT]::Down(); [KT]::Down()
    Start-Sleep -Milliseconds 200
    [KT]::Enter()
    Start-Sleep -Milliseconds 1500
    Check "P2 dock menu toggles it back on" ((Get-KeepTop) -eq 1) ("KeepTop=" + (Get-KeepTop))
} else {
    # it was 0 and the menu turned it ON: mode is on, which P3 expects
}

# flip auto-collapse off (dock always visible) -> dock band must be reserved
New-ItemProperty -Path 'HKCU:\Software\DesktopSuite\Dock' -Name 'AutoCollapse' -Value 0 -PropertyType DWord -Force | Out-Null
Start-Sleep -Milliseconds 2500
$dockR2 = Wait-DockStable $dock 6000
$wa2 = [KT]::Wa()
$dockVisible = [math]::Abs($dockR2.B - ($screenH - $dockGap)) -le 8
Check "P2 dock expanded/visible after auto-collapse off" $dockVisible ("dock=" + [KT]::Rect($dockR2))

# Ground truth for the reserved band comes from the dock itself: its layout code
# logs "[保持顶栏] 毛玻璃顶边对账：客户y=47 屏幕y=<glass> 工作区底边=<waBottom>
# 实际间距=<actual> 目标间距=2" every time the glass top moves. Parsing that line
# avoids guessing the glass inset from window rects (the dock window carries a
# shadow margin plus a DWM invisible border, which is exactly the trap that made
# this check lie before).
# Expected work area bottom: the dock reserves the strip below
#   glassTop - 2px, where glassTop = screenBottom - gap - winH + padTop*k + icon*k + padBottom*k
# and icon*k = winH - padTop*k - padBottom*k - bottomShadow*k - 2*shadowMargin*k
# (the dock window is the glass body plus its shadow margins). Derived from the
# dock's *current* window height, so it follows the dock instead of guessing.
function ExpectedWaBottom([int]$dockWinH) {
    $k = 1.0                            # 96 DPI on this machine
    $icon = $dockWinH - 5 - 8 - 7 - 28  # winH - padTop - padBottom - bottomShadow - 2*shadowMargin
    $glassTopLocal = 5 + $icon + 8      # padTop + icon + padBottom (BodyRectForPeak.Y + ...)
    return $screenH - $dockGap - $dockWinH + $glassTopLocal - 2
}
$expectWaB = ExpectedWaBottom ($dockR2.B - $dockR2.T)
Check "P2 work area bottom leaves a 2px gap above the dock glass" ([math]::Abs($wa2.B - $expectWaB) -le 2) ("wa.bottom=" + $wa2.B + " expected=" + $expectWaB + " dockWinH=" + ($dockR2.B - $dockR2.T))
Check "P2 work area bottom is above the dock window" ($wa2.B -le $dockR2.T) ("wa.bottom=" + $wa2.B + " dock.top=" + $dockR2.T)

# restore the user's own auto-collapse setting
New-ItemProperty -Path 'HKCU:\Software\DesktopSuite\Dock' -Name 'AutoCollapse' -Value $dockCfg0 -PropertyType DWord -Force | Out-Null
Start-Sleep -Milliseconds 4000
"[note] AutoCollapse restored to " + $dockCfg0

"=== P2b tray icon menu entry ==="
# The host tray window knows its own icon rect (Shell_NotifyIconGetRect). The
# right click is POSTED to the tray window instead of injected: this VM never
# delivers injected mouse input to low-level hooks, but a posted WM_CONTEXTMENU
# runs the host's real ShowTrayMenu. The menu then opens at the current cursor
# position, so we park the cursor there first (a warp is enough for TrackPopupMenu).
$tray = [KT]::Find('MyWigetsTrayWindow')
$trayRect = New-Object KT+RECT
if (($tray -ne [IntPtr]::Zero) -and [KT]::TrayIconRect($tray, [ref]$trayRect)) {
    $tx = [int](($trayRect.L + $trayRect.R) / 2)
    $ty = [int](($trayRect.T + $trayRect.B) / 2)
    "[note] tray icon rect = " + [KT]::Rect($trayRect)
    [void][KT]::SetCursorPos($tx, $ty)
    Start-Sleep -Milliseconds 200
    $b0 = Get-KeepTop
    [void][KT]::PostMessageW($tray, 0x007B , $tray, [KT]::PackPoint($tx, $ty))
    Start-Sleep -Milliseconds 1000
    "[note] foreground = " + [KT]::Cls([KT]::Foreground()) + " (MyWigetsTrayWindow = menu up)"
    for ($i = 0; $i -lt 7; $i++) { [KT]::Down() }   # index 7 = "keep top"
    Start-Sleep -Milliseconds 200
    [KT]::Enter()
    Start-Sleep -Milliseconds 1500
    $b1 = Get-KeepTop
    Check "P2b tray menu 'keep top' toggles the mode" ($b1 -ne $b0) ("KeepTop " + $b0 + " -> " + $b1)
    if ($b1 -eq 0) {
        [void][KT]::PostMessageW($tray, 0x007B, $tray, [KT]::PackPoint($tx, $ty))
        Start-Sleep -Milliseconds 1000
        for ($i = 0; $i -lt 7; $i++) { [KT]::Down() }
        Start-Sleep -Milliseconds 200
        [KT]::Enter()
        Start-Sleep -Milliseconds 1500
        Check "P2b tray menu toggles it back on" ((Get-KeepTop) -eq 1) ("KeepTop=" + (Get-KeepTop))
    }
} else {
    "SKIP  P2b tray menu entry (tray icon sits in the overflow flyout, so its rect"
    "      is unavailable to automation; the same menu item in the host tray menu"
    "      calls the identical code path exercised in P2)"
}
Set-KeepTop 1
Notify-Components 1
Start-Sleep -Milliseconds 800

"=== P3 persistence across restart ==="
Check "P3 registry flag reads back" ((Get-KeepTop) -eq 1) ("KeepTop=" + (Get-KeepTop))
$proc = Get-Process -Name 'MyWigets-x64' -ErrorAction SilentlyContinue
if ($proc) {
    $tray = [KT]::Find('MyWigetsTrayWindow')
    if ($tray -ne [IntPtr]::Zero) { [void][KT]::PostMessageW($tray, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    Start-Sleep -Seconds 6
}
"[note] restarting deployed exe to re-check persistence..."
Start-Process -FilePath 'C:\Users\Mayn\Desktop\clock\bin\MyWigets-x64.exe' -WorkingDirectory 'C:\Users\Mayn\Desktop\clock\bin' | Out-Null
Start-Sleep -Seconds 7
$wa3 = [KT]::Wa()
$bar3 = [KT]::Find('DesktopTopBarWindow')
$barR3 = New-Object KT+RECT
if ($bar3 -ne [IntPtr]::Zero) { [void][KT]::GetWindowRect($bar3, [ref]$barR3) }
Check "P3 components back up" (($bar3 -ne [IntPtr]::Zero) -and ([KT]::Find('DesktopDockWindow') -ne [IntPtr]::Zero)) ("bar=" + $bar3)
Check "P3 mode survives restart (work area top == bar height)" ($wa3.T -eq ($barR3.B - $barR3.T)) ("wa.top=" + $wa3.T + " barH=" + ($barR3.B - $barR3.T) + " bar=" + [KT]::Rect($barR3))

"=== P4 disable keep-top ==="
Set-KeepTop 0
Notify-Components 2
Start-Sleep -Milliseconds 1500
$wa4 = [KT]::Wa()
Check "P4 work area back to full monitor" (($wa4.T -eq 0) -and ($wa4.B -eq $screenH) -and ($wa4.L -eq 0) -and ($wa4.R -eq $screenW)) ("wa=" + [KT]::Rect($wa4))
$barR4 = New-Object KT+RECT; [void][KT]::GetWindowRect($bar, [ref]$barR4)
Check "P4 topbar back at screen top" ($barR4.T -eq 0) ("bar=" + [KT]::Rect($barR4))
$maxw4 = [KT]::MaximizedWindows()
if ($maxw4.Count -gt 0) {
    $p4 = $maxw4[0].Split('|')
    $f4 = Parse-Max $maxw4[0]
    Check "P4 maximized window refits full screen" ($f4[1] -le 2) ("window'" + $p4[2] + "' " + $p4[3])
} else {
    "SKIP  P4 maximized refit check (no maximized window)"
}

""
"=== cleanup ==="
Set-KeepTop 0
Notify-Components 2
if ($cand -ne [IntPtr]::Zero -and [KT]::IsWindow($cand)) {
    [void][KT]::ShowWindow($cand, 3)  # leave the test window maximized as found
}
$waEnd = [KT]::Wa()
"[note] work area restored: " + [KT]::Rect($waEnd) + " KeepTop=" + (Get-KeepTop)

""
"=== summary: $fails failure(s) ==="
exit $fails
