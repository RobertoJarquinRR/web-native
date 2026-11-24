# src\runtime\c-r2\build_abi.ps1 - build + acceptance for the C ABI v0 layer.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build_abi.ps1
#
# Compiles the core/renderer/host/view/app split, the C and C++ header checks,
# the error-surface smoke test and the two demos, then runs:
#   standalone.exe --frames N
#   embedded.exe   --frames N --input-demo --resize-demo
# Both must exit 0 with the D3D12 backend and the expected events.
#
# This script never runs cargo. If build\ui_data.h is missing it invokes the
# existing build.ps1 with -SkipRustBuild -SkipParityCheck -SkipRun to reuse the
# already-built frontend executable and Python emitter.
[CmdletBinding()]
param(
    [int]$Frames = 20,
    [switch]$SkipRun
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $root))
$out = Join-Path $root 'build_abi'
$obj = Join-Path $out 'obj'
$r2build = Join-Path $root 'build'
$wgpu = Join-Path $repo 'third_party\wgpu-native'
$include = Join-Path $repo 'include'
$r2BuildScript = Join-Path $root 'build.ps1'
$logPath = Join-Path $out 'abi_build_log.txt'
$reportPath = Join-Path $out 'abi_report.json'
$vcvars = $null
$report = [ordered]@{
    layer      = 'c-r2-abi'
    started    = (Get-Date).ToString('o')
    frames     = $Frames
    timings_ms = [ordered]@{}
    steps      = [ordered]@{}
    demos      = [ordered]@{}
    checks     = [ordered]@{}
}

function Stop-Fail([string]$message) {
    Write-Host "ABI BUILD FAILED: $message" -ForegroundColor Red
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

function Invoke-Tool([string]$what, [string]$command, [string]$logFile) {
    Write-Host "  $what" -ForegroundColor DarkGray
    $scriptFile = Join-Path $out 'abi_step.cmd'
    $content = "@echo off`r`n$command`r`nexit /b %errorlevel%`r`n"
    Set-Content -LiteralPath $scriptFile -Value $content -Encoding ascii
    & cmd.exe /d /c $scriptFile 2>&1 | Tee-Object -FilePath $logFile -Append | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Stop-Fail "$what exited with $LASTEXITCODE"
    }
    Remove-Item -LiteralPath $scriptFile -Force -ErrorAction SilentlyContinue
}

function Invoke-Demo([string]$title, [string]$exe, [string[]]$arguments) {
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
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $sw.Stop()
    Add-Content -LiteralPath $logPath -Value "`r`n### $title`r`n> $exe $($arguments -join ' ')" -Encoding utf8
    $lines = @()
    if ($stdout) { $lines += ($stdout -split "`r?`n" | Where-Object { $_ -ne '' }) }
    if ($stderr) { $lines += ($stderr -split "`r?`n" | Where-Object { $_ -ne '' }) }
    foreach ($line in $lines) {
        Write-Host $line
        Add-Content -LiteralPath $logPath -Value ([string]$line) -Encoding utf8
    }
    return [ordered]@{
        exit       = $process.ExitCode
        wall_ms    = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
        output     = $lines
        text       = ($lines -join "`n")
    }
}

function Get-MatchDouble([string]$text, [string]$pattern) {
    if ($text -match $pattern) { return [double]$matches[1] }
    return $null
}

