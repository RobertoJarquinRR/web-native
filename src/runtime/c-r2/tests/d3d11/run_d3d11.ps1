# src\runtime\c-r2\tests\d3d11\run_d3d11.ps1
#
# Acceptance for the D3D11 renderer backend (renderer_d3d11.c) as a second
# implementation of wn_renderer_ops:
#   - builds build_abi\standalone.exe / embedded.exe (both backends linked),
#   - captures a D3D11 window frame and compares it with the wgpu offscreen
#     reference of the same compiled document (glyphs, clips, solids),
#   - runs the same demos with WN_RENDERER=wgpu to prove the fallback,
#   - runs the pilot image page (18 WIC images) through D3D11 and compares the
#     window capture with the pilot's wgpu offscreen render when available.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\d3d11\run_d3d11.ps1
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$SkipPilot
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $here

$runtime = Split-Path -Parent (Split-Path -Parent $here)
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $runtime))
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$buildAbi = Join-Path $runtime 'build_abi'
$build = Join-Path $runtime 'build'
$abi = Join-Path $runtime 'build_abi.ps1'
$r2build = Join-Path $runtime 'build.ps1'
$pilot = Join-Path $runtime 'tests\pilot-media-library\run_pilot.ps1'
$pilotOut = Join-Path $runtime 'tests\pilot-media-library\out'
$verify = Join-Path $here 'verify_d3d11.py'
$py = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'
$logPath = Join-Path $out 'd3d11_run.log'
$reportPath = Join-Path $out 'd3d11_report.json'
$report = [ordered]@{
    suite      = 'd3d11-backend'
    started    = (Get-Date).ToString('o')
    timings_ms = [ordered]@{}
    checks     = [ordered]@{}
    runs       = [ordered]@{}
}

function Stop-Fail([string]$message) {
    Write-Host "D3D11 TEST FAILED: $message" -ForegroundColor Red
    throw $message
}

