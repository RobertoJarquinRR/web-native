# src\runtime\c-r2\build.ps1 - canonical R2 build + acceptance.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1
#
# Build-time Python (src\emitter\python\emit_ui.py, using the canonical
# src\frontend\python venv) produces build\ui_data.h; runtime_r2.exe contains
# the layout engine, Win32 loop, hit testing and wgpu-native renderer.
#
# Everything generated stays under src\runtime\c-r2\build\.
[CmdletBinding()]
param(
    [switch]$SkipEmit,
    [switch]$SkipRun
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root

# c-r2 -> runtime -> src -> repo root
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $root))
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$fixtureHtml = Join-Path $root 'page\index.html'
$fixtureCss = Join-Path $root 'page\style.css'
$runtimeC = Join-Path $root 'runtime_r2.c'
$verifyPy = Join-Path $root 'verify.py'
$emitter = Join-Path $repo 'src\emitter\python\emit_ui.py'
$py = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'
$wgpu = Join-Path $repo 'third_party\wgpu-native'
$report = [ordered]@{
    runtime    = 'c-r2'
    started    = (Get-Date).ToString('o')
    timings_ms = [ordered]@{}
    steps      = [ordered]@{}
}

function Stop-Fail([string]$message) {
    Write-Host "BUILD FAILED: $message" -ForegroundColor Red
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

function Invoke-Native([string]$what, [string]$command) {
    Write-Host "  $command" -ForegroundColor DarkGray
    $scriptFile = Join-Path $out 'build_step.cmd'
    $content = "@echo off`r`n$command`r`nexit /b %errorlevel%`r`n"
    Set-Content -LiteralPath $scriptFile -Value $content -Encoding ascii
    & cmd.exe /d /c $scriptFile | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Stop-Fail "$what exited with $LASTEXITCODE"
    }
    Remove-Item -LiteralPath $scriptFile -Force -ErrorAction SilentlyContinue
}

$script:logPath = Join-Path $out 'build_log.txt'
Set-Content -LiteralPath $script:logPath -Value "c-r2 build log`r`n" -Encoding utf8

function Invoke-Runtime([string]$title, [string[]]$arguments) {
    Write-Host "  runtime_r2.exe $($arguments -join ' ')" -ForegroundColor DarkGray
    Add-Content -LiteralPath $script:logPath -Value "`r`n### $title`r`n> runtime_r2.exe $($arguments -join ' ')" -Encoding utf8
    $exe = Join-Path $out 'runtime_r2.exe'
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
    $output = @()
    if ($stdout) { $output += ($stdout -split "`r?`n" | Where-Object { $_ -ne '' }) }
    if ($stderr) { $output += ($stderr -split "`r?`n" | Where-Object { $_ -ne '' }) }
    foreach ($line in $output) {
        Write-Host $line
        Add-Content -LiteralPath $script:logPath -Value ([string]$line) -Encoding utf8
    }
    if ($process.ExitCode -ne 0) {
        Stop-Fail "$title exited with $($process.ExitCode)"
    }
    return $output
}

