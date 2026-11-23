# exp10-aot-gpu build: IR JSON -> baked GPU data -> wgpu-native -> PNG.
#
# Build/verify only. The produced scene_gpu.exe has no Python dependency.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build.ps1
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Proof
#
[CmdletBinding()]
param(
    [switch]$Proof,
    [switch]$SkipFetch
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root

$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$wgpuVersionFile = 'wgpu-native\VERSION.txt'
$timings = [ordered]@{}
$report = [ordered]@{
    experiment   = 'exp10-aot-gpu'
    started      = (Get-Date).ToString('o')
    timings_ms   = $timings
    commands     = [ordered]@{}
    proof        = $null
}

function Stop-Fail([string]$message) {
    Write-Host "BUILD FAILED: $message" -ForegroundColor Red
    throw $message
}

function Invoke-Native([string]$what, [string]$command) {
    Write-Host "  $command" -ForegroundColor DarkGray
    $scriptFile = Join-Path $root 'build_step.cmd'
    $content = "@echo off`r`n$command`r`nexit /b %errorlevel%`r`n"
    Set-Content -LiteralPath $scriptFile -Value $content -Encoding ascii
    & cmd.exe /d /c $scriptFile | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Stop-Fail "$what exited with $LASTEXITCODE"
    }
}

function Compile-Scene([string]$exePath) {
    $compile = "cl /nologo /O2 /W3 /std:c17 /TC runtime_gpu.c /Iwgpu-native\include /Fe:$exePath " +
               "/link wgpu-native\lib\wgpu_native.dll.lib windowscodecs.lib ole32.lib user32.lib"
    $line = "call `"$vcvars`" >nul && $compile"
    Invoke-Native 'compile' $line
    return $compile
}

function Run-Isolated([string]$exePath, [string]$arguments, [string]$workDir, [string]$dllDir) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path -LiteralPath $exePath).Path
    $psi.Arguments = $arguments
    $psi.WorkingDirectory = $workDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    # Acceptance: only System32 and the DLL directory on PATH.
    $psi.EnvironmentVariables['PATH'] = "$env:WINDIR\System32;$dllDir"
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

function Invoke-Verify([string]$png, [string]$reportFile, [string[]]$extra) {
    $verifyArgs = @('verify.py', '--png', $png, '--report', $reportFile)
    foreach ($item in $extra) { $verifyArgs += @('--extra', $item) }
    & $script:py @verifyArgs
    if ($LASTEXITCODE -ne 0) {
        Stop-Fail "pixel verification failed for $png"
    }
}

function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $path).Path)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLower()
    }
    finally {
        $sha.Dispose()
    }
}

try {
    if (-not (Test-Path -LiteralPath $vcvars)) {
        Stop-Fail "vcvars64.bat not found at $vcvars"
    }

    # ---------------------------------------------------------- toolchain/qc
    $script:py = Join-Path $root '.venv\Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $script:py)) {
        Write-Host 'setup: creating .venv (build/verify only)' -ForegroundColor Cyan
        & py -3.12 -m venv .venv
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'python -m venv failed' }
        & $script:py -m pip install --quiet --disable-pip-version-check pillow
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'pip install pillow failed' }
    }
    & $script:py -c "import PIL" 2>$null
    if ($LASTEXITCODE -ne 0) {
        & $script:py -m pip install --quiet --disable-pip-version-check pillow
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'pip install pillow failed' }
    }

    # ------------------------------------------------------------- wgpu-native
    if (-not $SkipFetch -and -not (Test-Path -LiteralPath $wgpuVersionFile)) {
        Write-Host 'setup: downloading wgpu-native (Python urllib)' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $script:py fetch_wgpu.py
        $sw.Stop()
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'fetch_wgpu.py failed' }
        $timings['fetch_wgpu'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    }
    if (-not (Test-Path -LiteralPath $wgpuVersionFile)) {
        Stop-Fail 'wgpu-native is not present; run py -3.12 fetch_wgpu.py'
    }
    $wgpuVersion = (Get-Content -LiteralPath $wgpuVersionFile -Raw).Trim()
    $report['wgpu_native'] = $wgpuVersion

    # ------------------------------------------------------------------ emit
    Write-Host 'emit: display_list.json -> scene_data.h' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $script:py emit_c.py --ir display_list.json --out scene_data.h
    $sw.Stop()
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'emit_c.py failed' }
    $timings['emit'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $emitLine = "$script:py emit_c.py --ir display_list.json --out scene_data.h"
    $report['commands']['emit'] = $emitLine

    # --------------------------------------------------------------- compile
    Write-Host 'compile: cl /O2 -> scene_gpu.exe' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $compileLine = Compile-Scene 'scene_gpu.exe'
    $sw.Stop()
    $timings['compile'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report['commands']['compile'] = $compileLine

    if (-not (Test-Path -LiteralPath 'scene_gpu.exe')) { Stop-Fail 'scene_gpu.exe was not produced' }
    Copy-Item -LiteralPath 'wgpu-native\bin\wgpu_native.dll' -Destination 'wgpu_native.dll' -Force

    # ------------------------------------------------------------------- run
    Write-Host 'run: isolated PATH = System32 + this directory' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $run = Run-Isolated 'scene_gpu.exe' 'scene_gpu.png' $root $root
    $sw.Stop()
    $timings['run'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    Write-Host $run.Stdout
    if ($run.Stderr) { Write-Host $run.Stderr -ForegroundColor DarkYellow }
    if ($run.ExitCode -ne 0) { Stop-Fail "scene_gpu.exe exited with $($run.ExitCode)" }
    $report['isolation'] = [ordered]@{
        path      = $run.PathUsed
        exit_code = $run.ExitCode
        stdout    = $run.Stdout.Trim()
        stderr    = $run.Stderr.Trim()
    }
    $report['commands']['run'] = "PATH=$($run.PathUsed) ; .\scene_gpu.exe scene_gpu.png"
    $report['commands']['verify'] = "$script:py verify.py --png scene_gpu.png --report verify_report.json"

    $adapterLine = ($run.Stdout -split "`r?`n" | Where-Object { $_ -match '^adapter: backend=' } | Select-Object -First 1)
    $deviceLine = ($run.Stdout -split "`r?`n" | Where-Object { $_ -match '^adapter: device=' } | Select-Object -First 1)
    $report['adapter'] = [ordered]@{ backend = $adapterLine; device = $deviceLine }

    # ---------------------------------------------------------------- verify
    Write-Host 'verify: Pillow pixel checks (build-time only)' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Verify 'scene_gpu.png' 'verify_report.json' @()
    $sw.Stop()
    $timings['verify'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report['verify_report'] = 'verify_report.json'

    # ------------------------------------------------------------ strong proof
    if ($Proof) {
        Write-Host 'proof: append a 4th rect to display_list.json, re-emit, recompile, verify' -ForegroundColor Cyan
        New-Item -ItemType Directory -Force -Path 'proof' | Out-Null
        Copy-Item -LiteralPath 'display_list.json' -Destination 'proof\display_list.canonical.json' -Force

        & $script:py proof_add_rect.py --ir display_list.json
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'proof_add_rect.py failed' }

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $script:py emit_c.py --ir display_list.json --out scene_data.h
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'proof emit failed' }
        $null = Compile-Scene 'proof\scene_gpu_proof.exe'
        Copy-Item -LiteralPath 'wgpu-native\bin\wgpu_native.dll' -Destination 'proof\wgpu_native.dll' -Force
        $proofRun = Run-Isolated 'proof\scene_gpu_proof.exe' 'scene_gpu_proof.png' (Join-Path $root 'proof') (Join-Path $root 'proof')
        $sw.Stop()
        Write-Host $proofRun.Stdout
        if ($proofRun.Stderr) { Write-Host $proofRun.Stderr -ForegroundColor DarkYellow }
        if ($proofRun.ExitCode -ne 0) { Stop-Fail "proof exe exited with $($proofRun.ExitCode)" }
        $timings['proof_rebuild_run'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        Invoke-Verify 'proof\scene_gpu_proof.png' 'proof\verify_report_proof.json' @('proof-rect,300,210,#2ecc71')
        $report['proof'] = [ordered]@{
            extra_rect   = 'x=260 y=180 w=80 h=60 fill=#2ecc71'
            exe          = 'proof\scene_gpu_proof.exe'
            png          = 'proof\scene_gpu_proof.png'
            verify       = 'proof\verify_report_proof.json'
            pixel        = '(300,210) = #2ecc71'
            note         = 'display_list.json was modified, re-emitted, recompiled and rendered; canonical file restored afterwards'
        }

        # restore canonical scene and rebuild the final artifact
        Copy-Item -LiteralPath 'proof\display_list.canonical.json' -Destination 'display_list.json' -Force
        & $script:py emit_c.py --ir display_list.json --out scene_data.h
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'canonical re-emit failed' }
        $null = Compile-Scene 'scene_gpu.exe'
        Copy-Item -LiteralPath 'wgpu-native\bin\wgpu_native.dll' -Destination 'wgpu_native.dll' -Force
        $finalRun = Run-Isolated 'scene_gpu.exe' 'scene_gpu.png' $root $root
        if ($finalRun.ExitCode -ne 0) { Stop-Fail "canonical rebuild exited with $($finalRun.ExitCode)" }
        Invoke-Verify 'scene_gpu.png' 'verify_report.json' @()

        # keep a copy of the proof artifacts for inspection
        Copy-Item -LiteralPath 'display_list.json' -Destination 'proof\display_list.canonical.json' -Force
    }

    # ---------------------------------------------------------------- summary
    $exe = Get-Item -LiteralPath 'scene_gpu.exe'
    $png = Get-Item -LiteralPath 'scene_gpu.png'
    $scriptFile = Join-Path $root 'build_step.cmd'
    Set-Content -LiteralPath $scriptFile -Value "@echo off`r`ncall `"$vcvars`" >nul && cl 2>&1`r`nexit /b 0`r`n" -Encoding ascii
    $compiler = (& cmd.exe /d /c $scriptFile | Select-Object -First 1)
    $report['exe'] = [ordered]@{ path = $exe.FullName; bytes = $exe.Length }
    $report['png'] = [ordered]@{ path = $png.FullName; bytes = $png.Length }
    $report['hashes'] = [ordered]@{
        display_list_json = Get-Sha256 'display_list.json'
        scene_data_h      = Get-Sha256 'scene_data.h'
        scene_gpu_png     = Get-Sha256 'scene_gpu.png'
    }
    $report['compiler'] = $compiler
    $report['finished'] = (Get-Date).ToString('o')

    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath 'build_report.json' -Encoding utf8

    Write-Host ''
    Write-Host '================ exp10-aot-gpu: PASS ================' -ForegroundColor Green
    Write-Host ("wgpu-native : {0}" -f $wgpuVersion)
    Write-Host ("compiler    : {0}" -f $compiler)
    Write-Host ("adapter     : {0}" -f ($adapterLine -replace '^adapter: ', ''))
    Write-Host ("device      : {0}" -f ($deviceLine -replace '^adapter: device=', ''))
    Write-Host ("artifact    : {0} ({1} bytes)" -f $exe.FullName, $exe.Length)
    Write-Host ("png         : {0} ({1} bytes)" -f $png.FullName, $png.Length)
    Write-Host ("isolated    : exit={0} PATH={1}" -f $run.ExitCode, $run.PathUsed)
    foreach ($key in $timings.Keys) {
        Write-Host ("timing {0,-16}: {1,9:N2} ms" -f $key, $timings[$key])
    }
    Write-Host '=====================================================' -ForegroundColor Green
}
finally {
    $helper = Join-Path $root 'build_step.cmd'
    if (Test-Path -LiteralPath $helper) { Remove-Item -LiteralPath $helper -Force }
    Pop-Location
}
