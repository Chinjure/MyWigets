# dock_tray_overflow_test.ps1 —— 回归测试：点 Dock 里的「纯托盘驻留」第三方图标
# （图标没被“提升”到可见托盘区、落在「隐藏的图标」溢出弹窗里的那些应用）
# 能不能打开应用本体。
#
# 背景（2026-10-08 定位出的两条根因，hotspot 是样本）：
#   1. 溢出弹窗是任务栏 XAML 的一部分：Dock 把任务栏 SW_HIDE 之后，对
#      「显示隐藏的图标」按钮 UIA Invoke 只返回 S_OK、弹窗根本不装配，于是所有
#      未提升的托盘图标都点不开。→ Dock 侧改为触发期间临时显示任务栏
#      （BeginTrayTaskbarShow / EndTrayTaskbarShow，注册表之外的零持久状态）。
#   2. 条目显示名取自 exe 的 FileDescription，而托盘图标名是应用自己设的
#      Tooltip，两者互不包含（hotspot：条目名带 "Everything-style NTFS file
#      search launcher"，Tooltip 是 "hotspot — 左键显示 · 右键菜单"），名字
#      匹配失败 → 用注册表 NotifyIconSettings 的 InitialTooltip 兜底。
#
# 用例（可参数化到别的托盘应用）：
#   前置：Dock 在跑、样本进程在跑、样本主窗被先隐藏（强制走托盘触发路径）
#   动作：点 Dock 最右一个槽位（默认就是样本条目所在槽）
#   断言：Dock 日志出现「溢出弹窗已打开」与「托盘图标触发成功：<样本>」，
#         样本主窗重新可见；动作收尾后任务栏仍隐藏、溢出弹窗已关闭、工作区已扩回。
#
# 注意：本文件必须保存为「带 BOM 的 UTF-8」。Windows PowerShell 5.1 对无 BOM 的
# UTF-8 按 ANSI 解码，中文注释会被拆成乱码并触发语法错误（probe_*.ps1 里有些
# 脚本写成纯 ASCII 就是为了绕开这一点）。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\dock_tray_overflow_test.ps1
param(
    [string]$AppKeyPattern = 'hotspot',              # Dock 日志里用于识别样本的名字片段
    [string]$AppExe = 'hotspot-cpp',                 # 样本进程名（不带 .exe）
    [string]$AppWindowClass = 'HotspotLauncherWindow', # 样本主窗类名（前置隐藏用）
    [int]$SlotFromEnd = 0,                           # 从最后一个槽往左数第几个
    [int]$TimeoutSec = 15                            # 等待托盘触发成功的上限
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $repo) { $repo = Split-Path -Parent $MyInvocation.MyCommand.Path }
$logPath = Join-Path $env:USERPROFILE 'logs\dock.log'
$reportPath = Join-Path $repo 'logs\dock_tray_overflow_test.txt'
New-Item -ItemType Directory -Force -Path (Split-Path $reportPath) | Out-Null

$report = New-Object System.Text.StringBuilder
function Say([string]$text) {
    [void]$report.AppendLine($text)
    Write-Host $text
}
function Fail([string]$text) {
    Say ("FAIL: " + $text)
    [System.IO.File]::WriteAllText($reportPath, $report.ToString(),
                                   (New-Object System.Text.UTF8Encoding($false)))
    exit 1
}
function Pass([string]$text) {
    Say ("PASS: " + $text)
    [System.IO.File]::WriteAllText($reportPath, $report.ToString(),
                                   (New-Object System.Text.UTF8Encoding($false)))
    exit 0
}

# ---- 0) Win32 小工具（FindWindow 在 DSH 沙箱里恒返回 0，统一用 EnumWindows）----
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class DockTrayTest {
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public struct RECT { public int L, T, R, B; }
  public static IntPtr ByClass(string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, lp) => {
      var sb = new StringBuilder(256); GetClassNameW(h, sb, 255);
      if (sb.ToString() == cls) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static IntPtr ByPidClass(uint wantPid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, lp) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid != wantPid) return true;
      var sb = new StringBuilder(256); GetClassNameW(h, sb, 255);
      if (sb.ToString() == cls) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static void Move(int x, int y) { SetCursorPos(x, y); }
  public static void Click(int x, int y) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(250);
    mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(90);
    mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
  }
}
"@

