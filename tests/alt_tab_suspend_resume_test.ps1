# tests/alt_tab_suspend_resume_test.ps1
#
# 注意：本文件必须保存为 UTF-8 with BOM。Windows PowerShell 5.1 在无 BOM 时
# 会按 ANSI 解码中文注释，导致 Add-Type 的 here-string 解析失败（报“字符串
# 缺少终止符”）。
#
# 回归验证：被「后台应用挂起」冻结的最小化窗口，必须能在 Alt+Tab 切换时
# 先恢复运行，而不是让切换器/激活对着一个冻结进程卡住。
#
# 判定链：
#   1. 自建一个单窗口 GUI 进程（bin\alt_tab_victim\victim.exe），等到
#      Suspender 的最小进程年龄（8s）之后最小化它；
#   2. Dock 的挂起日志 + SendMessageTimeout 超时 + 心跳文件停止增长，
#      三者同时证明它真的被 NtSuspendProcess 冻结；
#   3. 按住 Alt、按 Tab、在松开 Alt 之前检查：victim 已经重新响应、
#      心跳恢复增长（proves "激活前恢复"），日志出现 [挂起] Alt+Tab；
#   4. 松开 Alt 后若 victim 是切换目标，窗口应已还原并成为前台。
#
# 前置条件：MyWigets 正在运行（Dock + 顶栏），且 Dock 已开启「后台应用挂起」
# （HKCU\Software\DesktopSuite\Dock\SuspendInactive=1）。缺条件时 [SKIP]
# 并退出 0，不擅自修改用户设置。
#
# 用法（脚本执行策略可能禁止直接运行）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\alt_tab_suspend_resume_test.ps1
#
# 退出码：0 = PASS/SKIP，1 = FAIL。

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root 'bin\alt_tab_victim'
$victimExe = Join-Path $outDir 'victim.exe'
$readyFile = Join-Path $outDir 'ready.txt'
$heartbeatFile = Join-Path $outDir 'heartbeat.bin'
# dock_main.cpp 的 LogInit 按“exe 所在目录的上一级”写 logs\dock.log：
#   bin\MyWigets-x64.exe        → <repo>\logs\dock.log
#   C:\Users\may\Desktop\MyWigets-x64.exe → C:\Users\may\logs\dock.log
# 部署后的实例在桌面，直接写死 $root\logs 会读错文件。以运行中进程的真实
# 路径反推，和实现保持同一口径。
$logPath = Join-Path $root 'logs\dock.log'
$suite = Get-Process -Name 'MyWigets-x64' -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($suite -and $suite.Path) {
    $logPath = Join-Path (Split-Path -Parent (Split-Path -Parent $suite.Path)) 'logs\dock.log'
}

$fails = 0

function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) {
        Write-Host ("PASS  {0}  {1}" -f $name, $detail)
    } else {
        Write-Host ("FAIL  {0}  {1}" -f $name, $detail) -ForegroundColor Red
        $script:fails++
    }
}

function Skip([string]$reason) {
    Write-Host ("SKIP  {0}" -f $reason) -ForegroundColor Yellow
    exit 0
}

function Find-VsDevCmd {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if ($vsPath) {
            $candidate = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }
    return $null
}

