# build_text.ps1 - text-capable canonical C runtime build + pixel gate.
#
# Builds TWO runtimes from the same sources:
#   1. text:  experiments/16-text-atlas-c/{display_list.json,atlas.bin}
#             -> emit_c_text.py -> scene_data.h
#             -> runtime_gpu_text.c  -> scene_text.exe  -> scene_text.png
#             -> runtime_window.c   -> webnative-text.exe (swapchain + glyphs)
#   2. rect regression:
#             a rect-only IR -> emit_c.py (existing emitter) -> scene_data.h
#             -> runtime_gpu.c       -> render_rect.exe -> scene_rect.png
#             -> runtime_window.c    -> webnative-rect.exe (rect-only header)
#
# Then it verifies:
#   - scene_text.png is pixel-identical (maxdiff 0) to exp16 scene_text.png
#   - 400x240, >=100 bright text pixels, exact chips, exact background
#   - atlas upload FNV-1a CPU == GPU (from runtime stdout)
#   - window exits 0 on --frames N, D3D12 adapter, glyph pipeline survives a
#     programmatic 720x480 resize, atlas proof matches
#   - rect PNG is pixel-identical to an independent Pillow reference
#   - rect-only window builds and exits 0
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build_text.ps1
#   powershell -ExecutionPolicy Bypass -File build_text.ps1 -Frames 20 -ResizeAfter 5
#   powershell -ExecutionPolicy Bypass -File build_text.ps1 -OutDir C:\temp\wn-text
#
# Build/verify only: Python and Pillow are NOT needed by the produced .exe.

[CmdletBinding()]
param(
    [string]$OutDir = "",
    [string]$Exp16Dir = "",
    [int]$Frames = 20,
    [int]$ResizeAfter = 5,
    [switch]$SkipWindow,
    [switch]$SkipRect
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $root '..\..\..')).Path
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$wgpu = Join-Path $repo 'third_party\wgpu-native'
$emitter = Join-Path $repo 'src\emitter\python\emit_c_text.py'
$rectEmitter = Join-Path $repo 'src\emitter\python\emit_c.py'
$verifier = Join-Path $root 'verify_text.py'
$runtimeGpuText = Join-Path $root 'runtime_gpu_text.c'
$runtimeGpu = Join-Path $root 'runtime_gpu.c'
$runtimeWindow = Join-Path $root 'runtime_window.c'
$pythonVenv = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'

if (-not $OutDir) { $OutDir = Join-Path $repo 'build\text-runtime' }
if (-not $Exp16Dir) { $Exp16Dir = Join-Path $repo 'experiments\16-text-atlas-c' }
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$Exp16Dir = [System.IO.Path]::GetFullPath($Exp16Dir)

foreach ($required in @($vcvars, $emitter, $rectEmitter, $verifier,
                       $runtimeGpuText, $runtimeGpu, $runtimeWindow,
                       (Join-Path $wgpu 'include\wgpu.h'),
                       (Join-Path $wgpu 'lib\wgpu_native.dll.lib'),
                       (Join-Path $wgpu 'bin\wgpu_native.dll'),
                       (Join-Path $Exp16Dir 'display_list.json'),
                       (Join-Path $Exp16Dir 'atlas.bin'),
                       (Join-Path $Exp16Dir 'atlas.json'),
                       (Join-Path $Exp16Dir 'glyph_quads.json'),
                       (Join-Path $Exp16Dir 'scene_text.png'))) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "missing required path: $required"
    }
}

if (-not (Test-Path -LiteralPath $pythonVenv)) {
    $pythonVenv = (Get-Command py -ErrorAction SilentlyContinue)
    if (-not $pythonVenv) { throw "Python 3.12 not found (looked for $pythonVenv)" }
    $pythonVenv = $pythonVenv.Source
    $pythonPrefix = @('-3.12')
} else {
    $pythonPrefix = @()
}
& $pythonVenv @pythonPrefix -c "import PIL" 2>$null
if ($LASTEXITCODE -ne 0) {
    throw "Pillow is required for verification: $pythonVenv -m pip install pillow"
}
function Invoke-Python([string]$what, [string[]]$arguments) {
    $all = @()
    $all += $pythonPrefix
    $all += $arguments
    & $pythonVenv @all
    if ($LASTEXITCODE -ne 0) { throw "$what failed with exit code $LASTEXITCODE" }
}

