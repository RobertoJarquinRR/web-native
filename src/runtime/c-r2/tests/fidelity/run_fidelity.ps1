# src\runtime\c-r2\tests\fidelity\run_fidelity.ps1
#
# E1 visual/layout fidelity acceptance:
#   1. emit the pre-layout tree with frontend-rust (--pilot: overflow/scroll),
#   2. emit_ui.py --ui-json -> out\emit\ui_data.h (headers gain the E1 fields
#      only for this page: UI_STYLE_EXTRAS),
#   3. compile+run fidelity_core_test.exe (/W4 /WX, headless core): bold vs
#      regular, text-align, line-height, margin:auto, scrollbar track/thumb
#      movement and the rounded focus ring (deterministic vertex geometry),
#   4. compile runtime_r2.exe and render out\render.png / out\render_wheel.png
#      as visual evidence.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\fidelity\run_fidelity.ps1
[CmdletBinding()]
param(
    [switch]$SkipRustBuild,
    [switch]$SkipRender
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $here

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))))
$out = Join-Path $here 'out'
$emitDir = Join-Path $out 'emit'
$runtime = Join-Path $repo 'src\runtime\c-r2'
$include = Join-Path $repo 'include'
$wgpu = Join-Path $repo 'third_party\wgpu-native'
$py = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'
$emitter = Join-Path $repo 'src\emitter\python\emit_ui.py'
$rustDir = Join-Path $repo 'src\frontend\rust'
$rustExe = Join-Path $rustDir 'target\release\frontend-rust.exe'
$report = [ordered]@{
    suite      = 'e1-fidelity'
    started    = (Get-Date).ToString('o')
    timings_ms = [ordered]@{}
}

New-Item -ItemType Directory -Force -Path $out, $emitDir | Out-Null

function Stop-Fail([string]$message) {
    Write-Host "FIDELITY FAILED: $message" -ForegroundColor Red
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
    $step = Join-Path $out 'fidelity_step.cmd'
    Set-Content -LiteralPath $step -Encoding ascii -Value "@echo off`r`n$command`r`nexit /b %errorlevel%`r`n"
    & cmd.exe /d /c $step | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail "$what exited with $LASTEXITCODE" }
    Remove-Item -LiteralPath $step -Force -ErrorAction SilentlyContinue
}

