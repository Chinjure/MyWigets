# service_guard_test.ps1 - 服务守护安装/归属自检
#
# 默认只读检查；-Install / -Uninstall / -RepairQuark 会弹一次 UAC。
param(
    [switch]$Install,
    [switch]$Uninstall,
    [switch]$RepairQuark
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$guard = Join-Path $root 'bin\MyWigetsServiceGuard.exe'
$cleanupTest = Join-Path $root 'bin\app_cleanup_test.exe'
$quark = 'C:\Program Files\QuarkCloudDrive\quark_cloud_drive.exe'

if (-not (Test-Path $guard)) { throw "缺少 $guard" }
Write-Host "[1/3] 服务守护程序存在：$guard" -ForegroundColor Green

if ((Test-Path $cleanupTest) -and (Test-Path $quark)) {
    Write-Host '[2/3] Quark 服务归属：' -ForegroundColor Cyan
    & $cleanupTest --services $quark
} else {
    Write-Warning '缺少 app_cleanup_test.exe 或 Quark，跳过归属检查'
}

$marker = Join-Path $env:LOCALAPPDATA 'DesktopSuite\ServiceGuard\installed.flag'
$installedExe = Join-Path $env:ProgramFiles 'DesktopSuite\MyWigetsServiceGuard.exe'
$installed = (Test-Path $marker) -and (Test-Path $installedExe)
Write-Host "[3/3] 已安装=$installed marker=$marker" -ForegroundColor Cyan

if ($Install) {
    Write-Host '启动安装/修复（UAC）...' -ForegroundColor Yellow
    $p = Start-Process -FilePath $guard -ArgumentList '--install' `
        -Verb RunAs -Wait -PassThru
    Write-Host "安装退出码=$($p.ExitCode)"
    $installed = (Test-Path $marker) -and (Test-Path $installedExe)
    if (-not $installed) { throw '安装后 marker/Program Files exe 不存在' }
    Write-Host '安装/修复完成；任务已允许电池运行' -ForegroundColor Green
}
if ($Uninstall) {
    if (-not $installed) {
        Write-Host '未安装，跳过卸载' -ForegroundColor Yellow
    } else {
        Write-Host '启动卸载（UAC）...' -ForegroundColor Yellow
        $p = Start-Process -FilePath $guard -ArgumentList '--uninstall' `
            -Verb RunAs -Wait -PassThru
        Write-Host "卸载退出码=$($p.ExitCode)"
    }
}
function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if ($RepairQuark) {
    if (-not (Test-Path $quark)) { throw '未找到 Quark' }
    Write-Host '修复 Quark 服务启动类型（安装新 broker + 重新记录 Auto + 再禁用）...' -ForegroundColor Yellow
    $argList = @('--install', '--repair-auto', $quark)
    if (Test-Admin) {
        # 已经是管理员：直接启动，不再触发 UAC。
        $p = Start-Process -FilePath $guard -ArgumentList $argList -Wait -PassThru
    } else {
        try {
            $p = Start-Process -FilePath $guard -ArgumentList $argList `
                -Verb RunAs -Wait -PassThru -ErrorAction Stop
        } catch {
            Write-Warning 'UAC 被取消。请重试并点「是」，或右键以管理员身份打开 PowerShell 后运行：'
            Write-Host ('  & "{0}" --install --repair-auto "{1}"' -f $guard, $quark) -ForegroundColor Yellow
            throw
        }
    }
    Write-Host "修复退出码=$($p.ExitCode)"
    if ($p.ExitCode -ne 0) { throw '修复失败，查看 guard.log' }
    Write-Host '修复完成' -ForegroundColor Green
}
if (-not $Install -and -not $Uninstall -and -not $RepairQuark) {
    Write-Host '只读检查完成。要安装：-Install；修复 Quark 启动类型：-RepairQuark' -ForegroundColor Green
}
