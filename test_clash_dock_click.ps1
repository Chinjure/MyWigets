# test_clash_dock_click.ps1 —— 回归测试：Clash Verge 关闭到托盘（窗口隐藏、
# 托盘图标在溢出区）时，点击 Dock 里的 Clash Verge 图标应能把它打开。
#
# 断言链路（全程真实鼠标 / 真实 Dock 代码路径）：
#   1) 从 dock.log 的「清单 + 布局自检」反解出图标槽位中心（不硬编码坐标）；
#   2) 前置：Clash Verge 主窗口必须处于隐藏态（关闭到托盘）；
#   3) 光标移到 Dock 底边 → 等展开动画 → 在 Clash Verge 图标上左键点击；
#   4) 轮询断言 Clash Verge 的 Tauri 主窗口变为可见（≤5s）；
#   5) 打印 dock.log 尾部作为证据（应出现「托盘图标触发成功」）。
#
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File test_clash_dock_click.ps1
param(
    [string]$LogPath,
    [string]$ItemName = 'Clash Verge',
    [int]$HoverSettleMs = 800,
    [int]$WaitOpenMs = 5000
)

$ErrorActionPreference = 'Stop'

# Dock 布局常量（dock_main.cpp：kIconSize / kIconGapHalf / kSepGap / kShadowMargin …）
$kIconSize    = 44.0
$kIconGapHalf = 3.5
$kSepGap      = 14.0
$kBarPadX     = 7.0
$kShadowMargin = 14.0

$code = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class ClashTest {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

  public static IntPtr FindByClass(string cls, bool visibleOnly) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, lp) => {
      var sb = new StringBuilder(128); GetClassNameW(h, sb, 128);
      if (sb.ToString() != cls) return true;
      if (visibleOnly && !IsWindowVisible(h)) return true;
      found = h; return false;
    }, IntPtr.Zero);
    return found;
  }

  // Clash Verge(Tauri) 主窗口：类名 Tauri Window，标题 Clash Verge
  public static IntPtr ClashMainWindow(bool visibleOnly) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, lp) => {
      var sb = new StringBuilder(128); GetClassNameW(h, sb, 128);
      if (sb.ToString() != "Tauri Window") return true;
      var t = new StringBuilder(128); GetWindowTextW(h, t, 128);
      if (!t.ToString().Contains("Clash Verge")) return true;
      if (visibleOnly && !IsWindowVisible(h)) return true;
      found = h; return false;
    }, IntPtr.Zero);
    return found;
  }

  public static string Describe(IntPtr h) {
    if (h == IntPtr.Zero) return "(无)";
    var t = new StringBuilder(160); GetWindowTextW(h, t, 160);
    var c = new StringBuilder(128); GetClassNameW(h, c, 128);
    RECT r; GetWindowRect(h, out r);
    return string.Format("0x{0:X} [{1}] '{2}' vis={3} ({4},{5})-({6},{7})",
      h.ToInt64(), c.ToString(), t.ToString(), IsWindowVisible(h), r.L, r.T, r.R, r.B);
  }

  public static void Click(int x, int y) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(60);
    mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(50);
    mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
  }

  public static void CloseToTray(IntPtr h) {
    PostMessageW(h, 0x0010, IntPtr.Zero, IntPtr.Zero);  // WM_CLOSE
  }
}
'@
Add-Type -TypeDefinition $code -Language CSharp
[ClashTest]::SetProcessDPIAware() | Out-Null

# ---- 定位 Dock 日志 ----
if (-not $LogPath) {
    $exe = (Get-Process -Name 'MyWigets-x64' -ErrorAction Stop | Select-Object -First 1).Path
    $dir = Split-Path -Parent $exe
    $repoDir = Split-Path -Parent $dir
    $LogPath = Join-Path $repoDir 'logs\dock.log'
}
if (-not (Test-Path $LogPath)) { throw "找不到 Dock 日志：$LogPath" }
Write-Host "[0] 日志：$LogPath"

# ---- 1) 从日志解析图标槽位 ----
$lines = Get-Content $LogPath -Encoding UTF8
$geomLine = ($lines | Where-Object { $_ -match '布局自检：' } | Select-Object -Last 1)
$listLine = ($lines | Where-Object { $_ -match '清单\(\d+\)' } | Select-Object -Last 1)
if (-not $geomLine -or -not $listLine) { throw '日志里没有「清单/布局自检」，Dock 可能还没完成首次布局' }

$m = [regex]::Match($geomLine, 'winW=(\d+) n=(\d+) pinN=(\d+) 首槽left=([\d.]+) 末槽cx=([\d.]+)')
if (-not $m.Success) { throw "布局自检行无法解析：$geomLine" }
$winW     = [int]$m.Groups[1].Value
$n        = [int]$m.Groups[2].Value
$pinN     = [int]$m.Groups[3].Value
$firstLeft = [double]$m.Groups[4].Value

$names = ([regex]::Match($listLine, '清单\(\d+\):\s*(.+)$').Groups[1].Value) -split ';\s*' |
         Where-Object { $_ -ne '' }
$idx = -1
for ($i = 0; $i -lt $names.Count; $i++) {
    if ($names[$i] -like "*$ItemName*") { $idx = $i; break }
}
if ($idx -lt 0) { throw "Dock 清单里没有「$ItemName」：$($names -join ' | ')" }