# ---- 前置条件 ----
Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public class AltTabProbe {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll")]
  public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")]
  public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")]
  public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")]
  public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")]
  public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")]
  public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr SendMessageTimeoutW(
      IntPtr h, uint msg, IntPtr wp, IntPtr lp, uint flags, uint timeout,
      out IntPtr result);

  const int SMTO_BLOCK = 0x0001;
  const int SMTO_ABORTIFHUNG = 0x0002;

  public static bool WindowExists(string cls) {
    return FindWindowW(cls, null) != IntPtr.Zero;
  }

  // 窗口所属 UI 线程是否还能在 timeoutMs 内处理 WM_NULL。
  // 被 NtSuspendProcess 冻结的窗口一定会超时。
  public static bool Responds(IntPtr h, int timeoutMs) {
    if (h == IntPtr.Zero || !IsWindow(h)) return false;
    IntPtr result;
    IntPtr ok = SendMessageTimeoutW(
        h, 0, IntPtr.Zero, IntPtr.Zero,
        (uint)(SMTO_BLOCK | SMTO_ABORTIFHUNG), (uint)timeoutMs, out result);
    return ok != IntPtr.Zero;
  }

  // 解锁前台限制的常规做法：按一下 Alt 再 SetForegroundWindow。
  public static bool ForceForeground(IntPtr h) {
    keybd_event(0x12, 0, 0, UIntPtr.Zero);
    keybd_event(0x12, 0, 2, UIntPtr.Zero);
    return SetForegroundWindow(h);
  }

  public static void AltDown() { keybd_event(0x12, 0, 0, UIntPtr.Zero); }
  public static void AltUp() { keybd_event(0x12, 0, 2, UIntPtr.Zero); }
  public static void TabTap() {
    keybd_event(0x09, 0, 0, UIntPtr.Zero);
    keybd_event(0x09, 0, 2, UIntPtr.Zero);
  }
}
'@

if (-not [AltTabProbe]::WindowExists('DesktopDockWindow')) {
    Skip 'Dock 未运行（找不到 DesktopDockWindow）'
}
if (-not [AltTabProbe]::WindowExists('DesktopTopBarWindow')) {
    Skip '顶栏未运行（找不到 DesktopTopBarWindow）'
}
$reg = Get-ItemProperty -Path 'HKCU:\Software\DesktopSuite\Dock' -ErrorAction SilentlyContinue
if (-not $reg -or $reg.SuspendInactive -ne 1) {
    Skip '后台应用挂起未开启（HKCU\Software\DesktopSuite\Dock\SuspendInactive != 1）'
}
if (-not $reg -or $reg.LogEnabled -ne 1) {
    Skip 'Dock 运行日志未开启（LogEnabled != 1），无法断言 Alt+Tab 恢复路径'
}

# ---- 编译 victim ----
$vsDevCmd = Find-VsDevCmd
if (-not $vsDevCmd -and -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'MSVC not found. Run from "x64 Native Tools Command Prompt" or install the VS C++ workload.'
}

