# tests/topbar_edge_tab_drag_test.ps1 —— Chrome 同步模式下"拖动标签页"端到端回归
#
# 与窗口模式（tests/topbar_tab_drag_test.ps1）不同，这里验证浏览器真实标签：
#   1) 用 Edge 打开 3 个标题互不相同的本地页面（同一个窗口 3 个标签）；
#   2) 顶栏进入 Chrome 同步模式后，用 UI Automation 读 Edge 标签栏，拿到
#      浏览器侧的**真实标签顺序**和**当前激活标签**（不依赖窗口标题：
#      Edge 的窗口标题不会跟着激活标签走）；
#   3) 依次点顶栏 0/1/2 号标签，每次读 Edge 的激活标签 —— 应当等于浏览器
#      顺序的第 0/1/2 个，证明"顶栏槽位顺序 = 浏览器标签顺序"；
#   4) 把顶栏 2 号标签拖到最前；
#   5) 再读浏览器标签顺序：扩展已处理 moveTab 时应变成 [T2,T0,T1] ——
#      这才是"浏览器里的真实标签顺序一起变"的证据；
#      同时再点顶栏 0/1/2 号标签，确认顶栏顺序也跟着对齐。
#
# 扩展未重载时本测试会在第 4 步失败并提示去 edge://extensions 重新加载。
#
# 运行：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\topbar_edge_tab_drag_test.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$cs = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public class EdgeTabDrag {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L, T, R, B; }

  const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004, KEYUP = 0x0002;

  public static IntPtr FindByClass(string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      var c = new StringBuilder(256); GetClassName(h, c, 256);
      if (c.ToString() == cls) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  // 标题含 key、且属于浏览器进程的可见顶层窗口（Edge/Chrome 主窗口）
  public static List<IntPtr> BrowserWindows(string key) {
    var list = new List<IntPtr>();
    EnumWindows((h, l) => {
      if (!IsWindowVisible(h)) return true;
      var c = new StringBuilder(256); GetClassName(h, c, 256);
      if (c.ToString() != "Chrome_WidgetWin_1") return true;
      uint pid; GetWindowThreadProcessId(h, out pid);
      string pn = "";
      try { pn = Process.GetProcessById((int)pid).ProcessName.ToLower(); } catch (Exception) { }
      if (pn != "msedge" && pn != "chrome") return true;
      var t = new StringBuilder(512); GetWindowTextW(h, t, 512);
      if (t.ToString().IndexOf(key, StringComparison.OrdinalIgnoreCase) < 0) return true;
      RECT r; GetWindowRect(h, out r);
      if (r.R - r.L > 200 && r.B - r.T > 200) list.Add(h);
      return true;
    }, IntPtr.Zero);
    return list;
  }
  public static void Click(int x, int y) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(120);
    mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(60);
    mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(350);
  }
  public static void Drag(int fromX, int fromY, int toX, int toY, int steps) {
    SetCursorPos(fromX, fromY);
    System.Threading.Thread.Sleep(150);
    mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(120);
    for (int i = 1; i <= steps; i++) {
      SetCursorPos(fromX + (toX - fromX) * i / steps, fromY + (toY - fromY) * i / steps);
      System.Threading.Thread.Sleep(25);
    }
    System.Threading.Thread.Sleep(120);
    mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(400);
  }
  // Ctrl+W：关掉当前激活标签（清理测试标签用）
  public static void CtrlW() {
    keybd_event(0x11, 0, 0, UIntPtr.Zero);
    keybd_event(0x57, 0, 0, UIntPtr.Zero);
    keybd_event(0x57, 0, KEYUP, UIntPtr.Zero);
    keybd_event(0x11, 0, KEYUP, UIntPtr.Zero);
    System.Threading.Thread.Sleep(400);
  }
  public static void Shot(string path, int x, int y, int w, int h) {
    using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
      using (var g = Graphics.FromImage(bmp)) {
        g.CopyFromScreen(x, y, 0, 0, new Size(w, h), CopyPixelOperation.SourceCopy);
      }
      bmp.Save(path, ImageFormat.Png);
    }
  }
}
'@

Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type -TypeDefinition $cs -ReferencedAssemblies System.dll,System.Core.dll,System.Drawing.dll,System.Windows.Forms.dll
[void][EdgeTabDrag]::SetProcessDPIAware()

$fail = 0
function Check([string]$name, [bool]$ok, [string]$detail) {
  if ($ok) { Write-Host ("PASS: {0} ({1})" -f $name, $detail) -ForegroundColor Green }
  else { Write-Host ("FAIL: {0} ({1})" -f $name, $detail) -ForegroundColor Red; $script:fail++ }
}

$logs = (Resolve-Path (Join-Path $PSScriptRoot '..\logs')).Path

