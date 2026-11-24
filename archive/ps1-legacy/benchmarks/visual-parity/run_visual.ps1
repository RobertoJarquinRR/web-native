<#
.SYNOPSIS
  Run the Chromium-vs-native visual parity harness (dev-only).

.DESCRIPTION
  Captures one reference PNG per fixture with Playwright's pinned Chromium,
  renders the native offscreen PNG, compares both and writes report.json /
  report.md plus side-by-side and diff images under benchmarks/visual-parity/out.

  This script is benchmark tooling. It does not build or link anything into the
  runtime and it never captures a window.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks\visual-parity\run_visual.ps1

.EXAMPLE
  powershell -File benchmarks\visual-parity\run_visual.ps1 -Fixtures flex-page,media-library -EnforceGate
#>
[CmdletBinding()]
param(
    [string[]]$Fixtures = @(),
    [string]$OutDir = "",
    [switch]$SkipChromium,
    [switch]$SkipNative,
    [switch]$RebuildNative,
    [switch]$AllowPartial,
    [switch]$EnforceGate,
    [switch]$NoArchive
)

$ErrorActionPreference = 'Continue'
try {
    $OutputEncoding = [System.Text.Encoding]::UTF8
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
}
catch { }
$here = $PSScriptRoot
$repo = Split-Path -Parent (Split-Path -Parent $here)
$fixturesJson = Join-Path $here 'fixtures.json'
$py = Join-Path $here '.venv\Scripts\python.exe'

function Stop-Harness {
    param([string]$Message, [int]$Code = 1)
    Write-Host ""
    Write-Host "HARNESS ERROR: $Message" -ForegroundColor Red
    exit $Code
}

function Invoke-Tool {
    param(
        [string]$Label,
        [string[]]$ToolArgs,
        [string]$LogBase
    )
    $stdoutPath = "$LogBase.out.txt"
    $stderrPath = "$LogBase.err.txt"
    $quoted = $ToolArgs | ForEach-Object {
        $arg = [string]$_
        if ($arg -match '[\s"]') { '"' + ($arg -replace '"', '\"') + '"' } else { $arg }
    }
    $argString = ($quoted -join ' ')
    try {
        $process = Start-Process -FilePath $py -ArgumentList $argString -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    }
    catch {
        return [pscustomobject]@{
            ExitCode = 127
            StdOut   = ''
            StdErr   = "$Label could not start: $($_.Exception.Message)"
        }
    }
    $outText = ''
    $errText = ''
    if (Test-Path -LiteralPath $stdoutPath) {
        $outText = [string](Get-Content -LiteralPath $stdoutPath -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)
    }
    if (Test-Path -LiteralPath $stderrPath) {
        $errText = [string](Get-Content -LiteralPath $stderrPath -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)
    }
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        StdOut   = $outText
        StdErr   = $errText
    }
}

function Show-ToolFailure {
    param([string]$Label, $Result)
    Write-Host "  $Label FAILED (exit $($Result.ExitCode))" -ForegroundColor Red
    $lines = @()
    if ($Result.StdErr) {
        $lines += ($Result.StdErr -split "`r?`n" | Where-Object { $_.Trim() -ne '' })
    }
    if ($Result.StdOut) {
        $lines += ($Result.StdOut -split "`r?`n" | Where-Object { $_.Trim() -ne '' })
    }
    $tail = @($lines | Select-Object -Last 14)
    if ($tail.Count -gt 0) {
        foreach ($line in $tail) { Write-Host "    $line" -ForegroundColor DarkRed }
    }
}

# ---------------------------------------------------------------- preflight
if (-not (Test-Path -LiteralPath $fixturesJson)) {
    Stop-Harness "fixtures.json not found: $fixturesJson"
}
if (-not (Test-Path -LiteralPath $py)) {
    Stop-Harness @"
Harness venv not found: $py

Create it (dev-only, never the runtime venv):
  py -3.12 -m venv "$here\.venv"
  "$py" -m pip install -r "$here\requirements.txt"
  "$py" -m playwright install chromium-headless-shell
"@
}

