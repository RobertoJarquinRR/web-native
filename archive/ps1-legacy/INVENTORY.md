## benchmarks\engine-metrics\run.ps1
```powershell
param(
    [int]$Runs = 10,
    [switch]$ForceSnapshot,
    [switch]$RebuildR1,
    [switch]$SkipR1,
    [switch]$SkipLayout100,
    [switch]$NoWarmup
)
  & py -3 -m venv (Join-Path $root '.venv')
  & python -m venv (Join-Path $root '.venv')
  & $venvPy -c "import psutil" 2>$null
  & $venvPy -m pip install --disable-pip-version-check --no-input psutil
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'src\runtime\\build.ps1') -SkipParityCheck
  & powershell -NoProfile -ExecutionPolicy Bypass -File $r1Script `
  & $frontRust --html (Join-Path $root 'fixtures\layout-100\index.html') `
  & $canonPy $emitUi --ui-json (Join-Path $l100Dir 'ui.json') `
  & cmd.exe /d /c $buildFile | Write-Host
  & $venvPy @measureArgs
## benchmarks\frontend-parity\tools\run_style_parity.ps1
```powershell
param()

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$parity = Split-Path -Parent $here
$repo = Split-Path -Parent (Split-Path -Parent $parity)
$rustExe = Join-Path $repo 'src\frontend\rust\target\release\frontend-rust.exe'
$py = Join-Path $repo 'src\frontend\python\.venv\Scripts\python.exe'
$emitter = Join-Path $repo 'src\emitter\python\emit_ui.py'
$compare = Join-Path $here 'compare_style.py'
$out = Join-Path $parity 'results\style'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$fixtures = @(
    @{ Name = 'contract'; Html = 'contract\input.html'; Css = 'contract\input.css'; W = 400; H = 300 },
    @{ Name = 'flex-page'; Html = 'flex-page\index.html'; Css = 'flex-page\style.css'; W = 480; H = 320 }
)
  & $rustExe --html $html --css $css --width $fixture.W --height $fixture.H --emit-ui $rust | Out-Null
  & $py $emitter --html $html --css $css --width $fixture.W --height $fixture.H `
  & $py $compare --rust $rust --python $pyDump --label $fixture.Name
## benchmarks\pilot-media-library\stress\run_stress.ps1
```powershell
param(
    [int]$Runs = 7,
    [switch]$Regenerate,
    [switch]$SkipEmit,
    [switch]$SkipCompile,
    [switch]$SkipPilot,
    [string]$Only,
    [string]$Report
)
  & $py @argsList
## benchmarks\visual-parity\run_visual.ps1
```powershell
param(
    [string[]]$Fixtures = @(),
    [string]$OutDir = "",
    [switch]$SkipChromium,
    [switch]$SkipNative,
    [switch]$RebuildNative,
    [switch]$AllowPartial,
    [switch]$EnforceGate,
    [switch]$NoArchive
)

