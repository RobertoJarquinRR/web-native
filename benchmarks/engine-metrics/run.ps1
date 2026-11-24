# benchmarks\engine-metrics\run.ps1 - engine metrics harness entry point.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks\engine-metrics\run.ps1
#
# Steps:
#   1. local venv + psutil (only for RSS cross-check; the harness also uses Win32)
#   2. freeze the current R2 build (exe + wgpu_native.dll + atlas) into snapshot/
#      and record commit + SHA-256. If the build is missing, build R2 once from
#      the committed sources (writes only generated src\runtime\c-r2\build files).
#   3. build R1 once with src\build.ps1 into work\r1-build (throw-away)
#   4. build a ~100-node R2 variant into work\layout100 (layout scaling)
#   5. measure 10 runs per metric and write baseline.json + BASELINE.md
#
# Nothing outside benchmarks\engine-metrics\ is modified, except generated
# artifacts under src\runtime\c-r2\build only when the snapshot is missing.
[CmdletBinding()]
param(
    [int]$Runs = 10,
    [switch]$ForceSnapshot,
    [switch]$RebuildR1,
    [switch]$SkipR1,
    [switch]$SkipLayout100,
    [switch]$NoWarmup
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root
$repo = (Resolve-Path (Join-Path $root '..\..')).Path
$wgpu = Join-Path $repo 'third_party\wgpu-native'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$canonPy = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'
$frontRust = Join-Path $repo 'src\frontend\rust\target\release\frontend-rust.exe'
$emitUi = Join-Path $repo 'src\emitter\python\emit_ui.py'
$r2Source = Join-Path $repo 'src\runtime\c-r2\runtime_r2.c'
$r1Fixture = Join-Path $repo 'benchmarks\frontend-parity\fixtures\flex-page\index.html'
$r1Dir = Join-Path $root 'work\r1-build'
$l100Dir = Join-Path $root 'work\layout100'
$l100Exe = Join-Path $l100Dir 'runtime_r2_layout100.exe'
$venvPy = Join-Path $root '.venv\Scripts\python.exe'
$snapDir = Join-Path $root 'snapshot'
$snapExe = Join-Path $snapDir 'runtime_r2.exe'
$snapDll = Join-Path $snapDir 'wgpu_native.dll'

function Fail([string]$message) {
    Write-Host "engine-metrics: $message" -ForegroundColor Red
    exit 1
}

function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLower()
    }
    finally { $sha.Dispose() }
}

function Get-FileLength([string]$path) {
    return (Get-Item -LiteralPath $path).Length
}

function Write-Utf8([string]$path, [string]$text) {
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $text, $encoding)
}

if (-not (Test-Path -LiteralPath $vcvars)) {
    Fail "vcvars64.bat not found: $vcvars (MSVC C++ workload required for R1/layout100 builds)"
}

# ------------------------------------------------------------------- 1 venv
if (-not (Test-Path -LiteralPath $venvPy)) {
    Write-Host 'venv: creating benchmarks\engine-metrics\.venv' -ForegroundColor Cyan
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) {
        & py -3 -m venv (Join-Path $root '.venv')
    }
    else {
        $python = Get-Command python -ErrorAction SilentlyContinue
        if (-not $python) { Fail 'Python 3 not found (install Python or the py launcher)' }
        & python -m venv (Join-Path $root '.venv')
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $venvPy)) { Fail 'venv creation failed' }
}
& $venvPy -c "import psutil" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host 'venv: installing psutil' -ForegroundColor Cyan
    & $venvPy -m pip install --disable-pip-version-check --no-input psutil
    if ($LASTEXITCODE -ne 0) { Fail 'psutil install failed (no network?)' }
}

