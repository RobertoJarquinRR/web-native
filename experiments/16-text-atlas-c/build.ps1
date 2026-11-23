# exp16-text-c build: HTML+CSS -> IR -> atlas + glyph quads -> scene_data.h
#                    -> wgpu-native C runtime -> PNG, and full verification.
#
# Build/verify only. The produced scene_text.exe has NO Python, NO font file,
# NO JSON/atlas file and NO browser at runtime: everything is baked C data.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build.ps1
#   powershell -ExecutionPolicy Bypass -File build.ps1 -SkipReference
#
[CmdletBinding()]
param(
    [switch]$SkipReference
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root

$repo = (Resolve-Path (Join-Path $root '..\..')).Path
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$wgpuRoot = Join-Path $repo 'third_party\wgpu-native'
$timings = [ordered]@{}
$report = [ordered]@{
    experiment = 'exp16-text-c'
    started    = (Get-Date).ToString('o')
    timings_ms = $timings
    commands   = [ordered]@{}
}

function Stop-Fail([string]$message) {
    throw $message
}

function Invoke-Native([string]$what, [string]$command) {
    Write-Host "  $command" -ForegroundColor DarkGray
    $scriptFile = Join-Path $root 'build_step.cmd'
    $content = "@echo off`r`n$command`r`nexit /b %errorlevel%`r`n"
    Set-Content -LiteralPath $scriptFile -Value $content -Encoding ascii
    & cmd.exe /d /c $scriptFile | Write-Host
    if ($LASTEXITCODE -ne 0) { Stop-Fail "$what exited with $LASTEXITCODE" }
}

function Compile-Runtime([string]$exePath) {
    $compile = "cl /nologo /O2 /W3 /std:c17 /TC runtime_text.c " +
               "/I`"$wgpuRoot\include`" /Fe:$exePath " +
               "/link `"$wgpuRoot\lib\wgpu_native.dll.lib`" " +
               "windowscodecs.lib ole32.lib user32.lib"
    Invoke-Native 'compile' "call `"$vcvars`" >nul && $compile"
    return $compile
}

function Run-Isolated([string]$exePath, [string]$arguments, [string]$workDir, [string]$dllDir) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path -LiteralPath $exePath).Path
    $psi.Arguments = $arguments
    $psi.WorkingDirectory = $workDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    # Acceptance: only System32 and the DLL directory on PATH.
    $psi.EnvironmentVariables['PATH'] = "$env:WINDIR\System32;$dllDir"
    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Stdout   = $stdout
        Stderr   = $stderr
        PathUsed = $psi.EnvironmentVariables['PATH']
    }
}

function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $path).Path)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLower()
    }
    finally { $sha.Dispose() }
}

function Write-Utf8NoBom([string]$path, [string]$text) {
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $text, $encoding)
}

function Get-Dumpbin([string]$exePath) {
    $scriptFile = Join-Path $root 'build_step.cmd'
    $command = "call `"$vcvars`" >nul && dumpbin /nologo /dependents `"$exePath`""
    $content = "@echo off`r`n$command`r`nexit /b 0`r`n"
    Set-Content -LiteralPath $scriptFile -Value $content -Encoding ascii
    return (& cmd.exe /d /c $scriptFile | Out-String)
}

$script:buildFailed = $true
try {
    if (-not (Test-Path -LiteralPath $vcvars)) {
        Stop-Fail "vcvars64.bat not found at $vcvars"
    }
    if (-not (Test-Path -LiteralPath "$wgpuRoot\include\webgpu.h")) {
        Stop-Fail "wgpu-native not found at $wgpuRoot (read-only exp10 copy)"
    }

    # ---------------------------------------------------------- toolchain/qc
    $script:py = Join-Path $root '.venv\Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $script:py)) {
        Write-Host 'setup: creating .venv (build/verify only)' -ForegroundColor Cyan
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & py -3.12 -m venv .venv
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'python -m venv failed' }
        & $script:py -m pip install --quiet --disable-pip-version-check --upgrade pip
        & $script:py -m pip install --quiet --disable-pip-version-check `
            html5lib tinycss2 cssselect2 uharfbuzz freetype-py pillow wgpu
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'pip install failed' }
        $sw.Stop()
        $timings['venv_setup'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    }
    & $script:py -c "import PIL, freetype, uharfbuzz, wgpu, html5lib, tinycss2, cssselect2" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'setup: installing missing Python packages' -ForegroundColor Cyan
        & $script:py -m pip install --quiet --disable-pip-version-check `
            html5lib tinycss2 cssselect2 uharfbuzz freetype-py pillow wgpu
        if ($LASTEXITCODE -ne 0) { Stop-Fail 'pip install failed' }
    }

    # ----------------------------------------------- build-time bake pipeline
    Write-Host 'bake: input.html+input.css -> display_list.json -> atlas.bin/glyph_quads.json -> scene_data.h' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $bakeArgs = @('run.py')
    if ($SkipReference) { $bakeArgs += '--skip-frontend-render' }
    & $script:py @bakeArgs
    $sw.Stop()
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'run.py failed' }
    $timings['emit'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report['commands']['emit'] = "$script:py run.py"
    $report['bake_subtimings_ms'] = Get-Content -LiteralPath 'build_timings.json' -Raw | ConvertFrom-Json

    # --------------------------------------------------------------- compile
    Write-Host 'compile: cl /O2 /TC runtime_text.c -> scene_text.exe' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $compileLine = Compile-Runtime 'scene_text.exe'
    $sw.Stop()
    $timings['compile'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report['commands']['compile'] = $compileLine
    if (-not (Test-Path -LiteralPath 'scene_text.exe')) { Stop-Fail 'scene_text.exe was not produced' }
    Copy-Item -LiteralPath "$wgpuRoot\bin\wgpu_native.dll" -Destination 'wgpu_native.dll' -Force

    # ------------------------------------------------------------------- run
    Write-Host 'run: isolated PATH = System32 + this directory' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $run = Run-Isolated 'scene_text.exe' 'scene_text.png' $root $root
    $sw.Stop()
    $timings['run'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    Set-Content -LiteralPath 'run_stdout.txt' -Value $run.Stdout -Encoding ascii
    if ($run.Stderr) { Set-Content -LiteralPath 'run_stderr.txt' -Value $run.Stderr -Encoding ascii }
    Write-Host $run.Stdout
    if ($run.Stderr) { Write-Host $run.Stderr -ForegroundColor DarkYellow }
    if ($run.ExitCode -ne 0) { Stop-Fail "scene_text.exe exited with $($run.ExitCode)" }
    $report['isolation'] = [ordered]@{
        path = $run.PathUsed; exit_code = $run.ExitCode
        stdout = $run.Stdout.Trim(); stderr = $run.Stderr.Trim()
    }
    $report['commands']['run'] = "PATH=$($run.PathUsed) ; .\scene_text.exe scene_text.png"
    $adapterLine = ($run.Stdout -split "`r?`n" | Where-Object { $_ -match '^adapter: backend=' } | Select-Object -First 1)
    $atlasLine = ($run.Stdout -split "`r?`n" | Where-Object { $_ -match '^atlas: uploaded' } | Select-Object -First 1)
    $report['adapter'] = $adapterLine
    $report['atlas_proof'] = $atlasLine

    # ---------------------------------------------------------------- verify
    Write-Host 'verify: Pillow pixel checks + frontend parity (build-time only)' -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $verifyArgs = @('verify.py', '--png', 'scene_text.png', '--header', 'scene_data.h',
                    '--ir', 'display_list.json', '--atlas', 'atlas.bin',
                    '--quads', 'glyph_quads.json', '--stdout', 'run_stdout.txt',
                    '--report', 'verify_report.json')
    if ($SkipReference) {
        $verifyArgs += '--no-frontend'
    } else {
        $verifyArgs += @('--frontend', 'render_frontend.png')
    }
    $verifyArgs += @('--visuals', 'proof')
    $verifyArgs += @('--exp09', (Join-Path (Split-Path -Parent $root) '09-core-package-v1\render.png'))
    & $script:py @verifyArgs
    $sw.Stop()
    if ($LASTEXITCODE -ne 0) { Stop-Fail 'pixel verification failed' }
    $timings['verify'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    $report['commands']['verify'] = "$script:py verify.py --png scene_text.png --frontend render_frontend.png --report verify_report.json"

    # -------------------------------------- self-containment + no Python DLL
    Write-Host 'proof: run from a directory with ONLY scene_text.exe + wgpu_native.dll' -ForegroundColor Cyan
    $proofDir = Join-Path $root 'proof\runtime_only'
    Remove-Item -LiteralPath $proofDir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $proofDir | Out-Null
    Copy-Item -LiteralPath 'scene_text.exe' -Destination $proofDir -Force
    Copy-Item -LiteralPath 'wgpu_native.dll' -Destination $proofDir -Force
    $proofFilesBefore = (Get-ChildItem -LiteralPath $proofDir | Select-Object -ExpandProperty Name) -join ', '
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proofRun = Run-Isolated (Join-Path $proofDir 'scene_text.exe') 'scene_text.png' $proofDir $proofDir
    $sw.Stop()
    $timings['proof_selfcontained'] = [math]::Round($sw.Elapsed.TotalMilliseconds, 2)
    if ($proofRun.ExitCode -ne 0) {
        Write-Host $proofRun.Stdout
        Write-Host $proofRun.Stderr -ForegroundColor DarkYellow
        Stop-Fail "self-contained run exited with $($proofRun.ExitCode)"
    }
    $proofPng = Join-Path $proofDir 'scene_text.png'
    $samePng = (Get-Sha256 'scene_text.png') -eq (Get-Sha256 $proofPng)
    if (-not $samePng) { Stop-Fail 'self-contained run produced a different PNG' }
    $proofFiles = (Get-ChildItem -LiteralPath $proofDir | Select-Object -ExpandProperty Name) -join ', '
    Write-Host "  proof dir before run: $proofFilesBefore"
    Write-Host "  proof dir after run : $proofFiles"
    $report['self_contained'] = [ordered]@{
        dir = $proofDir
        files_before_run = $proofFilesBefore
        files_after_run = $proofFiles
        png = $proofPng
        png_sha256_matches = $samePng
        exit_code = $proofRun.ExitCode
        path = $proofRun.PathUsed
    }

    Write-Host 'proof: dumpbin /dependents (no Python / FreeType / HarfBuzz DLL)' -ForegroundColor Cyan
    $dependents = Get-Dumpbin 'scene_text.exe'
    New-Item -ItemType Directory -Force -Path 'proof' | Out-Null
    Set-Content -LiteralPath 'proof\dependents.txt' -Value $dependents -Encoding ascii
    $imports = @([regex]::Matches($dependents, '(?im)^\s+([A-Za-z0-9_.-]+\.dll)\s*$') |
                 ForEach-Object { $_.Groups[1].Value })
    $forbidden = @($imports | Where-Object {
        $_ -match '(?i)python|freetype|harfbuzz|pybind|cairo|skia|electron|webkit|chrome'
    })
    $pythonDll = @($imports | Where-Object { $_ -match '(?i)python' })
    Write-Host ("  imports: " + ($imports -join ', '))
    if ($pythonDll.Count -gt 0) { Stop-Fail "exe imports Python: $($pythonDll -join ', ')" }
    $report['dependents'] = [ordered]@{
        imports = $imports
        forbidden = $forbidden
        python_imports = $pythonDll
        file = 'proof\dependents.txt'
    }

    # ---------------------------------------------------------------- summary
    $exe = Get-Item -LiteralPath 'scene_text.exe'
    $png = Get-Item -LiteralPath 'scene_text.png'
    $report['artifacts'] = [ordered]@{
        exe = [ordered]@{ path = $exe.FullName; bytes = $exe.Length }
        png = [ordered]@{ path = $png.FullName; bytes = $png.Length }
        scene_data_h = [ordered]@{ bytes = (Get-Item 'scene_data.h').Length }
        atlas_bin = [ordered]@{ bytes = (Get-Item 'atlas.bin').Length }
    }
    $report['hashes'] = [ordered]@{
        display_list_json = Get-Sha256 'display_list.json'
        atlas_bin         = Get-Sha256 'atlas.bin'
        glyph_quads_json  = Get-Sha256 'glyph_quads.json'
        scene_data_h      = Get-Sha256 'scene_data.h'
        scene_text_exe    = Get-Sha256 'scene_text.exe'
        scene_text_png    = Get-Sha256 'scene_text.png'
        render_frontend   = if (Test-Path 'render_frontend.png') { Get-Sha256 'render_frontend.png' } else { $null }
    }
    $report['verify_report'] = 'verify_report.json'
    $report['finished'] = (Get-Date).ToString('o')
    Write-Utf8NoBom (Join-Path $root 'build_report.json') ($report | ConvertTo-Json -Depth 8)

    $verifyDoc = Get-Content -LiteralPath 'verify_report.json' -Raw | ConvertFrom-Json
    $whole = $verifyDoc.metrics.frontend_diff.whole
    $textDiff = $verifyDoc.metrics.frontend_diff.text_area
    Write-Host ''
    Write-Host '================ exp16-text-c: PASS ================' -ForegroundColor Green
    Write-Host ("adapter     : {0}" -f ($adapterLine -replace '^adapter: ', ''))
    Write-Host ("{0}" -f $atlasLine)
    Write-Host ("exe         : {0} ({1} bytes)" -f $exe.FullName, $exe.Length)
    Write-Host ("png         : {0} ({1} bytes)" -f $png.FullName, $png.Length)
    Write-Host ("imports     : {0}" -f ($imports -join ', '))
    Write-Host ("verify      : {0}/{1} checks" -f $verifyDoc.passed, ($verifyDoc.passed + $verifyDoc.failed))
    if ($whole) { Write-Host ("frontend    : whole maxdiff={0} changed={1} ({2}%)" -f $whole.maxdiff, $whole.changed_pixels, $whole.changed_pct) }
    if ($textDiff) { Write-Host ("text area   : maxdiff={0} changed={1} ({2}%)" -f $textDiff.maxdiff, $textDiff.changed_pixels, $textDiff.changed_pct) }
    foreach ($key in $timings.Keys) {
        Write-Host ("timing {0,-18}: {1,9:N2} ms" -f $key, $timings[$key])
    }
    Write-Host '====================================================' -ForegroundColor Green
    $script:buildFailed = $false
}
catch {
    Write-Host ''
    Write-Host "BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray }
}
finally {
    $helper = Join-Path $root 'build_step.cmd'
    if (Test-Path -LiteralPath $helper) { Remove-Item -LiteralPath $helper -Force }
    Pop-Location
}

if ($script:buildFailed) { exit 1 }
exit 0
