# Pinned DirectML runtime and offline ONNX converter build dependencies.
[CmdletBinding()]
param([string]$VisualStudioRoot, [switch]$WithConverter)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runtime = Join-Path $root 'vendor\directml'
$include = Join-Path $root 'vendor\directml\include'
$cache = Join-Path $root '_bin\downloads'
New-Item -ItemType Directory -Force -Path $runtime, $include, $cache | Out-Null

function Sha256([string]$file) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($file)
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}

function Download([string]$url, [string]$destination, [string]$expectedHash) {
    if ((Test-Path -LiteralPath $destination -PathType Leaf) -and (Sha256 $destination) -eq $expectedHash) { return }
    $legacyName = Split-Path -Leaf $destination
    foreach ($legacyRoot in @('licenses')) {
        $legacy = Join-Path (Join-Path $root $legacyRoot) $legacyName
        if ((Test-Path -LiteralPath $legacy -PathType Leaf) -and (Sha256 $legacy) -eq $expectedHash) {
            Copy-Item -LiteralPath $legacy -Destination $destination -Force
            return
        }
    }
    $temp = $destination + '.partial-' + [Guid]::NewGuid().ToString('N')
    try {
        Write-Host "[INFO] Downloading $([IO.Path]::GetFileName($destination))..."
        Invoke-WebRequest -Uri $url -UseBasicParsing -OutFile $temp -TimeoutSec 300
        if ((Sha256 $temp) -ne $expectedHash) { throw "SHA256 mismatch: $url" }
        Move-Item -LiteralPath $temp -Destination $destination -Force
    } finally { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue }
}

function Extract([IO.Compression.ZipArchive]$archive, [string]$name, [string]$destination) {
    $entry = $archive.GetEntry($name)
    if ($null -eq $entry) { throw "Missing entry $name in pinned package." }
    # Preserve the timestamp of unchanged headers for incremental builds.
    $temp = $destination + '.partial-' + [Guid]::NewGuid().ToString('N')
    try {
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $temp, $false)
        if (!(Test-Path -LiteralPath $destination -PathType Leaf) -or
            (Sha256 $temp) -ne (Sha256 $destination)) {
            Move-Item -LiteralPath $temp -Destination $destination -Force
        }
    } finally { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue }
}

$nuget = 'https://api.nuget.org/v3-flatcontainer'
$ortName = 'microsoft.ml.onnxruntime.directml.1.24.4.nupkg'
$dmlName = 'microsoft.ai.directml.1.15.4.nupkg'
$ortPackage = Join-Path $cache $ortName
$dmlPackage = Join-Path $cache $dmlName
$flatPackage = Join-Path $cache 'flatbuffers-23.5.26.zip'
$jsonHeader = Join-Path $include 'json.hpp'
Download 'https://codeload.github.com/google/flatbuffers/zip/refs/tags/v23.5.26' $flatPackage `
    '57bd580c0772fd1a726c34ab8bf05325293bc5f9c165060a898afa1feeeb95e1'
Download "$nuget/microsoft.ai.directml/1.15.4/$dmlName" $dmlPackage `
    '4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9'

$dml = [IO.Compression.ZipFile]::OpenRead($dmlPackage)
try {
    Extract $dml 'bin/x64-win/DirectML.dll' (Join-Path $runtime 'DirectML.dll')
    Extract $dml 'include/DirectML.h' (Join-Path $include 'DirectML.h')
    Extract $dml 'LICENSE.txt' (Join-Path $runtime 'LICENSE-DirectML.txt')
    Extract $dml 'LICENSE-CODE.txt' (Join-Path $runtime 'LICENSE-CODE-DirectML.txt')
    Extract $dml 'ThirdPartyNotices.txt' (Join-Path $runtime 'THIRD-PARTY-DirectML.txt')
} finally { $dml.Dispose() }

$flat = [IO.Compression.ZipFile]::OpenRead($flatPackage)
try {
    $prefix = 'flatbuffers-23.5.26/include/'
    $headers = Join-Path $root 'vendor\directml\flatbuffers\include'
    foreach ($entry in $flat.Entries) {
        if ($entry.FullName.StartsWith($prefix) -and $entry.Name.EndsWith('.h')) {
            $destination = Join-Path $headers $entry.FullName.Substring($prefix.Length)
            $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination)
            Extract $flat $entry.FullName $destination
        }
    }
    Extract $flat 'flatbuffers-23.5.26/LICENSE' (Join-Path $runtime 'LICENSE-FlatBuffers.txt')
} finally { $flat.Dispose() }

if (!$WithConverter) {
    Write-Host '[OK] Prepared pinned DirectML runtime and renderer headers.'
    return
}


Download 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp' $jsonHeader `
    'aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63'
Download 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT' (Join-Path $runtime 'LICENSE-nlohmann-json.txt') `
    '46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603'
