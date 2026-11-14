<#
Builds fly-by-wire with the MSVC toolset that is already installed. No other
tools or libraries are needed.

  build.ps1                 tests, then the program and the self test
  build.ps1 -Only tests     just the rule tests (also runs them)
  build.ps1 -Only app       just fly-by-wire.exe
  build.ps1 -Only selftest  just the tools: selftest.exe, inject.exe (scripted mouse), replay.exe
  build.ps1 -SrcDir <dir>   take the sources from another folder (used by the
                            mutation check, which builds deliberately broken copies)
#>
param(
    [ValidateSet('all', 'tests', 'app', 'selftest')]
    [string]$Only = 'all',
    [string]$SrcDir,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not $SrcDir) { $SrcDir = Join-Path $root 'src' }
if (-not $OutDir) { $OutDir = Join-Path $root 'build' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# /W4 /WX: every warning is an error. /MT: no runtime DLL needed next to the exe.
$cl = 'cl /nologo /std:c++17 /O2 /W4 /WX /EHsc /MT /DUNICODE /D_UNICODE'

function Invoke-Vc([string]$What, [string[]]$Lines) {
    Write-Host "== $What"
    $script = Join-Path $OutDir '_step.cmd'
    $body = @('@echo off', "call `"$vcvars`" >nul 2>&1 || exit /b 1", "cd /d `"$OutDir`" || exit /b 1")
    foreach ($l in $Lines) { $body += "$l || exit /b 1" }
    Set-Content -LiteralPath $script -Value $body -Encoding ascii
    & $script
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

if ($Only -in 'all', 'tests') {
    Invoke-Vc 'rule tests: compile' @(
        "$cl /I`"$SrcDir`" `"$root\tests\filter_tests.cpp`" /Fe:filter_tests.exe /Fo:filter_tests.obj"
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
        "$cl /I`"$SrcDir`" `"$SrcDir\main.cpp`" fly-by-wire.res /Fe:fly-by-wire.exe /Fo:main.obj /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib ws2_32.lib gdi32.lib advapi32.lib hid.lib"
    )
}

Write-Host 'build ok'
