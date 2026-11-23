# Builds build\frontend-cpp.exe with MSVC (cl.exe, Release /O2 /MT).
#
#   powershell -ExecutionPolicy Bypass -File build.ps1
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Clean
#
# Gumbo is compiled from the vendored C sources in third_party\gumbo\src.
[CmdletBinding()]
param(
    [string]$VsPath = 'C:\Program Files\Microsoft Visual Studio\18\Community',
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$buildDir = Join-Path $root 'build'
$objDir = Join-Path $buildDir 'obj'

# --- locate and import the MSVC x64 environment -----------------------------
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath | Select-Object -First 1
        if ($found) {
            $vcvars = Join-Path $found.Trim() 'VC\Auxiliary\Build\vcvars64.bat'
        }
    }
}
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found. Pass -VsPath <Visual Studio installation>."
}

Write-Host "importing MSVC environment from $vcvars"
$envLines = cmd.exe /c "call `"$vcvars`" >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) {
    throw "failed to initialise the MSVC environment (vcvars64 exit $LASTEXITCODE)"
}
foreach ($line in $envLines) {
    $eq = $line.IndexOf('=')
    if ($eq -gt 0) {
        Set-Item -Path ('env:' + $line.Substring(0, $eq)) -Value $line.Substring($eq + 1)
    }
}
$cl = (Get-Command cl.exe -ErrorAction Stop).Source

# --- directories ------------------------------------------------------------
if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Remove-Item -Recurse -Force -LiteralPath $buildDir
}
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$gumboInclude = Join-Path $root 'third_party\gumbo\src'
$gumboCompat = Join-Path $root 'third_party\gumbo\compat'
$compileFlags = @('/nologo', '/c', '/O2', '/MT', '/DNDEBUG', "/I$gumboCompat")
$objects = New-Object System.Collections.Generic.List[string]

function Invoke-Cl {
    param([string[]]$Arguments, [string]$What)
    & $cl @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "compilation failed ($What), exit code $LASTEXITCODE"
    }
}

# --- Gumbo (C) --------------------------------------------------------------
$gumboSources = Get-ChildItem -LiteralPath $gumboInclude -Filter '*.c' | Sort-Object Name
foreach ($source in $gumboSources) {
    $object = Join-Path $objDir ('gumbo_' + $source.BaseName + '.obj')
    Write-Host ("  cl  gumbo: " + $source.Name)
    Invoke-Cl -Arguments ($compileFlags + @('/W0', "/Fo$object", $source.FullName)) `
        -What $source.Name
    $objects.Add($object)
}

# --- frontend (C++20) -------------------------------------------------------
$sources = Get-ChildItem -LiteralPath (Join-Path $root 'src') -Filter '*.cpp' | Sort-Object Name
foreach ($source in $sources) {
    $object = Join-Path $objDir ('wn_' + $source.BaseName + '.obj')
    Write-Host ("  cl  src:   " + $source.Name)
    Invoke-Cl -Arguments ($compileFlags + @(
            '/std:c++20', '/EHsc', '/W4', "/I$gumboInclude",
            "/Fo$object", $source.FullName)) -What $source.Name
    $objects.Add($object)
}

# --- link -------------------------------------------------------------------
$exe = Join-Path $buildDir 'frontend-cpp.exe'
if (Test-Path -LiteralPath $exe) {
    Remove-Item -Force -LiteralPath $exe
}
Write-Host "  link:     $exe"
& $cl @('/nologo', '/O2', '/MT', "/Fe:$exe") $objects.ToArray() '/link' '/SUBSYSTEM:CONSOLE'
if ($LASTEXITCODE -ne 0) {
    throw "link failed, exit code $LASTEXITCODE"
}

# --- BUILD_INFO.json --------------------------------------------------------
$banner = ''
try {
    $banner = (& cmd.exe /c 'cl 2>&1' | Select-Object -First 1)
} catch {
    $banner = ''
}
$compiler = if ($banner) { ([string]$banner).Trim() } else { "MSVC $($env:VCToolsVersion)" }
$info = [ordered]@{
    binary       = 'build/frontend-cpp.exe'
    build_mode   = 'release (/O2, /MT, C++20)'
    compiler     = $compiler
    binary_bytes = [long](Get-Item -LiteralPath $exe).Length
    built_at     = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
}
$json = $info | ConvertTo-Json
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $root 'BUILD_INFO.json'), $json + "`r`n", $utf8)

Write-Host ''
Write-Host ("built {0} ({1} bytes)" -f $exe, $info.binary_bytes)
Write-Host ("compiler: {0}" -f $compiler)
