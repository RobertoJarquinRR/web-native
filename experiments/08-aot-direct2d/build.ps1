#requires -Version 5.1
<#
  AOT build driver: IR JSON -> generated C data -> cl.exe -> scene.exe -> PNG.

  Python is used ONLY here, at build time (emitter + Pillow verification).
  scene.exe itself is a native Windows binary with no Python dependency.
#>
param(
    [ValidateSet("d2d", "cpu")]
    [string]$Backend = "d2d",
    [string]$Ir = "display_list.json",
    [string]$OutPng = "scene.png",
    [string]$Report = "build_report.json",
    [string]$VerifyReport = "verify_report.json",
    [string[]]$ExtraPoint = @()
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root

$venvPython = Join-Path $root ".venv\Scripts\python.exe"
if (Test-Path $venvPython) {
    $pythonExe = $venvPython
    $pythonPrefix = @()
} else {
    $pythonExe = "py"
    $pythonPrefix = @("-3.12")
}

$vcvarsCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
)
$vcvars = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsPath) {
            $candidate = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) { $vcvars = $candidate }
        }
    }
}
if (-not $vcvars) { throw "vcvars64.bat not found; install the MSVC C++ toolchain" }

function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = (Resolve-Path -LiteralPath $Path).Path
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::OpenRead($full)
        try {
            $bytes = $sha.ComputeHash($stream)
        } finally {
            $stream.Dispose()
        }
    } finally {
        $sha.Dispose()
    }
    return (($bytes | ForEach-Object { $_.ToString("x2") }) -join "")
}

Write-Host "== toolchain =="
Write-Host "vcvars64: $vcvars"
$envLines = & cmd.exe /c "call `"$vcvars`" >nul && set"
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        $name = $matches[1]
        $value = $matches[2]
        if ($name -notmatch '^=') {
            Set-Item -Path "env:$name" -Value $value
        }
    }
}
$clVersion = (& cmd.exe /c "cl.exe 2>&1" | Select-String -Pattern "Version" | Select-Object -First 1)
if ($clVersion) { Write-Host "compiler: $($clVersion.ToString().Trim())" }
Write-Host ""

$timings = [ordered]@{}
$total = [System.Diagnostics.Stopwatch]::StartNew()

Write-Host "== 1. emit =="
$emitCommand = (@($pythonExe) + $pythonPrefix + @("emit_c.py", "--ir", $Ir, "--out", "scene_data.h")) -join " "
Write-Host "cmd: $emitCommand"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $pythonExe @pythonPrefix emit_c.py --ir $Ir --out scene_data.h
if ($LASTEXITCODE -ne 0) { throw "emit_c.py failed with exit code $LASTEXITCODE" }
$sw.Stop()
$timings["emit_ms"] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
Write-Host ""

Write-Host "== 2. compile ($Backend) =="
if ($Backend -eq "d2d") {
    $compileCommand = "cl /nologo /O2 /W4 /wd4201 /Brepro /EHsc /TP runtime_d2d.cpp /Fe:scene.exe /link /Brepro d2d1.lib windowscodecs.lib ole32.lib uuid.lib"
    $backendLabel = "Direct2D + WIC (C++)"
} else {
    $compileCommand = "cl /nologo /O2 /W4 /Brepro /TC runtime.c /Fe:scene.exe /link /Brepro windowscodecs.lib ole32.lib uuid.lib"
    $backendLabel = "pure C software rasterizer + WIC PNG"
}
Write-Host "cmd: $compileCommand"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& cmd.exe /c "$compileCommand 2>&1"
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with exit code $LASTEXITCODE" }
$sw.Stop()
$timings["compile_ms"] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
$exeHash = Get-Sha256Hex -Path "scene.exe"
Write-Host ""

Write-Host "== 3. run (native exe, no Python) =="
$runCommand = ".\scene.exe $OutPng"
Write-Host "cmd: $runCommand"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& ".\scene.exe" $OutPng
if ($LASTEXITCODE -ne 0) { throw "scene.exe failed with exit code $LASTEXITCODE" }
$sw.Stop()
$timings["run_ms"] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
if (-not (Test-Path $OutPng)) { throw "scene.exe did not produce $OutPng" }
$pngHash = Get-Sha256Hex -Path $OutPng
Write-Host ""

Write-Host "== 4. verify (Pillow, build-time only) =="
$verifyArgs = @("verify.py", "--png", $OutPng, "--ir", $Ir, "--report", $VerifyReport)
foreach ($point in $ExtraPoint) {
    $verifyArgs += @("--extra-point", $point)
}
$verifyCommand = (@($pythonExe) + $pythonPrefix + $verifyArgs) -join " "
Write-Host "cmd: $verifyCommand"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $pythonExe @pythonPrefix @verifyArgs
$verifyExit = $LASTEXITCODE
$sw.Stop()
$timings["verify_ms"] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
Write-Host ""

$total.Stop()
$timings["total_ms"] = [math]::Round($total.Elapsed.TotalMilliseconds, 2)

$reportDoc = [ordered]@{
    ir = $Ir
    png = $OutPng
    exe = "scene.exe"
    compiler = if ($clVersion) { $clVersion.ToString().Trim() } else { $null }
    backend = $backendLabel
    commands = [ordered]@{
        emit = $emitCommand
        compile = $compileCommand
        run = $runCommand
        verify = $verifyCommand
    }
    timings_ms = $timings
    hashes = [ordered]@{
        scene_exe_sha256 = $exeHash
        png_sha256 = $pngHash
    }
    verify_exit_code = $verifyExit
    pass = ($verifyExit -eq 0)
}
$reportDoc | ConvertTo-Json -Depth 6 | Set-Content -Path $Report -Encoding UTF8

Write-Host "== summary =="
Write-Host ("emit {0} ms | compile {1} ms | run {2} ms | verify {3} ms | total {4} ms" -f `
    $timings["emit_ms"], $timings["compile_ms"], $timings["run_ms"], $timings["verify_ms"], $timings["total_ms"])
Write-Host "scene.exe sha256: $exeHash"
Write-Host "$OutPng sha256: $pngHash"
Write-Host "report: $Report"
if ($verifyExit -ne 0) { exit 1 }
