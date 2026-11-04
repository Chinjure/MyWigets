param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string[]]$Sources,
    [string[]]$Includes = @(),
    [string]$Defines = '',
    [string]$Libs = 'pdh.lib advapi32.lib user32.lib shell32.lib ole32.lib',
    [ValidateSet('WINDOWS', 'CONSOLE')][string]$Subsystem = 'WINDOWS'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

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
if (-not $vsDevCmd) { throw 'MSVC not found' }

$outDir = Split-Path -Parent $Out
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

$srcList = ($Sources | ForEach-Object { '"' + (Resolve-Path $_).Path + '"' }) -join ' '
$incList = ($Includes | ForEach-Object { '/I"' + (Resolve-Path $_).Path + '"' }) -join ' '
$defList = if ($Defines) { $Defines } else { '' }

$bat = Join-Path $env:TEMP ("bench_build_" + [Guid]::NewGuid().ToString('N') + '.bat')
$lines = @('@echo off', 'setlocal')
$lines += "call `"$vsDevCmd`" -no_logo -arch=x64"
$lines += 'if errorlevel 1 exit /b 1'
$lines += "cd /d `"$root`""
$lines += "cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /permissive- /utf-8 " +
          "/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX $defList " +
          "$incList /Fe:`"$Out`" $srcList /link /SUBSYSTEM:$Subsystem $Libs"
$lines += 'if errorlevel 1 exit /b 1'
$lines += 'endlocal'
Set-Content -Path $bat -Value $lines -Encoding Ascii
& cmd.exe /d /c $bat
$code = $LASTEXITCODE
Remove-Item -Path $bat -Force -ErrorAction SilentlyContinue
if ($code -ne 0) { throw "build failed: $Out (exit $code)" }
Write-Host "built $Out" -ForegroundColor Green
