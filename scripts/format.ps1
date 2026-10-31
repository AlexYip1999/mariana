#Requires -Version 5.1
<#
.SYNOPSIS
    Format agent-core C++ sources with clang-format.
.DESCRIPTION
    - Formats .h / .hpp / .cpp under source/ and tests/
    - Skips build/, third_party/, .git/
    - clang-format lookup: $env:CLANG_FORMAT -> PATH -> Visual Studio bundled LLVM
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Find-ClangFormat {
    if ($env:CLANG_FORMAT -and (Test-Path -LiteralPath $env:CLANG_FORMAT)) {
        return $env:CLANG_FORMAT
    }
    $cmd = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\bin\clang-format.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang-format.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\x64\bin\clang-format.exe",
        "C:\Program Files\LLVM\bin\clang-format.exe"
    )
    foreach ($p in $candidates) {
        if (Test-Path -LiteralPath $p) {
            return $p
        }
    }
    throw "clang-format not found. Install LLVM / VS C++ Clang tools, or set CLANG_FORMAT."
}

function Get-CppFiles {
    param([string]$Root)
    Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Extension -in @(".h", ".hpp", ".cpp") -and
            $_.FullName -notmatch [regex]::Escape("\build\") -and
            $_.FullName -notmatch [regex]::Escape("\third_party\") -and
            $_.FullName -notmatch [regex]::Escape("\.git\")
        }
}

$clangFormat = Find-ClangFormat
$files = @(Get-CppFiles -Root $repoRoot)

if ($files.Count -eq 0) {
    Write-Host "No C++ files to format." -ForegroundColor Yellow
    exit 0
}

foreach ($f in $files) {
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $clangFormat -i --style=file $f.FullName
    $clangExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap

    if ($clangExit -ne 0) {
        throw ("clang-format failed: " + $f.FullName)
    }
}

Write-Host ("Formatted {0} file(s) with {1}" -f $files.Count, (& $clangFormat --version)) -ForegroundColor Green
