# SPDX-License-Identifier: GPL-3.0-or-later
# Build-only resource acquisition. Called by package-release.ps1; no manual step.
# The unmodified upstream exporter and its dependencies live outside Edition.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$WorkDirectory)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) { throw 'LOCALAPPDATA is required.' }
. (Join-Path $PSScriptRoot 'model-cache.ps1')
$cache = [IO.Path]::GetFullPath($WorkDirectory)
$work = Join-Path $env:LOCALAPPDATA 'DepthPro2OfflineExport'
$revision = '1afa8587b6c285875b29d80892a457016ea0757f'
$checkout = Join-Path $work ('depthpro2-onnx-' + $revision)
$repo = 'https://github.com/protortyp/depthpro2-onnx.git'

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed (exit $LASTEXITCODE)." }
}
$git = Get-Command git.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
$uvExe = Get-PortableDepthUv $cache
New-Item -ItemType Directory -Force -Path $work,$cache | Out-Null
if (!(Test-Path -LiteralPath $checkout -PathType Container)) {
    $temp = $checkout + '.partial-' + [guid]::NewGuid().ToString('N')
    try {
        New-Item -ItemType Directory -Path $temp | Out-Null
        Invoke-Checked $git.Source @('-C',$temp,'init')
        Invoke-Checked $git.Source @('-C',$temp,'remote','add','origin',$repo)
        Invoke-Checked $git.Source @('-C',$temp,'fetch','--depth','1','origin',$revision)
        Invoke-Checked $git.Source @('-C',$temp,'checkout','--detach','FETCH_HEAD')
        [IO.Directory]::Move($temp,$checkout)
    } finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force } }
}
$head = & $git.Source -C $checkout rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or "$head".Trim() -ne $revision) { throw 'Unexpected upstream exporter commit; refusing to modify the existing checkout.' }
$dirty = & $git.Source -C $checkout status --porcelain --untracked-files=no
if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Upstream checkout contains tracked changes.' }
if (!(Test-Path -LiteralPath (Join-Path $checkout 'uv.lock') -PathType Leaf)) { throw 'Pinned upstream lockfile is missing.' }

# uv keeps the Python environment outside Edition. The unchanged upstream
# exporter needs PyTorch only on first-time resource acquisition, never playback.
# Do not inherit an activated environment or an unrelated Python project.
$oldEnv = $env:UV_PROJECT_ENVIRONMENT
$env:UV_PROJECT_ENVIRONMENT = Join-Path $work ('venv-' + $revision)
Push-Location -LiteralPath $checkout
try {
    $shape = '352x192'
    $stem = $depthStem
    $source = Join-Path $cache "$stem.source.onnx"
    $output = Join-Path $work ("export-$shape-" + [guid]::NewGuid().ToString('N'))
    $partial = $source + '.partial-' + [guid]::NewGuid().ToString('N')
    $receipt = $source + '.json'
    $receiptPartial = $partial + '.json'
    try {
        Write-Host "[INFO] Exporting the unchanged upstream S W4 FP16 model at $shape (external environment)."
        $exportHelper = Join-Path $PSScriptRoot 'export-lowres.py'
        Invoke-Checked $uvExe @('run','--locked','--no-dev','--extra','export','--python','3.12','python',$exportHelper,
            '--checkout',$checkout,'--width','352','--height','192','--out',$output)
        $file = Join-Path $output 'onnx\model_stream4_fp16.onnx'
        if (!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -le 0) { throw 'Upstream exporter did not produce the requested ONNX.' }
        $checkpoint = Get-Content -LiteralPath (Join-Path $output 'checkpoint.json') -Raw | ConvertFrom-Json
        if ($checkpoint.repo -cne 'apple/DepthPro2-S' -or $checkpoint.revision -cne '903a1c1ade918831d138adcc4aedbcc8f4da0078') { throw 'Invalid official model checkpoint provenance.' }
        [IO.File]::Copy($file,$partial,$false)
        @{sha256=(Get-DepthSha256Hex $partial); exporter=$repo; revision=$revision; shape=$shape; model_repo=$checkpoint.repo; model_revision=$checkpoint.revision} |
            ConvertTo-Json -Compress | Set-Content -LiteralPath $receiptPartial -Encoding ASCII
        if (Test-Path -LiteralPath $source) { [IO.File]::Replace($partial,$source,[NullString]::Value) }
        else { [IO.File]::Move($partial,$source) }
        if (Test-Path -LiteralPath $receipt) { [IO.File]::Replace($receiptPartial,$receipt,[NullString]::Value) }
        else { [IO.File]::Move($receiptPartial,$receipt) }
        Write-Host "[OK] Local fixed source ONNX: $source"
    } finally {
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
        if (Test-Path -LiteralPath $receiptPartial) { Remove-Item -LiteralPath $receiptPartial -Force }
        if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Recurse -Force }
    }
} finally {
    Pop-Location
    $env:UV_PROJECT_ENVIRONMENT = $oldEnv
}
Write-Host '[OK] Source ONNX prepared; continuing native DirectML conversion in the same build.'
