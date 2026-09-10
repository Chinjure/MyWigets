# test_dingtalk_dock_click.ps1 —— 回归测试：Edge 在前台时点击 Dock 里的钉钉图标，
# 钉钉的全部主窗口（主窗 + 图片/文档查看窗）都应升到 Edge 之上。
#
# 复现的用户场景：Edge 打开着，点 Dock 里的钉钉 —— 旧版本只有被激活的那一个
# 窗口升到 Edge 上，另一个仍留在 Edge 下（SetForegroundWindow 只抬目标窗口）。
#
# 测试步骤（全程真实鼠标/真实 Dock 代码路径，不做任何窗口 API 取巧）：
#   1) 解析 Dock 运行日志里的「清单」+「布局自检」，算出钉钉图标在 Dock 内的
#      槽位中心（不硬编码坐标，Dock 增删图标后依然有效）；
#   2) 先把 Edge 切到前台；
#   3) 光标移到 Dock 底部 → 等展开/放大动画 → 在钉钉图标上左键点击；
#   4) 按 Z 序断言：钉钉的全部主窗口名次都 < Edge 的名次（= 都在 Edge 之上）。
#
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File test_dingtalk_dock_click.ps1
# 说明：日志路径默认取「运行中的 MyWigets-x64.exe 所在目录的上一级 \logs\dock.log」，
#       与 dock_main.cpp 的 LogInit() 规则一致（Desktop 运行 → C:\Users\<user>\logs）。
param(
    [string]$LogPath,
    [string]$ItemName = '钉钉',
    [int]$HoverSettleMs = 700,
    [int]$AfterClickMs = 1200,
    [switch]$SkipToggle
)

$ErrorActionPreference = 'Stop'

# Dock 布局常量（dock_main.cpp：kIconSize / kIconGapHalf / kSepGap / kShadowMargin …）
$kIconSize   = 44.0
$kIconGapHalf = 3.5
$kSepGap     = 14.0

$code = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class DockTest {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtrW(IntPtr h, int idx);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int sz);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public KEYBDINPUT ki; public int pad1, pad2, pad3; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }

  public static List<IntPtr> ZOrder = new List<IntPtr>();
  public static List<string> ZNames = new List<string>();

  public static bool Cloaked(IntPtr h) {
    int c = 0; DwmGetWindowAttribute(h, 14, out c, 4); return c != 0;
  }

  // Visible top-level windows in Z order (top first). Owned windows are kept
  // out so the assertion talks about main windows only.
  public static void Snapshot() {
    ZOrder.Clear(); ZNames.Clear();
    EnumWindows((h, lp) => {
      if (!IsWindowVisible(h) || IsIconic(h) || Cloaked(h)) return true;
      if (GetWindow(h, 4) != IntPtr.Zero) return true;
      RECT r; GetWindowRect(h, out r);
      if (r.R - r.L <= 0 || r.B - r.T <= 0) return true;
      uint pid; GetWindowThreadProcessId(h, out pid);
      string proc = ""; try { proc = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch {}
      var cls = new StringBuilder(128); GetClassNameW(h, cls, 128);
      ZOrder.Add(h); ZNames.Add(proc + "/" + cls.ToString());
      return true;
    }, IntPtr.Zero);
  }

  public static int ZOf(IntPtr h) { return ZOrder.IndexOf(h); }

  public static IntPtr FindWindowByClass(string cls, string procContains, bool largest) {
    IntPtr found = IntPtr.Zero; long best = -1;
    EnumWindows((h, lp) => {
      if (!IsWindowVisible(h) || IsIconic(h) || Cloaked(h)) return true;
      var sb = new StringBuilder(128); GetClassNameW(h, sb, 128);
      if (sb.ToString() != cls) return true;
      if (procContains.Length > 0) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        string p = ""; try { p = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch {}
        if (p.IndexOf(procContains, StringComparison.OrdinalIgnoreCase) < 0) return true;
      }
      RECT r; GetWindowRect(h, out r);
      long area = (long)(r.R - r.L) * (r.B - r.T);
      if (!largest) { found = h; return false; }
      if (area > best) { best = area; found = h; }
      return true;
    }, IntPtr.Zero);
    return found;
  }

  // All main-shape top-level windows of a process-name match (same filter as
  // dock_main.cpp IsMainShapeWindow: unowned, no WS_EX_TOOLWINDOW, >= 400x300).
  public static List<IntPtr> MainShapeWindows(string procContains) {
    var list = new List<IntPtr>();
    EnumWindows((h, lp) => {
      if (!IsWindowVisible(h) || IsIconic(h) || Cloaked(h)) return true;
      if (GetWindow(h, 4) != IntPtr.Zero) return true;
      uint pid; GetWindowThreadProcessId(h, out pid);
      string p = ""; try { p = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch {}
      if (p.IndexOf(procContains, StringComparison.OrdinalIgnoreCase) < 0) return true;
      long ex = GetWindowLongPtrW(h, -20).ToInt64();
      if ((ex & 0x80) != 0 && (ex & 0x40000) == 0) return true;   // WS_EX_TOOLWINDOW
      RECT r; GetWindowRect(h, out r);
      long area = (long)(r.R - r.L) * (r.B - r.T);
      if (area < 400L * 300L) return true;
      list.Add(h);
      return true;
    }, IntPtr.Zero);
    return list;
  }

  public static string Describe(IntPtr h) {
    var t = new StringBuilder(160); GetWindowTextW(h, t, 160);
    var c = new StringBuilder(128); GetClassNameW(h, c, 128);
    RECT r; GetWindowRect(h, out r);
    return string.Format("0x{0:X} {1} [{2}] ({3},{4})-({5},{6})",
      h.ToInt64(), c.ToString(), t.ToString().Replace("\n", " "),
      r.L, r.T, r.R, r.B);
  }

  public static bool RectOf(IntPtr h, out RECT r) { return GetWindowRect(h, out r); }
  public static bool IsMinimized(IntPtr h) { return IsWindow(h) && IsIconic(h); }

  // Dock-style foreground grab (ALT inject then SetForegroundWindow)
  public static void ForceForeground(IntPtr h) {
    INPUT[] alt = new INPUT[2];
    alt[0].type = 1; alt[0].ki.wVk = 0x12;
    alt[1].type = 1; alt[1].ki.wVk = 0x12; alt[1].ki.dwFlags = 2;
    SendInput(2, alt, Marshal.SizeOf(typeof(INPUT)));
    SetForegroundWindow(h);
  }

  public static void Click(int x, int y) {
    SetCursorPos(x, y);
    mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);   // LEFTDOWN
    mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);   // LEFTUP
  }
}
'@
Add-Type -TypeDefinition $code -Language CSharp
[DockTest]::SetProcessDPIAware() | Out-Null