$unitDip = $kIconSize + 2 * $kIconGapHalf
$sepExtra = if ($n -gt $pinN) { $kSepGap } else { 0.0 }
$widthDip = $n * $unitDip + $sepExtra + 2 * $kBarPadX + 2 * $kShadowMargin
$k = $winW / $widthDip
$cxWindow = $firstLeft + $idx * $unitDip * $k + ($kIconSize / 2) * $k
if ($idx -ge $pinN) { $cxWindow += $kSepGap * $k }
Write-Host ("[1] 清单($n)：{0}" -f ($names -join ' | '))
Write-Host ("    {0} index={1}/{2} 槽位中心 = 窗口内 x={3:N1}（winW={4} k={5:N3}）" -f `
    $ItemName, $idx, $n, $cxWindow, $winW, $k)

# ---- 2) 前置状态：Clash Verge 必须“关闭到托盘”（主窗口隐藏、进程存活） ----
$vis = [ClashTest]::ClashMainWindow($true)
if ($vis -ne [IntPtr]::Zero) {
    Write-Host "[2] Clash Verge 主窗口当前可见，先发 WM_CLOSE 关到托盘…"
    [ClashTest]::CloseToTray($vis)
    Start-Sleep -Milliseconds 1500
}
$proc = Get-Process clash-verge -ErrorAction SilentlyContinue
if (-not $proc) { throw 'clash-verge 进程不在（托盘驻留应用未运行），无法复现场景' }
$hidden = [ClashTest]::ClashMainWindow($false)
Write-Host ("[2] 前置：clash-verge pid={0} 主窗口={1}" -f $proc.Id, [ClashTest]::Describe($hidden))
if ($hidden -ne [IntPtr]::Zero -and [ClashTest]::IsWindowVisible($hidden)) {
    throw '无法把 Clash Verge 关到托盘（主窗口仍可见）'
}
Write-Host "    主窗口处于隐藏态 → 复现用户场景成立"

# ---- 3) 点击 Dock 图标 ----
[ClashTest]::SetCursorPos(60, 60) | Out-Null
Start-Sleep -Milliseconds 500
$dock = [ClashTest]::FindByClass('DesktopDockWindow', $false)
if ($dock -eq [IntPtr]::Zero) { throw '找不到 Dock 窗口（DesktopDockWindow）' }
$rIdle = New-Object ClashTest+RECT
[void][System.Runtime.InteropServices.Marshal]::SizeOf($rIdle)
$rectCode = @'
using System; using System.Runtime.InteropServices;
public class RHelp {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
Add-Type -TypeDefinition $rectCode
$rr = New-Object RHelp+RECT
[void][RHelp]::GetWindowRect($dock, [ref]$rr)
$dockLeft = $rr.L
$clickX = [int]($dockLeft + $cxWindow)
$clickY = [ClashTest]::GetSystemMetrics(1) - 45
Write-Host ("[3] Dock 静止 rect=({0},{1})-({2},{3}) w={4}（日志 winW={5}）" -f `
    $rr.L, $rr.T, $rr.R, $rr.B, ($rr.R - $rr.L), $winW)
Write-Host ("    点击坐标 = ({0},{1})" -f $clickX, $clickY)

# 先把光标移到 Dock 底边触发展开，再点图标。
# 注意：Dock 有展开动画，动画期间图标位置还在变 —— 必须等 Dock 窗口尺寸
# 稳定下来再点，否则点击会落在图标之间的空隙上（实测过这种假失败）。
$rectCode2 = @'
using System; using System.Runtime.InteropServices;
public class RH2 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
Add-Type -TypeDefinition $rectCode2
[ClashTest]::SetCursorPos($clickX, $clickY) | Out-Null
Start-Sleep -Milliseconds $HoverSettleMs
$stable = 0; $last = ''
$settle = [Diagnostics.Stopwatch]::StartNew()
while ($settle.ElapsedMilliseconds -lt 2500 -and $stable -lt 3) {
    Start-Sleep -Milliseconds 80
    $r2 = New-Object RH2+RECT
    [void][RH2]::GetWindowRect($dock, [ref]$r2)
    $sig = "$($r2.L),$($r2.T),$($r2.R),$($r2.B)"
    if ($sig -eq $last) { $stable++ } else { $stable = 0; $last = $sig }
}
Write-Host ("    Dock 展开稳定于 rect=$last（等待 {0}ms）" -f $settle.ElapsedMilliseconds)
[ClashTest]::Click($clickX, $clickY)

# ---- 4) 断言：主窗口变可见 ----
$sw = [Diagnostics.Stopwatch]::StartNew()
$opened = $null
while ($sw.ElapsedMilliseconds -lt $WaitOpenMs) {
    Start-Sleep -Milliseconds 200
    $w = [ClashTest]::ClashMainWindow($true)
    if ($w -ne [IntPtr]::Zero) { $opened = $w; break }
}
if ($opened -ne $null) {
    Write-Host ("[4] PASS：点击后 {0}ms Clash Verge 主窗口已打开 → {1}" -f `
        $sw.ElapsedMilliseconds, [ClashTest]::Describe($opened))
} else {
    Write-Host ("[4] FAIL：{0}ms 内 Clash Verge 主窗口仍未出现" -f $WaitOpenMs)
}

# ---- 5) 证据：日志尾部 ----
Write-Host "[5] dock.log 尾部："
Get-Content $LogPath -Encoding UTF8 -Tail 12 | ForEach-Object { "    $_" }

if ($opened -eq $null) { exit 1 }
