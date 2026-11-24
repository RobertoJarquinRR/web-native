# exp15-r2-core build + acceptance.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1
#
# Build-time Python (emit_ui.py) produces ui_data.h; runtime_r2.exe contains
# the layout engine, Win32 loop, hit testing and wgpu-native renderer.
[CmdletBinding()]
param(
    [switch]$SkipEmit,
    [switch]$SkipRun
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root

$repo = (Resolve-Path (Join-Path $root '..\..')).Path
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$wgpu = Join-Path $repo 'third_party\wgpu-native'
$report = [ordered]@{
    experiment = 'exp15-r2-core'
    started    = (Get-Date).ToString('o')
    timings_ms = [ordered]@{}
    steps      = [ordered]@{}
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
    Remove-Item -LiteralPath $scriptFile -Force -ErrorAction SilentlyContinue
}

$script:logPath = Join-Path $root 'build_log.txt'
Set-Content -LiteralPath $script:logPath -Value "exp15-r2-core build log`r`n" -Encoding utf8

function Invoke-Runtime([string]$title, [string[]]$arguments) {
    Write-Host "  runtime_r2.exe $($arguments -join ' ')" -ForegroundColor DarkGray
    Add-Content -LiteralPath $script:logPath -Value "`r`n### $title`r`n> runtime_r2.exe $($arguments -join ' ')" -Encoding utf8
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path -LiteralPath '.\runtime_r2.exe').Path
    $psi.Arguments = ($arguments | ForEach-Object {
        if ("$_" -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' '
    $psi.WorkingDirectory = $root
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
    if (-not (Test-Path -LiteralPath $vcvars)) { Stop-Fail "vcvars64.bat not found" }
    if (-not (Test-Path -LiteralPath "$wgpu\include\wgpu.h")) { Stop-Fail "wgpu-native headers not found" }

    $py = Join-Path $root '.venv\Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $py)) {
        Write-Host 'setup: creating .venv' -ForegroundColor Cyan
        & py -3.12 -m venv .venv
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'python -m venv failed' }
        & $py -m pip install --quiet --disable-pip-version-check html5lib tinycss2 cssselect2 pillow uharfbuzz freetype-py
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'pip install failed' }
    }

    if (-not $SkipEmit) {
        Write-Host 'emit: page/index.html + page/style.css -> ui_data.h + ui_meta.json' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $py emit_ui.py
        $sw.Stop()
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'emit_ui.py failed' }
        $report.timings_ms['emit'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    }

    Write-Host 'compile: cl /O2 /TC -> runtime_r2.exe' -ForegroundColor Cyan
    $compile = "cl /nologo /O2 /W3 /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS runtime_r2.c /I`"$wgpu\include`" /Fe:runtime_r2.exe " +
               "/link `"$wgpu\lib\wgpu_native.dll.lib`" windowscodecs.lib ole32.lib user32.lib gdi32.lib"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-Native 'compile' "call `"$vcvars`" >nul && $compile"
    $sw.Stop()
    $report.timings_ms['compile'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report.steps['compile'] = $compile
    if (-not (Test-Path -LiteralPath 'runtime_r2.exe')) { Stop-Fail 'runtime_r2.exe was not produced' }
    Copy-Item -LiteralPath "$wgpu\bin\wgpu_native.dll" -Destination 'wgpu_native.dll' -Force

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
        New-Item -ItemType Directory -Force -Path 'states' | Out-Null
        $statesRun = Invoke-Runtime 'state frames' @('--states', 'states/state')

        Write-Host 'run: window --frames 30 --resize-demo' -ForegroundColor Cyan
        $windowRun = Invoke-Runtime 'window frames + resize + input' @('--frames', '30',
            '--resize-demo', '--input-demo')

        Write-Host 'verify: layout + pixels + states' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $py verify.py | Tee-Object -FilePath $script:logPath -Append | Write-Host
        $sw.Stop()
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'verify.py failed' }
        $report.timings_ms['verify'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)

        $adapterLine = ($runs["960"] | Where-Object { "$_" -match '^adapter: backend=' } | Select-Object -First 1)
        $deviceLine = ($runs["960"] | Where-Object { "$_" -match '^adapter: device=' } | Select-Object -First 1)
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
            exit       = 0
            reflow     = @($windowRun | Where-Object { "$_" -match 'WM_SIZE/layout|resize-demo:|input-demo:' } | ForEach-Object { "$_" })
            summary    = @($windowRun | Where-Object { "$_" -match '^window: ' } | ForEach-Object { "$_" })
        }
        if ("$($windowStats.summary -join ' ')" -match 'closed after (\d+) frame\(s\), init ([0-9.]+) ms, ([0-9.]+) ms/frame') {
            $windowStats.frames = [int]$matches[1]
            $windowStats.init_ms = [double]$matches[2]
            $windowStats.frame_avg_ms = [double]$matches[3]
        }
        $report['window'] = $windowStats

        $states = [ordered]@{
            normal   = 'states/state_normal.png'
            hover    = 'states/state_hover.png'
            press    = 'states/state_press.png'
            selected = 'states/state_selected.png'
            report   = 'states/state_states.json'
        }
        $report['states'] = $states
    }

    $exe = Get-Item -LiteralPath 'runtime_r2.exe'
    $report['exe'] = [ordered]@{ path = $exe.FullName; bytes = $exe.Length }
    $report['finished'] = (Get-Date).ToString('o')
    $reportJson = $report | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText((Join-Path $root 'build_report.json'), $reportJson,
        (New-Object System.Text.UTF8Encoding($false)))

    Write-Host ''
    Write-Host '================ exp15-r2-core: PASS ================' -ForegroundColor Green
    Write-Host ("exe    : {0} ({1} bytes)" -f $exe.FullName, $exe.Length)
    foreach ($key in $report.timings_ms.Keys) {
        Write-Host ("timing {0,-10}: {1,9:N2} ms" -f $key, $report.timings_ms[$key])
    }
    Write-Host '====================================================' -ForegroundColor Green
}
finally {
    $helper = Join-Path $root 'build_step.cmd'
    if (Test-Path -LiteralPath $helper) { Remove-Item -LiteralPath $helper -Force }
    Pop-Location
}
