<#
Builds fly-by-wire with Microsoft's C++ compiler. Needs Visual Studio or the
free "Build Tools for Visual Studio" (2019 or newer) with "Desktop development
with C++". No other tools or libraries.

  build.ps1                 tests, then the program and the self test
  build.ps1 -Only tests     just the rule tests (also runs them)
  build.ps1 -Only app       just fly-by-wire.exe
  build.ps1 -Only selftest  just the tools: selftest.exe, inject.exe (scripted mouse), replay.exe
  build.ps1 -SrcDir <dir>   take the sources from another folder (used by the
                            mutation check, which builds deliberately broken copies)
  build.ps1 -VsPath <dir>   use this Visual Studio installation instead of the newest one
#>
param(
    [ValidateSet('all', 'tests', 'app', 'selftest')]
    [string]$Only = 'all',
    [string]$SrcDir,
    [string]$OutDir,
    [string]$VsPath
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not $SrcDir) { $SrcDir = Join-Path $root 'src' }
if (-not $OutDir) { $OutDir = Join-Path $root 'build' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# The compiler comes with Visual Studio. vswhere, which ships with every
# installation since 2017, says where each one is; the newest with the C++
# tools wins.
function Find-Vcvars {
    $candidates = @()
    if ($VsPath) {
        $candidates = @($VsPath)
    } else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $candidates = @(& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath -sort)
        }
    }
    foreach ($c in $candidates) {
        $bat = Join-Path $c 'VC\Auxiliary\Build\vcvars64.bat'
        if (Test-Path -LiteralPath $bat) { return $bat }
    }
    throw 'No Visual Studio with the C++ tools found. Install "Build Tools for Visual Studio" with "Desktop development with C++", or pass -VsPath.'
}
$vcvars = Find-Vcvars

# /W4 /WX: every warning is an error. /MT: no runtime DLL needed next to the exe.
$cl = 'cl /nologo /std:c++17 /O2 /W4 /WX /EHsc /MT /DUNICODE /D_UNICODE'

function Invoke-Vc([string]$What, [string[]]$Lines) {
    Write-Host "== $What"
    $script = Join-Path $OutDir '_step.cmd'
    # UTF-8 without a byte order mark, and cmd told so, so paths in any language work.
    $body = @('@echo off', 'chcp 65001 >nul', "call `"$vcvars`" >nul 2>&1 || exit /b 1", "cd /d `"$OutDir`" || exit /b 1")
    foreach ($l in $Lines) { $body += "$l || exit /b 1" }
    [IO.File]::WriteAllText($script, ($body -join "`r`n") + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
    & $script
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

if ($Only -in 'all', 'tests') {
    Invoke-Vc 'rule tests: compile' @(
        "$cl /I`"$SrcDir`" /I`"$root\src`" `"$root\tests\filter_tests.cpp`" /Fe:filter_tests.exe /Fo:filter_tests.obj"
    )
    Write-Host '== rule tests: run'
    & (Join-Path $OutDir 'filter_tests.exe') (Join-Path $root 'tests\cases.txt')
    if ($LASTEXITCODE -ne 0) { throw "rule tests failed with exit code $LASTEXITCODE" }
}

if ($Only -in 'all', 'selftest') {
    Invoke-Vc 'self test, scripted mouse and replay: compile' @(
        "$cl /I`"$SrcDir`" `"$SrcDir\selftest.cpp`" /Fe:selftest.exe /Fo:selftest.obj /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib winmm.lib",
        "$cl /I`"$SrcDir`" `"$SrcDir\inject.cpp`" /Fe:inject.exe /Fo:inject.obj /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib winmm.lib",
        "$cl /I`"$SrcDir`" `"$SrcDir\replay.cpp`" /Fe:replay.exe /Fo:replay.obj /link /SUBSYSTEM:CONSOLE"
    )
}

if ($Only -in 'all', 'app') {
    Invoke-Vc 'program: compile' @(
        "rc /nologo /fo fly-by-wire.res `"$SrcDir\fly-by-wire.rc`"",
        "$cl /I`"$SrcDir`" `"$SrcDir\main.cpp`" fly-by-wire.res /Fe:fly-by-wire.exe /Fo:main.obj /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib ws2_32.lib gdi32.lib advapi32.lib cfgmgr32.lib"
    )
}

Write-Host 'build ok'
