param(
    [Parameter(Mandatory = $true)][string]$Html,
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Timings
)

# Frontend parity harness entry point for the C# implementation.
# Relative paths are resolved against this script's directory (see README).
# Only .NET APIs are used (no PowerShell cmdlets) to keep the harness wall time low:
# each cmdlet would autoload Microsoft.PowerShell.Management and cost ~100 ms.
$ErrorActionPreference = 'Stop'
$dir = [System.IO.Path]::GetDirectoryName($MyInvocation.MyCommand.Path)

function Resolve-FromScript([string]$path) {
    if ([System.IO.Path]::IsPathRooted($path)) { return $path }
    return [System.IO.Path]::Combine($dir, $path)
}

# Fastest working binary first: native AOT, then published JIT, then plain build.
$candidates = @(
    [System.IO.Path]::Combine($dir, 'out', 'frontend-csharp.exe'),
    [System.IO.Path]::Combine($dir, 'out-jit', 'frontend-csharp.exe'),
    [System.IO.Path]::Combine($dir, 'bin', 'Release', 'net10.0', 'win-x64', 'publish', 'frontend-csharp.exe'),
    [System.IO.Path]::Combine($dir, 'bin', 'Release', 'net10.0', 'frontend-csharp.exe')
)

$exe = $null
foreach ($candidate in $candidates) {
    if ([System.IO.File]::Exists($candidate)) {
        $exe = $candidate
        break
    }
}

$argv = @(
    '--html', (Resolve-FromScript $Html),
    '--width', [string]$Width,
    '--height', [string]$Height,
    '--out', (Resolve-FromScript $Out)
)
if ($Timings) { $argv += @('--timings', (Resolve-FromScript $Timings)) }

if ($exe) {
    & $exe @argv
    exit $LASTEXITCODE
}

$dll = [System.IO.Path]::Combine($dir, 'bin', 'Release', 'net10.0', 'frontend-csharp.dll')
if ([System.IO.File]::Exists($dll)) {
    & dotnet $dll @argv
    exit $LASTEXITCODE
}

throw "frontend-csharp binary not found under $dir; run build.ps1 first"
