#Requires -Version 5.1
<#
.SYNOPSIS
    Pre-commit verification for agent-core: format + build + tests.
.DESCRIPTION
    1. scripts/check-format.ps1 (must exit 0)
    2. cmake --preset vs2022 (when build dir is missing)
    3. cmake --build --preset debug
    4. ctest --preset debug
    Exits non-zero on the first failure.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Find-Tool {
    param(
        [string]$Name,
        [string[]]$Candidates
    )
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }
    foreach ($p in $Candidates) {
        if (Test-Path -LiteralPath $p) {
            return $p
        }
    }
    throw ("{0} not found. Install CMake or enable the VS C++ CMake tools." -f $Name)
}

$vsCMakeBin = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$cmake = Find-Tool -Name "cmake" -Candidates @(
    (Join-Path $vsCMakeBin "cmake.exe"),
    "C:\Program Files\CMake\bin\cmake.exe"
)
$ctest = Find-Tool -Name "ctest" -Candidates @(
    (Join-Path $vsCMakeBin "ctest.exe"),
    "C:\Program Files\CMake\bin\ctest.exe"
)

Push-Location $repoRoot
try {
    Write-Host "[1/3] Format check ..." -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot "check-format.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "Format check failed."
    }

    $cacheFile = Join-Path $repoRoot "build\vs2022\CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cacheFile)) {
        Write-Host "[2/3] Configure CMake ..." -ForegroundColor Cyan
        & $cmake --preset vs2022
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed."
        }
    }
    else {
        Write-Host "[2/3] Build directory exists; skip configure." -ForegroundColor Cyan
    }

    Write-Host "[3/3] Build and test ..." -ForegroundColor Cyan
    & $cmake --build --preset debug
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed."
    }
    & $ctest --preset debug
    if ($LASTEXITCODE -ne 0) {
        throw "Tests failed."
    }

    Write-Host "All checks passed: format + build + tests." -ForegroundColor Green
}
finally {
    Pop-Location
}
