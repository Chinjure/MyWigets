param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64'
)

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
if ($vsDevCmd -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    # 已在开发者环境中则直接用当前 cl
    $vsDevCmd = $null
}
if (-not $vsDevCmd -and -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'MSVC not found. Run this script from "x64 Native Tools Command Prompt" or install the VS C++ workload.'
}

$src = Join-Path $root 'src\dock_main.cpp'
$outDir = Join-Path $root 'bin'
$null = New-Item -ItemType Directory -Force -Path $outDir
$exe = Join-Path $outDir "DesktopDock-$Architecture.exe"

$defs = '/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX'
if ($Configuration -eq 'Release') {
    $optim = '/O2 /MT'
} else {
    $optim = '/Od /MTd /Zi'
}

# gdiplus: rendering; dwmapi/uuid/version/shell32/ole32: tray apps, shortcuts, icons
$clLine = "cl /nologo /std:c++17 /EHsc $optim /W4 /permissive- /utf-8 $defs /Fe:`"$exe`" `"$src`" /link /SUBSYSTEM:WINDOWS gdiplus.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib uuid.lib version.lib dwmapi.lib uiautomationcore.lib oleaut32.lib"

$bat = Join-Path $env:TEMP 'desktopdock_build.bat'
$lines = @('@echo off', 'setlocal')
if ($vsDevCmd) {
    $lines += "call `"$vsDevCmd`" -no_logo -arch=$Architecture"
    $lines += 'if errorlevel 1 exit /b 1'
}
$lines += "cd /d `"$outDir`""
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
Write-Host "Build OK: $exe" -ForegroundColor Green
