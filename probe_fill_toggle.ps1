# probe_fill_toggle.ps1 —— 端到端验证「双击顶栏 → 目标窗口填充顶栏以外区域」。
#
# 做法：本脚本建一次性 WinForms 窗口当靶子（不碰用户的其他窗口），把它激活成
# 顶栏目标，然后直接向顶栏窗口投递 WM_LBUTTONDBLCLK（与真实双击走同一条
# WndProc 分支），再用 DWM 扩展边框量窗口的**可见边框**是否正好等于
# (工作区左, 顶栏下缘, 工作区右, 工作区下缘)。
#
# 只看可见边框而不是窗口矩形：标准边框窗口的窗口矩形在左/右/下会比可见边框
# 多出约 9px（150% DPI）不可见边缘，代码已做补偿，这里正是要验证它。
#
# 三个阶段：
#   1) 普通窗口：双击填充 → 再双击还原
#   2) 系统最大化窗口：双击转为填充（顶栏重新可见）→ 再双击回到最大化前的位置
#   3) 两个窗口：填充 B → 切到 A 再切回 B → 双击仍能还原（验证按窗口记还原位置）
$ErrorActionPreference = 'Stop'

$cs = @'
using System;
using System.Runtime.InteropServices;
public class FT {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string win);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint flags);
  [DllImport("user32.dll")] public static extern bool GetMonitorInfoW(IntPtr mon, ref MONITORINFO mi);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
  public struct RECT { public int L, T, R, B; }
  public struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public int flags; }
  public const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
  public const uint WM_LBUTTONDBLCLK = 0x0203;
  public const int SW_MAXIMIZE = 3;
  public static IntPtr XY(int x, int y) { return (IntPtr)((y << 16) | (x & 0xFFFF)); }

  public static bool ForceForeground(IntPtr h) {
    if (h == IntPtr.Zero) return false;
    IntPtr fg = GetForegroundWindow();
    uint cur = GetCurrentThreadId();
    uint fgt = fg == IntPtr.Zero ? 0 : GetWindowThreadProcessId(fg, IntPtr.Zero);
    bool attached = fgt != 0 && fgt != cur && AttachThreadInput(cur, fgt, true);
    SetForegroundWindow(h);
    if (attached) AttachThreadInput(cur, fgt, false);
    return GetForegroundWindow() == h;
  }
}
'@
Add-Type -TypeDefinition $cs

[FT]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null  # PER_MONITOR_AWARE_V2
Add-Type -AssemblyName System.Windows.Forms

$script:fail = 0
function Check([bool]$cond, [string]$msg) {
  if ($cond) { Write-Host ("PASS: " + $msg) } else { Write-Host ("FAIL: " + $msg); $script:fail++ }
}
function GetRect([IntPtr]$h) {
  $r = New-Object FT+RECT
  [void][FT]::GetWindowRect($h, [ref]$r)
  return $r
}
function GetVisible([IntPtr]$h) {
  $r = New-Object FT+RECT
  $hr = [FT]::DwmGetWindowAttribute($h, [FT]::DWMWA_EXTENDED_FRAME_BOUNDS, [ref]$r, 16)
  if ($hr -ne 0) { Write-Host ("  (DwmGetWindowAttribute hr=$hr)") }
  return $r
}
function RectText($r) { return ("{0},{1} - {2},{3}" -f $r.L, $r.T, $r.R, $r.B) }
function RectEq($a, $b) { return ($a.L -eq $b.L -and $a.T -eq $b.T -and $a.R -eq $b.R -and $a.B -eq $b.B) }

# 注意 [NullString]::Value：PowerShell 会把 $null 绑定成空字符串，
# 而 FindWindowW 的空标题只匹配「标题为空」的窗口，会找不到顶栏。
$bar = [FT]::FindWindowW("DesktopTopBarWindow", [NullString]::Value)
if ($bar -eq [IntPtr]::Zero) { Write-Host "FAIL: topbar window not found"; exit 1 }

