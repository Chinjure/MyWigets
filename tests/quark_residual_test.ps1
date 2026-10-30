# quark_residual_test.ps1 —— 夸克网盘关闭后零进程验收（阻断项）
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\quark_residual_test.ps1
#   powershell ... -File tests\quark_residual_test.ps1 -AutoClose
#
# 前置：
#   - 已构建/部署包含 app_cleanup 的 MyWigets，Dock 正在运行；
#   - 已开启「关闭后清理残留进程」（脚本会临时置 ResidualCleanup=1）；
#   - 夸克网盘已安装。
#
# 脚本会：备份 Dock 配置 → 通过 Dock 的 WM_COPYDATA 把夸克固定到 Dock →
# 启动夸克 → 让你完全退出（窗口和托盘都消失）→ 等日志 [清理] →
# 采样 10s 断言无 Quark 进程/服务 → 重启夸克验证可正常拉起 → 恢复配置。
# 采样只用于测试观察；产品清理本身没有轮询。

param(
    [switch]$AutoClose,
    [switch]$KeepConfig,
    [int]$WaitLogSeconds = 60
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dockProc = Get-Process -Name 'MyWigets-x64' -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $dockProc) {
    throw 'MyWigets-x64 未运行，无法验收。请先 deploy_mywigets.ps1 并启动 Dock。'
}
$dockExe = $dockProc.Path
$repoDir = Split-Path (Split-Path $dockExe -Parent) -Parent
$dockLog = Join-Path $repoDir 'logs\dock.log'
$quarkExe = 'C:\Program Files\QuarkCloudDrive\quark_cloud_drive.exe'
$quarkShortcut = 'C:\Users\Public\Desktop\夸克网盘.lnk'
if (-not (Test-Path $quarkExe)) {
    throw "夸克网盘未安装或路径变化：$quarkExe"
}

function Get-DockWindow {
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class QuarkTestNative {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint msg,
        IntPtr wp, ref COPYDATASTRUCT lp, uint flags, uint timeout, out IntPtr result);
    [StructLayout(LayoutKind.Sequential)]
    public struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }
    public static IntPtr FindByClass(string cls) {
        IntPtr found = IntPtr.Zero;
        EnumProc cb = (h, l) => {
            var b = new StringBuilder(128);
            GetClassNameW(h, b, 128);
            if (b.ToString() == cls) { found = h; return false; }
            return true;
        };
        EnumWindows(cb, IntPtr.Zero);
        return found;
    }
}
'@ -ErrorAction SilentlyContinue
    return [QuarkTestNative]::FindByClass('DesktopDockWindow')
}

function Add-QuarkPin {
    $hwnd = Get-DockWindow
    if ($hwnd -eq [IntPtr]::Zero) { throw '找不到 DesktopDockWindow' }
    $bytes = [Text.Encoding]::Unicode.GetBytes($quarkExe + [char]0)
    $lp = [Runtime.InteropServices.Marshal]::AllocHGlobal($bytes.Length)
    try {
        [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $lp, $bytes.Length)
        $cds = New-Object 'QuarkTestNative+COPYDATASTRUCT'
        $cds.dwData = [IntPtr]0x4B434F44   # 'DOCK'
        $cds.cbData = $bytes.Length
        $cds.lpData = $lp
        $result = [IntPtr]::Zero
        [void][QuarkTestNative]::SendMessageTimeoutW($hwnd, 0x004A,
            [IntPtr]::Zero, [ref]$cds, 2, 2000, [ref]$result)
    } finally {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($lp)
    }
}

function Get-QuarkProcesses {
    $roots = @(
        'C:\Program Files\QuarkCloudDrive',
        'C:\Program Files\QuarkCloudDriveUpdater',
        'C:\Program Files (x86)\QuarkCloudDriveUpdater',
        "$env:LOCALAPPDATA\QuarkCloudDrive",
        "$env:APPDATA\QuarkCloudDrive"
    )
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $p = $_
            if (-not $p.ExecutablePath) { return $false }
            foreach ($rootPath in $roots) {
                if ($p.ExecutablePath.StartsWith($rootPath,
                        [StringComparison]::OrdinalIgnoreCase)) {
                    return $true
                }
            }
            return $false
        } |
        Select-Object ProcessId, ParentProcessId, Name, ExecutablePath
}

