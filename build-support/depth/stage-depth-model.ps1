# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [string]$Converter,
    [scriptblock]$EnsureConverter,
    [Parameter(Mandatory=$true)][string]$Edition,
    [Parameter(Mandatory=$true)][string]$PreviousEdition
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'model-cache.ps1')
if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) { throw 'LOCALAPPDATA is required for the model cache.' }
$work = Join-Path $env:LOCALAPPDATA 'MPC-VR-Edition\DepthPro2Research'
$pair = Join-Path $work 'split-352x192'
$pack = Join-Path $work "$depthStem.dmlpack"
$onnx = Join-Path $work "$depthStem.onnx"
$source = Join-Path $work "$depthStem.source.onnx"
$null = New-Item -ItemType Directory -Path $work -Force
$lock = [IO.File]::Open((Join-Path $work 'prepare-352x192.lock'), 'OpenOrCreate', 'Write', 'None')

function Copy-PairToEdition {
    foreach ($name in @($depthGraphName,$depthWeightName)) {
        Copy-Item -LiteralPath (Join-Path $pair $name) -Destination $Edition
    }
}

try {
    if (Test-ValidatedDepthPair $pair) {
        Copy-PairToEdition
        Write-Host '[INFO] Reused verified 352x192 graph and weights.'
        return
    }
    $validPack = Test-ValidatedDepthPack $pack
    if (!$validPack -and !(Test-DepthAdaptedOnnx $onnx) -and !(Test-DepthSourceOnnx $source)) {
        & (Join-Path $PSScriptRoot 'export-depthpro2.ps1') -WorkDirectory $work
    }
    if ($EnsureConverter) { & $EnsureConverter }
    if (!$Converter -or !(Test-Path -LiteralPath $Converter -PathType Leaf)) {
        throw 'Building the depth resource requires the native converter.'
    }
    if (!$validPack) {
        & (Join-Path $PSScriptRoot 'convert-depthpro2.ps1') -Converter $Converter -WorkDirectory $work
        if (!(Test-ValidatedDepthPack $pack)) { throw 'Native conversion did not produce a verified depth pack.' }
    }


    $candidate = $pair + '.stage-' + [Guid]::NewGuid().ToString('N')
    $previous = $pair + '.previous-' + [Guid]::NewGuid().ToString('N')
    foreach ($path in @($pair,$candidate,$previous)) {
        if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)) -ne [IO.Path]::GetFullPath($work) -or
            ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint))) {
            throw "Unsafe depth cache path: $path"
        }
    }
    $null = New-Item -ItemType Directory -Path $candidate
    try {
        $graph = Join-Path $candidate $depthGraphName
        $weights = Join-Path $candidate $depthWeightName
        foreach ($folder in @($PreviousEdition,(Join-Path $PreviousEdition 'DepthModels'))) {
            $oldGraph = Join-Path $folder $depthGraphName
            $oldWeights = Join-Path $folder $depthWeightName
            if (!(Test-Path -LiteralPath $oldGraph -PathType Leaf) -or !(Test-Path -LiteralPath $oldWeights -PathType Leaf)) { continue }
            & $Converter --verify-pair $pack $oldGraph $oldWeights
            if ($LASTEXITCODE -ne 0) { continue }
            Copy-Item -LiteralPath $oldGraph -Destination $graph
            Copy-Item -LiteralPath $oldWeights -Destination $weights
            break
        }
        if (!(Test-Path -LiteralPath $graph -PathType Leaf)) {
            & $Converter --split $pack $graph $weights
            if ($LASTEXITCODE -ne 0) { throw 'Native depth resource split failed.' }
        }
        @{
            format=1
            pack=(Get-DepthSha256Hex $pack)
            graph=(Get-DepthSha256Hex $graph)
            weights=(Get-DepthSha256Hex $weights)
        } | ConvertTo-Json -Compress | Set-Content -LiteralPath (Join-Path $candidate 'split-352x192.json') -Encoding ASCII
        if (Test-Path -LiteralPath $pair) { Move-Item -LiteralPath $pair -Destination $previous }
        Move-Item -LiteralPath $candidate -Destination $pair
    } finally {
        if (!(Test-Path -LiteralPath $pair) -and (Test-Path -LiteralPath $previous)) {
            Move-Item -LiteralPath $previous -Destination $pair
        }
        if (Test-Path -LiteralPath $candidate) { Remove-Item -LiteralPath $candidate -Recurse -Force }
        if (Test-Path -LiteralPath $previous) {
            $unexpected = @(Get-ChildItem -LiteralPath $previous -Force | Where-Object {
                $_.PSIsContainer -or ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
                    $_.Name -notin @($depthGraphName,$depthWeightName,'split-352x192.json')
            })
            if ($unexpected.Count) { Write-Warning "Unmanaged cache files preserved in $previous." }
            else { Remove-Item -LiteralPath $previous -Recurse -Force }
        }
    }
    Copy-PairToEdition
    Write-Host '[INFO] Staged verified 352x192 graph and weights.'
} finally {
    $lock.Dispose()
}
