#requires -Version 5.1
<#
.SYNOPSIS
    Download the pinned wgpu-native Windows x64 MSVC release.

.DESCRIPTION
    Fetches wgpu-native v29.0.1.1, verifies the SHA-256 of the release zip and
    extracts only the files the engine needs:

        bin/wgpu_native.dll
        lib/wgpu_native.dll.lib
        include/wgpu.h
        include/webgpu.h

    The DLL and the import library are not tracked by git. Run this script
    once after cloning, or whenever the pinned version changes.

.PARAMETER Force
    Re-download and overwrite the files even if the pinned version is present.

.PARAMETER ZipPath
    Use a local release zip instead of downloading it. The SHA-256 is still
    verified.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-wgpu.ps1

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-wgpu.ps1 -Force
#>
param(
    [switch]$Force,
    [string]$ZipPath
)

$ErrorActionPreference = 'Stop'

$Version = 'v29.0.1.1'
$Asset = 'wgpu-windows-x86_64-msvc-release.zip'
$Sha256 = '7e67d7445c42aeb85e30f88930fd8d7d83ee769e3390aeb1ada75ebf3cf78132'
$Url = "https://github.com/gfx-rs/wgpu-native/releases/download/$Version/$Asset"

$Repo = Split-Path -Parent $PSScriptRoot
$Dest = Join-Path $Repo 'third_party\wgpu-native'
$VersionFile = Join-Path $Dest 'VERSION.txt'
$ExpectedVersion = "$Version $Asset"

$Required = @(
    'bin\wgpu_native.dll',
    'lib\wgpu_native.dll.lib',
    'include\wgpu.h',
    'include\webgpu.h'
)

$Missing = @($Required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Dest $_)) })
$CurrentVersion = ''
if (Test-Path -LiteralPath $VersionFile) {
    $CurrentVersion = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
}

if (-not $Force -and $Missing.Count -eq 0 -and $CurrentVersion -eq $ExpectedVersion) {
    Write-Host "fetch-wgpu: already present ($ExpectedVersion)"
    exit 0
}

$TempZip = $null
try {
    if ($ZipPath) {
        $Zip = (Resolve-Path -LiteralPath $ZipPath).Path
    } else {
        $TempZip = Join-Path ([System.IO.Path]::GetTempPath()) "wgpu-native-$Version-$([guid]::NewGuid().ToString('N')).zip"
        $Zip = $TempZip
        Write-Host "fetch-wgpu: GET $Url"
        $Client = [System.Net.Http.HttpClient]::new()
        try {
            $Client.Timeout = [TimeSpan]::FromMinutes(10)
            $Client.DefaultRequestHeaders.UserAgent.ParseAdd('webnative-fetch-wgpu/1.0')
            $Bytes = $Client.GetByteArrayAsync($Url).GetAwaiter().GetResult()
        } finally {
            $Client.Dispose()
        }
        [System.IO.File]::WriteAllBytes($Zip, $Bytes)
        Write-Host ("fetch-wgpu: downloaded {0:N0} bytes" -f $Bytes.Length)
    }

    $Hash = [System.Security.Cryptography.SHA256]::Create()
    try {
        $Actual = [System.BitConverter]::ToString(
            $Hash.ComputeHash([System.IO.File]::ReadAllBytes($Zip))
        ).Replace('-', '').ToLowerInvariant()
    } finally {
        $Hash.Dispose()
    }

    if ($Actual -ne $Sha256) {
        throw "SHA-256 mismatch for $Zip`nexpected: $Sha256`nactual:   $Actual"
    }
    Write-Host "fetch-wgpu: sha256 ok ($Actual)"

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $Archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $Map = @{
            'include/webgpu/wgpu.h'       = 'include\wgpu.h'
            'include/webgpu/webgpu.h'     = 'include\webgpu.h'
            'lib/wgpu_native.dll'         = 'bin\wgpu_native.dll'
            'lib/wgpu_native.dll.lib'     = 'lib\wgpu_native.dll.lib'
        }
        $Extracted = 0
        foreach ($Entry in $Archive.Entries) {
            $Relative = $Map[$Entry.FullName]
            if (-not $Relative) { continue }
            $Target = Join-Path $Dest $Relative
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) | Out-Null
            if (Test-Path -LiteralPath $Target) { Remove-Item -LiteralPath $Target -Force }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($Entry, $Target, $true)
            Write-Host "  $($Entry.FullName) -> $Relative"
            $Extracted++
        }
        if ($Extracted -ne $Map.Count) {
            throw "release zip is missing expected entries ($Extracted/$($Map.Count) extracted)"
        }
    } finally {
        $Archive.Dispose()
    }

    $Missing = @($Required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Dest $_)) })
    if ($Missing.Count -ne 0) {
        throw "missing files after extraction: $($Missing -join ', ')"
    }

    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    Set-Content -LiteralPath $VersionFile -Value $ExpectedVersion -Encoding Ascii
    Write-Host "fetch-wgpu: ready in $Dest ($ExpectedVersion)"
} finally {
    if ($TempZip -and (Test-Path -LiteralPath $TempZip)) {
        Remove-Item -LiteralPath $TempZip -Force
    }
}
