# SPDX-License-Identifier: GPL-3.0-or-later
# Offline preparation only. Never run from the renderer or download automatically at playback.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Converter,
    [Parameter(Mandatory=$true)][string]$WorkDirectory,
    [switch]$Rebuild
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'model-cache.ps1')

$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')).TrimEnd([char[]]"\/")
$work = [IO.Path]::GetFullPath($WorkDirectory)
if ($work.TrimEnd([char[]]"\/").Equals($project,[StringComparison]::OrdinalIgnoreCase) -or
    $work.StartsWith($project + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Use a work directory outside the source repository (generated models are not source files).'
}
$converterPath = (Resolve-Path -LiteralPath $Converter).Path
$edition = Split-Path -Parent $converterPath
if (!(Test-Path -LiteralPath $converterPath -PathType Leaf) -or
    !(Test-Path -LiteralPath (Join-Path $edition 'DirectML.dll') -PathType Leaf)) {
    throw 'Run depth-convert.exe alongside its matching DirectML.dll.'
}
$adapter = Join-Path $PSScriptRoot 'prepare_model.py'
$stem = $depthStem
$onnx = Join-Path $work "$stem.onnx"
$pack = Join-Path $work "$stem.dmlpack"
$source = Join-Path $work "$stem.source.onnx"
$adapterReceipt = $onnx + '.dml-reshape-v1'
New-Item -ItemType Directory -Force -Path $work | Out-Null
function Invoke-Checked([string]$program, [string[]]$arguments) {
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "$program failed (exit $LASTEXITCODE). See output above." }
}
function Invoke-GraphAdapter([string]$inputPath,[string]$outputPath) {
    $adapterArgs = @($adapter,$inputPath,$outputPath)
    # Reuse an existing Python installation if its ONNX library is available.
    $python = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($python -and $python.Source -notlike '*\WindowsApps\*') {
        $hasOnnx = $false
        try {
            & $python.Source -c 'import onnx' 2>$null
            $hasOnnx = $LASTEXITCODE -eq 0
        } catch { }
        if ($hasOnnx) { Invoke-Checked $python.Source $adapterArgs; return }
    }
    $uv = Get-PortableDepthUv $work
    # --no-project is deliberate: do not sync or install ANY third-party exporter project.
    Invoke-Checked $uv (@('run','--no-project','--python','3.12','--with','onnx==1.19.1','python') + $adapterArgs)
}
$valid = !$Rebuild -and (Test-ValidatedDepthPack $pack)
if ($valid) {
    Write-Host "[INFO] Reusing validated native model pack: $pack"
} else {
    $hasSource = Test-DepthSourceOnnx $source
    $adapted = Test-DepthAdaptedOnnx $onnx
    if (!$adapted -and !$hasSource) { throw 'No verified 352x192 source or adapted ONNX. Resource acquisition must succeed first.' }
    if (!$adapted -or ($Rebuild -and $hasSource)) {
        $tempOnnx = Join-Path $work ("$stem.partial-" + [guid]::NewGuid().ToString('N') + '.onnx')
        try {
            Invoke-GraphAdapter $source $tempOnnx
            if (!(Test-Path -LiteralPath $tempOnnx -PathType Leaf)) { throw 'Graph adapter did not produce ONNX.' }
            if (Test-Path -LiteralPath $onnx -PathType Leaf) { [IO.File]::Replace($tempOnnx,$onnx,[NullString]::Value) }
            else { [IO.File]::Move($tempOnnx,$onnx) }
            Get-DepthSha256Hex $onnx | Set-Content -LiteralPath $adapterReceipt -Encoding ASCII
        } finally { if (Test-Path -LiteralPath $tempOnnx) { Remove-Item -LiteralPath $tempOnnx -Force } }
    } else { Write-Host "[INFO] Reusing adapted ONNX: $onnx" }

    # Record provenance of the successful native conversion. Future ordinary builds
    # validate only the pack bytes; they do not invalidate it when the tools change.
    $receiptPath = $pack + '.json'
    $inputs = [ordered]@{
        onnx = (Get-DepthSha256Hex $onnx)
        converter = (Get-DepthSha256Hex $converterPath)
        directml = (Get-DepthSha256Hex (Join-Path $edition 'DirectML.dll'))
    }
    # Convert to a sibling candidate; the converter validates it before publishing.
    # A failed forced conversion must not destroy the previous good pack.
    $candidate = Join-Path $work ("$stem.partial-" + [guid]::NewGuid().ToString('N') + '.dmlpack')
    $receiptCandidate = $candidate + '.json'
    try {
        try { Invoke-Checked $converterPath @($onnx,$candidate) } catch {
            Write-Host "[ERROR] DirectML conversion failed. Cached ONNX preserved: $onnx"
            Write-Host "[INFO] Original source (if present): $source"
            throw
        }
        if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) { throw 'Converter did not publish a validated pack.' }
        $inputs['pack'] = (Get-DepthSha256Hex $candidate)
        $inputs | ConvertTo-Json -Compress | Set-Content -LiteralPath $receiptCandidate -Encoding ASCII
        if (Test-Path -LiteralPath $pack -PathType Leaf) {
            [IO.File]::Replace($candidate,$pack,[NullString]::Value)
        } else { [IO.File]::Move($candidate,$pack) }
        if (Test-Path -LiteralPath $receiptPath -PathType Leaf) {
            [IO.File]::Replace($receiptCandidate,$receiptPath,[NullString]::Value)
        } else { [IO.File]::Move($receiptCandidate,$receiptPath) }
    } finally {
        foreach ($file in @($candidate,$receiptCandidate)) {
            if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
        }
    }
}
Write-Host "Validated pack: $pack"
