param()

$ErrorActionPreference = 'Stop'

$ProgressPreference = 'SilentlyContinue'

# Windows PowerShell 5.1 and recent PowerShell versions.

[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12



function Get-Sha256Hex {

    param([Parameter(Mandatory = $true)][string]$FilePath)

    $sha = [System.Security.Cryptography.SHA256]::Create()

    $stream = $null

    try {

        $stream = [System.IO.File]::OpenRead($FilePath)

        return [System.BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')

    } finally {

        if ($null -ne $stream) { $stream.Dispose() }

        $sha.Dispose()

    }

}



$root = (Get-Item -LiteralPath (Split-Path -Parent $PSScriptRoot)).FullName

$compiled = Join-Path $root '_bin\Filter_x64'

$ax = Join-Path $compiled 'MPCVRMFG64.ax'

$shim = Join-Path $compiled 'edition\mpcvrmfg_nvngx.dll'
$converter = Join-Path $compiled 'edition\zipdepth-convert.exe'

$install = Join-Path $PSScriptRoot 'Install_MPCVR_64.cmd'

$uninstall64 = Join-Path $PSScriptRoot 'Uninstall_MPCVR_64.cmd'

$gplLicense = Join-Path $root '.github\LICENSE.md'
$readme = Join-Path $PSScriptRoot 'README.dist.md'

$nvidiaLicense = Join-Path $root 'vendor\nvidia\LICENSE.txt'
$zipSource = Join-Path $root 'runtime\zipdepth'

. (Join-Path $PSScriptRoot 'license-notices.ps1')



foreach ($required in @($ax,$shim,$converter,$install,$uninstall64,$readme,$gplLicense,$nvidiaLicense)) {

    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {

        throw ('Cannot package: required file not found: ' + $required)

    }

}



function Get-NvidiaRuntime {
    param(
        [string]$FileName,
        [string]$Version,
        [string]$Revision,
        [string]$ExpectedHash
    )

    $cacheFolder = Join-Path $root 'runtime\dlss'
    $cacheFile = Join-Path $cacheFolder $FileName
    foreach ($candidate in @($cacheFile, (Join-Path $compiled ('edition\' + $FileName)),
                             (Join-Path $compiled ('dlss\' + $FileName)))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            if ((Get-Sha256Hex -FilePath $candidate) -eq $ExpectedHash) {
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
            if ((Get-Sha256Hex -FilePath $temporary) -ne $ExpectedHash) {
                throw "SHA-256 mismatch for $FileName."
            }
            Move-Item -LiteralPath $temporary -Destination $cacheFile -Force
            return $cacheFile
        } catch {
            if ($attempt -ge 3) {
                throw "Could not obtain NVIDIA $FileName $Version. Build output remains in _bin. Put the matching DLL in runtime\dlss. Last error: $($_.Exception.Message)"
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

# A fresh staging directory avoids leaving a partly populated dist/ on failure.

$dist = Join-Path $root 'dist'

$stage = Join-Path $root ('dist.stage-' + [Guid]::NewGuid().ToString('N'))

$old = Join-Path $root ('dist.previous-' + [Guid]::NewGuid().ToString('N'))

foreach ($path in @($dist, $stage, $old)) {
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)) -ne $root) {
        throw ('Distribution path is outside the repository: ' + $path)
    }
    if ((Test-Path -LiteralPath $path) -and
        ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw ('Distribution path is a junction or symlink: ' + $path)
    }
}
$previousMoved = $false

$published = $false

try {

    $edition = Join-Path $stage 'edition'

    $null = New-Item -ItemType Directory -Path $edition -Force

    Copy-Item -LiteralPath $ax -Destination (Join-Path $stage 'MPCVRMFG64.ax') -ErrorAction Stop

    Copy-Item -LiteralPath $shim -Destination (Join-Path $edition 'mpcvrmfg_nvngx.dll') -ErrorAction Stop

    foreach ($runtime in $runtimes) {
        $target = Join-Path $edition $runtime.FileName
        Copy-Item -LiteralPath $runtime.Source -Destination $target
        if ((Get-Sha256Hex -FilePath $target) -ne $runtime.ExpectedHash) {
            throw ('Staged NVIDIA runtime has the wrong SHA-256: ' + $runtime.FileName)
        }
    }

    Copy-Item -LiteralPath $install -Destination (Join-Path $stage 'Install_MPCVR_64.cmd') -ErrorAction Stop

    Copy-Item -LiteralPath $uninstall64 -Destination (Join-Path $stage 'Uninstall_MPCVR_64.cmd') -ErrorAction Stop

    $modelSource = Join-Path $PSScriptRoot 'models\zipdepth_edgepad_384_mixed.onnx'
    $modelHash = '4ab14c8b586a16f61ae60776dcfe3ef3612694e44722682df130cc4571b9496a'
    if ((Get-Sha256Hex -FilePath $modelSource) -ne $modelHash) {
        throw 'ZipDepth mixed FP16 ONNX model hash mismatch.'
    }
    $directml = Join-Path $edition 'DirectML.dll'
    Copy-Item -LiteralPath (Join-Path $zipSource 'DirectML.dll') -Destination $directml

    $stagedModel = Join-Path $edition 'zipdepth.dmlpack'
    $modelCache = Join-Path $root '.zipdepth-directml-cache'
    $null = New-Item -ItemType Directory -Force -Path $modelCache
    $converterHash = Get-Sha256Hex -FilePath $converter
    $dmlHash = Get-Sha256Hex -FilePath $directml
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $keyBytes = [Text.Encoding]::ASCII.GetBytes("$modelHash|${converterHash}|${dmlHash}")
        $modelKey = [BitConverter]::ToString($sha.ComputeHash($keyBytes)).Replace('-', '')
    } finally { $sha.Dispose() }
    $cachedModel = Join-Path $modelCache ($modelKey + '.dmlpack')
    $cachedDigest = $cachedModel + '.sha256'
    if ((Test-Path -LiteralPath $cachedModel -PathType Leaf) -and
        (Test-Path -LiteralPath $cachedDigest -PathType Leaf) -and
        ((Get-Sha256Hex -FilePath $cachedModel) -eq [IO.File]::ReadAllText($cachedDigest).Trim())) {
        Copy-Item -LiteralPath $cachedModel -Destination $stagedModel
        Write-Host '[INFO] Reused verified default ZipDepth model pack.'
    } else {
        Write-Host '[INFO] Converting default ZipDepth model with DirectML...'
        # The converter resolves DirectML.dll next to its executable. Keep
        # the build-time copy in staging only for the duration of conversion.
        $stagedConverter = Join-Path $edition 'zipdepth-convert.exe'
        try {
            Copy-Item -LiteralPath $converter -Destination $stagedConverter
            & $stagedConverter $modelSource $stagedModel
            if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $stagedModel -PathType Leaf)) {
                throw 'ZipDepth conversion failed. Default model generation needs a compatible Direct3D 12 GPU.'
            }
        } finally {
            Remove-Item -LiteralPath $stagedConverter -Force
        }
        $digest = Get-Sha256Hex -FilePath $stagedModel
        $cacheTemp = Join-Path $modelCache ([Guid]::NewGuid().ToString('N') + '.dmlpack')
        try {
            Copy-Item -LiteralPath $stagedModel -Destination $cacheTemp
            [IO.File]::WriteAllText($cacheTemp + '.sha256', $digest)
            Move-Item -LiteralPath $cacheTemp -Destination $cachedModel -Force
            Move-Item -LiteralPath ($cacheTemp + '.sha256') -Destination $cachedDigest -Force
        } finally {
            Remove-Item -LiteralPath @($cacheTemp, ($cacheTemp + '.sha256')) -Force -ErrorAction SilentlyContinue
        }
    }
    Copy-Item -LiteralPath $readme -Destination (Join-Path $stage 'README.md')
    $playbackNotices = [ordered]@{
        'NVIDIA DLSS and NGX SDK' = $nvidiaLicense
        'NVIDIA Optical Flow SDK interface headers' = (Join-Path $PSScriptRoot 'LICENSE-NvOF.txt')
        'Microsoft DirectML runtime' = (Join-Path $zipSource 'LICENSE-DirectML.txt')
        'Microsoft DirectML SDK source' = (Join-Path $zipSource 'LICENSE-CODE-DirectML.txt')
        'Microsoft DirectML runtime third-party notices' = (Join-Path $zipSource 'THIRD-PARTY-DirectML.txt')
        'ZipDepth EdgePad model' = (Join-Path $PSScriptRoot 'models\LICENSE-ZipDepth.txt')
        'DxDispatch graph code' = (Join-Path $root 'Source\DLSS\DirectMLGraph\LICENSE-DxDispatch.txt')
        'ONNX Runtime graph format code' = (Join-Path $root 'Source\DLSS\DirectMLGraph\LICENSE-ONNXRuntime.txt')
        'FlatBuffers' = (Join-Path $zipSource 'LICENSE-FlatBuffers.txt')
        'DirectShow BaseClasses (Windows Classic Samples)' = (Join-Path $root 'external\BaseClasses\LICENSE.txt')
        'MinHook' = (Join-Path $root 'external\minhook\LICENSE.txt')
    }
    Write-LicenseDocument -Destination (Join-Path $stage 'LICENSE.md') -ProjectLicense $gplLicense -Notices $playbackNotices

    $tools = Join-Path $root 'tools'
    $null = New-Item -ItemType Directory -Force -Path $tools
    $tool = Join-Path $tools 'zipdepth-convert.exe'
    if (!(Test-Path -LiteralPath $tool -PathType Leaf) -or (Get-Sha256Hex -FilePath $tool) -ne $converterHash) {
        $toolStage = $tool + '.tmp-' + [Guid]::NewGuid().ToString('N')
        try {
            Copy-Item -LiteralPath $converter -Destination $toolStage
            Move-Item -LiteralPath $toolStage -Destination $tool -Force
        } finally {
            if (Test-Path -LiteralPath $toolStage) { Remove-Item -LiteralPath $toolStage -Force }
        }
    }

    if (Test-Path -LiteralPath $dist) {

        Move-Item -LiteralPath $dist -Destination $old -ErrorAction Stop

        $previousMoved = $true

    }

    Move-Item -LiteralPath $stage -Destination $dist -ErrorAction Stop

    $published = $true

    Write-Host ('[OK] Clean distribution ready: ' + $dist)

    Write-Host '[INFO] Contents: MPCVRMFG64.ax, x64 install/uninstall, README.md, LICENSE.md and five playback files in edition.'
    Write-Host ('[OK] Optional model converter: ' + $tool)

    # One-time cleanup of an obsolete build-support template from earlier ZIPs.

    # Never touch the actual x64 installer or uninstaller.

    $obsoleteX86Stub = Join-Path $PSScriptRoot 'Uninstall_MPCVR_32.cmd'

    if (Test-Path -LiteralPath $obsoleteX86Stub -PathType Leaf) {

        try {

            Remove-Item -LiteralPath $obsoleteX86Stub -Force -ErrorAction Stop

            Write-Host '[INFO] Removed obsolete x86 uninstall stub from build-support.'

        } catch {

            Write-Warning ('Obsolete x86 stub could not be removed: ' + $_.Exception.Message)

        }

    }

} finally {

    if (-not $published -and $previousMoved -and -not (Test-Path -LiteralPath $dist)) {

        Move-Item -LiteralPath $old -Destination $dist -ErrorAction SilentlyContinue

    }

    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }

    if ($published -and $previousMoved -and (Test-Path -LiteralPath $old)) {

        Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue

    }

}

