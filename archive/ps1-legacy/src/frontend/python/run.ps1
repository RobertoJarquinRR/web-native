param(
    [Parameter(Mandatory = $true)][string]$Html,
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Timings
)
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$py = Join-Path $dir '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $py)) {
    throw "Python venv not found at $py; run: py -3.12 -m venv .venv && .venv\Scripts\python.exe -m pip install -r requirements.txt"
}
$argv = @(
    (Join-Path $dir 'frontend_only.py'),
    '--html', $Html,
    '--width', $Width,
    '--height', $Height,
    '--out', $Out
)
if ($Timings) { $argv += @('--timings', $Timings) }
& $py @argv
exit $LASTEXITCODE