$null = New-Item -ItemType Directory -Force -Path $outDir
$bat = Join-Path $env:TEMP 'mywigets_alt_tab_victim_build.bat'
$lines = @('@echo off', 'setlocal')
if ($vsDevCmd) {
    $lines += "call `"$vsDevCmd`" -no_logo -arch=x64"
    $lines += 'if errorlevel 1 exit /b 1'
}
$lines += "cd /d `"$root`""
$lines += "cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /permissive- /utf-8 " +
          "/DUNICODE /D_UNICODE /DNOMINMAX " +
          "/Fe:`"$victimExe`" `"$root\tests\alt_tab_victim.cpp`" " +
          "/link /SUBSYSTEM:WINDOWS user32.lib shell32.lib"
$lines += 'if errorlevel 1 exit /b 1'
$lines += 'endlocal'
Set-Content -Path $bat -Value $lines -Encoding Ascii
& cmd.exe /d /c $bat
$buildCode = $LASTEXITCODE
Remove-Item -Path $bat -Force -ErrorAction SilentlyContinue
if ($buildCode -ne 0) {
    throw "victim build failed, exit code: $buildCode"
}

# ---- 创建“另一个窗口”，用来把 victim 变成 MRU 里的前一个窗口 ----
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$other = New-Object System.Windows.Forms.Form
$other.Text = 'AltTabSuspendOther'
$other.Size = New-Object System.Drawing.Size(420, 260)
$other.StartPosition = 'Manual'
$other.Location = New-Object System.Drawing.Point(180, 180)
$other.Show()
[System.Windows.Forms.Application]::DoEvents()

$originalForeground = [AltTabProbe]::GetForegroundWindow()
$victim = $null
$victimPid = 0
$victimHwnd = [IntPtr]::Zero

try {
    Remove-Item -Path $readyFile, $heartbeatFile -Force -ErrorAction SilentlyContinue
    # 用 CreateProcess（UseShellExecute=false）而不是 Start-Process：
    # 本机对新建的 GUI exe 走 ShellExecute 会返回 1223（用户取消），
    # 直接 CreateProcess 正常。PID 仍以 victim 写出的 ready 文件为准。
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $victimExe
    $psi.UseShellExecute = $false
    $psi.Arguments = '"' + $readyFile + '" "' + $heartbeatFile + '"'
    $victim = [System.Diagnostics.Process]::Start($psi)

    $deadline = (Get-Date).AddSeconds(8)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $readyFile) { break }
        Start-Sleep -Milliseconds 50
    }
    if (-not (Test-Path $readyFile)) {
        throw 'victim ready 文件未出现（进程启动失败？）'
    }
    $parts = @((Get-Content -Raw -Path $readyFile).Trim() -split '\s+')
    if ($parts.Count -lt 2) { throw 'victim ready 文件格式错误' }
    $victimPid = [int]$parts[0]
    $victimHwnd = [IntPtr]([Convert]::ToInt64($parts[1].Substring(2), 16))
    Write-Host ("victim pid={0} hwnd=0x{1:X}" -f $victimPid, $victimHwnd.ToInt64())

    # Suspender 的最小进程年龄是 8s：太早最小化会被 TooYoung 延后。
    Start-Sleep -Seconds 9

    # 让 victim 成为“前一个前台窗口”，随后把另一个窗口置前台并最小化
    # victim —— 这样顶栏焦点不是 victim，挂起条件成立；同时 Alt+Tab 的
    # MRU 顺序里 victim 紧跟在当前窗口之后。
    [void][AltTabProbe]::ForceForeground($victimHwnd)
    Start-Sleep -Milliseconds 300
    [void][AltTabProbe]::ForceForeground($other.Handle)
    Start-Sleep -Milliseconds 300

    [void][AltTabProbe]::ShowWindow($victimHwnd, 6)  # SW_MINIMIZE
    Start-Sleep -Seconds 4

    $suspended = $false
    $deadline = (Get-Date).AddSeconds(6)
    while ((Get-Date) -lt $deadline) {
        if (-not [AltTabProbe]::Responds($victimHwnd, 400)) {
            $suspended = $true
            break
        }
        Start-Sleep -Milliseconds 300
    }
    Check 'victim 被挂起（UI 线程不再响应）' $suspended `
        ("hwnd=0x{0:X}" -f $victimHwnd.ToInt64())

    $hbBefore = 0
    if (Test-Path $heartbeatFile) {
        $hbBefore = (Get-Item $heartbeatFile).Length
    }
    Start-Sleep -Milliseconds 500
    $hbAfter = if (Test-Path $heartbeatFile) { (Get-Item $heartbeatFile).Length } else { 0 }
    Check '挂起期间心跳停止增长' ($hbAfter -eq $hbBefore) `
        ("before={0} after={1}" -f $hbBefore, $hbAfter)

    $logLinesBefore = 0
    if (Test-Path $logPath) {
        $logLinesBefore = @(Get-Content -Path $logPath -Encoding UTF8).Count
    }
    $hbResumeBefore = $hbAfter

    # ---- 关键：按住 Alt 按 Tab，保持切换器打开，检查“松手前”已恢复 ----
    [AltTabProbe]::AltDown()
    Start-Sleep -Milliseconds 100
    [AltTabProbe]::TabTap()

    $resumedWhileHolding = $false
    $deadline = (Get-Date).AddSeconds(3)
    while ((Get-Date) -lt $deadline) {
        if ([AltTabProbe]::Responds($victimHwnd, 400)) {
            $resumedWhileHolding = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    Check '按住 Alt 未松开时 victim 已恢复响应' $resumedWhileHolding ''

    Start-Sleep -Milliseconds 500
    $hbResumeAfter = if (Test-Path $heartbeatFile) { (Get-Item $heartbeatFile).Length } else { 0 }
    Check '松开 Alt 前心跳恢复增长' ($hbResumeAfter -gt $hbResumeBefore) `
        ("before={0} after={1}" -f $hbResumeBefore, $hbResumeAfter)

    [AltTabProbe]::AltUp()
    Start-Sleep -Milliseconds 800

    # ---- 日志断言：必须来自新增的 Alt+Tab 路径，而不是旧的前台兜底 ----
    $newLog = @()
    if (Test-Path $logPath) {
        $all = @(Get-Content -Path $logPath -Encoding UTF8)
        if ($all.Count -gt $logLinesBefore) {
            $newLog = $all[$logLinesBefore..($all.Count - 1)]
        }
    }
    $newLogText = $newLog -join "`n"
    $newLogTail = ($newLog | Select-Object -Last 3) -join ' | '
    Check 'Dock 日志出现 Alt+Tab 恢复记录' ($newLogText -match '\[挂起\] Alt\+Tab') $newLogTail
    Check '日志包含 victim.exe' ($newLogText -match 'victim\.exe') ''
    # 任务切换器窗口检测是触摸板手势的唯一“切换前”信号；键盘 Alt+Tab
    # 也会显示同一个窗口，所以这里能直接验证该路径是否生效。
    Check 'Dock 日志出现任务切换器显示记录' ($newLogText -match '任务切换器显示') ''
    Check 'Dock 日志出现任务切换器关闭记录' ($newLogText -match '任务切换器关闭') ''

    $fg = [AltTabProbe]::GetForegroundWindow()
    if ($fg -eq $victimHwnd) {
        Check 'victim 已还原（非最小化）' (-not [AltTabProbe]::IsIconic($victimHwnd)) ''
    } else {
        Write-Host ("INFO  Alt+Tab 落点不是 victim（0x{0:X}），跳过前台/还原断言" -f $fg.ToInt64())
    }

    # ---- 保护期必须会过期：再次最小化后仍能正常重新挂起 ----
    # 1.5s 落定期 + 1.2s 宽限 + 一次保底复核，5s 足够。若这里失败，说明
    # Alt+Tab 之后挂起功能被永久停用（保护期没有结束）。
    [void][AltTabProbe]::ForceForeground($other.Handle)
    Start-Sleep -Milliseconds 300
    [void][AltTabProbe]::ShowWindow($victimHwnd, 6)  # SW_MINIMIZE
    Start-Sleep -Seconds 5
    $resuspended = $false
    $deadline = (Get-Date).AddSeconds(4)
    while ((Get-Date) -lt $deadline) {
        if (-not [AltTabProbe]::Responds($victimHwnd, 400)) {
            $resuspended = $true
            break
        }
        Start-Sleep -Milliseconds 300
    }
    Check 'Alt+Tab 后再次最小化能重新挂起' $resuspended ''
} finally {
    # 保证 Alt 不处于按下状态，避免影响后续输入
    [AltTabProbe]::AltUp()
    if ($victim -and -not $victim.HasExited) {
        Stop-Process -Id $victimPid -Force -ErrorAction SilentlyContinue
    }
    $other.Close()
    $other.Dispose()
    if ($originalForeground -ne [IntPtr]::Zero) {
        [void][AltTabProbe]::SetForegroundWindow($originalForeground)
    }
}

Write-Host ''
if ($fails -ne 0) {
    Write-Host ("FAILED: {0} check(s)" -f $fails) -ForegroundColor Red
    exit 1
}
Write-Host 'ALL PASS' -ForegroundColor Green
exit 0
