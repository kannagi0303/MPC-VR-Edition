param([Parameter(Mandatory=$true)][string]$PreparedDepth)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$root = (Get-Item -LiteralPath (Split-Path -Parent $PSScriptRoot)).FullName
$compiled = Join-Path $root '_bin\Filter_x64'
$ax = Join-Path $compiled 'MPCVRMFG64.ax'
$shim = Join-Path $compiled 'edition\mpcvrmfg_nvngx.dll'
$install = Join-Path $PSScriptRoot 'Install_MPCVR_64.cmd'
$uninstall64 = Join-Path $PSScriptRoot 'Uninstall_MPCVR_64.cmd'
$gplLicense = Join-Path $root '.github\LICENSE.md'
$readme = Join-Path $PSScriptRoot 'README.dist.md'
$nvidiaLicense = Join-Path $root 'vendor\nvidia\LICENSE.txt'
$directml = Join-Path $root 'vendor\directml'
$depthLicense = Join-Path $root 'licenses\LICENSE-Apple-DepthPro2.txt'
. (Join-Path $PSScriptRoot 'license-notices.ps1')
. (Join-Path $PSScriptRoot 'depth\model-cache.ps1')
$requiredFiles = @($ax,$shim,$install,$uninstall64,$readme,$gplLicense,$nvidiaLicense,(Join-Path $directml 'DirectML.dll'))
$requiredFiles += @($depthLicense,(Join-Path $PreparedDepth $depthGraphName),(Join-Path $PreparedDepth $depthWeightName))
foreach ($required in $requiredFiles) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) { throw "Cannot package: required file not found: $required" }
}
if ((Get-DepthSha256Hex $depthLicense) -ne '343C50837F76E2536440B8FD0FAF5038CF75E883B49334B79A2F826F55510C5B') {
    throw 'Apple model license checksum mismatch.'
}


