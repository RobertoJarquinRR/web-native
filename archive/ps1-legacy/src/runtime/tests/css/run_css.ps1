# src\runtime\c-r2\tests\css\run_css.ps1
#
# CSS core v1 acceptance (rounded solid backgrounds, uniform border, first
# box-shadow, rounded textured image):
#   1. generate the deterministic 2x2 quadrant poster,
#   2. emit ui_data.h for the styled card, the square control and the
#      border_style:none fixture (emit_ui.py --ui-json),
#   3. compile the reference runtime twice (/W4 /WX) with each header,
#   4. offscreen render 400x240 (`runtime_r2.exe --render`) + layout report,
#   5. verify_css.py: header fields + exact pixels, including a byte-identical
#      comparison of the control against the pre-CSS reference PNG.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\css\run_css.ps1
[CmdletBinding()]
param(
    [switch]$SkipCompile
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $here

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))))
$out = Join-Path $here 'out'
$page = Join-Path $here 'page'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$py = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'
$emitter = Join-Path $repo 'src\emitter\python\emit_ui.py'
$runtime = Join-Path $repo 'src\runtime\c-r2'
$wgpu = Join-Path $repo 'third_party\wgpu-native'
$report = [ordered]@{
    suite      = 'css-core-v1'
    started    = (Get-Date).ToString('o')
    timings_ms = [ordered]@{}
}

function Stop-Fail([string]$message) {
    Write-Host "CSS FAILED: $message" -ForegroundColor Red
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
    $step = Join-Path $out 'css_step.cmd'
    Set-Content -LiteralPath $step -Encoding ascii -Value "@echo off`r`n$command`r`nexit /b %errorlevel%`r`n"
    & cmd.exe /d /c $step | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail "$what exited with $LASTEXITCODE" }
    Remove-Item -LiteralPath $step -Force -ErrorAction SilentlyContinue
}

