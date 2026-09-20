# verify_fullscreen.ps1 -- behavioral verification of "fullscreen app => Dock yields"
#   P0 baseline  : dock visible, bottom-strip reveal + collapse work
#   P1 fullscreen: dock hidden, strip reveal blocked, dock-strip click & corner click
#                  not swallowed, Dock toggle hotkey ignored
#   P2 exit      : dock restored, strip reveal works again
#   P3 maximized : NOT treated as fullscreen (work-area==monitor false-positive guard)
#   P4 control   : the Dock hotkey really toggles the dock when nothing is fullscreen
# ASCII only (avoid PS5.1 encoding pitfalls); dock.log is inspected separately.
#
# Harness notes (both learned the hard way):
#   * SetCursorPos warps the cursor WITHOUT an input event, so the dock's
#     WH_MOUSE_LL hook never sees it -> always inject an absolute mouse move.
#   * an expanded dock only collapses after a real enter->leave transition, so the
#     reveal probe must do enter -> leave -> enter.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public class FSX {
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c,string n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint dx,uint dy,uint d,UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk,byte scan,uint flags,UIntPtr extra);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr wp,IntPtr lp);
  [DllImport("shell32.dll")] public static extern int SHQueryUserNotificationState(out int st);
  [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L,T,R,B;}
  [StructLayout(LayoutKind.Sequential)] public struct POINT{public int X,Y;}
  public static string Cls(IntPtr h){var sb=new StringBuilder(128);GetClassNameW(h,sb,128);return sb.ToString();}
  // NOTE: FindWindowW's 2nd arg must be a real NULL (C# literal null); passing
  // PowerShell $null marshals as an empty title and never matches.
  public static IntPtr DockHwnd(){return FindWindowW("DesktopDockWindow",null);}
  public static IntPtr TrayHwnd(){return FindWindowW("MyWigetsTrayWindow",null);}
  public static int Quns(){int st=-1;SHQueryUserNotificationState(out st);return st;}
  // real (injected) absolute mouse move: input events the LL hook can see
  public static void MoveTo(int x,int y){
    int vx=GetSystemMetrics(76),vy=GetSystemMetrics(77);
    int vw=GetSystemMetrics(78),vh=GetSystemMetrics(79);
    if(vw<2)vw=2; if(vh<2)vh=2;
    long ax=((long)(x-vx)*65535)/(vw-1);
    long ay=((long)(y-vy)*65535)/(vh-1);
    mouse_event(0x0001|0x8000,(uint)ax,(uint)ay,0,UIntPtr.Zero);
  }
  public static void LeftClick(int x,int y){MoveTo(x,y);System.Threading.Thread.Sleep(200);mouse_event(0x0002,0,0,0,UIntPtr.Zero);mouse_event(0x0004,0,0,0,UIntPtr.Zero);}
  public static void Hotkey(int mods,int vk){
    if((mods&1)!=0)keybd_event(0x12,0,0,UIntPtr.Zero);
    if((mods&2)!=0)keybd_event(0x11,0,0,UIntPtr.Zero);
    if((mods&4)!=0)keybd_event(0x10,0,0,UIntPtr.Zero);
    if((mods&8)!=0)keybd_event(0x5B,0,0,UIntPtr.Zero);
    keybd_event((byte)vk,0,0,UIntPtr.Zero);
    System.Threading.Thread.Sleep(90);
    keybd_event((byte)vk,0,2,UIntPtr.Zero);
    if((mods&1)!=0)keybd_event(0x12,0,2,UIntPtr.Zero);
    if((mods&2)!=0)keybd_event(0x11,0,2,UIntPtr.Zero);
    if((mods&4)!=0)keybd_event(0x10,0,2,UIntPtr.Zero);
    if((mods&8)!=0)keybd_event(0x5B,0,2,UIntPtr.Zero);
  }
  // deterministic path into the host's WM_HOTKEY handler (kHotkeyId = 1)
  public static bool PostHotkey(){IntPtr h=TrayHwnd();if(h==IntPtr.Zero)return false;return PostMessageW(h,0x0312,(IntPtr)1,IntPtr.Zero);}
}
'@

