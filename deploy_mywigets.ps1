# deploy_mywigets.ps1 —— 把构建产物部署到桌面并以新版本重启 MyWigets。
#
# 为什么需要它：MyWigets.exe 是常驻进程（Dock/顶栏/时钟/日历/启动台都在里面），
# 直接覆盖正在运行的 exe 会被文件锁拒绝，手工“右键退出 → 拷贝 → 双击”也容易
# 忘掉备份。本脚本按固定顺序做完整套：
#   1) 备份当前运行的 exe（bin\MyWigets-x64.prev.exe，可随时回滚）；
#   2) WM_CLOSE 给宿主窗口 → 宿主内部 StopAllWidgets() 优雅退出
#      （Dock 退出时恢复系统任务栏，不能 taskkill 硬杀）；
#   3) 超时未退出才 Stop-Process 兜底，并手动恢复任务栏可见性；
#   4) 覆盖部署新 exe 并重新启动，等待 Dock / 顶栏窗口出现后报告结果。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File deploy_mywigets.ps1
#   powershell ... -File deploy_mywigets.ps1 -NoRestart      # 只停+拷贝，不启动
param(
    [string]$Source,
    [string]$Target = 'C:\Users\may\Desktop\MyWigets-x64.exe',
    [switch]$NoRestart,
    [int]$StopTimeoutSec = 15
)

$ErrorActionPreference = 'Stop'
if (-not $Source) { $Source = Join-Path $PSScriptRoot 'bin\MyWigets-x64.exe' }
$backup = Join-Path $PSScriptRoot 'bin\MyWigets-x64.prev.exe'

$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Wig {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string win);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string win);
  public static IntPtr TrayWindow() { return FindWindowW("MyWigetsTrayWindow", null); }
  public static bool DockAlive() { return FindWindowW("DesktopDockWindow", null) != IntPtr.Zero; }
  public static bool TopbarAlive() { return FindWindowW("DesktopTopBarWindow", null) != IntPtr.Zero; }
  public static void RestoreTaskbar() {
    IntPtr t = FindWindowW("Shell_TrayWnd", null);
    if (t != IntPtr.Zero) ShowWindow(t, 5);   // SW_SHOW
    IntPtr sec = IntPtr.Zero;
    while ((sec = FindWindowExW(IntPtr.Zero, sec, "Shell_SecondaryTrayWnd", null)) != IntPtr.Zero) {
      ShowWindow(sec, 5);
    }
  }
}
'@
Add-Type -TypeDefinition $code -Language CSharp

# ---- 0) 校验源文件 ----
if (-not (Test-Path $Source)) { throw "构建产物不存在：$Source（先运行 build_mywigets.ps1）" }
$srcItem = Get-Item $Source
Write-Host ("[0/5] 源：{0} ({1} 字节, {2})" -f $srcItem.FullName, $srcItem.Length, $srcItem.LastWriteTime)

# ---- 1) 备份当前部署（若存在） ----
if (Test-Path $Target) {
    Copy-Item $Target $backup -Force
    Write-Host ("[1/5] 已备份现有 exe → {0}" -f $backup)
} else {
    Write-Host "[1/5] 目标位置暂无 exe，跳过备份"
}

# ---- 2) 优雅停止正在运行的实例 ----
$running = Get-Process -Name 'MyWigets-x64' -ErrorAction SilentlyContinue
if ($running) {
    $host_ = [Wig]::TrayWindow()
    Write-Host ("[2/5] 停止运行中的实例 pid={0}（宿主窗口 0x{1:X}）" -f ($running.Id -join ','), $host_.ToInt64())
    if ($host_ -ne [IntPtr]::Zero) {
        [Wig]::PostMessageW($host_, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
    } else {
        # 找不到宿主窗口（异常状态）：直接按线程数最多的进程硬停，任务栏下面兜底恢复
        $running | Stop-Process -Force
    }
    $deadline = (Get-Date).AddSeconds($StopTimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Name 'MyWigets-x64' -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 200
    }
    if (Get-Process -Name 'MyWigets-x64' -ErrorAction SilentlyContinue) {
        Write-Warning "优雅退出超时（${StopTimeoutSec}s），改为强制结束进程"
        Get-Process -Name 'MyWigets-x64' -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Milliseconds 500
        [Wig]::RestoreTaskbar()
        Write-Warning "已手动恢复系统任务栏显示（强杀不会走 Dock 的恢复逻辑）"
    } else {
        Write-Host "[2/5] 已优雅退出（Dock 已恢复系统任务栏）"
    }
} else {
    Write-Host "[2/5] 没有运行中的实例"
}

# ---- 3) 覆盖部署（文件锁可能延迟释放，重试几次） ----
$copied = $false
for ($i = 0; $i -lt 20; $i++) {
    try {
        Copy-Item $Source $Target -Force -ErrorAction Stop
        $copied = $true
        break
    } catch {
        Start-Sleep -Milliseconds 250
    }
}
if (-not $copied) { throw "覆盖部署失败（文件可能仍被占用）：$Target" }
Write-Host ("[3/5] 已部署 → {0}" -f $Target)

if ($NoRestart) {
    Write-Host "[4/5] -NoRestart：不启动新实例"
    return
}

# ---- 4) 启动新版本 ----
Start-Process -FilePath $Target -WorkingDirectory (Split-Path -Parent $Target) | Out-Null
Write-Host "[4/5] 已启动新实例，等待 Dock / 顶栏窗口就绪..."

# ---- 5) 校验组件窗口 ----
$deadline = (Get-Date).AddSeconds(20)
$dockOk = $false
$barOk = $false
while ((Get-Date) -lt $deadline) {
    $dockOk = [Wig]::DockAlive()
    $barOk = [Wig]::TopbarAlive()
    if ($dockOk -and $barOk) { break }
    Start-Sleep -Milliseconds 250
}
$proc = Get-Process -Name 'MyWigets-x64' -ErrorAction SilentlyContinue
$state = if ($proc) { "运行中 pid=" + (($proc | ForEach-Object { $_.Id }) -join ',') } else { "未运行" }
Write-Host ("[5/5] 进程={0} Dock窗口={1} 顶栏窗口={2}" -f $state, $dockOk, $barOk)
if (-not $proc -or -not $dockOk) {
    Write-Warning "新实例未正常起来，可回滚：Copy-Item '$backup' '$Target' -Force"
    exit 1
}