# --------------------------------------------------------------- 2 snapshot
$needSnapshot = $ForceSnapshot -or -not (Test-Path -LiteralPath $snapExe) -or `
    -not (Test-Path -LiteralPath $snapDll)
$needMeta = $needSnapshot -or -not (Test-Path -LiteralPath (Join-Path $snapDir 'snapshot.json'))
if ($needSnapshot) {
    $srcBuild = Join-Path $repo 'src\runtime\c-r2\build'
    if (-not (Test-Path -LiteralPath (Join-Path $srcBuild 'runtime_r2.exe'))) {
        Write-Host 'snapshot: R2 build missing; building from committed sources' -ForegroundColor Yellow
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'src\runtime\c-r2\build.ps1') -SkipParityCheck
        if ($LASTEXITCODE -ne 0) { Fail 'R2 build failed and no snapshot exists' }
    }
    Write-Host 'snapshot: freezing src\runtime\c-r2\build -> snapshot\' -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path $snapDir, (Join-Path $snapDir 'assets') | Out-Null
    Copy-Item -LiteralPath (Join-Path $srcBuild 'runtime_r2.exe') -Destination $snapExe -Force
    Copy-Item -LiteralPath (Join-Path $srcBuild 'wgpu_native.dll') -Destination $snapDll -Force
    foreach ($asset in @('atlas.bin', 'atlas.json')) {
        $from = Join-Path $srcBuild $asset
        if (Test-Path -LiteralPath $from) {
            Copy-Item -LiteralPath $from -Destination (Join-Path $snapDir "assets\$asset") -Force
        }
    }
    $buildReport = Join-Path $srcBuild 'build_report.json'
    if (Test-Path -LiteralPath $buildReport) {
        Copy-Item -LiteralPath $buildReport -Destination (Join-Path $snapDir 'source_build_report.json') -Force
    }
}
if ($needMeta) {
    $commit = ''
    try { $commit = (& git -C $repo rev-parse --short HEAD 2>$null) } catch { }
    $meta = [ordered]@{
        captured_at_utc = (Get-Date).ToUniversalTime().ToString('o')
        commit          = "$commit".Trim()
        source          = 'src/runtime/c-r2/build'
        exe_bytes       = Get-FileLength $snapExe
        exe_sha256      = Get-Sha256 $snapExe
        dll_bytes       = Get-FileLength $snapDll
        dll_sha256      = Get-Sha256 $snapDll
        atlas_bytes     = if (Test-Path -LiteralPath (Join-Path $snapDir 'assets\atlas.bin')) {
            Get-FileLength (Join-Path $snapDir 'assets\atlas.bin')
        } else { $null }
        atlas_sha256    = if (Test-Path -LiteralPath (Join-Path $snapDir 'assets\atlas.bin')) {
            Get-Sha256 (Join-Path $snapDir 'assets\atlas.bin')
        } else { $null }
    }
    Write-Utf8 (Join-Path $snapDir 'snapshot.json') ($meta | ConvertTo-Json)
}
else {
    Write-Host 'snapshot: using existing frozen R2 binaries (use -ForceSnapshot to refresh)' -ForegroundColor DarkGray
}

# ------------------------------------------------------------------- 3 R1 build
# R1 is archived (archive/r1-static). When the script is gone, the R1 engine-size
# and startup rows are simply not produced; R2 metrics are unaffected.
if (-not $SkipR1) {
    $r1Script = Join-Path $repo 'src\build.ps1'
    if (-not (Test-Path -LiteralPath $r1Script)) {
        Write-Host 'R1: archived (archive/r1-static) - skipping R1 metrics' -ForegroundColor Yellow
        $SkipR1 = $true
    }
    elseif ($RebuildR1 -or -not (Test-Path -LiteralPath (Join-Path $r1Dir 'webnative-rust.exe'))) {
        Write-Host 'R1: building with src\build.ps1 -> work\r1-build (one time)' -ForegroundColor Cyan
        & powershell -NoProfile -ExecutionPolicy Bypass -File $r1Script `
            -Html $r1Fixture -OutDir $r1Dir -Width 480 -Height 320 | Write-Host
        if ($LASTEXITCODE -ne 0) { Fail 'R1 build failed (src\build.ps1)' }
    }
    else {
        Write-Host 'R1: reusing work\r1-build (use -RebuildR1 to rebuild)' -ForegroundColor DarkGray
    }
}

