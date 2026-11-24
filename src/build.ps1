param(
    [Parameter(Mandatory = $true)][string]$Html,
    [string]$OutDir = "",
    [int]$Width = 480,
    [int]$Height = 320
)
$ErrorActionPreference = 'Stop'

$srcDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $srcDir
$rust = Join-Path $repo 'src\frontend\rust\target\release\frontend-rust.exe'
$emitter = Join-Path $repo 'src\emitter\python\emit_c.py'
$runtimeDir = Join-Path $repo 'src\runtime\c-static'
$wgpu = Join-Path $repo 'third_party\wgpu-native'
$py = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'

foreach ($required in @($rust, $emitter, $py, $vcvars)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "missing required path: $required"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll')) -or
    -not (Test-Path -LiteralPath (Join-Path $wgpu 'lib\wgpu_native.dll.lib'))) {
    throw "wgpu-native binaries are missing. Run tools\fetch-wgpu.ps1 first (see THIRD_PARTY_NOTICES.md)."
}
if (-not $OutDir) { $OutDir = Join-Path $repo 'build\demo' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

# 1) Rust frontend: HTML + CSS -> display-list IR
Write-Host "frontend: $rust" -ForegroundColor Cyan
& $rust --html $Html --width $Width --height $Height `
    --out (Join-Path $OutDir 'display_list.json') `
    --timings (Join-Path $OutDir 'timings.json')
if ($LASTEXITCODE -ne 0) { throw "Rust frontend failed with $LASTEXITCODE" }

# 2) Emitter: IR -> baked C data + WGSL
Write-Host "emitter: $emitter" -ForegroundColor Cyan
& $py $emitter --ir (Join-Path $OutDir 'display_list.json') --out (Join-Path $OutDir 'scene_data.h')
if ($LASTEXITCODE -ne 0) { throw "emit_c.py failed with $LASTEXITCODE" }

# 3) Runtime sources + DLL
Copy-Item -LiteralPath (Join-Path $runtimeDir 'runtime_gpu.c') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $runtimeDir 'runtime_window.c') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $wgpu 'bin\wgpu_native.dll') -Destination $OutDir -Force

# 4) MSVC build: offscreen PNG renderer + windowed runtime
$buildCmd = @"
@echo off
cd /d "$OutDir"
call "$vcvars" >nul
cl /nologo /O2 /W3 /std:c17 /TC runtime_gpu.c /I"$wgpu\include" /Fe:render_png.exe /link "$wgpu\lib\wgpu_native.dll.lib" windowscodecs.lib ole32.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /std:c17 /TC runtime_window.c /I"$wgpu\include" /Fe:webnative-rust.exe /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup "$wgpu\lib\wgpu_native.dll.lib" user32.lib gdi32.lib
exit /b %errorlevel%
"@
$buildFile = Join-Path $OutDir 'build.cmd'
Set-Content -LiteralPath $buildFile -Value $buildCmd -Encoding ascii
Write-Host "compile: $buildFile" -ForegroundColor Cyan
& cmd.exe /d /c $buildFile
if ($LASTEXITCODE -ne 0) { throw "cl.exe build failed with $LASTEXITCODE" }

Write-Host ""
Write-Host "OK -> $OutDir" -ForegroundColor Green
Get-ChildItem $OutDir | Select-Object Name, Length