function Write-Utf8([string]$path, [string]$text) {
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $text, $encoding)
}

function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLower()
    }
    finally {
        $sha.Dispose()
    }
}

function Invoke-Process([string]$exe, [string]$arguments, [string]$workDir) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $arguments
    $psi.WorkingDirectory = $workDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    # Acceptance isolation: only System32 and the executable's directory.
    $psi.EnvironmentVariables['PATH'] = "$env:WINDIR\System32;$workDir"
    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Stdout   = $stdout
        Stderr   = $stderr
        PathUsed = $psi.EnvironmentVariables['PATH']
    }
}

function Get-Report([string]$path) {
    return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}

$timings = [ordered]@{}
$report = [ordered]@{
    status = 'RUNNING'
    started = (Get-Date).ToString('o')
    out_dir = $OutDir
    exp16_dir = $Exp16Dir
    timings_ms = $timings
    commands = [ordered]@{}
    artifacts = [ordered]@{}
}
$script:failed = $true
$scripts = @()
try {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $rectDir = Join-Path $OutDir 'rect'
    New-Item -ItemType Directory -Force -Path $rectDir | Out-Null

    # Remove stale outputs so a failed build cannot be mistaken for a pass.
    foreach ($stale in @('scene_text.exe', 'webnative-text.exe', 'scene_text.png',
                         'scene_data.h', 'offscreen_stdout.txt', 'offscreen_stderr.txt',
                         'window_stdout.txt', 'window_stderr.txt', 'verify_text.json',
                         'build_report.json', 'build_timings.json')) {
        Remove-Item -LiteralPath (Join-Path $OutDir $stale) -Force -ErrorAction SilentlyContinue
    }
    foreach ($stale in @('render_rect.exe', 'webnative-rect.exe', 'scene_rect.png',
                         'scene_data.h', 'rect_stdout.txt', 'rect_stderr.txt',
                         'rect_window_stdout.txt', 'rect_window_stderr.txt',
                         'verify_rect.json')) {
        Remove-Item -LiteralPath (Join-Path $rectDir $stale) -Force -ErrorAction SilentlyContinue
    }

    # ---------------------------------------------------------- input copies
    Write-Host 'copy: exp16 display_list.json + atlas.bin + atlas.json + glyph_quads.json' -ForegroundColor Cyan
    foreach ($name in @('display_list.json', 'atlas.bin', 'atlas.json', 'glyph_quads.json')) {
        Copy-Item -LiteralPath (Join-Path $Exp16Dir $name) -Destination $OutDir -Force
    }
    Copy-Item -LiteralPath $runtimeGpuText -Destination $OutDir -Force
    Copy-Item -LiteralPath $runtimeWindow -Destination $OutDir -Force
    Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') -Destination $OutDir -Force

    # --------------------------------------------------------- text emitter
    Write-Host 'emit: display_list.json + atlas.bin -> scene_data.h (rects + glyphs)' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Python 'emit_c_text.py' @(
        $emitter,
        '--ir', (Join-Path $OutDir 'display_list.json'),
        '--atlas', (Join-Path $OutDir 'atlas.bin'),
        '--atlas-json', (Join-Path $OutDir 'atlas.json'),
        '--quads', (Join-Path $OutDir 'glyph_quads.json'),
        '--out', (Join-Path $OutDir 'scene_data.h'))
    $sw.Stop()
    $timings['emit_text'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report.commands['emit_text'] = "$pythonVenv $emitter --ir display_list.json --atlas atlas.bin --atlas-json atlas.json --quads glyph_quads.json --out scene_data.h"

    # ----------------------------------------------------- rect regression IR
    Write-Host 'rect: generate a rect-only IR, emit with the existing emit_c.py' -ForegroundColor Cyan
    $rectIr = @'
{
  "viewport": { "width": 320, "height": 200 },
  "ops": [
    { "type": "rect", "x": 0, "y": 0, "w": 320, "h": 200, "fill": "#202030" },
    { "type": "rect", "x": 20, "y": 20, "w": 60, "h": 40, "fill": "#e74c3c" },
    { "type": "rect", "x": 100, "y": 20, "w": 60, "h": 40, "fill": "#2ecc71" },
    { "type": "rect", "x": 180, "y": 20, "w": 60, "h": 40, "fill": "#3498db" }
  ]
}
'@
    Write-Utf8 (Join-Path $rectDir 'display_list.json') ($rectIr + "`n")
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Python 'emit_c.py (rect-only)' @(
        $rectEmitter,
        '--ir', (Join-Path $rectDir 'display_list.json'),
        '--out', (Join-Path $rectDir 'scene_data.h'))
    $sw.Stop()
    $timings['emit_rect'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    foreach ($name in @('runtime_gpu.c', 'runtime_window.c')) {
        Copy-Item -LiteralPath (Join-Path $root $name) -Destination $rectDir -Force
    }
    Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') -Destination $rectDir -Force

    # -------------------------------------------------------------- compile
    Write-Host 'compile: scene_text.exe + webnative-text.exe + rect regression exes' -ForegroundColor Cyan
    $buildCmd = @"
@echo off
call "$vcvars" >nul
cd /d "$OutDir"
cl /nologo /O2 /W3 /std:c17 /TC runtime_gpu_text.c /I"$wgpu\include" /Fe:scene_text.exe /link "$wgpu\lib\wgpu_native.dll.lib" windowscodecs.lib ole32.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /std:c17 /TC runtime_window.c /I"$wgpu\include" /Fe:webnative-text.exe /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup "$wgpu\lib\wgpu_native.dll.lib" user32.lib gdi32.lib
if errorlevel 1 exit /b 2
cd /d "$rectDir"
cl /nologo /O2 /W3 /std:c17 /TC runtime_gpu.c /I"$wgpu\include" /Fe:render_rect.exe /link "$wgpu\lib\wgpu_native.dll.lib" windowscodecs.lib ole32.lib user32.lib
if errorlevel 1 exit /b 3
cl /nologo /O2 /W3 /std:c17 /TC runtime_window.c /I"$wgpu\include" /Fe:webnative-rect.exe /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup "$wgpu\lib\wgpu_native.dll.lib" user32.lib gdi32.lib
exit /b %errorlevel%
"@
    $buildFile = Join-Path $OutDir 'build_text.cmd'
    Write-Utf8 $buildFile $buildCmd
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & cmd.exe /d /c $buildFile
    $sw.Stop()
    if ($LASTEXITCODE -ne 0) { throw "cl.exe build failed with exit code $LASTEXITCODE" }
    $timings['compile'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report.commands['compile'] = $buildCmd
    foreach ($exe in @('scene_text.exe', 'webnative-text.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $OutDir $exe))) { throw "missing $exe" }
    }
    foreach ($exe in @('render_rect.exe', 'webnative-rect.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $rectDir $exe))) { throw "missing $exe" }
    }

    # ---------------------------------------------------- offscreen text run
    Write-Host 'run: scene_text.exe scene_text.png (isolated PATH)' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $textRun = Invoke-Process (Join-Path $OutDir 'scene_text.exe') 'scene_text.png' $OutDir
    $sw.Stop()
    $timings['run_text'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    Write-Utf8 (Join-Path $OutDir 'offscreen_stdout.txt') $textRun.Stdout
    Write-Utf8 (Join-Path $OutDir 'offscreen_stderr.txt') $textRun.Stderr
    Write-Host $textRun.Stdout
    if ($textRun.Stderr) { Write-Host $textRun.Stderr -ForegroundColor DarkYellow }
    if ($textRun.ExitCode -ne 0) { throw "scene_text.exe exited with $($textRun.ExitCode)" }
    if ($textRun.Stdout -notmatch 'backend=D3D12') { throw "offscreen adapter is not D3D12" }
    if ($textRun.Stdout -notmatch 'atlas: uploaded.*MATCH') { throw "atlas upload proof missing" }
    $report['offscreen'] = [ordered]@{
        exit_code = $textRun.ExitCode
        adapter = (($textRun.Stdout -split "`r?`n") | Where-Object { $_ -match '^adapter: backend=' } | Select-Object -First 1)
        atlas_proof = (($textRun.Stdout -split "`r?`n") | Where-Object { $_ -match '^atlas: uploaded' } | Select-Object -First 1)
        path_used = $textRun.PathUsed
    }

    # ------------------------------------------------------- text PNG verify
    Write-Host 'verify: text PNG vs exp16 scene_text.png (maxdiff 0)' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Python 'verify_text.py --mode text' @(
        $verifier,
        '--mode', 'text',
        '--png', (Join-Path $OutDir 'scene_text.png'),
        '--reference', (Join-Path $Exp16Dir 'scene_text.png'),
        '--ir', (Join-Path $OutDir 'display_list.json'),
        '--header', (Join-Path $OutDir 'scene_data.h'),
        '--stdout', (Join-Path $OutDir 'offscreen_stdout.txt'),
        '--report', (Join-Path $OutDir 'verify_text.json'))
    $sw.Stop()
    $timings['verify_text'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $textVerify = Get-Report (Join-Path $OutDir 'verify_text.json')
    if ($textVerify.failed -ne 0) { throw "text verification failed ($($textVerify.failed) check(s))" }
    $report['text'] = [ordered]@{
        passed = $textVerify.passed
        failed = $textVerify.failed
        parity = $textVerify.metrics.diff
        bright_pixels = $textVerify.metrics.bright_pixels
        glyphs = $textVerify.metrics.glyphs
        checks = $textVerify.checks
    }

    # ------------------------------------------- self-containment (no Python)
    Write-Host 'proof: scene_text.exe with ONLY the exe + wgpu_native.dll present' -ForegroundColor Cyan
    $proofDir = Join-Path $OutDir 'proof_runtime_only'
    Remove-Item -LiteralPath $proofDir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $proofDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $OutDir 'scene_text.exe') -Destination $proofDir -Force
    Copy-Item -LiteralPath (Join-Path $OutDir 'wgpu_native.dll') -Destination $proofDir -Force
    $proofFilesBefore = (Get-ChildItem -LiteralPath $proofDir | Select-Object -ExpandProperty Name) -join ', '
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proofRun = Invoke-Process (Join-Path $proofDir 'scene_text.exe') 'scene_text.png' $proofDir
    $sw.Stop()
    $timings['proof_selfcontained'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    if ($proofRun.ExitCode -ne 0) {
        Write-Host $proofRun.Stdout
        Write-Host $proofRun.Stderr -ForegroundColor DarkYellow
        throw "self-contained scene_text.exe exited with $($proofRun.ExitCode)"
    }
    $rootHash = Get-Sha256 (Join-Path $OutDir 'scene_text.png')
    $proofHash = Get-Sha256 (Join-Path $proofDir 'scene_text.png')
    if ($rootHash -ne $proofHash) { throw "self-contained run produced a different PNG" }
    $report['self_contained'] = [ordered]@{
        dir = $proofDir
        files_before_run = $proofFilesBefore
        files_after_run = ((Get-ChildItem -LiteralPath $proofDir | Select-Object -ExpandProperty Name) -join ', ')
        png_sha256_matches = $true
        exit_code = $proofRun.ExitCode
    }

    # ------------------------------------------------------- window smoke test
    if (-not $SkipWindow) {
        Write-Host "run: webnative-text.exe --frames $Frames --resize-after $ResizeAfter" -ForegroundColor Cyan
        $windowArgs = "--frames $Frames"
        if ($ResizeAfter -gt 0) { $windowArgs += " --resize-after $ResizeAfter" }
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $windowRun = Invoke-Process (Join-Path $OutDir 'webnative-text.exe') $windowArgs $OutDir
        $sw.Stop()
        $timings['run_window'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        Write-Utf8 (Join-Path $OutDir 'window_stdout.txt') $windowRun.Stdout
        Write-Utf8 (Join-Path $OutDir 'window_stderr.txt') $windowRun.Stderr
        Write-Host $windowRun.Stdout
        if ($windowRun.Stderr) { Write-Host $windowRun.Stderr -ForegroundColor DarkYellow }
        if ($windowRun.ExitCode -ne 0) { throw "webnative-text.exe exited with $($windowRun.ExitCode)" }
        if ($windowRun.Stdout -notmatch 'backend=D3D12') { throw "window adapter is not D3D12" }
        if ($windowRun.Stdout -notmatch 'atlas: uploaded.*MATCH') { throw "window atlas proof missing" }
        if ($windowRun.Stdout -notmatch "closed after $Frames frame") { throw "window did not run $Frames frames" }
        if ($windowRun.Stdout -notmatch 'glyph frame') { throw "window did not draw glyphs" }
        if ($ResizeAfter -gt 0) {
            if ($windowRun.Stdout -notmatch 'resized to 720x480') { throw "window resize was not exercised" }
            if ($windowRun.Stdout -notmatch 'glyph pipeline kept') { throw "glyph pipeline did not survive resize" }
        }
        $report['window'] = [ordered]@{
            exit_code = $windowRun.ExitCode
            frames = $Frames
            resize_after = $ResizeAfter
            adapter = (($windowRun.Stdout -split "`r?`n") | Where-Object { $_ -match '^adapter: backend=' } | Select-Object -First 1)
            atlas_proof = (($windowRun.Stdout -split "`r?`n") | Where-Object { $_ -match '^atlas: uploaded' } | Select-Object -First 1)
            summary = (($windowRun.Stdout -split "`r?`n") | Where-Object { $_ -match '^window: closed' } | Select-Object -First 1)
            stdout = $windowRun.Stdout
            stderr = $windowRun.Stderr
        }
    }

    # ------------------------------------------------------ rect regression
    if (-not $SkipRect) {
        Write-Host 'run: render_rect.exe scene_rect.png (existing emitter/runtime)' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $rectRun = Invoke-Process (Join-Path $rectDir 'render_rect.exe') 'scene_rect.png' $rectDir
        $sw.Stop()
        $timings['run_rect'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        Write-Utf8 (Join-Path $rectDir 'rect_stdout.txt') $rectRun.Stdout
        Write-Utf8 (Join-Path $rectDir 'rect_stderr.txt') $rectRun.Stderr
        Write-Host $rectRun.Stdout
        if ($rectRun.Stderr) { Write-Host $rectRun.Stderr -ForegroundColor DarkYellow }
        if ($rectRun.ExitCode -ne 0) { throw "render_rect.exe exited with $($rectRun.ExitCode)" }

        Write-Host 'verify: rect PNG vs Pillow reference (maxdiff 0)' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        Invoke-Python 'verify_text.py --mode rect' @(
            $verifier,
            '--mode', 'rect',
            '--png', (Join-Path $rectDir 'scene_rect.png'),
            '--ir', (Join-Path $rectDir 'display_list.json'),
            '--report', (Join-Path $rectDir 'verify_rect.json'))
        $sw.Stop()
        $timings['verify_rect'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        $rectVerify = Get-Report (Join-Path $rectDir 'verify_rect.json')
        if ($rectVerify.failed -ne 0) { throw "rect verification failed" }

        Write-Host 'run: webnative-rect.exe --frames 5 (rect-only header)' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $rectWindow = Invoke-Process (Join-Path $rectDir 'webnative-rect.exe') '--frames 5' $rectDir
        $sw.Stop()
        $timings['run_rect_window'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        Write-Utf8 (Join-Path $rectDir 'rect_window_stdout.txt') $rectWindow.Stdout
        Write-Utf8 (Join-Path $rectDir 'rect_window_stderr.txt') $rectWindow.Stderr
        Write-Host $rectWindow.Stdout
        if ($rectWindow.Stderr) { Write-Host $rectWindow.Stderr -ForegroundColor DarkYellow }
        if ($rectWindow.ExitCode -ne 0) { throw "webnative-rect.exe exited with $($rectWindow.ExitCode)" }
        if ($rectWindow.Stdout -notmatch 'backend=D3D12') { throw "rect window adapter is not D3D12" }
        if ($rectWindow.Stdout -notmatch 'text=off') { throw "rect window did not take the text-off path" }
        $report['rect'] = [ordered]@{
            offscreen_exit = $rectRun.ExitCode
            parity = $rectVerify.metrics.diff
            window_exit = $rectWindow.ExitCode
            window_summary = (($rectWindow.Stdout -split "`r?`n") | Where-Object { $_ -match '^window: closed' } | Select-Object -First 1)
        }
    }

    $report.status = 'PASS'
    $report.finished = (Get-Date).ToString('o')
    foreach ($artifact in @(
            @{name='scene_text.exe'; path=(Join-Path $OutDir 'scene_text.exe')},
            @{name='webnative-text.exe'; path=(Join-Path $OutDir 'webnative-text.exe')},
            @{name='scene_text.png'; path=(Join-Path $OutDir 'scene_text.png')},
            @{name='scene_data.h'; path=(Join-Path $OutDir 'scene_data.h')},
            @{name='render_rect.exe'; path=(Join-Path $rectDir 'render_rect.exe')},
            @{name='scene_rect.png'; path=(Join-Path $rectDir 'scene_rect.png')})) {
        if (Test-Path -LiteralPath $artifact.path) {
            $item = Get-Item -LiteralPath $artifact.path
            $report.artifacts[$artifact.name] = [ordered]@{
                path = $item.FullName
                bytes = $item.Length
            }
        }
    }
    Write-Utf8 (Join-Path $OutDir 'build_timings.json') (($timings | ConvertTo-Json) + "`n")
    Write-Utf8 (Join-Path $OutDir 'build_report.json') (($report | ConvertTo-Json -Depth 8) + "`n")

    Write-Host ''
    Write-Host '================ canonical C text runtime: PASS ================' -ForegroundColor Green
    Write-Host ("out dir     : {0}" -f $OutDir)
    Write-Host ("adapter     : {0}" -f $report.offscreen.adapter)
    Write-Host ("atlas       : {0}" -f $report.offscreen.atlas_proof)
    Write-Host ("text verify : {0}/{1} checks" -f $report.text.passed, ($report.text.passed + $report.text.failed))
    Write-Host ("text parity : maxdiff={0} changed={1} ({2}%)" -f `
        $report.text.parity.maxdiff, $report.text.parity.changed_pixels, $report.text.parity.changed_pct)
    Write-Host ("bright text : {0} pixels" -f $report.text.bright_pixels)
    Write-Host ("proof       : only exe+dll present; PNG SHA-256 matches; files before: {0}" -f `
        $report.self_contained.files_before_run)
    if ($report.Contains('window')) {
        Write-Host ("window      : exit {0}; {1}" -f $report.window.exit_code, $report.window.summary)
    }
    if ($report.Contains('rect')) {
        Write-Host ("rect regress: maxdiff={0} changed={1}; window exit {2}" -f `
            $report.rect.parity.maxdiff, $report.rect.parity.changed_pixels, $report.rect.window_exit)
    }
    foreach ($key in $timings.Keys) {
        Write-Host ("timing {0,-16}: {1,9:N2} ms" -f $key, $timings[$key])
    }
    Write-Host '==============================================================' -ForegroundColor Green
    $script:failed = $false
}
catch {
    Write-Host ''
    Write-Host "BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray }
    $report.status = 'FAIL'
    $report.error = $_.Exception.Message
    $report.finished = (Get-Date).ToString('o')
    try {
        Write-Utf8 (Join-Path $OutDir 'build_timings.json') (($timings | ConvertTo-Json) + "`n")
        Write-Utf8 (Join-Path $OutDir 'build_report.json') (($report | ConvertTo-Json -Depth 8) + "`n")
    } catch { }
}
finally {
    $scripts | ForEach-Object {
        Remove-Item -LiteralPath $_ -Force -ErrorAction SilentlyContinue
    }
}

if ($script:failed) { exit 1 }
exit 0