# ---- UI Automation：读浏览器标签栏（顺序 + 激活项 + 元素矩形）----
function Get-BrowserTabs([IntPtr]$win) {
  try {
    $el = [System.Windows.Automation.AutomationElement]::FromHandle($win)
  } catch {
    return @()  # 窗口已经关掉（清理阶段最后一个标签关闭后整个窗口消失）
  }
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::TabItem)
  try {
    $items = $el.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
  } catch {
    return @()
  }
  $out = @()
  for ($i = 0; $i -lt $items.Count; $i++) {
    $it = $items.Item($i)
    $sel = $it.GetCurrentPropertyValue(
      [System.Windows.Automation.SelectionItemPattern]::IsSelectedProperty)
    $r = $it.Current.BoundingRectangle
    $out += [pscustomobject]@{
      Name     = $it.Current.Name
      Selected = [bool]$sel
      X        = [int]$r.X
      Y        = [int]$r.Y
      W        = [int]$r.Width
      H        = [int]$r.Height
    }
  }
  return @($out)
}
function TabTitle([object]$t) {
  foreach ($x in $titles) { if ($t.Name -like "*$x*") { return $x } }
  return $t.Name
}
function OrderTitles([object[]]$tabs) { return @($tabs | ForEach-Object { TabTitle $_ }) }
function ActiveTitle([IntPtr]$win) {
  $sel = @(Get-BrowserTabs $win | Where-Object { $_.Selected })
  if ($sel.Count -gt 0) { return (TabTitle $sel[0]) }
  return '<none>'
}