function Invoke-Runtime([string]$title, [string]$exe, [string[]]$arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($arguments | ForEach-Object {
        if ("$_" -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' '
    $psi.WorkingDirectory = $page
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

    Write-Host 'css: deterministic poster' -ForegroundColor Cyan
    & $py (Join-Path $here 'make_images.py') | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'make_images.py failed' }

    foreach ($name in @('card', 'control', 'none')) {
        Write-Host "css: emit $name ui_data.h" -ForegroundColor Cyan
        $emitDir = Join-Path $out "emit\$name"
        New-Item -ItemType Directory -Force -Path $emitDir | Out-Null
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $py $emitter --ui-json (Join-Path $page "$name.json") `
            --out (Join-Path $emitDir 'ui_data.h') `
            --meta (Join-Path $emitDir 'ui_meta.json') | Write-Host
        $sw.Stop()
        if ($LASTEXITCODE -ne 0) { Stop-Fail "emit_ui.py --ui-json $name failed" }
        $report.timings_ms["emit_$name"] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    }

    Write-Host 'css: --compare regression (old-format + new-format self compare)' -ForegroundColor Cyan
    & $py $emitter --compare (Join-Path $here 'fixture\legacy_ui_data.h') `
        (Join-Path $here 'fixture\legacy_ui_data.h') | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'legacy header compare failed' }
    & $py $emitter --compare (Join-Path $out 'emit\card\ui_data.h') `
        (Join-Path $out 'emit\card\ui_data.h') | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'new header compare failed' }

    if (-not $SkipCompile) {
        $vcvars = Find-VcVars
        if (-not $vcvars) { Stop-Fail 'vcvars64.bat not found' }
        foreach ($name in @('card', 'control')) {
            $buildDir = Join-Path $out "build\$name"
            New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
            $emitDir = Join-Path $out "emit\$name"
            Write-Host "css: compile runtime_css_$name.exe (/W4 /WX)" -ForegroundColor Cyan
            $compile = "call `"$vcvars`" >nul && cd /d `"$buildDir`" && cl /nologo /O2 /W4 /WX " +
                "/std:c17 /TC /D_CRT_SECURE_NO_WARNINGS /DWN_STATIC " +
                "`"$(Join-Path $runtime 'runtime_r2.c')`" `"$(Join-Path $runtime 'wn_core.c')`" " +
                "`"$(Join-Path $runtime 'wn_common.c')`" `"$(Join-Path $runtime 'wn_view.c')`" " +
                "`"$(Join-Path $runtime 'wn_app.c')`" `"$(Join-Path $runtime 'renderer_wgpu.c')`" " +
                "`"$(Join-Path $runtime 'renderer_d3d11.c')`" `"$(Join-Path $runtime 'host_win32.c')`" " +
                "/I`"$(Join-Path $repo 'include')`" /I`"$runtime`" /I`"$(Join-Path $wgpu 'include')`" " +
                "/I`"$emitDir`" /Fo:`"$buildDir\\`" /Fe:`"$buildDir\runtime_css_$name.exe`" " +
                "/link `"$(Join-Path $wgpu 'lib\wgpu_native.dll.lib')`" d3d11.lib dxgi.lib d3dcompiler.lib windowscodecs.lib ole32.lib user32.lib gdi32.lib"
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            Invoke-Command "compile $name" $compile
            $sw.Stop()
            $report.timings_ms["compile_$name"] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
            Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') `
                -Destination (Join-Path $buildDir 'wgpu_native.dll') -Force
        }
    }

    $cardExe = Join-Path $out 'build\card\runtime_css_card.exe'
    $controlExe = Join-Path $out 'build\control\runtime_css_control.exe'
    foreach ($exe in @($cardExe, $controlExe)) {
        if (-not (Test-Path -LiteralPath $exe)) { Stop-Fail "missing $exe (run without -SkipCompile)" }
    }

    Write-Host 'css: layout report + offscreen renders (400x240)' -ForegroundColor Cyan
    # The pixel goldens (and the pre-CSS control) were captured with the wgpu
    # renderer. Pin it here: this suite checks CSS semantics, not a backend.
    $env:WN_RENDERER = 'wgpu'
    Invoke-Runtime 'layout_card' $cardExe @('--report', (Join-Path $out 'layout_card.json'),
        '--sizes', '400x240') | Out-Null
    Invoke-Runtime 'render_card' $cardExe @('--render', '400', '240',
        (Join-Path $out 'render_card.png')) | Out-Null
    Invoke-Runtime 'render_control' $controlExe @('--render', '400', '240',
        (Join-Path $out 'render_control.png')) | Out-Null
    Remove-Item Env:\WN_RENDERER -ErrorAction SilentlyContinue

    Write-Host 'css: verify fields + exact pixels' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $py (Join-Path $here 'verify_css.py') --dir $out --page $page | Write-Host
    $sw.Stop()
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'verify_css.py failed' }
    $report.timings_ms['verify'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)

    $verify = Get-Content -LiteralPath (Join-Path $out 'css_verify_report.json') -Raw | ConvertFrom-Json
    $report['verify'] = [ordered]@{
        ok     = $verify.ok
        passed = $verify.total_checks - $verify.failed_checks
        total  = $verify.total_checks
        failed = $verify.failed_checks
    }
    $report['artifacts'] = [ordered]@{
        render_card    = 'out/render_card.png'
        render_control = 'out/render_control.png'
        layout         = 'out/layout_card.json'
        verify         = 'out/css_verify_report.json'
        legacy_ref     = 'expected/control_legacy.png'
    }
    $report['finished'] = (Get-Date).ToString('o')
    [System.IO.File]::WriteAllText((Join-Path $out 'run_report.json'),
        ($report | ConvertTo-Json -Depth 8),
        (New-Object System.Text.UTF8Encoding($false)))

    Write-Host ''
    Write-Host '========== css core v1: PASS ==========' -ForegroundColor Green
    Write-Host ("verify : {0}/{1} checks" -f $report.verify.passed, $report.verify.total)
    foreach ($key in $report.timings_ms.Keys) {
        Write-Host ("timing {0,-14}: {1,9:N2} ms" -f $key, $report.timings_ms[$key])
    }
    Write-Host '=======================================' -ForegroundColor Green
}
catch {
    Write-Host ''
    Write-Host "CSS FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    $step = Join-Path $out 'css_step.cmd'
    if (Test-Path -LiteralPath $step) { Remove-Item -LiteralPath $step -Force -ErrorAction SilentlyContinue }
    Pop-Location
}