$barRc = GetRect $bar
$mon = [FT]::MonitorFromWindow($bar, 2)  # MONITOR_DEFAULTTONEAREST
$mi = New-Object FT+MONITORINFO
$mi.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($mi)
[void][FT]::GetMonitorInfoW($mon, [ref]$mi)
$work = $mi.rcWork
# 期望的「顶栏以外区域」：工作区去掉顶栏占据的上沿
$expect = New-Object FT+RECT
$expect.L = $work.L; $expect.T = $barRc.B; $expect.R = $work.R; $expect.B = $work.B
Write-Host ("topbar  = {0}   workarea = {1}" -f (RectText $barRc), (RectText $work))
Write-Host ("expect  = {0}   (visible frame of the filled window)" -f (RectText $expect))
Write-Host ""

# 双击落点必须避开音量键/标签/时钟/三键：标签从音量键右侧起、每个 360px
# （150% DPI 下的 kUniformTabWidth），时钟紧贴最小化键左侧。x=1900 对
# 5 个以内的标签都是空白处。
function DblClickBar() {
  [void][FT]::PostMessageW($bar, [FT]::WM_LBUTTONDBLCLK, [IntPtr]1, [FT]::XY(1900, 30))
  Start-Sleep -Milliseconds 600
}
function NewTarget([string]$title, [int]$x, [int]$y) {
  $f = New-Object System.Windows.Forms.Form
  $f.Text = $title
  $f.StartPosition = 'Manual'
  $f.Location = [System.Drawing.Point]::new($x, $y)
  $f.Size = [System.Drawing.Size]::new(700, 460)
  $f.Show()
  $f.Activate()
  [void][FT]::ForceForeground($f.Handle)
  Start-Sleep -Milliseconds 900   # 等顶栏把靶窗口认作当前目标
  return $f
}

Write-Host "== Phase 1: plain window =="
$a = NewTarget 'MyWigets fill probe A' 260 180
$beforeA = GetRect $a.Handle
DblClickBar
$vis = GetVisible $a.Handle
Check (RectEq $vis $expect) ("filled visible frame == below-topbar area (got " + (RectText $vis) + ")")
DblClickBar
Check (RectEq (GetRect $a.Handle) $beforeA) ("second double-click restores the original rect (" + (RectText $beforeA) + ")")

Write-Host ""
Write-Host "== Phase 2: system-maximized window =="
[void][FT]::ShowWindow($a.Handle, [FT]::SW_MAXIMIZE)
Start-Sleep -Milliseconds 500
$maxVis = GetVisible $a.Handle
Write-Host ("  maximized visible = " + (RectText $maxVis))
DblClickBar
$vis = GetVisible $a.Handle
Check (RectEq $vis $expect) ("double-click converts maximize into fill (got " + (RectText $vis) + ")")
DblClickBar
Check (RectEq (GetRect $a.Handle) $beforeA) "restore goes back to the pre-maximize rect"

Write-Host ""
Write-Host "== Phase 3: switching windows keeps the restore target =="
$b = NewTarget 'MyWigets fill probe B' 900 460
$beforeB = GetRect $b.Handle
DblClickBar
Check (RectEq (GetVisible $b.Handle) $expect) "B is filled"
[void][FT]::ForceForeground($a.Handle); Start-Sleep -Milliseconds 900
[void][FT]::ForceForeground($b.Handle); Start-Sleep -Milliseconds 900
DblClickBar
Check (RectEq (GetRect $b.Handle) $beforeB) ("after switching away and back, double-click still restores B (" + (RectText $beforeB) + ")")

$b.Close()
$a.Close()

Write-Host ""
if ($script:fail -eq 0) { Write-Host "ALL PASS" } else { Write-Host ("FAILED: {0} check(s)" -f $script:fail) }
if ($script:fail -eq 0) { exit 0 } else { exit 1 }