# ---- 定位 Dock 日志 ----
if (-not $LogPath) {
    $exe = (Get-Process -Name 'MyWigets-x64' -ErrorAction Stop | Select-Object -First 1).Path
    $dir = Split-Path -Parent $exe                       # exe 目录 = “…\bin” 或桌面
    $repoDir = Split-Path -Parent $dir                   # 上一级（LogInit 用 PathDir 两次）
    $LogPath = Join-Path $repoDir 'logs\dock.log'
}
if (-not (Test-Path $LogPath)) { throw "找不到 Dock 日志：$LogPath" }
Write-Host "[0] 日志：$LogPath"

# ---- 1) 从日志解析钉钉图标槽位 ----
$lines = Get-Content $LogPath -Encoding UTF8
$geomLine = ($lines | Where-Object { $_ -match '布局自检：' } | Select-Object -Last 1)
$listLine = ($lines | Where-Object { $_ -match '清单\(\d+\)' } | Select-Object -Last 1)
if (-not $geomLine -or -not $listLine) { throw '日志里没有「清单/布局自检」，Dock 可能还没完成首次布局' }

$m = [regex]::Match($geomLine, 'winW=(\d+) n=(\d+) pinN=(\d+) 首槽left=([\d.]+) 末槽cx=([\d.]+)')
if (-not $m.Success) { throw "布局自检行无法解析：$geomLine" }
$winW = [int]$m.Groups[1].Value
$n    = [int]$m.Groups[2].Value
$pinN = [int]$m.Groups[3].Value
$firstLeft = [double]$m.Groups[4].Value

$names = ([regex]::Match($listLine, '清单\(\d+\):\s*(.+)$').Groups[1].Value) -split ';\s*' |
         Where-Object { $_ -ne '' }
$idx = -1
for ($i = 0; $i -lt $names.Count; $i++) {
    if ($names[$i] -like "*$ItemName*") { $idx = $i; break }
}
if ($idx -lt 0) { throw "Dock 清单里没有「$ItemName」：$($names -join ' | ')" }

