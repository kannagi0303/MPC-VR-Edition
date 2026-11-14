# Remove retired ZipDepth backends from this project checkout.
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (!(Test-Path -LiteralPath (Join-Path $root 'MpcVideoRenderer.sln') -PathType Leaf)) {
    throw 'Run cleanup from a MPC-VR-Edition project checkout.'
}

function AssertNotReparsePoint($item) {
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Refusing symlink or junction: $($item.FullName)"
    }
}

function FindOwnedItem([string]$relative) {
    $full = [IO.Path]::GetFullPath((Join-Path $root $relative))
    $prefix = $root + [IO.Path]::DirectorySeparatorChar
    if (!$full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing path outside project: $full"
    }
    $path = $root
    foreach ($part in ($full.Substring($prefix.Length) -split '[\\/]')) {
        $path = Join-Path $path $part
        try {
            $item = Get-Item -LiteralPath $path -Force
        } catch [System.Management.Automation.ItemNotFoundException] {
            return
        }
        AssertNotReparsePoint $item
    }
    return $item
}

function AssertSafeTree($directory) {
    foreach ($child in (Get-ChildItem -LiteralPath $directory.FullName -Force)) {
        AssertNotReparsePoint $child
        if ($child.PSIsContainer) { AssertSafeTree $child }
    }
}

function RemoveOwned([string]$relative) {
    $item = FindOwnedItem $relative
    if (!$item) { return }
    if ($item.PSIsContainer) { AssertSafeTree $item }
    if ($PSCmdlet.ShouldProcess($item.FullName, 'Delete obsolete ZipDepth artifact')) {
        Remove-Item -LiteralPath $item.FullName -Recurse -Force -Confirm:$false
        Write-Host "[CLEAN] $relative"
    }
}

function RemovePartialDownloads([string]$relative) {
    $folder = Split-Path -Parent $relative
    $name = Split-Path -Leaf $relative
    $parent = FindOwnedItem $folder
    if (!$parent) { return }
    foreach ($file in (Get-ChildItem -LiteralPath $parent.FullName -Filter "$name.partial-*" -File -Force)) {
        if ($file.Name -match '\.partial-[0-9a-f]{32}$') {
            RemoveOwned (Join-Path $folder $file.Name)
        }
    }
}

# CPU source and the old instructions are still present after extracting an update ZIP.
RemoveOwned 'Source/DLSS/ZipDepthCpu.cpp'
RemoveOwned 'Source/DLSS/ZipDepthCpu.h'
RemoveOwned 'docs/ZIPDEPTH-NCNN.md'
RemoveOwned '.zipdepth-lite-cache'

# The first ncnn bootstrap used these project-local paths.
RemoveOwned '.zipdepth-build'
RemoveOwned 'vendor/ncnn'
RemoveOwned 'build-support/zipdepth-model-check'

$zipDepthFolders = @('runtime/zipdepth', 'dist/dlss/zipdepth')
foreach ($folder in (Get-ChildItem -LiteralPath $root -Directory -Force)) {
    if ($folder.Name -match '^dist\.(stage|previous)-[0-9a-f]{32}$') {
        $zipDepthFolders += "$($folder.Name)/dlss/zipdepth"
    }
}
foreach ($folder in $zipDepthFolders) {
    foreach ($name in @('tensorrt', 'zipdepth-base-256-cpu.tflite', 'tensorflowlite_c.dll',
                        'LICENSE-TensorFlowLite.txt', 'zipdepth.ncnn.param',
                        'zipdepth.ncnn.bin', 'validated-model.sha256')) {
        RemoveOwned "$folder/$name"
    }
}

foreach ($name in @('TensorRT-RTX-EP-ABI-v0.5.0-cu13.zip', 'onnxruntime-win-x64-1.30.0.zip',
                    'cuda-runtime-13.4.49.zip', 'cuda-nvrtc-13.4.59.zip')) {
    $file = ".zipdepth-directml-cache/$name"
    RemoveOwned $file
    RemovePartialDownloads $file
}

# Only remove the exact ORT 1.30 headers that the CUDA experiment downloaded.
# build-x64.cmd restores the pinned DirectML headers from its cached NuGet package.
$interopHeaders = [ordered]@{
    'onnxruntime_c_api.h' = 'e035e30c27e74c8c00e0f483e576e12b4067d17f12e9237fd6eff8b346c9b381'
    'onnxruntime_ep_c_api.h' = 'e6c986c9e98583f8113b2c6bc3864814883b806d501cf24da4d239c45753e235'
    'onnxruntime_error_code.h' = '5ce3b054e798eced8d14f5b86e98692fd33470463f96194ce0700a2d53dd8721'
}
function GetSha256([string]$path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}
foreach ($name in $interopHeaders.Keys) {
    $file = "vendor/zipdepth/include/$name"
    $item = FindOwnedItem $file
    if ($item -and !$item.PSIsContainer -and
        (GetSha256 $item.FullName) -eq $interopHeaders[$name]) {
        RemoveOwned $file
    }
}

if ($WhatIfPreference) {
    Write-Host '[OK] Preview complete.'
} else {
    Write-Host '[OK] Obsolete ZipDepth files cleaned.'
}
