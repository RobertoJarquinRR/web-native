param(
    [Parameter(Mandatory = $true)][string]$Html,
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Timings,
    [string]$Atlas,
    [switch]$StrictSubset
)

$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $dir 'target\release\frontend-rust.exe'

if (-not (Test-Path -LiteralPath $exe)) {
    throw "Binario no encontrado: $exe. Ejecuta build.ps1 primero."
}

# Las rutas relativas se resuelven contra el directorio actual (como el
# frontend Python); las absolutas se dejan intactas.
$htmlPath = if ([System.IO.Path]::IsPathRooted($Html)) { $Html } else { Join-Path (Get-Location).Path $Html }
$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Get-Location).Path $Out }

$argv = @('--html', $htmlPath, '--width', $Width, '--height', $Height, '--out', $outPath)
if ($Timings) {
    $timingsPath = if ([System.IO.Path]::IsPathRooted($Timings)) { $Timings } else { Join-Path (Get-Location).Path $Timings }
    $argv += @('--timings', $timingsPath)
}
if ($Atlas) {
    $atlasPath = if ([System.IO.Path]::IsPathRooted($Atlas)) { $Atlas } else { Join-Path (Get-Location).Path $Atlas }
    $argv += @('--atlas', $atlasPath)
}
if ($StrictSubset) {
    $argv += '--strict-subset'
}

& $exe @argv
exit $LASTEXITCODE
