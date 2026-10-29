# app_suspend_test.ps1 - 编译并运行 app_suspend 的单元 + 真实挂起/恢复测试
#
# 用法（脚本执行策略可能禁止直接运行）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\app_suspend_test.ps1
#
# 测试只操作 bin 下由测试自己创建/复制的 app.exe、helper.exe，不会碰
# 系统里正在运行的其它应用。集成测试会真实调用 NtSuspendProcess，并用
# 两个心跳文件验证主进程与不同 exe 的辅助进程一起停/一起恢复；持久化
# 恢复用例需要可写的 HKCU\Software\DesktopSuite\Dock，受限运行环境下
# 会打印 [SKIP] 而不是误报失败。

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root 'bin'
$exe = Join-Path $outDir 'app_suspend_test.exe'
$null = New-Item -ItemType Directory -Force -Path $outDir

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

$vsDevCmd = Find-VsDevCmd
if (-not $vsDevCmd -and -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'MSVC not found. Run from "x64 Native Tools Command Prompt" or install the VS C++ workload.'
}

$optim = if ($Configuration -eq 'Release') { '/O2 /MT' } else { '/Od /MTd /Zi' }
$bat = Join-Path $env:TEMP 'mywigets_app_suspend_test_build.bat'
$lines = @('@echo off', 'setlocal')
if ($vsDevCmd) {
    $lines += "call `"$vsDevCmd`" -no_logo -arch=x64"
    $lines += 'if errorlevel 1 exit /b 1'
}
$lines += "cd /d `"$root`""
$lines += "cl /nologo /std:c++17 /EHsc $optim /W4 /permissive- /utf-8 " +
          "/DUNICODE /D_UNICODE /DNOMINMAX " +
          "/I`"$root\src`" /Fe:`"$exe`" " +
          "`"$root\tests\app_suspend_test.cpp`" `"$root\src\app_suspend.cpp`" " +
          "/link advapi32.lib dwmapi.lib user32.lib"
$lines += 'if errorlevel 1 exit /b 1'
$lines += 'endlocal'
Set-Content -Path $bat -Value $lines -Encoding Ascii

& cmd.exe /d /c $bat
$code = $LASTEXITCODE
Remove-Item -Path $bat -Force -ErrorAction SilentlyContinue
if ($code -ne 0) {
    throw "cl.exe build failed, exit code: $code"
}

Write-Host "Running: $exe" -ForegroundColor Green
& $exe
exit $LASTEXITCODE