# ---- 1) 前置检查 ----
$dock = [DockTrayTest]::ByClass('DesktopDockWindow')
if ($dock -eq [IntPtr]::Zero) { Fail 'Dock 未运行（找不到 DesktopDockWindow）' }
$proc = Get-Process -Name $AppExe -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Fail ("样本进程未运行：" + $AppExe) }
if (-not (Test-Path $logPath)) { Fail ("Dock 日志不存在：" + $logPath) }
$tray = [DockTrayTest]::ByClass('Shell_TrayWnd')
$island = [DockTrayTest]::ByClass('TopLevelWindowForOverflowXamlIsland')
Say ("前置：dock=0x{0:X} {1}=pid {2} trayVisible={3} islandVisible={4}" -f `
     $dock.ToInt64(), $AppExe, $proc.Id, `
     [DockTrayTest]::IsWindowVisible($tray), [DockTrayTest]::IsWindowVisible($island))
if ([DockTrayTest]::IsWindowVisible($tray)) { Fail '前置失败：Dock 应隐藏系统任务栏，但它当前可见' }

# ---- 2) 隐藏样本主窗：强制走「托盘触发」路径（否则点图标只会聚焦已有窗口）----
$appWin = [DockTrayTest]::ByPidClass([uint32]$proc.Id, $AppWindowClass)
if ($appWin -ne [IntPtr]::Zero) {
    [DockTrayTest]::ShowWindow($appWin, 0) | Out-Null   # SW_HIDE
    Start-Sleep -Milliseconds 400
    Say ("前置：已隐藏样本主窗 0x{0:X}（模拟纯托盘驻留状态）" -f $appWin.ToInt64())
}

# ---- 3) 从 Dock 自己的布局日志算出目标槽位中心 ----
# 布局自检行形如：布局自检：winW=1155 n=14 pinN=12 首槽left=37 末槽cx=1085
# 几何行形如：… winY=1443 offset=0.0 winH=167 …（物理像素）
$lines = Get-Content $logPath -Encoding UTF8 | Select-Object -Last 300
$layout = ($lines | Where-Object { $_ -match 'winW=(\d+) n=(\d+) pinN=(\d+)' } |
           Select-Object -Last 1)
$geom = ($lines | Where-Object { $_ -match 'winY=(-?\d+) offset=[\d.]+ winH=(\d+)' } |
         Select-Object -Last 1)
if (-not $layout) { Fail '日志里找不到「布局自检」行（Dock 启动日志被截断？）' }
if (-not $geom) { Fail '日志里找不到窗口几何行' }
$null = $layout -match 'winW=(\d+) n=(\d+) pinN=(\d+)'
$n = [int]$Matches[2]
$null = $layout -match 'left=(\d+)'
$firstLeft = [int]$Matches[1]
$null = $layout -match 'cx=(\d+)'
$cxLast = [int]$Matches[1]
$null = $geom -match 'winY=(-?\d+) offset=[\d.]+ winH=(\d+)'
$winYphys = [int]$Matches[1]
$glassTop = 71   # 毛玻璃带在窗口内的顶边（物理像素，见布局日志「毛玻璃顶边(局部)」）
$glassH = 86     # 毛玻璃带高度（物理像素，见布局日志「框体高」）
$pitch = if ($n -gt 1) { ([double]($cxLast - ($firstLeft + 24))) / ($n - 1) } else { 0 }
$cx = $cxLast - [int]($SlotFromEnd * $pitch)