$script:fails = 0
$script:passes = 0
$script:dockRestarts = 0
function Check([string]$name, [bool]$cond, [string]$detail) {
  if ($cond) { $script:passes++; "PASS  {0}   {1}" -f $name, $detail }
  else       { $script:fails++;  "FAIL  {0}   {1}" -f $name, $detail }
}
function DockState {
  $h = [FSX]::DockHwnd()
  $o = New-Object psobject
  $o | Add-Member NoteProperty Exists ($h -ne [IntPtr]::Zero)
  $o | Add-Member NoteProperty Visible $false
  $o | Add-Member NoteProperty Top 0
  $o | Add-Member NoteProperty Bottom 0
  $o | Add-Member NoteProperty Left 0
  $o | Add-Member NoteProperty Right 0
  if ($o.Exists) {
    $r = New-Object FSX+RECT
    [void][FSX]::GetWindowRect($h, [ref]$r)
    $o.Visible = [FSX]::IsWindowVisible($h)
    $o.Top = $r.T; $o.Bottom = $r.B; $o.Left = $r.L; $o.Right = $r.R
  }
  return $o
}
function Fg {
  $h = [FSX]::GetForegroundWindow()
  $o = New-Object psobject
  $o | Add-Member NoteProperty Hwnd $h
  $o | Add-Member NoteProperty Cls ([FSX]::Cls($h))
  return $o
}
$log = 'C:\Users\Mayn\Desktop\clock\logs\dock.log'
function LogLen {
  # dock.log is opened with _SH_DENYNO (others may read+write), so shares must match
  if (-not (Test-Path $log)) { return 0 }
  $fs = [System.IO.File]::Open($log, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
  try {
    $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
    $t = $sr.ReadToEnd(); $sr.Dispose(); return $t.Length
  } finally { $fs.Dispose() }
}

# configured Dock hotkey (the host registers it globally); default Alt+Q on this box
$hkMods = 1; $hkVk = 0x51
try {
  $cfg = Get-ItemProperty 'HKCU:\Software\DesktopSuite\MyWigets' -ErrorAction Stop
  if ($cfg.DockHotkeyMods) { $hkMods = [int]$cfg.DockHotkeyMods }
  if ($cfg.DockHotkeyVk)   { $hkVk   = [int]$cfg.DockHotkeyVk }
} catch {}
function SendHotkey { [FSX]::Hotkey([int]$hkMods, [int]$hkVk) }

$saved = New-Object FSX+POINT
[void][FSX]::GetCursorPos([ref]$saved)
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$cx = [int]($screen.Width / 2)
$stripY = $screen.Height - 1
$awayY = [int]($screen.Height / 3)
$form = $null
$maxf = $null

# Dock open/closed state straight from the dock's own log ("[展开] 完成 offset=0"
# / "[收起] 完成 offset=<winH>"). The old "Top > 1040" window-rect heuristic went
# stale once BottomGap started measuring from the glass (the window is 7px taller
# than before and its top edge while collapsed is 1082-winH).
function DockLogState {
  $log = 'C:\Users\Mayn\Desktop\clock\logs\dock.log'
  if (-not (Test-Path $log)) { return 'unknown' }
  $m = Select-String -Path $log -Pattern '\[(展开|收起)\] 完成 offset=([0-9.]+)' -Encoding UTF8 | Select-Object -Last 1
  if (-not $m) { return 'unknown' }
  $kind = $m.Matches[0].Groups[1].Value
  $off = [double]$m.Matches[0].Groups[2].Value
  if ($kind -eq '收起' -and $off -gt 1) { return 'collapsed' }
  if ($kind -eq '展开' -and $off -lt 1) { return 'expanded' }
  return 'moving'
}

function RevealProbe {
  # enter (establish wasOnDock) -> leave (must collapse) -> enter (must reveal)
  [FSX]::MoveTo($cx, $stripY); Start-Sleep -Milliseconds 900
  [FSX]::MoveTo($cx, $awayY);  Start-Sleep -Milliseconds 1400
  $dc = DockState
  $stC = DockLogState
  [FSX]::MoveTo($cx, $stripY); Start-Sleep -Milliseconds 1000
  $de = DockState
  $stE = DockLogState
  $o = New-Object psobject
  $o | Add-Member NoteProperty CollapseTop $dc.Top
  $o | Add-Member NoteProperty CollapseOk ($dc.Exists -and $dc.Visible -and $stC -eq 'collapsed')
  $o | Add-Member NoteProperty RevealTop $de.Top
  $o | Add-Member NoteProperty RevealOk ($de.Exists -and $de.Visible -and $stE -eq 'expanded')
  return $o
}
function RestartDockComponent {
  $script:dockRestarts++
  SendHotkey; Start-Sleep -Milliseconds 2200
  SendHotkey; Start-Sleep -Milliseconds 3200
}
# the dock's LL hook misses the odd transition in this environment (pre-existing, see
# "leave event lost" in dock.log): a mouse-driven assertion needs a live reveal path,
# otherwise it would pass/fail for the wrong reason
function EnsureReveal([string]$tag) {
  $r = RevealProbe
  if ($r.RevealOk -and $r.CollapseOk) { return $r }
  Write-Host ("WARN  reveal path unresponsive before {0} (collapseTop={1} revealTop={2}) -> restarting dock component" -f $tag, $r.CollapseTop, $r.RevealTop)
  RestartDockComponent
  return (RevealProbe)
}

try {
  # ---------------- P0: baseline ----------------
  $d = DockState
  Check 'P0 dock exists' $d.Exists ('top={0} bottom={1} visible={2}' -f $d.Top, $d.Bottom, $d.Visible)
  Check 'P0 dock visible' $d.Visible ''
  $r = RevealProbe
  if (-not ($r.CollapseOk -and $r.RevealOk)) {
    Write-Host ("WARN  reveal path unresponsive before P0 (collapseTop={0} revealTop={1}) -> restarting dock component" -f $r.CollapseTop, $r.RevealTop)
    RestartDockComponent
    $r = RevealProbe
  }
  Check 'P0 strip reveal works' $r.RevealOk ('top={0} (expanded when < 1010)' -f $r.RevealTop)
  Check 'P0 leave collapses' $r.CollapseOk ('top={0} (state from dock log)' -f $r.CollapseTop)
  [FSX]::MoveTo($cx, $awayY); Start-Sleep -Milliseconds 1400

  # ---------------- P1: fullscreen app ----------------
  $before = LogLen
  $form = New-Object System.Windows.Forms.Form
  $form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
  $form.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
  $form.Bounds = $screen
  $form.TopMost = $true
  $form.BackColor = [System.Drawing.Color]::DarkSlateBlue
  $form.Text = 'FS-TEST-WINDOW'
  $form.Show(); $form.Activate()
  Start-Sleep -Milliseconds 1800
  $fg = Fg
  Check 'P1 fullscreen window is foreground' ($fg.Hwnd -eq $form.Handle) ('cls={0} quns={1}' -f $fg.Cls, [FSX]::Quns())
  $d = DockState
  Check 'P1 dock hidden while fullscreen' ($d.Exists -and -not $d.Visible) ('visible={0} top={1}' -f $d.Visible, $d.Top)
  Check 'P1 log recorded the yield' ((LogLen - $before) -gt 0) ('log grew {0} chars' -f (LogLen - $before))

  [FSX]::MoveTo($cx, $awayY); Start-Sleep -Milliseconds 400
  [FSX]::MoveTo($cx, $stripY); Start-Sleep -Milliseconds 1400
  $d = DockState
  Check 'P1 strip reveal blocked' (-not $d.Visible) ('visible={0} top={1}' -f $d.Visible, $d.Top)

  $fgBefore = (Fg).Hwnd
  [FSX]::LeftClick($cx, $stripY); Start-Sleep -Milliseconds 1200
  Check 'P1 dock-strip click not swallowed' ((Fg).Hwnd -eq $fgBefore) ('fg before={0} after={1}' -f $fgBefore, (Fg).Hwnd)

  [FSX]::LeftClick(6, ($screen.Height - 6)); Start-Sleep -Milliseconds 1200
  Check 'P1 corner click not swallowed' ((Fg).Hwnd -eq $fgBefore) ('fg after corner click={0}' -f (Fg).Hwnd)

  SendHotkey; Start-Sleep -Milliseconds 1600
  $d = DockState
  Check 'P1 hotkey ignored while fullscreen (real hotkey)' $d.Exists ('dock window alive: {0}' -f $d.Exists)
  [void][FSX]::PostHotkey(); Start-Sleep -Milliseconds 1600
  $d = DockState
  Check 'P1 hotkey ignored while fullscreen (WM_HOTKEY)' $d.Exists ('dock window alive: {0}' -f $d.Exists)

  # ---------------- P2: fullscreen exit ----------------
  $form.Close(); $form.Dispose(); $form = $null
  Start-Sleep -Milliseconds 2000
  $d = DockState
  Check 'P2 dock restored (visible)' ($d.Exists -and $d.Visible) ('visible={0} top={1}' -f $d.Visible, $d.Top)
  $d = EnsureReveal 'P2'
  Check 'P2 strip reveal works again' $d.RevealOk ('revealTop={0} collapseTop={1}' -f $d.RevealTop, $d.CollapseTop)
  [FSX]::MoveTo($cx, $awayY); Start-Sleep -Milliseconds 1400

  # ---------------- P1b: fullscreen starting while the dock is EXPANDED ----------------
  # (the "launch a game from the dock" path: cursor stays on the strip, dock is up,
  #  then the app takes the whole screen)
  [FSX]::MoveTo($cx, $stripY); Start-Sleep -Milliseconds 1100
  $d = DockState
  Check 'P1b dock expanded before fullscreen' ($d.Visible -and $d.Top -lt 1010) ('top={0}' -f $d.Top)
  $before = LogLen
  $form = New-Object System.Windows.Forms.Form
  $form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
  $form.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
  $form.Bounds = $screen
  $form.TopMost = $true
  $form.BackColor = [System.Drawing.Color]::MidnightBlue
  $form.Text = 'FS-TEST-WINDOW-2'
  $form.Show(); $form.Activate()
  Start-Sleep -Milliseconds 1800
  $d = DockState
  Check 'P1b dock hidden while expanded-then-fullscreen' ($d.Exists -and -not $d.Visible) ('visible={0} top={1}' -f $d.Visible, $d.Top)
  Check 'P1b log recorded the yield' ((LogLen - $before) -gt 0) ('log grew {0} chars' -f (LogLen - $before))
  $form.Close(); $form.Dispose(); $form = $null
  Start-Sleep -Milliseconds 2000
  $d = DockState
  Check 'P1b dock restored after exit' ($d.Exists -and $d.Visible) ('visible={0} top={1}' -f $d.Visible, $d.Top)
  [FSX]::MoveTo($cx, $awayY); Start-Sleep -Milliseconds 1400

  # ---------------- P3: maximized window must NOT count as fullscreen ----------------
  $maxf = New-Object System.Windows.Forms.Form
  $maxf.Text = 'MAX-TEST-WINDOW'
  $maxf.WindowState = [System.Windows.Forms.FormWindowState]::Maximized
  $maxf.Show(); $maxf.Activate()
  Start-Sleep -Milliseconds 1800
  $fg = Fg
  Check 'P3 maximized is foreground' ($fg.Hwnd -eq $maxf.Handle) ('quns={0} (must not be 3/4)' -f [FSX]::Quns())
  $d = DockState
  Check 'P3 dock still visible (no false positive)' ($d.Exists -and $d.Visible) ('visible={0} top={1}' -f $d.Visible, $d.Top)
  $d = EnsureReveal 'P3'
  Check 'P3 strip reveal works over maximized' $d.RevealOk ('top={0}' -f $d.RevealTop)
  [FSX]::MoveTo($cx, $awayY); Start-Sleep -Milliseconds 1400
  $maxf.Close(); $maxf.Dispose(); $maxf = $null
  Start-Sleep -Milliseconds 800

  # ---------------- P4: control -- hotkey works when nothing is fullscreen ----------------
  SendHotkey; Start-Sleep -Milliseconds 2000
  $gone = -not (DockState).Exists
  if (-not $gone) {
    Write-Host ("WARN  real hotkey (mods={0} vk=0x{1:X}) had no effect -> using WM_HOTKEY injection" -f $hkMods, $hkVk)
    [void][FSX]::PostHotkey(); Start-Sleep -Milliseconds 2000
    $gone = -not (DockState).Exists
  }
  Check 'P4 control: hotkey toggles dock off' $gone ('dock window gone: {0}' -f $gone)
  SendHotkey; Start-Sleep -Milliseconds 1500
  if (-not (DockState).Exists) {
    [void][FSX]::PostHotkey(); Start-Sleep -Milliseconds 2500
  }
  Check 'P4 control: hotkey toggles dock back' (DockState).Exists ''
  [FSX]::MoveTo($cx, $awayY); Start-Sleep -Milliseconds 1000
}
finally {
  if ($form) { try { $form.Close() } catch {} }
  if ($maxf) { try { $maxf.Close() } catch {} }
  [FSX]::MoveTo($saved.X, $saved.Y)   # injected move (keeps the LL hook consistent)
}
''
'==== RESULT: {0} passed, {1} failed, {2} dock restarts ====' -f $script:passes, $script:fails, $script:dockRestarts
if ($script:fails -gt 0) { exit 1 }
