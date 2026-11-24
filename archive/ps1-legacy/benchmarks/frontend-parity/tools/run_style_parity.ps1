# tools\run_style_parity.ps1 - Rust vs Python parity on the style tree.
#
# The frozen C++/C# references still produce display_list.json (post-layout,
# border-box). Rust and Python now also agree on the *pre-layout* style tree,
# which is the contract the runtime consumes, so box model and flex changes are
# checked here instead of duplicating layout in every frontend.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks\frontend-parity\tools\run_style_parity.ps1

[CmdletBinding()]
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

$failures = 0
foreach ($fixture in $fixtures) {
    $html = Join-Path (Join-Path $parity 'fixtures') $fixture.Html
    $css = Join-Path (Join-Path $parity 'fixtures') $fixture.Css
    $rust = Join-Path $out "$($fixture.Name).rust.json"
    $pyDump = Join-Path $out "$($fixture.Name).python.json"
    Write-Host "style parity: $($fixture.Name)" -ForegroundColor Cyan
    & $rustExe --html $html --css $css --width $fixture.W --height $fixture.H --emit-ui $rust | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "frontend-rust --emit-ui failed for $($fixture.Name)" }
    & $py $emitter --html $html --css $css --width $fixture.W --height $fixture.H `
        --out (Join-Path $out "$($fixture.Name).python.h") `
        --meta (Join-Path $out "$($fixture.Name).python.meta.json") `
        --no-text-metrics --dump-style-json $pyDump | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "emit_ui.py --dump-style-json failed for $($fixture.Name)" }
    & $py $compare --rust $rust --python $pyDump --label $fixture.Name
    if ($LASTEXITCODE -ne 0) { $failures += 1 }
}

if ($failures -gt 0) {
    Write-Host "STYLE PARITY: $failures fixture(s) differ" -ForegroundColor Red
    exit 1
}
Write-Host '================ style parity: PASS ================' -ForegroundColor Green