# ---- 0) 顶栏几何 ----
$bar = [EdgeTabDrag]::FindByClass('DesktopTopBarWindow')
if ($bar -eq [IntPtr]::Zero) { Write-Host 'FAIL: 顶栏窗口不存在（MyWigets 未运行？）'; exit 1 }
$rc = New-Object EdgeTabDrag+RECT
[void][EdgeTabDrag]::GetWindowRect($bar, [ref]$rc)
$barW = $rc.R - $rc.L; $barH = $rc.B - $rc.T
$dpi = [EdgeTabDrag]::GetDpiForWindow($bar); if ($dpi -le 0) { $dpi = 96 }
$k = $dpi / 96.0
$bw = 46.0 * $k; $gap = 4.0 * $k; $clockW = 112.0 * $k
$clockX = ($barW - 3 * $bw - 2 * $gap) - $gap - $clockW
$tabLeft = 46.0 * $k + 6.0 * $k
$tabRight = $clockX - 6.0 * $k
$tabY = $rc.T + [int]($barH / 2)
$tabCount = 3
$tabW = [Math]::Min(240.0 * $k, ($tabRight - $tabLeft) / $tabCount)
function TabX([int]$i) { return $rc.L + [int]($tabLeft + $tabW * ($i + 0.5)) }
Write-Host ("顶栏标签区 x={0:N1}..{1:N1}，3 槽位中心 = {2},{3},{4}" -f `
  $tabLeft, $tabRight, (TabX 0), (TabX 1), (TabX 2))

# ---- 1) Edge 打开 3 个标题互不相同的本地页面 ----
$stamp = Get-Date -Format 'HHmmss'
$dir = Join-Path $env:TEMP "topbar_edge_drag_$stamp"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$titles = @(1..3 | ForEach-Object { "EDGETAB-$stamp-$_" })
$urls = @()
for ($i = 0; $i -lt 3; $i++) {
  $p = Join-Path $dir "tab$($i + 1).html"
  Set-Content -Path $p -Encoding UTF8 -Value ("<!doctype html><title>{0}</title><h1>{0}</h1>" -f $titles[$i])
  $urls += ('file:///' + ($p -replace '\\', '/'))
}
$key = "EDGETAB-$stamp"
Start-Process msedge.exe -ArgumentList ($urls + '--new-window')
Start-Sleep -Seconds 5

$bwins = @([EdgeTabDrag]::BrowserWindows($key))
if ($bwins.Count -lt 1) {
  Write-Host 'FAIL: 没找到含测试标签的 Edge 窗口'
  Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
  exit 1
}
$win = $bwins[0]
$wrc = New-Object EdgeTabDrag+RECT
[void][EdgeTabDrag]::GetWindowRect($win, [ref]$wrc)
Write-Host ("Edge 窗口 0x{0:X} = ({1},{2}) {3}x{4}" -f $win.ToInt64(), $wrc.L, $wrc.T, ($wrc.R - $wrc.L), ($wrc.B - $wrc.T))

$stripH = [int](90 * $k); if ($stripH -gt ($wrc.B - $wrc.T)) { $stripH = $wrc.B - $wrc.T }
$shotX = $wrc.L; $shotY = $wrc.T; $shotW = $wrc.R - $wrc.L

try {
  # 聚焦浏览器窗口，等扩展推一次全量标签
  [EdgeTabDrag]::Click($wrc.L + 400, $wrc.T + 300)
  Start-Sleep -Milliseconds 1500

  [EdgeTabDrag]::Shot((Join-Path $logs 'edge_tab_drag_before.png'), $shotX, $shotY, $shotW, $stripH)

  $B0 = OrderTitles (Get-BrowserTabs $win)
  Write-Host ("  浏览器标签顺序（拖动前）= [" + ($B0 -join ', ') + "]")
  Check '浏览器里有 3 个测试标签' ($B0.Count -eq 3) ("实际 " + $B0.Count)

  # ---- 2) 顶栏槽位顺序应等于浏览器顺序（点标签 -> 读激活标签）----
  $T = @()
  foreach ($i in 0..2) {
    [EdgeTabDrag]::Click((TabX $i), $tabY)
    Start-Sleep -Milliseconds 500
    $T += (ActiveTitle $win)
    Write-Host ("  点顶栏标签 {0} -> Edge 激活标签 '{1}'" -f $i, $T[$i])
  }
  Check '顶栏槽位顺序 = 浏览器标签顺序' `
    (($T.Count -eq 3) -and ($T[0] -eq $B0[0]) -and ($T[1] -eq $B0[1]) -and ($T[2] -eq $B0[2])) `
    ("顶栏 [" + ($T -join ', ') + "] vs 浏览器 [" + ($B0 -join ', ') + "]")

  # ---- 3) 把顶栏 2 号标签拖到最前 ----
  $toX = $rc.L + [int]$tabLeft + 12
  [EdgeTabDrag]::Drag((TabX 2), $tabY, $toX, $tabY, 16)
  Start-Sleep -Milliseconds 1500
  [EdgeTabDrag]::Shot((Join-Path $logs 'edge_tab_drag_after.png'), $shotX, $shotY, $shotW, $stripH)

  # ---- 4) 浏览器侧的真实标签顺序应变成 [T2,T0,T1] ----
  $B1 = OrderTitles (Get-BrowserTabs $win)
  Write-Host ("  浏览器标签顺序（拖动后）= [" + ($B1 -join ', ') + "]")
  $expectB = @($B0[2], $B0[0], $B0[1])
  Check '浏览器里的真实标签顺序也跟着重排了（扩展处理了 moveTab）' `
    (($B1.Count -eq 3) -and ($B1[0] -eq $expectB[0]) -and ($B1[1] -eq $expectB[1]) -and ($B1[2] -eq $expectB[2])) `
    ("期望 [" + ($expectB -join ', ') + "]，实际 [" + ($B1 -join ', ') + "]")

  # ---- 5) 顶栏顺序也应与之一致 ----
  $order = @()
  foreach ($i in 0..2) {
    [EdgeTabDrag]::Click((TabX $i), $tabY)
    Start-Sleep -Milliseconds 500
    $order += (ActiveTitle $win)
  }
  Write-Host ("  拖动后再点顶栏 0/1/2 -> [" + ($order -join ', ') + "]")
  Check '顶栏拖动后顺序为 [原2号, 原0号, 原1号]' `
    (($order.Count -eq 3) -and ($order[0] -eq $T[2]) -and ($order[1] -eq $T[0]) -and ($order[2] -eq $T[1])) `
    ("期望 [" + (@($T[2], $T[0], $T[1]) -join ', ') + "]，实际 [" + ($order -join ', ') + "]")

  if ($fail -ne 0) {
    Write-Host ''
    Write-Host '提示：若"浏览器里的真实标签顺序"未变，请到 edge://extensions 对本扩展点一次「重新加载」' -ForegroundColor Yellow
    Write-Host '      （moveTab 是本次新增的扩展侧命令），然后重跑本测试。' -ForegroundColor Yellow
  }
}
finally {
  # ---- 清理：只关带本次唯一标题前缀的测试标签 ----
  for ($n = 0; $n -lt 6; $n++) {
    $tabs = @(Get-BrowserTabs $win | Where-Object { $_.Name -like "*$key*" })
    if ($tabs.Count -eq 0) { break }
    $t = $tabs[0]
    [EdgeTabDrag]::Click($t.X + [int]($t.W / 2), $t.Y + [int]($t.H / 2))  # 选中它
    Start-Sleep -Milliseconds 300
    [EdgeTabDrag]::Click($wrc.L + 400, $wrc.T + 300)                      # 焦点回到页面
    Start-Sleep -Milliseconds 200
    [EdgeTabDrag]::CtrlW()
  }
  Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
}

Write-Host ''
if ($fail -eq 0) {
  Write-Host 'ALL TESTS PASSED' -ForegroundColor Green
  Write-Host ("截图：{0}\edge_tab_drag_before.png / edge_tab_drag_after.png" -f $logs)
  exit 0
}
Write-Host ("{0} TEST(S) FAILED" -f $fail) -ForegroundColor Red
exit 1