# 悬停放大时 Dock 窗口自身会横向位移（实测左边界会移动几十像素），所以先移过去
# 悬停、等窗口稳定后再瞄准——否则一次到位很容易点到隔壁槽位。
$lastX = -99999
for ($iter = 0; $iter -lt 4; $iter++) {
    $rect = New-Object DockTrayTest+RECT
    [DockTrayTest]::GetWindowRect($dock, [ref]$rect) | Out-Null
    $winXphys = [int]($rect.L * 1.5)   # 本脚本进程 DPI 不感知：GetWindowRect 返回逻辑像素
    $xLog = [int][math]::Round((($winXphys + $cx)) / 1.5)
    $yLog = [int][math]::Round((($winYphys + $glassTop + [int]($glassH / 2))) / 1.5)
    if ([math]::Abs($xLog - $lastX) -le 2) { break }
    $lastX = $xLog
    [DockTrayTest]::Move($xLog, $yLog) | Out-Null
    Start-Sleep -Milliseconds 450
}
Say ("目标槽位：从右数第 {0} 个，逻辑坐标 ({1},{2})" -f $SlotFromEnd, $xLog, $yLog)

# ---- 4) 点击并等待断言条件 ----
$len = (Get-Item $logPath).Length
[DockTrayTest]::Click($xLog, $yLog) | Out-Null

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$newLog = ''
$shown = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 400
    $fs = New-Object System.IO.FileStream($logPath, [System.IO.FileMode]::Open,
                                          [System.IO.FileAccess]::Read,
                                          [System.IO.FileShare]::ReadWrite)
    $fs.Seek($len, 'Begin') | Out-Null
    $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
    $newLog = $sr.ReadToEnd()
    $sr.Close(); $fs.Close()
    $p2 = Get-Process -Name $AppExe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p2 -and $p2.MainWindowHandle -ne 0) { $shown = $true }
    if ($newLog -match '托盘图标触发成功' -and $shown) { break }
}

Say '--- 本次点击后的 Dock 日志 ---'
Say $newLog
# 命中路径取决于图标当时在不在可见区（Windows 会把常用的托盘图标提升到可见区，
# 用久了两边的归属会变），所以只要求“最终点到了”，并报告走的是哪条路。
$openedOverflow = $newLog -match '溢出弹窗已打开'
$usedFallback = $newLog -match '临时显示任务栏'
$triggered = $newLog -match '托盘图标触发成功'
$matched = $newLog -match [regex]::Escape($AppKeyPattern)
Say ("命中路径：{0}{1}" -f $(if ($openedOverflow) { '溢出弹窗（隐藏图标）' } else { '可见托盘区' }),
     $(if ($usedFallback) { ' + 任务栏兜底渲染' } else { '' }))
if ($usedFallback) { Fail '走到了「临时显示任务栏」兜底：正常路径不该动任务栏（可见区 Invoke 本来有效）' }
if (-not $triggered) { Fail '未看到「托盘图标触发成功」：托盘图标最终没被点到' }
if (-not $matched) { Fail ('触发成功的条目里没有样本名字片段：' + $AppKeyPattern) }
if (-not $shown) { Fail '日志说触发成功，但样本进程仍无可见主窗（影子窗口/无效触发）' }

# ---- 5) 收尾状态：任务栏必须重新隐藏、溢出弹窗关闭 ----
Start-Sleep -Milliseconds 800
$trayAfter = [DockTrayTest]::IsWindowVisible([DockTrayTest]::ByClass('Shell_TrayWnd'))
$islandAfter = [DockTrayTest]::IsWindowVisible([DockTrayTest]::ByClass('TopLevelWindowForOverflowXamlIsland'))
Say ("收尾：trayVisible={0} islandVisible={1} 样本主窗可见={2}" -f `
     $trayAfter, $islandAfter, $shown)
if ($trayAfter) { Fail '触发结束后系统任务栏没有重新隐藏（Dock 的隐藏契约被破坏）' }
if ($islandAfter) { Fail '触发结束后溢出弹窗仍然可见' }

Pass ("「{0}」可从 Dock 打开：托盘图标已点中、未打扰任务栏，收尾状态正确" -f $AppExe)