function Get-NvidiaRuntime {
    param(
        [string]$FileName,
        [string]$Version,
        [string]$Revision,
        [string]$ExpectedHash
    )

    $cacheFolder = Join-Path $root 'vendor\nvidia\runtime'
    $cacheFile = Join-Path $cacheFolder $FileName
    foreach ($candidate in @($cacheFile,
                             (Join-Path $root ('dist\edition\' + $FileName)), (Join-Path $compiled ('edition\' + $FileName)),
                             (Join-Path $compiled ('dlss\' + $FileName)))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            if ((Get-DepthSha256Hex $candidate) -eq $ExpectedHash) {
                Write-Host ('[INFO] Using verified local NVIDIA runtime: ' + $candidate)
                return $candidate
            }
            Write-Warning ('Ignoring a different NVIDIA runtime: ' + $candidate)
        }
    }

    $url = "https://raw.githubusercontent.com/NVIDIA/DLSS/$Revision/lib/Windows_x86_64/rel/$FileName"
    $null = New-Item -ItemType Directory -Path $cacheFolder -Force
    Write-Host "[INFO] Downloading NVIDIA $FileName $Version..."
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $temporary = $cacheFile + '.download-' + [Guid]::NewGuid().ToString('N')
        try {
            Invoke-WebRequest -Uri $url -OutFile $temporary -UseBasicParsing -TimeoutSec 120
            if ((Get-DepthSha256Hex $temporary) -ne $ExpectedHash) {
                throw "SHA-256 mismatch for $FileName."
            }
            Move-Item -LiteralPath $temporary -Destination $cacheFile -Force
            return $cacheFile
        } catch {
            if ($attempt -ge 3) {
                throw "Could not obtain NVIDIA $FileName $Version. Build output remains in _bin. Put the matching DLL in vendor\nvidia\runtime. Last error: $($_.Exception.Message)"
            }
            Write-Warning "Download attempt $attempt failed for $FileName; retrying..."
            Start-Sleep -Seconds (2 * $attempt)
        } finally {
            if (Test-Path -LiteralPath $temporary) {
                Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

# MFG requires precisely the binary reviewed by Source/Mfg/Runtime.cpp.
$runtimes = @(
    @{ FileName = 'nvngx_dlssg.dll'; Version = '310.9.1'; Revision = 'v310.9.1'
       ExpectedHash = 'FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82' },
    @{ FileName = 'nvngx_dlss.dll'; Version = '310.9.16'; Revision = '9981c6350dc58c0ce21f024fd65e31e851c31689'
       ExpectedHash = 'D00B5B0AB29C3CA101B70C3A5ECB3A01B7EF33766A3383A28A18623083E6123E' }
)
foreach ($runtime in $runtimes) {
    $runtimeSource = Get-NvidiaRuntime @runtime
    $runtime.Source = $runtimeSource
}

# Build a complete sibling stage before replacing local dist.
$dist = Join-Path $root 'dist'
$stage = $dist + '.stage-' + [Guid]::NewGuid().ToString('N')
$old = $dist + '.previous-' + [Guid]::NewGuid().ToString('N')
foreach ($path in @($dist,$stage,$old)) {
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)) -ne $root -or
        ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint))) {
        throw "Unsafe distribution path: $path"
    }
}
$managed = @(
    'MPCVRMFG64.ax','Install_MPCVR_64.cmd','Uninstall_MPCVR_64.cmd','README.md','LICENSE.md',
    'RESEARCH-ONLY.txt','LICENSE-Apple-DepthPro2.txt','edition\mpcvrmfg_nvngx.dll','edition\nvngx_dlssg.dll',
    'edition\nvngx_dlss.dll','edition\DirectML.dll','edition\svdlut-fivek-srgb.dmlgraph',
    'edition\depth-model.ini','edition\zipdepth-convert.exe',
    "edition\$depthGraphName","edition\$depthWeightName",
    "edition\DepthModels\$depthGraphName","edition\DepthModels\$depthWeightName",
    "edition\DepthModels\$depthStem.RESEARCH-LICENSE.txt"
)
$previousMoved = $false
$published = $false
$keepPrevious = $false
$existingModels = @()
$distFiles = @{}
$lock = [IO.File]::Open((Join-Path $root '_bin\package.lock'), 'OpenOrCreate', 'Write', 'None')
try {
    $edition = Join-Path $stage 'edition'
    $null = New-Item -ItemType Directory -Path $edition -Force
    if (Test-Path -LiteralPath $dist) {
        $pending = [Collections.Generic.Queue[string]]::new()
        $pending.Enqueue($dist)
        while ($pending.Count) {
            $folder = $pending.Dequeue()
            foreach ($item in Get-ChildItem -LiteralPath $folder -Force) {
                $relative = $item.FullName.Substring($dist.Length + 1)
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                    $keepPrevious = $true
                    continue
                }
                if ($item.PSIsContainer) {
                    $pending.Enqueue($item.FullName)
                    continue
                }
                $distFiles[$relative] = $item
                if ($relative -in @("edition\$depthGraphName","edition\$depthWeightName",
                                   "edition\DepthModels\$depthGraphName","edition\DepthModels\$depthWeightName")) {
                    $existingModels += $item
                    continue
                }
                if ($relative -in $managed) { continue }
                if ($relative -eq 'edition\zipdepth.dmlpack' -and
                    (Get-DepthSha256Hex $item.FullName) -eq '659AF6BE9F3C35E3DAB58E5889DFA7A65C1A217853F9AACB50E6FFFE8D381C88') { continue }
                if ($relative -like 'VRProfiles\*') {
                    $template = Join-Path $PSScriptRoot $relative
                    if ((Test-Path -LiteralPath $template -PathType Leaf) -and
                        (Get-DepthSha256Hex $template) -eq (Get-DepthSha256Hex $item.FullName)) { continue }
                }
                # Keep additional model payloads and development files outside the new package.
                if ($relative -like 'edition\DepthModels\*' -or
                    $item.Extension -in @('.dmlpack','.dmlgraph','.dmlweights','.onnx','.safetensors','.pdb','.exe','.lib','.obj','.cso','.log')) {
                    $keepPrevious = $true
                    continue
                }
                $destination = Join-Path $stage $relative
                $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
                Copy-Item -LiteralPath $item.FullName -Destination $destination
            }
        }
    }
    Copy-Item -LiteralPath $ax,$install,$uninstall64 -Destination $stage
    Copy-Item -LiteralPath $shim,(Join-Path $directml 'DirectML.dll') -Destination $edition
    foreach ($runtime in $runtimes) {
        $target = Join-Path $edition $runtime.FileName
        Copy-Item -LiteralPath $runtime.Source -Destination $target
        if ((Get-DepthSha256Hex $target) -ne $runtime.ExpectedHash) { throw "Staged NVIDIA hash mismatch: $($runtime.FileName)" }
    }
    Copy-Item -LiteralPath (Join-Path $PreparedDepth $depthGraphName),(Join-Path $PreparedDepth $depthWeightName) -Destination $edition
    foreach ($existingModel in $existingModels) {
        if ((Get-DepthSha256Hex $existingModel.FullName) -ne
            (Get-DepthSha256Hex (Join-Path $edition $existingModel.Name))) { $keepPrevious = $true }
    }
    Copy-Item -LiteralPath $depthLicense -Destination $stage
    @('OPTIONAL DEPTH MODEL FOR RESEARCH PURPOSES ONLY',
      '',
      'Apple Machine Learning Research Model is licensed under the Apple Machine Learning Research Model License Agreement.',
      'Read LICENSE-Apple-DepthPro2.txt for the full terms and the definition of Research Purposes.',
      'Depth estimation is disabled by default. For permitted research, use Depth use on the Edition Extended tab to enable it.',
      '',
      'These files are derivatives of the Apple Depth Pro 2-S checkpoint:',
      "  edition/$depthGraphName",
      "  edition/$depthWeightName",
      'Checkpoint: apple/DepthPro2-S at 903a1c1ade918831d138adcc4aedbcc8f4da0078.',
      'The pinned exporter produces a 352x192 W4 ONNX using its mixed FP16 policy.',
      'Edition binds the focal-length input, applies a DirectML Reshape compatibility fix,',
      'and converts the adapted graph to a DirectML graph and a separate weight file.',
      'Keep this notice and the full Apple agreement with the model files.') |
        Set-Content -LiteralPath (Join-Path $stage 'RESEARCH-ONLY.txt') -Encoding ASCII

    Copy-Item -LiteralPath $readme -Destination (Join-Path $stage 'README.md')
    $playbackNotices = [ordered]@{
        'NVIDIA DLSS and NGX SDK' = $nvidiaLicense
        'NVIDIA Optical Flow SDK interface headers' = (Join-Path $PSScriptRoot 'LICENSE-NvOF.txt')
        'Microsoft DirectML runtime' = (Join-Path $directml 'LICENSE-DirectML.txt')
        'Microsoft DirectML SDK source' = (Join-Path $directml 'LICENSE-CODE-DirectML.txt')
        'Microsoft DirectML runtime third-party notices' = (Join-Path $directml 'THIRD-PARTY-DirectML.txt')
        'DxDispatch graph code' = (Join-Path $root 'Source\Depth\DirectMLGraph\LICENSE-DxDispatch.txt')
        'ONNX Runtime graph format code' = (Join-Path $root 'Source\Depth\DirectMLGraph\LICENSE-ONNXRuntime.txt')
        'FlatBuffers' = (Join-Path $directml 'LICENSE-FlatBuffers.txt')
        'DirectShow BaseClasses (Windows Classic Samples)' = (Join-Path $root 'external\BaseClasses\LICENSE.txt')
        'MinHook' = (Join-Path $root 'external\minhook\LICENSE.txt')
    }
    $svdlutSource = Join-Path $root 'models\svdlut_fivek_srgb'
    $svdlutGraph = Join-Path $svdlutSource 'svdlut-fivek-srgb.dmlgraph'
    if (Test-Path -LiteralPath $svdlutGraph -PathType Leaf) {
        Copy-Item -LiteralPath $svdlutGraph -Destination $edition
        $playbackNotices['SVDLUT FiveK sRGB'] = Join-Path $svdlutSource 'LICENSE-SVDLUT.txt'
    }
    Write-LicenseDocument -Destination (Join-Path $stage 'LICENSE.md') -ProjectLicense $gplLicense -Notices $playbackNotices
    foreach ($profile in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'VRProfiles') -Filter '*.ini' -File) {
        $profileFolder = Join-Path $stage 'VRProfiles'
        $null = New-Item -ItemType Directory -Path $profileFolder -Force
        if (!(Test-Path -LiteralPath (Join-Path $profileFolder $profile.Name))) {
            Copy-Item -LiteralPath $profile.FullName -Destination $profileFolder
        }
    }
    if (!$keepPrevious -and (Test-Path -LiteralPath $dist)) {
        $stagedFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Force)
        $same = $stagedFiles.Count -eq $distFiles.Count
        foreach ($file in $stagedFiles) {
            if (!$same) { break }
            $relative = $file.FullName.Substring($stage.Length + 1)
            $existing = $distFiles[$relative]
            $same = $null -ne $existing -and $file.Length -eq $existing.Length -and
                (Get-DepthSha256Hex $file.FullName) -eq (Get-DepthSha256Hex $existing.FullName)
        }
        if ($same) {
            Write-Host '[OK] Distribution already matches the staged files.'
            return
        }
    }
    if (Test-Path -LiteralPath $dist) {
        Move-Item -LiteralPath $dist -Destination $old
        $previousMoved = $true
    }
    Move-Item -LiteralPath $stage -Destination $dist
    $published = $true
    Write-Host "[OK] Distribution ready: $dist"
} finally {
    try {
        if (!$published -and $previousMoved -and !(Test-Path -LiteralPath $dist)) {
            Move-Item -LiteralPath $old -Destination $dist
        }
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
        if ($published -and $previousMoved) {
            if ($keepPrevious) { Write-Host "[INFO] Local files preserved in $old" }
            else { Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue }
        }
    } finally { $lock.Dispose() }
}
