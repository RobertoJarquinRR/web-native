# benchmarks/pilot-media-library/stress/run_stress.ps1
#
# Thin wrapper around run_stress.py so the suite can be launched from PowerShell
# without remembering the venv path. All arguments are forwarded.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks\pilot-media-library\stress\run_stress.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File ...\run_stress.ps1 -Runs 7
#   powershell -NoProfile -ExecutionPolicy Bypass -File ...\run_stress.ps1 -Regenerate
#
# The Python script does the real work: generate fixtures, frontend-rust --emit-ui,
# emit_ui.py --ui-json, cl.exe against src/runtime/c-r2 (unmodified sources), then
# --report/--render/--wheel/--states measurements and stress_report.json.

[CmdletBinding()]
param(
    [int]$Runs = 7,
    [switch]$Regenerate,
    [switch]$SkipEmit,
    [switch]$SkipCompile,
    [switch]$SkipPilot,
    [string]$Only,
    [string]$Report
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))
$py = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'

if (-not (Test-Path -LiteralPath $py)) {
    Write-Host "missing Python venv: $py" -ForegroundColor Red
    exit 2
}

$argsList = @((Join-Path $here 'run_stress.py'), '--runs', "$Runs")
if ($Regenerate) { $argsList += '--regenerate' }
if ($SkipEmit) { $argsList += '--skip-emit' }
if ($SkipCompile) { $argsList += '--skip-compile' }
if ($SkipPilot) { $argsList += '--skip-pilot' }
if ($Only) { $argsList += @('--only', $Only) }
if ($Report) { $argsList += @('--report', $Report) }

& $py @argsList
exit $LASTEXITCODE