function Invoke-Process([string]$title, [string]$exe, [string[]]$arguments,
                        [string]$workdir, [hashtable]$environment) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($arguments | ForEach-Object {
        if ("$_" -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' '
    $psi.WorkingDirectory = $workdir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if ($null -ne $environment) {
        foreach ($key in $environment.Keys) {
            $psi.EnvironmentVariables[$key] = [string]$environment[$key]
        }
    }
    Write-Host "run: $title" -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $sw.Stop()
    $lines = @()
    if ($stdout) { $lines += ($stdout -split "`r?`n" | Where-Object { $_ -ne '' }) }
    if ($stderr) { $lines += ($stderr -split "`r?`n" | Where-Object { $_ -ne '' }) }
    foreach ($line in $lines) { Write-Host "  $line" }
    Add-Content -LiteralPath $logPath -Value "`r`n### $title`r`n> $exe $($psi.Arguments)" -Encoding utf8
    Add-Content -LiteralPath $logPath -Value ($lines -join "`r`n") -Encoding utf8
    $report.timings_ms[$title] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    return [ordered]@{
        exit  = $process.ExitCode
        wall  = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        lines = $lines
        text  = ($lines -join "`n")
    }
}

function Invoke-ChildScript([string]$title, [string]$script, [string[]]$arguments) {
    Write-Host "build: $title" -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File $script @arguments | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail "$title exited with $LASTEXITCODE" }
}

try {
    Set-Content -LiteralPath $logPath -Value "c-r2 D3D11 backend test log`r`n" -Encoding utf8
    foreach ($required in @($py, $verify)) {
        if (-not (Test-Path -LiteralPath $required)) { Stop-Fail "missing $required" }
    }

    if (-not $SkipBuild) {
        $standalone = Join-Path $buildAbi 'standalone.exe'
        $embedded = Join-Path $buildAbi 'embedded.exe'
        if (-not (Test-Path -LiteralPath $standalone) -or
            -not (Test-Path -LiteralPath $embedded)) {
            Invoke-ChildScript 'build_abi.ps1 -SkipRun' $abi @('-SkipRun')
        }
        if (-not (Test-Path -LiteralPath (Join-Path $build 'runtime_r2.exe')) -or
            -not (Test-Path -LiteralPath (Join-Path $build 'ui_data.h'))) {
            Invoke-ChildScript 'build.ps1 -SkipRustBuild -SkipParityCheck -SkipRun' $r2build `
                @('-SkipRustBuild', '-SkipParityCheck', '-SkipRun')
        }
    }
    foreach ($required in @(
        (Join-Path $buildAbi 'standalone.exe'),
        (Join-Path $buildAbi 'embedded.exe'),
        (Join-Path $build 'runtime_r2.exe'),
        (Join-Path $build 'ui_data.h')
    )) {
        if (-not (Test-Path -LiteralPath $required)) { Stop-Fail "missing $required" }
    }

    # ---------------------------------------------------- base fixture: D3D11
    $capture = Join-Path $out 'd3d11_960x640.png'
    Remove-Item -LiteralPath $capture -Force -ErrorAction SilentlyContinue
    $d3d11 = Invoke-Process 'standalone-d3d11-capture' (Join-Path $buildAbi 'standalone.exe') `
        @('--frames', '6') $buildAbi @{
            'WN_RENDERER' = 'd3d11'
            'WN_CAPTURE' = $capture
            'WN_CAPTURE_FRAME' = '3'
        }
    $report.runs['standalone_d3d11'] = [ordered]@{
        exit = $d3d11.exit
        wall_ms = $d3d11.wall
        backend = 'D3D11'
    }
    if ($d3d11.exit -ne 0) { Stop-Fail "standalone.exe (D3D11) exited $($d3d11.exit)" }
    if ($d3d11.text -notmatch 'backend=D3D11') { Stop-Fail 'standalone D3D11 run did not report backend=D3D11' }
    if ($d3d11.text -match 'device=Microsoft Basic Render Driver') {
        Stop-Fail 'standalone D3D11 run fell back to WARP'
    }
    if ($d3d11.text -notmatch 'captured frame 3') { Stop-Fail 'standalone D3D11 run did not capture frame 3' }
    if (-not (Test-Path -LiteralPath $capture)) { Stop-Fail "missing capture $capture" }
    $report.checks['standalone_d3d11_hardware'] = $true

    # ------------------------------------------- base fixture: wgpu reference
    $reference = Join-Path $out 'wgpu_960x640.png'
    Remove-Item -LiteralPath $reference -Force -ErrorAction SilentlyContinue
    $wgpuRender = Invoke-Process 'wgpu-offscreen-reference' (Join-Path $build 'runtime_r2.exe') `
        @('--render', '960', '640', $reference) $build $null
    if ($wgpuRender.exit -ne 0) { Stop-Fail "runtime_r2.exe --render exited $($wgpuRender.exit)" }
    if (-not (Test-Path -LiteralPath $reference)) { Stop-Fail "missing reference $reference" }
    $report.checks['wgpu_reference'] = $true

    # ------------------------------------------------------- screenshot parity
    $comparePath = Join-Path $out 'd3d11_compare.json'
    & $py $verify --capture $capture --reference $reference --report $comparePath `
        --label 'base-960x640-d3d11-vs-wgpu' | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'D3D11/wgpu base screenshot comparison failed' }
    $compare = Get-Content -LiteralPath $comparePath -Raw | ConvertFrom-Json
    $report.checks['screenshot_parity'] = $true
    $report['screenshot'] = [ordered]@{
        result           = $compare.result
        max_channel_diff = $compare.checks.max_channel_diff
        exact_fraction   = $compare.checks.exact_fraction
        content_fraction = $compare.checks.content_fraction
        capture          = 'out/d3d11_960x640.png'
        reference        = 'out/wgpu_960x640.png'
        report           = 'out/d3d11_compare.json'
    }

    # ------------------------------------------------- forced wgpu fallback
    $wgpu = Invoke-Process 'standalone-wgpu' (Join-Path $buildAbi 'standalone.exe') `
        @('--frames', '6') $buildAbi @{ 'WN_RENDERER' = 'wgpu' }
    $report.runs['standalone_wgpu'] = [ordered]@{
        exit = $wgpu.exit
        wall_ms = $wgpu.wall
        backend = $(if ($wgpu.text -match 'backend=(\S+)') { $matches[1] } else { '?' })
    }
    if ($wgpu.exit -ne 0) { Stop-Fail "standalone.exe (wgpu) exited $($wgpu.exit)" }
    if ($wgpu.text -notmatch 'backend=(D3D11|D3D12|Vulkan)') {
        Stop-Fail 'forced wgpu run did not report a wgpu backend'
    }
    if ($wgpu.text -notmatch 'exit=0 frames=6') { Stop-Fail 'forced wgpu run did not present 6 frames' }
    $report.checks['wgpu_fallback'] = $true

    # --------------------------------------------------- embedded: both paths
    $embeddedD3d = Invoke-Process 'embedded-d3d11' (Join-Path $buildAbi 'embedded.exe') `
        @('--frames', '20', '--input-demo', '--resize-demo') $buildAbi @{ 'WN_RENDERER' = 'd3d11' }
    if ($embeddedD3d.exit -ne 0) { Stop-Fail "embedded.exe (D3D11) exited $($embeddedD3d.exit)" }
    if ($embeddedD3d.text -notmatch 'backend=D3D11') { Stop-Fail 'embedded D3D11 run did not report backend=D3D11' }
    if ($embeddedD3d.text -notmatch 'exit=0 frames=20') { Stop-Fail 'embedded D3D11 run did not present 20 frames' }
    if ($embeddedD3d.text -notmatch 'pointer_ups=1') { Stop-Fail 'embedded D3D11 run did not select a node' }
    $report.checks['embedded_d3d11'] = $true

    $embeddedWgpu = Invoke-Process 'embedded-wgpu' (Join-Path $buildAbi 'embedded.exe') `
        @('--frames', '20', '--input-demo', '--resize-demo') $buildAbi @{ 'WN_RENDERER' = 'wgpu' }
    if ($embeddedWgpu.exit -ne 0) { Stop-Fail "embedded.exe (wgpu) exited $($embeddedWgpu.exit)" }
    if ($embeddedWgpu.text -notmatch 'backend=(D3D11|D3D12|Vulkan)') { Stop-Fail 'embedded wgpu run did not report a backend' }
    if ($embeddedWgpu.text -notmatch 'exit=0 frames=20') { Stop-Fail 'embedded wgpu run did not present 20 frames' }
    $report.checks['embedded_wgpu'] = $true

    # ------------------------------------- images: pilot page through D3D11
    $pilotExe = Join-Path $pilotOut 'abi\abi_pilot.exe'
    $pilotReference = Join-Path $pilotOut 'render_1280.png'
    if ($SkipPilot) {
        $report.checks['pilot_images'] = 'skipped'
    } else {
        if (-not (Test-Path -LiteralPath $pilotExe) -or
            -not (Test-Path -LiteralPath $pilotReference)) {
            Invoke-ChildScript 'run_pilot.ps1 (image fixture)' $pilot @()
        }
        if ((Test-Path -LiteralPath $pilotExe) -and (Test-Path -LiteralPath $pilotReference)) {
            $pilotCapture = Join-Path $out 'd3d11_pilot_1280x800.png'
            Remove-Item -LiteralPath $pilotCapture -Force -ErrorAction SilentlyContinue
            $pilotRun = Invoke-Process 'pilot-d3d11-capture' $pilotExe `
                @('--frames', '5', '--width', '1280', '--height', '800') $pilotOut @{
                    'WN_RENDERER' = 'd3d11'
                    'WN_CAPTURE' = $pilotCapture
                    'WN_CAPTURE_FRAME' = '3'
                }
            if ($pilotRun.exit -ne 0) { Stop-Fail "abi_pilot.exe (D3D11) exited $($pilotRun.exit)" }
            if ($pilotRun.text -notmatch 'backend=D3D11') { Stop-Fail 'pilot D3D11 run did not report backend=D3D11' }
            if (-not (Test-Path -LiteralPath $pilotCapture)) { Stop-Fail "missing pilot capture $pilotCapture" }
            $pilotCompare = Join-Path $out 'd3d11_compare_pilot.json'
            & $py $verify --capture $pilotCapture --reference $pilotReference `
                --report $pilotCompare --label 'pilot-1280x800-d3d11-vs-wgpu' | Write-Host
            if ($LASTEXITCODE -ne 0) { Stop-Fail 'pilot D3D11/wgpu screenshot comparison failed' }
            $comparePilot = Get-Content -LiteralPath $pilotCompare -Raw | ConvertFrom-Json
            $report.checks['pilot_images'] = $true
            $report['pilot_screenshot'] = [ordered]@{
                result           = $comparePilot.result
                max_channel_diff = $comparePilot.checks.max_channel_diff
                exact_fraction   = $comparePilot.checks.exact_fraction
                content_fraction = $comparePilot.checks.content_fraction
                capture          = 'out/d3d11_pilot_1280x800.png'
                reference        = 'tests/pilot-media-library/out/render_1280.png'
                report           = 'out/d3d11_compare_pilot.json'
            }
        } else {
            Write-Host 'pilot image fixtures unavailable; skipping the image screenshot check' -ForegroundColor Yellow
            $report.checks['pilot_images'] = 'unavailable'
        }
    }

    $report['finished'] = (Get-Date).ToString('o')
    $report['artifacts'] = [ordered]@{
        log     = 'tests/d3d11/out/d3d11_run.log'
        capture = 'tests/d3d11/out/d3d11_960x640.png'
        reference = 'tests/d3d11/out/wgpu_960x640.png'
    }
    [System.IO.File]::WriteAllText($reportPath,
        ($report | ConvertTo-Json -Depth 8),
        (New-Object System.Text.UTF8Encoding($false)))

    Write-Host ''
    Write-Host '================ D3D11 backend: PASS ================' -ForegroundColor Green
    Write-Host ("screenshot: max_diff={0} exact={1:P2} content={2:P1}" -f `
        $report.screenshot.max_channel_diff, $report.screenshot.exact_fraction,
        $report.screenshot.content_fraction)
    foreach ($key in $report.timings_ms.Keys) {
        Write-Host ("timing {0,-26}: {1,9:N2} ms" -f $key, $report.timings_ms[$key])
    }
    Write-Host '====================================================' -ForegroundColor Green
}
catch {
    Write-Host ''
    Write-Host "D3D11 TEST FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