$probeOutput = & $py -c "import PIL, numpy, playwright; print('harness-deps-ok')" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host ($probeOutput | Out-String) -ForegroundColor DarkRed
    Stop-Harness @"
Harness dependencies are missing in $py

Install them with:
  "$py" -m pip install -r "$here\requirements.txt"
  "$py" -m playwright install chromium-headless-shell
"@
}

try {
    $config = Get-Content -LiteralPath $fixturesJson -Raw | ConvertFrom-Json
}
catch {
    Stop-Harness "cannot parse fixtures.json: $($_.Exception.Message)"
}
$allFixtures = @($config.fixtures)
$allNames = @($allFixtures | ForEach-Object { [string]$_.name })
if ($allNames.Count -eq 0) {
    Stop-Harness "fixtures.json declares no fixtures"
}
$requested = @()
foreach ($item in $Fixtures) {
    foreach ($part in ([string]$item -split ',')) {
        $trimmed = $part.Trim()
        if ($trimmed -ne '') { $requested += $trimmed }
    }
}
if ($requested.Count -eq 0) {
    $names = $allNames
}
else {
    $names = $requested
}
foreach ($name in $names) {
    if ($allNames -notcontains $name) {
        Stop-Harness "unknown fixture '$name'. Known fixtures: $($allNames -join ', ')"
    }
}