# wgpu prints adapter notes to stderr; capture both streams so a warning is not
# promoted to a PowerShell error record.
function Invoke-Runtime([string]$title, [string]$exe, [string[]]$arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($arguments | ForEach-Object {
        if ("$_" -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' '
    $psi.WorkingDirectory = $out
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    foreach ($line in @(($stdout + $stderr) -split "`r?`n")) {
        if ($line -ne '') { Write-Host "  $line" }
    }
    if ($process.ExitCode -ne 0) { Stop-Fail "$title exited with $($process.ExitCode)" }
}

try {
    foreach ($required in @($py, $emitter, (Join-Path $runtime 'wn_core.c'),
                            (Join-Path $runtime 'wn_common.c'),
                            (Join-Path $here 'page\index.html'))) {
        if (-not (Test-Path -LiteralPath $required)) { Stop-Fail "missing $required" }
    }

    if (-not $SkipRustBuild) {
        Write-Host 'fidelity: cargo build --release' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        Push-Location $rustDir
        try {
            $rustOut = & cargo build --release --quiet 2>&1
            $rustOut | Write-Host
            if ($LASTEXITCODE -ne 0) { Stop-Fail 'cargo build --release failed' }
        } finally {
            Pop-Location
        }
        $sw.Stop()
        $report.timings_ms['rust_build'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    }
    if (-not (Test-Path -LiteralPath $rustExe)) { Stop-Fail "missing $rustExe" }

    Write-Host 'fidelity: frontend-rust --emit-ui (480x560)' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $rustExe --html 'page\index.html' --width 480 --height 560 `
        --emit-ui 'out\ui.json' --pilot | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'frontend-rust --emit-ui failed' }
    $sw.Stop()
    $report.timings_ms['rust_emit'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)

    Write-Host 'fidelity: emit_ui.py --ui-json -> out\emit\ui_data.h' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $py $emitter --ui-json (Join-Path $out 'ui.json') `
        --out (Join-Path $emitDir 'ui_data.h') `
        --meta (Join-Path $emitDir 'ui_meta.json') | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'emit_ui.py --ui-json failed' }
    $sw.Stop()
    $report.timings_ms['emit'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)

    $header = Get-Content -LiteralPath (Join-Path $emitDir 'ui_data.h') -Raw
    if ($header -notmatch '#define UI_STYLE_EXTRAS 1') {
        Stop-Fail 'header is missing UI_STYLE_EXTRAS'
    }
    if ($header -match 'text_align' -and $header -notmatch 'int text_align;') {
        Stop-Fail 'header has text_align data but no UiNode field'
    }

    Write-Host 'fidelity: legacy emitter guard (old fixtures stay byte-identical)' -ForegroundColor Cyan
    $legacyHeader = Join-Path $out 'legacy_input.h'
    & $py $emitter --ui-json (Join-Path $repo 'src\runtime\c-r2\tests\input\page\ui.json') `
        --out $legacyHeader --meta (Join-Path $out 'legacy_input_meta.json') | Out-Null
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'legacy emit_ui.py failed' }
    $cmp = & $py -c "import sys; a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read(); print('IDENTICAL' if a==b else 'DIFFERS')" `
        $legacyHeader (Join-Path $repo 'src\runtime\c-r2\tests\input\fixture\ui_data.h')
    if ($cmp -ne 'IDENTICAL') {
        Stop-Fail 'emitter output for tests/input changed (old fixtures must stay byte-identical)'
    }

    $vcvars = Find-VcVars
    if (-not $vcvars) { Stop-Fail 'vcvars64.bat not found' }
    $inc = "/I`"$include`" /I`"$runtime`" /I`"$emitDir`""
    $runtimeInc = "$inc /I`"$(Join-Path $wgpu 'include')`""
    $cflags = '/nologo /O2 /W4 /WX /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS /DWN_STATIC'

    foreach ($required in @((Join-Path $runtime 'runtime_r2.c'))) {
        if (-not $SkipRender -and -not (Test-Path -LiteralPath $required)) {
            Stop-Fail "missing $required"
        }
    }

    Write-Host 'fidelity: compile+run fidelity_core_test.exe (headless, /W4 /WX)' -ForegroundColor Cyan
    $coreOut = Join-Path $out 'obj_core'
    New-Item -ItemType Directory -Force -Path $coreOut | Out-Null
    $coreExe = Join-Path $out 'fidelity_core_test.exe'
    $coreCmd = "call `"$vcvars`" >nul && cd /d `"$out`" && cl $cflags $inc " +
        "`"$(Join-Path $here 'fidelity_core_test.c')`" `"$(Join-Path $runtime 'wn_core.c')`" " +
        "`"$(Join-Path $runtime 'wn_common.c')`" /Fo:`"$coreOut\\`" /Fe:`"$coreExe`""
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Command 'compile fidelity core test' $coreCmd
    $coreRun = & $coreExe 2>&1
    $coreRun | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail "fidelity_core_test.exe exited $LASTEXITCODE" }
    $coreLine = @($coreRun | Where-Object { $_ -match '^fidelity_core_test: ' })
    if ($coreLine.Count -eq 0 -or $coreLine[-1] -notmatch 'PASS') {
        Stop-Fail 'fidelity core test did not print PASS'
    }
    if ($coreLine[-1] -match '\((\d+) checks\)') {
        $report.checks = [int]$Matches[1]
    }
    $sw.Stop()
    $report.timings_ms['core_test'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)

    if (-not $SkipRender) {
        Write-Host 'fidelity: compile runtime + offscreen renders' -ForegroundColor Cyan
        $runtimeOut = Join-Path $out 'obj_runtime'
        New-Item -ItemType Directory -Force -Path $runtimeOut | Out-Null
        $runtimeExe = Join-Path $out 'runtime_fidelity.exe'
        $runtimeCmd = "call `"$vcvars`" >nul && cd /d `"$out`" && cl $cflags $runtimeInc " +
            "`"$(Join-Path $runtime 'runtime_r2.c')`" `"$(Join-Path $runtime 'wn_core.c')`" " +
            "`"$(Join-Path $runtime 'wn_common.c')`" /Fo:`"$runtimeOut\\`" /Fe:`"$runtimeExe`" " +
            "/link `"$(Join-Path $wgpu 'lib\wgpu_native.dll.lib')`" " +
            "windowscodecs.lib ole32.lib user32.lib gdi32.lib"
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        Invoke-Command 'compile runtime' $runtimeCmd
        $sw.Stop()
        $report.timings_ms['compile_runtime'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') `
            -Destination $out -Force

        Invoke-Runtime 'render' $runtimeExe @('--render', '480', '560',
            (Join-Path $out 'render.png'))
        Invoke-Runtime 'render wheel' $runtimeExe @('--render', '480', '560',
            (Join-Path $out 'render_wheel.png'), '--wheel', '100', '350', '150')
    }

    $report['finished'] = (Get-Date).ToString('o')
    $report['artifacts'] = [ordered]@{
        ui_json      = 'out/ui.json'
        header       = 'out/emit/ui_data.h'
        core_test    = 'out/fidelity_core_test.exe'
        render       = 'out/render.png'
        render_wheel = 'out/render_wheel.png'
    }
    [System.IO.File]::WriteAllText((Join-Path $out 'run_report.json'),
        ($report | ConvertTo-Json -Depth 8),
        (New-Object System.Text.UTF8Encoding($false)))

    Write-Host ''
    Write-Host '========== E1 fidelity: PASS ==========' -ForegroundColor Green
    Write-Host ("core checks: {0}" -f $report.checks)
    foreach ($key in $report.timings_ms.Keys) {
        Write-Host ("timing {0,-16}: {1,9:N2} ms" -f $key, $report.timings_ms[$key])
    }
    Write-Host '=======================================' -ForegroundColor Green
}
catch {
    Write-Host ''
    Write-Host "FIDELITY FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    $step = Join-Path $out 'fidelity_step.cmd'
    if (Test-Path -LiteralPath $step) { Remove-Item -LiteralPath $step -Force -ErrorAction SilentlyContinue }
    Pop-Location
}
