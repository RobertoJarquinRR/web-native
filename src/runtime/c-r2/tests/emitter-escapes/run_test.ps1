# src\runtime\c-r2\tests\emitter-escapes\run_test.ps1
#
# Regression for the emitter's C string escaping: non-ASCII text followed by a
# hex digit must not become an out-of-range escape (`\xd72160`). Emits
# ui_data.h, compiles a C program that prints the UTF-8 strings and checks the
# bytes.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $here

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))))
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$py = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'
$emitter = Join-Path $repo 'src\emitter\python\emit_ui.py'

function Stop-Fail([string]$message) {
    Write-Host "ESCAPE TEST FAILED: $message" -ForegroundColor Red
    throw $message
}

function Find-VcVars {
    $fixed = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $fixed) { return $fixed }
    return $null
}

try {
    Write-Host 'escapes: emit_ui.py --ui-json' -ForegroundColor Cyan
    & $py $emitter --ui-json (Join-Path $here 'ui.json') `
        --out (Join-Path $out 'ui_data.h') --meta (Join-Path $out 'ui_meta.json') | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'emit_ui.py failed' }

    $vcvars = Find-VcVars
    if (-not $vcvars) { Stop-Fail 'vcvars64.bat not found' }
    Write-Host 'escapes: compile check.c against the generated header' -ForegroundColor Cyan
    $step = Join-Path $out 'escape_step.cmd'
    $command = "call `"$vcvars`" >nul && cd /d `"$out`" && " +
        "cl /nologo /O2 /W3 /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS /I`"$out`" `"$(Join-Path $here 'check.c')`" " +
        "/Fe:`"$out\check.exe`" && check.exe `"$out\strings.txt`""
    Set-Content -LiteralPath $step -Encoding ascii -Value "@echo off`r`n$command`r`nexit /b %errorlevel%`r`n"
    & cmd.exe /d /c $step | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail "check.c failed to compile/run ($LASTEXITCODE)" }
    Remove-Item -LiteralPath $step -Force -ErrorAction SilentlyContinue

    Write-Host 'escapes: verify bytes' -ForegroundColor Cyan
    & $py (Join-Path $here 'verify_escapes.py') `
        --header (Join-Path $out 'ui_data.h') --strings (Join-Path $out 'strings.txt') | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'verify_escapes.py failed' }

    Write-Host ''
    Write-Host '========== emitter-escapes: PASS ==========' -ForegroundColor Green
}
catch {
    Write-Host ''
    Write-Host "ESCAPE TEST FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    $step = Join-Path $out 'escape_step.cmd'
    if (Test-Path -LiteralPath $step) { Remove-Item -LiteralPath $step -Force -ErrorAction SilentlyContinue }
    Pop-Location
}