if (-not $OutDir) {
    $OutDir = Join-Path $here 'out'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $repo $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logDir = Join-Path $OutDir "logs\$runStamp"
foreach ($dir in @(
        $OutDir,
        (Join-Path $OutDir 'captures'),
        (Join-Path $OutDir 'meta'),
        (Join-Path $OutDir 'diff'),
        (Join-Path $OutDir 'reports'),
        $logDir
    )) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

Write-Host "visual-parity: $($names.Count) fixture(s) -> $OutDir" -ForegroundColor Cyan
Write-Host "python: $py" -ForegroundColor DarkGray

# ---------------------------------------------------------------- checks
if (-not $SkipChromium) {
    Write-Host "[check] Playwright Chromium + fixture fonts" -ForegroundColor Cyan
    $checkArgs = @(
        (Join-Path $here 'tools\capture_chromium.py'),
        '--check',
        '--fixtures-json', $fixturesJson,
        '--out', $OutDir,
        '--meta', (Join-Path $OutDir 'meta\chromium.json'),
        '--fixtures'
    ) + $names
    $check = Invoke-Tool -Label 'chromium check' -ToolArgs $checkArgs -LogBase (Join-Path $logDir 'chromium-check')
    if ($check.ExitCode -ne 0) {
        Show-ToolFailure 'chromium check' $check
        $message = @"
Chromium/Playwright is not usable.

Fix it with:
  "$py" -m playwright install chromium-headless-shell

Then rerun:
  powershell -NoProfile -ExecutionPolicy Bypass -File "$here\run_visual.ps1"
"@
        if ($AllowPartial) {
            Write-Host $message -ForegroundColor Yellow
        }
        else {
            Stop-Harness $message
        }
    }
    else {
        try {
            $checkJson = $check.StdOut | ConvertFrom-Json
            $browserVersion = $checkJson.browser.version
            $browserBuild = $checkJson.browser.playwright_browser_build
            Write-Host "  chromium $browserVersion (build $browserBuild)" -ForegroundColor DarkGray
        }
        catch {
            Write-Host "  chromium check ok" -ForegroundColor DarkGray
        }
    }
}
else {
    Write-Host "[check] Chromium skipped; reusing existing captures" -ForegroundColor Yellow
}

if (-not $SkipNative) {
    Write-Host "[check] native renderer prerequisites" -ForegroundColor Cyan
    $nativeCheckArgs = @(
        (Join-Path $here 'tools\render_native.py'),
        '--check',
        '--fixtures-json', $fixturesJson,
        '--out', $OutDir,
        '--meta', (Join-Path $OutDir 'meta\native.json'),
        '--fixtures'
    ) + $names
    $nativeCheck = Invoke-Tool -Label 'native check' -ToolArgs $nativeCheckArgs -LogBase (Join-Path $logDir 'native-check')
    if ($nativeCheck.ExitCode -ne 0) {
        Show-ToolFailure 'native check' $nativeCheck
        $message = @"
Native renderer prerequisites are missing.

Every fixture is compiled by the engine: src\runtime\\build.ps1 -Html <html>
-Css <css> -Width W -Height H -OutDir out\build\r2-<fixture>, then rendered with
runtime_r2.exe --render. This needs MSVC, frontend-rust and emit_ui.py.
If the build breaks, fix the error; do not fall back to a stale binary.
"@
        if ($AllowPartial) {
            Write-Host $message -ForegroundColor Yellow
        }
        else {
            Stop-Harness $message
        }
    }
    else {
        Write-Host "  native prerequisites ok" -ForegroundColor DarkGray
    }
}
else {
    Write-Host "[check] native render skipped; reusing existing captures" -ForegroundColor Yellow
}

# ---------------------------------------------------------------- captures
$captureFailures = @()
$nativeFailures = @()
for ($index = 0; $index -lt $names.Count; $index++) {
    $name = $names[$index]
    $fixture = @($allFixtures | Where-Object { [string]$_.name -eq $name })[0]
    $width = [int]$fixture.viewport.width
    $height = [int]$fixture.viewport.height
    Write-Host ("[{0}/{1}] {2} ({3}x{4})" -f ($index + 1), $names.Count, $name, $width, $height) -ForegroundColor Cyan

    if (-not $SkipChromium) {
        $captureArgs = @(
            (Join-Path $here 'tools\capture_chromium.py'),
            '--fixture', $name,
            '--fixtures-json', $fixturesJson,
            '--out', (Join-Path $OutDir "captures\$name.chromium.png"),
            '--meta', (Join-Path $OutDir "meta\$name.chromium.json")
        )
        $capture = Invoke-Tool -Label 'chromium capture' -ToolArgs $captureArgs -LogBase (Join-Path $logDir "$name.chromium")
        if ($capture.ExitCode -ne 0) {
            Show-ToolFailure "$name chromium capture" $capture
            $captureFailures += $name
        }
        else {
            Write-Host "  chromium: ok" -ForegroundColor Green
        }
    }

    if (-not $SkipNative) {
        $nativeArgs = @(
            (Join-Path $here 'tools\render_native.py'),
            '--fixture', $name,
            '--fixtures-json', $fixturesJson,
            '--out', (Join-Path $OutDir "captures\$name.native.png"),
            '--meta', (Join-Path $OutDir "meta\$name.native.json")
        )
        if ($RebuildNative) { $nativeArgs += '--rebuild' }
        $native = Invoke-Tool -Label 'native render' -ToolArgs $nativeArgs -LogBase (Join-Path $logDir "$name.native")
        if ($native.ExitCode -ne 0) {
            Show-ToolFailure "$name native render" $native
            $nativeFailures += $name
        }
        else {
            $lastLine = @($native.StdOut -split "`r?`n" | Where-Object { $_.Trim() -ne '' }) | Select-Object -Last 1
            Write-Host "  native: ok $lastLine" -ForegroundColor Green
        }
    }
}

# ---------------------------------------------------------------- compare
Write-Host "[compare] metrics, regions and reports" -ForegroundColor Cyan
$compareArgs = @(
    (Join-Path $here 'tools\compare.py'),
    '--fixtures-json', $fixturesJson,
    '--out', $OutDir,
    '--capture-dir', (Join-Path $OutDir 'captures'),
    '--meta-dir', (Join-Path $OutDir 'meta'),
    '--fixtures'
) + $names
$compare = Invoke-Tool -Label 'compare' -ToolArgs $compareArgs -LogBase (Join-Path $logDir 'compare')
if ($compare.StdOut) {
    foreach ($line in ($compare.StdOut -split "`r?`n" | Where-Object { $_.Trim() -ne '' })) {
        Write-Host "  $line" -ForegroundColor DarkGray
    }
}
if ($compare.ExitCode -gt 1) {
    Show-ToolFailure 'compare' $compare
    Stop-Harness "compare could not produce a report. See $($logDir)\compare.err.txt" 1
}

$reportPath = Join-Path $OutDir 'report.json'
if (-not (Test-Path -LiteralPath $reportPath)) {
    Show-ToolFailure 'compare' $compare
    Stop-Harness "compare finished but $reportPath is missing" 1
}
try {
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
}
catch {
    Stop-Harness "cannot parse $reportPath : $($_.Exception.Message)" 1
}

# ---------------------------------------------------------------- archive
if (-not $NoArchive) {
    $archive = Join-Path $here "reports\$runStamp"
    New-Item -ItemType Directory -Force -Path $archive | Out-Null
    Copy-Item -LiteralPath (Join-Path $OutDir 'report.json') -Destination $archive -Force
    Copy-Item -LiteralPath (Join-Path $OutDir 'report.md') -Destination $archive -Force
    Get-ChildItem -LiteralPath (Join-Path $OutDir 'reports') -Filter '*.report.*' |
    Copy-Item -Destination $archive -Force
    $archiveNote = @"
Visual parity run: $runStamp

report.json / report.md y los reportes por fixture conservan las metricas.
Las capturas y diff PNG no se archivan (ni se commitean): viven en
..\..\out\captures y ..\..\out\diff de la corrida.
"@
    Set-Content -LiteralPath (Join-Path $archive 'README.txt') -Value $archiveNote -Encoding UTF8
    Write-Host "archive: $archive" -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- summary
Write-Host ""
Write-Host "=== visual parity summary ===" -ForegroundColor Cyan
Write-Host ("{0,-16} {1,-10} {2,9} {3,9} {4,9} {5}" -f 'fixture', 'size', 'SSIM', 'diff %', 'MAE', 'status')
foreach ($item in @($report.fixtures)) {
    $size = "$($item.fixture.viewport.width)x$($item.fixture.viewport.height)"
    $ssim = if ($item.global) { [math]::Round([double]$item.global.metrics.ssim, 5) } else { '-' }
    $diff = if ($item.global) { [math]::Round([double]$item.global.metrics.pixel_diff_pct, 3) } else { '-' }
    $mae = if ($item.global) { [math]::Round([double]$item.global.metrics.mae, 3) } else { '-' }
    $status = [string]$item.status
    $line = "{0,-16} {1,-10} {2,9} {3,9} {4,9} {5}" -f $item.fixture.name, $size, $ssim, $diff, $mae, $status
    switch ($status) {
        'PASS' { Write-Host $line -ForegroundColor Green }
        'FAIL' { Write-Host $line -ForegroundColor Red }
        'INFO' { Write-Host $line -ForegroundColor Cyan }
        default { Write-Host $line -ForegroundColor Red }
    }
}
Write-Host ""
Write-Host "report: $(Join-Path $OutDir 'report.md')" -ForegroundColor Gray

$summary = $report.summary
$hardErrors = @($captureFailures) + @($nativeFailures)
$partial = $false
if ($summary.error -gt 0) {
    Write-Host "ERROR: $($summary.error) fixture(s) could not be compared." -ForegroundColor Red
    if (-not $AllowPartial) {
        exit 1
    }
    $partial = $true
}
if ($hardErrors.Count -gt 0) {
    Write-Host "WARNING: tool failures: $($hardErrors -join ', ')" -ForegroundColor Yellow
    if (-not $AllowPartial) {
        exit 1
    }
    $partial = $true
}
if ($summary.failed -gt 0) {
    Write-Host "GATE FAIL: $($summary.gate_failures -join ', ')" -ForegroundColor Yellow
    if ($EnforceGate) {
        exit 1
    }
}
if ($summary.would_fail -and $summary.would_fail.Count -gt 0) {
    Write-Host "INFO fixtures that would fail the gate: $($summary.would_fail -join ', ')" -ForegroundColor DarkYellow
}
if ($partial) {
    Write-Host "HARNESS FINISHED WITH ERRORS (AllowPartial)" -ForegroundColor Yellow
}
elseif ($summary.failed -gt 0) {
    Write-Host "HARNESS OK (measurements complete; gate failures reported above)" -ForegroundColor Green
}
else {
    Write-Host "HARNESS OK" -ForegroundColor Green
}
exit 0
