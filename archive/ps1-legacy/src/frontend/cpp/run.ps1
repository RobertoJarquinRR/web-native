# CLI wrapper used by benchmarks/frontend-parity/tools/bench.py.
#
#   run.ps1 -Html <path> -Width <W> -Height <H> -Out <ir.json> [-Timings <timings.json>]
#
# Relative paths are resolved against this script's directory, like the C#
# frontend.  Only .NET APIs are used (no PowerShell cmdlets) so the wrapper does
# not autoload modules and inflate the harness wall time.
param(
    [Parameter(Mandatory = $true)][string]$Html,
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Timings
)

$ErrorActionPreference = 'Stop'
$dir = [System.IO.Path]::GetDirectoryName($MyInvocation.MyCommand.Path)

function Resolve-FromScript([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return [System.IO.Path]::Combine($dir, $Path)
}

$exe = [System.IO.Path]::Combine($dir, 'build', 'frontend-cpp.exe')
if (-not [System.IO.File]::Exists($exe)) {
    throw "frontend-cpp.exe not found at $exe; run build.ps1 first"
}

$arguments = @(
    '--html', (Resolve-FromScript $Html),
    '--width', [string]$Width,
    '--height', [string]$Height,
    '--out', (Resolve-FromScript $Out)
)
if ($Timings) {
    $arguments += @('--timings', (Resolve-FromScript $Timings))
}

& $exe @arguments
exit $LASTEXITCODE