function Get-QuarkServices {
    Get-CimInstance Win32_Service -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -match 'QuarkCloudDrive' -or
            $_.DisplayName -match 'Quark' -or
            $_.PathName -match 'QuarkCloudDrive'
        } |
        Select-Object Name, DisplayName, State, ProcessId, PathName
}

function Get-LogSize {
    # 返回字符数而不是字节数：Wait-LogPattern 用 .Substring 比较，
    # UTF-8 中文的字节数和字符数不同，用文件长度会永远跳过新日志。
    if (Test-Path $dockLog) {
        return (Get-Content $dockLog -Raw -Encoding UTF8).Length
    }
    return 0
}

function Wait-LogPattern([string]$pattern, [int]$seconds, [long]$from) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $dockLog) {
            $text = Get-Content $dockLog -Raw -Encoding UTF8 -ErrorAction SilentlyContinue
            if ($text -and $text.Length -gt $from) {
                $tail = $text.Substring([Math]::Min($from, $text.Length))
                if ($tail -match $pattern) { return $true }
            }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# ---- 0) 备份配置 + 开启清理 ----
$backup = Join-Path $env:TEMP ('mywigets_dock_backup_' + [guid]::NewGuid().ToString('N') + '.reg')
& reg.exe export 'HKCU\Software\DesktopSuite\Dock' $backup /y | Out-Null
if ($LASTEXITCODE -ne 0) { throw '备份 Dock 注册表失败' }
$key = 'HKCU:\Software\DesktopSuite\Dock'
Set-ItemProperty -Path $key -Name ResidualCleanup -Type DWord -Value 1
Write-Host '[0/5] 已备份 Dock 配置并临时开启 ResidualCleanup=1' -ForegroundColor Cyan

# ---- 1) 固定夸克到 Dock（若已固定则消息会去重）----
Add-QuarkPin
Start-Sleep -Seconds 2
Write-Host '[1/5] 已向 Dock 请求固定夸克网盘' -ForegroundColor Cyan

# ---- 2) 启动夸克 ----
if (Get-Process -Name 'quark_cloud_drive' -ErrorAction SilentlyContinue) {
    Write-Host '[2/5] 夸克已在运行，请直接使用当前实例' -ForegroundColor Yellow
} else {
    if (Test-Path $quarkShortcut) {
        Start-Process -FilePath $quarkShortcut | Out-Null
    } else {
        Start-Process -FilePath $quarkExe -ArgumentList '--launch-from=desktopicon --brand-clouddrive' | Out-Null
    }
    Write-Host '[2/5] 已启动夸克，等待主窗口...' -ForegroundColor Cyan
}
$deadline = (Get-Date).AddSeconds(60)
$started = $false
while ((Get-Date) -lt $deadline) {
    $p = Get-Process -Name 'quark_cloud_drive' -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) { $started = $true; break }
    Start-Sleep -Milliseconds 500
}
if (-not $started) { throw '60s 内未看到夸克主窗口，无法继续验收' }
$logFrom = Get-LogSize
Write-Host '[3/5] 夸克已启动，日志基线已记录' -ForegroundColor Cyan

# ---- 3) 让用户完全退出（或 -AutoClose 只发 WM_CLOSE）----
if ($AutoClose) {
    # 让 Dock 先观察到「夸克已打开」再关闭；否则关闭太快时 Dock 可能只
    # 看到最终无窗口状态，这是事件顺序问题，不是清理失败。
    Start-Sleep -Seconds 8
    $p = Get-Process -Name 'quark_cloud_drive' -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) { [void]$p.CloseMainWindow() }
    Start-Sleep -Seconds 3
    $stillWindow = Get-Process -Name 'quark_cloud_drive' -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 }
    if ($stillWindow) {
        throw 'AutoClose 后夸克仍有主窗口。请改用交互模式，从托盘菜单完全退出。'
    }
    # 主窗关闭后可能仍有 proxy/helper 进程；这正是本次要清理的对象。
} else {
    Write-Host ''
    Write-Host '请现在完全退出夸克网盘：关闭主窗口，并在托盘菜单选择“退出”；' -ForegroundColor Yellow
    Write-Host '确认任务栏/托盘里没有夸克图标，也没有夸克窗口。' -ForegroundColor Yellow
    [void](Read-Host '完成后按 Enter 继续')
}

