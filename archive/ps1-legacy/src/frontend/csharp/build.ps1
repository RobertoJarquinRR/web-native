param(
    [switch]$Jit,   # force the framework-dependent Release publish (skip native AOT)
    [switch]$NoClean
)

# Builds frontend-csharp and writes BUILD_INFO.json describing the shipped binary.
# Default: native AOT (win-x64). Fallback: framework-dependent Release publish.
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$project = Join-Path $dir 'WebNative.Frontend.csproj'
$infoPath = Join-Path $dir 'BUILD_INFO.json'

Push-Location $dir
try {
    $dotnetVersion = (& dotnet --version).Trim()
    if ($LASTEXITCODE -ne 0) { throw "dotnet not available" }

    $mode = $null
    $binary = $null

    if (-not $Jit) {
        if (-not $NoClean) {
            Remove-Item -Recurse -Force (Join-Path $dir 'out') -ErrorAction SilentlyContinue
        }
        Write-Host "[build] dotnet publish native AOT (win-x64, SDK $dotnetVersion)..."
        & dotnet publish $project -c Release -r win-x64 `
            -p:PublishAot=true `
            -p:StripSymbols=true `
            -p:OptimizationPreference=Speed `
            -p:DebugType=None `
            -p:DebugSymbols=false `
            -o (Join-Path $dir 'out')
        $aotExe = Join-Path $dir 'out\frontend-csharp.exe'
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $aotExe)) {
            # Smoke test: the AOT image must start and expose the CLI.
            & $aotExe --help | Out-Null
            if ($LASTEXITCODE -eq 0) {
                $mode = 'aot'
                $binary = 'out\frontend-csharp.exe'
                # The IL PDB is only useful for debugging; keep the shipped folder lean.
                Get-ChildItem -LiteralPath (Join-Path $dir 'out') -Filter '*.pdb' -File |
                    Remove-Item -Force -ErrorAction SilentlyContinue
            }
        }
        if (-not $mode) {
            Write-Warning "[build] native AOT failed; falling back to framework-dependent Release publish"
        }
    }

    if (-not $mode) {
        if (-not $Jit) {
            Remove-Item -Recurse -Force (Join-Path $dir 'out-jit') -ErrorAction SilentlyContinue
        }
        Write-Host "[build] dotnet publish framework-dependent Release (win-x64, SDK $dotnetVersion)..."
        & dotnet publish $project -c Release -r win-x64 --self-contained false `
            -p:PublishSingleFile=false `
            -p:DebugType=None `
            -p:DebugSymbols=false `
            -o (Join-Path $dir 'out-jit')
        $jitExe = Join-Path $dir 'out-jit\frontend-csharp.exe'
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $jitExe)) {
            throw "publish failed: $jitExe was not produced"
        }
        $mode = 'jit'
        $binary = 'out-jit\frontend-csharp.exe'
    }

    $binaryPath = Join-Path $dir $binary
    $bytes = (Get-Item -LiteralPath $binaryPath).Length
    $info = [ordered]@{
        binary       = ($binary -replace '\\', '/')
        build_mode   = $mode
        dotnet_version = $dotnetVersion
        binary_bytes = $bytes
        built_at     = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    $json = ($info | ConvertTo-Json -Depth 3) -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($infoPath, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "[build] mode=$mode binary=$binary bytes=$bytes"
    Write-Host "[build] wrote $infoPath"
}
finally {
    Pop-Location
}
