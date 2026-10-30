# app_cleanup_test.ps1 - 编译并运行 app_cleanup 单元/集成测试
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root 'bin'
$exe = Join-Path $outDir 'app_cleanup_test.exe'
$null = New-Item -ItemType Directory -Force -Path $outDir

function Find-VsDevCmd {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if ($vsPath) {
            $candidate = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
            if (Test-Path $candidate) { return $candidate }
        }
    }
    return $null
}

$vsDevCmd = Find-VsDevCmd
if (-not $vsDevCmd -and -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'MSVC not found.'
}

$optim = if ($Configuration -eq 'Release') { '/O2 /MT' } else { '/Od /MTd /Zi' }
$bat = Join-Path $env:TEMP 'mywigets_app_cleanup_test_build.bat'
$lines = @('@echo off', 'setlocal')
if ($vsDevCmd) {
    $lines += "call `"$vsDevCmd`" -no_logo -arch=x64"
    $lines += 'if errorlevel 1 exit /b 1'
}
$lines += "cd /d `"$root`""
$lines += "cl /nologo /std:c++17 /EHsc $optim /W4 /permissive- /utf-8 " +
          "/DUNICODE /D_UNICODE /DNOMINMAX " +
          "/I`"$root\src`" /Fe:`"$exe`" " +
          "`"$root\tests\app_cleanup_test.cpp`" `"$root\src\app_cleanup.cpp`" `"$root\src\app_suspend.cpp`" " +
          "/link advapi32.lib dwmapi.lib user32.lib"
$lines += 'if errorlevel 1 exit /b 1'
$lines += 'endlocal'
Set-Content -Path $bat -Value $lines -Encoding Ascii

& cmd.exe /d /c $bat
$code = $LASTEXITCODE
Remove-Item -Path $bat -Force -ErrorAction SilentlyContinue
if ($code -ne 0) { throw "cl.exe build failed, exit code: $code" }

Write-Host "Running: $exe" -ForegroundColor Green
& $exe
exit $LASTEXITCODE