# 日志里的 winW / 首槽left 都是物理像素（Dock 是 per-monitor DPI aware，
# 布局常量已乘 s.scale）。用 dock_main.cpp 的同一个宽度公式反解缩放 k，
# 再按 k 把槽位算到物理坐标（不依赖屏幕 DPI 猜测、也不受多屏缩放影响）：
#   winW = n*(kIconSize+2*kIconGapHalf)*k + (分隔线? kSepGap*k) + 2*kBarPadX*k
#          + 2*kShadowMargin*k
$kBarPadX = 7.0
$kShadowMargin = 14.0
$unitDip = $kIconSize + 2 * $kIconGapHalf
$sepExtra = if ($n -gt $pinN) { $kSepGap } else { 0.0 }
$widthDip = $n * $unitDip + $sepExtra + 2 * $kBarPadX + 2 * $kShadowMargin
$k = $winW / $widthDip
$cxWindow = $firstLeft + $idx * $unitDip * $k + ($kIconSize / 2) * $k
if ($idx -ge $pinN) { $cxWindow += $kSepGap * $k }   # 分隔线之后整体右移
Write-Host ("[1] 清单($n)：{0}" -f ($names -join ' | '))
Write-Host ("    {0} index={1}/{2}（pinN={3}）槽位中心 = 窗口内 x={4:N1}（winW={5} k={6:N3}）" -f `
    $ItemName, $idx, $n, $pinN, $cxWindow, $winW, $k)

# ---- 2) 找到 Edge / 钉钉窗口，并把 Edge 切到前台 ----
$edge  = [DockTest]::FindWindowByClass('Chrome_WidgetWin_1', 'msedge', $true)
$dingM = [DockTest]::FindWindowByClass('StandardFrame_DingTalk', '', $false)
if ($edge -eq [IntPtr]::Zero) { throw '找不到 Edge 主窗口（Chrome_WidgetWin_1）' }
if ($dingM -eq [IntPtr]::Zero) { throw '找不到钉钉主窗口（StandardFrame_DingTalk）' }
[DockTest]::ForceForeground($edge)
Start-Sleep -Milliseconds 600

[DockTest]::Snapshot()
$dingWins = [DockTest]::MainShapeWindows('DingTalk')
Write-Host ("[2] Edge={0}" -f [DockTest]::Describe($edge))
Write-Host ("    钉钉主窗口形态窗口 {0} 个：" -f $dingWins.Count)
foreach ($w in $dingWins) { Write-Host ("      z={0} {1}" -f [DockTest]::ZOf($w), [DockTest]::Describe($w)) }
$edgeZ = [DockTest]::ZOf($edge)
Write-Host ("    前置基线：Edge z={0}，钉钉窗口在其上的个数={1}" -f `
    $edgeZ, (($dingWins | Where-Object { [DockTest]::ZOf($_) -lt $edgeZ }).Count))
if ($edgeZ -lt 0) { throw 'Edge 不在可见顶层窗口里（可能被最小化）' }

