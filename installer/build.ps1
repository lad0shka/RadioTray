<#
.SYNOPSIS
Builds RadioTray.exe (Release, x64) and the Windows installer.

.REQUIREMENTS
Visual Studio 2022 C++ build tools (with the Windows SDK), CMake and Inno Setup 6.
Inno Setup: https://jrsoftware.org/isinfo.php

.OUTPUT
installer\Output\RadioTray-Setup-<version>.exe
#>
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'

cmake -S (Join-Path $root 'source') -B $build -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

$iscc = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) was not found.' }

$exe = Join-Path $build 'Release\RadioTray.exe'
& $iscc "/DSourceExe=$exe" (Join-Path $PSScriptRoot 'RadioTray.iss')
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed.' }
Write-Host "Installer written to $(Join-Path $PSScriptRoot 'Output')"
