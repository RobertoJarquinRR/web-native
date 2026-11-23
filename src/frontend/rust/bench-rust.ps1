param(
    [int]$Runs = 10,
    [int]$Warmup = 2
)

# Benchmark local de paridad: mide wall time (PowerShell + proceso + pipeline)
# y timings internos por etapa. Deja los artefactos en
# benchmarks/frontend-parity/results/rust/ y verifica el IR contra el golden.

$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$parity = [System.IO.Path]::GetFullPath((Join-Path $dir '..\..\..\benchmarks\frontend-parity'))
$repo = [System.IO.Path]::GetFullPath((Join-Path $parity '..\..'))
$results = Join-Path $parity 'results\rust'
$goldenDir = Join-Path $parity 'golden'
$runPs1 = Join-Path $dir 'run.ps1'
$py = '.\src\frontend\python\.venv\Scripts\python.exe'
$verify = Join-Path $parity 'tools\verify_ir.py'
$verifyText = Join-Path $dir 'verify_text.py'
$pyRoot = 'python'
$exp16 = Join-Path $repo 'experiments\16-text-atlas-c'

New-Item -ItemType Directory -Force -Path $results | Out-Null

$fixtures = @(
    @{ name = 'contract'; html = (Join-Path $parity 'fixtures\contract\input.html'); width = 400; height = 300; kind = 'rect' },
    @{ name = 'flex-page'; html = (Join-Path $parity 'fixtures\flex-page\index.html'); width = 480; height = 320; kind = 'rect' },
    @{ name = 'text-card'; html = (Join-Path $parity 'fixtures\text-card\input.html'); width = 400; height = 240; kind = 'text'; atlas = (Join-Path $results 'text-card.atlas.bin') }
)

function Invoke-Frontend {
    param($fixture, $out, $timings)
    $cmd = @(
        'powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', $runPs1,
        '-Html', $fixture.html,
        '-Width', $fixture.width,
        '-Height', $fixture.height,
        '-Out', $out,
        '-Timings', $timings
    )
    if ($fixture.kind -eq 'text') {
        $cmd += @('-Atlas', $fixture.atlas)
    }
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $null = & $cmd[0] $cmd[1..($cmd.Length - 1)]
    $watch.Stop()
    if ($LASTEXITCODE -ne 0) {
        throw ("run.ps1 fallo para {0} (exit {1})" -f $fixture.name, $LASTEXITCODE)
    }
    return $watch.Elapsed.TotalMilliseconds
}

function Get-Median {
    param([double[]]$values)
    $sorted = $values | Sort-Object
    $count = $sorted.Count
    if ($count -eq 0) { return 0 }
    if ($count % 2 -eq 1) { return $sorted[[int](($count - 1) / 2)] }
    return ($sorted[$count / 2 - 1] + $sorted[$count / 2]) / 2.0
}

$report = [ordered]@{
    frontend  = 'rust'
    runs      = $Runs
    warmup    = $Warmup
    built_at  = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    fixtures  = [ordered]@{}
}
$allPass = $true

foreach ($fixture in $fixtures) {
    $out = Join-Path $results ($fixture.name + '.ir.json')
    $timingsPath = Join-Path $results ($fixture.name + '.timings.json')

    for ($i = 0; $i -lt $Warmup; $i++) {
        $null = Invoke-Frontend -fixture $fixture -out $out -timings $timingsPath
    }

    $wall = New-Object System.Collections.Generic.List[double]
    $stages = @{ html = @(); css = @(); cascade = @(); fonts = @(); layout = @(); text = @(); emit = @(); pipeline = @() }
    $ops = 0
    $rss = 0

    for ($i = 0; $i -lt $Runs; $i++) {
        $wall.Add((Invoke-Frontend -fixture $fixture -out $out -timings $timingsPath))
        $internal = Get-Content -LiteralPath $timingsPath -Raw | ConvertFrom-Json
        foreach ($stage in @('html', 'css', 'cascade', 'fonts', 'layout', 'text', 'emit', 'pipeline')) {
            $stages[$stage] += [double]$internal.$stage
        }
        $ops = [int]$internal.ops
        $rss = [int]$internal.peak_rss_kb
    }

    if ($fixture.kind -eq 'text') {
        $verifyOutput = & $pyRoot $verifyText --ir $out --atlas $fixture.atlas `
            --atlas-json (Join-Path $results 'text-card.atlas.json') `
            --reference (Join-Path $exp16 'display_list.json') `
            --reference-atlas (Join-Path $exp16 'atlas.bin') `
            --report (Join-Path $results 'text-card.verify.json') 2>&1
    } else {
        $verifyOutput = & $py $verify (Join-Path $goldenDir ($fixture.name + '.ir.json')) $out 2>&1
    }
    $verified = ($LASTEXITCODE -eq 0)
    if (-not $verified) { $allPass = $false }

    $stageReport = [ordered]@{}
    foreach ($stage in @('html', 'css', 'cascade', 'fonts', 'layout', 'text', 'emit', 'pipeline')) {
        $stageReport[$stage] = [math]::Round((Get-Median $stages[$stage]), 3)
    }
    $stageReport['ops'] = $ops
    $stageReport['peak_rss_kb'] = $rss

    $report.fixtures[$fixture.name] = [ordered]@{
        status   = if ($verified) { 'PASS' } else { 'FAIL' }
        verify   = ($verifyOutput -join ' ')
        wall_ms  = [ordered]@{
            median = [math]::Round((Get-Median $wall.ToArray()), 3)
            min    = [math]::Round(($wall | Measure-Object -Minimum).Minimum, 3)
            max    = [math]::Round(($wall | Measure-Object -Maximum).Maximum, 3)
            samples = @($wall | ForEach-Object { [math]::Round($_, 3) })
        }
        internal_timings_ms = $stageReport
        ir       = $out
        timings  = $timingsPath
    }

    Write-Host ("{0,-10} {1,-4} wall median={2,7:N1} ms  min={3,7:N1}  max={4,7:N1}  ops={5}" -f `
        $fixture.name, $report.fixtures[$fixture.name].status, `
        $report.fixtures[$fixture.name].wall_ms.median, `
        $report.fixtures[$fixture.name].wall_ms.min, `
        $report.fixtures[$fixture.name].wall_ms.max, $ops)
    Write-Host ("           interno: html={0} css={1} cascade={2} layout={3} text={4} emit={5} pipeline={6} rss={7} KB" -f `
        $stageReport['html'], $stageReport['css'], $stageReport['cascade'], `
        $stageReport['layout'], $stageReport['text'], $stageReport['emit'], `
        $stageReport['pipeline'], $rss)
}

$json = $report | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText(
    (Join-Path $results 'benchmark.json'),
    $json,
    (New-Object System.Text.UTF8Encoding($false))
)

if (-not $allPass) {
    Write-Host 'PARITY FAIL' -ForegroundColor Red
    exit 1
}
Write-Host 'PASS (3 fixtures)' -ForegroundColor Green
exit 0
