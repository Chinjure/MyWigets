# tests/topbar_minimize_all_test.ps1
#
# 回归验证：最小化「顶栏聚焦应用」的**全部**窗口后，顶栏不得再粘住该应用，
# 必须改跟 Windows 当前聚焦的窗口；只最小化其中一张时仍要保持粘住。
#
# 判定手段：向顶栏发 kDockFocusQueryMsg（WM_APP+16，wParam = 待判定窗口），
# 返回 1 = 该窗口属于顶栏当前目标应用，0 = 否（-1 = 顶栏无应答）。
# 这正是 Dock 判断"点图标 = 最小化 / 聚焦"用的那条查询，等价于
# "顶栏当前聚焦哪个应用"。
#
# 用例窗口由本脚本自己创建两个同进程 WinForms 顶层窗口（= 同一个"应用"的
# 两张窗口），不用记事本：Win11 记事本是单进程多窗口且会恢复上次会话，
# 窗口数不可控；自建窗口用完即关，不碰用户任何已开窗口。
#   （注意：纯 Win32 自建窗口会被顶栏当作非任务栏窗口忽略——任务栏可见性
#     判据只认真实应用窗口，WinForms 窗口才是有效的测试载体。）
#
#   1) 聚焦窗口 A              → 顶栏目标 = 测试应用（1）
#   2) 只最小化 A（B 仍可见）  → 顶栏保持粘住（1）—— 旧语义不能丢
#   3) 再最小化 B（整组最小化）→ 顶栏必须放弃该应用（0）并跟系统前台
#
# 退出码：0 = 全部 PASS，1 = 有 FAIL（回滚验证时旧 exe 应当 FAIL）。
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public class TB {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint msg, IntPtr wp, IntPtr lp, uint flags, uint timeout, out IntPtr result);
  public delegate bool EnumProc(IntPtr h, IntPtr l);

  public static IntPtr FindByClass(string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      var c = new StringBuilder(128); GetClassNameW(h, c, 128);
      if (c.ToString() == cls) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }

  // 查询顶栏：probe 是否属于其当前目标应用（1 = 是，0 = 否，-1 = 无应答）
  public static int BarTargets(IntPtr probe) {
    IntPtr bar = FindByClass("DesktopTopBarWindow");
    if (bar == IntPtr.Zero) return -1;
    IntPtr res;
    IntPtr ok = SendMessageTimeoutW(bar, 0x8000 + 16, probe, IntPtr.Zero, 0x0002 | 0x0001, 800, out res);
    if (ok == IntPtr.Zero) return -1;
    return res == IntPtr.Zero ? 0 : 1;
  }

  // 解锁前台限制的常规做法：按一下 Alt 再 SetForegroundWindow
  public static bool ForceForeground(IntPtr h) {
    keybd_event(0x12, 0, 0, UIntPtr.Zero);
    keybd_event(0x12, 0, 2, UIntPtr.Zero);
    return SetForegroundWindow(h);
  }

  public static string Describe(IntPtr h) {
    if (h == IntPtr.Zero) return "(null)";
    var cls = new StringBuilder(128); GetClassNameW(h, cls, 128);
    var t = new StringBuilder(128); GetWindowTextW(h, t, 128);
    string title = t.ToString();
    if (title.Length > 30) title = title.Substring(0, 30);
    uint pid; GetWindowThreadProcessId(h, out pid);
    return string.Format("0x{0:X} [{1}] pid={2} \"{3}\"", h.ToInt64(), cls.ToString(), pid, title);
  }
}
'@ -Language CSharp

$fails = 0
function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Host ("PASS  {0}  {1}" -f $name, $detail) }
    else { Write-Host ("FAIL  {0}  {1}" -f $name, $detail) -ForegroundColor Red; $script:fails++ }
}

if ([TB]::FindByClass('DesktopTopBarWindow') -eq [IntPtr]::Zero) {
    Write-Host 'FAIL  顶栏未运行' -ForegroundColor Red
    exit 1
}

# ---- 准备：两个同进程 WinForms 顶层窗口 = 同一应用的 A / B 两张窗口 ----
function NewTestForm([string]$title, [int]$x) {
    $f = New-Object System.Windows.Forms.Form
    $f.Text = $title
    $f.Size = New-Object System.Drawing.Size(500, 350)
    $f.StartPosition = 'Manual'
    $f.Location = New-Object System.Drawing.Point($x, 140)
    $f.Show()
    return $f
}

$fA = NewTestForm 'TopBarTest-A' 140
$fB = NewTestForm 'TopBarTest-B' 240
Start-Sleep -Milliseconds 600
$A = $fA.Handle
$B = $fB.Handle

$descA = [TB]::Describe($A)
$descB = [TB]::Describe($B)
$clsA = ($descA -split ' ')[1]
$clsB = ($descB -split ' ')[1]
$sameApp = ($clsA -eq $clsB) -and $A -ne [IntPtr]::Zero -and $B -ne [IntPtr]::Zero
Check '创建测试应用的两张窗口' $sameApp ("A = $descA ; B = $descB")

try {
    if ($sameApp) {
        # ---- 1) 聚焦 A → 顶栏目标应为测试应用 ----
        [void][TB]::ForceForeground($A)
        Start-Sleep -Milliseconds 900
        $fg = [TB]::GetForegroundWindow()
        Check '聚焦测试窗口A' ($fg -eq $A) ("前台 = " + [TB]::Describe($fg))
        $q = [TB]::BarTargets($A)
        Check '顶栏目标=测试应用' ($q -eq 1) ("BarTargets(A) = $q （期望 1）")

        # ---- 2) 只最小化 A（B 仍可见）→ 顶栏继续粘住该应用 ----
        $fA.WindowState = 'Minimized'
        Start-Sleep -Milliseconds 1300
        $q = [TB]::BarTargets($B)
        Check '部分最小化仍粘住同应用' ($q -eq 1) ("BarTargets(B) = $q （期望 1：B 仍可见且同应用）")

        # ---- 3) 再最小化 B（整组最小化）→ 顶栏必须放弃该应用 ----
        $fB.WindowState = 'Minimized'
        Start-Sleep -Milliseconds 1500
        $fg = [TB]::GetForegroundWindow()
        $qA = [TB]::BarTargets($A)
        $qB = [TB]::BarTargets($B)
        Check '整组最小化后不再粘住' (($qA -eq 0) -and ($qB -eq 0)) ("BarTargets(A)=$qA BarTargets(B)=$qB （期望 0/0）")
        Check '顶栏改跟系统前台' ($fg -ne $A -and $fg -ne $B) ("系统前台 = " + [TB]::Describe($fg))
    }
} finally {
    $fA.Close()
    $fB.Close()
    $fA.Dispose()
    $fB.Dispose()
}

Write-Host ""
if ($fails -eq 0) { Write-Host 'ALL PASS' -ForegroundColor Green; exit 0 }
Write-Host ("{0} FAIL" -f $fails) -ForegroundColor Red
exit 1
