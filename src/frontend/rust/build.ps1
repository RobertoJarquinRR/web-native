param()

$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $dir
try {
    cargo build --release
    if ($LASTEXITCODE -ne 0) {
        throw "cargo build --release fallo con codigo $LASTEXITCODE"
    }

    $exe = Join-Path $dir 'target\release\frontend-rust.exe'
    if (-not (Test-Path -LiteralPath $exe)) {
        throw "Binario no encontrado tras compilar: $exe"
    }

    $rustc = ((& rustc --version) -join '').Trim()
    $info = [ordered]@{
        binary       = 'target/release/frontend-rust.exe'
        build_mode   = 'release'
        rustc        = $rustc
        binary_bytes = (Get-Item -LiteralPath $exe).Length
        built_at     = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    $json = $info | ConvertTo-Json
    [System.IO.File]::WriteAllText(
        (Join-Path $dir 'BUILD_INFO.json'),
        $json,
        (New-Object System.Text.UTF8Encoding($false))
    )

    Write-Host ("built {0} ({1} bytes, {2})" -f $info.binary, $info.binary_bytes, $rustc)
}
finally {
    Pop-Location
}
