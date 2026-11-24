# src\runtime\c-r2\tests\pilot-media-library\run_pilot.ps1
#
# End-to-end evidence for ui-tree/2 (grid + vertical scroll/clip + images):
#   1. generate 18 tiny poster PNGs and the hand-authored ui.json,
#   2. emit ui_data.h with src\emitter\python\emit_ui.py --ui-json,
#   3. compile runtime_r2.c against that header (separate from build\),
#   4. render 1280x800 and 800x600, plus wheel-scrolled frames,
#   5. run verify_pilot.py (grid columns, object-fit, clipping, wheel).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\pilot-media-library\run_pilot.ps1
[CmdletBinding()]
param(
    [switch]$SkipCompile
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $here

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))))
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$py = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'
$emitter = Join-Path $repo 'src\emitter\python\emit_ui.py'
$runtime = Join-Path $repo 'src\runtime\c-r2'
$wgpu = Join-Path $repo 'third_party\wgpu-native'
$report = [ordered]@{
    suite      = 'pilot-media-library'
    schema     = 'webnative.ui-tree/2'
    started    = (Get-Date).ToString('o')
    timings_ms = [ordered]@{}
}

function Stop-Fail([string]$message) {
    Write-Host "PILOT FAILED: $message" -ForegroundColor Red
    throw $message
}

function Find-VcVars {
    $fixed = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $fixed) { return $fixed }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $install = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if ($LASTEXITCODE -eq 0 -and $install) {
            return (Join-Path $install.Trim() 'VC\Auxiliary\Build\vcvars64.bat')
        }
    }
    return $null
}

function Invoke-Command([string]$what, [string]$command) {
    $step = Join-Path $out 'pilot_step.cmd'
    Set-Content -LiteralPath $step -Encoding ascii -Value "@echo off`r`n$command`r`nexit /b %errorlevel%`r`n"
    & cmd.exe /d /c $step | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail "$what exited with $LASTEXITCODE" }
    Remove-Item -LiteralPath $step -Force -ErrorAction SilentlyContinue
}