# ---- 3) 光标先远离 Dock 量静止窗口，再移到钉钉图标上点击 ----
[DockTest]::SetCursorPos(60, 60) | Out-Null
Start-Sleep -Milliseconds 500
$dock = [DockTest]::FindWindowByClass('DesktopDockWindow', '', $false)
if ($dock -eq [IntPtr]::Zero) { throw '找不到 Dock 窗口（DesktopDockWindow）' }
$rIdle = New-Object DockTest+RECT
[DockTest]::RectOf($dock, [ref]$rIdle) | Out-Null
$dockLeft = $rIdle.L           # 光标在远处 → 这个 rect 就是静止尺寸，left 与槽位同坐标系
$clickX = [int]($dockLeft + $cxWindow)
$clickY = [DockTest]::GetSystemMetrics(1) - 45      # Dock 命中区一直延伸到屏幕底边
Write-Host ("[3] Dock 静止 rect=({0},{1})-({2},{3}) w={4}（日志 winW={5}）" -f `
    $rIdle.L, $rIdle.T, $rIdle.R, $rIdle.B, ($rIdle.R - $rIdle.L), $winW)
if ([Math]::Abs(($rIdle.R - $rIdle.L) - $winW) -gt 4) {
    Write-Warning "静止宽度与日志 winW 不一致：条目清单在启动后变过，槽位可能已偏移（本次结果仅供参考）"
}
Write-Host ("    点击坐标 = ({0},{1})" -f $clickX, $clickY)

# 先移到 Dock 底部让自动收起展开，再移到图标上等放大动画稳定，最后点击
[DockTest]::SetCursorPos($clickX, [DockTest]::GetSystemMetrics(1) - 3) | Out-Null
Start-Sleep -Milliseconds 450
[DockTest]::SetCursorPos($clickX, $clickY) | Out-Null
Start-Sleep -Milliseconds $HoverSettleMs
[DockTest]::Click($clickX, $clickY)
Start-Sleep -Milliseconds $AfterClickMs
[DockTest]::SetCursorPos(60, 60) | Out-Null
Start-Sleep -Milliseconds 400

# ---- 4) 断言 ----
$clickedLine = (Get-Content $LogPath -Encoding UTF8 | Where-Object { $_ -match '打开全部窗口：' } | Select-Object -Last 1)
[DockTest]::Snapshot()
$edgeZ = [DockTest]::ZOf($edge)
$bad = @()
foreach ($w in $dingWins) {
    if ([DockTest]::ZOf($w) -gt $edgeZ) { $bad += $w }
}
Write-Host ("[4] 日志末条：{0}" -f $clickedLine)
Write-Host ("    点击后：Edge z={0}" -f $edgeZ)
foreach ($w in $dingWins) { Write-Host ("      钉钉 z={0} {1}" -f [DockTest]::ZOf($w), [DockTest]::Describe($w)) }

$hitRight = $clickedLine -like "*$ItemName*"
if (-not $hitRight) {
    Write-Host "结果：INCONCLUSIVE —— 这次点击没有落在钉钉图标上（日志显示点的是别的条目）" -ForegroundColor Yellow
    exit 2
}
if ($bad.Count -eq 0) {
    Write-Host ("结果：PASS —— 钉钉 {0} 个主窗口全部在 Edge 之上（z 序 0 = 最上）" -f $dingWins.Count) -ForegroundColor Green
} else {
    Write-Host ("结果：FAIL —— 仍有 {0} 个钉钉主窗口压在 Edge 之下" -f $bad.Count) -ForegroundColor Red
    exit 1
}

# ---- 5) 切换语义回归：应用已在前台时再点一次 = 最小化全部窗口，
#         第三次点击 = 重新整组打开（restored 路径） ----
if ($SkipToggle) { exit 0 }
function Move-Hover-Click {
    [DockTest]::SetCursorPos($clickX, [DockTest]::GetSystemMetrics(1) - 3) | Out-Null
    Start-Sleep -Milliseconds 450
    [DockTest]::SetCursorPos($clickX, $clickY) | Out-Null
    Start-Sleep -Milliseconds $HoverSettleMs
    [DockTest]::Click($clickX, $clickY)
    Start-Sleep -Milliseconds $AfterClickMs
    [DockTest]::SetCursorPos(60, 60) | Out-Null
    Start-Sleep -Milliseconds 300
}

Move-Hover-Click
$minLine = (Get-Content $LogPath -Encoding UTF8 | Where-Object { $_ -match '最小化：' } | Select-Object -Last 1)
$minCount = ($dingWins | Where-Object { [DockTest]::IsMinimized($_) }).Count
Write-Host ("[5] 第二次点击日志：{0}" -f $minLine)
Write-Host ("    最小化窗口数 = {0}/{1}" -f $minCount, $dingWins.Count)

Move-Hover-Click
$reopenLine = (Get-Content $LogPath -Encoding UTF8 | Where-Object { $_ -match '打开全部窗口：' } | Select-Object -Last 1)
[DockTest]::Snapshot()
$edgeZ = [DockTest]::ZOf($edge)
$bad2 = @($dingWins | Where-Object { [DockTest]::ZOf($_) -gt $edgeZ -or [DockTest]::ZOf($_) -lt 0 })
Write-Host ("[6] 第三次点击日志：{0}" -f $reopenLine)
Write-Host ("    重新打开后：Edge z={0}，钉钉窗口 z={1}" -f $edgeZ, `
    (($dingWins | ForEach-Object { [DockTest]::ZOf($_) }) -join ','))

if ($minCount -eq $dingWins.Count -and $minLine -like "*$ItemName*" -and $bad2.Count -eq 0) {
    Write-Host "结果：PASS —— 再点最小化、三点击重新整组打开，行为均正确" -ForegroundColor Green
    exit 0
}
Write-Host ("结果：FAIL —— 切换语义异常（最小化 {0}/{1}，重新打开后异常窗口 {2} 个）" -f `
    $minCount, $dingWins.Count, $bad2.Count) -ForegroundColor Red
exit 1