try {
    $vcvars = Find-VcVars
    if (-not $vcvars) { Stop-Fail 'vcvars64.bat not found (install the MSVC C++ workload)' }
    foreach ($required in @($py, $emitter, $runtimeC, $verifyPy, $fixtureHtml, $fixtureCss)) {
        if (-not (Test-Path -LiteralPath $required)) { Stop-Fail "missing required path: $required" }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $wgpu 'include\wgpu.h'))) {
        Stop-Fail "wgpu-native headers not found under $wgpu"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll'))) {
        Stop-Fail "wgpu_native.dll not found under $wgpu"
    }

    if (-not $SkipEmit) {
        Write-Host 'emit: page/index.html + page/style.css -> build\ui_data.h + build\ui_meta.json' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $py $emitter --html $fixtureHtml --css $fixtureCss `
            --out (Join-Path $out 'ui_data.h') --meta (Join-Path $out 'ui_meta.json')
        $sw.Stop()
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'emit_ui.py failed' }
        $report.timings_ms['emit'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    }

    if (-not (Test-Path -LiteralPath (Join-Path $out 'ui_data.h'))) {
        Stop-Fail 'build\ui_data.h is missing (run without -SkipEmit first)'
    }

    Write-Host 'compile: cl /O2 /TC -> build\runtime_r2.exe' -ForegroundColor Cyan
    $compile = "cl /nologo /O2 /W3 /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS `"$runtimeC`" " +
               "/I`"$wgpu\include`" /I`"$out`" /Fo:`"$out\runtime_r2.obj`" /Fe:`"$out\runtime_r2.exe`" " +
               "/link `"$wgpu\lib\wgpu_native.dll.lib`" windowscodecs.lib ole32.lib user32.lib gdi32.lib"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Native 'compile' "call `"$vcvars`" >nul && cd /d `"$out`" && $compile"
    $sw.Stop()
    $report.timings_ms['compile'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report.steps['compile'] = $compile
    if (-not (Test-Path -LiteralPath (Join-Path $out 'runtime_r2.exe'))) {
        Stop-Fail 'runtime_r2.exe was not produced'
    }
    Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') `
        -Destination (Join-Path $out 'wgpu_native.dll') -Force

    if (-not $SkipRun) {
        Write-Host 'run: layout report (960/720/560)' -ForegroundColor Cyan
        $reportRun = Invoke-Runtime 'layout report' @('--report', 'layout_report.json',
            '--sizes', '960x640,720x640,560x640')

        Write-Host 'run: offscreen renders' -ForegroundColor Cyan
        $runs = [ordered]@{}
        foreach ($width in @(960, 720, 560)) {
            $output = Invoke-Runtime "render ${width}x640" @('--render', "$width", '640', "out_$width.png")
            $runs["$width"] = $output
        }

        Write-Host 'run: state frames' -ForegroundColor Cyan
        New-Item -ItemType Directory -Force -Path (Join-Path $out 'states') | Out-Null
        $statesRun = Invoke-Runtime 'state frames' @('--states', 'states/state')

        Write-Host 'run: window --frames 30 --resize-demo --input-demo' -ForegroundColor Cyan
        $windowRun = Invoke-Runtime 'window frames + resize + input' @('--frames', '30',
            '--resize-demo', '--input-demo')

        Write-Host 'verify: layout + pixels + states' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $py $verifyPy --dir $out | Tee-Object -FilePath $script:logPath -Append | Write-Host
        $sw.Stop()
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'verify.py failed' }
        $report.timings_ms['verify'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)

        $adapterLine = ($runs['960'] | Where-Object { "$_" -match '^adapter: backend=' } | Select-Object -First 1)
        $deviceLine = ($runs['960'] | Where-Object { "$_" -match '^adapter: device=' } | Select-Object -First 1)
        $report['adapter'] = [ordered]@{
            backend = "$adapterLine"
            device  = "$deviceLine"
        }
        $renderStats = [ordered]@{}
        foreach ($width in @(960, 720, 560)) {
            $line = ($runs["$width"] | Where-Object { "$_" -match '^render: ' } | Select-Object -First 1)
            $layoutMs = $null
            $frameMs = $null
            if ("$line" -match 'layout=([0-9.]+)ms frame=([0-9.]+)ms') {
                $layoutMs = [double]$matches[1]
                $frameMs = [double]$matches[2]
            }
            $renderStats["$width"] = [ordered]@{
                layout_ms = $layoutMs
                frame_ms  = $frameMs
                log       = "$line"
            }
        }
        $report['render'] = $renderStats

        $windowStats = [ordered]@{
            exit    = 0
            reflow  = @($windowRun | Where-Object { "$_" -match 'WM_SIZE/layout|resize-demo:|input-demo:' } | ForEach-Object { "$_" })
            summary = @($windowRun | Where-Object { "$_" -match '^window: ' } | ForEach-Object { "$_" })
        }
        if ("$($windowStats.summary -join ' ')" -match 'closed after (\d+) frame\(s\), init ([0-9.]+) ms, ([0-9.]+) ms/frame') {
            $windowStats.frames = [int]$matches[1]
            $windowStats.init_ms = [double]$matches[2]
            $windowStats.frame_avg_ms = [double]$matches[3]
        }
        $report['window'] = $windowStats

        $report['states'] = [ordered]@{
            normal   = 'states/state_normal.png'
            hover    = 'states/state_hover.png'
            press    = 'states/state_press.png'
            selected = 'states/state_selected.png'
            report   = 'states/state_states.json'
        }

        $verifyReport = Join-Path $out 'verify_report.json'
        if (Test-Path -LiteralPath $verifyReport) {
            $verify = Get-Content -LiteralPath $verifyReport -Raw | ConvertFrom-Json
            $report['verify'] = [ordered]@{
                ok     = $verify.ok
                passed = $verify.total_checks - $verify.failed_checks
                total  = $verify.total_checks
                failed = $verify.failed_checks
            }
        }
    }

    $exe = Get-Item -LiteralPath (Join-Path $out 'runtime_r2.exe')
    $report['exe'] = [ordered]@{ path = $exe.FullName; bytes = $exe.Length }
    $report['finished'] = (Get-Date).ToString('o')
    $reportJson = $report | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText((Join-Path $out 'build_report.json'), $reportJson,
        (New-Object System.Text.UTF8Encoding($false)))

    Write-Host ''
    Write-Host '================ src/runtime/c-r2: PASS ================' -ForegroundColor Green
    Write-Host ("exe    : {0} ({1} bytes)" -f $exe.FullName, $exe.Length)
    foreach ($key in $report.timings_ms.Keys) {
        Write-Host ("timing {0,-10}: {1,9:N2} ms" -f $key, $report.timings_ms[$key])
    }
    if ($report.verify) {
        Write-Host ("verify : {0}/{1} checks PASS" -f $report.verify.passed, $report.verify.total)
    }
    Write-Host '=======================================================' -ForegroundColor Green
}
catch {
    Write-Host ''
    Write-Host "R2 BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    $helper = Join-Path $out 'build_step.cmd'
    if (Test-Path -LiteralPath $helper) { Remove-Item -LiteralPath $helper -Force -ErrorAction SilentlyContinue }
    Pop-Location
}
