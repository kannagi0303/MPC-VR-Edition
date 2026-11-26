# SPDX-License-Identifier: GPL-3.0-or-later
# Maintainer-only invocation of the pinned upstream exporter.
param([Parameter(Mandatory=$true)][string]$WorkDirectory)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'model-cache.ps1')
$work = [IO.Path]::GetFullPath($WorkDirectory)
$null = New-Item -ItemType Directory -Force -Path $work
$checkpoint = Get-PinnedDepthCheckpoint $work
$revision = '1afa8587b6c285875b29d80892a457016ea0757f'
$checkpointRevision = '903a1c1ade918831d138adcc4aedbcc8f4da0078'
$upstream = Join-Path $env:LOCALAPPDATA 'DepthPro2OfflineExport'
$checkout = Join-Path $upstream "depthpro2-onnx-$revision"
$git = (Get-Command git.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$uv = (Get-Command uv.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if (!$uv) {
    $cachedUv = Join-Path $env:LOCALAPPDATA 'MPC-VR-Edition\DepthPro2Research\uv-0.9.26.exe'
    if (Test-Path -LiteralPath $cachedUv -PathType Leaf) { $uv = $cachedUv }
    else { throw 'Install uv as required by the upstream exporter or put its executable on PATH.' }
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed (exit $LASTEXITCODE)." }
}
if (!(Test-Path -LiteralPath $checkout -PathType Container)) {
    $null = New-Item -ItemType Directory -Force -Path $upstream
    $clone = Join-Path $upstream ('exporter-' + [guid]::NewGuid())
    $null = New-Item -ItemType Directory -Path $clone
    Invoke-Checked $git @('-C',$clone,'init')
    Invoke-Checked $git @('-C',$clone,'fetch','--depth','1','https://github.com/protortyp/depthpro2-onnx.git',$revision)
    Invoke-Checked $git @('-C',$clone,'checkout','--detach','FETCH_HEAD')
    [IO.Directory]::Move($clone,$checkout)
}

# Archive the immutable commit into a disposable checkout. Change only the
# checkpoint revision literal there; the installed upstream checkout is read-only.
$stage = Join-Path $work ('export-' + [guid]::NewGuid())
$null = New-Item -ItemType Directory -Path $stage
$archive = Join-Path $stage 'upstream.zip'
$source = Join-Path $stage 'upstream'
$oldCache, $oldOffline, $oldVenv = $env:HF_HUB_CACHE, $env:HF_HUB_OFFLINE, $env:UV_PROJECT_ENVIRONMENT
try {
    Invoke-Checked $git @('-C',$checkout,'archive','--format=zip',"--output=$archive",$revision)
    Expand-Archive -LiteralPath $archive -DestinationPath $source
    $file = Join-Path $source 'depthpro2_onnx\model.py'
    $text = [IO.File]::ReadAllText($file)
    $oldPin = '30e647174773e3bdf5ab5dcc2a815c38cec5fdd8'
    if ([regex]::Matches($text,$oldPin).Count -ne 1) { throw 'Upstream S checkpoint pin changed.' }
    [IO.File]::WriteAllText($file,$text.Replace($oldPin,$checkpointRevision),[Text.UTF8Encoding]::new($false))

    $env:HF_HUB_CACHE = Join-Path $work 'hub'
    $snapshot = Join-Path $env:HF_HUB_CACHE "models--apple--DepthPro2-S\snapshots\$checkpointRevision"
    $null = New-Item -ItemType Directory -Force -Path $snapshot
    foreach ($name in @('config.json','model.safetensors')) {
        $target = Join-Path $snapshot $name
        if (!(Test-Path -LiteralPath $target -PathType Leaf) -or
            (Get-DepthSha256Hex $target) -ne (Get-DepthSha256Hex $checkpoint[$name])) {
            Copy-Item -LiteralPath $checkpoint[$name] -Destination $target -Force
        }
    }
    $env:HF_HUB_OFFLINE = '1'
    $env:UV_PROJECT_ENVIRONMENT = Join-Path $upstream "venv-$revision"
    Push-Location -LiteralPath $source
    try {
        Invoke-Checked $uv @('run','--locked','--no-dev','--extra','export','--python','3.12',
            'python','export_onnx.py','--size','S','--width','352','--height','192','--stream','4',
            '--only','model_stream4_fp16','--out',(Join-Path $stage 'result'))
    } finally { Pop-Location }
    $output = Join-Path $work "$depthStem.source.onnx"
    $candidate = Join-Path $stage 'result\onnx\model_stream4_fp16.onnx'
    if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) { throw 'Upstream exporter produced no ONNX file.' }
    if (Test-Path -LiteralPath $output -PathType Leaf) { [IO.File]::Replace($candidate,$output,[NullString]::Value) }
    else { [IO.File]::Move($candidate,$output) }
    Write-Host "Source ONNX: $output"
} finally {
    $env:HF_HUB_CACHE, $env:HF_HUB_OFFLINE, $env:UV_PROJECT_ENVIRONMENT = $oldCache, $oldOffline, $oldVenv
    if ((Split-Path -Parent $stage) -eq $work -and (Test-Path -LiteralPath $stage -PathType Container)) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
