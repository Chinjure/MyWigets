# app_cpu_limit_test.ps1 - 编译并运行 app_cpu_limit 的单元 + 真实 CPU 限速测试
#
# 用法（脚本执行策略可能禁止直接运行）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\app_cpu_limit_test.ps1
#
# 测试只操作临时目录里由测试自己派生/复制的 app.exe、helper.exe，不会碰
# 系统里正在运行的其它应用。集成测试会真实创建 Job Object 并把两个忙循环
# 进程限制到 0.01%，用 GetProcessTimes 的 CPU 时间增量验证限速/恢复。

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root 'bin'
$exe = Join-Path $outDir 'app_cpu_limit_test.exe'
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
$bat = Join-Path $env:TEMP 'mywigets_app_cpu_limit_test_build.bat'
$lines = @('@echo off', 'setlocal')
if ($vsDevCmd) {
    $lines += "call `"$vsDevCmd`" -no_logo -arch=x64"
    $lines += 'if errorlevel 1 exit /b 1'
}
$lines += "cd /d `"$root`""
$lines += "cl /nologo /std:c++17 /EHsc $optim /W4 /permissive- /utf-8 " +
          "/DUNICODE /D_UNICODE /DNOMINMAX " +
          "/I`"$root\src`" /Fe:`"$exe`" " +
          "`"$root\tests\app_cpu_limit_test.cpp`" `"$root\src\app_cpu_limit.cpp`" " +
          "/link advapi32.lib"
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