## src\frontend\cpp\build.ps1
```powershell
param(
    [string]$VsPath = 'C:\Program Files\Microsoft Visual Studio\18\Community',
    [switch]$Clean
)
  & $cl @Arguments
  Invoke-Cl -Arguments ($compileFlags + @('/W0', "/Fo$object", $source.FullName)) `
  Invoke-Cl -Arguments ($compileFlags + @(
  & $cl @('/nologo', '/O2', '/MT', "/Fe:$exe") $objects.ToArray() '/link' '/SUBSYSTEM:CONSOLE'
## src\frontend\cpp\run.ps1
```powershell
param(
    [Parameter(Mandatory = $true)][string]$Html,
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Timings
)
  & $exe @arguments
## src\frontend\csharp\build.ps1
```powershell
param(
    [switch]$Jit,   # force the framework-dependent Release publish (skip native AOT)
    [switch]$NoClean
)
  & dotnet publish $project -c Release -r win-x64 `
  & $aotExe --help | Out-Null
  & dotnet publish $project -c Release -r win-x64 --self-contained false `
## src\frontend\csharp\run.ps1
```powershell
param(
    [Parameter(Mandatory = $true)][string]$Html,
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Timings
)
  & $exe @argv
  & dotnet $dll @argv
## src\frontend\python\run.ps1
```powershell
param(
    [Parameter(Mandatory = $true)][string]$Html,
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Timings
)
  & $py @argv
## src\frontend\rust\bench-rust.ps1
```powershell
param(
    [int]$Runs = 10,
    [int]$Warmup = 2
)

## src\frontend\rust\build.ps1
```powershell
# (sin param)
  cargo build --release
## src\frontend\rust\run.ps1
```powershell
param(
    [Parameter(Mandatory = $true)][string]$Html,
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Timings,
    [string]$Atlas,
    [switch]$StrictSubset,
    [switch]$Pilot
)
  & $exe @argv
## src\runtime\build_abi.ps1
```powershell
param(
    [int]$Frames = 20,
    [switch]$SkipRun,
    [ValidateSet('auto', 'd3d11', 'wgpu')]
    [string]$Renderer = 'auto'
)
  & cmd.exe /d /c $scriptFile 2>&1 | Tee-Object -FilePath $logFile -Append | Write-Host
  & powershell -NoProfile -ExecutionPolicy Bypass -File $r2BuildScript `
  & powershell -NoProfile -ExecutionPolicy Bypass -File $r2BuildScript `
  Invoke-Tool 'compile ABI objects (cl /W3) + renderer_d3d11.c (cl /W4)' $compileCmd $logPath
  Invoke-Tool 'compile+run header checks (C17 and C++17)' $headerCmd $logPath
  Invoke-Tool 'compile+run abi_smoke' $smokeCmd $logPath
  Invoke-Tool 'compile+link standalone.exe and embedded.exe' $demoCmd $logPath
## src\runtime\build.ps1
```powershell
param(
    [switch]$SkipEmit,
    [switch]$SkipRun,
    [switch]$UsePythonTree,
    [switch]$SkipRustBuild,
    [switch]$SkipParityCheck,
    [switch]$Pilot,
    # Arbitrary page mode (tooling/harness): build one HTML file and its CSS
    # into a throw-away runtime under -OutDir. Skips parity and verify.
    [string]$Html,
    [string]$Css,
    [int]$Width = 0,
    [int]$Height = 0,
    [string]$OutDir
)
  & cmd.exe /d /c $scriptFile | Write-Host
  & $py $emitter --html $fixtureHtml --css $fixtureCss `
  & cargo build --release --manifest-path (Join-Path $rustDir 'Cargo.toml')
  & $rustExe @rustArgs
  & $py $emitter --ui-json $uiJson `
  & $py $emitter --html $fixtureHtml --css $fixtureCss `
  & $py $emitter --compare (Join-Path $oracleDir 'ui_data.h') (Join-Path $out 'ui_data.h') `
  Invoke-Native 'compile' "call `"$vcvars`" >nul && cd /d `"$out`" && $compile"
  & $py $pilotVerifyPy --runtime (Join-Path $out 'runtime_r2.exe') --out $verifyOut |
  & $py $verifyPy --dir $out | Tee-Object -FilePath $script:logPath -Append | Write-Host
## src\runtime\tests\css\run_css.ps1
```powershell
param(
    [switch]$SkipCompile
)
  & cmd.exe /d /c $step | Write-Host
  & $py (Join-Path $here 'make_images.py') | Write-Host
  & $py $emitter --ui-json (Join-Path $page "$name.json") `
  & $py $emitter --compare (Join-Path $here 'fixture\legacy_ui_data.h') `
  & $py $emitter --compare (Join-Path $out 'emit\card\ui_data.h') `
  Invoke-Command "compile $name" $compile
  Invoke-Runtime 'layout_card' $cardExe @('--report', (Join-Path $out 'layout_card.json'),
  Invoke-Runtime 'render_card' $cardExe @('--render', '400', '240',
  Invoke-Runtime 'render_control' $controlExe @('--render', '400', '240',
  & $py (Join-Path $here 'verify_css.py') --dir $out --page $page | Write-Host
## src\runtime\tests\d3d11\measure_cpu.ps1
```powershell
param(
    [int]$IdleMs = 2000,
    [int]$HoverMs = 4000
)

## src\runtime\tests\d3d11\run_d3d11.ps1
```powershell
param(
    [switch]$SkipBuild,
    [switch]$SkipPilot
)
  & powershell -NoProfile -ExecutionPolicy Bypass -File $script @arguments | Write-Host
  Invoke-ChildScript 'build_abi.ps1 -SkipRun' $abi @('-SkipRun')
  Invoke-ChildScript 'build.ps1 -SkipRustBuild -SkipParityCheck -SkipRun' $r2build `
  & $py $verify --capture $capture --reference $reference --report $comparePath `
  Invoke-ChildScript 'run_pilot.ps1 (image fixture)' $pilot @()
  & $py $verify --capture $pilotCapture --reference $pilotReference `
## src\runtime\tests\dynamic\run_dynamic.ps1
```powershell
param(
    [switch]$Regenerate,
    [switch]$SkipRun
)
  & cmd.exe /d /c $step 2>&1 | Write-Host
  & $py (Join-Path $here 'make_images.py') $page | Write-Host
  & $py (Join-Path $here 'make_images.py') $page | Write-Host
  & $rustExe --html 'page\index.html' --css 'page\style.css' `
  & $py $emitter --ui-json 'page\ui.json' `
  Invoke-Tool 'compile dynamic core test' $coreCmd
  Invoke-Tool 'compile runtime_dynamic' $runtimeCmd
  Invoke-Tool 'compile dynamic ABI demo' $abiCmd
  & $py (Join-Path $here 'verify_dynamic.py') --dir $out |
## src\runtime\tests\emitter-escapes\run_test.ps1
```powershell
# (sin param)
  & $py $emitter --ui-json (Join-Path $here 'ui.json') `
  & cmd.exe /d /c $step | Write-Host
  & $py (Join-Path $here 'verify_escapes.py') `
## src\runtime\tests\fidelity\run_fidelity.ps1
```powershell
param(
    [switch]$SkipRustBuild,
    [switch]$SkipRender
)
  & cmd.exe /d /c $step | Write-Host
  & $rustExe --html 'page\index.html' --width 480 --height 560 `
  & $py $emitter --ui-json (Join-Path $out 'ui.json') `
  & $py $emitter --ui-json (Join-Path $repo 'src\runtime\c-r2\tests\input\page\ui.json') `
  Invoke-Command 'compile fidelity core test' $coreCmd
  Invoke-Command 'compile runtime' $runtimeCmd
  Invoke-Runtime 'render' $runtimeExe @('--render', '480', '560',
  Invoke-Runtime 'render wheel' $runtimeExe @('--render', '480', '560',
## src\runtime\tests\input\run_input.ps1
```powershell
param(
    [switch]$Regenerate,
    [switch]$SkipRun
)
  & cmd.exe /d /c $step 2>&1 | Write-Host
  & $rustExe --html 'page\index.html' --css 'page\style.css' `
  & $py $emitter --ui-json 'page\ui.json' `
  Invoke-Tool 'compile input core test' $coreCmd
  Invoke-Tool 'compile runtime_input' $runtimeCmd
  Invoke-Tool 'compile input ABI demo' $abiCmd
  & $py (Join-Path $here 'verify_input.py') --dir $out |
## src\runtime\tests\offscreen\run_offscreen.ps1
```powershell
param(
    [int]$Width = 960,
    [int]$Height = 640
)
  & cmd.exe /d /c $cmdFile | Add-Content -LiteralPath $logPath
  & (Join-Path $out 'offscreen_test.exe') $Width $Height $d3d11Png 2>&1 | Add-Content -LiteralPath $logPath
  & (Join-Path $out 'offscreen_test.exe') $Width $Height $wgpuPng 2>&1 | Add-Content -LiteralPath $logPath
  & $py $verify --capture $d3d11Png --reference $wgpuPng --report $compare `
## src\runtime\tests\pilot-media-library\run_pilot.ps1
```powershell
param(
    [switch]$SkipCompile
)
  & cmd.exe /d /c $step | Write-Host
  & $py (Join-Path $here 'make_posters.py') | Write-Host
  & $py (Join-Path $here 'make_ui.py') | Write-Host
  & $py $emitter --ui-json (Join-Path $here 'ui.json') `
  Invoke-Command 'compile' $compile
  Invoke-Render 'layout_report' @('--report', 'pilot_layout.json',
  Invoke-Render 'render_1280' @('--render', '1280', '800', 'render_1280.png') 'render_1280.log' | Out-Null
  Invoke-Render 'render_800' @('--render', '800', '600', 'render_800.png') 'render_800.log' | Out-Null
  Invoke-Render 'wheel_800' @('--render', '800', '600', 'wheel_800.png',
  Invoke-Render 'wheel_1280' @('--render', '1280', '800', 'wheel_1280.png',
  Invoke-Render 'wheel_max' @('--render', '800', '600', 'wheel_max.png',
  Invoke-Render 'wheel_outside' @('--render', '800', '600', 'wheel_outside.png',
  & $py (Join-Path $here 'verify_pilot.py') --dir $out | Write-Host
  Invoke-Command 'compile abi pilot' $abiCmd
  & $py (Join-Path $runtime 'tests\d3d11\verify_d3d11.py') `
## tools\fetch-wgpu.ps1
```powershell
param(
    [switch]$Force,
    [string]$ZipPath
)

