# src\runtime\c-r2\tests\d3d11\measure_cpu.ps1
#
# Measures idle and hover CPU cost of the same app window with both renderers.
# The machine used for the D3D11 acceptance has an Intel HD Graphics adapter
# that only exposes D3D11 (no D3D12), so the wgpu path runs on WARP. The script
# moves the pointer across the three cards of the built-in fixture for a fixed
# window and reports core-equivalents (process CPU time / wall time).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\d3d11\measure_cpu.ps1
[CmdletBinding()]
param(
    [int]$IdleMs = 2000,
    [int]$HoverMs = 4000
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $here

$runtime = Split-Path -Parent (Split-Path -Parent $here)
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe = Join-Path $runtime 'build_abi\standalone.exe'
$reportPath = Join-Path $out 'cpu_measure.json'

if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "missing $exe (run build_abi.ps1 first)" -ForegroundColor Red
    exit 1
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WnWin32 {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X, Y; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam);
}
'@

function Wait-Window($process, [int]$timeoutMs) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalMilliseconds -lt $timeoutMs) {
        $process.Refresh()
        if (-not $process.HasExited -and
            $process.MainWindowHandle -ne [IntPtr]::Zero -and
            $process.MainWindowTitle -eq 'webnative standalone (C ABI v0)') {
            return $process.MainWindowHandle
        }
        Start-Sleep -Milliseconds 50
    }
    return [IntPtr]::Zero
}

function Measure-Renderer([string]$name, [string]$renderer) {
    $env:WN_RENDERER = $renderer
    Remove-Item Env:\WN_CAPTURE -ErrorAction SilentlyContinue
    Remove-Item Env:\WN_CAPTURE_FRAME -ErrorAction SilentlyContinue

    $process = Start-Process -FilePath $exe -WorkingDirectory (Join-Path $runtime 'build_abi') `
        -PassThru -WindowStyle Normal
    $hwnd = Wait-Window $process 8000
    if ($hwnd -eq [IntPtr]::Zero) {
        try { $process.Kill() } catch { }
        throw "window not found for $name"
    }
    [WnWin32]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 1200

    $rect = New-Object WnWin32+RECT
    [WnWin32]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $client = New-Object WnWin32+RECT
    [WnWin32]::GetClientRect($hwnd, [ref]$client) | Out-Null
    $origin = New-Object WnWin32+POINT
    [WnWin32]::ClientToScreen($hwnd, [ref]$origin) | Out-Null

    $process.Refresh()
    $idleStartCpu = $process.TotalProcessorTime.TotalMilliseconds
    $idleWatch = [System.Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Milliseconds $IdleMs
    $idleWatch.Stop()
    $process.Refresh()
    $idleEndCpu = $process.TotalProcessorTime.TotalMilliseconds
    $idleCores = ($idleEndCpu - $idleStartCpu) / $idleWatch.Elapsed.TotalMilliseconds

    # Cards sit in the content column: alternate synthetic WM_MOUSEMOVE messages
    # over the three card headers and the sidebar. Posting to the HWND is the
    # same event path the real cursor uses (host_win32.c -> wn_view_event) and
    # does not depend on window focus, Z-order or a physical cursor.
    # The built-in fixture's interactive nodes are the four sidebar nav items;
    # the cards themselves are non-interactive (action=0).
    $points = @(
        @(40, 61), @(40, 105), @(40, 150), @(40, 194),
        @(60, 61), @(60, 105), @(60, 150), @(60, 194)
    )
    $process.Refresh()
    $hoverStartCpu = $process.TotalProcessorTime.TotalMilliseconds
    $hoverWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $index = 0
    while ($hoverWatch.Elapsed.TotalMilliseconds -lt $HoverMs) {
        $point = $points[$index % $points.Count]
        $wparam = [IntPtr]::Zero
        $lparam = [IntPtr](($point[1] -band 0xFFFF) -shl 16 -bor ($point[0] -band 0xFFFF))
        [WnWin32]::PostMessage($hwnd, 0x0200, $wparam, $lparam) | Out-Null
        $index += 1
        Start-Sleep -Milliseconds 8
    }
    Start-Sleep -Milliseconds 120  # let the queued repaints drain before reading
    $hoverWatch.Stop()
    $process.Refresh()
    $hoverEndCpu = $process.TotalProcessorTime.TotalMilliseconds
    $hoverCores = ($hoverEndCpu - $hoverStartCpu) / $hoverWatch.Elapsed.TotalMilliseconds

    [WnWin32]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not $process.WaitForExit(4000)) {
        try { $process.Kill() } catch { }
    }

    return [ordered]@{
        renderer              = $renderer
        idle_ms               = [math]::Round($idleWatch.Elapsed.TotalMilliseconds, 2)
        idle_cpu_ms           = [math]::Round($idleEndCpu - $idleStartCpu, 2)
        idle_cores            = [math]::Round($idleCores, 4)
        hover_ms              = [math]::Round($hoverWatch.Elapsed.TotalMilliseconds, 2)
        hover_cpu_ms          = [math]::Round($hoverEndCpu - $hoverStartCpu, 2)
        hover_cores           = [math]::Round($hoverCores, 4)
        pointer_moves         = $index
        window_client         = @($client.Right, $client.Bottom)
        process_exit          = $process.ExitCode
    }
}

try {
    $d3d11 = Measure-Renderer 'd3d11' 'd3d11'
    $wgpu = Measure-Renderer 'wgpu' 'wgpu'
    $payload = [ordered]@{
        suite  = 'd3d11-cpu'
        host   = $env:COMPUTERNAME
        date   = (Get-Date).ToString('o')
        idle_ms = $IdleMs
        hover_ms = $HoverMs
        runs   = [ordered]@{ d3d11 = $d3d11; wgpu = $wgpu }
        ratio  = [ordered]@{
            idle_cores = $(if ($wgpu.idle_cores -gt 0) {
                [math]::Round($d3d11.idle_cores / $wgpu.idle_cores, 3) } else { $null })
            hover_cores = $(if ($wgpu.hover_cores -gt 0) {
                [math]::Round($d3d11.hover_cores / $wgpu.hover_cores, 3) } else { $null })
        }
    }
    $payload | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8

    Write-Host ''
    Write-Host '================ D3D11 vs wgpu CPU ================' -ForegroundColor Green
    foreach ($run in @($d3d11, $wgpu)) {
        Write-Host ("{0,-5} idle={1,7:N3} cores  hover={2,7:N3} cores  ({3} moves)" -f `
            $run.renderer, $run.idle_cores, $run.hover_cores, $run.pointer_moves)
    }
    if ($null -ne $payload.ratio.hover_cores) {
        Write-Host ("hover ratio d3d11/wgpu = {0:N3}" -f $payload.ratio.hover_cores)
    }
    Write-Host '===================================================' -ForegroundColor Green
    Write-Host "report: $reportPath"
}
catch {
    Write-Host "CPU MEASURE FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