try {
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    New-Item -ItemType Directory -Force -Path $obj | Out-Null
    Set-Content -LiteralPath $logPath -Value "c-r2 ABI build log`r`n" -Encoding utf8
    $totalSw = [System.Diagnostics.Stopwatch]::StartNew()

    $vcvars = Find-VcVars
    if (-not $vcvars) { Stop-Fail 'vcvars64.bat not found (install the MSVC C++ workload)' }
    foreach ($required in @(
        (Join-Path $include 'webnative.h'),
        (Join-Path $wgpu 'include\wgpu.h'),
        (Join-Path $wgpu 'bin\wgpu_native.dll'),
        (Join-Path $wgpu 'lib\wgpu_native.dll.lib'),
        (Join-Path $root 'wn_core.c'),
        (Join-Path $root 'wn_view.c'),
        (Join-Path $root 'wn_app.c'),
        (Join-Path $root 'examples\standalone.c'),
        (Join-Path $root 'examples\embedded.c')
    )) {
        if (-not (Test-Path -LiteralPath $required)) { Stop-Fail "missing required path: $required" }
    }

    if (-not (Test-Path -LiteralPath (Join-Path $r2build 'ui_data.h'))) {
        $rustExe = Join-Path $repo 'src\frontend\rust\target\release\frontend-rust.exe'
        $py = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'
        if (Test-Path -LiteralPath $rustExe) {
            Write-Host 'ui_data.h missing: emitting with build.ps1 -SkipRustBuild (no cargo)' -ForegroundColor Cyan
            & powershell -NoProfile -ExecutionPolicy Bypass -File $r2BuildScript `
                -SkipRustBuild -SkipParityCheck -SkipRun
        } elseif (Test-Path -LiteralPath $py) {
            Write-Host 'ui_data.h missing and frontend-rust.exe absent: emitting with the Python oracle tree' -ForegroundColor Cyan
            & powershell -NoProfile -ExecutionPolicy Bypass -File $r2BuildScript `
                -UsePythonTree -SkipRun
        } else {
            Stop-Fail 'build/ui_data.h is missing and no emitter is available (need frontend-rust.exe or the Python venv)'
        }
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'build.ps1 (emit only) failed' }
    }

    # ------------------------------------------------------------ header grep
    $headerText = Get-Content -LiteralPath (Join-Path $include 'webnative.h') -Raw
    $forbidden = 'WGPU|wgpu|ID3D|D3D12|D3D11|HWND|HINSTANCE|IDXGI|Win32|windows\.h'
    if ($headerText -match $forbidden) {
        Stop-Fail "include/webnative.h contains forbidden GPU/OS type tokens ($($matches[0]))"
    }
    Write-Host 'grep proof: include/webnative.h has no WGPU/D3D/HWND/HINSTANCE tokens' -ForegroundColor Green
    $report.checks['header_no_gpu_win32'] = $true

    $vc = "call `"$vcvars`" >nul && "
    $inc = "/I`"$root`" /I`"$include`" /I`"$wgpu\include`" /I`"$r2build`""
    $cflags = "/nologo /O2 /W3 /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS /DWN_STATIC"
    $abiSources = @(
        'wn_common.c', 'wn_core.c', 'renderer_wgpu.c',
        'host_win32.c', 'wn_view.c', 'wn_app.c'
    ) | ForEach-Object { Join-Path $root $_ }
    $abiObjects = @(
        'wn_common.obj', 'wn_core.obj', 'renderer_wgpu.obj',
        'host_win32.obj', 'wn_view.obj', 'wn_app.obj'
    ) | ForEach-Object { Join-Path $obj $_ }
    $wgpuLib = Join-Path $wgpu 'lib\wgpu_native.dll.lib'
    $linkLibs = "`"$wgpuLib`" windowscodecs.lib ole32.lib user32.lib gdi32.lib"

    # ---------------------------------------------------------------- compile
    $compileSw = [System.Diagnostics.Stopwatch]::StartNew()
    $abiList = ($abiSources | ForEach-Object { "`"$_`"" }) -join ' '
    $compileCmd = $vc + "cd /d `"$root`" && cl $cflags $inc /c $abiList /Fo:`"$obj\\`""
    Invoke-Tool 'compile ABI objects (cl /O2 /TC)' $compileCmd $logPath
    $compileSw.Stop()
    $report.timings_ms['compile_abi'] = [math]::Round($compileSw.Elapsed.TotalMilliseconds, 2)

    # ------------------------------------------------------- header C / C++
    $headerSw = [System.Diagnostics.Stopwatch]::StartNew()
    $headerC = Join-Path $root 'examples\abi_header_check.c'
    $headerCpp = Join-Path $root 'examples\abi_header_check.cpp'
    $headerCmd = $vc + "cd /d `"$out`" && cl /nologo /W4 /std:c17 /TC /DWN_STATIC $inc `"$headerC`" /Fe:abi_header_check_c.exe /link /SUBSYSTEM:CONSOLE" +
        " && cl /nologo /W4 /std:c++17 /TP /DWN_STATIC $inc `"$headerCpp`" /Fe:abi_header_check_cpp.exe /link /SUBSYSTEM:CONSOLE" +
        " && abi_header_check_c.exe && abi_header_check_cpp.exe"
    Invoke-Tool 'compile+run header checks (C17 and C++17)' $headerCmd $logPath
    $headerSw.Stop()
    $report.timings_ms['header_checks'] = [math]::Round($headerSw.Elapsed.TotalMilliseconds, 2)
    $report.checks['header_c'] = $true
    $report.checks['header_cpp'] = $true

    # ----------------------------------------------------------------- smoke
    $smokeSw = [System.Diagnostics.Stopwatch]::StartNew()
    $smokeSrc = Join-Path $root 'examples\abi_smoke.c'
    $smokeCmd = $vc + "cd /d `"$out`" && cl /nologo /O2 /W3 /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS /DWN_STATIC $inc `"$smokeSrc`" /Fe:abi_smoke.exe /link " +
        (($abiObjects | ForEach-Object { "`"$_`"" }) -join ' ') +
        " $linkLibs && abi_smoke.exe"
    Invoke-Tool 'compile+run abi_smoke' $smokeCmd $logPath
    $smokeSw.Stop()
    $report.timings_ms['abi_smoke'] = [math]::Round($smokeSw.Elapsed.TotalMilliseconds, 2)
    $report.checks['abi_smoke'] = $true

    # ----------------------------------------------------------------- demos
    $demoSw = [System.Diagnostics.Stopwatch]::StartNew()
    $standaloneSrc = Join-Path $root 'examples\standalone.c'
    $embeddedSrc = Join-Path $root 'examples\embedded.c'
    $demoCmd = $vc + "cd /d `"$root`" && cl $cflags $inc /c `"$standaloneSrc`" /Fo:`"$obj\standalone.obj`"" +
        " && cl $cflags $inc /c `"$embeddedSrc`" /Fo:`"$obj\embedded.obj`"" +
        " && link /nologo /OUT:`"$out\standalone.exe`" `"$obj\standalone.obj`" " +
        (($abiObjects | ForEach-Object { "`"$_`"" }) -join ' ') + " $linkLibs" +
        " && link /nologo /OUT:`"$out\embedded.exe`" `"$obj\embedded.obj`" " +
        (($abiObjects | ForEach-Object { "`"$_`"" }) -join ' ') + " $linkLibs"
    Invoke-Tool 'compile+link standalone.exe and embedded.exe' $demoCmd $logPath
    Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') `
        -Destination (Join-Path $out 'wgpu_native.dll') -Force
    $demoSw.Stop()
    $report.timings_ms['compile_demos'] = [math]::Round($demoSw.Elapsed.TotalMilliseconds, 2)

    if (-not $SkipRun) {
        # ----------------------------------------------------------- standalone
        Write-Host 'run: standalone.exe --frames N' -ForegroundColor Cyan
        $standalone = Invoke-Demo 'standalone' (Join-Path $out 'standalone.exe') @('--frames', "$Frames")
        if ($standalone.exit -ne 0) { Stop-Fail "standalone.exe exited $($standalone.exit)" }
        if ($standalone.text -notmatch 'backend=D3D12') { Stop-Fail 'standalone did not report the D3D12 adapter' }
        if ($standalone.text -notmatch "exit=0 frames=$Frames") { Stop-Fail "standalone did not present $Frames frames" }
        if ($standalone.text -notmatch 'startup_ms=') { Stop-Fail 'standalone did not report startup_ms' }
        $report.demos['standalone'] = [ordered]@{
            exit          = $standalone.exit
            wall_ms       = $standalone.wall_ms
            backend       = 'D3D12'
            frames        = $Frames
            startup_ms    = Get-MatchDouble $standalone.text 'startup_ms=([0-9.]+)'
            frame_avg_ms  = Get-MatchDouble $standalone.text 'frame_avg_ms=([0-9.]+)'
            cpu_avg_ms    = Get-MatchDouble $standalone.text 'cpu_frame_avg_ms=([0-9.]+)'
            pointer_ups   = Get-MatchDouble $standalone.text 'pointer_ups=([0-9]+)'
        }
        Write-Host ("standalone: PASS (frames={0}, startup={1} ms, frame={2} ms)" -f `
            $Frames, $report.demos['standalone'].startup_ms, $report.demos['standalone'].frame_avg_ms) -ForegroundColor Green

        # ------------------------------------------------------------- embedded
        Write-Host 'run: embedded.exe --frames N --input-demo --resize-demo' -ForegroundColor Cyan
        $embedded = Invoke-Demo 'embedded' (Join-Path $out 'embedded.exe') @('--frames', "$Frames", '--input-demo', '--resize-demo')
        if ($embedded.exit -ne 0) { Stop-Fail "embedded.exe exited $($embedded.exit)" }
        if ($embedded.text -notmatch 'backend=D3D12') { Stop-Fail 'embedded did not report the D3D12 adapter' }
        if ($embedded.text -notmatch "exit=0 frames=$Frames") { Stop-Fail "embedded did not present $Frames frames" }
        foreach ($needle in @('WM_SIZE', 'WM_MOUSEMOVE', 'WM_LBUTTONDOWN', 'WM_LBUTTONUP',
                              'resize-demo', 'set_text(text[1], "Texto") -> WN_OK')) {
            if ($embedded.text -notmatch [regex]::Escape($needle)) {
                Stop-Fail "embedded output is missing '$needle'"
            }
        }
        $embeddedPointerUps = Get-MatchDouble $embedded.text 'pointer_ups=([0-9]+)'
        if ($embeddedPointerUps -lt 1) { Stop-Fail 'embedded synthetic click did not select a node' }
        $report.demos['embedded'] = [ordered]@{
            exit         = $embedded.exit
            wall_ms      = $embedded.wall_ms
            backend      = 'D3D12'
            frames       = $Frames
            startup_ms   = Get-MatchDouble $embedded.text 'startup_ms=([0-9.]+)'
            frame_avg_ms = Get-MatchDouble $embedded.text 'frame_avg_ms=([0-9.]+)'
            cpu_avg_ms   = Get-MatchDouble $embedded.text 'cpu_frame_avg_ms=([0-9.]+)'
            pointer_ups  = $embeddedPointerUps
            events       = @($embedded.output | Where-Object {
                "$_" -match '^embedded: (WM_|event |resize-demo|set_)'
            })
        }
        Write-Host ("embedded: PASS (frames={0}, startup={1} ms, frame={2} ms, pointer_ups={3})" -f `
            $Frames, $report.demos['embedded'].startup_ms, $report.demos['embedded'].frame_avg_ms,
            $embeddedPointerUps) -ForegroundColor Green
    }

    $totalSw.Stop()
    $report.timings_ms['build_total'] = [math]::Round($totalSw.Elapsed.TotalMilliseconds, 2)
    $report['finished'] = (Get-Date).ToString('o')
    $report['artifacts'] = [ordered]@{
        standalone = 'build_abi/standalone.exe'
        embedded   = 'build_abi/embedded.exe'
        smoke      = 'build_abi/abi_smoke.exe'
        log        = 'build_abi/abi_build_log.txt'
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8

    Write-Host ''
    Write-Host '================ src/runtime/c-r2 ABI v0: PASS ================' -ForegroundColor Green
    Write-Host ("standalone: {0} ({1} bytes)" -f (Join-Path $out 'standalone.exe'),
        (Get-Item -LiteralPath (Join-Path $out 'standalone.exe')).Length)
    Write-Host ("embedded  : {0} ({1} bytes)" -f (Join-Path $out 'embedded.exe'),
        (Get-Item -LiteralPath (Join-Path $out 'embedded.exe')).Length)
    foreach ($key in $report.timings_ms.Keys) {
        Write-Host ("timing {0,-14}: {1,9:N2} ms" -f $key, $report.timings_ms[$key])
    }
    Write-Host '===============================================================' -ForegroundColor Green
}
catch {
    Write-Host ''
    Write-Host "ABI BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    $helper = Join-Path $out 'abi_step.cmd'
    if (Test-Path -LiteralPath $helper) { Remove-Item -LiteralPath $helper -Force -ErrorAction SilentlyContinue }
    Pop-Location
}