# Captures stdout/stderr and writes them as UTF-8, one log per render.
function Invoke-Render([string]$title, [string[]]$arguments, [string]$logName) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $out 'runtime_r2_pilot.exe'
    $psi.Arguments = ($arguments | ForEach-Object {
        if ("$_" -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' '
    $psi.WorkingDirectory = $out
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $sw.Stop()
    $lines = @()
    if ($stdout) { $lines += ($stdout -split "`r?`n" | Where-Object { $_ -ne '' }) }
    if ($stderr) { $lines += ($stderr -split "`r?`n" | Where-Object { $_ -ne '' }) }
    $lines | Set-Content -LiteralPath (Join-Path $out $logName) -Encoding utf8
    foreach ($line in $lines) { Write-Host "  $line" }
    if ($process.ExitCode -ne 0) { Stop-Fail "$title exited with $($process.ExitCode)" }
    $report.timings_ms[$title] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    return $lines
}

try {
    foreach ($required in @($py, $emitter, (Join-Path $runtime 'runtime_r2.c'),
                            (Join-Path $runtime 'wn_core.c'), (Join-Path $runtime 'wn_common.c'))) {
        if (-not (Test-Path -LiteralPath $required)) { Stop-Fail "missing $required" }
    }

    Write-Host 'pilot: posters + ui.json' -ForegroundColor Cyan
    & $py (Join-Path $here 'make_posters.py') | Write-Host
    & $py (Join-Path $here 'make_ui.py') | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'poster/ui.json generation failed' }

    Write-Host 'pilot: emit_ui.py --ui-json -> out\ui_data.h' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $py $emitter --ui-json (Join-Path $here 'ui.json') `
        --out (Join-Path $out 'ui_data.h') --meta (Join-Path $out 'ui_meta.json') | Write-Host
    $sw.Stop()
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'emit_ui.py --ui-json failed' }
    $report.timings_ms['emit'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)

    New-Item -ItemType Directory -Force -Path (Join-Path $out 'posters') | Out-Null
    Copy-Item -Path (Join-Path $here 'posters\*.png') -Destination (Join-Path $out 'posters') -Force

    if (-not $SkipCompile) {
        $vcvars = Find-VcVars
        if (-not $vcvars) { Stop-Fail 'vcvars64.bat not found' }
        Write-Host 'pilot: compile runtime_r2.exe against the pilot ui_data.h' -ForegroundColor Cyan
        $compile = "call `"$vcvars`" >nul && cd /d `"$out`" && cl /nologo /O2 /W3 /std:c17 /TC " +
            "/D_CRT_SECURE_NO_WARNINGS /DWN_STATIC " +
            "`"$(Join-Path $runtime 'runtime_r2.c')`" `"$(Join-Path $runtime 'wn_core.c')`" " +
            "`"$(Join-Path $runtime 'wn_common.c')`" " +
            "/I`"$(Join-Path $repo 'include')`" /I`"$runtime`" /I`"$(Join-Path $wgpu 'include')`" " +
            "/I`"$out`" /Fo:`"$out\\`" /Fe:`"$out\runtime_r2_pilot.exe`" " +
            "/link `"$(Join-Path $wgpu 'lib\wgpu_native.dll.lib')`" windowscodecs.lib ole32.lib user32.lib gdi32.lib"
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        Invoke-Command 'compile' $compile
        $sw.Stop()
        $report.timings_ms['compile'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') `
            -Destination (Join-Path $out 'wgpu_native.dll') -Force
    }

    Write-Host 'pilot: layout report (1280x800, 800x600)' -ForegroundColor Cyan
    Invoke-Render 'layout_report' @('--report', 'pilot_layout.json',
        '--sizes', '1280x800,800x600') 'layout.log' | Out-Null

    Write-Host 'pilot: offscreen render 1280x800' -ForegroundColor Cyan
    Invoke-Render 'render_1280' @('--render', '1280', '800', 'render_1280.png') 'render_1280.log' | Out-Null
    Write-Host 'pilot: offscreen render 800x600' -ForegroundColor Cyan
    Invoke-Render 'render_800' @('--render', '800', '600', 'render_800.png') 'render_800.log' | Out-Null

    Write-Host 'pilot: wheel +240 at 800x600' -ForegroundColor Cyan
    Invoke-Render 'wheel_800' @('--render', '800', '600', 'wheel_800.png',
        '--wheel', '400', '300', '240') 'wheel_800.log' | Out-Null
    Write-Host 'pilot: wheel +200 at 1280x800' -ForegroundColor Cyan
    Invoke-Render 'wheel_1280' @('--render', '1280', '800', 'wheel_1280.png',
        '--wheel', '600', '300', '200') 'wheel_1280.log' | Out-Null
    Write-Host 'pilot: wheel clamp (+4000) at 800x600' -ForegroundColor Cyan
    Invoke-Render 'wheel_max' @('--render', '800', '600', 'wheel_max.png',
        '--wheel', '400', '300', '4000') 'wheel_max.log' | Out-Null
    Write-Host 'pilot: wheel outside the scroll container' -ForegroundColor Cyan
    Invoke-Render 'wheel_outside' @('--render', '800', '600', 'wheel_outside.png',
        '--wheel', '400', '560', '240') 'wheel_outside.log' | Out-Null

    Write-Host 'pilot: verify grid/object-fit/clip/wheel' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $py (Join-Path $here 'verify_pilot.py') --dir $out | Write-Host
    $sw.Stop()
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'verify_pilot.py failed' }
    $report.timings_ms['verify'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)

    # Windowed ABI path: renderer_wgpu.c with the same pilot ui_data.h. This
    # exercises the WIC image pipeline + scissor batches outside the offscreen
    # reference renderer.
    Write-Host 'pilot: ABI window renderer (renderer_wgpu.c) with the pilot tree' -ForegroundColor Cyan
    $vcvars = Find-VcVars
    if (-not $vcvars) { Stop-Fail 'vcvars64.bat not found' }
    $abiDir = Join-Path $out 'abi'
    New-Item -ItemType Directory -Force -Path $abiDir | Out-Null
    $abiObjects = @('wn_common.c', 'wn_core.c', 'renderer_wgpu.c',
                    'renderer_d3d11.c', 'host_win32.c', 'wn_view.c', 'wn_app.c') |
        ForEach-Object { Join-Path $runtime $_ }
    $abiList = ($abiObjects | ForEach-Object { "`"$_`"" }) -join ' '
    $abiCmd = "call `"$vcvars`" >nul && cd /d `"$abiDir`" && cl /nologo /O2 /W3 /std:c17 /TC " +
        "/D_CRT_SECURE_NO_WARNINGS /DWN_STATIC /c $abiList /Fo:`"$abiDir\\`" " +
        "/I`"$(Join-Path $repo 'include')`" /I`"$runtime`" /I`"$(Join-Path $wgpu 'include')`" /I`"$out`" " +
        "&& cl /nologo /O2 /W3 /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS /DWN_STATIC /c " +
        "`"$(Join-Path $runtime 'examples\standalone.c')`" /Fo:`"$abiDir\standalone.obj`" " +
        "/I`"$(Join-Path $repo 'include')`" /I`"$runtime`" /I`"$(Join-Path $wgpu 'include')`" /I`"$out`" " +
        "&& link /nologo /OUT:`"$abiDir\abi_pilot.exe`" `"$abiDir\standalone.obj`" " +
        (($abiObjects | ForEach-Object {
            "`"$abiDir\$([System.IO.Path]::GetFileNameWithoutExtension($_)).obj`""
        }) -join ' ') +
        " `"$(Join-Path $wgpu 'lib\wgpu_native.dll.lib')`" d3d11.lib dxgi.lib d3dcompiler.lib " +
        "windowscodecs.lib ole32.lib user32.lib gdi32.lib"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Command 'compile abi pilot' $abiCmd
    $sw.Stop()
    $report.timings_ms['compile_abi'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') `
        -Destination (Join-Path $abiDir 'wgpu_native.dll') -Force

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $abiDir 'abi_pilot.exe'
    $psi.Arguments = '--frames 20 --wheel-demo'
    $psi.WorkingDirectory = $out
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables['WN_RENDERER'] = 'wgpu'  # this run proves the wgpu path
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $sw.Stop()
    $abiLines = @()
    if ($stdout) { $abiLines += ($stdout -split "`r?`n" | Where-Object { $_ -ne '' }) }
    if ($stderr) { $abiLines += ($stderr -split "`r?`n" | Where-Object { $_ -ne '' }) }
    $abiLines | Set-Content -LiteralPath (Join-Path $out 'abi_pilot.log') -Encoding utf8
    foreach ($line in $abiLines) { Write-Host "  $line" }
    if ($process.ExitCode -ne 0) { Stop-Fail "abi_pilot.exe exited with $($process.ExitCode)" }
    if (($abiLines -join "`n") -notmatch 'backend=D3D12') { Stop-Fail 'abi_pilot did not report D3D12' }
    if (($abiLines -join "`n") -notmatch 'exit=0 frames=20') { Stop-Fail 'abi_pilot did not present 20 frames' }
    if (($abiLines -join "`n") -notmatch 'standalone: WHEEL .*dy=120 target=body\.root/div\.scroll') {
        Stop-Fail 'abi_pilot wheel-demo did not route WM_MOUSEWHEEL to the scroll container'
    }
    $report.timings_ms['abi_window'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report['abi_window'] = [ordered]@{
        exit         = $process.ExitCode
        frames       = 20
        wheel_target = 'body.root/div.scroll'
        log          = 'out/abi_pilot.log'
        exe          = 'out/abi/abi_pilot.exe'
    }

    # D3D11 image/glyph parity: the same pilot tree captured from the window
    # (renderer_d3d11.c, WIC image cache, scissor batches) at 1280x800 compared
    # with the wgpu offscreen reference render_1280.png.
    Write-Host 'pilot: D3D11 window capture parity (images + glyphs + clips)' -ForegroundColor Cyan
    $capturePath = Join-Path $abiDir 'capture_d3d11_1280.png'
    $capturePsi = New-Object System.Diagnostics.ProcessStartInfo
    $capturePsi.FileName = Join-Path $abiDir 'abi_pilot.exe'
    $capturePsi.Arguments = '--frames 5 --width 1280 --height 800'
    $capturePsi.WorkingDirectory = $out
    $capturePsi.UseShellExecute = $false
    $capturePsi.CreateNoWindow = $true
    $capturePsi.RedirectStandardOutput = $true
    $capturePsi.RedirectStandardError = $true
    $capturePsi.EnvironmentVariables['WN_RENDERER'] = 'd3d11'
    $capturePsi.EnvironmentVariables['WN_CAPTURE'] = $capturePath
    $capturePsi.EnvironmentVariables['WN_CAPTURE_FRAME'] = '3'
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $captureProcess = [System.Diagnostics.Process]::Start($capturePsi)
    $captureOut = $captureProcess.StandardOutput.ReadToEnd()
    $captureErr = $captureProcess.StandardError.ReadToEnd()
    $captureProcess.WaitForExit()
    $sw.Stop()
    $captureLines = @()
    if ($captureOut) { $captureLines += ($captureOut -split "`r?`n" | Where-Object { $_ -ne '' }) }
    if ($captureErr) { $captureLines += ($captureErr -split "`r?`n" | Where-Object { $_ -ne '' }) }
    $captureLines | Set-Content -LiteralPath (Join-Path $abiDir 'abi_pilot_d3d11.log') -Encoding utf8
    foreach ($line in $captureLines) { Write-Host "  $line" }
    $captureText = $captureLines -join "`n"
    if ($captureProcess.ExitCode -ne 0) { Stop-Fail "D3D11 capture run exited $($captureProcess.ExitCode)" }
    if ($captureText -notmatch 'backend=D3D11') { Stop-Fail 'D3D11 capture run did not report backend=D3D11' }
    if ($captureText -match 'device=Microsoft Basic Render Driver') {
        Stop-Fail 'D3D11 capture run fell back to WARP'
    }
    if ($captureText -notmatch 'captured frame 3') { Stop-Fail 'D3D11 capture run did not write frame 3' }
    $comparePath = Join-Path $abiDir 'd3d11_compare_1280.json'
    & $py (Join-Path $runtime 'tests\d3d11\verify_d3d11.py') `
        --capture $capturePath --reference (Join-Path $out 'render_1280.png') `
        --report $comparePath --label 'pilot-1280-d3d11-vs-wgpu' | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'D3D11/wgpu screenshot comparison failed' }
    $compare = Get-Content -LiteralPath $comparePath -Raw | ConvertFrom-Json
    $report.timings_ms['d3d11_capture'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report['d3d11_screenshot'] = [ordered]@{
        result            = $compare.result
        capture           = 'out/abi/capture_d3d11_1280.png'
        reference         = 'out/render_1280.png'
        max_channel_diff  = $compare.checks.max_channel_diff
        exact_fraction    = $compare.checks.exact_fraction
        content_fraction  = $compare.checks.content_fraction
        log               = 'out/abi/abi_pilot_d3d11.log'
        report            = 'out/abi/d3d11_compare_1280.json'
    }

    $verify = Get-Content -LiteralPath (Join-Path $out 'pilot_verify_report.json') -Raw | ConvertFrom-Json
    $report['verify'] = [ordered]@{
        ok     = $verify.ok
        passed = $verify.total_checks - $verify.failed_checks
        total  = $verify.total_checks
        failed = $verify.failed_checks
    }
    $report['scroll'] = $verify.scroll
    $report['render'] = $verify.render
    $report['artifacts'] = [ordered]@{
        ui_data          = 'out/ui_data.h'
        ui_meta          = 'out/ui_meta.json'
        layout           = 'out/pilot_layout.json'
        render_1280      = 'out/render_1280.png'
        render_800       = 'out/render_800.png'
        wheel_1280       = 'out/wheel_1280.png'
        wheel_800        = 'out/wheel_800.png'
        wheel_max        = 'out/wheel_max.png'
        wheel_outside    = 'out/wheel_outside.png'
        verify           = 'out/pilot_verify_report.json'
    }
    $report['finished'] = (Get-Date).ToString('o')
    [System.IO.File]::WriteAllText((Join-Path $out 'pilot_report.json'),
        ($report | ConvertTo-Json -Depth 8),
        (New-Object System.Text.UTF8Encoding($false)))

    Write-Host ''
    Write-Host '========== pilot-media-library: PASS ==========' -ForegroundColor Green
    Write-Host ("verify : {0}/{1} checks" -f $report.verify.passed, $report.verify.total)
    foreach ($key in $report.timings_ms.Keys) {
        Write-Host ("timing {0,-14}: {1,9:N2} ms" -f $key, $report.timings_ms[$key])
    }
    Write-Host '===============================================' -ForegroundColor Green
}
catch {
    Write-Host ''
    Write-Host "PILOT FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    $step = Join-Path $out 'pilot_step.cmd'
    if (Test-Path -LiteralPath $step) { Remove-Item -LiteralPath $step -Force -ErrorAction SilentlyContinue }
    Pop-Location
}
