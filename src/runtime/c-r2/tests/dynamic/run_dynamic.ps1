# src\runtime\c-r2\tests\dynamic\run_dynamic.ps1 - Dynamic UI API v0 acceptance.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\dynamic\run_dynamic.ps1
#
# Builds and runs three layers against the small tests/dynamic fixture:
#   1. dynamic_core_test.exe  - headless core: visibility, clone/append/remove,
#      clear_children, set_image, set_text, layout/paint invalidation.
#   2. runtime_dynamic.exe --dynamic - offscreen wgpu renders (11 PNG stages).
#   3. dynamic.exe            - C ABI v3 wn_app_* demo with a real window.
# Then verify_dynamic.py checks geometry, counts and pixels (53 checks).
#
# -Regenerate re-emits page\ui.json + fixture\ui_data.h with frontend-rust and
# emit_ui.py. The committed fixture is used otherwise.
[CmdletBinding()]
param(
    [switch]$Regenerate,
    [switch]$SkipRun
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $here

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))))
$out = Join-Path $here 'out'
$fixture = Join-Path $here 'fixture'
$page = Join-Path $here 'page'
$wgpu = Join-Path $repo 'third_party\wgpu-native'
$py = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'
$rustExe = Join-Path $repo 'src\frontend\rust\target\release\frontend-rust.exe'
$emitter = Join-Path $repo 'src\emitter\python\emit_ui.py'
$include = Join-Path $repo 'include'
$report = [ordered]@{
    layer      = 'c-r2-dynamic-v0'
    started    = (Get-Date).ToString('o')
    timings_ms = [ordered]@{}
    checks     = [ordered]@{}
    artifacts  = [ordered]@{
        core_events       = 'out/core_events.jsonl'
        stage_report      = 'out/stage_report.json'
        verify_report     = 'out/dynamic_verify_report.json'
        runtime_log       = 'out/runtime_dynamic.log'
        abi_log           = 'out/abi_dynamic.log'
        screenshots       = 'out/stage_00.png .. out/stage_10.png'
    }
}

function Stop-Fail([string]$message) {
    Write-Host "DYNAMIC TEST FAILED: $message" -ForegroundColor Red
    throw $message
}

function Find-VcVars {
    $fixed = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $fixed) { return $fixed }
    $vswhere = $null
    if (${env:ProgramFiles(x86)}) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    } elseif ($env:ProgramFiles) {
        $vswhere = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if ($vswhere -and (Test-Path -LiteralPath $vswhere)) {
        $install = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if ($LASTEXITCODE -eq 0 -and $install) {
            $candidate = Join-Path $install.Trim() 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }
    return $null
}

function Invoke-Tool([string]$what, [string]$command) {
    Write-Host "  $what" -ForegroundColor DarkGray
    $step = Join-Path $out 'dynamic_step.cmd'
    Set-Content -LiteralPath $step -Encoding ascii -Value "@echo off`r`n$command`r`nexit /b %errorlevel%`r`n"
    & cmd.exe /d /c $step 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail "$what exited with $LASTEXITCODE" }
    Remove-Item -LiteralPath $step -Force -ErrorAction SilentlyContinue
}

function Invoke-Run([string]$exe, [string[]]$arguments, [string]$workdir, [string]$log) {
    Write-Host "  $(Split-Path -Leaf $exe) $($arguments -join ' ')" -ForegroundColor DarkGray
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
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $sw.Stop()
    $lines = @()
    if ($stdout) { $lines += ($stdout -split "`r?`n" | Where-Object { $_ -ne '' }) }
    if ($stderr) { $lines += ($stderr -split "`r?`n" | Where-Object { $_ -ne '' }) }
    Set-Content -LiteralPath $log -Value ($lines -join "`r`n") -Encoding utf8
    foreach ($line in $lines) { Write-Host $line }
    return [ordered]@{
        exit    = $process.ExitCode
        wall_ms = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        text    = ($lines -join "`n")
    }
}

