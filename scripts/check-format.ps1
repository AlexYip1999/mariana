#Requires -Version 5.1
<#
.SYNOPSIS
    Check agent-core C++ code format.
.DESCRIPTION
    Two checks; any failure exits with code 1:
    1. clang-format --dry-run --Werror against .clang-format
    2. Right brace rule: '}' must start a line (only indentation before it).
       Applies to block braces AND to nested entries of multi-line braced
       initializer lists. Single-line statements ending with ';' are exempt
       (e.g. 'return {};', 'std::vector<int> v = {1, 2};').
       Empty function bodies may stay on one line: 'virtual void OnTurnStart() {}'.
       Constructor initializer lists are followed by '{}' on its own line.
       Rejects "if (...) { ... }", "} else {", and
       '{agent::core::ChatRole::User, "hi"},'.
    Skips build/, third_party/, .git/.
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
    Write-Host "No C++ files to check." -ForegroundColor Yellow
    exit 0
}

$formatFailures = New-Object System.Collections.Generic.List[string]
$braceViolations = New-Object System.Collections.Generic.List[object]

foreach ($f in $files) {
    # Native stderr becomes an ErrorRecord; keep EAP=Continue so a format
    # violation does not abort the script before we print our summary.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = & $clangFormat --dry-run --Werror --style=file $f.FullName 2>&1
    $clangExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap

    if ($clangExit -ne 0) {
        $formatFailures.Add($f.FullName)
        $output | ForEach-Object { Write-Host ([string]$_) -ForegroundColor DarkYellow }
    }

    $lines = Get-Content -LiteralPath $f.FullName -Encoding UTF8
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]

        # Strip line comments before checking, so '}' in comments is ignored.
        $code = $line
        $commentIndex = $code.IndexOf("//")
        if ($commentIndex -ge 0) {
            $code = $code.Substring(0, $commentIndex)
        }
        if ($code -notmatch "}") {
            continue
        }

        # Skip '}' inside a string literal (simple heuristic: odd quote count before it).
        $braceIndex = $code.IndexOf("}")
        $prefix = $code.Substring(0, $braceIndex)
        $quoteCount = ([regex]::Matches($prefix, '"')).Count
        if ($quoteCount % 2 -eq 1) {
            continue
        }

        # Single-line statements (ending with ';') may keep compact braces,
        # e.g. 'return {};', 'std::vector<int> v = {1, 2};'. Multi-line
        # initializer entries ending with ',' must expand their closing brace
        # to its own line.
        if ($code.TrimEnd().EndsWith(";")) {
            continue
        }

        # Empty function bodies may stay on one line: 'void OnTurnStart() {}'.
        # Exclude control-flow blocks so 'if (ok) {}' is still rejected.
        if ($code -match '\)\s*(const\s*)?(override\s*)?\{\s*\}\s*$' -and
            $code -notmatch '^\s*(if|for|while|switch|catch)\s*\(') {
            continue
        }

        # Constructor with initializer list: body is '{}' on its own line.
        if ($code.Trim() -eq "{}") {
            continue
        }

        # A right brace (block or multi-line braced-list entry) must be the
        # first non-whitespace character of the line.
        if ($code -notmatch "^\s*}") {
            $braceViolations.Add([pscustomobject]@{
                    File = $f.FullName
                    Line = $i + 1
                    Text = $line.Trim()
                })
        }
    }
}

if ($formatFailures.Count -gt 0 -or $braceViolations.Count -gt 0) {
    Write-Host ""
    Write-Host "Format check FAILED:" -ForegroundColor Red

    if ($formatFailures.Count -gt 0) {
        Write-Host ("  Not clang-format clean ({0}):" -f $formatFailures.Count) -ForegroundColor Red
        foreach ($p in $formatFailures) {
            Write-Host ("    " + $p)
        }
        Write-Host "  Fix: run scripts/format.ps1, review the diff, then re-run this check." -ForegroundColor Yellow
    }

    if ($braceViolations.Count -gt 0) {
        Write-Host ("  Right brace not on its own line ({0}):" -f $braceViolations.Count) -ForegroundColor Red
        foreach ($v in $braceViolations) {
            Write-Host ("    {0}:{1}  {2}" -f $v.File, $v.Line, $v.Text)
        }
        Write-Host "  Rule: '}' must start a line (indentation allowed) — block braces and multi-line braced-list entries; single-line statements ending with ';', empty function bodies '{}', and constructor-body '{}' are exempt." -ForegroundColor Yellow
    }

    exit 1
}

Write-Host ("Format check PASSED: {0} file(s)." -f $files.Count) -ForegroundColor Green
exit 0
