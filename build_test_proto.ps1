# build_test_proto.ps1 - 编译并运行 topbar_ws_proto 协议层单元测试（控制台）
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
if (-not $root) {
    $root = Split-Path -Parent $MyInvocation.MyCommand.Path
}
Set-Location $root

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
    throw 'MSVC not found.'
}

$src = Join-Path $root 'test_proto.cpp'
$exe = Join-Path $root 'test_proto.exe'

$clLine = "cl /nologo /std:c++17 /EHsc /O2 /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE /I`"$root\src`" /Fe:`"$exe`" `"$src`" /link"

$bat = Join-Path $env:TEMP 'desktoptopbar_test_build.bat'
$lines = @('@echo off', 'setlocal')
if ($vsDevCmd) {
    $lines += "call `"$vsDevCmd`" -no_logo -arch=x64"
    $lines += 'if errorlevel 1 exit /b 1'
}
$lines += "cd /d `"$root`""
$lines += $clLine
$lines += 'if errorlevel 1 exit /b 1'
$lines += 'endlocal'
Set-Content -Path $bat -Value $lines -Encoding Ascii

& cmd.exe /d /c $bat
$code = $LASTEXITCODE
Remove-Item -Path $bat -Force -ErrorAction SilentlyContinue

if ($code -ne 0) {
    throw "cl.exe build failed, exit code: $code"
}

Write-Host ""
Write-Host "Running: $exe" -ForegroundColor Green
& $exe
exit $LASTEXITCODE