# ------------------------------------------------------ 4 layout ~100 nodes
if (-not $SkipLayout100) {
    $needL100 = -not (Test-Path -LiteralPath $l100Exe)
    if ($needL100) {
        Write-Host 'layout100: building ~100-node R2 variant -> work\layout100' -ForegroundColor Cyan
        $ok = $true
        foreach ($required in @($frontRust, $canonPy, $emitUi, $r2Source,
                               (Join-Path $wgpu 'include\wgpu.h'),
                               (Join-Path $wgpu 'lib\wgpu_native.dll.lib'),
                               (Join-Path $wgpu 'bin\wgpu_native.dll'))) {
            if (-not (Test-Path -LiteralPath $required)) {
                Write-Host "layout100: missing $required - skipping (R2 28-node layout still measured)" -ForegroundColor Yellow
                $ok = $false
            }
        }
        if ($ok) {
            New-Item -ItemType Directory -Force -Path $l100Dir | Out-Null
            & $frontRust --html (Join-Path $root 'fixtures\layout-100\index.html') `
                --css (Join-Path $root 'fixtures\layout-100\style.css') `
                --width 960 --height 640 --emit-ui (Join-Path $l100Dir 'ui.json')
            if ($LASTEXITCODE -ne 0) { Fail 'frontend-rust --emit-ui failed for layout100' }
            & $canonPy $emitUi --ui-json (Join-Path $l100Dir 'ui.json') `
                --out (Join-Path $l100Dir 'ui_data.h') --meta (Join-Path $l100Dir 'ui_meta.json')
            if ($LASTEXITCODE -ne 0) { Fail 'emit_ui.py failed for layout100' }
            $buildCmd = @"
@echo off
call "$vcvars" >nul
cd /d "$l100Dir"
cl /nologo /O2 /W3 /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS "$r2Source" /I"$wgpu\include" /I"$l100Dir" /Fo:"$l100Dir\runtime_r2_layout100.obj" /Fe:"$l100Exe" /link "$wgpu\lib\wgpu_native.dll.lib" windowscodecs.lib ole32.lib user32.lib gdi32.lib
exit /b %errorlevel%
"@
            $buildFile = Join-Path $l100Dir 'build_layout100.cmd'
            Write-Utf8 $buildFile $buildCmd
            & cmd.exe /d /c $buildFile | Write-Host
            if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $l100Exe)) {
                Write-Host 'layout100: compile failed - continuing without the 100-node row' -ForegroundColor Yellow
            }
            else {
                Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') `
                    -Destination (Join-Path $l100Dir 'wgpu_native.dll') -Force
            }
        }
    }
    else {
        Write-Host 'layout100: reusing work\layout100\runtime_r2_layout100.exe' -ForegroundColor DarkGray
    }
}

# ------------------------------------------------------------------ 5 measure
$measureArgs = @(
    (Join-Path $root 'measure.py'),
    '--runs', "$Runs",
    '--out', (Join-Path $root 'baseline.json'),
    '--md', (Join-Path $root 'BASELINE.md'),
    '--r1-dir', $r1Dir
)
if ($SkipR1) { $measureArgs += '--skip-r1' }
if ($NoWarmup) { $measureArgs += '--no-warmup' }
if ((-not $SkipLayout100) -and (Test-Path -LiteralPath $l100Exe)) {
    $measureArgs += @('--layout100-exe', $l100Exe)
}
Write-Host ''
Write-Host 'measure: running harness' -ForegroundColor Cyan
& $venvPy @measureArgs
if ($LASTEXITCODE -ne 0) { Fail "measure.py exited with $LASTEXITCODE" }

Write-Host ''
Write-Host 'engine-metrics: baseline written' -ForegroundColor Green
Write-Host "  json: $(Join-Path $root 'baseline.json')"
Write-Host "  md  : $(Join-Path $root 'BASELINE.md')"
Pop-Location
exit 0
