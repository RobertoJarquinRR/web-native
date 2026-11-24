# tests\offscreen\run_offscreen.ps1 - vtable offscreen (WN_SURFACE_OFFSCREEN).
#
# Builds a window-less test that creates a wn_view on a texture, renders one
# frame and writes it with the backend readback, then compares both backends:
#   d3d11 (hardware) vs wgpu (WARP on this machine)
# A blank frame or a missing PNG is a failure.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\offscreen\run_offscreen.ps1

[CmdletBinding()]
param(
    [int]$Width = 960,
    [int]$Height = 640
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)   # c-r2
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $root))
$out = Join-Path $here 'out'
$obj = Join-Path $out 'obj'
$logPath = Join-Path $out 'run.log'
$uiData = Join-Path $root 'build\ui_data.h'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$wgpu = Join-Path $repo 'third_party\wgpu-native'
$py = Join-Path $repo 'benchmarks\visual-parity\.venv\Scripts\python.exe'
$verify = Join-Path $repo 'src\runtime\c-r2\tests\d3d11\verify_d3d11.py'

New-Item -ItemType Directory -Force -Path $out, $obj | Out-Null
Set-Content -LiteralPath $logPath -Value "offscreen vtable test`r`n" -Encoding utf8

function Stop-Fail([string]$message) {
    Add-Content -LiteralPath $logPath -Value "FAIL: $message"
    Write-Host "OFFSCREEN FAILED: $message" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path -LiteralPath $vcvars)) { Stop-Fail "vcvars64.bat not found: $vcvars" }
if (-not (Test-Path -LiteralPath $uiData)) {
    Stop-Fail "missing $uiData; run src\runtime\c-r2\build.ps1 -SkipRun first"
}
if (-not (Test-Path -LiteralPath $py)) { Stop-Fail "harness python not found: $py" }

$sources = @(
    (Join-Path $root 'wn_common.c'),
    (Join-Path $root 'wn_core.c'),
    (Join-Path $root 'renderer_wgpu.c'),
    (Join-Path $root 'renderer_d3d11.c'),
    (Join-Path $root 'host_win32.c'),
    (Join-Path $root 'wn_view.c'),
    (Join-Path $root 'wn_app.c'),
    (Join-Path $here 'offscreen_test.c')
)
foreach ($source in $sources) {
    if (-not (Test-Path -LiteralPath $source)) { Stop-Fail "missing source: $source" }
}

$include = Join-Path $repo 'include'
$cmd = @"
@echo off
call "$vcvars" >nul
cd /d "$obj"
cl /nologo /O2 /W4 /WX /std:c17 /TC /DWN_STATIC /D_CRT_SECURE_NO_WARNINGS $($sources -join ' ') /I"$include" /I"$root" /I"$wgpu\include" /I"$root\build" /Fe:"$out\offscreen_test.exe" /link "$wgpu\lib\wgpu_native.dll.lib" d3d11.lib dxgi.lib d3dcompiler.lib windowscodecs.lib ole32.lib user32.lib gdi32.lib
exit /b %errorlevel%
"@
$cmdFile = Join-Path $obj 'build.cmd'
Set-Content -LiteralPath $cmdFile -Value $cmd -Encoding ascii

Write-Host "compile: offscreen_test.exe (/W4 /WX)" -ForegroundColor Cyan
& cmd.exe /d /c $cmdFile | Add-Content -LiteralPath $logPath
if ($LASTEXITCODE -ne 0) { Stop-Fail 'cl.exe failed' }
Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') -Destination $out -Force

$d3d11Png = Join-Path $out 'offscreen_d3d11.png'
$wgpuPng = Join-Path $out 'offscreen_wgpu.png'
$compare = Join-Path $out 'offscreen_compare.json'

Write-Host 'run: offscreen d3d11' -ForegroundColor Cyan
$env:WN_RENDERER = 'd3d11'
& (Join-Path $out 'offscreen_test.exe') $Width $Height $d3d11Png 2>&1 | Add-Content -LiteralPath $logPath
$d3d11Exit = $LASTEXITCODE
Write-Host 'run: offscreen wgpu' -ForegroundColor Cyan
$env:WN_RENDERER = 'wgpu'
& (Join-Path $out 'offscreen_test.exe') $Width $Height $wgpuPng 2>&1 | Add-Content -LiteralPath $logPath
$wgpuExit = $LASTEXITCODE
Remove-Item Env:\WN_RENDERER -ErrorAction SilentlyContinue

if ($d3d11Exit -ne 0 -or $wgpuExit -ne 0) {
    Stop-Fail "offscreen render failed (d3d11=$d3d11Exit wgpu=$wgpuExit); see $logPath"
}
foreach ($png in @($d3d11Png, $wgpuPng)) {
    if (-not (Test-Path -LiteralPath $png)) { Stop-Fail "missing PNG: $png" }
}

Write-Host 'compare: d3d11 vs wgpu (same display list)' -ForegroundColor Cyan
& $py $verify --capture $d3d11Png --reference $wgpuPng --report $compare `
    --label 'offscreen-d3d11-vs-wgpu' 2>&1 | Add-Content -LiteralPath $logPath
if ($LASTEXITCODE -ne 0) {
    Get-Content -LiteralPath $logPath -Tail 12 | Write-Host
    Stop-Fail "pixel comparison failed; see $compare"
}

$report = Get-Content -LiteralPath $compare -Raw | ConvertFrom-Json
$checks = $report.checks
Write-Host ("offscreen: OK {0}x{1} max_diff={2} bad={3:P2} exact={4:P2} content={5:P2}" -f `
    $checks.capture_size[0], $checks.capture_size[1], $checks.max_channel_diff,
    $checks.bad_fraction, $checks.exact_fraction, $checks.content_fraction) -ForegroundColor Green
Write-Host "========== offscreen vtable: PASS ==========" -ForegroundColor Green