try {
    New-Item -ItemType Directory -Force -Path $out, $fixture | Out-Null
    $totalSw = [System.Diagnostics.Stopwatch]::StartNew()
    $vcvars = Find-VcVars
    if (-not $vcvars) { Stop-Fail 'vcvars64.bat not found (install the MSVC C++ workload)' }
    if (-not (Test-Path -LiteralPath $py)) { Stop-Fail "python venv missing: $py" }
    if (-not (Test-Path -LiteralPath (Join-Path $wgpu 'include\wgpu.h'))) {
        Stop-Fail "wgpu-native headers missing under $wgpu"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll'))) {
        Stop-Fail "wgpu_native.dll missing under $wgpu"
    }

    $header = Join-Path $fixture 'ui_data.h'
    if (-not (Test-Path -LiteralPath (Join-Path $page 'images\a.png')) -or
        -not (Test-Path -LiteralPath (Join-Path $page 'images\b.png'))) {
        Write-Host 'fixture: thumbnails' -ForegroundColor Cyan
        & $py (Join-Path $here 'make_images.py') $page | Write-Host
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'make_images.py failed' }
    }
    if ($Regenerate -or -not (Test-Path -LiteralPath $header)) {
        if (-not (Test-Path -LiteralPath $rustExe)) {
            Stop-Fail "frontend-rust.exe missing and $header does not exist"
        }
        Write-Host 'fixture: images + ui.json + ui_data.h' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $py (Join-Path $here 'make_images.py') $page | Write-Host
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'make_images.py failed' }
        Push-Location $here
        try {
            & $rustExe --html 'page\index.html' --css 'page\style.css' `
                --width 400 --height 300 --emit-ui 'page\ui.json' | Write-Host
            if ($LASTEXITCODE -ne 0) { Stop-Fail 'frontend-rust --emit-ui failed' }
            & $py $emitter --ui-json 'page\ui.json' `
                --out $header --meta (Join-Path $fixture 'ui_meta.json') | Write-Host
            if ($LASTEXITCODE -ne 0) { Stop-Fail 'emit_ui.py --ui-json failed' }
        } finally {
            Pop-Location
        }
        $sw.Stop()
        $report.timings_ms['fixture'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    }

    $vc = "call `"$vcvars`" >nul && "
    $croot = (Resolve-Path (Join-Path $here '..\..')).Path
    $inc = "/I`"$include`" /I`"$croot`" /I`"$fixture`" /I`"$(Join-Path $wgpu 'include')`""
    $cflags = "/nologo /O2 /W3 /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS /DWN_STATIC"
    $wgpuLib = Join-Path $wgpu 'lib\wgpu_native.dll.lib'
    $linkLibs = "`"$wgpuLib`" d3d11.lib dxgi.lib d3dcompiler.lib " +
        "windowscodecs.lib ole32.lib user32.lib gdi32.lib"

    # ------------------------------------------------------------ core test
    Write-Host 'compile+run: dynamic_core_test.exe (headless core)' -ForegroundColor Cyan
    $coreOut = Join-Path $out 'obj_core'
    New-Item -ItemType Directory -Force -Path $coreOut | Out-Null
    $coreExe = Join-Path $out 'dynamic_core_test.exe'
    $coreCmd = $vc + "cd /d `"$here`" && cl $cflags $inc " +
        "`"dynamic_core_test.c`" `"$(Join-Path $here '..\..\wn_core.c')`" " +
        "`"$(Join-Path $here '..\..\wn_common.c')`" /Fo:`"$coreOut\\`" /Fe:`"$coreExe`""
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Tool 'compile dynamic core test' $coreCmd
    $coreRun = Invoke-Run $coreExe @((Join-Path $out 'core_events.jsonl')) $here (Join-Path $out 'core_test.log')
    if ($coreRun.exit -ne 0) { Stop-Fail "dynamic_core_test.exe exited $($coreRun.exit)" }
    if ($coreRun.text -notmatch 'dynamic_core_test: PASS') { Stop-Fail 'core test did not print PASS' }
    $sw.Stop()
    $report.timings_ms['core_test'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report.checks['core_test'] = $true

    # --------------------------------------------------------- runtime PNGs
    Write-Host 'compile+run: runtime_dynamic.exe --dynamic (offscreen wgpu)' -ForegroundColor Cyan
    $runtimeOut = Join-Path $out 'obj_runtime'
    New-Item -ItemType Directory -Force -Path $runtimeOut | Out-Null
    $runtimeExe = Join-Path $out 'runtime_dynamic.exe'
    $runtimeCmd = $vc + "cd /d `"$here`" && cl $cflags $inc " +
        "`"$(Join-Path $here '..\..\runtime_r2.c')`" `"$(Join-Path $here '..\..\wn_core.c')`" " +
        "`"$(Join-Path $here '..\..\wn_common.c')`" /Fo:`"$runtimeOut\\`" /Fe:`"$runtimeExe`" " +
        "/link $linkLibs"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Tool 'compile runtime_dynamic' $runtimeCmd
    Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') -Destination $out -Force
    Get-ChildItem -LiteralPath $out -Filter 'stage_*.png' | Remove-Item -Force -ErrorAction SilentlyContinue
    $runtimeRun = Invoke-Run $runtimeExe @('--dynamic', (Join-Path $out 'stage')) $page (Join-Path $out 'runtime_dynamic.log')
    if ($runtimeRun.exit -ne 0) { Stop-Fail "runtime_dynamic.exe exited $($runtimeRun.exit)" }
    if ($runtimeRun.text -notmatch 'dynamic: stage=10 action=clear cards') {
        Stop-Fail 'runtime_dynamic did not reach stage 10'
    }
    $sw.Stop()
    $report.timings_ms['runtime_render'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report.checks['runtime_render'] = $true

    # -------------------------------------------------------------- ABI app
    Write-Host 'compile+run: dynamic.exe (wn_app dynamic UI, auto-close)' -ForegroundColor Cyan
    $abiOut = Join-Path $out 'obj_abi'
    New-Item -ItemType Directory -Force -Path $abiOut | Out-Null
    $abiExe = Join-Path $out 'dynamic.exe'
    $abiSources = @('wn_common.c', 'wn_core.c', 'renderer_wgpu.c', 'renderer_d3d11.c',
                    'host_win32.c', 'wn_view.c', 'wn_app.c', 'examples\dynamic.c') |
        ForEach-Object { "`"$(Join-Path $here ('..\..\' + $_))`"" }
    $abiCmd = $vc + "cd /d `"$here`" && cl $cflags $inc " + ($abiSources -join ' ') +
        " /Fo:`"$abiOut\\`" /Fe:`"$abiExe`" /link $linkLibs"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Tool 'compile dynamic ABI demo' $abiCmd
    $imageB = (Resolve-Path (Join-Path $page 'images\b.png')).Path
    $abiRun = Invoke-Run $abiExe @('--frames', '8', '--image-b', $imageB) $page (Join-Path $out 'abi_dynamic.log')
    if ($abiRun.exit -ne 0) { Stop-Fail "dynamic.exe exited $($abiRun.exit)" }
    foreach ($needle in @(
        'ABI 4',
        'dynamic: PASS',
        'clone(card,cards)=',
        'nodes=21',
        'set_text(Omega) -> WN_ERR_UNSUPPORTED',
        'set_image(text) -> WN_ERR_UNSUPPORTED',
        'remove(root) -> WN_ERR_INVALID_ARGUMENT',
        'clear_children -> nodes=9'
    )) {
        if ($abiRun.text -notmatch [regex]::Escape($needle)) {
            Stop-Fail "dynamic.exe output is missing '$needle'"
        }
    }
    if ($abiRun.text -notmatch 'exit=0 frames=8') {
        Stop-Fail 'dynamic.exe did not present 8 frames'
    }
    $sw.Stop()
    $report.timings_ms['abi_demo'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report.checks['abi_demo'] = $true

    if (-not $SkipRun) {
        Write-Host 'verify: core events + stage counts + pixels' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $py (Join-Path $here 'verify_dynamic.py') --dir $out |
            Tee-Object -FilePath (Join-Path $out 'verify_dynamic.log') | Write-Host
        $sw.Stop()
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'verify_dynamic.py failed' }
        $report.timings_ms['verify'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        $verifyPath = Join-Path $out 'dynamic_verify_report.json'
        if (-not (Test-Path -LiteralPath $verifyPath)) { Stop-Fail 'dynamic_verify_report.json is missing' }
        $verify = Get-Content -LiteralPath $verifyPath -Raw | ConvertFrom-Json
        $report['verify'] = [ordered]@{
            result = $verify.result
            passed = $verify.total - $verify.failed
            total  = $verify.total
            failed = $verify.failed
        }
        if ($verify.result -ne 'PASS') { Stop-Fail 'verify_dynamic reported FAIL' }
    }

    $totalSw.Stop()
    $report.timings_ms['total'] = [math]::Round($totalSw.Elapsed.TotalMilliseconds, 2)
    $report['finished'] = (Get-Date).ToString('o')
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'run_report.json') -Encoding utf8

    Write-Host ''
    Write-Host '========== src/runtime/c-r2 dynamic UI v0: PASS ==========' -ForegroundColor Green
    if ($report.verify) {
        Write-Host ("verify : {0}/{1} checks PASS" -f $report.verify.passed, $report.verify.total)
    }
    Write-Host ("screens: {0}" -f (Join-Path $out 'stage_00.png .. stage_10.png'))
    foreach ($key in $report.timings_ms.Keys) {
        Write-Host ("timing {0,-14}: {1,9:N2} ms" -f $key, $report.timings_ms[$key])
    }
    Write-Host '========================================================='
}
catch {
    Write-Host ''
    Write-Host "DYNAMIC TEST FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    $step = Join-Path $out 'dynamic_step.cmd'
    if (Test-Path -LiteralPath $step) { Remove-Item -LiteralPath $step -Force -ErrorAction SilentlyContinue }
    Pop-Location
}