Download "$nuget/microsoft.ml.onnxruntime.directml/1.24.4/$ortName" $ortPackage `
    '57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681'

$ort = [IO.Compression.ZipFile]::OpenRead($ortPackage)
try {
    foreach ($file in @('onnxruntime.dll', 'onnxruntime_providers_shared.dll')) {
        Extract $ort "runtimes/win-x64/native/$file" (Join-Path $runtime $file)
    }
    foreach ($file in @('dml_provider_factory.h', 'onnxruntime_c_api.h', 'onnxruntime_ep_c_api.h')) {
        Extract $ort "build/native/include/$file" (Join-Path $include $file)
    }
    Extract $ort 'LICENSE' (Join-Path $runtime 'LICENSE-ONNXRuntime.txt')
    Extract $ort 'ThirdPartyNotices.txt' (Join-Path $runtime 'THIRD-PARTY-ONNXRuntime.txt')
} finally { $ort.Dispose() }

# ORT requires the x64 app-local VC++ DLLs even when our converter uses /MT.
$crtFiles = @('msvcp140.dll', 'msvcp140_1.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$redist = Join-Path $VisualStudioRoot 'VC\Redist\MSVC'
$crt = $null
if (Test-Path -LiteralPath $redist -PathType Container) {
    $crt = Get-ChildItem -LiteralPath $redist -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
        Sort-Object -Property { [version]$_.Name } -Descending |
        ForEach-Object {
            Get-ChildItem -LiteralPath (Join-Path $_.FullName 'x64') -Filter 'Microsoft.VC*.CRT' -Directory -ErrorAction SilentlyContinue
        } |
        Where-Object {
            $folder = $_.FullName
            @($crtFiles | Where-Object { Test-Path -LiteralPath (Join-Path $folder $_) -PathType Leaf }).Count -eq $crtFiles.Count
        } | Select-Object -First 1
}
if (-not $crt) {
    throw "Cannot find the x64 VC++ redistributable DLLs under $redist. Restore the Visual Studio C++ redistributable component."
}

. (Join-Path $PSScriptRoot 'license-notices.ps1')
$converterNotices = [ordered]@{
    'Microsoft ONNX Runtime DirectML package' = (Join-Path $runtime 'LICENSE-ONNXRuntime.txt')
    'Microsoft ONNX Runtime third-party notices' = (Join-Path $runtime 'THIRD-PARTY-ONNXRuntime.txt')
    'Microsoft DirectML runtime (required alongside the converter)' = (Join-Path $runtime 'LICENSE-DirectML.txt')
    'Microsoft DirectML SDK source' = (Join-Path $runtime 'LICENSE-CODE-DirectML.txt')
    'Microsoft DirectML runtime third-party notices' = (Join-Path $runtime 'THIRD-PARTY-DirectML.txt')
    'DxDispatch graph code' = (Join-Path $root 'Source\Depth\DirectMLGraph\LICENSE-DxDispatch.txt')
    'FlatBuffers' = (Join-Path $runtime 'LICENSE-FlatBuffers.txt')
    'JSON for Modern C++' = (Join-Path $runtime 'LICENSE-nlohmann-json.txt')
}
$converterLicense = Join-Path $include 'ConverterLicenses.md'
Write-LicenseDocument -Destination $converterLicense `
    -ProjectLicense (Join-Path $root '.github\LICENSE.md') -Notices $converterNotices

# Use full paths in the generated RC header, including when MSBuild runs in another directory.
$paths = [ordered]@{
    DEPTH_CONVERTER_ORT_PATH = Join-Path $runtime 'onnxruntime.dll'
    DEPTH_CONVERTER_ORT_SHARED_PATH = Join-Path $runtime 'onnxruntime_providers_shared.dll'
    DEPTH_CONVERTER_MSVCP_PATH = Join-Path $crt.FullName 'msvcp140.dll'
    DEPTH_CONVERTER_MSVCP_1_PATH = Join-Path $crt.FullName 'msvcp140_1.dll'
    DEPTH_CONVERTER_VCRUNTIME_PATH = Join-Path $crt.FullName 'vcruntime140.dll'
    DEPTH_CONVERTER_VCRUNTIME_1_PATH = Join-Path $crt.FullName 'vcruntime140_1.dll'
    DEPTH_CONVERTER_LICENSES_PATH = $converterLicense
}
$lines = foreach ($entry in $paths.GetEnumerator()) {
    '#define {0} "{1}" // {2}' -f $entry.Key, $entry.Value.Replace('\', '\\'), (Sha256 $entry.Value)
}
$payloadHeader = Join-Path $include 'ConverterPayloadPaths.h'
$content = ($lines -join "`r`n") + "`r`n"
if (!(Test-Path -LiteralPath $payloadHeader -PathType Leaf) -or
    [IO.File]::ReadAllText($payloadHeader) -cne $content) {
    [IO.File]::WriteAllText($payloadHeader, $content, [Text.UTF8Encoding]::new($true))
}

Write-Host '[OK] Prepared pinned DirectML and offline depth converter dependencies.'
