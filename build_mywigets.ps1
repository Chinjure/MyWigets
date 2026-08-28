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
if (-not $vsDevCmd -and -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'MSVC not found. Run this script from "x64 Native Tools Command Prompt" or install the VS C++ workload.'
}

$srcDir = Join-Path $root 'src'
$outDir = Join-Path $root 'bin'
$null = New-Item -ItemType Directory -Force -Path $outDir
$exe = Join-Path $outDir "MyWigets-$Architecture.exe"
$res = Join-Path $outDir "mywigets-$Architecture.res"

# Single-process host: all widget sources are linked into MyWigets.exe.
# No standalone widget exe is produced anymore.
$sources = @(
    'mywigets_main.cpp',
    'main.cpp',          # clock
    'calendar_main.cpp', # calendar
    'launcher_main.cpp', # launcher
    'topbar_main.cpp',   # topbar
    'dock_main.cpp'      # dock
) | ForEach-Object { (Join-Path $srcDir $_) }

$defs = '/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX'
if ($Configuration -eq 'Release') {
    $optim = '/O2 /MT'
} else {
    $optim = '/Od /MTd /Zi'
}

$srcNames = $sources | ForEach-Object { '"' + $_ + '"' }
$srcList = $srcNames -join ' '

$libs = 'gdiplus.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib ws2_32.lib uuid.lib version.lib propsys.lib dwmapi.lib uiautomationcore.lib comctl32.lib Mmdevapi.lib'

$clLine = "cl /nologo /std:c++17 /EHsc $optim /W4 /permissive- /utf-8 $defs /Fe:`"$exe`" $srcList `"$res`" /link /SUBSYSTEM:WINDOWS $libs"

$bat = Join-Path $env:TEMP 'mywigets_build.bat'
$lines = @('@echo off', 'setlocal')
if ($vsDevCmd) {
    $lines += "call `"$vsDevCmd`" -no_logo -arch=$Architecture"
    $lines += 'if errorlevel 1 exit /b 1'
}
$lines += "cd /d `"$srcDir`""
$lines += "rc /nologo /fo `"$res`" app.rc"
$lines += 'if errorlevel 1 exit /b 1'
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
