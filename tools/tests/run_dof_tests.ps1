$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out = Join-Path $root 'out\tests\dof-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$log = Join-Path $out 'results.log'
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($log,
    "Depth-of-field focused tests (offscreen WARP and CPU hook)`r`n", $utf8)

function Write-Result([string]$line) {
    [Console]::WriteLine($line)
    [System.IO.File]::AppendAllText($log, "$line`r`n", $utf8)
}

foreach ($runner in @('run_dof_hook.cmd', 'run_dof_warp.cmd')) {
    Write-Result "Running $runner"
    & cmd.exe /d /c (Join-Path $PSScriptRoot $runner) 2>&1 |
        ForEach-Object { Write-Result ([string]$_) }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Result 'PASS all depth-of-field focused tests'
