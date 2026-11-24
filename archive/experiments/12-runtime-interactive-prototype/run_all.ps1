# Ejecuta la aceptacion completa de exp12 y falla si algun paso falla.
$ErrorActionPreference = "Stop"
$env:PYTHONUTF8 = "1"
$py = Join-Path $PSScriptRoot ".venv\Scripts\python.exe"
if (-not (Test-Path $py)) { throw "Falta .venv; ver README seccion 9." }

Push-Location $PSScriptRoot
try {
    & $py runtime.py
    if ($LASTEXITCODE -ne 0) { throw "runtime.py fallo" }
    & $py -m pytest tests -q
    if ($LASTEXITCODE -ne 0) { throw "pytest fallo" }
    & $py tests\test_hit_test.py
    if ($LASTEXITCODE -ne 0) { throw "runner de tests fallo" }
    & $py verify.py
    if ($LASTEXITCODE -ne 0) { throw "verify.py fallo" }
    Write-Host "`nACEPTACION OK" -ForegroundColor Green
} finally {
    Pop-Location
}