# ---- 4) 等清理日志 + 采样断言 ----
Write-Host '[4/5] 等待 Dock 的 [清理] 日志...' -ForegroundColor Cyan
if (-not (Wait-LogPattern '\[清理\].*(夸克|Quark).*(已清理|无进程|需要管理员|失败|启动中|有托盘|有窗口)' $WaitLogSeconds $logFrom)) {
    Write-Warning "未在 ${WaitLogSeconds}s 内看到夸克清理日志。日志尾部："
    if (Test-Path $dockLog) { Get-Content $dockLog -Tail 30 }
    throw '夸克清理日志未出现：检查 ResidualCleanup/Dock 是否为新版本。'
}

$failures = New-Object System.Collections.Generic.List[string]
# 标准用户的 Dock 无法停止 LocalSystem 的 Quark 更新服务。若清理日志
# 显示 needsElevation 或采样时服务仍 Running，调用一次性提权助手；
# 用户可以拒绝 UAC，此时测试继续并最终如实失败。
$quarkServicesRunning = @(Get-QuarkServices | Where-Object State -eq 'Running')
if ($quarkServicesRunning.Count -gt 0) {
    $helper = Join-Path $root 'bin\MyWigetsCleanup.exe'
    if (Test-Path $helper) {
        Write-Host '检测到 Quark 更新服务；启动一次性提权清理助手（会弹 UAC）...' -ForegroundColor Yellow
        try {
            $hp = Start-Process -FilePath $helper `
                -ArgumentList @('--app', $quarkExe) `
                -Verb RunAs -Wait -PassThru -ErrorAction Stop
            Write-Host "提权清理助手退出码=$($hp.ExitCode)" -ForegroundColor Cyan
        } catch {
            Write-Warning "提权清理助手未运行：$($_.Exception.Message)"
        }
    } else {
        Write-Warning "未找到 $helper，无法停止 SYSTEM 服务"
    }
}

# 观察 10s，但只在窗口结束时判定。清理结果刚落下时，最后退出的进程
# 可能还在内核退出中；短暂存在不算驻留。
for ($i = 0; $i -lt 10; $i++) { Start-Sleep -Seconds 1 }
$procs = @(Get-QuarkProcesses)
$svcs = @(Get-QuarkServices | Where-Object State -eq 'Running')
foreach ($p in $procs) {
    $failures.Add("进程驻留: $($p.Name) pid=$($p.ProcessId) $($p.ExecutablePath)")
}
foreach ($s in $svcs) {
    $failures.Add("服务运行: $($s.Name) pid=$($s.ProcessId)")
}

if ($failures.Count -gt 0) {
    $failures | Sort-Object -Unique | ForEach-Object { Write-Host "FAIL $_" -ForegroundColor Red }
    throw '夸克关闭后仍有进程/服务驻留，验收失败。'
}
Write-Host '[PASS] 夸克关闭后 10s 内无 Quark 进程、无 Quark 服务运行' -ForegroundColor Green

# ---- 5) 重新启动验证 ----
Start-Process -FilePath $quarkExe -ArgumentList '--launch-from=desktopicon --brand-clouddrive' | Out-Null
$deadline = (Get-Date).AddSeconds(60)
$relaunched = $false
while ((Get-Date) -lt $deadline) {
    $p = Get-Process -Name 'quark_cloud_drive' -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) { $relaunched = $true; break }
    Start-Sleep -Milliseconds 500
}
if (-not $relaunched) { throw '清理后重新启动夸克失败' }
Write-Host '[5/5] 清理后夸克可正常重新启动' -ForegroundColor Green

if (-not $KeepConfig) {
    & reg.exe import $backup | Out-Null
    Remove-Item $backup -Force -ErrorAction SilentlyContinue
    Write-Host '已恢复 Dock 配置；若测试新增了固定项，重启 Dock 后恢复原固定列表。' -ForegroundColor Cyan
} else {
    Write-Host "保留测试配置；备份：$backup" -ForegroundColor Yellow
}
Write-Host 'QUARK RESIDUAL TEST PASS' -ForegroundColor Green
