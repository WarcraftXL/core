# SPDX-License-Identifier: GPL-3.0-or-later
#requires -Version 5.1
<#
.SYNOPSIS
    Builds WarcraftXL.dll and wxl-patcher.exe (32-bit) with Visual Studio.

.PARAMETER Config
    Build configuration. Default: Release.

.PARAMETER ClientPath
    Client folder the DLL and the patcher are copied into after each build; kept in the CMake cache.

.PARAMETER SdkPath
    wxl-sdk working copy. Default: ..\wxl-sdk when it exists, else the installed package (CMAKE_PREFIX_PATH).

.PARAMETER Clean
    Deletes the build folder first.

.PARAMETER Test
    Runs ctest after the build.

.PARAMETER AutoPatch
    Runs wxl-patcher on the client's Wow.exe (it skips a patched one and backs up the original to Wow.exe.orig).

.EXAMPLE
    .\build.ps1 -ClientPath "D:\Games\WoW 3.3.5a" -Test
#>
param(
    [string]$Config = "Release",
    [string]$ClientPath,
    [string]$SdkPath,
    [switch]$Clean,
    [switch]$Test,
    [switch]$AutoPatch
)

$ErrorActionPreference = "Stop"

$root     = $PSScriptRoot
$buildDir = Join-Path $root "build\dll"

if (-not (Test-Path (Join-Path $root "CMakeLists.txt"))) {
    throw "CMakeLists.txt is missing from '$root'. Place build.ps1 in the project root directory (next to CMakeLists.txt)."
}

function Get-CachedClientPath([string]$cacheDir) {
    $cache = Join-Path $cacheDir "CMakeCache.txt"
    if (Test-Path $cache) {
        $hit = Select-String -Path $cache -Pattern '^CLIENT_PATH:PATH=(.+)$' | Select-Object -First 1
        if ($hit) { return $hit.Matches[0].Groups[1].Value.Trim() }
    }
    return $null
}

if (-not $ClientPath) {
    $ClientPath = Get-CachedClientPath $buildDir
}
if ($ClientPath -and -not (Test-Path $ClientPath)) {
    throw "Client path not found: $ClientPath"
}

if (-not $SdkPath) {
    $sibling = Join-Path (Split-Path $root -Parent) "wxl-sdk"
    if (Test-Path (Join-Path $sibling "CMakeLists.txt")) { $SdkPath = $sibling }
}

Write-Host "Project : $root"
Write-Host "Client : $ClientPath"
Write-Host "Config : $Config"
Write-Host ""

function Invoke-Native([string]$exe, [string[]]$cmdArgs) {
    Write-Host ">> $exe $($cmdArgs -join ' ')" -ForegroundColor Cyan
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $exe @cmdArgs } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "Failure ($LASTEXITCODE): $exe $($cmdArgs -join ' ')" }
}

function Build-Dll {
    Write-Host "=== WarcraftXL.dll, wxl-patcher.exe (32-bit) ===" -ForegroundColor Green

    if ($Clean -and (Test-Path $buildDir)) {
        Write-Host "Clean $buildDir" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $buildDir
    }

    $needConfigure = $Clean `
        -or $PSBoundParameters.ContainsKey('ClientPath') `
        -or $PSBoundParameters.ContainsKey('SdkPath') `
        -or (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt")))
    if ($needConfigure) {
        $configureArgs = @("-S", $root, "-B", $buildDir, "-A", "Win32")
        if ($ClientPath) { $configureArgs += "-DCLIENT_PATH=$ClientPath" }
        if ($SdkPath) { $configureArgs += "-DWXL_SDK_DIR=$SdkPath" }
        Invoke-Native "cmake" $configureArgs
    }

    Invoke-Native "cmake" @("--build", $buildDir, "--config", $Config, "--parallel")
    Write-Host ""
}

function Invoke-AutoPatch {
    if (-not $ClientPath) { throw "-AutoPatch needs the client folder (-ClientPath)." }
    $patcher = Join-Path $buildDir "$Config\wxl-patcher.exe"
    $wow = Join-Path $ClientPath "Wow.exe"
    if (-not (Test-Path $wow)) { throw "Wow.exe not found: $wow" }

    Write-Host "=== AutoPatch ===" -ForegroundColor Green
    Invoke-Native $patcher @($wow)
    Write-Host ""
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()

Build-Dll
if ($Test) { Invoke-Native "ctest" @("--test-dir", $buildDir, "-C", $Config, "--output-on-failure") }
if ($AutoPatch) { Invoke-AutoPatch }

$sw.Stop()
Write-Host "OK in $([int]$sw.Elapsed.TotalSeconds)s" -ForegroundColor Green
